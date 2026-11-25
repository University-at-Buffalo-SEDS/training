#include "main.h"
#include "T-Threads.h"
#include "records.h"
#include "stm32g4xx_hal_spi.h"
#include "tx_api.h"
#include "w25q32jv.h"
#include <stdint.h>
#include <stdio.h>

TX_THREAD flash_thread;
#define FLASH_THREAD_STACK_SIZE         4096U   /* Table SDD 4.0*/
#define FLASH_THREAD_PRIORITY           15U     /* Table SDD 4.0*/ 
#define FLASH_THREAD_PREEMPT_THRESHOLD  15U     /* Assumption based on SDD 4.0*/
#define DATA_STEPS_PER_PASS             8U      /* "Data services up to eight record steps" SDD 4.2*/
#define RECORD_MAGIC                    "DLOG" /* Table SDD A.1 */

extern SPI_HandleTypeDef hspi1;

static uint32_t CRCTable[256];

/* CRC Code is copied from wikipedia */
/* https://en.wikipedia.org/wiki/Computation_of_cyclic_redundancy_checks#CRC-32_example */
static void CRC32_init(void)
{
    uint32_t crc32 = 1;
    // C guarantees CRCTable[0] = 0 already.
    for (unsigned int i = 128; i; i >>= 1) {
        crc32 = (crc32 >> 1) ^ (crc32 & 1 ? 0xedb88320 : 0);
        for (unsigned int j = 0; j < 256; j += 2*i)
            CRCTable[i + j] = crc32 ^ CRCTable[j];
    }
}

static uint32_t CRC32(const uint8_t data[], size_t data_length)
{
    uint32_t crc32 = 0xFFFFFFFFu;

    if (CRCTable[255] == 0)
        CRC32_init();
	
    for (size_t i = 0; i < data_length; i++) {
        crc32 ^= data[i];
        crc32 = (crc32 >> 8) ^ CRCTable[crc32 & 0xFF];
    }
	
    // Finalize the CRC-32 value by inverting all the bits
    crc32 ^= 0xFFFFFFFFu;
    return crc32;
}


static void encode_record(const logger_record *in, flash_record *out,
                          uint32_t session) {
    memset(out, 0, sizeof(*out));
    memcpy(out->bytes, RECORD_MAGIC, 4);

    out->bytes[4] = in->bytes[1]; // schema version
    out->bytes[5] = in->bytes[0]; // record type
    memcpy(out->bytes + 6, in->bytes + 2, 2); // payload length
    *(uint32_t *)(out->bytes + 8) = session;  // session id
    memcpy(out->bytes + 12, in->bytes + 4, 60); // input sequence, timestamp, and payload

    // CRC-32
    *(uint32_t *)(out->bytes + 72) = CRC32(out->bytes, 72);
}

void flash_thread_entry(ULONG initial_input)
{
    (void)initial_input;

    w25q_t flash;
    w25q_status_t ret;

    ret = w25q_init(&flash, &hspi1, flash_CS_GPIO_Port, flash_CS_Pin);
    printf("flash init return code: %d\n", ret);
  
    for (;;) {
        tx_thread_sleep(1);
    }
}

UINT create_flash_thread(TX_BYTE_POOL *byte_pool)
{

        CHAR *pointer;

  /* Allocate to the stack */
  if (tx_byte_allocate(byte_pool, (VOID**) &pointer,
                       FLASH_THREAD_STACK_SIZE, TX_NO_WAIT) != TX_SUCCESS)
  {
    return TX_POOL_ERROR;
  }

    UINT status = tx_thread_create(&flash_thread,
                                   "Flash Thread",
                                   flash_thread_entry,
                                   0,
                                   pointer,
                                   FLASH_THREAD_STACK_SIZE,
                                   FLASH_THREAD_PRIORITY,
                                   FLASH_THREAD_PREEMPT_THRESHOLD,
                                   TX_NO_TIME_SLICE,
                                   TX_AUTO_START);

    return status;
}
