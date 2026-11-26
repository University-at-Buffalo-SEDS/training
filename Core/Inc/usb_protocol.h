#pragma once
#include <stddef.h>
#include <stdint.h>

#define USB_LINE_LIMIT 128U /* Includes LF (and optional CR). */
#define USB_CHUNK_SIZE 512U
typedef enum {
    USB_CMD_STATUS = 1, USB_CMD_LIST, USB_CMD_START, USB_CMD_STOP,
    USB_CMD_READ, USB_CMD_ERASE_PREPARE, USB_CMD_ERASE_CONFIRM,
    USB_CMD_OP_STATUS, USB_CMD_LOG_READ, USB_CMD_CLOSE, USB_CMD_LIVE, USB_CMD_META, USB_CMD_UNPIN,
    USB_CMD_EVENTS
} usb_opcode;

/* Wire-independent, copied 64-byte command. Set connection/request in adapter. */
typedef struct {
    uint32_t opcode, connection, request;
    uint32_t id, offset, count, generation, token;
    uint32_t reserved[8];
} usb_command;
_Static_assert(sizeof(usb_command) == 64, "USB command envelope");

typedef struct {
    char line[USB_LINE_LIMIT];
    size_t used;
    int discard;
} usb_line_parser;
/* Feed one byte: 0 = incomplete, 1 = valid command, -1 = rejected whole line. */
int usb_parser_feed(usb_line_parser *parser, uint8_t byte, usb_command *out);
int usb_parse_command(const char *line, usb_command *out);
uint32_t usb_crc32(const void *data, size_t length);
