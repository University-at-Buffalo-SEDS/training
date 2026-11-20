# Datalogger USB client

The `usb` branch provides a CDC command transport, print capture, a Python CLI,
and an optional Tk desktop app. The flash branch currently has an idle worker;
session commands return `ERR NOT_READY` until the data worker implements the
[queue contract](../docs/usb-integration.md). `STATUS`, `LOG_READ`, `LIVE`, and `CLOSE`
work without that worker. Downloads/deletion are implemented in the client and
tested with simulated device responses, not claimed as working flash firmware.

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
Choose the STM32 CDC port, set a save folder, then Connect. The GUI displays
prints and automatically appends their original bytes to a timestamped `.log`,
with capture offsets, gaps, and timestamps in `.log.jsonl`. Open saved print log
loads a previous capture. Status/Start/Stop/List and Download selected run in a
background worker so the window stays responsive. Download selected stops
logging before listing/exporting a closed session.

Enable **Live sensors (bypass flash)** to view sensor-task readings directly.
This works before flash is implemented and stays independent of stored sessions
later. The GUI polls five snapshots per second and saves raw counts, SI values,
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
.venv/bin/python client/datalogger_client.py --port /dev/cu.usbmodemXXXX start
.venv/bin/python client/datalogger_client.py --port /dev/cu.usbmodemXXXX stop
.venv/bin/python client/datalogger_client.py --port /dev/cu.usbmodemXXXX download 7 --output captures
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

Export performs STATUS → STOP → LIST → repeated READ. Each chunk has its session,
offset, length, and CRC32 verified before saving. The output includes the raw
`.bin` and a JSON manifest with device identity, session generation, logical
length, incomplete flag, and per-chunk CRCs. It does not decode records or claim
that chunk CRC validation proves every record's commit/CRC.

An interrupted download leaves a `.part` file. Reconnect and rerun the same
download command to resume. The client rechecks the session ID, generation,
device UID, and logical length, verifies every saved chunk, and resumes at its
committed local offset. Different generations get separate files. Firmware must
include a stable `uid` in STATUS for cross-board identity checks; the current
local STATUS does. Malformed/partial frames and failed CRCs require reconnect.

Export never erases data. `erase` obtains a preparation token and then asks you
to type `DELETE <session> <generation>`. Confirmation can expire while waiting;
prepare again if needed. Completion is queried through `op-status`, not assumed
from acceptance. The GUI deliberately leaves deletion to this explicit CLI flow.

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
