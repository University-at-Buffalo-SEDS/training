#include "sensor_live.h"
#include "tx_api.h"

static logger_record latest;
static ULONG sample_tick;
static int available;
/* The USB live view keeps its own altitude zero independent of recording. */
static float reference_pa;

void sensor_live_zero(void)
{
    reference_pa = 0;
}

void sensor_live_publish(const logger_record *raw)
{
    /* Readers must see the sample and its timestamp from the same update. */
    TX_INTERRUPT_SAVE_AREA
    TX_DISABLE
    latest = *raw;
    sample_tick = tx_time_get();
    available = 1;
    TX_RESTORE
}

int sensor_live_raw(logger_record *raw, uint32_t *age_ms)
{
    TX_INTERRUPT_SAVE_AREA
    TX_DISABLE
    if (!available)
    {
        TX_RESTORE
        return 0;
    }
    *raw = latest;
    ULONG age = tx_time_get() - sample_tick;
    TX_RESTORE
    uint64_t ms = (uint64_t)age * 1000 / TX_TIMER_TICKS_PER_SECOND;
    *age_ms = ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
    return 1;
}

int sensor_live_read(logger_record frame[2], uint32_t *age_ms)
{
    if (!sensor_live_raw(&frame[0], age_ms))
        return 0;
    sensor_processed processed;
    record_process(&frame[1], &frame[0], &sensors_metadata, reference_pa, &processed);
    /* A cached but recent sample is fine; never anchor to invalid/stalled data. */
    if (reference_pa == 0 && (processed.flags & 12) == 12 && processed.baro_age_us <= 200000 &&
        *age_ms <= 200)
    {
        reference_pa = processed.pressure_pa;
        record_process(&frame[1], &frame[0], &sensors_metadata, reference_pa, &processed);
    }
    return 1;
}
