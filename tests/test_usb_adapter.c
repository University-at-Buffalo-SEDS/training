/* Run the actual adapter loop against deterministic ThreadX/USBX doubles. */
#include "usb_device.h"
#include "T-Threads.h"
#include "main.h"
#include "sensor_live.h"
#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

static jmp_buf finished;
static ULONG tick;
static unsigned sleeps, writes, mode, aborts, stale_tokens = 2;
static uint8_t output[4096];
static size_t output_size;
static UX_SYSTEM system_instance = {{UX_DEVICE_CONFIGURED}};
UX_SYSTEM *_ux_system_slave = &system_instance;
static UX_SLAVE_ENDPOINT in = {{0x81}, NULL, {0}};
static UX_SLAVE_ENDPOINT out = {{0x01}, &in, {0}};
static UX_INTERFACE interface = {&out};
static UX_SLAVE_CLASS_CDC_ACM cdc = {&interface, 1};
PCD_HandleTypeDef hpcd_USB_FS;
TX_QUEUE usb_rx_packets;
static ULONG rx_memory[8 * 16];
sensor_metadata sensors_metadata;

UINT tx_queue_create(TX_QUEUE *q, char *name, UINT words, VOID *memory, ULONG bytes)
{
    (void)name; assert(words <= 16);
    *q = (TX_QUEUE){.words = words, .depth = bytes / (words * 4), .storage = memory};
    return 0;
}
UINT tx_queue_delete(TX_QUEUE *q) { (void)q; return 0; }
UINT tx_queue_send(TX_QUEUE *q, VOID *p, ULONG wait)
{
    (void)wait;
    if (q->used == q->depth) return TX_QUEUE_FULL;
    memcpy(q->storage + q->tail * q->words, p, q->words * 4);
    q->tail = (q->tail + 1) % q->depth; ++q->used;
    if (q == &usb_commands && mode != 4) {
        usb_command *c = p;
        if (c->opcode == USB_CMD_META) {
            uint8_t metadata[256]={0}; memcpy(metadata,"DLH1",4);
            assert(usb_reply(c, USB_REPLY_DATA, 1, metadata, sizeof(metadata)) == TX_SUCCESS);
        } else {
            assert(c->opcode == USB_CMD_READ);
            const uint8_t payload[] = {0, '\n', 0xff, 'O', 'K'};
            assert(usb_reply(c, USB_REPLY_DATA, 1, payload, sizeof(payload)) == TX_SUCCESS);
        }
    }
    return TX_SUCCESS;
}
UINT tx_queue_receive(TX_QUEUE *q, VOID *p, ULONG wait)
{
    (void)wait;
    if (!q->used) return TX_QUEUE_EMPTY;
    memcpy(p, q->storage + q->head * q->words, q->words * 4);
    q->head = (q->head + 1) % q->depth; --q->used;
    return TX_SUCCESS;
}
UINT tx_thread_create(TX_THREAD *a, char *b, VOID (*c)(ULONG), ULONG d, VOID *e,
                      ULONG f, UINT g, UINT h, ULONG i, UINT j)
{
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    (void)g; (void)h; (void)i; (void)j; return 0;
}
UINT tx_thread_sleep(ULONG n)
{
    tick += n;
    if (++sleeps > 400) longjmp(finished, 1);
    return 0;
}
ULONG tx_time_get(void) { return tick; }
UINT usb_rx_init(void) { return 0; }
int HAL_PCDEx_PMAConfig(PCD_HandleTypeDef *a, unsigned b, unsigned c, unsigned d)
{ (void)a; (void)b; (void)c; (void)d; return 0; }
int HAL_PCD_Start(PCD_HandleTypeDef *a) { (void)a; return 0; }
uint32_t HAL_GetTick(void) { return tick * 10; }
uint32_t HAL_GetUIDw0(void) { return 1; }
uint32_t HAL_GetUIDw1(void) { return 2; }
uint32_t HAL_GetUIDw2(void) { return 3; }
void Error_Handler(void) { abort(); }
UINT ux_dcd_stm32_initialize(ULONG a, ULONG b) { (void)a; (void)b; return 0; }
UINT ux_utility_semaphore_get(int *a, ULONG b)
{
    (void)a; (void)b;
    if (stale_tokens) { --stale_tokens; return UX_SUCCESS; }
    return UX_ERROR;
}
UINT ux_device_class_cdc_acm_ioctl(UX_SLAVE_CLASS_CDC_ACM *a, ULONG b, VOID *c)
{
    (void)a; (void)c;
    if (b == UX_SLAVE_CLASS_CDC_ACM_IOCTL_ABORT_PIPE) ++aborts;
    return 0;
}
UINT ux_device_class_cdc_acm_write(UX_SLAVE_CLASS_CDC_ACM *a, UCHAR *p, ULONG n, ULONG *actual)
{
    (void)a; ++writes;
    assert(!stale_tokens); /* Sole writer removed stale abort completions. */
    if (mode == 3) { *actual = 0; return UX_ERROR; }
    assert(output_size + n < sizeof(output));
    memcpy(output + output_size, p, n); output_size += n;
    *actual = n;
    if (mode == 2) usb_connection_changed(); /* Disconnect after DATA header. */
    return 0;
}

static void run(unsigned scenario, const char *request)
{
    mode = scenario; sleeps = writes = output_size = aborts = tick = 0;
    stale_tokens = 2;
    memset(output, 0, sizeof(output));
    usb_backend_set_ready(scenario != 0);
    usb_rx_packet packet = {.connection = usb_connection, .packet = 1};
    packet.length = strlen(request);
    assert(packet.length <= sizeof(packet.bytes));
    memcpy(packet.bytes, request, packet.length);
    assert(tx_queue_send(&usb_rx_packets, &packet, 0) == TX_SUCCESS);
    if (!setjmp(finished)) usb_thread_entry(0);
}

int main(void)
{
    assert(usb_service_init() == 0);
    assert(tx_queue_create(&usb_rx_packets, "RX", 16, rx_memory, sizeof(rx_memory)) == 0);
    USBD_CDC_ACM_Activate(&cdc);
    uint32_t opening_epoch = usb_connection;
    USBD_CDC_ACM_ParameterChange(&cdc); /* Repeated settings / RTS-only change. */
    assert(usb_connection == opening_epoch && aborts == 0);
    cdc.ux_slave_class_cdc_acm_data_dtr_state = 0;
    USBD_CDC_ACM_ParameterChange(&cdc);
    assert(usb_connection != opening_epoch && aborts == 2);
    opening_epoch = usb_connection;
    USBD_CDC_ACM_ParameterChange(&cdc);
    assert(usb_connection == opening_epoch && aborts == 2);
    cdc.ux_slave_class_cdc_acm_data_dtr_state = 1;
    USBD_CDC_ACM_ParameterChange(&cdc);
    assert(usb_connection != opening_epoch && aborts == 4);
    run(0, "STATUS\nSTART\n");
    assert(strstr((char *)output, "OK STATUS protocol=1"));
    assert(strstr((char *)output, "state=NOT_READY"));
    assert(strstr((char *)output, "ERR NOT_READY"));
    run(0, "LOG_READ 0 512\n");
    assert(!memcmp(output, "LOG ", 4));
    assert(strstr((char *)output, "USB adapter ready\n"));
    run(1, "READ 7 80 5\n");
    const uint8_t expected[] = {0, '\n', 0xff, 'O', 'K', '\n'};
    assert(!memcmp(output, "DATA 7 80 5 ", 12));
    assert(output_size > sizeof(expected));
    assert(!memcmp(output + output_size - sizeof(expected), expected, sizeof(expected)));
    run(1, "META 7\n");
    assert(!memcmp(output, "META 7 256 ", 11));
    uint8_t *metadata_body=(uint8_t *)strchr((char *)output,'\n')+1;
    assert(!memcmp(metadata_body,"DLH1",4));
    assert(output_size==(size_t)(metadata_body-output)+257 && metadata_body[256]=='\n');
    uint32_t epoch = usb_connection;
    run(2, "READ 7 80 5\n");
    assert(writes == 1 && usb_connection != epoch && aborts);
    run(3, "READ 7 80 5\n");
    assert(writes == 1 && aborts && output_size == 0);
    epoch = usb_connection;
    run(4, "READ 7 80 5\n");
    assert(strstr((char *)output, "ERR TIMEOUT reconnect_required\n"));
    assert(usb_connection != epoch);
    run(0, "STATUS bad\n");
    assert(strstr((char *)output, "ERR MALFORMED"));
    run(0, "LIVE\n");
    assert(strstr((char *)output, "ERR NOT_READY sensors_initializing"));
    logger_record sample;
    sensor_raw raw = {0};
    record_raw(&sample, 42, 100000, &raw);
    sensor_live_publish(&sample);
    for (unsigned ready = 0; ready < 2; ++ready) {
        run(ready, "LIVE\n");
        assert(!memcmp(output, "LIVE 1 128 ", 11));
        uint8_t *body = (uint8_t *)strchr((char *)output, '\n') + 1;
        assert(output_size == (size_t)(body - output) + 129);
        assert(!memcmp(body, &sample, 64) && body[64] == 2 && body[128] == '\n');
    }
    /* Failed/disconnected transfers returned all response chunks. */
    usb_command c = {.connection = usb_connection};
    assert(usb_reply(&c, USB_REPLY_TEXT, 1, "OK A\n", 5) == 0);
    assert(usb_reply(&c, USB_REPLY_TEXT, 1, "OK B\n", 5) == 0);
    return 0;
}
