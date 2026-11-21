#pragma once
#include <stdint.h>
#include <stddef.h>
typedef uint32_t ULONG;
typedef unsigned UINT;
typedef void VOID;
typedef int32_t LONG;
typedef struct { int unused; } TX_THREAD;
typedef struct { int unused; } TX_BYTE_POOL;
typedef struct {
    unsigned words, depth, head, tail, used;
    uint32_t *storage;
} TX_QUEUE;
#define TX_SUCCESS 0U
#define TX_QUEUE_FULL 1U
#define TX_QUEUE_EMPTY 2U
#define TX_SIZE_ERROR 3U
#define TX_DELETED 4U
#define TX_NO_WAIT 0U
#define TX_WAIT_FOREVER UINT32_MAX
#define TX_NO_TIME_SLICE 0U
#define TX_AUTO_START 1U
#define TX_TIMER_TICKS_PER_SECOND 100U
#define TX_PARAMETER_NOT_USED(x) ((void)(x))
#define TX_INTERRUPT_SAVE_AREA
#define TX_DISABLE
#define TX_RESTORE
UINT tx_queue_create(TX_QUEUE *, char *, UINT, VOID *, ULONG);
UINT tx_queue_delete(TX_QUEUE *);
UINT tx_queue_send(TX_QUEUE *, VOID *, ULONG);
UINT tx_queue_receive(TX_QUEUE *, VOID *, ULONG);
UINT tx_thread_create(TX_THREAD *, char *, VOID (*)(ULONG), ULONG, VOID *, ULONG,
                      UINT, UINT, ULONG, UINT);
UINT tx_thread_sleep(ULONG);
ULONG tx_time_get(void);
