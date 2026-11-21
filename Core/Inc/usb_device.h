#pragma once
#include "ux_device_cdc_acm.h"
#include "usb_service.h"

extern UX_SLAVE_CLASS_CDC_ACM * volatile diagnostic_cdc;
extern volatile UINT diagnostic_usb_start_status;
extern volatile ULONG diagnostic_usb_lines_sent;
typedef struct {
    uint32_t connection, length, packet, reserved;
    uint8_t bytes[48];
} usb_rx_packet;
_Static_assert(sizeof(usb_rx_packet) == 64, "ThreadX queue limit is 16 words");
extern TX_QUEUE usb_rx_packets;
UINT usb_rx_init(void);
int usb_device_connected(UX_SLAVE_CLASS_CDC_ACM *cdc);
void usb_device_abort(UX_SLAVE_CLASS_CDC_ACM *cdc, ULONG direction);
/* Only the direction's owning thread, with no transfer in flight. */
void usb_device_drain_completion(UX_SLAVE_CLASS_CDC_ACM *cdc, ULONG direction);
