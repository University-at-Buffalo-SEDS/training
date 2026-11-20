#!/usr/bin/env python3
"""USB CDC client. Only opening a real serial port requires pyserial."""
from __future__ import annotations

import argparse
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


def export_session(client: Client, sid: int, destination: Path) -> Path:
    """Rerunning after reconnect rechecks identity and verifies saved chunks."""
    uint(str(sid))
    status = client.status()
    if not status.get("uid"):
        raise ProtocolError("STATUS must include a stable device UID before exporting")
    client.command("STOP")
    session = next((s for s in client.sessions() if s.id == sid), None)
    if session is None:
        raise ProtocolError(f"Closed session {sid} not found")
    destination.mkdir(parents=True, exist_ok=True)
    stem = destination / f"session-{sid}-g{session.generation}"
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
                manifest["complete"] = True
                save_json(manifest_path, manifest)
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
    manifest["complete"] = True
    save_json(manifest_path, manifest)
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
    download = commands.add_parser("download")
    download.add_argument("session", type=int)
    download.add_argument("--output", type=Path, default=Path("captures"))
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
        elif args.action in ("start", "stop"):
            print(client.command(args.action.upper()))
        elif args.action == "download":
            print(export_session(client, args.session, args.output))
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
