#include "usb_protocol.h"
#include <string.h>

static int number(const char **cursor, uint32_t *value)
{
    const char *p = *cursor;
    uint32_t n = 0;
    if (*p < '0' || *p > '9') return 0;
    do {
        uint32_t digit = (uint32_t)(*p++ - '0');
        if (n > (UINT32_MAX - digit) / 10U) return 0;
        n = n * 10U + digit;
    } while (*p >= '0' && *p <= '9');
    *cursor = p;
    *value = n;
    return 1;
}

int usb_parse_command(const char *line, usb_command *out)
{
    static const struct { const char *name; uint32_t opcode; unsigned args; } table[] = {
        {"STATUS", USB_CMD_STATUS, 0}, {"LIST", USB_CMD_LIST, 0},
        {"START", USB_CMD_START, 0}, {"STOP", USB_CMD_STOP, 0},
        {"READ", USB_CMD_READ, 3}, {"ERASE_PREPARE", USB_CMD_ERASE_PREPARE, 1},
        {"ERASE_CONFIRM", USB_CMD_ERASE_CONFIRM, 3},
        {"OP_STATUS", USB_CMD_OP_STATUS, 1}, {"LOG_READ", USB_CMD_LOG_READ, 2},
        {"CLOSE", USB_CMD_CLOSE, 0}, {"LIVE", USB_CMD_LIVE, 0}
    };
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        size_t n = strlen(table[i].name);
        if (strncmp(line, table[i].name, n) || (line[n] && line[n] != ' ')) continue;
        const char *p = line + n;
        uint32_t a[3] = {0};
        for (unsigned j = 0; j < table[i].args; ++j) {
            if (*p++ != ' ' || !number(&p, &a[j])) return 0;
        }
        if (*p) return 0;
        out->opcode = table[i].opcode;
        out->id = a[0];
        if (out->opcode == USB_CMD_READ) {
            out->offset = a[1]; out->count = a[2];
            if (!out->count || out->count > USB_CHUNK_SIZE ||
                out->offset > UINT32_MAX - out->count) return 0;
        } else if (out->opcode == USB_CMD_ERASE_CONFIRM) {
            out->generation = a[1]; out->token = a[2];
        } else if (out->opcode == USB_CMD_LOG_READ) {
            out->offset = a[0]; out->count = a[1]; out->id = 0;
            if (!out->count || out->count > USB_CHUNK_SIZE) return 0;
        }
        return 1;
    }
    return 0;
}

int usb_parser_feed(usb_line_parser *p, uint8_t byte, usb_command *out)
{
    if (byte == '\n') {
        int valid = !p->discard && p->used > 0;
        if (p->used && p->line[p->used - 1] == '\r') --p->used;
        p->line[p->used] = 0;
        valid = valid && usb_parse_command(p->line, out);
        p->used = 0; p->discard = 0;
        return valid ? 1 : -1;
    }
    if (p->discard) return 0;
    if (p->used == USB_LINE_LIMIT - 1 || (byte < 0x20 && byte != '\r') || byte > 0x7e ||
        (p->used && p->line[p->used - 1] == '\r')) {
        p->discard = 1;
        return 0;
    }
    p->line[p->used++] = (char)byte;
    return 0;
}

uint32_t usb_crc32(const void *data, size_t length)
{
    const uint8_t *p = data;
    uint32_t crc = UINT32_MAX;
    while (length--) {
        crc ^= *p++;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return crc ^ UINT32_MAX;
}
