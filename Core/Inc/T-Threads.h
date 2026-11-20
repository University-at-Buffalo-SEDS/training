#pragma once
#include "tx_api.h"

#define ABORT_FLAG  (0x01U)
#define LAUNCH_FLAG (0x02U)

extern TX_MUTEX spi1_mutex;   

/* ------ Flash Thread ------ */
extern TX_THREAD flash_thread;
void flash_thread_entry(ULONG initial_input);
UINT create_flash_thread(TX_BYTE_POOL *byte_pool);
/* RFBoard pattern: declarations here, implementation in each *_thread.c. */
extern TX_THREAD heartbeat_thread;
extern TX_THREAD usb_thread;
extern TX_THREAD usb_rx_thread;
extern TX_THREAD sensor_thread;
extern TX_THREAD processing_thread;
UINT create_sensor_thread(void);
UINT create_processing_thread(void);
VOID sensor_thread_entry(ULONG input);
VOID processing_thread_entry(ULONG input);
UINT create_heartbeat_thread(TX_BYTE_POOL *pool);
UINT create_usb_thread(TX_BYTE_POOL *pool);
VOID heartbeat_thread_entry(ULONG input);
VOID usb_thread_entry(ULONG input);
VOID usb_rx_thread_entry(ULONG input);

#define USB_THREAD_STACK_SIZE 2048U
#define USB_THREAD_PRIORITY 20U
#define USB_RX_THREAD_STACK_SIZE 1024U
#define USB_RX_THREAD_PRIORITY 21U
