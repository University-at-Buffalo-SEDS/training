#pragma once
#include "storage.h"
#include <assert.h>
#include <string.h>

/* NOR model, never connected to hardware. Inject torn writes/erases at every
 * persistent mutation boundary, including commit words and journal replacement. */
static uint8_t media[STORAGE_SECTORS * 4096], saved_media[STORAGE_SECTORS * 4096];
static int cut_after = -1;
static unsigned mutations, yields;
static int model_read(void *ctx, uint32_t address, uint8_t *out, size_t count)
{
    (void)ctx;
    assert(address <= sizeof(media) && count <= sizeof(media) - address);
    memcpy(out, media + address, count);
    return 0;
}

static int model_program(void *ctx, uint32_t address, const uint8_t *data, size_t count)
{
    (void)ctx;
    assert(count && count <= 256 && (address & 255) + count <= 256);
    assert(address < sizeof(media) && count <= sizeof(media) - address);
    ++mutations;
    int torn = cut_after == 0;
    if (cut_after > 0)
        --cut_after;
    size_t n = torn ? count / 2 : count;
    for (size_t i = 0; i < n; ++i)
    {
        assert((media[address + i] & data[i]) == data[i]);
        media[address + i] &= data[i];
    }
    return torn;
}

static int model_erase(void *ctx, uint32_t address)
{
    (void)ctx;
    assert(!(address & 4095) && address < sizeof(media));
    ++mutations;
    int torn = cut_after == 0;
    if (cut_after > 0)
        --cut_after;
    memset(media + address, 255, torn ? 2048 : 4096);
    return torn;
}

static void model_yield(void *ctx)
{
    (void)ctx;
    ++yields;
}
static const storage_io model_io = {NULL, model_read, model_program, model_erase, model_yield};
static uint32_t model_u32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
