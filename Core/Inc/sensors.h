#pragma once
#include <stdint.h>

enum {
    SENSOR_ACCEL = 1, SENSOR_GYRO = 2, SENSOR_PRESSURE = 4, SENSOR_TEMP = 8,
    SENSOR_ALTITUDE = 256,
    SENSOR_READ_ERROR = 1, SENSOR_STALE = 2, SENSOR_COMPENSATION_ERROR = 4,
    SENSOR_RANGE_ERROR = 8
};
typedef struct {
    uint32_t flags;
    int16_t accel[3], gyro[3];
    uint32_t pressure_adc, temperature_adc, baro_age_us, errors;
} sensor_raw;
_Static_assert(sizeof(sensor_raw) == 32, "raw payload");

/* Immutable after sensors_init. Save these in versioned session metadata.
 * Sensor axes, no mounting transform/bias applied. */
typedef struct {
    uint32_t version;
    uint8_t accel_id, gyro_id, baro_id;
    uint8_t accel_conf, accel_range, gyro_bw, gyro_range;
    uint8_t baro_osr, baro_odr, baro_iir;
    uint8_t trim[21];
} sensor_metadata;
extern sensor_metadata sensors_metadata;
uint32_t sensors_init(void); /* valid device mask; called once by sensor task */
void sensors_read(sensor_raw *sample, uint64_t now_us);
/* Pure conversion, safe in processing task; no SPI or global compensation state. */
int bmp390_compensate(const uint8_t trim[21], uint32_t pressure_adc,
                     uint32_t temperature_adc, float *pressure_pa, float *temperature_c);
