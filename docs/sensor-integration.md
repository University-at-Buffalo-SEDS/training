# Sensors and storage integration

BMI088 and BMP390 initialize on SPI1 at boot. The sensor worker prints device
identities and availability, then polls every 10 ms even without storage.
Initialization failures clear channel validity and prevent session START; reset
after correcting a fault to repeat initialization. Register setup and compensation
were adapted from flightcomputer26; its license is retained in
`third_party/flightcomputer26-LICENSE`. No sibling repository is needed to build.

| Device | Configuration | Processed units |
|---|---|---|
| BMI088 acceleration | 100 Hz normal, ±24 g | m/s² |
| BMI088 gyro | 200 Hz, 23 Hz bandwidth, ±2000 °/s | rad/s |
| BMP390 | 25 Hz, pressure ×8, temperature ×1, IIR coefficient 3 | Pa, °C |

Settings are read back during initialization. The gyro bandwidth check ignores
the read-only bit 7, which always reads as 1 (BMI088 datasheet §5.5.6), while
still checking writable bits. BMP390 uses pressure OSR in bits
2:0, temperature OSR in bits 5:3, filter selection in bits 3:1 and unsigned T2
trim. Compensation uses double intermediates. Invalid conversions clear validity.
Axes remain sensor axes with identity mounting transform and zero bias; physical
alignment/calibration remains a board qualification step.

Acceleration freshness uses data-ready status. Gyro data-ready is a short pulse
unsuitable for polling; its 200 Hz conversion rate exceeds the 100 Hz poll rate.
Barometer status is checked once per poll. Cached ADC data retains validity and
age, with freshness clear and stale status set. Read failures report errors
separately from a successful status read with no new data. No barometer data is
valid before its first successful read.

## Storage task boundary

`acquisition_init()` creates the SPI mutex, acquisition event flags, sensor thread
(priority 5, 2048-byte stack), processing thread (priority 10, 4096-byte stack),
`sensor_to_process` (32 × 64 bytes) and `records_to_data` (128 × 64 bytes).

The future priority-15 storage task owns session controls:

1. Identify/recover flash and establish the session ID and positive reference
   pressure. Persist versioned `sensors_metadata`: IDs, actual register settings
   and 21 calibration bytes. Encode fields explicitly, not the native struct.
2. Call `acquisition_start(session_id, reference_pa)`. It rejects missing sensors,
   active/pending producers, undrained old records and invalid arguments. Session
   context and reference remain immutable until STOP drains.
3. Drain `records_to_data` with bounded work per pass. Its envelopes already use
   the SDD §15 little-endian encoding: 16-byte header plus 48-byte payload area.
   Raw type 1 uses 32 bytes; processed type 2 uses 48. Storage adds the session ID,
   CRC and commit for durable slots; never persist an envelope directly.
4. Service USB commands and flash deadlines too. `ACQUISITION_RECORD` and
   `ACQUISITION_PROGRESS` wake the worker, but a bounded timed wait must also
   service USB and internal flash progress. See `usb-integration.md`.
5. On STOP, call `acquisition_stop()`. A sample in progress may finish. Wait for
   `acquisition_stopped()`, drain `records_to_data`, finish/verify writes and then
   close. Apply the SDD's 2-second timeout and mark an incomplete session on
   timeout. START cannot discard old records for you.
6. Include `acquisition_get_status()` in backend STATUS: last sequence, accepted
   and processed counts, separate processing/raw-log/processed-log drop counters,
   and missed deadlines. Counters reset on START.

Sensor attempts both deliveries independently with `TX_NO_WAIT`. Processing
produces one result per accepted input and attempts one nonblocking log delivery.
No fake storage task drains queues or claims persistence. Session commands remain
NOT_READY until storage registers USB readiness. STOP affects session records;
live RAM snapshots continue updating.

Sensors use `spi_bus_read`/`spi_bus_write`; the merged W25Q driver holds the same
global `spi1_mutex` for its complete transactions. The mutex is created once by
`spi_bus_init()` during acquisition setup, before flash or sensor tasks run.
The priority-inheritance mutex
covers header/dummy/payload and select cleanup on all paths. Mutex acquisition
is bounded to about 100 ms and each HAL transfer to 10 ms. Selects PB11–PB14 are
exclusive. Release SPI during flash internal busy time, queue waits and USB I/O.

## Live snapshots

`LIVE` stays adapter-local even after a storage backend registers. It reads RAM
without SPI, flash access, session controls or consuming record queues. Reply:
`LIVE 1 128 <sample_age_ms> <crc32>\n`, 128 binary bytes, then LF. The body contains
one raw envelope and its processed envelope, with matching sequence/timestamp;
CRC covers all 128 bytes. Before the first sample the reply is
`ERR NOT_READY sensors_initializing`.

Live sequence is separate from session sequence. Timestamps are monotonic
microseconds since polling began, with actual ThreadX tick resolution. Age is
time since publication. The first valid, recent live pressure after connecting
defines 0 m altitude. That pressure reference stays fixed until reconnecting;
lower pressure gives positive altitude and higher pressure gives negative altitude.
Invalid/stalled samples cannot establish the reference. Live zeroing is separate
from a stored session's reference. These are sensor-axis values, not navigation data.

The GUI setting **Live sensors (bypass flash)** polls at 5 Hz and saves decoded
snapshots to `usb-prints-<timestamp>-sensors.jsonl`. CLI:
`live --follow --interval 0.2 --output captures/sensors.jsonl`. Skipped sequences
are expected: this is a latest-sample viewer, not lossless 100 Hz capture.
Exports pause GUI polling. Flags/errors show sensor faults; snapshots older than
one second are marked STALE.

## Verification

`python3 -m unittest discover -s tests` covers register/dummy framing, readback
and identity gates, SPI failure cleanup, cached/stale readings, an analytically
known compensation vector (including unsigned T2), SI units, record encoding,
queue pressure, start/stop and real worker drain, live framing, CRC and schema
rejection under ASan/UBSan. Debug and Release builds check linkage/memory budgets.

Hardware sensor values, SPI timing, USB and thread high-water marks still need
a powered board. The current 100 Hz ThreadX tick gives 10 ms timestamp resolution;
the SDD's 1 ms jitter target is not claimed. Flash storage/recovery, session
metadata persistence and deletion belong to the remaining flash driver/data task.
