#include "storage_model.h"
#include "usb_protocol.h"
#include <stdio.h>

static storage disk, recovered, checkpoint;
static sensor_metadata metadata = {.version = 1,
                                   .accel_id = 0x1e,
                                   .gyro_id = 0x0f,
                                   .baro_id = 0x60,
                                   .accel_conf = 0xa8,
                                   .accel_range = 3,
                                   .gyro_bw = 4,
                                   .baro_osr = 3,
                                   .baro_odr = 3,
                                   .baro_iir = 4};
static logger_record sample(unsigned seq)
{
    logger_record r;
    sensor_raw raw = {.flags = 255,
                      .accel = {-123, 456, 789},
                      .gyro = {-3, 2, 1},
                      .pressure_adc = 123456,
                      .temperature_adc = 654321};
    record_raw(&r, seq, (uint64_t)seq * 10000, &raw);
    return r;
}

static void virgin(void)
{
    memset(media, 255, sizeof(media));
    cut_after = -1;
    mutations = yields = 0;
    assert(storage_mount(&disk, &model_io) == STORE_OK && disk.next_id == 1 && yields);
}

static uint32_t begin(void)
{
    uint32_t id;
    assert(storage_start(&disk, &metadata, 100000, &id) == STORE_OK);
    return id;
}

static void snapshot(void)
{
    memcpy(saved_media, media, sizeof(media));
    checkpoint = disk;
}

static void restore(void)
{
    memcpy(media, saved_media, sizeof(media));
    disk = checkpoint;
    cut_after = -1;
}

int main(void)
{
    /* Never format unknown media, even with erased journals. */
    memset(media, 255, sizeof(media));
    media[1000000] = 0;
    mutations = 0;
    assert(storage_mount(&disk, &model_io) == STORE_CORRUPT && mutations == 0);
    uint8_t body[512];
    assert(storage_read(&disk, 1, 0, body, 80) == STORE_STATE);
    virgin();
    uint32_t id = begin();
    assert(id == 1);
    uint8_t expected[100 * 80];
    for (unsigned i = 0; i < 100; ++i)
    {
        logger_record r = sample(i + 1);
        assert(storage_append(&disk, &r) == STORE_OK);
        storage_session *p = storage_find(&disk, id);
        uint32_t address = p->first_sector * 4096 + 256 + (i / 3) * 256 + (i % 3) * 80;
        memcpy(expected + i * 80, media + address, 80);
        assert(!memcmp(expected + i * 80, "DLOG", 4) && model_u32(expected + i * 80 + 12) == i + 1);
        assert(model_u32(expected + i * 80 + 72) == usb_crc32(expected + i * 80, 72));
        assert(model_u32(expected + i * 80 + 76) == 0);
        assert(storage_latest(&disk, body) == STORE_OK && !memcmp(body, expected + i * 80, 80));
    }
    assert(storage_find(&disk, id)->sectors == 32 && storage_used_sectors(&disk) == 34);
    assert(storage_read(&disk, id, 0, body, 80) == STORE_STATE);
    assert(storage_close(&disk, 0) == STORE_OK);
    assert(storage_find(&disk, id)->sectors == 3 && storage_used_sectors(&disk) == 5);
    for (unsigned offset = 0; offset < sizeof(expected);)
    {
        size_t n = sizeof(expected) - offset;
        if (n > 512)
            n = 512;
        assert(storage_read(&disk, id, offset, body, n) == STORE_OK);
        assert(!memcmp(body, expected + offset, n));
        offset += (unsigned)n;
    }
    assert(storage_read(&disk, id, 137, body, 512) == STORE_OK &&
           !memcmp(body, expected + 137, 512));
    assert(storage_read(&disk, id, 8000, body, 1) == STORE_BOUNDS);
    assert(storage_read(&disk, id, UINT32_MAX, body, 512) == STORE_BOUNDS);
    assert(storage_read(&disk, 999, 0, body, 1) == STORE_NOT_FOUND);
    assert(storage_metadata(&disk, id, body) == STORE_OK && !memcmp(body, "DLH1", 4));
    assert(model_u32(body + 8) == id && model_u32(body + 248) == usb_crc32(body, 248));
    assert(storage_mount(&recovered, &model_io) == STORE_OK);
    assert(storage_find(&recovered, id)->records == 100 &&
           !storage_find(&recovered, id)->incomplete);
    assert(storage_read(&recovered, id, 733, body, 512) == STORE_OK &&
           !memcmp(body, expected + 733, 512));

    /* Every partial record write: only committed records survive; torn slots
     * remain quarantined across multiple subsequent boots. */
    virgin();
    id = begin();
    for (unsigned i = 0; i < 3; ++i)
    {
        logger_record r = sample(i);
        assert(!storage_append(&disk, &r));
    }
    snapshot();
    for (int cut = 0; cut <= 2; ++cut)
    {
        restore();
        cut_after = cut;
        logger_record r = sample(99);
        int result = storage_append(&disk, &r);
        cut_after = -1;
        assert(storage_mount(&recovered, &model_io) == STORE_OK);
        storage_session *p = storage_find(&recovered, id);
        assert(p && p->incomplete && p->records == (result ? 3U : 4U));
        assert(storage_mount(&disk, &model_io) == STORE_OK);
        assert(storage_find(&disk, id)->records == p->records);
        uint32_t next = begin();
        assert(next > id);
    }

    /* Close journal: inactive erase, sixteen body pages, separate commit. */
    for (int cut = 0; cut <= 18; ++cut)
    {
        restore();
        cut_after = cut;
        int result = storage_close(&disk, 0);
        cut_after = -1;
        assert(storage_mount(&recovered, &model_io) == STORE_OK);
        storage_session *p = storage_find(&recovered, id);
        assert(p && p->records == 3 && p->state == SESSION_CLOSED);
        assert(p->incomplete == (result != STORE_OK));
    }

    /* START allocation and header can each tear without reusing the ID/extent
     * or damaging an older session. Empty torn headers remain quarantined. */
    virgin();
    snapshot();
    for (int cut = 0; cut <= 20; ++cut)
    {
        restore();
        uint32_t unused;
        cut_after = cut;
        storage_start(&disk, &metadata, 100000, &unused);
        cut_after = -1;
        assert(storage_mount(&recovered, &model_io) == STORE_OK);
        assert(storage_mount(&disk, &model_io) == STORE_OK);
        storage_session *torn = storage_find(&disk, 1);
        if (torn && storage_metadata(&disk, 1, body) == STORE_CORRUPT)
            assert(!disk.fault);
        uint32_t next = begin();
        assert(next == recovered.next_id);
    }

    /* Two sessions; deletion interrupted at every mutation in the first step
     * resumes its recorded sector set and never touches the second session. */
    virgin();
    id = begin();
    for (unsigned i = 0; i < 100; ++i)
    {
        logger_record r = sample(i);
        assert(!storage_append(&disk, &r));
    }
    assert(!storage_close(&disk, 0));
    uint32_t other = begin();
    logger_record r = sample(777);
    assert(!storage_append(&disk, &r));
    assert(!storage_close(&disk, 0));
    assert(storage_read(&disk, other, 0, expected, 80) == STORE_OK);
    uint32_t operation;
    assert(storage_delete_begin(&disk, id, &operation) == STORE_OK);
    assert(storage_read(&disk, id, 0, body, 1) == STORE_STATE);
    snapshot();
    for (int cut = 0; cut <= 19; ++cut)
    {
        restore();
        cut_after = cut;
        storage_delete_step(&disk);
        cut_after = -1;
        assert(storage_mount(&recovered, &model_io) == STORE_OK);
        while (recovered.delete_id)
            assert(storage_delete_step(&recovered) == STORE_OK);
        assert(!storage_find(&recovered, id) && recovered.op_state == STORE_OP_DONE);
        assert(storage_read(&recovered, other, 0, body, 80) == STORE_OK &&
               !memcmp(body, expected, 80));
        assert(storage_mount(&disk, &model_io) == STORE_OK && disk.op_state == STORE_OP_DONE);
        assert(disk.last_op == operation && disk.next_id > other);
    }
    /* Also tear the final deletion step's replacement journal and commit. */
    restore();
    assert(!storage_delete_step(&disk));
    assert(!storage_delete_step(&disk));
    snapshot();
    for (int cut = 0; cut <= 19; ++cut)
    {
        restore();
        cut_after = cut;
        storage_delete_step(&disk);
        cut_after = -1;
        assert(storage_mount(&recovered, &model_io) == STORE_OK);
        while (recovered.delete_id)
            assert(!storage_delete_step(&recovered));
        assert(recovered.op_state == STORE_OP_DONE && !storage_find(&recovered, id));
        assert(storage_read(&recovered, other, 0, body, 80) == STORE_OK &&
               !memcmp(body, expected, 80));
    }

    /* Closed-record corruption is a fault, never accepted as a shorter log. */
    restore();
    storage_session *p = storage_find(&disk, other);
    media[p->first_sector * 4096 + 256 + 30] ^= 1;
    assert(storage_mount(&recovered, &model_io) == STORE_CORRUPT);

    /* Exhaust the physical device, preserving all earlier committed records. */
    virgin();
    id = begin();
    unsigned count = 0;
    for (;;)
    {
        r = sample(count);
        int result = storage_append(&disk, &r);
        if (result == STORE_FULL)
            break;
        assert(result == STORE_OK);
        ++count;
    }
    assert(count == (1022 * 16 - 1) * 3 && storage_used_sectors(&disk) == 1024);
    assert(storage_close(&disk, 1) == STORE_OK);
    assert(storage_mount(&recovered, &model_io) == STORE_OK &&
           storage_find(&recovered, id)->records == count);
    assert(storage_read(&recovered, id, (count - 1) * 80, body, 80) == STORE_OK);
    assert(model_u32(body + 12) == count - 1);
    puts("Storage persistence, recovery, export, deletion and full-device tests passed.");
    return 0;
}
