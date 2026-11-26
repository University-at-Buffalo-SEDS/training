#include "w25q32jv.h"
#include "main.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

SPI_HandleTypeDef hspi1;
TX_MUTEX spi1_mutex;
static uint8_t media[W25Q_CAPACITY_BYTES], payload[256];
static uint32_t now_ms, busy_until, address;
static unsigned selected, locked, command, payload_size, sleeps, programs, read_chunks;
static int power_down = 1, wrong_id, deny_wel, wel, stuck_busy, fail_lock, fail_tx, fail_rx,
           corrupt_read;
static int busy(void)
{
    return stuck_busy || now_ms < busy_until;
}

uint32_t HAL_GetTick(void)
{
    return now_ms;
}

UINT tx_thread_sleep(ULONG ticks)
{
    assert(!locked && !selected);
    ++sleeps;
    now_ms +=
        (uint32_t)((ticks * 1000 + TX_TIMER_TICKS_PER_SECOND - 1) / TX_TIMER_TICKS_PER_SECOND);
    return TX_SUCCESS;
}

UINT tx_mutex_get(TX_MUTEX *mutex, ULONG timeout)
{
    assert(mutex == &spi1_mutex && timeout && timeout != TX_WAIT_FOREVER);
    if (fail_lock)
        return TX_NOT_AVAILABLE;
    assert(!locked);
    locked = 1;
    return TX_SUCCESS;
}

UINT tx_mutex_put(TX_MUTEX *mutex)
{
    assert(mutex == &spi1_mutex && locked && !selected);
    locked = 0;
    return TX_SUCCESS;
}

void HAL_GPIO_WritePin(void *port, uint16_t pins, int level)
{
    assert(port == GPIOB);
    if (pins != flash_CS_Pin)
    {
        assert(locked && level == GPIO_PIN_SET);
        return;
    }
    if (level == GPIO_PIN_RESET)
    {
        assert(locked && !selected);
        selected = 1;
        command = payload_size = 0;
    }
    else if (selected)
    {
        assert(locked);
        if (command == W25Q_CMD_RELEASE_PD && !busy())
            power_down = 0;
        if (!power_down && !busy())
        {
            if (command == W25Q_CMD_WRITE_ENABLE && !deny_wel)
                wel = 1;
            if (command == W25Q_CMD_PAGE_PROGRAM && wel && payload_size)
            {
                assert((address & 255) + payload_size <= 256);
                for (unsigned i = 0; i < payload_size; ++i)
                    media[address + i] &= payload[i];
                ++programs;
                wel = 0;
                busy_until = now_ms + 3;
            }
            if (command == W25Q_CMD_SECTOR_ERASE && wel)
            {
                assert(!(address & 4095));
                memset(media + address, 255, 4096);
                wel = 0;
                busy_until = now_ms + 400;
            }
        }
        selected = 0;
    }
}
HAL_StatusTypeDef
HAL_SPI_Transmit(SPI_HandleTypeDef *spi, uint8_t *data, uint16_t n, uint32_t timeout)
{
    assert(spi == &hspi1 && locked && selected && timeout == 5);
    if (fail_tx)
        return HAL_ERROR;
    if (!command)
    {
        command = data[0];
        if (command == W25Q_CMD_READ_DATA || command == W25Q_CMD_PAGE_PROGRAM ||
            command == W25Q_CMD_SECTOR_ERASE)
        {
            assert(n == 4);
            address = (uint32_t)data[1] << 16 | (uint32_t)data[2] << 8 | data[3];
            assert(address < W25Q_CAPACITY_BYTES);
        }
        else
            assert(n == 1);
    }
    else
    {
        assert(command == W25Q_CMD_PAGE_PROGRAM && n <= 256);
        memcpy(payload, data, n);
        payload_size = n;
    }
    return HAL_OK;
}
HAL_StatusTypeDef
HAL_SPI_Receive(SPI_HandleTypeDef *spi, uint8_t *data, uint16_t n, uint32_t timeout)
{
    assert(spi == &hspi1 && locked && selected && timeout == 5);
    if (fail_rx)
        return HAL_ERROR;
    assert(!power_down);
    if (command == W25Q_CMD_READ_STATUS1)
    {
        assert(n == 1);
        data[0] = (busy() ? 1 : 0) | (wel ? 2 : 0);
    }
    else if (command == W25Q_CMD_JEDEC_ID)
    {
        assert(n == 3 && !busy());
        data[0] = 0xef;
        data[1] = 0x40;
        data[2] = wrong_id ? 0x17 : 0x16;
    }
    else
    {
        assert(command == W25Q_CMD_READ_DATA && !busy() && n <= W25Q_READ_CHUNK);
        assert(n <= W25Q_CAPACITY_BYTES - address);
        memcpy(data, media + address, n);
        if (corrupt_read)
            data[0] ^= 1;
        address += n;
        ++read_chunks;
    }
    return HAL_OK;
}

int main(void)
{
    memset(media, 255, sizeof(media));
    w25q_t dev;
    assert(w25q_init(&dev, &hspi1, GPIOB, flash_CS_Pin) == W25Q_OK && dev.initialized);
    assert(!power_down && sleeps >= 2 && !locked && !selected);
    busy_until = now_ms + 80;
    assert(w25q_init(&dev, &hspi1, GPIOB, flash_CS_Pin) ==
           W25Q_OK); /* MCU reset during an erase. */
    uint8_t data[1024], expected[256];
    for (unsigned i = 0; i < sizeof(expected); ++i)
        expected[i] = (uint8_t)(i ^ 0x5a);
    assert(w25q_page_program(&dev, 0x100, expected, 256) == W25Q_OK && programs == 1);
    assert(!memcmp(media + 0x100, expected, 256) && !wel);
    read_chunks = 0;
    assert(w25q_read(&dev, 0, data, sizeof(data)) == W25Q_OK && read_chunks == 4);
    assert(!memcmp(data + 0x100, expected, 256));
    unsigned before = programs;
    assert(w25q_page_program(&dev, 255, expected, 2) == W25Q_ERR_PARAM && programs == before);
    assert(w25q_read(&dev, W25Q_CAPACITY_BYTES - 2, data, 3) == W25Q_ERR_PARAM);
    assert(w25q_sector_erase(&dev, 0x100) == W25Q_ERR_PARAM);
    deny_wel = 1;
    assert(w25q_page_program(&dev, 0x200, expected, 16) == W25Q_ERR_WEL && programs == before);
    deny_wel = 0;
    corrupt_read = 1;
    assert(w25q_page_program(&dev, 0x200, expected, 16) == W25Q_ERR_VERIFY &&
           programs == before + 1);
    corrupt_read = 0;
    memset(media + 0x4000, 0, 4096);
    uint32_t start = now_ms;
    assert(w25q_sector_erase(&dev, 0x4000) == W25Q_OK && now_ms - start >= 400);
    for (unsigned i = 0; i < 4096; ++i)
        assert(media[0x4000 + i] == 255);
    assert(!memcmp(media + 0x100, expected, 256));
    fail_lock = 1;
    assert(w25q_read(&dev, 0, data, 1) == W25Q_ERR_LOCK);
    fail_lock = 0;
    fail_tx = 1;
    assert(w25q_read(&dev, 0, data, 1) == W25Q_ERR_HAL);
    fail_tx = 0;
    assert(!selected && !locked);
    fail_rx = 1;
    assert(w25q_read(&dev, 0, data, 1) == W25Q_ERR_HAL);
    fail_rx = 0;
    assert(!selected && !locked);
    stuck_busy = 1;
    start = now_ms;
    assert(w25q_read(&dev, 0, data, 1) == W25Q_ERR_TIMEOUT && now_ms - start >= 400 &&
           now_ms - start < 420);
    stuck_busy = 0;
    wrong_id = 1;
    assert(w25q_init(&dev, &hspi1, GPIOB, flash_CS_Pin) == W25Q_ERR_ID && !dev.initialized);
    assert(w25q_read(&dev, 0, data, 1) == W25Q_ERR_ID);
    assert(w25q_page_program(&dev, 0, expected, 1) == W25Q_ERR_ID);
    assert(w25q_sector_erase(&dev, 0) == W25Q_ERR_ID);
    w25q_t invalid = {0};
    assert(w25q_read_jedec_id(&invalid, data) == W25Q_ERR_PARAM);
    puts("W25Q SPI command, timeout, readback, identity and cleanup tests passed.");
    return 0;
}
