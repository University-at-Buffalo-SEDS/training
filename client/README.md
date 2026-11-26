# Datalogger USB client

The `usb-imu` branch provides the flash logger, CDC transport, print capture,
Python CLI and Tk desktop app. Firmware recovers flash at boot, then starts a
new recording session automatically once sensors and a recent valid pressure
reference are available. Connecting/disconnecting the client does not stop it.
See [storage integration](../docs/storage-integration.md) for the persistent
format, recovery and hardware-validation limits.

## Run

Use Python 3.10 or newer. Create a virtual environment and install the serial
dependency:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r client/requirements.txt
.venv/bin/python client/datalogger_client.py ports
.venv/bin/python client/datalogger_gui.py
```

The GUI also needs Tkinter (provided by a Python distribution with Tk support).
Choose the UBSEDS CrashNBurn Datalogger CDC port, set a save folder, then Connect. The GUI displays
prints and automatically appends their original bytes to a timestamped `.log`,
with capture offsets, gaps, and timestamps in `.log.jsonl`. Open saved print log
loads a previous capture. Status/Start/Stop/List and Download selected run in a
background worker so the window stays responsive. Download selected stops normal
logging before exporting. An active protected capture cannot be stopped or
downloaded; other closed files can be downloaded while it continues.

Device status shows recording state, active session, durable export records, actual flash data usage and storage drops. Click Start after a Stop/download to begin another
session. Delete selected stops recording, asks for explicit confirmation and
shows deletion progress. Successful downloads release their export pin. An
interrupted read/export stays pinned until disconnect or 30 seconds of inactivity;
reconnect before deleting if still pinned. Initial
recovery can take several seconds. FAULT never formats unknown flash. Normal compact history rolls in roughly
20-second files and older unprotected history is reclaimed automatically.
Protected files and legacy recordings require explicit deletion.

The **Protected launch/drop files** tab lists captures triggered by acceleration
spikes, freefall or fresh barometric shifts. They preserve 20 seconds before and
five minutes after the first trigger; later triggers combine reasons. Early-boot
or interrupted captures are marked incomplete. At 100 Hz a full capture uses
about 1 MiB of compact flash storage, with raw and processed data reconstructed
into a larger export. The device shows capture progress and whether enough space
remains to arm another capture. Export and explicitly delete protected files when
capacity runs out. Launch CSV includes `trigger_relative_s` (normally −20 to +300).

Enable **Live sensors (bypass flash)** to view sensor-task readings directly.
This stays independent of stored sessions and flash faults. The GUI polls five
snapshots per second and saves raw counts, SI values,
flags, errors and timestamps in a `*-sensors.jsonl` file beside the print capture.
Invalid readings are labeled; old snapshots show STALE. Live snapshots are not
a lossless 100 Hz recording. Downloads pause polling.
Relative altitude is shown in meters: the first valid pressure after connecting
defines 0 m, and the reference stays fixed until reconnecting. Pressure and
temperature remain visible, and saved snapshots include altitude.

Connection setup waits for DTR to settle and verifies STATUS before reporting
success. Transient startup failures get up to three handshake attempts; session
operations are not automatically replayed.

The CLI supports the full command set, including confirmed deletion:

```sh
.venv/bin/python client/datalogger_client.py --port /dev/cu.usbmodemXXXX status
.venv/bin/python client/datalogger_client.py --port /dev/cu.usbmodemXXXX logs --follow --output captures/prints.log
.venv/bin/python client/datalogger_client.py --port /dev/cu.usbmodemXXXX live --follow --output captures/sensors.jsonl
.venv/bin/python client/datalogger_client.py --port /dev/cu.usbmodemXXXX list
.venv/bin/python client/datalogger_client.py --port /dev/cu.usbmodemXXXX events
.venv/bin/python client/datalogger_client.py --port /dev/cu.usbmodemXXXX start
.venv/bin/python client/datalogger_client.py --port /dev/cu.usbmodemXXXX stop
.venv/bin/python client/datalogger_client.py --port /dev/cu.usbmodemXXXX download 7 --output captures
.venv/bin/python client/datalogger_client.py --port /dev/cu.usbmodemXXXX download 8 --launch --output captures
.venv/bin/python client/datalogger_client.py --port /dev/cu.usbmodemXXXX erase 7
.venv/bin/python client/datalogger_client.py --port /dev/cu.usbmodemXXXX op-status 12
```

Use the actual port name (`COM...` on Windows, `/dev/ttyACM...` on Linux).
Only one application may open the port at once. DTR must be asserted. A regular
terminal will no longer receive unsolicited `alive` messages: send `STATUS` for
health, or use the client for binary-safe print capture. The CDC baud value does
not control data rate. USB CDC is distinct from ROM DFU firmware programming.

## Print history and persistence

Firmware redirects stdout/stderr and `__io_putchar` to an 8 KiB RAM ring without
waiting for USB. It retains recent output even when no client is connected. Old
bytes are overwritten when full, and overwrite/gap counts are reported. Reset
or loss of board power clears this RAM history. Captured host files remain
available later; print output is not written to sensor flash sessions. Concurrent
callers can interleave text; each raw output byte is retained in ring order.
Log cursors are unsigned 32-bit byte positions and wrap modulo 2³².

Prints are retrieved with `LOG_READ` and never inserted into command replies or
binary DATA payloads. During a long export the GUI prioritizes the download;
prints continue accumulating on the board and may wrap before polling resumes.

## Export and resume

Export performs STATUS → STOP → LIST/EVENTS → repeated READ. During an active
protected capture, STOP is omitted and only closed files may be exported. Each chunk has its session,
offset, length, and CRC32 verified before saving. The output includes the raw
`.bin` and a JSON manifest with device identity, session generation, logical
length, incomplete flag, and per-chunk CRCs.

Current firmware advertises `metadata=1`. The client retrieves and validates
the session header, saves its original bytes and decoded sensor configuration,
factory trim and pressure reference in the manifest, and validates each durable
record's schema/CRC/commit. It generates `.csv` with raw counts and processed
SI values, flags, errors and relative altitude. Invalid channels are blank.
Older firmware without metadata support retains binary-only export.

An interrupted download leaves a `.part` file. Reconnect and rerun the same
download command to resume. The client rechecks the session ID, generation,
device UID, and logical length, verifies every saved chunk, and resumes at its
committed local offset. Different generations get separate files. Firmware must
include a stable `uid` in STATUS for cross-board identity checks; the current
local STATUS does. Malformed/partial frames and failed CRCs require reconnect.

Export never erases data. `erase` obtains a preparation token and then asks you
to type `DELETE <session> <generation>`. Confirmation can expire while waiting;
prepare again if needed. Completion is queried through `op-status`, not assumed
from acceptance. The GUI provides the same preparation/confirmation flow and
polls OP_STATUS until completion or failure. The CLI stops recording before
preparation; use `op-status` to check its returned operation ID.

## Verify without hardware

```sh
python3 -m unittest discover -s tests -v
./build.py build --debug
./build.py build --release
```

Tests use fragmented byte streams, real CRC calculations, interrupted exports,
and a native C harness with AddressSanitizer/UndefinedBehaviorSanitizer for the
parser, print ring, queue saturation, and chunk ownership. A C compiler with
sanitizers is needed for the native test. Board enumeration, DTR transitions,
USB unplug/reset, a stalled host, and stack high-water marks still require a
powered STM32 board and its native USB connector.
