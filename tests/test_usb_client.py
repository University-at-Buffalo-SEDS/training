import json
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch
from types import SimpleNamespace
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "client"))
from datalogger_client import Client, DeviceError, ProtocolError, capture_logs, export_session, open_client


class Serial:
    """Packet-fragmenting byte stream, not a USB packet framed transport."""
    def __init__(self, handler, fragment=3):
        self.handler, self.fragment = handler, fragment
        self.incoming = bytearray()
        self.commands = []
        self.closed = False

    def write(self, wire):
        self.commands.append(wire)
        self.incoming.extend(self.handler(wire))
        return len(wire)

    def read(self, count):
        count = min(count, self.fragment)
        result = bytes(self.incoming[:count])
        del self.incoming[:count]
        return result

    def close(self):
        self.closed = True


def data_frame(sid, offset, data):
    return f"DATA {sid} {offset} {len(data)} {zlib.crc32(data):08X}\n".encode() + data + b"\n"


class ClientTests(unittest.TestCase):
    def test_connection_handshake_retries_and_closes_failed_port(self):
        failed = Serial(lambda _: b"ERR BUSY reconnect_required\n")
        good = Serial(lambda _: b"OK STATUS protocol=1 uid=test\n")
        for port in (failed, good):
            port.open = Mock()
            port.reset_input_buffer = Mock()
            port.reset_output_buffer = Mock()
        factory = Mock(side_effect=[failed, good])
        with patch.dict(sys.modules, {"serial": SimpleNamespace(Serial=factory)}), \
                patch("datalogger_client.time.sleep"):
            client = open_client("test-port")
        self.assertIs(client.port, good)
        self.assertTrue(failed.closed)
        self.assertFalse(good.closed)
        self.assertEqual(factory.call_count, 2)
        self.assertEqual(failed.commands, [b"STATUS\n"])
        self.assertEqual(good.commands, [b"STATUS\n"])
        self.assertEqual(good.reset_input_buffer.call_count, 2)
        self.assertEqual(client.timeout, 5.0)

    def test_connection_handshake_exhaustion_closes_all_ports(self):
        ports = [Serial(lambda _: b"ERR BUSY reconnect_required\n") for _ in range(3)]
        for port in ports:
            port.open = Mock()
            port.reset_input_buffer = Mock()
            port.reset_output_buffer = Mock()
        factory = Mock(side_effect=ports)
        with patch.dict(sys.modules, {"serial": SimpleNamespace(Serial=factory)}), \
                patch("datalogger_client.time.sleep"), self.assertRaises(DeviceError):
            open_client("test-port")
        self.assertEqual(factory.call_count, 3)
        self.assertTrue(all(p.closed and p.commands == [b"STATUS\n"] for p in ports))

    def test_live_snapshot_and_corruption(self):
        data = bytearray(128)
        struct.pack_into("<BBHIQ", data, 0, 1, 1, 32, 42, 123456789012)
        struct.pack_into("<I3h3h4I", data, 16, 255, -1, 2, 3, 4, 5, 6, 100, 200, 300, 0)
        struct.pack_into("<BBHIQ", data, 64, 2, 1, 48, 42, 123456789012)
        struct.pack_into("<I9f2I", data, 80, 255, 1, 2, 3, 4, 5, 6, 100000, 25, 0, 300, 0)
        def frame(payload):
            return f"LIVE 1 128 10 {zlib.crc32(payload):08X}\n".encode() + payload + b"\n"
        port = Serial(lambda _: frame(data), fragment=1)
        result = Client(port).live()
        self.assertEqual(port.commands, [b"LIVE\n"])
        self.assertEqual(result["sequence"], 42)
        self.assertEqual(result["timestamp_us"], 123456789012)
        self.assertEqual(result["sample_age_ms"], 10)
        self.assertEqual(result["raw"]["accel_counts"], [-1, 2, 3])
        self.assertEqual(result["processed"]["pressure_pa"], 100000)
        self.assertIsNone(result["processed"]["altitude_m"])
        with_altitude = bytearray(data)
        struct.pack_into("<I", with_altitude, 80, 511)
        struct.pack_into("<f", with_altitude, 116, 12.5)
        self.assertEqual(Client(Serial(lambda _: frame(with_altitude))).live()["processed"]["altitude_m"], 12.5)
        for offset in (0, 1, 2, 48, 64, 68):
            broken = bytearray(data); broken[offset] ^= 1
            client = Client(Serial(lambda _: frame(broken)))
            with self.assertRaises(ProtocolError):
                client.live()
            self.assertTrue(client.broken)
        bad_crc = frame(data).replace(f"{zlib.crc32(data):08X}".encode(), b"00000000", 1)
        with self.assertRaises(ProtocolError):
            Client(Serial(lambda _: bad_crc)).live()

    def test_binary_newlines_nul_and_response_strings(self):
        data = b"\x00\nERR BAD\nOK END\n\xff\r\n"
        port = Serial(lambda _: data_frame(7, 80, data), fragment=1)
        self.assertEqual(Client(port).read(7, 80, len(data)), data)

    def test_corrupt_frame_poisons_connection(self):
        for frame in (b"DATA 7 0 1 00000000\nx\n", data_frame(8, 0, b"x"),
                      b"DATA 7 0 513 00000000\n", b"DATA 7 0 1 invalid!\nx\n",
                      data_frame(7, 0, b"x")[:-1] + b"!"):
            client = Client(Serial(lambda _: frame))
            with self.assertRaises(ProtocolError):
                client.read(7, 0, 1)
            self.assertTrue(client.broken)
            with self.assertRaises(ProtocolError):
                client.command("STATUS")

    def test_not_ready_and_close(self):
        port = Serial(lambda wire: b"OK CLOSE\n" if wire == b"CLOSE\n" else b"ERR NOT_READY storage_worker_unavailable\n")
        client = Client(port)
        with self.assertRaises(DeviceError) as error:
            client.sessions()
        self.assertEqual(error.exception.code, "NOT_READY")
        self.assertFalse(client.broken)
        client.close()
        self.assertTrue(port.closed)

    def test_timeout_and_malformed_header(self):
        for data in (b"", b"\xff\n", b"OK\x00 TEST\n", b"A" * 512):
            client = Client(Serial(lambda _: data), timeout=0.001)
            with self.assertRaises(ProtocolError):
                client.command("STATUS")
            self.assertTrue(client.broken)

    def test_list_duplicate_and_bounds(self):
        port = Serial(lambda _: b"OK SESSION 7 2 160 0\nOK SESSION 8 3 80 1\nOK END\n")
        rows = Client(port).sessions()
        self.assertEqual([(s.id, s.incomplete) for s in rows], [(7, False), (8, True)])
        for response in (b"OK SESSION 7 2 160 0\nOK SESSION 7 2 160 0\nOK END\n",
                         b"OK SESSION 7 2 160 2\n", b"OK SESSION -1 2 160 0\n"):
            with self.assertRaises(ProtocolError):
                Client(Serial(lambda _: response)).sessions()

    def test_resume_rechecks_generation_and_local_crc(self):
        payload = bytes(range(256)) * 5
        generation = 2
        fail = True

        def handler(wire):
            nonlocal fail
            words = wire.decode().split()
            if words[0] == "STATUS":
                return b"OK STATUS state=READY uid=board1\n"
            if words[0] == "STOP":
                return b"OK STOP\n"
            if words[0] == "LIST":
                return f"OK SESSION 7 {generation} {len(payload)} 0\nOK END\n".encode()
            if words[0] == "READ":
                sid, offset, count = map(int, words[1:])
                if fail and offset == 512:
                    return b"DATA 7 512 512 00000000\n" + payload[offset:offset+count] + b"\n"
                return data_frame(sid, offset, payload[offset:offset+count])
            raise AssertionError(wire)

        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            with self.assertRaises(ProtocolError):
                export_session(Client(Serial(handler)), 7, folder)
            partial = folder / "session-7-g2.part"
            self.assertEqual(partial.read_bytes(), payload[:512])
            # Uncommitted local tail is discarded on resume.
            with partial.open("ab") as f:
                f.write(b"interrupted local write")
            fail = False
            port = Serial(handler)
            result = export_session(Client(port), 7, folder)
            self.assertEqual(result.read_bytes(), payload)
            self.assertNotIn(b"READ 7 0 512\n", port.commands)
            manifest = json.loads(result.with_suffix(".json").read_text())
            self.assertTrue(manifest["complete"])
            self.assertEqual(len(manifest["chunks"]), 3)
            # Complete exports are verified, without downloading again.
            port = Serial(handler)
            export_session(Client(port), 7, folder)
            self.assertFalse(any(c.startswith(b"READ") for c in port.commands))
            result.write_bytes(b"bad data")
            with self.assertRaises(ProtocolError):
                export_session(Client(Serial(handler)), 7, folder)
            # Different generation gets a separate file, never resumes old data.
            generation = 3
            result = export_session(Client(Serial(handler)), 7, folder)
            self.assertEqual(result.name, "session-7-g3.bin")

    def test_manifest_wrong_device(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            (folder / "session-7-g2.json").write_text(json.dumps({"format": 1, "session": {}}))
            replies = {b"STATUS\n": b"OK STATUS state=READY uid=board2\n",
                       b"STOP\n": b"OK STOP\n",
                       b"LIST\n": b"OK SESSION 7 2 0 0\nOK END\n"}
            with self.assertRaises(ProtocolError):
                export_session(Client(Serial(replies.__getitem__)), 7, folder)

    def test_print_history_gap_and_saved_raw_bytes(self):
        data = b"printf says hello\n\x00\xff\n"
        def handler(wire):
            self.assertEqual(wire, b"LOG_READ 0 512\n")
            return f"LOG 20 {len(data)} {20+len(data)} 20 {zlib.crc32(data):08x}\n".encode() + data + b"\n"
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "prints.log"
            capture_logs(Client(Serial(handler)), out)
            self.assertEqual(out.read_bytes(), data)
            self.assertEqual(json.loads(out.with_suffix(".log.jsonl").read_text())["gap"], 20)

    def test_log_empty_and_invalid_length(self):
        client = Client(Serial(lambda _: b"LOG 0 0 0 0 00000000\n\n"))
        self.assertEqual(client.logs(), (0, 0, 0, b""))
        for frame in (b"LOG 0 513 513 0 00000000\n", b"LOG 10 5 12 0 00000000\n"):
            with self.assertRaises(ProtocolError):
                Client(Serial(lambda _: frame)).logs()


class NativeTests(unittest.TestCase):
    def test_adapter_frames_timeout_disconnect_and_not_ready(self):
        with tempfile.TemporaryDirectory() as tmp:
            executable = Path(tmp) / "adapter-tests"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-g",
                            "-Itests/usb_stubs", "-ICore/Inc", "tests/test_usb_adapter.c",
                            "Core/Src/usb_thread.c", "Core/Src/usb_protocol.c",
                            "Core/Src/usb_service.c", "Core/Src/usb_console.c",
                            "Core/Src/sensor_live.c", "Core/Src/records.c",
                            "USBX/App/ux_device_cdc_acm.c",
                            "-lm", "-o", str(executable)], cwd=ROOT, check=True)
            subprocess.run([str(executable)], cwd=ROOT, check=True)

    def test_parser_ring_and_queue_ownership_with_sanitizers(self):
        with tempfile.TemporaryDirectory() as tmp:
            executable = Path(tmp) / "usb-tests"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-g",
                            "-Itests/usb_stubs", "-ICore/Inc", "tests/test_usb_native.c",
                            "Core/Src/usb_protocol.c", "Core/Src/usb_service.c",
                            "Core/Src/usb_console.c", "-o", str(executable)], cwd=ROOT, check=True)
            subprocess.run([str(executable)], cwd=ROOT, check=True)


if __name__ == "__main__":
    unittest.main()
