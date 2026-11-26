# SPI flash review and manual fixes

Reviewed on 2026-11-20. This is a training review: the suggestions below have
not been applied to firmware. No flash was programmed or erased during review.

The driver has the basic command sequence right: address bounds, page-boundary
checks, write-enable latch (WEL) verification, and readback verification. Fix the
following issues before relying on it for recording.

## Findings

### High: read chunks exceed the SPI timeout

Location: `Drivers/flash/w25q32jv.h`, definitions of `W25Q_READ_CHUNK` and
`W25Q_SPI_TIMEOUT_MS`; `w25q_read()` in `Drivers/flash/w25q32jv.c`.

The driver permits a 65,535-byte read with a 5 ms SPI timeout. At the configured
8 MHz SPI clock, the payload alone takes about 65.5 ms. Large reads can time out
after partially filling the output buffer, and hold the shared bus too long.

Suggested manual fix:

- Reduce read chunks to 256–512 bytes, releasing the SPI mutex between chunks.
- Remove the duplicate `W25Q_READ_CHUNK` definition.
- Keep timeout and bus-hold budgets consistent with the configured SPI clock.

### High: BUSY polling can starve USB

Location: `wait_not_busy()` in `Drivers/flash/w25q32jv.c`.

`tx_thread_relinquish()` yields to ready threads at the same priority. It does
not let lower-priority USB workers run while the priority-15 flash worker remains
ready. A sector erase can therefore stall USB for hundreds of milliseconds even
though the SPI mutex is released between polls.

Suggested manual fix:

- Sleep between polls so lower-priority workers can run.
- Account for scheduler resolution: the current ThreadX tick is 10 ms, while
  the page-program timeout is 3 ms.
- Recheck BUSY after waking before declaring timeout. Simply replacing
  `tx_thread_relinquish()` with a sleep in the current loop can report timeout
  without checking whether the operation completed during that sleep.
- Keep a bounded deadline for a genuinely stuck device.

### Medium: initialization assumes flash is already awake and idle

Location: `w25q_init()` in `Drivers/flash/w25q32jv.c`.

Initialization requests JEDEC ID after a power-up delay. An MCU-only reset need
not reset the external flash: an earlier operation may still be busy, or flash
may be in power-down mode. Identification can then fail despite a working part.

Suggested manual fix:

- Handle release from power-down and its required delay.
- Allow a bounded wait for an earlier operation to complete before identification.
- Continue rejecting unexpected IDs; do not guess geometry.

### Medium: public operations do not enforce successful initialization

Location: public read/program/erase functions in `Drivers/flash/w25q32jv.c`.

Checking `dev` alone does not establish that its SPI handle and chip-select port
are valid or that JEDEC identification succeeded. A caller could use a partially
initialized device after initialization failed.

Suggested manual fix:

- Validate device handles and chip-select arguments.
- Track successful initialization and reject operations after failed identification.
- Initialize captured diagnostic fields so failure reports cannot print
  uninitialized ID bytes.

### Medium: unused command helper has weaker guarantees

Location: `flash_send_command()` in `Core/Src/flash_thread.c`.

This helper ignores mutex-acquisition results, waits indefinitely, and accepts
nonzero argument lengths with a null argument buffer, leaving transmitted bytes
uninitialized. It also duplicates the driver transaction path.

Suggested manual fix: remove the unused helper, or give it checked arguments,
bounded mutex acquisition, exclusive chip selection, and cleanup on every path.
Include `<string.h>` explicitly if retaining its `memcpy()` call.

## Current integration limitation

`flash_thread_entry()` initializes the flash and then sleeps. It does not consume
sensor record queues, program records, recover sessions, or implement USB storage
commands. A successful initialization message does not establish that sensor
logging is implemented.

## Suggested debug print

Manually insert the following after successful initialization in
`flash_thread_entry()`. It reads the first 64 bytes and prints their addresses
and hexadecimal values through the existing `printf`/USB log-history path.

```c
if (ret == W25Q_OK) {
    uint8_t data[64];
    w25q_status_t status = w25q_read(&flash, 0, data, sizeof(data));

    printf("FLASH read status=%u\n", (unsigned)status);
    if (status == W25Q_OK) {
        for (size_t i = 0; i < sizeof(data); ++i) {
            if (i % 16 == 0)
                printf("FLASH 0x%06lX: ", (unsigned long)i);
            printf("%02X%c", (unsigned)data[i],
                   i % 16 == 15 ? '\n' : ' ');
        }
    }
}
```

For a periodic dump, place it in the worker loop and sleep approximately one
second between dumps. Print only after the read returns and releases the bus.
Do not print the buffer after a failed read: it may be only partially filled.

This snippet does not erase or program flash. `FF` is expected for erased bytes,
but an all-`FF` dump alone does not prove that communication or programming works.
The expected JEDEC ID is `EF 40 16`. A later write/erase test needs an explicitly
designated scratch sector and readback verification.

## Analysis performed and limits

- Ran GCC `-fanalyzer -Wextra` using the Debug compilation flags for the original
  flash driver, flash task, and shared SPI adapter. No diagnostics were reported.
- Manually inspected command sequencing, locking, transfer sizes, timeout handling,
  address/page bounds, and task integration.
- Checked timing against the included W25Q32JV datasheet and configured SPI/tick
  rates.

Compiler analysis does not establish correct peripheral timing or hardware
behavior. JEDEC identification, real read/program/erase behavior, protected-area
failures, recovery after MCU reset, and USB responsiveness during erase still
require board testing.
