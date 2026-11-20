#pragma once
#include "records.h"

/* Latest-sample view only: polling may skip 100 Hz samples. Never consumes the
 * storage queues and stays available while storage is absent, busy or full. */
void sensor_live_publish(const logger_record *raw);
int sensor_live_read(logger_record frame[2], uint32_t *age_ms);
/* USB adapter owner only; begin a new relative-altitude reference. */
void sensor_live_zero(void);
