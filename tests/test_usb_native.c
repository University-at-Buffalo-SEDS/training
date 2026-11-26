#include "usb_protocol.h"
#include "usb_console.h"
#include "usb_service.h"
#include <assert.h>
#include <string.h>

UINT tx_queue_create(TX_QUEUE *q, char *name, UINT words, VOID *memory, ULONG bytes)
{
    (void)name;
    assert(words <= 16);
    *q = (TX_QUEUE){.words = words, .depth = bytes / (words * 4), .storage = memory};
    return TX_SUCCESS;
}

UINT tx_queue_delete(TX_QUEUE *q)
{
    q->used = 0;
    return TX_SUCCESS;
}

UINT tx_queue_send(TX_QUEUE *q, VOID *p, ULONG wait)
{
    (void)wait;
    if (q->used == q->depth)
        return TX_QUEUE_FULL;
    memcpy(q->storage + q->tail * q->words, p, q->words * 4);
    q->tail = (q->tail + 1) % q->depth;
    ++q->used;
    return TX_SUCCESS;
}

UINT tx_queue_receive(TX_QUEUE *q, VOID *p, ULONG wait)
{
    (void)wait;
    if (!q->used)
        return TX_QUEUE_EMPTY;
    memcpy(p, q->storage + q->head * q->words, q->words * 4);
    q->head = (q->head + 1) % q->depth;
    --q->used;
    return TX_SUCCESS;
}

static int feed(usb_line_parser *parser, const char *bytes, usb_command *c)
{
    int result = 0;
    while (*bytes)
        result = usb_parser_feed(parser, (uint8_t)*bytes++, c);
    return result;
}

int main(void)
{
    usb_line_parser p = {0};
    usb_command c;
    assert(feed(&p, "REA", &c) == 0);
    assert(feed(&p, "D 7 160 80\r\n", &c) == 1);
    assert(c.opcode == USB_CMD_READ && c.id == 7 && c.offset == 160 && c.count == 80);
    const char *invalid[] = {"READ 1 0 0",
                             "READ 1 0 513",
                             "READ -1 0 1",
                             "READ 1 4294967295 1",
                             "READ 4294967296 0 1",
                             "READ 1 0 1 x",
                             "READ  1 0 1",
                             "READ 1 0",
                             "STATUS ",
                             "",
                             "UNKNOWN",
                             "START 1",
                             "ERASE_CONFIRM 1 2 -3",
                             "LOG_READ 0 0",
                             "LOG_READ 0 513"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        assert(!usb_parse_command(invalid[i], &c));
    assert(usb_parse_command("ERASE_CONFIRM 7 2 4294967295", &c));
    assert(c.generation == 2 && c.token == UINT32_MAX);
    for (unsigned i = 0; i < 256; ++i)
        usb_parser_feed(&p, 'A', &c);
    assert(feed(&p, "START\n", &c) == -1); /* Partial suffix must never execute. */
    assert(feed(&p, "START\n", &c) == 1 && c.opcode == USB_CMD_START);
    assert(feed(&p, "STAT\rUS\n", &c) == -1);
    usb_parser_feed(&p, 0, &c);
    assert(feed(&p, "START\n", &c) == -1);
    assert(feed(&p, "STOP\n", &c) == 1);
    assert(feed(&p, "LIVE\n", &c) == 1 && c.opcode == USB_CMD_LIVE);
    assert(feed(&p, "LIVE 1\n", &c) == -1);
    assert(feed(&p, "META 7\n", &c) == 1 && c.opcode == USB_CMD_META && c.id == 7);
    assert(feed(&p, "META 7 0\n", &c) == -1);
    assert(feed(&p, "UNPIN 7\n", &c) == 1 && c.opcode == USB_CMD_UNPIN && c.id == 7);
    assert(usb_crc32("123456789", 9) == 0xcbf43926U);
    assert(usb_crc32("", 0) == 0);
    /* Deterministic arbitrary byte streams, including NUL, CR and long lines. */
    uint32_t random = 123;
    for (unsigned i = 0; i < 200000; ++i)
    {
        random = random * 1664525U + 1013904223U;
        usb_parser_feed(&p, random >> 24, &c);
        assert(p.used < USB_LINE_LIMIT);
    }

    uint8_t data[USB_LOG_CAPACITY + 100], out[USB_LOG_CAPACITY];
    uint32_t start, end, lost;
    for (unsigned i = 0; i < sizeof(data); ++i)
        data[i] = (uint8_t)i;
    usb_log_write(data, 16);
    assert(usb_log_read(0, out, sizeof(out), &start, &end, &lost) == 16);
    assert(start == 0 && end == 16 && lost == 0 && !memcmp(data, out, 16));
    usb_log_write(data, sizeof(data));
    assert(usb_log_read(0, out, sizeof(out), &start, &end, &lost) == USB_LOG_CAPACITY);
    assert(start == 116 && lost == 116 && end == sizeof(data) + 16);
    assert(!memcmp(out, data + 100, USB_LOG_CAPACITY));
    assert(usb_log_read(end, out, 512, &start, &end, &lost) == 0);

    assert(usb_service_init() == TX_SUCCESS);
    assert(!usb_backend_is_ready());
    usb_backend_set_ready(1);
    assert(usb_backend_is_ready());
    c = (usb_command){.connection = usb_connection, .request = 1};
    assert(usb_reply(&c, USB_REPLY_TEXT, 1, "OK TEST\n", 8) == TX_SUCCESS);
    assert(usb_reply(&c, USB_REPLY_TEXT, 1, "OK TWO\n", 7) == TX_SUCCESS);
    assert(usb_reply(&c, USB_REPLY_TEXT, 1, "OK FULL\n", 8) == TX_QUEUE_FULL);
    usb_response r, old;
    assert(tx_queue_receive(&usb_responses, &r, 0) == TX_SUCCESS);
    assert(!memcmp(usb_response_data(&r), "OK TEST\n", 8));
    old = r;
    usb_response_release(&r);
    assert(usb_reply(&c, USB_REPLY_TEXT, 1, "OK NEW\n", 7) == TX_SUCCESS);
    usb_response_release(&old); /* Old generation cannot release reused slot. */
    assert(usb_response_data(&old) == NULL);
    assert(usb_reply(&c, USB_REPLY_TEXT, 1, "OK FULL\n", 8) == TX_QUEUE_FULL);
    while (tx_queue_receive(&usb_responses, &r, 0) == TX_SUCCESS)
        usb_response_release(&r);
    /* Saturated reply queue returns ownership to the free pool. */
    usb_responses.used = usb_responses.depth;
    for (unsigned i = 0; i < 5; ++i)
        assert(usb_reply(&c, USB_REPLY_TEXT, 1, "OK FULL\n", 8) == TX_QUEUE_FULL);
    usb_responses.used = 0;
    assert(usb_reply(&c, USB_REPLY_TEXT, 1, "OK AGAIN\n", 9) == TX_SUCCESS);
    assert(tx_queue_receive(&usb_responses, &r, 0) == TX_SUCCESS);
    usb_response_release(&r);
    assert(usb_reply(&c, USB_REPLY_TEXT, 1, "NO LF", 5) == TX_SIZE_ERROR);
    c.opcode = USB_CMD_READ;
    c.id = 7;
    c.offset = 80;
    c.count = 512;
    assert(usb_reply(&c, USB_REPLY_DATA, 1, data, 511) == TX_SIZE_ERROR);
    assert(usb_reply(&c, USB_REPLY_DATA, 1, data, 512) == TX_SUCCESS);
    assert(tx_queue_receive(&usb_responses, &r, 0) == TX_SUCCESS);
    assert(r.crc32 == usb_crc32(data, 512) && r.offset == 80 && r.id == 7);
    usb_connection_changed();
    assert(!usb_command_is_current(&c));
    assert(usb_reply(&c, USB_REPLY_DATA, 1, data, 512) == TX_DELETED);
    usb_response_release(&r);
    return 0;
}
