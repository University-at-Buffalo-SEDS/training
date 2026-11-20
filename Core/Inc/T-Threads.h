#pragma once
#include "telemetry.h"
#include "tx_api.h"

#define ABORT_FLAG  (0x01U)
#define LAUNCH_FLAG (0x02U)

extern TX_MUTEX spi1_mutex;   

/* ------ Flash Thread ------ */
extern TX_THREAD flash_thread;
void flash_thread_entry(ULONG initial_input);
UINT create_flash_thread(TX_BYTE_POOL *byte_pool);
