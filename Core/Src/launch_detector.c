#include "launch_detector.h"
#include <math.h>
#include <string.h>

void launch_detector_reset(launch_detector *d)
{
    memset(d, 0, sizeof(*d));
}

unsigned launch_detector_update(launch_detector *d, uint64_t time, const sensor_processed *p)
{
    /* A long gap or a clock reset invalidates the debounce history. */
    if (d->have_sample && (time < d->last_us || time - d->last_us > 200000))
        launch_detector_reset(d);
    d->last_us = time;
    d->have_sample = 1;
    unsigned reason = 0;
    /* Require both valid and fresh acceleration. Compare squared magnitudes
     * so spike and freefall detection do not need a square root. */
    int accel_ok = (p->flags & 0x11) == 0x11;
    float a2 = p->accel[0] * p->accel[0] + p->accel[1] * p->accel[1] + p->accel[2] * p->accel[2];
    float spike = LAUNCH_ACCEL_G * 9.80665f, freefall = LAUNCH_FREEFALL_G * 9.80665f;
    if (accel_ok && isfinite(a2) && a2 >= spike * spike)
    {
        if (!d->spike_active)
        {
            d->spike_us = time;
            d->spike_active = 1;
        }
        if (time - d->spike_us >= 20000 && !d->spike_reported)
        {
            reason |= LAUNCH_ACCEL;
            d->spike_reported = 1;
        }
    }
    else
        d->spike_active = d->spike_reported = 0;
    if (accel_ok && isfinite(a2) && a2 <= freefall * freefall)
    {
        if (!d->freefall_active)
        {
            d->freefall_us = time;
            d->freefall_active = 1;
        }
        if (time - d->freefall_us >= 100000 && !d->freefall_reported)
        {
            reason |= LAUNCH_FREEFALL;
            d->freefall_reported = 1;
        }
    }
    else
        d->freefall_active = d->freefall_reported = 0;
    /* Consider only fresh altitude conversions. Compare to the oldest
     * reading still in the one-second window and debounce the shift. */
    if ((p->flags & 0x1cc) == 0x1cc && p->baro_age_us <= 200000 && isfinite(p->altitude_m))
    {
        if (d->last_baro_us && time - d->last_baro_us > 200000)
            d->baro_count = d->baro_hits = 0;
        d->last_baro_us = time;
        float reference = p->altitude_m;
        uint64_t oldest = time;
        for (unsigned i = 0; i < d->baro_count; ++i)
        {
            uint64_t t = d->baro[i].time_us;
            if (t <= time && time - t <= 1000000 && t < oldest)
            {
                oldest = t;
                reference = d->baro[i].altitude;
            }
        }
        if (time - oldest >= 200000 && fabsf(p->altitude_m - reference) >= LAUNCH_BARO_SHIFT_M)
        {
            if (++d->baro_hits >= 3 && !d->baro_reported)
            {
                reason |= LAUNCH_BARO;
                d->baro_reported = 1;
            }
        }
        else
            d->baro_hits = d->baro_reported = 0;
        d->baro[d->baro_next].time_us = time;
        d->baro[d->baro_next].altitude = p->altitude_m;
        d->baro_next = (d->baro_next + 1) % 32;
        if (d->baro_count < 32)
            ++d->baro_count;
    }
    return reason;
}
