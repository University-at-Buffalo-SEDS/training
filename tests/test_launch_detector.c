#include "launch_detector.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

int main(void)
{
    launch_detector d;
    launch_detector_reset(&d);
    sensor_processed s = {.flags = 0x11, .accel = {0, 0, 9.80665f}};
    for (unsigned i = 0; i < 200; ++i)
        assert(!launch_detector_update(&d, i * 10000, &s));
    s.accel[2] = 30;
    assert(!launch_detector_update(&d, 2000000, &s));
    s.accel[2] = 9.80665f;
    assert(!launch_detector_update(&d, 2010000, &s)); /* One-sample bump is rejected. */
    s.accel[2] = 30;
    assert(!launch_detector_update(&d, 2020000, &s));
    assert(!launch_detector_update(&d, 2030000, &s));
    assert(launch_detector_update(&d, 2040000, &s) == LAUNCH_ACCEL);
    for (unsigned i = 205; i < 220; ++i)
        assert(!launch_detector_update(&d, i * 10000, &s));
    s.accel[2] = 0;
    for (unsigned i = 220; i < 230; ++i)
        assert(!launch_detector_update(&d, i * 10000, &s));
    assert(launch_detector_update(&d, 2300000, &s) == LAUNCH_FREEFALL);
    for (unsigned i = 231; i < 250; ++i)
        assert(!launch_detector_update(&d, i * 10000, &s));
    s.flags = 0;
    s.accel[2] = 100;
    for (unsigned i = 250; i < 400; ++i)
        assert(!launch_detector_update(&d, i * 10000, &s));
    launch_detector_reset(&d);
    s = (sensor_processed){.flags = 0x1cc, .altitude_m = 0};
    for (unsigned i = 0; i < 25; ++i)
        assert(!launch_detector_update(&d, i * 40000, &s));
    s.altitude_m = 3.5f;
    assert(!launch_detector_update(&d, 1000000, &s));
    assert(!launch_detector_update(&d, 1040000, &s));
    assert(launch_detector_update(&d, 1080000, &s) == LAUNCH_BARO);
    for (unsigned i = 28; i < 60; ++i)
        assert(!launch_detector_update(&d, i * 40000, &s));
    launch_detector_reset(&d);
    s.baro_age_us = 300000;
    for (unsigned i = 0; i < 40; ++i)
    {
        s.altitude_m = (float)i;
        assert(!launch_detector_update(&d, i * 40000, &s));
    }
    s.flags = 0x11;
    s.accel[2] = NAN;
    for (unsigned i = 0; i < 20; ++i)
        assert(!launch_detector_update(&d, i * 10000, &s));
    puts("Launch/drop thresholds, debounce, invalid channels and stale barometer tests passed.");
    return 0;
}
