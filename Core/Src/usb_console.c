#include "usb_console.h"
#include "tx_api.h"
#include <errno.h>
#include <stdio.h>

static unsigned char history[USB_LOG_CAPACITY];
static uint32_t log_end, retained, lost;

void usb_log_write(const void *bytes, size_t length)
{
    const unsigned char *p = bytes;
    /* Never wait for USB, a queue, or a mutex; usable before the scheduler. */
    while (length) {
        size_t n = length > 64 ? 64 : length;
        TX_INTERRUPT_SAVE_AREA
        TX_DISABLE
        uint32_t room = USB_LOG_CAPACITY - retained;
        lost += n > room ? (uint32_t)(n - room) : 0;
        for (size_t i = 0; i < n; ++i) history[log_end++ % USB_LOG_CAPACITY] = *p++;
        retained = n >= room ? USB_LOG_CAPACITY : retained + (uint32_t)n;
        TX_RESTORE
        length -= n;
    }
}

size_t usb_log_read(uint32_t cursor, void *out, size_t capacity,
                   uint32_t *start, uint32_t *end, uint32_t *overwritten)
{
    unsigned char *p = out;
    TX_INTERRUPT_SAVE_AREA
    TX_DISABLE
    if ((uint32_t)(log_end - cursor) > retained) cursor = log_end - retained;
    size_t n = (uint32_t)(log_end - cursor);
    if (n > capacity) n = capacity;
    for (size_t i = 0; i < n; ++i) p[i] = history[(cursor + (uint32_t)i) % USB_LOG_CAPACITY];
    *start = cursor; *end = log_end; *overwritten = lost;
    TX_RESTORE
    return n;
}

/* Strong override of CubeMX's weak newlib syscall. */
int _write(int file, char *bytes, int length)
{
    if (file != 1 && file != 2) { errno = EBADF; return -1; }
    if (length < 0) { errno = EINVAL; return -1; }
    usb_log_write(bytes, (size_t)length);
    return length;
}

int __io_putchar(int ch)
{
    unsigned char c = (unsigned char)ch;
    usb_log_write(&c, 1);
    return ch;
}
