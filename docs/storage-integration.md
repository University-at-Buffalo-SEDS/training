# SPI flash logger implementation

Implemented on `usb-imu`, 24 November 2026. This supersedes the implementation
findings in `spi-flash-review.md`, which remains available as training material.
Native model tests and builds pass; board qualification remains separate.

## Recording and protected launch/drop files

Power-up identifies the W25Q32JV, recovers committed data, and automatically
starts recording once the sensors and a fresh pressure reference are ready.
The USB product name is **UBSEDS CrashNBurn Datalogger**. A connected client is
not required. Live sensor viewing bypasses flash and has a separate altitude zero.
The first valid pressure of each acquisition run defines its stored altitude zero.

Normal history rolls into approximately 20-second files. Older, closed,
unprotected compact files are automatically reclaimed as space or catalog slots
are needed. The most recent 20 seconds of the current acquisition are retained.
Legacy files and protected event files are never automatically reclaimed.

The detector recognizes these default conditions:

| Reason | Threshold and debounce |
|---|---|
| Acceleration spike | Magnitude at least 2.5 g for 20 ms |
| Freefall/drop | Magnitude at most 0.35 g for 100 ms |
| Barometric shift | At least 3 m change within a one-second reference window, with three fresh qualifying readings |

Barometer samples older than 200 ms and invalid channels are ignored. A sensor
gap longer than 200 ms resets detector history. These are tunable heuristics in
`launch_detector.h` and `launch_detector.c`; validate them against real handling,
noise, launches and drops. The implementation does not infer a flight trajectory.

A trigger protects the preceding 20 seconds and records the next 300 seconds at
100 Hz. Triggers during that capture combine reason flags without extending its
original deadline. Capture then closes and normal rolling recording continues.
If a trigger occurs before 20 seconds of history exist, all available history is
saved and the file is marked incomplete. Reset, data loss or an interrupted
capture also marks it incomplete. Buffered samples not yet committed can be
lost on power failure (at most seven samples, normally 70 ms).

The GUI separates **Rolling history** and **Protected launch/drop files**. The
protected tab shows reasons, available pre/post duration, capture state and
protection. Closed files download as binary, manifest and decoded CSV; the CSV's
`trigger_relative_s` places the trigger at zero. A capture in progress cannot be
downloaded or stopped through normal STOP: it returns `ERR CAPTURING`. Closed
files can be exported while another event is capturing. Otherwise downloading
stops recording; Start resumes it. Delete requires explicit confirmation.

Protection is finite: when retained protected files prevent another full capture
reservation, STATUS reports `armed=0` and counts `missed_launches`. Export and
explicitly delete protected files to restore capacity. Smaller normal recording
reservations may still be possible. No protected file is silently overwritten.
There are 64 physical file entries and eight protected event descriptors; an
event references up to seven physical sources, sharing sources when windows
overlap. Early events can consume more overhead than the nominal capacity below.

## Compact storage and capacity

The 4 MiB device has 1024 sectors of 4096 bytes. Sectors 0 and 1 are alternating
metadata journals; the remainder holds sector-aligned files. New recordings use
encoding 2: one 256-byte page holds eight complete raw samples, or **32 bytes per
sample including page metadata and CRC**. At 100 Hz this is about 3.2 kB/s,
approximately 21 minutes across an otherwise empty chip before headers and
allocation overhead. A full 20 + 300 second event occupies about 1 MiB. Typically
three non-overlapping complete events fit with room for rolling history; exact
capacity depends on placement, retained files and source sharing.

Raw counts, ADC readings, flags, errors, barometer age, sequence and timestamps
are preserved. Processed floats are regenerated for export from the saved sensor
configuration, BMP390 factory trim and pressure reference. This avoids storing
both raw and redundant processed records on flash. The exported DLOG stream
still contains both 80-byte raw and processed frames per sample, so a 320-second
export is about 5.12 MB even though it uses about 1 MiB on flash. STATUS's
`phys_bytes` reports actual journal/header/data-page occupancy; reserved extents
are separately represented by used/free sectors.

All integers are little-endian. CRC is CRC-32/ISO-HDLC. Objects become durable
only when their final four-byte commit word is zero. Body and CRC are programmed
and verified before that disjoint commit write, then verified again. Programmed
bytes are never reused without sector erase. Every data page is checked blank
before programming. No normal command issues chip erase.

### Compact page (DRP2)

Every compact data page starts with its own 24-byte header. The header tells
the reader how many sample slots to decode, which physical recording owns the
page, and how to reconstruct the samples' original times and sequence numbers.
The recording's separate DLH1 file header stores sensor configuration, factory
trim, and the reference pressure used to regenerate processed values.

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 4 | `DRP2` |
| 4 | 1 | Version 2 |
| 5 | 1 | Sample count, 1–8 |
| 6 | 1 | Trigger reason mask. Zero means no marker. Bits 0, 1, and 2 mean acceleration, freefall, and barometer. |
| 7 | 1 | Zero-based trigger sample index. If the mask is nonzero, the index must be below the sample count. Otherwise it must be zero. |
| 8 | 4 | Base sequence |
| 12 | 8 | Base source timestamp, microseconds |
| 20 | 4 | Physical source ID |
| 24 | 224 | Eight 28-byte sample slots |
| 248 | 4 | CRC of bytes 0–247 |
| 252 | 4 | Commit word |

Sample slot `i` starts at page offset `24 + 28 × i`. The sample count can be
less than eight when recording stops, a trigger forces a write, or a sample
cannot fit the current page's delta range. Unused slots are zero-filled and
covered by the CRC.

| Offset within slot | Bytes | Field |
|---:|---:|---|
| 0 | 1 | Sensor validity and freshness flags |
| 1 | 6 | Accelerometer X, Y, Z as signed 16-bit counts |
| 7 | 6 | Gyroscope X, Y, Z as signed 16-bit counts |
| 13 | 3 | Unsigned raw pressure ADC value |
| 16 | 3 | Unsigned raw temperature ADC value |
| 19 | 4 | Barometer reading age in microseconds |
| 23 | 1 | Sensor error flags |
| 24 | 2 | Unsigned timestamp delta in milliseconds |
| 26 | 2 | Unsigned sequence delta |

The first sample has zero deltas. For later samples, add the stored sequence
delta to the page's base sequence using 32-bit arithmetic, and add
`timestamp_delta_ms × 1000` to the base timestamp. For example, with base
sequence 4200 and base timestamp 42,000,000 µs, deltas 3 and 30 ms decode to
sequence 4203 at 42,030,000 µs. Storing both deltas preserves gaps in sampling
without assuming that every slot is exactly 10 ms after the previous one.

On a trigger, the worker commits the page containing the trigger marker before
updating the event catalog. If power fails between those writes, recovery can
read the marker, recover its sample time using the same delta calculation, and
rebuild the protected event. The page must have a valid CRC and a zero commit
word before recovery or export uses it.

### Journals and headers

The reader accepts existing DLJ1/version-1 journals and 80-byte physical DLOG
records. New snapshots use DLJ2/version 2. An update erases the inactive journal,
writes/verifies/commits it, and only then makes it current. The previous valid
copy remains available throughout replacement. Unknown nonblank media faults
without erasing it; fully blank media is initialized after a whole-chip scan.

Each 4096-byte journal retains the original operation/counter fields at 0–51,
64 physical file entries of 32 bytes at offset 64, and eight event entries of
96 bytes at offset 2112. CRC and commit are at 4088 and 4092. A physical entry
contains ID, generation, first sector, sector count, virtual export record count,
state/encoding flags, incomplete flag and event protection marker. State uses
bits 0–7, encoding bits 8–15 (zero means legacy), and bit 16 suppresses a deleted
shared source's old trigger marker. Each event entry stores identity, acquisition
epoch, reasons, state/incomplete, skipped prefix sample count, exported sample
count, begin/trigger/target/end timestamps, source count and seven source IDs.

The first 256-byte file page remains DLH1/version 1 with ID/generation, pressure
reference, sensor identity/configuration, factory trim, identity mounting matrix,
zero bias vectors, CRC and commit. Encoding is at 124, acquisition epoch at 128,
and source timestamp origin at 132. Event META replies add event capability/protection
flags at 140, reasons at 144, trigger/target/end timestamps at 148/156/164,
requested pre/post durations at 172/176 actual pre-duration at 180 and incomplete flag at 184. Header
CRC/commit remain at 248/252. The client saves original header bytes and decoded
fields in its manifest. Sensor axes are uncalibrated board sensor axes.

Compact allocation prefers a contiguous 256-sector reservation, falling back to
32 then four sectors for normal logging. A capture arms only with a free event
descriptor and sufficient reserved post-trigger capacity. Close releases unused
tails without erasing them; each future page is independently checked blank.
Acquisition continues across normal file rotations with the same epoch and
pressure reference. Exported event timestamps are relative to event begin;
trigger-relative CSV timestamps subtract its actual pre-duration.

## Recovery, deletion and service behavior

Mount reconstructs page-validity and per-sector sample counts, verifies closed
file lengths, and closes interrupted files as incomplete. A trigger is first
committed in its data page, then protection is committed to the journal. If the
second step tears, mount reconstructs the event from the durable trigger marker.
Shared event sources remain protected until their last event reference is
explicitly deleted. A durable suppression flag prevents a deliberately deleted
shared event's marker from resurrecting it after reset.

READ and META pin closed files for a 30-second inactivity lease. Successful
exports send UNPIN; disconnect also releases leases and preparation tokens.
Deletion preparation refuses pinned/open files. Confirmation binds connection,
ID and generation and lasts 30 seconds. Confirmation journals intent before
sector erasure. The worker checks/erases one owned sector per step; interrupted
deletion resumes after reset. Completion is committed before allocations are
freed. Repeated retained confirmations return the same operation ID. Only the
most recent operation result is retained.

STATUS advertises `firmware=storage-2 metadata=1 unpin=1 events=1` with UID,
state, active file, durable export-frame count, physical usage, queue/drop/fault
counters, event count, armed state, remaining capture seconds and missed triggers.
LIST returns normal files. EVENTS returns rows in this form, followed by OK END:

```text
OK EVENT id generation export_bytes incomplete reasons trigger_ms pre_ms post_ms CAPTURING|CLOSED
```

READ/META use the same IDs for events and ordinary files. META returns
`META id 256 crc32`, LF, 256 bytes, LF. Event export synthesizes a single logical
file from its protected sources and omits earlier history. Client downloads
verify chunk identity, CRC, metadata and record schema/CRC/commit before CSV.
Invalid channels are blank. Partial local downloads can resume after reconnect.

The priority-15 flash worker is the sole runtime flash owner, using a static
4096-byte stack. Acquisition remains 100 Hz with absolute 10 ms deadlines and
1000 ThreadX ticks/s. The output queue holds 256 envelopes (16 KiB); bounded
journal/erase operations can buffer input but sustained overload still drops it.
The worker retains copied USB replies under host backpressure. Sensor SPI and
flash SPI share a mutex; flash BUSY waits release it and sleep. JEDEC must be
EF 40 16. Reads split into 256-byte transfers; program/erase verify results.
Faults stop persistence, preserve uncertain data and leave live sensing available.

Print history includes JEDEC/recovery status, boot readback, file starts, capture
trigger/completion, durable counts and validated latest-record payload bytes read
back through flash. Prints are retrieved through LOG_READ and saved by the client.
The heartbeat thread has been removed.

## Verification

Run `python3 -m unittest discover -s tests`, then Debug and Release builds.
Native C models use ASan/UBSan and exercise the actual driver, torn journals,
record commits, reset recovery, deletion, legacy compatibility, a full-rate
320-second event, shared protection/deletion, trigger recovery after a torn
protection update, detector debouncing/stale inputs, 25 minutes of rolling
reclamation while preserving an event, USB backpressure, leases and client
export/resume/CSV. GCC -fanalyzer checks storage, codec, detector, service and
driver code.

Board validation must measure actual 100 Hz throughput, program/erase latency,
per-path drops, stack high-water marks, detector thresholds, sensor axes/bias,
USB reconnects and power cuts. Compact pages normally require 12.5 page commits
per second, rather than two writes per 200 redundant physical records per second.
Successful simulation/builds do not establish hardware throughput or flash
endurance. Preserve app_timing.h, tx_user.h and the SysTick shared timing expression
when regenerating CubeMX files; the IOC does not encode that timing override.
