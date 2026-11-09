#include "main.h"
#include "T-Threads.h"
#include "tx_api.h"

TX_THREAD flash_thread;
#define FLASH_THREAD_STACK_SIZE         4096U   /* Table SDD 4.0*/
#define FLASH_THREAD_PRIORITY           15U     /* Table SDD 4.0*/ 
#define FLASH_THREAD_PREEMPT_THRESHOLD  15U     /* Assumption based on SDD 4.0*/
#define DATA_STEPS_PER_PASS             8U      /* "Data services up to eight record steps" SDD 4.2*/
#define FLASH_SPI_TIMEOUT               100U    /* Uncertain, should verify later */

/* flash instructions have at most 8 bytes of input, 8.1.2 & 8.1.3 of flash datasheet */
#define FLASH_ARG_LIMIT 8

HAL_StatusTypeDef flash_send_command(SPI_HandleTypeDef *hspi, uint8_t command,
                                     const uint8_t *args, size_t argc) {

    if (argc > FLASH_ARG_LIMIT) {
        return HAL_ERROR;
    }

    HAL_StatusTypeDef st;
    uint8_t tx[1 + FLASH_ARG_LIMIT];
    tx[0] = command;

    if (args && argc) {
        memcpy(&tx[1], args, argc);
    }

    // insert mutex_get
    HAL_GPIO_WritePin(flash_CS_GPIO_Port, flash_CS_Pin, GPIO_PIN_RESET);
    st = HAL_SPI_Transmit(hspi, tx, 1 + argc, FLASH_SPI_TIMEOUT);
    HAL_GPIO_WritePin(flash_CS_GPIO_Port, flash_CS_Pin, GPIO_PIN_SET);
    // insert mutex_put

    return st;
}

void flash_thread_entry(ULONG initial_input)
{
    (void)initial_input; 
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
