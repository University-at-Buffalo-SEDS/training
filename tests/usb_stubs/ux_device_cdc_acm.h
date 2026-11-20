#pragma once
#include "tx_api.h"
typedef unsigned char UCHAR;
typedef struct { unsigned bEndpointAddress; } UX_DESCRIPTOR;
typedef struct { int ux_slave_transfer_request_semaphore; } UX_TRANSFER;
typedef struct UX_SLAVE_ENDPOINT_STRUCT {
    UX_DESCRIPTOR ux_slave_endpoint_descriptor;
    struct UX_SLAVE_ENDPOINT_STRUCT *ux_slave_endpoint_next_endpoint;
    UX_TRANSFER ux_slave_endpoint_transfer_request;
} UX_SLAVE_ENDPOINT;
typedef struct { UX_SLAVE_ENDPOINT *ux_slave_interface_first_endpoint; } UX_INTERFACE;
typedef struct {
    UX_INTERFACE *ux_slave_class_cdc_acm_interface;
    int ux_slave_class_cdc_acm_data_dtr_state;
} UX_SLAVE_CLASS_CDC_ACM;
typedef struct { int ux_slave_device_state; } UX_DEVICE;
typedef struct { UX_DEVICE ux_system_slave_device; } UX_SYSTEM;
extern UX_SYSTEM *_ux_system_slave;
#define UX_SUCCESS 0U
#define UX_ERROR 1U
#define UX_NULL NULL
#define UX_DEVICE_CONFIGURED 1U
#define UX_ENDPOINT_DIRECTION 0x80U
#define UX_ENDPOINT_IN 0x80U
#define UX_ENDPOINT_OUT 0U
#define UX_SLAVE_CLASS_CDC_ACM_ENDPOINT_XMIT 1U
#define UX_SLAVE_CLASS_CDC_ACM_ENDPOINT_RCV 2U
#define UX_SLAVE_CLASS_CDC_ACM_IOCTL_ABORT_PIPE 4U
#define UX_SLAVE_CLASS_CDC_ACM_IOCTL_SET_WRITE_TIMEOUT 9U
UINT ux_device_class_cdc_acm_ioctl(UX_SLAVE_CLASS_CDC_ACM *, ULONG, VOID *);
UINT ux_device_class_cdc_acm_write(UX_SLAVE_CLASS_CDC_ACM *, UCHAR *, ULONG, ULONG *);
VOID USBD_CDC_ACM_Activate(VOID *);
VOID USBD_CDC_ACM_Deactivate(VOID *);
VOID USBD_CDC_ACM_ParameterChange(VOID *);
