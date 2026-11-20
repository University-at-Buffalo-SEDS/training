#include "T-Threads.h"
#include "usb_device.h"
#include <string.h>

TX_THREAD usb_rx_thread;
TX_QUEUE usb_rx_packets;
_Alignas(8) static ULONG rx_stack[USB_RX_THREAD_STACK_SIZE / sizeof(ULONG)];
static ULONG rx_memory[8 * (sizeof(usb_rx_packet) / sizeof(ULONG))];

UINT usb_rx_init(void)
{
    UINT status = tx_queue_create(&usb_rx_packets, "USB RX packets",
                                  sizeof(usb_rx_packet) / sizeof(ULONG),
                                  rx_memory, sizeof(rx_memory));
    if (status != TX_SUCCESS) return status;
    status = tx_thread_create(&usb_rx_thread, "USB receive", usb_rx_thread_entry, 0,
                              rx_stack, sizeof(rx_stack), USB_RX_THREAD_PRIORITY,
                              USB_RX_THREAD_PRIORITY, TX_NO_TIME_SLICE, TX_AUTO_START);
    if (status != TX_SUCCESS) tx_queue_delete(&usb_rx_packets);
    return status;
}

VOID usb_rx_thread_entry(ULONG input)
{
    uint32_t serial = 0;
    TX_PARAMETER_NOT_USED(input);
    for (;;) {
        UX_SLAVE_CLASS_CDC_ACM *cdc = diagnostic_cdc;
        if (!usb_device_connected(cdc)) { tx_thread_sleep(1); continue; }
        usb_rx_packet p = {0};
        p.connection = usb_connection;
        /* One blocking reader, no synchronous writes in this thread. The
         * adapter remains free to service responses while OUT is idle. */
        ux_device_class_cdc_acm_ioctl(cdc, UX_SLAVE_CLASS_CDC_ACM_IOCTL_SET_READ_TIMEOUT,
                                      (VOID *)(uintptr_t)TX_WAIT_FOREVER);
        uint8_t bytes[64];
        ULONG actual = 0;
        usb_device_drain_completion(cdc, UX_SLAVE_CLASS_CDC_ACM_ENDPOINT_RCV);
        UINT status = ux_device_class_cdc_acm_read(cdc, bytes, sizeof(bytes), &actual);
        if (status == UX_SUCCESS && actual && p.connection == usb_connection) {
            for (ULONG offset = 0; offset < actual; offset += p.length) {
                p.length = (uint32_t)(actual - offset);
                if (p.length > sizeof(p.bytes)) p.length = sizeof(p.bytes);
                memcpy(p.bytes, bytes + offset, p.length);
                p.packet = ++serial;
                if (tx_queue_send(&usb_rx_packets, &p, TX_NO_WAIT) != TX_SUCCESS) ++usb_rx_drops;
            }
        } else if (status != UX_SUCCESS) {
            tx_thread_sleep(1);
        }
    }
}
