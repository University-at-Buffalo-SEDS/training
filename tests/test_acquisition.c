#include "acquisition.h"
#include "spi_bus.h"
#include "sensor_live.h"
#include "T-Threads.h"
#include <assert.h>
#include <math.h>
#include <string.h>
#include <setjmp.h>

SPI_HandleTypeDef hspi1;
static uint8_t regs[4][128], selected_reg;
static int selected = -1, locked, fail_rx, fail_lock, fail_tx;
static int corrupt_gyro_bw;
static ULONG ticks;
static int worker_mode;
static jmp_buf worker_finished;
static void put16(uint8_t *b, uint16_t v) { b[0] = v; b[1] = v >> 8; }
static uint32_t get32(const uint8_t *b)
{ return b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }

UINT tx_mutex_create(TX_MUTEX *m, char *name, UINT inheritance)
{ (void)m; (void)name; assert(inheritance == TX_INHERIT); return TX_SUCCESS; }
UINT tx_mutex_get(TX_MUTEX *m, ULONG wait)
{
    (void)m; assert(wait && wait != TX_WAIT_FOREVER);
    if (fail_lock) return TX_NOT_AVAILABLE;
    assert(!locked); locked = 1; return TX_SUCCESS;
}
UINT tx_mutex_put(TX_MUTEX *m)
{ (void)m; assert(locked && selected == -1); locked = 0; return TX_SUCCESS; }
void HAL_GPIO_WritePin(void *port, uint16_t pins, int level)
{
    assert(port == GPIOB && locked);
    if (level == GPIO_PIN_SET) selected = -1;
    else {
        assert(selected == -1);
        for (int i = 0; i < 4; ++i) if (pins == (1u << (11 + i))) selected = i;
        assert(selected >= 0);
    }
}
HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *spi, uint8_t *b, uint16_t n, uint32_t wait)
{
    assert(spi == &hspi1 && locked && selected >= 0 && wait == 10);
    if (fail_tx) return HAL_ERROR;
    selected_reg = b[0] & 127;
    if (b[0] & 128) {
        assert(n == (selected == SPI_GYRO ? 1 : 2)); /* Includes SPI dummy byte. */
    } else {
        assert(n == 2);
        if (selected_reg != 0x7e && selected_reg != 0x14) regs[selected][selected_reg] = b[1];
    }
    return HAL_OK;
}
HAL_StatusTypeDef HAL_SPI_Receive(SPI_HandleTypeDef *spi, uint8_t *b, uint16_t n, uint32_t wait)
{
    assert(spi == &hspi1 && locked && selected >= 0 && wait == 10);
    if (fail_rx) return HAL_ERROR;
    assert(selected_reg + n <= 128);
    memcpy(b, &regs[selected][selected_reg], n);
    if (selected == SPI_GYRO && selected_reg == 0x10) {
        b[0] |= 0x80; /* Real silicon always reads this read-only bit as 1. */
        if (corrupt_gyro_bw) b[0] ^= 1;
    }
    return HAL_OK;
}
UINT tx_thread_sleep(ULONG count)
{
    assert(!locked); ticks += count;
    if (worker_mode == 1 && acquisition_state.initialized) longjmp(worker_finished, 1);
    return TX_SUCCESS;
}
ULONG tx_time_get(void) { return ticks; }
UINT tx_thread_create(TX_THREAD *t, char *name, VOID (*entry)(ULONG), ULONG arg,
                      VOID *stack, ULONG size, UINT priority, UINT threshold, ULONG slice, UINT start)
{
    (void)t; (void)name; (void)entry; (void)arg; (void)stack;
    assert(size >= 2048 && priority == threshold && slice == TX_NO_TIME_SLICE && start == TX_AUTO_START);
    return TX_SUCCESS;
}
UINT tx_event_flags_create(TX_EVENT_FLAGS_GROUP *g, char *name)
{ (void)name; g->flags = 0; return TX_SUCCESS; }
UINT tx_event_flags_set(TX_EVENT_FLAGS_GROUP *g, ULONG flags, UINT option)
{ assert(option == TX_OR); g->flags |= flags; return TX_SUCCESS; }
UINT tx_queue_create(TX_QUEUE *q, char *name, UINT words, VOID *memory, ULONG bytes)
{
    (void)name; assert(words == 16);
    *q = (TX_QUEUE){.words = words, .depth = bytes / (words * 4), .storage = memory};
    return TX_SUCCESS;
}
UINT tx_queue_send(TX_QUEUE *q, VOID *p, ULONG wait)
{
    assert(wait == TX_NO_WAIT);
    if (q->used == q->depth) return TX_QUEUE_FULL;
    memcpy(q->storage + q->tail * q->words, p, q->words * 4);
    q->tail = (q->tail + 1) % q->depth; ++q->used; return TX_SUCCESS;
}
UINT tx_queue_receive(TX_QUEUE *q, VOID *p, ULONG wait)
{
    (void)wait;
    if (!q->used) {
        if (worker_mode == 2) longjmp(worker_finished, 1);
        return TX_QUEUE_EMPTY;
    }
    memcpy(p, q->storage + q->head * q->words, q->words * 4);
    q->head = (q->head + 1) % q->depth; --q->used; return TX_SUCCESS;
}
UINT tx_queue_info_get(TX_QUEUE *q, char **n, ULONG *used, ULONG *free_slots,
                       TX_THREAD **first, ULONG *suspended, TX_QUEUE **next)
{
    (void)n; (void)free_slots; (void)first; (void)suspended; (void)next;
    *used = q->used; return TX_SUCCESS;
}

int main(void)
{
    assert(acquisition_init() == TX_SUCCESS);
    logger_record live[2];
    uint32_t age;
    assert(!sensor_live_read(live, &age));
    assert(acquisition_start(1, 101325) == TX_NOT_AVAILABLE);
    regs[SPI_ACCEL][0] = 0x1e; regs[SPI_GYRO][0] = 0x0f; regs[SPI_BARO][0] = 0x60;
    regs[SPI_BARO][3] = 0x10;
    uint8_t *trim = &regs[SPI_BARO][0x31];
    /* Analytically known trim vector: P1/P2 cancel their offset, P5 alone
     * produces 100000 Pa; unsigned T2=32768 and ADC_T=819200 give 25 C. */
    put16(trim + 2, 32768); put16(trim + 5, 16384); put16(trim + 7, 16384);
    put16(trim + 11, 12500);
    assert(sensors_init() == 15);
    assert(regs[SPI_BARO][0x1c] == 3 && regs[SPI_BARO][0x1d] == 3 && regs[SPI_BARO][0x1f] == 4);
    regs[SPI_ACCEL][3] = 128;
    put16(&regs[SPI_ACCEL][0x12], (uint16_t)-32768);
    put16(&regs[SPI_GYRO][2], 16384);
    regs[SPI_BARO][3] = 0x70;
    regs[SPI_BARO][4] = 1;
    regs[SPI_BARO][7] = 0; regs[SPI_BARO][8] = 0x80; regs[SPI_BARO][9] = 0x0c;
    sensor_raw raw;
    sensors_read(&raw, 1000000);
    assert(raw.flags == 255 && raw.accel[0] == -32768 && raw.gyro[0] == 16384);
    assert(raw.temperature_adc == 819200 && raw.baro_age_us == 0 && raw.errors == 0);
    logger_record encoded, processed;
    sensor_processed result;
    record_raw(&encoded, 0x12345678, UINT64_C(0x100000002), &raw);
    assert(encoded.bytes[0] == 1 && encoded.bytes[2] == 32);
    assert(get32(encoded.bytes + 4) == 0x12345678 && get32(encoded.bytes + 12) == 1);
    for (unsigned i = 48; i < 64; ++i) assert(encoded.bytes[i] == 0);
    record_process(&processed, &encoded, &sensors_metadata, 100000, &result);
    assert(result.flags == 511 && result.errors == 0);
    assert(fabsf(result.accel[0] + 24 * 9.80665f) < 0.001f);
    assert(fabsf(result.gyro[0] - 17.45329252f) < 0.001f);
    assert(result.pressure_pa == 100000 && result.temperature_c == 25 && result.altitude_m == 0);
    assert(processed.bytes[0] == 2 && processed.bytes[2] == 48);
    assert(!memcmp(processed.bytes + 4, encoded.bytes + 4, 12));
    assert(get32(processed.bytes + 44) == 0x47c35000); /* binary32 100000 */
    sensor_live_publish(&encoded);
    ticks += 2;
    assert(sensor_live_read(live, &age) && age == 20);
    assert(!memcmp(&live[0], &encoded, sizeof(encoded)));
    assert(get32(live[1].bytes + 16) == 511);
    assert(get32(live[1].bytes + 52) == 0); /* First valid live sample is 0 m. */
    /* Inject a lower pressure using the analytic P5-only calibration fixture. */
    put16(sensors_metadata.trim + 11, 12400); /* 99200 Pa */
    assert(sensor_live_read(live, &age));
    float altitude;
    uint32_t bits = get32(live[1].bytes + 52);
    memcpy(&altitude, &bits, sizeof(altitude));
    assert(fabsf(altitude - 44330.0f * (1 - powf(0.992f, 0.1903f))) < 0.01f);
    sensor_live_zero();
    assert(sensor_live_read(live, &age) && get32(live[1].bytes + 52) == 0);
    put16(sensors_metadata.trim + 11, 12500);
    sensor_live_zero();
    sensor_raw old = raw; old.baro_age_us = 300000;
    logger_record old_record;
    record_raw(&old_record, 43, 2000000, &old);
    sensor_live_publish(&old_record);
    assert(sensor_live_read(live, &age) && !(get32(live[1].bytes + 16) & 256));
    sensor_live_publish(&encoded);
    assert(sensor_live_read(live, &age) && get32(live[1].bytes + 52) == 0);
    assert(records_to_data.used == 0); /* Live reads never consume flash queues. */

    regs[SPI_BARO][3] = 0x10; regs[SPI_ACCEL][3] = 0;
    sensors_read(&raw, 1040000);
    assert(raw.flags == (15 | 32) && raw.baro_age_us == 40000 && raw.errors == SENSOR_STALE);
    fail_rx = 1; sensors_read(&raw, 1050000); fail_rx = 0;
    assert(raw.flags == 12 && (raw.errors & SENSOR_READ_ERROR) && raw.baro_age_us == 50000);
    assert(!locked && selected == -1);
    uint8_t bytes[2] = {0x80, 0}, data;
    fail_tx = 1;
    assert(spi_bus_read(SPI_ACCEL, bytes, 2, &data, 1) == HAL_ERROR);
    fail_tx = 0; assert(!locked && selected == -1);
    fail_lock = 1;
    assert(spi_bus_read(SPI_ACCEL, bytes, 2, &data, 1) == HAL_TIMEOUT);
    fail_lock = 0; assert(!locked && selected == -1);
    assert(spi_bus_write((spi_device)99, bytes, 2) == HAL_ERROR);

    /* Bad compensation clears validity, leaves finite zero outputs. */
    sensor_metadata bad = sensors_metadata;
    memset(bad.trim, 0, sizeof(bad.trim));
    record_process(&processed, &encoded, &bad, 100000, &result);
    assert(!(result.flags & SENSOR_PRESSURE) && result.pressure_pa == 0 &&
           (result.errors & SENSOR_RANGE_ERROR));
    bad.accel_range = 255; bad.gyro_range = 255;
    record_process(&processed, &encoded, &bad, NAN, &result);
    assert(!(result.flags & (SENSOR_ACCEL | SENSOR_GYRO | SENSOR_ALTITUDE)));

    acquisition_state.initialized = 1; acquisition_state.devices = 15;
    assert(acquisition_start(0, 100000) == TX_OPTION_ERROR);
    assert(acquisition_start(1, NAN) == TX_OPTION_ERROR);
    assert(acquisition_start(1, 100000) == TX_SUCCESS);
    assert(!acquisition_stopped() && acquisition_start(2, 100000) == TX_NOT_AVAILABLE);
    acquisition_state.sensor_busy = 1;
    acquisition_stop(); assert(!acquisition_stopped());
    acquisition_state.sensor_busy = 0; acquisition_state.accepted = 2;
    acquisition_state.processed = 1; assert(!acquisition_stopped());
    acquisition_state.processed = 2; assert(acquisition_stopped());
    for (unsigned i = 0; i < 257; ++i)
        acquisition_publish(&encoded, &acquisition_state.raw_log_drops);
    assert(records_to_data.used == 256 && acquisition_state.raw_log_drops == 1);
    assert(acquisition_events.flags & ACQUISITION_RECORD);
    assert(acquisition_start(2, 100000) == TX_NOT_AVAILABLE); /* Must drain old session. */
    for (unsigned i = 0; i < 256; ++i)
        assert(tx_queue_receive(&records_to_data, &processed, TX_NO_WAIT) == TX_SUCCESS);
    assert(acquisition_start(2, 100000) == TX_SUCCESS && acquisition_state.session == 2);
    assert(acquisition_state.accepted == 0 && acquisition_state.raw_log_drops == 0);
    acquisition_stop(); acquisition_state.devices = 3;
    assert(acquisition_start(3, 100000) == TX_NOT_AVAILABLE);

    /* Run real worker bodies through one acquisition and queue-drain cycle. */
    acquisition_state = (acquisition_status){0};
    worker_mode = 1;
    if (!setjmp(worker_finished)) sensor_thread_entry(0);
    worker_mode = 0;
    assert(sensor_live_read(live, &age));
    assert(records_to_data.used == 0 && sensor_to_process.used == 0); /* Idle live sampling. */
    assert(acquisition_start(4, 100000) == TX_SUCCESS);
    acquisition_state.initialized = 0;
    worker_mode = 1;
    if (!setjmp(worker_finished)) sensor_thread_entry(0);
    worker_mode = 0;
    assert(records_to_data.used == 1 && sensor_to_process.used == 1 && acquisition_state.accepted == 1);
    acquisition_stop(); assert(!acquisition_stopped());
    worker_mode = 2;
    if (!setjmp(worker_finished)) processing_thread_entry(0);
    worker_mode = 0;
    assert(acquisition_stopped() && records_to_data.used == 2 && acquisition_state.processed == 1);
    assert(tx_queue_receive(&records_to_data, &encoded, TX_NO_WAIT) == TX_SUCCESS);
    assert(tx_queue_receive(&records_to_data, &processed, TX_NO_WAIT) == TX_SUCCESS);
    assert(encoded.bytes[0] == 1 && processed.bytes[0] == 2 &&
           !memcmp(encoded.bytes + 4, processed.bytes + 4, 12));
    assert(acquisition_start(5, 100000) == TX_SUCCESS);
    for (unsigned i = 0; i < 256; ++i)
        acquisition_publish(&encoded, &acquisition_state.raw_log_drops);
    acquisition_state.initialized = 0; worker_mode = 1;
    if (!setjmp(worker_finished)) sensor_thread_entry(0);
    worker_mode = 0;
    assert(acquisition_state.raw_log_drops == 1 && acquisition_state.accepted == 1 &&
           acquisition_state.processing_drops == 0 && sensor_to_process.used == 1);
    acquisition_stop(); worker_mode = 2;
    if (!setjmp(worker_finished)) processing_thread_entry(0);
    worker_mode = 0;
    assert(acquisition_state.processed_log_drops == 1 && acquisition_stopped());
    while (tx_queue_receive(&records_to_data, &processed, TX_NO_WAIT) == TX_SUCCESS) {}
    assert(acquisition_start(6, 100000) == TX_SUCCESS);
    for (unsigned i = 0; i < 32; ++i)
        assert(tx_queue_send(&sensor_to_process, &encoded, TX_NO_WAIT) == TX_SUCCESS);
    acquisition_state.accepted = 32;
    acquisition_state.initialized = 0; worker_mode = 1;
    if (!setjmp(worker_finished)) sensor_thread_entry(0);
    worker_mode = 0;
    assert(acquisition_state.processing_drops == 1 && acquisition_state.raw_log_drops == 0 &&
           records_to_data.used == 1); /* Raw logging survives a full processing queue. */
    acquisition_stop(); worker_mode = 2;
    if (!setjmp(worker_finished)) processing_thread_entry(0);
    worker_mode = 0;
    assert(acquisition_stopped() && acquisition_state.processed == 32);
    regs[SPI_ACCEL][0] = 0;
    assert(!(sensors_init() & SENSOR_ACCEL));
    regs[SPI_ACCEL][0] = 0x1e;
    corrupt_gyro_bw = 1;
    assert(sensors_init() == 13); /* Actual writable-bit mismatch still fails. */
    corrupt_gyro_bw = 0;
    assert(sensors_init() == 15);
    return 0;
}
