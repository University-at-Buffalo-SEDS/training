#include "sensor_live.h"
#include "tx_api.h"

static logger_record latest;
static ULONG sample_tick;
static int available;
static float reference_pa;

void sensor_live_zero(void) { reference_pa = 0; }

void sensor_live_publish(const logger_record *raw)
{
    TX_INTERRUPT_SAVE_AREA
    TX_DISABLE
    latest = *raw;
    sample_tick = tx_time_get();
    available = 1;
    TX_RESTORE
}

int sensor_live_read(logger_record frame[2], uint32_t *age_ms)
{
    TX_INTERRUPT_SAVE_AREA
    TX_DISABLE
    if (!available) { TX_RESTORE return 0; }
    frame[0] = latest;
    ULONG age = tx_time_get() - sample_tick;
    TX_RESTORE
    uint64_t ms = (uint64_t)age * 1000 / TX_TIMER_TICKS_PER_SECOND;
    *age_ms = ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
    sensor_processed processed;
    record_process(&frame[1], &frame[0], &sensors_metadata, reference_pa, &processed);
    /* A cached but recent sample is fine; never anchor to invalid/stalled data. */
    if (reference_pa == 0 && (processed.flags & 12) == 12 &&
        processed.baro_age_us <= 200000 && *age_ms <= 200) {
        reference_pa = processed.pressure_pa;
        record_process(&frame[1], &frame[0], &sensors_metadata, reference_pa, &processed);
    }
    return 1;
}
