#pragma once
#include "records.h"
#include "tx_api.h"

enum { ACQUISITION_RECORD = 1, ACQUISITION_PROGRESS = 2 };
extern TX_QUEUE records_to_data;
extern TX_EVENT_FLAGS_GROUP acquisition_events;
typedef struct {
    uint32_t initialized, devices, session, running, sensor_busy;
    uint32_t last_sequence, accepted, processed;
    uint32_t processing_drops, raw_log_drops, processed_log_drops, missed_deadlines;
} acquisition_status;
UINT acquisition_init(void);
/* Storage task is the sole caller of start/stop. Call start only after flash
 * recovery and metadata persistence. All sensors required. No implicit START. */
UINT acquisition_start(uint32_t session, float reference_pressure_pa);
void acquisition_stop(void);
/* True only after sensor cutoff and all accepted processing inputs finish.
 * Storage must ALSO drain records_to_data and finish writes before closing. */
int acquisition_stopped(void);
void acquisition_get_status(acquisition_status *out);

/* Internal worker boundary; not storage APIs. */
extern TX_QUEUE sensor_to_process;
extern volatile acquisition_status acquisition_state;
extern float acquisition_reference_pa;
extern volatile uint32_t acquisition_epoch;
void acquisition_publish(const logger_record *record, volatile uint32_t *drops);
