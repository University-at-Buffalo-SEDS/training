#pragma once
#include "sensors.h"
#include <stddef.h>

/* Queue envelope is already encoded little-endian, including IEEE binary32.
 * Storage adds session ID/CRC/commit; never writes a native struct to flash. */
typedef struct { uint8_t bytes[64]; } logger_record;
_Static_assert(sizeof(logger_record) == 64, "record envelope");
typedef struct {
    uint32_t flags;
    float accel[3], gyro[3], pressure_pa, temperature_c, altitude_m;
    uint32_t baro_age_us, errors;
} sensor_processed;
_Static_assert(sizeof(sensor_processed) == 48, "processed payload");
void record_raw(logger_record *out, uint32_t sequence, uint64_t time_us, const sensor_raw *raw);
void record_process(logger_record *out, const logger_record *raw,
                    const sensor_metadata *metadata, float reference_pa,
                    sensor_processed *processed);
