#include "main.h"
#include "T-Threads.h"
#include "storage_service.h"
#include "w25q32jv.h"
#include <stdio.h>

TX_THREAD flash_thread;
#define FLASH_THREAD_STACK_SIZE 4096U
_Alignas(8) static ULONG flash_stack[FLASH_THREAD_STACK_SIZE / sizeof(ULONG)];
static w25q_t flash;
volatile w25q_status_t diagnostic_flash_init_status;
volatile int diagnostic_storage_mount_status;

static int read_flash(void *context, uint32_t address, uint8_t *out, size_t count)
{ return w25q_read(context, address, out, count) != W25Q_OK; }
static int program_flash(void *context, uint32_t address, const uint8_t *data, size_t count)
{ return w25q_page_program(context, address, data, count) != W25Q_OK; }
static int erase_flash(void *context, uint32_t address)
{ return w25q_sector_erase(context, address) != W25Q_OK; }
static void yield_flash(void *context)
{ TX_PARAMETER_NOT_USED(context); tx_thread_sleep(1); }

void flash_thread_entry(ULONG input)
{
    TX_PARAMETER_NOT_USED(input);
    diagnostic_flash_init_status = w25q_init(&flash, &hspi1, flash_CS_GPIO_Port, flash_CS_Pin);
    printf("FLASH init=%u JEDEC=%02X %02X %02X (expected EF 40 16)\n",
           (unsigned)diagnostic_flash_init_status, (unsigned)flash.jedec_id[0],
           (unsigned)flash.jedec_id[1], (unsigned)flash.jedec_id[2]);
    storage_io io = {&flash, read_flash, program_flash, erase_flash, yield_flash};
    diagnostic_storage_mount_status = storage_service_init(&io);
    /* Read-only boot dump. No debug code erases/programs a scratch sector. */
    uint8_t data[64];
    if (diagnostic_flash_init_status == W25Q_OK && w25q_read(&flash, 0, data, sizeof(data)) == W25Q_OK) {
        for (unsigned row = 0; row < sizeof(data); row += 16) {
            char hex[49];
            for (unsigned i = 0; i < 16; ++i) snprintf(hex + i*3, 4, "%02X ", (unsigned)data[row+i]);
            printf("FLASH 0x%06X: %s\n", row, hex);
        }
    }
    ULONG report = tx_time_get();
    for (;;) {
        storage_service_step();
        if ((ULONG)(tx_time_get() - report) >= TX_TIMER_TICKS_PER_SECOND) {
            const storage *disk = storage_service_state();
            if (disk->active != UINT32_MAX) {
                const storage_session *session = &disk->sessions[disk->active];
                printf("FLASH session=%lu durable=%lu bytes=%lu fault=%lu\n",
                       (unsigned long)session->id, (unsigned long)session->records,
                       (unsigned long)(session->records * 80), (unsigned long)disk->fault);
                uint8_t record[80];
                int status = storage_service_debug_last(record);
                if (status == STORE_OK) {
                    char hex[49];
                    for (unsigned i = 0; i < 16; ++i)
                        snprintf(hex + i*3, 4, "%02X ", (unsigned)record[24+i]);
                    printf("FLASH readback type=%u payload[0:16]=%s\n", (unsigned)record[5], hex);
                } else if (status != STORE_NOT_FOUND && status != STORE_STATE)
                    printf("FLASH debug readback failed=%u\n", (unsigned)status);
            }
            report = tx_time_get();
        }
        tx_thread_sleep(1);
    }
}

UINT create_flash_thread(TX_BYTE_POOL *pool)
{
    TX_PARAMETER_NOT_USED(pool);
    return tx_thread_create(&flash_thread, "Flash/data", flash_thread_entry, 0,
                            flash_stack, sizeof(flash_stack), 15, 15,
                            TX_NO_TIME_SLICE, TX_AUTO_START);
}
