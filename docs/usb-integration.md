# USB transport and data-worker integration

## Workers and stack inventory

`Core/Inc/T-Threads.h` follows RFBoard's `RF-Threads.h`: it declares thread
objects, entry functions, and creation functions. `app_threadx.c` creates the
flash and acquisition workers; USBX class initialization calls `create_usb_thread()` after installing
the class. The USB adapter starts the PMA/DCD/controller once. There is no
additional generated USB application thread.

| Worker | Priority / preemption threshold | Static stack | Source |
|---|---|---:|---|
| Sensor | 5 / 5 | 2048 B | `Core/Src/sensor_thread.c` |
| Processing | 10 / 10 | 4096 B | `Core/Src/processing_thread.c` |
| USB adapter | 20 / 20 | 2048 B | `Core/Src/usb_thread.c` |
| USB receive | 21 / 21 | 1024 B | `Core/Src/usb_rx_thread.c` |
| Flash bring-up | 15 / 15 | 4096 B | `Core/Src/flash_thread.c` |

No worker uses a time slice. The extra receive worker is an implementation
addition to the SDD's single USB adapter: this USBX version's synchronous OUT
read blocks, so keeping it in the adapter would stop command responses while
waiting for the next input packet. RX reads at most 64 bytes and copies them
into 64-byte queue messages (48 bytes of data plus metadata). Commands and
responses each have eight 64-byte envelopes. Two owned 512-byte response chunks
serve the data worker. Print history is 8192 B; the adapter has a separate
512-byte snapshot and 256-byte formatting buffer. Queues/stacks are static;
USBX retains its own 20 KiB arena from the existing 24 KiB pool. CDC callback
transmission workers remain disabled. ThreadX's timer thread is additional
infrastructure (default priority/stack from the bundled ThreadX configuration).

Root CMake enables `TX_ENABLE_STACK_CHECKING` consistently for application and
middleware. A stack error records `diagnostic_stack_error_thread` then enters
`Error_Handler`. GNU `.su` files provide per-function static estimates; they are
not measured worst-case stack use. Measure thread high-water marks under
command/print/export load before changing the SDD's stack budgets.

## Transport

Requests are strict ASCII, one outstanding request, at most 128 bytes including
LF, optionally CRLF. The parser rejects signed/overflowing numbers, invalid
characters, unexpected arguments, and READ counts outside 1–512. Invalid input
is discarded through LF. RX queue loss also discards through the next LF, so a
suffix cannot execute as a command. All numeric request arguments are decimal.
The CDC read worker cannot wait on application/storage queues. IN transfers have
a per-transfer timeout of about 250 ms; a failed/partial frame aborts the pipe
and requires a DTR close/reopen. Before each new synchronous transfer, its
sole owning thread drains stale completion tokens left by a prior abort. Response inactivity times out after 3 s (each
backend continuation renews it). Firmware work after timeout must be reconciled
with STATUS/OP_STATUS; do not blindly repeat START or deletion.

Base requests follow SDD §6: STATUS, LIST, START, STOP, READ, ERASE_PREPARE,
ERASE_CONFIRM, OP_STATUS. Backend text responses fill in these concrete formats:

```text
OK STATUS protocol=1 firmware=... uid=... state=... active=... storage=... durable=... <fault/drop counters>
OK SESSION <id> <generation> <logical_length> <incomplete:0|1>
OK END
OK START <id>
OK STOP
OK ERASE_PREPARE <id> <generation> <token>
OK ERASE_CONFIRM <operation_id>
OK OP_STATUS <operation_id> <state> <progress>
ERR <code> <detail>
DATA <id> <offset> <length> <crc32_hex8>
<exactly length binary bytes><LF>
```

LIST emits zero or more OK SESSION rows followed by OK END. STOP should succeed
when already stopped (but must complete the cutoff/drain if logging). The
backend should keep text payloads to one LF-terminated line (≤512 B). CRC uses
CRC-32/ISO-HDLC, matching the SDD and Python `zlib.crc32`.

Extensions for bounded print history and explicit lease release:

```text
LOG_READ <cursor> <count:1..512>
LOG <actual_start> <length:0..512> <current_end> <overwritten_bytes> <crc32_hex8>
<exactly length binary bytes><LF>
CLOSE
OK CLOSE
```

The print cursor is modulo 2³². If it is no longer retained, actual_start clamps
to the oldest available byte. A zero-length snapshot still has a final LF and
CRC `00000000`. Prints are polled, never asynchronously mixed with other frames.
CLOSE replies then invalidates the connection; reopen DTR before more commands.

## Connect the flash/data task

The [sensor integration](sensor-integration.md) supplies shared SPI transactions,
metadata, record queues and start/stop/drain counters. `LIVE` stays local to the
adapter even after backend readiness. It serves a RAM snapshot, bypassing flash
and record queues, with CRC-framed raw and processed envelopes.

`Core/Inc/usb_service.h` is the boundary; USB never calls SPI or uses physical
addresses. No session persistence is fabricated on this branch. Until the data
worker registers readiness, STATUS describes `state=NOT_READY` and other session
commands return `ERR NOT_READY storage_worker_unavailable`.

1. After flash identification and recovery, call `usb_backend_set_ready(1)`.
2. On each bounded data-worker pass, compare your last connection with
   `usb_connection`. Release old export leases and confirmation tokens after
   in-flight data access is quiescent, even when no new command arrives. This
   must also run on disconnect; it cannot depend on enqueuing a CLOSE command.
3. Nonblocking receive from `usb_commands` into a `usb_command`. Reject stale
   work using `usb_command_is_current()`. Validate state, session, generation,
   logical bounds, lease, and token in the data task. The parser only validates
   syntax and integer bounds. USB does not implement the 30 s lease/token policy.
4. Maintain the SDD's storage/service fairness. STOP must honor the producer
   acknowledgments and queue drain; deletion must journal intent and progress.
   Maintain accepted-operation identity to handle duplicate confirmations.
5. Call `usb_reply(&command, USB_REPLY_TEXT, final, text, length)` for lines, or
   `usb_reply(&command, USB_REPLY_DATA, 1, chunk, command.count)` for READ. The
   adapter generates DATA headers/CRC/trailing LF; do not include them in chunk
   payload. For LIST rows set final=0, and final=1 for OK END. READ returns exactly
   the validated requested count; bounds violations return ERR rather than a
   short DATA frame.
6. `usb_reply` copies into one of two chunks. On `TX_SUCCESS`, source memory is
   reusable immediately. On `TX_QUEUE_FULL`, retain your response state and retry
   on a later pass; never block the flash worker. On `TX_DELETED`, abandon the
   stale response. Queued descriptors contain indices and generation numbers.
   USB frees a chunk only after write completion/abort, and retires stale chunks
   after disconnect. Never call USBX write from data, sensors, or printf hooks.
7. Include stable silicon UID and the SDD's real storage, durability, fault and
   per-path drop counters in backend STATUS. Call `usb_backend_set_ready(0)` if
   storage becomes unavailable.

Connection IDs change on activation, deactivation, actual DTR edges,
explicit close, and transport failure. The worker must not cancel an already journaled
erase merely because its originating connection closed. Its status remains
queryable through OP_STATUS. Similarly, acceptance is not proof of completion.

Repeated CDC settings and RTS/baud changes leave the connection intact. The
client opens with DTR low, allows 100 ms for the old reader to abort, clears
buffers, raises DTR, and waits 150 ms before a STATUS handshake. It retries this
read-only handshake at most three times, closing failed ports between attempts.
User commands are never automatically replayed by connection setup.

## CubeMX regeneration

Keep root CMake sources and `T-Threads.h`/thread modules. Reapply the intentional
replacement in `USBX/App/app_usbx_device.c`: remove CubeMX's app-thread object,
allocation/create block, and entry stub, and call `create_usb_thread(byte_pool)`
in `MX_USBX_Device_Init1`. The generated USBX class/framework setup remains.
Keep adapter stack=2048 and priority=20 if CubeMX recreates header constants.
Preserve CDC callback USER CODE hooks for connection epochs and pipe aborts.
Preserve existing PMA endpoint addresses (bulk OUT 0x01, bulk IN 0x81,
notification IN 0x82), IRQ hookup, HSI48 SOF recovery, and the ST DCD sources.
Rebuild Debug/Release and repeat physical USB tests after regeneration.
