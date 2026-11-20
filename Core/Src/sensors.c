/* BMI088/BMP390 register setup adapted from ../flightcomputer26/Core/Src/
 * accelerometer.c, gyroscope.c and barometer.c. See third_party/flightcomputer26-LICENSE.
 * This board polls the devices and retains raw counts for offline reproduction. */
#include "sensors.h"
#include "spi_bus.h"
#include <string.h>

sensor_metadata sensors_metadata = {
    .version = 1, .accel_conf = 0xa8, .accel_range = 3,
    .gyro_bw = 4, .gyro_range = 0,
    /* BMP390 OSR: pressure bits 2:0, temperature bits 5:3. */
    .baro_osr = 3, .baro_odr = 3, .baro_iir = 4
};
static uint32_t available;
static sensor_raw cached;
static uint64_t baro_time;
static int have_baro;

static void delay_ms(unsigned ms)
{
    tx_thread_sleep(((ULONG)ms * TX_TIMER_TICKS_PER_SECOND + 999) / 1000);
}

static int read_regs(spi_device dev, uint8_t reg, uint8_t *out, uint16_t size)
{
    uint8_t header[2] = {reg | 0x80, 0};
    return spi_bus_read(dev, header, dev == SPI_GYRO ? 1 : 2, out, size) == HAL_OK;
}

static int write_reg(spi_device dev, uint8_t reg, uint8_t value)
{
    uint8_t bytes[] = {reg & 0x7f, value};
    return spi_bus_write(dev, bytes, sizeof(bytes)) == HAL_OK;
}

static int config_masked(spi_device dev, uint8_t reg, uint8_t value, unsigned delay,
                         uint8_t mask)
{
    uint8_t actual;
    if (!write_reg(dev, reg, value)) return 0;
    delay_ms(delay);
    return read_regs(dev, reg, &actual, 1) && (actual & mask) == (value & mask);
}

static int config(spi_device dev, uint8_t reg, uint8_t value, unsigned delay)
{
    return config_masked(dev, reg, value, delay, 0xff);
}

static int init_accel(void)
{
    uint8_t ignored;
    /* First access switches the accelerometer from I2C to SPI. Repeat after reset. */
    if (!read_regs(SPI_ACCEL, 0, &ignored, 1)) return 0;
    delay_ms(50);
    if (!write_reg(SPI_ACCEL, 0x7e, 0xb6)) return 0;
    delay_ms(50);
    if (!read_regs(SPI_ACCEL, 0, &ignored, 1)) return 0;
    delay_ms(1);
    if (!read_regs(SPI_ACCEL, 0, &sensors_metadata.accel_id, 1) ||
        sensors_metadata.accel_id != 0x1e) return 0;
    return config(SPI_ACCEL, 0x7c, 0, 50) && config(SPI_ACCEL, 0x7d, 4, 50) &&
           config(SPI_ACCEL, 0x40, sensors_metadata.accel_conf, 1) &&
           config(SPI_ACCEL, 0x41, sensors_metadata.accel_range, 1);
}

static int init_gyro(void)
{
    if (!read_regs(SPI_GYRO, 0, &sensors_metadata.gyro_id, 1) ||
        sensors_metadata.gyro_id != 0x0f || !write_reg(SPI_GYRO, 0x14, 0xb6)) return 0;
    delay_ms(50);
    return config(SPI_GYRO, 0x11, 0, 30) &&
           config(SPI_GYRO, 0x0f, sensors_metadata.gyro_range, 1) &&
           /* BMI088 datasheet 5.5.6: bit 7 is read-only, always 1. */
           config_masked(SPI_GYRO, 0x10, sensors_metadata.gyro_bw, 1, 0x7f);
}

static int init_baro(void)
{
    uint8_t status, errors;
    if (!write_reg(SPI_BARO, 0x7e, 0xb6)) return 0;
    delay_ms(50);
    if (!read_regs(SPI_BARO, 0, &sensors_metadata.baro_id, 1) ||
        sensors_metadata.baro_id != 0x60 ||
        !read_regs(SPI_BARO, 3, &status, 1) || !(status & 0x10)) return 0;
    if (!config(SPI_BARO, 0x1a, 0, 5) || !config(SPI_BARO, 0x1b, 3, 5) ||
        !config(SPI_BARO, 0x1c, sensors_metadata.baro_osr, 5) ||
        !config(SPI_BARO, 0x1d, sensors_metadata.baro_odr, 5) ||
        !config(SPI_BARO, 0x1f, sensors_metadata.baro_iir, 5) ||
        !read_regs(SPI_BARO, 0x31, sensors_metadata.trim, 21)) return 0;
    int all_zero = 1, all_ff = 1;
    for (unsigned i = 0; i < 21; ++i) {
        all_zero &= sensors_metadata.trim[i] == 0;
        all_ff &= sensors_metadata.trim[i] == 255;
    }
    if (all_zero || all_ff || !config(SPI_BARO, 0x1b, 0x33, 10)) return 0;
    return read_regs(SPI_BARO, 2, &errors, 1) && !(errors & 7);
}

uint32_t sensors_init(void)
{
    available = 0;
    memset(&cached, 0, sizeof(cached));
    have_baro = 0;
    if (init_accel()) available |= SENSOR_ACCEL;
    if (init_gyro()) available |= SENSOR_GYRO;
    if (init_baro()) available |= SENSOR_PRESSURE | SENSOR_TEMP;
    return available;
}

static uint32_t u24(const uint8_t *b)
{
    return b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16;
}

void sensors_read(sensor_raw *s, uint64_t now_us)
{
    uint8_t b[6], status;
    memset(s, 0, sizeof(*s));
    if ((available & SENSOR_ACCEL) && read_regs(SPI_ACCEL, 3, &status, 1) &&
        read_regs(SPI_ACCEL, 0x12, b, 6)) {
        for (unsigned i = 0; i < 3; ++i) s->accel[i] = (int16_t)(b[i*2] | b[i*2+1] << 8);
        if (status & 0x80) cached.flags |= SENSOR_ACCEL;
        s->flags |= cached.flags & SENSOR_ACCEL;
        if (status & 0x80) s->flags |= SENSOR_ACCEL << 4;
    } else s->errors |= SENSOR_READ_ERROR;
    if ((available & SENSOR_GYRO) && read_regs(SPI_GYRO, 2, b, 6)) {
        for (unsigned i = 0; i < 3; ++i) s->gyro[i] = (int16_t)(b[i*2] | b[i*2+1] << 8);
        /* 200 Hz gyro conversion, sampled at most every 10 ms; its DRDY
         * interrupt status is only a 280-400 us pulse, unsuitable for polling. */
        s->flags |= SENSOR_GYRO | SENSOR_GYRO << 4;
    } else s->errors |= SENSOR_READ_ERROR;
    int fresh = 0;
    if (!(available & SENSOR_PRESSURE) || !read_regs(SPI_BARO, 3, &status, 1))
        s->errors |= SENSOR_READ_ERROR;
    else if ((status & 0x60) == 0x60) {
        if (read_regs(SPI_BARO, 4, b, 6)) {
            cached.pressure_adc = u24(b);
            cached.temperature_adc = u24(b + 3);
            baro_time = now_us;
            have_baro = fresh = 1;
        } else s->errors |= SENSOR_READ_ERROR;
    }
    if (have_baro) {
        s->pressure_adc = cached.pressure_adc;
        s->temperature_adc = cached.temperature_adc;
        uint64_t age = now_us - baro_time;
        s->baro_age_us = age > UINT32_MAX ? UINT32_MAX : (uint32_t)age;
        s->flags |= SENSOR_PRESSURE | SENSOR_TEMP;
        if (fresh) s->flags |= (SENSOR_PRESSURE | SENSOR_TEMP) << 4;
    } else s->baro_age_us = UINT32_MAX;
    if (!fresh) s->errors |= SENSOR_STALE;
}
