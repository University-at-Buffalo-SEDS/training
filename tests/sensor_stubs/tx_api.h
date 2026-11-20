#pragma once
#include "../usb_stubs/tx_api.h"
typedef struct { int locked; } TX_MUTEX;
typedef struct { ULONG flags; } TX_EVENT_FLAGS_GROUP;
#define TX_INHERIT 1U
#define TX_OR 0U
#define TX_OPTION_ERROR 5U
#define TX_NOT_AVAILABLE 6U
UINT tx_mutex_create(TX_MUTEX *, char *, UINT);
UINT tx_mutex_get(TX_MUTEX *, ULONG);
UINT tx_mutex_put(TX_MUTEX *);
UINT tx_event_flags_create(TX_EVENT_FLAGS_GROUP *, char *);
UINT tx_event_flags_set(TX_EVENT_FLAGS_GROUP *, ULONG, UINT);
UINT tx_queue_info_get(TX_QUEUE *, char **, ULONG *, ULONG *, TX_THREAD **, ULONG *, TX_QUEUE **);
