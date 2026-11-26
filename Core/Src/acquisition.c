#include "acquisition.h"
#include "spi_bus.h"
#include "T-Threads.h"
#include <math.h>

TX_QUEUE records_to_data, sensor_to_process;
TX_EVENT_FLAGS_GROUP acquisition_events;
static ULONG raw_memory[32 * 16], record_memory[256 * 16];
volatile acquisition_status acquisition_state;
float acquisition_reference_pa;
volatile uint32_t acquisition_epoch;

UINT acquisition_init(void)
{
    _Static_assert(sizeof(ULONG) == 4, "ThreadX queue word size");
    UINT status = spi_bus_init();
    if (status != TX_SUCCESS) return status;
    status = tx_event_flags_create(&acquisition_events, "Acquisition events");
    if (status != TX_SUCCESS) return status;
    status = tx_queue_create(&sensor_to_process, "Raw to processing", 16, raw_memory, sizeof(raw_memory));
    if (status != TX_SUCCESS) return status;
    status = tx_queue_create(&records_to_data, "Records to data", 16, record_memory, sizeof(record_memory));
    if (status != TX_SUCCESS) return status;
    status = create_processing_thread();
    if (status != TX_SUCCESS) return status;
    return create_sensor_thread();
}

void acquisition_get_status(acquisition_status *out)
{
    TX_INTERRUPT_SAVE_AREA
    TX_DISABLE
    *out = acquisition_state;
    TX_RESTORE
}

int acquisition_stopped(void)
{
    acquisition_status s;
    acquisition_get_status(&s);
    return !s.running && !s.sensor_busy && s.accepted == s.processed;
}

UINT acquisition_start(uint32_t session, float reference)
{
    if (!session || !isfinite(reference) || reference <= 0) return TX_OPTION_ERROR;
    if (!acquisition_stopped()) return TX_NOT_AVAILABLE;
    ULONG enqueued;
    UINT status = tx_queue_info_get(&records_to_data, NULL, &enqueued, NULL, NULL, NULL, NULL);
    if (status != TX_SUCCESS) return status;
    if (enqueued) return TX_NOT_AVAILABLE;
    TX_INTERRUPT_SAVE_AREA
    TX_DISABLE
    if (!acquisition_state.initialized || acquisition_state.devices != 15) {
        TX_RESTORE
        return TX_NOT_AVAILABLE;
    }
    acquisition_reference_pa = reference;
    acquisition_state.session = session;
    acquisition_state.last_sequence = 0;
    acquisition_state.accepted = acquisition_state.processed = 0;
    acquisition_state.processing_drops = acquisition_state.raw_log_drops = 0;
    acquisition_state.processed_log_drops = acquisition_state.missed_deadlines = 0;
    ++acquisition_epoch;
    acquisition_state.running = 1;
    TX_RESTORE
    return TX_SUCCESS;
}

void acquisition_stop(void)
{
    TX_INTERRUPT_SAVE_AREA
    TX_DISABLE
    acquisition_state.running = 0;
    TX_RESTORE
    tx_event_flags_set(&acquisition_events, ACQUISITION_PROGRESS, TX_OR);
}

void acquisition_publish(const logger_record *r, volatile uint32_t *drops)
{
    if (tx_queue_send(&records_to_data, (VOID *)r, TX_NO_WAIT) != TX_SUCCESS) ++*drops;
    else tx_event_flags_set(&acquisition_events, ACQUISITION_RECORD, TX_OR);
}
