#pragma once
#include <stddef.h>
#include <stdint.h>
#define USB_LOG_CAPACITY 8192U
void usb_log_write(const void *bytes, size_t length);
/* Returns retained bytes; clamps expired/future cursors to oldest retained. */
size_t usb_log_read(uint32_t cursor, void *out, size_t capacity,
                   uint32_t *start, uint32_t *end, uint32_t *overwritten);
