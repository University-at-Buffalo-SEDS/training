#!/usr/bin/env python3
"""USB CDC client. Only opening a real serial port requires pyserial."""
from __future__ import annotations

import argparse
import csv
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
import json
import os
import math
from pathlib import Path
import struct
import threading
import time
import zlib

U32_MAX = (1 << 32) - 1


class ProtocolError(RuntimeError):
    pass


class DeviceError(ProtocolError):
    def __init__(self, line: str):
        self.code = line.split()[1] if len(line.split()) > 1 else "UNKNOWN"
        super().__init__(line)


@dataclass(frozen=True)
class Session:
    id: int
    generation: int
    length: int
    incomplete: bool


@dataclass(frozen=True)
class LaunchFile:
    id: int
    generation: int
    length: int
    incomplete: bool
    reasons: int
    trigger_ms: int
    pre_ms: int
    post_ms: int
    state: str
    kind: str = "launch"
    protected: bool = True


def uint(text: str, maximum: int = U32_MAX) -> int:
    if not text.isascii() or not text.isdecimal():
        raise ProtocolError(f"Invalid unsigned number: {text!r}")
    value = int(text)
    if value > maximum:
        raise ProtocolError("Unsigned value out of range")
    return value


class Client:
    def __init__(self, port, timeout: float = 5.0):
        self.port = port
        self.timeout = timeout
        self.lock = threading.Lock()
        self.broken = False

    def _exact(self, length: int, deadline: float) -> bytes:
        result = bytearray()
        while len(result) < length:
            if time.monotonic() >= deadline:
                raise ProtocolError("USB response timed out; reconnect before retrying")
            chunk = self.port.read(length - len(result))
            if chunk:
                result.extend(chunk)
        return bytes(result)

    def _line(self, deadline: float) -> str:
        result = bytearray()
        while len(result) < 512:
            byte = self._exact(1, deadline)
            if byte == b"\n":
                try:
                    line = result.decode("ascii")
                except UnicodeDecodeError as exc:
                    raise ProtocolError("Non-ASCII response header") from exc
                if line.startswith("ERR "):
                    raise DeviceError(line)
                if any(ord(c) < 32 or ord(c) > 126 for c in line):
                    raise ProtocolError("Invalid response header")
                return line
            result.extend(byte)
        raise ProtocolError("Overlength response header")

    def _send(self, command: str) -> float:
        if self.broken:
            raise ProtocolError("Connection unusable; reconnect before retrying")
        wire = (command + "\n").encode("ascii")
        if len(wire) > 128 or any(b < 32 or b > 126 for b in wire[:-1]):
            raise ValueError("Invalid command line")
        count = self.port.write(wire)
        if count != len(wire):
            raise ProtocolError("Short USB write")
        return time.monotonic() + self.timeout

    def _transaction(self, command, reader):
        with self.lock:
            try:
                return reader(self._send(command))
            except DeviceError as exc:
                if exc.code in {"TIMEOUT", "BUSY"}:
                    self.broken = True
                raise
            except (ProtocolError, OSError):
                self.broken = True
                raise

    def command(self, command: str) -> str:
        def read(deadline):
            line = self._line(deadline)
            if not line.startswith("OK "):
                raise ProtocolError(f"Expected OK: {line}")
            return line
        return self._transaction(command, read)

    def status(self) -> dict[str, str]:
        line = self.command("STATUS")
        if not line.startswith("OK STATUS "):
            self.broken = True
            raise ProtocolError(f"Invalid STATUS response: {line}")
        try:
            return dict(item.split("=", 1) for item in line.split()[2:])
        except ValueError as exc:
            self.broken = True
            raise ProtocolError("Invalid STATUS fields") from exc

    def sessions(self) -> list[Session]:
        def read(deadline):
            sessions = []
            ids = set()
            while True:
                line = self._line(deadline)
                if line == "OK END":
                    return sessions
                fields = line.split()
                if len(fields) != 6 or fields[:2] != ["OK", "SESSION"]:
                    raise ProtocolError(f"Invalid LIST row: {line}")
                sid, generation, length = map(uint, fields[2:5])
                incomplete = uint(fields[5], 1)
                if sid in ids or len(sessions) >= 65536:
                    raise ProtocolError("Duplicate session or overlength LIST")
                ids.add(sid)
                sessions.append(Session(sid, generation, length, bool(incomplete)))
                deadline = time.monotonic() + self.timeout
        return self._transaction("LIST", read)

    def _body(self, length: int, checksum: str, deadline: float) -> bytes:
        if len(checksum) != 8 or any(c not in "0123456789abcdefABCDEF" for c in checksum):
            raise ProtocolError("Invalid CRC32 field")
        data = self._exact(length, deadline)
        if self._exact(1, deadline) != b"\n":
            raise ProtocolError("Missing binary-frame terminator")
        if zlib.crc32(data) != int(checksum, 16):
            raise ProtocolError("Chunk CRC32 mismatch")
        return data

    def events(self) -> list[LaunchFile]:
        def read(deadline):
            rows, identities = [], set()
            while True:
                line = self._line(deadline)
                if line == "OK END":
                    return rows
                words = line.split()
                if len(words) != 11 or words[:2] != ["OK", "EVENT"] or words[10] not in {"CAPTURING", "CLOSED"}:
                    raise ProtocolError("Invalid launch-file listing")
                sid, generation, length = map(uint, words[2:5])
                incomplete = bool(uint(words[5], 1))
                reasons, trigger, pre, post = map(uint, words[6:10])
                if (not sid or sid in identities or len(rows) >= 64 or not 1 <= reasons <= 7
                        or pre > 20000 or post > 300020 or length % 160):
                    raise ProtocolError("Invalid launch-file fields")
                rows.append(LaunchFile(sid, generation, length, incomplete, reasons, trigger, pre, post, words[10]))
                identities.add(sid)
                deadline = time.monotonic() + self.timeout
        return self._transaction("EVENTS", read)

    def read(self, sid: int, offset: int, count: int) -> bytes:
        if not (0 <= sid <= U32_MAX and 0 <= offset <= U32_MAX and
                1 <= count <= 512 and offset + count <= U32_MAX):
            raise ValueError("Invalid READ bounds")
        def read(deadline):
            fields = self._line(deadline).split()
            if len(fields) != 5 or fields[0] != "DATA":
                raise ProtocolError("Expected DATA header")
            identity = tuple(map(uint, fields[1:4]))
            if identity != (sid, offset, count):
                raise ProtocolError("DATA session, offset, or length mismatch")
            return self._body(count, fields[4], deadline)
        return self._transaction(f"READ {sid} {offset} {count}", read)

    def logs(self, cursor: int = 0, count: int = 512) -> tuple[int, int, int, bytes]:
        if not (0 <= cursor <= U32_MAX and 1 <= count <= 512):
            raise ValueError("Invalid LOG_READ bounds")
        def read(deadline):
            fields = self._line(deadline).split()
            if len(fields) != 6 or fields[0] != "LOG":
                raise ProtocolError("Expected LOG header")
            start, length, end, lost = map(uint, fields[1:5])
            if length > count or length > ((end - start) & U32_MAX):
                raise ProtocolError("Invalid LOG length")
            return start, end, lost, self._body(length, fields[5], deadline)
        return self._transaction(f"LOG_READ {cursor} {count}", read)

    def metadata(self, sid: int, generation: int) -> tuple[bytes, dict]:
        uint(str(sid)); uint(str(generation))
        def read(deadline):
            fields = self._line(deadline).split()
            if len(fields) != 4 or fields[:3] != ["META", str(sid), "256"]:
                raise ProtocolError("Invalid META header")
            data = self._body(256, fields[3], deadline)
            if (data[:4] != b"DLH1" or struct.unpack_from("<III", data, 4) != (1, sid, generation)
                    or data[252:] != b"\0" * 4
                    or struct.unpack_from("<I", data, 248)[0] != zlib.crc32(data[:248])):
                raise ProtocolError("Invalid session metadata")
            reference = struct.unpack_from("<f", data, 16)[0]
            if not math.isfinite(reference) or reference <= 0:
                raise ProtocolError("Invalid session reference pressure")
            encoding, epoch = struct.unpack_from("<II", data, 124)
            origin = struct.unpack_from("<Q", data, 132)[0]
            info = {
                "format": 1, "reference_pressure_pa": reference,
                "sensor_metadata_version": struct.unpack_from("<I", data, 20)[0],
                "device_ids": list(data[24:27]), "accel_conf": data[27], "accel_range": data[28],
                "gyro_bw": data[29], "gyro_range": data[30], "baro_osr": data[31],
                "baro_odr": data[32], "baro_iir": data[33], "baro_trim_hex": data[34:55].hex(),
                "mounting_matrix": list(struct.unpack_from("<9f", data, 64)),
                "accel_bias": list(struct.unpack_from("<3f", data, 100)),
                "gyro_bias": list(struct.unpack_from("<3f", data, 112)),
                "storage_encoding": encoding, "acquisition_epoch": epoch, "source_origin_us": origin,
                "processed_values": "regenerated from original sensor counts" if encoding == 2 else "stored",
            }
            if struct.unpack_from("<I", data, 140)[0] & 1:
                trigger, target, end = struct.unpack_from("<QQQ", data, 148)
                reasons = struct.unpack_from("<I", data, 144)[0]
                pre, post, actual_pre, incomplete = struct.unpack_from("<IIII", data, 172)
                if (encoding != 2 or not 1 <= reasons <= 7 or origin > trigger or end < trigger
                        or target - trigger != 300000000 or end > target + 20000
                        or pre != 20000000 or post != 300000000 or actual_pre != trigger - origin
                        or actual_pre > pre or incomplete > 1):
                    raise ProtocolError("Invalid protected launch capture metadata")
                info["launch"] = {"protected": True, "reasons": struct.unpack_from("<I", data, 144)[0],
                                  "trigger_us": trigger, "trigger_time_in_file_us": trigger - origin,
                                  "target_end_us": target, "actual_end_us": end,
                                  "requested_pre_us": struct.unpack_from("<I", data, 172)[0],
                                  "requested_post_us": struct.unpack_from("<I", data, 176)[0],
                                  "actual_pre_us": struct.unpack_from("<I", data, 180)[0],
                                  "incomplete": bool(struct.unpack_from("<I", data, 184)[0])}
            return data, info
        return self._transaction(f"META {sid}", read)

    def operation_status(self, operation: int) -> dict:
        uint(str(operation))
        def read(deadline):
            fields = self._line(deadline).split()
            if len(fields) != 7 or fields[:3] != ["OK", "OP_STATUS", str(operation)]:
                raise ProtocolError("Invalid OP_STATUS response")
            try:
                values = dict(item.split("=", 1) for item in fields[3:])
                result = {"id": operation, "state": values["state"],
                          **{key: uint(values[key]) for key in ("done", "total", "error")}}
            except (ValueError, KeyError) as exc:
                raise ProtocolError("Invalid operation status fields") from exc
            if (result["state"] not in {"RUNNING", "DONE", "FAILED"}
                    or result["done"] > result["total"]
                    or (result["state"] == "DONE" and result["done"] != result["total"])):
                raise ProtocolError("Invalid operation progress")
            return result
        return self._transaction(f"OP_STATUS {operation}", read)

    def live(self) -> dict:
        """Poll the latest sensor-task snapshot without accessing flash."""
        def read(deadline):
            fields = self._line(deadline).split()
            if len(fields) != 5 or fields[:3] != ["LIVE", "1", "128"]:
                raise ProtocolError("Invalid LIVE header")
            age_ms = uint(fields[3])
            data = self._body(128, fields[4], deadline)
            raw_header = struct.unpack_from("<BBHIQ", data)
            processed_header = struct.unpack_from("<BBHIQ", data, 64)
            if (raw_header[:3] != (1, 1, 32) or processed_header[:3] != (2, 1, 48)
                    or raw_header[3:] != processed_header[3:] or any(data[48:64])):
                raise ProtocolError("Invalid LIVE record envelopes")
            raw = struct.unpack_from("<I3h3h4I", data, 16)
            processed = struct.unpack_from("<I9f2I", data, 80)
            if not all(math.isfinite(v) for v in processed[1:10]):
                raise ProtocolError("Non-finite LIVE reading")
            return {
                "source": "sensor_task", "sequence": raw_header[3],
                "timestamp_us": raw_header[4], "sample_age_ms": age_ms,
                "raw": {"flags": raw[0], "accel_counts": list(raw[1:4]),
                        "gyro_counts": list(raw[4:7]), "pressure_adc": raw[7],
                        "temperature_adc": raw[8], "baro_age_us": raw[9], "errors": raw[10]},
                "processed": {"flags": processed[0], "acceleration_m_s2": list(processed[1:4]),
                              "angular_rate_rad_s": list(processed[4:7]),
                              "pressure_pa": processed[7], "temperature_c": processed[8],
                              "altitude_m": processed[9] if processed[0] & 256 else None,
                              "baro_age_us": processed[10], "errors": processed[11]},
            }
        return self._transaction("LIVE", read)

    def close(self):
        try:
            if not self.broken:
                self.command("CLOSE")
        except (ProtocolError, OSError):
            pass  # Closing a failed/removed device must still release the port.
        finally:
            self.port.close()


def save_json(path: Path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8") as f:
        json.dump(value, f, indent=2)
        f.write("\n")
        f.flush()
        os.fsync(f.fileno())
    os.replace(temporary, path)


CSV_FIELDS = ["session", "type", "sequence", "timestamp_us", "trigger_relative_s", "flags", "errors", "baro_age_us",
              "accel_x_counts", "accel_y_counts", "accel_z_counts", "gyro_x_counts", "gyro_y_counts", "gyro_z_counts",
              "pressure_adc", "temperature_adc", "accel_x_m_s2", "accel_y_m_s2", "accel_z_m_s2",
              "gyro_x_rad_s", "gyro_y_rad_s", "gyro_z_rad_s", "pressure_pa", "temperature_c", "altitude_m"]


def decode_record(data: bytes, sid: int) -> dict:
    if (len(data) != 80 or data[:4] != b"DLOG" or data[4] != 1 or data[5] not in (1, 2)
            or struct.unpack_from("<H", data, 6)[0] != (32 if data[5] == 1 else 48)
            or struct.unpack_from("<I", data, 8)[0] != sid or data[76:] != b"\0" * 4
            or struct.unpack_from("<I", data, 72)[0] != zlib.crc32(data[:72])):
        raise ProtocolError("Invalid committed sensor record")
    row = {"session": sid, "type": "raw" if data[5] == 1 else "processed",
           "sequence": struct.unpack_from("<I", data, 12)[0],
           "timestamp_us": struct.unpack_from("<Q", data, 16)[0]}
    if data[5] == 1:
        if any(data[56:72]):
            raise ProtocolError("Nonzero raw record padding")
        values = struct.unpack_from("<I3h3h4I", data, 24)
        flags = values[0]
        for axis, value in zip("xyz", values[1:4]):
            row[f"accel_{axis}_counts"] = value if flags & 1 else None
        for axis, value in zip("xyz", values[4:7]):
            row[f"gyro_{axis}_counts"] = value if flags & 2 else None
        row.update(pressure_adc=values[7] if flags & 4 else None,
                   temperature_adc=values[8] if flags & 8 else None,
                   baro_age_us=values[9], errors=values[10])
    else:
        values = struct.unpack_from("<I9f2I", data, 24)
        flags = values[0]
        if not all(math.isfinite(v) for v in values[1:10]):
            raise ProtocolError("Non-finite processed record")
        for axis, value in zip("xyz", values[1:4]):
            row[f"accel_{axis}_m_s2"] = value if flags & 1 else None
        for axis, value in zip("xyz", values[4:7]):
            row[f"gyro_{axis}_rad_s"] = value if flags & 2 else None
        row.update(pressure_pa=values[7] if flags & 4 else None,
                   temperature_c=values[8] if flags & 8 else None,
                   altitude_m=values[9] if flags & 256 else None,
                   baro_age_us=values[10], errors=values[11])
    if flags & ~(255 if data[5] == 1 else 511) or row["errors"] & ~15:
        raise ProtocolError("Reserved record flags/status bits")
    row["flags"] = flags
    return row


def export_csv(source: Path, sid: int, trigger_us=None):
    target = source.with_suffix(".csv")
    temporary = target.with_suffix(".csv.tmp")
    with source.open("rb") as binary, temporary.open("w", newline="", encoding="utf-8") as out:
        writer = csv.DictWriter(out, fieldnames=CSV_FIELDS)
        writer.writeheader()
        while data := binary.read(80):
            row = decode_record(data, sid)
            if trigger_us is not None:
                row["trigger_relative_s"] = (row["timestamp_us"] - trigger_us) / 1000000
            writer.writerow(row)
        out.flush(); os.fsync(out.fileno())
    os.replace(temporary, target)


def export_session(client: Client, sid: int, destination: Path, launch: bool = False) -> Path:
    """Rerunning after reconnect rechecks identity and verifies saved chunks."""
    uint(str(sid))
    status = client.status()
    if not status.get("uid"):
        raise ProtocolError("STATUS must include a stable device UID before exporting")
    if status.get("state") != "CAPTURING":
        client.command("STOP")
    session = next((s for s in (client.events() if launch else client.sessions()) if s.id == sid), None)
    if session is None:
        raise ProtocolError(f"Closed session {sid} not found")
    if launch and session.state != "CLOSED":
        raise ProtocolError("Launch/drop capture is still recording its five-minute window")
    destination.mkdir(parents=True, exist_ok=True)
    stem = destination / f"{'launch' if launch else 'session'}-{sid}-g{session.generation}"
    manifest_path = stem.with_suffix(".json")
    partial = stem.with_suffix(".part")
    final = stem.with_suffix(".bin")
    identity = {**asdict(session), "device_uid": status.get("uid")}
    manifest = {"format": 1, "session": identity, "chunks": [], "complete": False,
                "created_utc": datetime.now(timezone.utc).isoformat()}
    if manifest_path.exists():
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        if manifest.get("format") != 1 or manifest.get("session") != identity:
            raise ProtocolError("Manifest identity differs from device/session; choose another directory")
    elif partial.exists() or final.exists():
        raise ProtocolError("Export exists without a manifest; choose another directory")
    if status.get("metadata") == "1":
        header, metadata = client.metadata(sid, session.generation)
        if manifest.get("metadata_hex", header.hex()) != header.hex():
            raise ProtocolError("Session metadata differs from saved export")
        manifest.update(record_format=1, metadata_hex=header.hex(), metadata=metadata)
    source = final if final.exists() else partial
    offset = 0
    if source.exists():
        with source.open("rb") as f:
            for chunk in manifest["chunks"]:
                if chunk["offset"] != offset or not 1 <= chunk["length"] <= 512:
                    raise ProtocolError("Invalid saved chunk manifest")
                data = f.read(chunk["length"])
                if len(data) != chunk["length"] or f"{zlib.crc32(data):08x}" != chunk["crc32"]:
                    raise ProtocolError("Saved chunk failed verification")
                offset += len(data)
            if offset > session.length:
                raise ProtocolError("Saved export exceeds session length")
            if final.exists():
                if offset != session.length or f.read(1):
                    raise ProtocolError("Completed export length mismatch")
                if manifest.get("record_format") == 1:
                    export_csv(final, sid, manifest.get("metadata", {}).get("launch", {}).get("trigger_time_in_file_us"))
                manifest["complete"] = True
                save_json(manifest_path, manifest)
                if status.get("unpin") == "1":
                    client.command(f"UNPIN {sid}")
                return final
    elif manifest["chunks"]:
        raise ProtocolError("Saved export data is missing")
    save_json(manifest_path, manifest)
    with partial.open("r+b" if partial.exists() else "w+b") as f:
        f.truncate(offset)  # Remove bytes written before an interrupted manifest update.
        f.seek(offset)
        while offset < session.length:
            data = client.read(sid, offset, min(512, session.length - offset))
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
            manifest["chunks"].append({"offset": offset, "length": len(data),
                                       "crc32": f"{zlib.crc32(data):08x}"})
            offset += len(data)
            save_json(manifest_path, manifest)
    os.replace(partial, final)
    if manifest.get("record_format") == 1:
        export_csv(final, sid, manifest.get("metadata", {}).get("launch", {}).get("trigger_time_in_file_us"))
    manifest["complete"] = True
    save_json(manifest_path, manifest)
    if status.get("unpin") == "1":
        client.command(f"UNPIN {sid}")
    return final


def capture_logs(client: Client, output: Path, follow: bool = False, emit=None, stop=None):
    """Append raw print bytes plus timestamped capture metadata to disk."""
    output.parent.mkdir(parents=True, exist_ok=True)
    cursor = 0
    with output.open("ab") as raw, output.with_suffix(output.suffix + ".jsonl").open("a", encoding="utf-8") as meta:
        while stop is None or not stop.is_set():
            start, end, lost, data = client.logs(cursor)
            offset = raw.tell()
            if data:
                raw.write(data)
                raw.flush()
                if emit:
                    emit(data.decode("utf-8", errors="replace"))
            meta.write(json.dumps({"utc": datetime.now(timezone.utc).isoformat(),
                                   "file_offset": offset, "cursor": start, "length": len(data),
                                   "device_end": end, "overwritten": lost,
                                   "gap": (start - cursor) & U32_MAX}) + "\n")
            meta.flush()
            cursor = (start + len(data)) & U32_MAX
            if cursor == end:
                if not follow:
                    return
                if stop:
                    stop.wait(0.1)
                else:
                    time.sleep(0.1)


def open_client(device: str) -> Client:
    try:
        import serial
    except ImportError as exc:
        raise RuntimeError("Install client dependencies: python3 -m pip install -r client/requirements.txt") from exc
    # Retry only the read-only connection handshake, never user operations.
    for attempt in range(3):
        port = serial.Serial(None, 115200, timeout=0.1, write_timeout=1)
        try:
            port.port = device
            port.dtr = False
            port.open()
            # Let the old blocking OUT read abort before starting a new epoch.
            time.sleep(0.1)
            port.reset_input_buffer()
            port.reset_output_buffer()
            port.dtr = True
            time.sleep(0.15)
            port.reset_input_buffer()
            client = Client(port, timeout=1.0)
            client.status()
            client.timeout = 5.0
            return client
        except (ProtocolError, OSError):
            port.close()
            if attempt == 2:
                raise
            time.sleep(0.2)
        except BaseException:
            port.close()
            raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="CDC serial port, e.g. /dev/cu.usbmodem...")
    commands = parser.add_subparsers(dest="action", required=True)
    for name in ("ports", "status", "list", "start", "stop"):
        commands.add_parser(name)
    commands.add_parser("events", help="List protected launch/drop files separately")
    download = commands.add_parser("download")
    download.add_argument("session", type=int)
    download.add_argument("--output", type=Path, default=Path("captures"))
    download.add_argument("--launch", action="store_true", help="Download a protected file from EVENTS")
    erase = commands.add_parser("erase")
    erase.add_argument("session", type=int)
    operation = commands.add_parser("op-status")
    operation.add_argument("operation", type=int)
    logs = commands.add_parser("logs")
    logs.add_argument("--output", type=Path, default=Path("captures/usb-prints.log"))
    logs.add_argument("--follow", action="store_true")
    live = commands.add_parser("live", help="Read sensors directly, bypassing flash")
    live.add_argument("--follow", action="store_true")
    live.add_argument("--interval", type=float, default=0.2, help="Polling seconds (minimum 0.05)")
    live.add_argument("--output", type=Path, help="Also save snapshots as JSON lines")
    args = parser.parse_args()
    if args.action == "ports":
        from serial.tools import list_ports
        for p in list_ports.comports():
            print(f"{p.device}\t{p.description}")
        return
    if not args.port:
        parser.error("--port is required")
    if args.action == "live" and (not math.isfinite(args.interval) or args.interval < 0.05):
        parser.error("--interval must be at least 0.05 seconds")
    client = open_client(args.port)
    try:
        if args.action == "status":
            print(json.dumps(client.status(), indent=2))
        elif args.action == "list":
            print(json.dumps([asdict(s) for s in client.sessions()], indent=2))
        elif args.action == "events":
            print(json.dumps([asdict(s) for s in client.events()], indent=2))
        elif args.action in ("start", "stop"):
            print(client.command(args.action.upper()))
        elif args.action == "download":
            print(export_session(client, args.session, args.output, launch=args.launch))
        elif args.action == "logs":
            capture_logs(client, args.output, args.follow, emit=lambda text: print(text, end="", flush=True))
        elif args.action == "live":
            output = None
            try:
                if args.output:
                    args.output.parent.mkdir(parents=True, exist_ok=True)
                    output = args.output.open("a", encoding="utf-8")
                while True:
                    line = json.dumps({"unix_time": time.time(), **client.live()})
                    print(line, flush=True)
                    if output:
                        output.write(line + "\n"); output.flush()
                    if not args.follow:
                        break
                    time.sleep(args.interval)
            finally:
                if output:
                    output.close()
        elif args.action == "op-status":
            uint(str(args.operation))
            print(client.command(f"OP_STATUS {args.operation}"))
        elif args.action == "erase":
            uint(str(args.session))
            client.command("STOP")
            fields = client.command(f"ERASE_PREPARE {args.session}").split()
            if len(fields) != 5 or fields[:2] != ["OK", "ERASE_PREPARE"] or uint(fields[2]) != args.session:
                raise ProtocolError("Invalid erase preparation response")
            generation, token = map(uint, fields[3:5])
            expected = f"DELETE {args.session} {generation}"
            if input(f"Type '{expected}' to delete this session: ") != expected:
                print("Deletion cancelled")
                return
            print(client.command(f"ERASE_CONFIRM {args.session} {generation} {token}"))
    finally:
        client.close()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
    except (ProtocolError, OSError, RuntimeError, ValueError) as error:
        raise SystemExit(str(error))
