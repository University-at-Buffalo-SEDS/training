#include "usb_service.h"
#include <string.h>

TX_QUEUE usb_commands, usb_responses;
static ULONG command_memory[8 * 16], response_memory[8 * 16];
/* A reply owns a transmit chunk until USB sends it or discards it. The
 * generation lets the adapter reject stale handles after a chunk is reused. */
static struct
{
    uint8_t data[USB_CHUNK_SIZE];
    uint32_t generation;
    int owned;
} chunks[2];
volatile uint32_t usb_connection = 1;
volatile uint32_t usb_command_drops, usb_rx_drops, usb_transfer_errors;
static volatile int backend_ready;

UINT usb_service_init(void)
{
    _Static_assert(sizeof(ULONG) == 4, "ThreadX messages use 32-bit words");
    UINT status =
        tx_queue_create(&usb_commands, "USB commands", 16, command_memory, sizeof(command_memory));
    if (status != TX_SUCCESS)
        return status;
    status = tx_queue_create(
        &usb_responses, "USB responses", 16, response_memory, sizeof(response_memory));
    if (status != TX_SUCCESS)
        tx_queue_delete(&usb_commands);
    return status;
}

void usb_backend_set_ready(int ready)
{
    backend_ready = ready;
}

int usb_backend_is_ready(void)
{
    return backend_ready;
}

int usb_command_is_current(const usb_command *c)
{
    return c->connection == usb_connection;
}

void usb_connection_changed(void)
{
    TX_INTERRUPT_SAVE_AREA
    TX_DISABLE
    if (++usb_connection == 0)
        ++usb_connection;
    TX_RESTORE
}

UINT usb_reply(const usb_command *c, unsigned kind, int final, const void *payload, size_t length)
{
    usb_response r = {0};
    unsigned index;
    if (!usb_command_is_current(c))
        return TX_DELETED;
    if (length > USB_CHUNK_SIZE || !payload || (kind != USB_REPLY_TEXT && kind != USB_REPLY_DATA) ||
        (kind == USB_REPLY_DATA && (!final || !length ||
                                    !((c->opcode == USB_CMD_READ && length == c->count) ||
                                      (c->opcode == USB_CMD_META && length == 256)))) ||
        (kind == USB_REPLY_TEXT && (!length || ((const char *)payload)[length - 1] != '\n')))
        return TX_SIZE_ERROR;
    TX_INTERRUPT_SAVE_AREA
    TX_DISABLE
    for (index = 0; index < 2 && chunks[index].owned; ++index)
    {
    }
    if (index == 2)
    {
        TX_RESTORE return TX_QUEUE_FULL;
    }
    chunks[index].owned = 1;
    r.buffer_generation = ++chunks[index].generation;
    TX_RESTORE
    memcpy(chunks[index].data, payload, length);
    r.buffer_index = index;
    r.connection = c->connection;
    r.request = c->request;
    r.kind = kind;
    r.final = final != 0;
    r.length = (uint32_t)length;
    r.id = c->id;
    r.offset = c->offset;
    r.crc32 = usb_crc32(payload, length);
    UINT status =
        usb_command_is_current(c) ? tx_queue_send(&usb_responses, &r, TX_NO_WAIT) : TX_DELETED;
    if (status != TX_SUCCESS)
        usb_response_release(&r);
    return status;
}

const uint8_t *usb_response_data(const usb_response *r)
{
    if (r->buffer_index >= 2 || !chunks[r->buffer_index].owned ||
        chunks[r->buffer_index].generation != r->buffer_generation)
        return NULL;
    return chunks[r->buffer_index].data;
}

void usb_response_release(const usb_response *r)
{
    TX_INTERRUPT_SAVE_AREA
    TX_DISABLE
    if (r->buffer_index < 2 && chunks[r->buffer_index].generation == r->buffer_generation)
        chunks[r->buffer_index].owned = 0;
    TX_RESTORE
}
