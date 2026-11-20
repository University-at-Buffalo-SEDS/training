#include "T-Threads.h"
#include "acquisition.h"

TX_THREAD processing_thread;
_Alignas(8) static ULONG processing_stack[4096 / sizeof(ULONG)];

UINT create_processing_thread(void)
{
    return tx_thread_create(&processing_thread, "Processing", processing_thread_entry, 0,
                            processing_stack, sizeof(processing_stack), 10, 10,
                            TX_NO_TIME_SLICE, TX_AUTO_START);
}

VOID processing_thread_entry(ULONG input)
{
    TX_PARAMETER_NOT_USED(input);
    logger_record raw, processed;
    sensor_processed values;
    for (;;) {
        if (tx_queue_receive(&sensor_to_process, &raw, TX_WAIT_FOREVER) != TX_SUCCESS) continue;
        record_process(&processed, &raw, &sensors_metadata, acquisition_reference_pa, &values);
        acquisition_publish(&processed, &acquisition_state.processed_log_drops);
        ++acquisition_state.processed;
        tx_event_flags_set(&acquisition_events, ACQUISITION_PROGRESS, TX_OR);
    }
}
