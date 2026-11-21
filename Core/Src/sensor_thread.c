#include "T-Threads.h"
#include "acquisition.h"
#include "sensor_live.h"
#include <stdio.h>

TX_THREAD sensor_thread;
_Alignas(8) static ULONG sensor_stack[2048 / sizeof(ULONG)];

UINT create_sensor_thread(void)
{
    return tx_thread_create(&sensor_thread, "Sensor", sensor_thread_entry, 0,
                            sensor_stack, sizeof(sensor_stack), 5, 5,
                            TX_NO_TIME_SLICE, TX_AUTO_START);
}

VOID sensor_thread_entry(ULONG input)
{
    TX_PARAMETER_NOT_USED(input);
    _Static_assert(TX_TIMER_TICKS_PER_SECOND >= 100 && TX_TIMER_TICKS_PER_SECOND % 100 == 0,
                   "100 Hz acquisition requires an integral tick period");
    const ULONG period = TX_TIMER_TICKS_PER_SECOND / 100;
    acquisition_state.devices = sensors_init();
    acquisition_state.initialized = 1;
    printf("Sensors: mask=%lu accel=%02X gyro=%02X baro=%02X\n",
           (unsigned long)acquisition_state.devices, sensors_metadata.accel_id,
           sensors_metadata.gyro_id, sensors_metadata.baro_id);
    ULONG previous_tick = tx_time_get(), next = previous_tick;
    uint64_t elapsed_ticks = 0, origin = 0;
    uint32_t epoch = 0;
    uint32_t live_sequence = 0;
    for (;;) {
        ULONG tick = tx_time_get();
        elapsed_ticks += (ULONG)(tick - previous_tick);
        previous_tick = tick;
        {
            /* Priority 5 makes cutoff atomic relative to the lower-priority
             * storage worker. busy stays set across blocking SPI transfers. */
            acquisition_state.sensor_busy = 1;
            int logging = acquisition_state.running;
            if (logging && epoch != acquisition_epoch) {
                epoch = acquisition_epoch; origin = elapsed_ticks; next = tick;
            }
            sensor_raw raw;
            logger_record record;
            uint64_t uptime_us = elapsed_ticks * 1000000 / TX_TIMER_TICKS_PER_SECOND;
            uint64_t time_us = (elapsed_ticks - origin) * 1000000 / TX_TIMER_TICKS_PER_SECOND;
            sensors_read(&raw, uptime_us);
            record_raw(&record, ++live_sequence, uptime_us, &raw);
            sensor_live_publish(&record);
            if (logging) {
                record_raw(&record, ++acquisition_state.last_sequence, time_us, &raw);
                /* Independent deliveries, even if either queue is full. */
                if (tx_queue_send(&sensor_to_process, &record, TX_NO_WAIT) == TX_SUCCESS)
                    ++acquisition_state.accepted;
                else ++acquisition_state.processing_drops;
                acquisition_publish(&record, &acquisition_state.raw_log_drops);
            }
            acquisition_state.sensor_busy = 0;
            tx_event_flags_set(&acquisition_events, ACQUISITION_PROGRESS, TX_OR);
        }
        next += period;
        tick = tx_time_get();
        if ((LONG)(next - tick) <= 0) {
            ULONG skipped = (tick - next) / period + 1;
            if (acquisition_state.running) acquisition_state.missed_deadlines += skipped;
            next += skipped * period;
        }
        tx_thread_sleep(next - tick);
    }
}
