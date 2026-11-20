/* Compensation polynomial adapted from flightcomputer26 barometer.c.
 * See third_party/flightcomputer26-LICENSE. */
#include "records.h"
#include <math.h>
#include <string.h>

static uint16_t u16(const uint8_t *b) { return b[0] | (uint16_t)b[1] << 8; }
static uint32_t u32(const uint8_t *b)
{ return u16(b) | (uint32_t)u16(b + 2) << 16; }
static void put16(uint8_t *b, uint16_t v) { b[0] = v; b[1] = v >> 8; }
static void put32(uint8_t *b, uint32_t v)
{ put16(b, v); put16(b + 2, v >> 16); }
static void putfloat(uint8_t *b, float v)
{
    uint32_t bits;
    _Static_assert(sizeof(float) == 4, "IEEE binary32 required");
    memcpy(&bits, &v, 4); put32(b, bits);
}

int bmp390_compensate(const uint8_t c[21], uint32_t adc_p, uint32_t adc_t,
                     float *pressure, float *temperature)
{
    /* Use double intermediates: pressure contains cancellation between terms.
     * T2 is UNSIGNED in the BMP390 trim table. */
    double t1 = ldexp((double)u16(c), 8);
    double t2 = ldexp((double)u16(c + 2), -30);
    double t3 = ldexp((double)(int8_t)c[4], -48);
    double p1 = ldexp((double)(int16_t)u16(c + 5) - 16384, -20);
    double p2 = ldexp((double)(int16_t)u16(c + 7) - 16384, -29);
    double p3 = ldexp((double)(int8_t)c[9], -32);
    double p4 = ldexp((double)(int8_t)c[10], -37);
    double p5 = ldexp((double)u16(c + 11), 3);
    double p6 = ldexp((double)u16(c + 13), -6);
    double p7 = ldexp((double)(int8_t)c[15], -8);
    double p8 = ldexp((double)(int8_t)c[16], -15);
    double p9 = ldexp((double)(int16_t)u16(c + 17), -48);
    double p10 = ldexp((double)(int8_t)c[19], -48);
    double p11 = ldexp((double)(int8_t)c[20], -65);
    double dt = adc_t - t1;
    double t = dt * t2 + dt * dt * t3;
    double p = adc_p;
    double result = p5 + p6*t + p7*t*t + p8*t*t*t +
        p*(p1 + p2*t + p3*t*t + p4*t*t*t) + p*p*(p9 + p10*t) + p*p*p*p11;
    *pressure = (float)result; *temperature = (float)t;
    return adc_p <= 0xffffff && adc_t <= 0xffffff && isfinite(result) && isfinite(t);
}

void record_raw(logger_record *out, uint32_t sequence, uint64_t time, const sensor_raw *s)
{
    memset(out, 0, sizeof(*out));
    uint8_t *b = out->bytes;
    b[0] = 1; b[1] = 1; put16(b + 2, 32); put32(b + 4, sequence);
    put32(b + 8, (uint32_t)time); put32(b + 12, (uint32_t)(time >> 32));
    b += 16;
    put32(b, s->flags);
    for (unsigned i = 0; i < 3; ++i) {
        put16(b + 4 + 2*i, (uint16_t)s->accel[i]);
        put16(b + 10 + 2*i, (uint16_t)s->gyro[i]);
    }
    put32(b + 16, s->pressure_adc); put32(b + 20, s->temperature_adc);
    put32(b + 24, s->baro_age_us); put32(b + 28, s->errors);
}

void record_process(logger_record *out, const logger_record *raw,
                    const sensor_metadata *m, float reference, sensor_processed *p)
{
    const uint8_t *b = raw->bytes + 16;
    memset(p, 0, sizeof(*p));
    p->flags = u32(b) & 0xff; p->errors = u32(b + 28); p->baro_age_us = u32(b + 24);
    if (raw->bytes[0] != 1 || raw->bytes[1] != 1 || u16(raw->bytes + 2) != 32) {
        p->flags = 0; p->errors |= SENSOR_COMPENSATION_ERROR;
    }
    if (m->accel_range > 3) { p->flags &= ~0x11u; p->errors |= SENSOR_RANGE_ERROR; }
    if (m->gyro_range > 4) { p->flags &= ~0x22u; p->errors |= SENSOR_RANGE_ERROR; }
    for (unsigned i = 0; i < 3; ++i) {
        if (p->flags & SENSOR_ACCEL)
            p->accel[i] = (int16_t)u16(b + 4 + 2*i) *
                (3.0f * (1u << m->accel_range) * 9.80665f / 32768.0f);
        if (p->flags & SENSOR_GYRO)
            p->gyro[i] = (int16_t)u16(b + 10 + 2*i) *
                (2000.0f / (1u << m->gyro_range) / 32768.0f * 0.01745329252f);
    }
    if ((p->flags & 12) == 12) {
        float pressure, temperature;
        if (!bmp390_compensate(m->trim, u32(b + 16), u32(b + 20), &pressure, &temperature)) {
            p->flags &= ~0xccu; p->errors |= SENSOR_COMPENSATION_ERROR;
        } else {
            if (temperature >= -40 && temperature <= 85) p->temperature_c = temperature;
            else { p->flags &= ~0x88u; p->errors |= SENSOR_RANGE_ERROR; }
            if (pressure >= 30000 && pressure <= 125000) p->pressure_pa = pressure;
            else { p->flags &= ~0x44u; p->errors |= SENSOR_RANGE_ERROR; }
            if ((p->flags & 12) == 12 && isfinite(reference) && reference > 0) {
                float altitude = 44330.0f * (1 - powf(pressure / reference, 0.1903f));
                if (isfinite(altitude)) { p->altitude_m = altitude; p->flags |= SENSOR_ALTITUDE; }
                else p->errors |= SENSOR_COMPENSATION_ERROR;
            }
        }
    }
    memset(out, 0, sizeof(*out)); memcpy(out->bytes, raw->bytes, 16);
    out->bytes[0] = 2; put16(out->bytes + 2, 48);
    uint8_t *d = out->bytes + 16; put32(d, p->flags);
    for (unsigned i = 0; i < 3; ++i) {
        putfloat(d + 4 + 4*i, p->accel[i]); putfloat(d + 16 + 4*i, p->gyro[i]);
    }
    putfloat(d + 28, p->pressure_pa); putfloat(d + 32, p->temperature_c);
    putfloat(d + 36, p->altitude_m); put32(d + 40, p->baro_age_us); put32(d + 44, p->errors);
}
