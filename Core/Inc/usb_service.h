#pragma once
#include "tx_api.h"
#include "usb_protocol.h"

enum { USB_REPLY_TEXT = 1, USB_REPLY_DATA = 2 };
typedef struct {
    uint32_t connection, request, kind, final;
    uint32_t buffer_index, buffer_generation, length, id, offset, crc32;
    uint32_t reserved[6];
} usb_response;
_Static_assert(sizeof(usb_response) == 64, "USB response envelope");

extern TX_QUEUE usb_commands;
extern TX_QUEUE usb_responses;
extern volatile uint32_t usb_connection;
extern volatile uint32_t usb_command_drops, usb_rx_drops, usb_transfer_errors;

UINT usb_service_init(void);
/* Data/flash calls ready only after its queues/session recovery are ready. */
void usb_backend_set_ready(int ready);
int usb_backend_is_ready(void);
/* Data owns session/token/lease state. Check current connection before work,
 * and on EVERY service pass release old leases/tokens (even if no command).
 * A connection change cancels replies, not a deletion already journaled. */
int usb_command_is_current(const usb_command *command);
/* Nonblocking copy into one of two owned chunks. On failure caller retains
 * payload and retries later; on success it may reuse its source immediately. */
UINT usb_reply(const usb_command *command, unsigned kind, int final,
               const void *payload, size_t length);
/* Adapter only. Response ownership lasts through synchronous write/abort. */
const uint8_t *usb_response_data(const usb_response *response);
void usb_response_release(const usb_response *response);
void usb_connection_changed(void);
