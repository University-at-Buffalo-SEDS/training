#include "T-Threads.h"
#include "usb_device.h"
#include "usb_console.h"
#include "sensor_live.h"
#include "main.h"
#include "ux_dcd_stm32.h"
#include "ux_device_descriptors.h"
#include "ux_utility.h"
#include <stdio.h>
#include <string.h>

TX_THREAD usb_thread;
_Alignas(8) static ULONG usb_stack[USB_THREAD_STACK_SIZE / sizeof(ULONG)];
volatile UINT diagnostic_usb_start_status = UX_ERROR;
volatile ULONG diagnostic_usb_lines_sent;
extern PCD_HandleTypeDef hpcd_USB_FS;

int usb_device_connected(UX_SLAVE_CLASS_CDC_ACM *cdc)
{
    return cdc != UX_NULL && diagnostic_cdc == cdc &&
        _ux_system_slave->ux_system_slave_device.ux_slave_device_state == UX_DEVICE_CONFIGURED &&
        cdc->ux_slave_class_cdc_acm_data_dtr_state;
}

void usb_device_abort(UX_SLAVE_CLASS_CDC_ACM *cdc, ULONG direction)
{
    if (cdc != UX_NULL)
        ux_device_class_cdc_acm_ioctl(cdc, UX_SLAVE_CLASS_CDC_ACM_IOCTL_ABORT_PIPE,
                                      (VOID *)(uintptr_t)direction);
}

void usb_device_drain_completion(UX_SLAVE_CLASS_CDC_ACM *cdc, ULONG direction)
{
    UX_SLAVE_ENDPOINT *endpoint = cdc->ux_slave_class_cdc_acm_interface->ux_slave_interface_first_endpoint;
    unsigned desired = direction == UX_SLAVE_CLASS_CDC_ACM_ENDPOINT_XMIT ? UX_ENDPOINT_IN : UX_ENDPOINT_OUT;
    while (endpoint && (endpoint->ux_slave_endpoint_descriptor.bEndpointAddress & UX_ENDPOINT_DIRECTION) != desired)
        endpoint = endpoint->ux_slave_endpoint_next_endpoint;
    if (endpoint) {
        /* This ST DCD leaves a semaphore token when abort follows timeout.
         * Remove stale completion before this sole owner starts a new transfer. */
        while (ux_utility_semaphore_get(&endpoint->ux_slave_endpoint_transfer_request.
                                         ux_slave_transfer_request_semaphore, TX_NO_WAIT) == UX_SUCCESS) {}
    }
}

UINT create_usb_thread(TX_BYTE_POOL *pool)
{
    TX_PARAMETER_NOT_USED(pool);
    UINT status = usb_service_init();
    if (status != TX_SUCCESS) return status;
    status = usb_rx_init();
    if (status != TX_SUCCESS) return status;
    return tx_thread_create(&usb_thread, "USB adapter", usb_thread_entry, 0,
                            usb_stack, sizeof(usb_stack), USB_THREAD_PRIORITY,
                            USB_THREAD_PRIORITY, TX_NO_TIME_SLICE, TX_AUTO_START);
}

static int transmit(UX_SLAVE_CLASS_CDC_ACM *cdc, uint32_t connection,
                    const void *data, size_t size)
{
    ULONG actual = 0;
    if (connection != usb_connection || !usb_device_connected(cdc)) return 0;
    usb_device_drain_completion(cdc, UX_SLAVE_CLASS_CDC_ACM_ENDPOINT_XMIT);
    ux_device_class_cdc_acm_ioctl(cdc, UX_SLAVE_CLASS_CDC_ACM_IOCTL_SET_WRITE_TIMEOUT,
                                  (VOID *)(uintptr_t)(TX_TIMER_TICKS_PER_SECOND / 4 + 1));
    UINT status = ux_device_class_cdc_acm_write(cdc, (UCHAR *)data, (ULONG)size, &actual);
    if (status != UX_SUCCESS || actual != size || connection != usb_connection) {
        /* A partial frame cannot safely be followed by another response. */
        usb_device_abort(cdc, UX_SLAVE_CLASS_CDC_ACM_ENDPOINT_XMIT);
        ++usb_transfer_errors;
        return 0;
    }
    return 1;
}

static int text(UX_SLAVE_CLASS_CDC_ACM *cdc, uint32_t epoch, const char *message)
{
    if (!transmit(cdc, epoch, message, strlen(message))) return 0;
    ++diagnostic_usb_lines_sent;
    return 1;
}

static void controller_start(void)
{
    /* ST G4 PMA: reserve 64 bytes for eight endpoint-table entries. */
    if (HAL_PCDEx_PMAConfig(&hpcd_USB_FS, 0x00, PCD_SNG_BUF, 0x40) != HAL_OK ||
        HAL_PCDEx_PMAConfig(&hpcd_USB_FS, 0x80, PCD_SNG_BUF, 0x80) != HAL_OK ||
        HAL_PCDEx_PMAConfig(&hpcd_USB_FS, USBD_CDCACM_EPOUT_ADDR, PCD_SNG_BUF, 0xC0) != HAL_OK ||
        HAL_PCDEx_PMAConfig(&hpcd_USB_FS, USBD_CDCACM_EPIN_ADDR, PCD_SNG_BUF, 0x100) != HAL_OK ||
        HAL_PCDEx_PMAConfig(&hpcd_USB_FS, USBD_CDCACM_EPINCMD_ADDR, PCD_SNG_BUF, 0x140) != HAL_OK)
        Error_Handler();
    diagnostic_usb_start_status = ux_dcd_stm32_initialize((ULONG)USB, (ULONG)(uintptr_t)&hpcd_USB_FS);
    if (diagnostic_usb_start_status != UX_SUCCESS) Error_Handler();
    if (HAL_PCD_Start(&hpcd_USB_FS) != HAL_OK) {
        diagnostic_usb_start_status = UX_ERROR;
        Error_Handler();
    }
    usb_log_write("USB adapter ready\n", 18);
}

/* Local diagnostics are polled, never injected between DATA header/body. */
static int local_command(UX_SLAVE_CLASS_CDC_ACM *cdc, usb_command *c)
{
    /* Adapter-only scratch keeps binary snapshots off the 2 KiB stack. */
    static char header[256];
    if (c->opcode == USB_CMD_LIVE) {
        static uint32_t reference_connection;
        static logger_record frame[2];
        uint32_t age_ms;
        if (reference_connection != c->connection) {
            sensor_live_zero();
            reference_connection = c->connection;
        }
        if (!sensor_live_read(frame, &age_ms))
            return text(cdc, c->connection, "ERR NOT_READY sensors_initializing\n");
        snprintf(header, sizeof(header), "LIVE 1 128 %lu %08lX\n",
                 (unsigned long)age_ms, (unsigned long)usb_crc32(frame, sizeof(frame)));
        return text(cdc, c->connection, header) &&
            transmit(cdc, c->connection, frame, sizeof(frame)) && text(cdc, c->connection, "\n");
    }
    if (c->opcode == USB_CMD_LOG_READ) {
        static uint8_t data[USB_CHUNK_SIZE];
        uint32_t start, end, lost;
        size_t n = usb_log_read(c->offset, data, c->count, &start, &end, &lost);
        snprintf(header, sizeof(header), "LOG %lu %u %lu %lu %08lX\n",
                 (unsigned long)start, (unsigned)n, (unsigned long)end,
                 (unsigned long)lost, (unsigned long)usb_crc32(data, n));
        return text(cdc, c->connection, header) &&
            (!n || transmit(cdc, c->connection, data, n)) && text(cdc, c->connection, "\n");
    }
    if (c->opcode == USB_CMD_STATUS && !usb_backend_is_ready()) {
        snprintf(header, sizeof(header),
                 "OK STATUS protocol=1 firmware=usb-1 state=NOT_READY active=0 storage=unavailable "
                 "durable=0 cmd_drop=%lu rx_drop=%lu tx_error=%lu uptime_ms=%lu uid=%08lX%08lX%08lX\n",
                 (unsigned long)usb_command_drops, (unsigned long)usb_rx_drops,
                 (unsigned long)usb_transfer_errors, (unsigned long)HAL_GetTick(),
                 (unsigned long)HAL_GetUIDw0(), (unsigned long)HAL_GetUIDw1(),
                 (unsigned long)HAL_GetUIDw2());
        return text(cdc, c->connection, header);
    }
    return text(cdc, c->connection, "ERR NOT_READY storage_worker_unavailable\n");
}

VOID usb_thread_entry(ULONG input)
{
    TX_PARAMETER_NOT_USED(input);
    controller_start();
    usb_line_parser parser = {0};
    usb_command active = {0};
    uint32_t epoch = usb_connection, request = 0, last_packet = 0;
    ULONG deadline = 0;
    int pending = 0, failed = 0;
    for (;;) {
        UX_SLAVE_CLASS_CDC_ACM *cdc = diagnostic_cdc;
        if (epoch != usb_connection) {
            epoch = usb_connection;
            memset(&parser, 0, sizeof(parser));
            pending = failed = 0; last_packet = 0;
        }
        usb_response r;
        if (!usb_device_connected(cdc) || failed) {
            while (tx_queue_receive(&usb_responses, &r, TX_NO_WAIT) == TX_SUCCESS)
                usb_response_release(&r);
            tx_thread_sleep(1); continue;
        }
        /* Always retire stale chunks, including after a reconnect. */
        for (unsigned i = 0; i < 8 && tx_queue_receive(&usb_responses, &r, TX_NO_WAIT) == TX_SUCCESS; ++i) {
            const uint8_t *data = usb_response_data(&r);
            if (data && pending && r.connection == epoch && r.request == active.request) {
                int ok = 1;
                if (r.kind == USB_REPLY_DATA) {
                    char header[96];
                    snprintf(header, sizeof(header), "DATA %lu %lu %lu %08lX\n",
                             (unsigned long)r.id, (unsigned long)r.offset,
                             (unsigned long)r.length, (unsigned long)r.crc32);
                    ok = text(cdc, epoch, header);
                }
                ok = ok && transmit(cdc, epoch, data, r.length);
                if (ok && r.kind == USB_REPLY_DATA) ok = text(cdc, epoch, "\n");
                if (!ok) failed = 1;
                if (r.final) pending = 0;
                deadline = tx_time_get() + 3 * TX_TIMER_TICKS_PER_SECOND;
            }
            usb_response_release(&r);
            if (failed) break;
        }
        if (failed) { usb_connection_changed(); failed = 1; epoch = usb_connection; continue; }
        if (pending && (LONG)(tx_time_get() - deadline) >= 0) {
            text(cdc, epoch, "ERR TIMEOUT reconnect_required\n");
            usb_connection_changed(); epoch = usb_connection; pending = 0; failed = 1;
            continue;
        }
        usb_rx_packet packet;
        if (tx_queue_receive(&usb_rx_packets, &packet, TX_NO_WAIT) != TX_SUCCESS) {
            tx_thread_sleep(1); continue;
        }
        if (packet.connection != epoch) continue;
        if (last_packet && packet.packet != last_packet + 1) parser.discard = 1;
        last_packet = packet.packet;
        for (uint32_t i = 0; i < packet.length && !failed; ++i) {
            usb_command c;
            int result = usb_parser_feed(&parser, packet.bytes[i], &c);
            if (!result) continue;
            if (pending) {
                /* Pipeline violation: preserve the one outstanding response,
                 * discard this request, and require reconnect. */
                text(cdc, epoch, "ERR BUSY reconnect_required\n");
                failed = 1;
            } else if (result < 0) {
                failed = !text(cdc, epoch, "ERR MALFORMED invalid_or_overlength_line\n");
            } else {
                c.connection = epoch; c.request = ++request;
                if (c.opcode == USB_CMD_CLOSE) {
                    text(cdc, epoch, "OK CLOSE\n"); failed = 1;
                } else if (c.opcode == USB_CMD_LOG_READ || c.opcode == USB_CMD_LIVE ||
                           !usb_backend_is_ready()) {
                    failed = !local_command(cdc, &c);
                } else if (tx_queue_send(&usb_commands, &c, TX_NO_WAIT) != TX_SUCCESS) {
                    ++usb_command_drops;
                    failed = !text(cdc, epoch, "ERR BUSY command_queue_full\n");
                } else {
                    active = c; pending = 1;
                    deadline = tx_time_get() + 3 * TX_TIMER_TICKS_PER_SECOND;
                }
            }
        }
        if (failed) {
            usb_connection_changed(); epoch = usb_connection; pending = 0;
            usb_device_abort(cdc, UX_SLAVE_CLASS_CDC_ACM_ENDPOINT_RCV);
        }
    }
}
