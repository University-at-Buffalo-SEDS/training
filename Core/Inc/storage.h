#pragma once
#include "records.h"
#include "launch_detector.h"

#define STORAGE_SECTOR_SIZE 4096U
#define STORAGE_SECTORS 1024U
#define STORAGE_MAX_SESSIONS 64U
#define STORAGE_SLOT_SIZE 80U
#define STORAGE_HEADER_SIZE 256U
#define STORAGE_MAX_EVENTS 8U
#define STORAGE_EVENT_SOURCES 7U
enum
{
    STORE_OK,
    STORE_IO,
    STORE_CORRUPT,
    STORE_FULL,
    STORE_STATE,
    STORE_BOUNDS,
    STORE_NOT_FOUND,
    STORE_PARAM
};
enum
{
    SESSION_OPEN = 1,
    SESSION_CLOSED,
    SESSION_DELETING
};
enum
{
    STORE_OP_NONE,
    STORE_OP_RUNNING,
    STORE_OP_DONE,
    STORE_OP_FAILED
};
typedef struct
{
    void *context;
    int (*read)(void *, uint32_t, uint8_t *, size_t);
    int (*program)(void *, uint32_t, const uint8_t *, size_t);
    int (*erase)(void *, uint32_t);
    void (*yield)(void *);
} storage_io;
/* One physical extent and the counters needed to read its committed data. */
typedef struct
{
    uint32_t id, generation, first_sector, sectors, records, state, incomplete;
    uint32_t next_slot; /* Reconstructed, never a native persistent struct. */
    uint32_t format, event_id, epoch, event_suppressed;
    uint64_t origin_us, first_us, last_us;
} storage_session;
/* One protected export spanning one or more existing physical extents. */
typedef struct
{
    uint32_t id, generation, epoch, reasons, state, incomplete, skip_samples, samples;
    uint64_t begin_us, trigger_us, target_us, end_us;
    uint32_t source_count, sources[STORAGE_EVENT_SOURCES];
} storage_event;
typedef struct
{
    storage_io io;
    storage_session sessions[STORAGE_MAX_SESSIONS];
    storage_event events[STORAGE_MAX_EVENTS];
    uint32_t journal_generation, next_id, journal_slot, active;
    uint32_t delete_id, delete_op, delete_progress, last_op;
    uint32_t op_state, op_session, op_generation, op_error, op_total;
    uint32_t mounted, fault, io_bytes;
    /* Rebuilt on mount. Per-sector totals let reads skip whole sectors. */
    uint8_t page_state[STORAGE_SECTORS * 16 / 2];
    uint8_t sector_records[STORAGE_SECTORS];
    uint8_t journal[STORAGE_SECTOR_SIZE], scratch[256];
    /* Samples in this buffer do not count as durable until flushed. */
    uint8_t pending_page[256];
} storage;

/* Sole data-worker owner. No dynamic allocation. I/O callbacks return zero on
 * success. Programs may not cross a 256-byte page. Never auto-format nonblank
 * unknown media. On uncertainty, stop writes and require recovery/reset. */
int storage_mount(storage *s, const storage_io *io);
storage_session *storage_find(storage *s, uint32_t id);
int storage_start(storage *s, const sensor_metadata *metadata, float reference_pa, uint32_t *id);
int storage_append(storage *s, const logger_record *record);
int storage_close(storage *s, int incomplete);
int storage_read(storage *s, uint32_t id, uint32_t offset, uint8_t *out, size_t count);
int storage_metadata(storage *s, uint32_t id, uint8_t out[256]);
int storage_delete_begin(storage *s, uint32_t id, uint32_t *operation);
int storage_delete_step(storage *s);
uint32_t storage_used_sectors(const storage *s);
/* Data-worker diagnostic: read and validate the active session's latest slot. */
int storage_latest(storage *s, uint8_t out[80]);
int storage_start_compact(storage *s,
                          const sensor_metadata *metadata,
                          float reference_pa,
                          uint32_t epoch,
                          uint64_t origin_us,
                          unsigned reserve_sectors,
                          uint32_t *id);
int storage_flush(storage *s);
storage_event *storage_event_find(storage *s, uint32_t id);
int storage_trigger(storage *s, const logger_record *raw, unsigned reasons);
int storage_event_note(storage *s, unsigned reasons);
int storage_reclaim_begin(storage *s, uint32_t id, uint32_t *operation);
int storage_can_reserve(const storage *s, unsigned sectors);
