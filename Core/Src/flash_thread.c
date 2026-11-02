#include "main.h"
#include "T-Threads.h"
#include "tx_api.h"

TX_THREAD flash_thread;
#define FLASH_THREAD_STACK_SIZE         4096U   /* Table SDD 4.0*/
#define FLASH_THREAD_PRIORITY           15U     /* Table SDD 4.0*/ 
#define FLASH_THREAD_PREEMPT_THRESHOLD  15U     /* Assumption based on SDD 4.0*/
#define DATA_STEPS_PER_PASS             8U      /* "Data services up to eight record steps" SDD 4.2*/

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