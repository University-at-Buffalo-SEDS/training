#pragma once
#include "records.h"

enum
{
    LAUNCH_ACCEL = 1,
    LAUNCH_FREEFALL = 2,
    LAUNCH_BARO = 4
};
#define LAUNCH_PRE_US UINT64_C(20000000)
#define LAUNCH_POST_US UINT64_C(300000000)
#define LAUNCH_ACCEL_G 2.5f
#define LAUNCH_FREEFALL_G 0.35f
#define LAUNCH_BARO_SHIFT_M 3.0f
typedef struct
{
    uint64_t last_us, spike_us, freefall_us, last_baro_us;
    uint32_t have_sample, spike_active, freefall_active, baro_count, baro_next, baro_hits;
    uint32_t spike_reported, freefall_reported, baro_reported;
    struct
    {
        uint64_t time_us;
        float altitude;
    } baro[32];
} launch_detector;
void launch_detector_reset(launch_detector *detector);
unsigned
launch_detector_update(launch_detector *detector, uint64_t time_us, const sensor_processed *sample);
