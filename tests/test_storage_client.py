import csv
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "client"))
from datalogger_client import Client, ProtocolError, decode_record, export_session
from test_usb_client import Serial, data_frame


def metadata():
    b = bytearray(256)
    struct.pack_into("<4sIIIfI", b, 0, b"DLH1", 1, 7, 2, 100000, 1)
    b[24:34] = bytes([0x1e, 0x0f, 0x60, 0xa8, 3, 4, 0, 3, 3, 4])
    struct.pack_into("<9f", b, 64, 1, 0, 0, 0, 1, 0, 0, 0, 1)
    struct.pack_into("<I", b, 248, zlib.crc32(b[:248]))
    return bytes(b)


def record(kind, seq):
    b = bytearray(80)
    struct.pack_into("<4sBBHIIQ", b, 0, b"DLOG", 1, kind, 32 if kind == 1 else 48, 7, seq, seq * 10000)
    if kind == 1:
        struct.pack_into("<I3h3h4I", b, 24, 255, -100, 2, 3, 4, 5, 6, 123456, 819200, 0, 0)
    else:
        struct.pack_into("<I9f2I", b, 24, 511, 1, 2, 3, 4, 5, 6, 100000, 25, 0, 0, 0)
    struct.pack_into("<I", b, 72, zlib.crc32(b[:72]))
    return bytes(b)


class StoredClientTests(unittest.TestCase):
    def test_protected_launch_export_is_separate_and_does_not_stop_another_capture(self):
        header = bytearray(metadata())
        struct.pack_into("<IIQIIQQQIIII", header, 124, 2, 7, 5000000, 3, 2,
                         5000000, 305000000, 5000000, 20000000, 300000000, 0, 1)
        struct.pack_into("<I", header, 248, zlib.crc32(header[:248]))
        rows = []
        for kind in (1, 2):
            b = bytearray(record(kind, 500))
            struct.pack_into("<Q", b, 16, 0)
            struct.pack_into("<I", b, 72, zlib.crc32(b[:72]))
            rows.append(bytes(b))
        payload = b"".join(rows)
        def handler(wire):
            words = wire.decode().split()
            if words[0] == "STATUS":
                return b"OK STATUS metadata=1 unpin=1 events=1 state=CAPTURING uid=board1\n"
            if words[0] == "EVENTS":
                return b"OK EVENT 7 2 160 1 2 5000 0 0 CLOSED\nOK EVENT 9 9 0 0 1 10000 0 0 CAPTURING\nOK END\n"
            if words[0] == "META":
                return f"META 7 256 {zlib.crc32(header):08X}\n".encode() + header + b"\n"
            if words[0] == "READ":
                return data_frame(7, 0, payload)
            if words[0] == "UNPIN":
                return b"OK UNPIN 7\n"
            self.fail(f"Unexpected command during protected capture: {wire}")
        port = Serial(handler)
        with tempfile.TemporaryDirectory() as tmp:
            path = export_session(Client(port), 7, Path(tmp), launch=True)
            self.assertEqual(path.name, "launch-7-g2.bin")
            manifest = json.loads(path.with_suffix(".json").read_text())
            self.assertTrue(manifest["session"]["protected"])
            self.assertEqual(manifest["metadata"]["launch"]["trigger_time_in_file_us"], 0)
            with path.with_suffix(".csv").open() as stream:
                csv_rows = list(csv.DictReader(stream))
            self.assertEqual(csv_rows[0]["trigger_relative_s"], "0.0")
            with self.assertRaisesRegex(ProtocolError, "still recording"):
                export_session(Client(port), 9, Path(tmp), launch=True)
        self.assertNotIn(b"STOP\n", port.commands)

    def test_operation_progress_validation(self):
        wire = b"OK OP_STATUS 3 state=DONE done=2 total=2 error=0\n"
        self.assertEqual(Client(Serial(lambda _: wire)).operation_status(3)["state"], "DONE")
        for bad in (b"OK OP_STATUS 4 state=DONE done=2 total=2 error=0\n",
                    b"OK OP_STATUS 3 state=DONE done=1 total=2 error=0\n",
                    b"OK OP_STATUS 3 state=RUNNING done=3 total=2 error=0\n"):
            client = Client(Serial(lambda _: bad))
            with self.assertRaises(ProtocolError):
                client.operation_status(3)
            self.assertTrue(client.broken)

    def test_metadata_records_csv_and_resume(self):
        payload = b"".join(record(1 + i % 2, i // 2) for i in range(12))
        header = metadata()
        fail = True

        def handler(wire):
            nonlocal fail
            words = wire.decode().split()
            if words[0] == "STATUS":
                return b"OK STATUS metadata=1 unpin=1 state=READY uid=board1\n"
            if words[0] == "STOP":
                return b"OK STOP\n"
            if words[0] == "LIST":
                return f"OK SESSION 7 2 {len(payload)} 1\nOK END\n".encode()
            if words[0] == "META":
                return f"META 7 256 {zlib.crc32(header):08X}\n".encode() + header + b"\n"
            if words[0] == "UNPIN":
                return b"OK UNPIN 7\n"
            self.assertEqual(words[0], "READ")
            offset, count = map(int, words[2:])
            if offset and fail:
                fail = False
                return data_frame(7, offset, payload[offset:offset+count])[:-1] + b"!"
            return data_frame(7, offset, payload[offset:offset+count])

        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            with self.assertRaises(ProtocolError):
                export_session(Client(Serial(handler, fragment=1)), 7, folder)
            manifest = json.loads((folder / "session-7-g2.json").read_text())
            self.assertFalse(manifest["complete"])
            self.assertEqual(manifest["metadata"]["reference_pressure_pa"], 100000)
            self.assertEqual(manifest["metadata_hex"], header.hex())
            self.assertEqual(len(manifest["chunks"]), 1)
            port = Serial(handler, fragment=1)
            out = export_session(Client(port), 7, folder)
            self.assertEqual(out.read_bytes(), payload)
            self.assertNotIn(b"READ 7 0 512\n", port.commands)
            self.assertEqual(port.commands[-1], b"UNPIN 7\n")
            with out.with_suffix(".csv").open() as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual(len(rows), 12)
            self.assertEqual(rows[0]["accel_x_counts"], "-100")
            self.assertEqual(rows[1]["pressure_pa"], "100000.0")
            self.assertEqual(rows[1]["altitude_m"], "0.0")
            self.assertEqual(rows[0]["type"], "raw")
            self.assertTrue(json.loads(out.with_suffix(".json").read_text())["complete"])
            self.assertEqual(export_session(Client(Serial(handler)), 7, folder), out)

    def test_record_crc_commit_session_padding_and_reserved_bits(self):
        good = record(1, 3)
        self.assertEqual(decode_record(good, 7)["sequence"], 3)
        for offset in (0, 4, 5, 6, 8, 30, 72, 76):
            bad = bytearray(good)
            bad[offset] ^= 1
            with self.assertRaises(ProtocolError):
                decode_record(bad, 7)
        for offset in (56, 27):
            bad = bytearray(good)
            bad[offset] = 128
            struct.pack_into("<I", bad, 72, zlib.crc32(bad[:72]))
            with self.assertRaises(ProtocolError):
                decode_record(bad, 7)

    def test_metadata_rejects_crc_identity_and_commit(self):
        for offset in (0, 4, 8, 12, 248, 252):
            bad = bytearray(metadata())
            bad[offset] ^= 1
            frame = f"META 7 256 {zlib.crc32(bad):08X}\n".encode() + bad + b"\n"
            client = Client(Serial(lambda _: frame))
            with self.assertRaises(ProtocolError):
                client.metadata(7, 2)
            self.assertTrue(client.broken)
