#include "storage_model.h"
#include "storage_service.h"
#include "acquisition.h"
#include "usb_service.h"
#include <stdio.h>

TX_QUEUE records_to_data;
sensor_metadata sensors_metadata={.version=1,.accel_id=0x1e,.gyro_id=0x0f,.baro_id=0x60,
    .accel_conf=0xa8,.accel_range=3,.gyro_bw=4,.baro_osr=3,.baro_odr=3,.baro_iir=4};
static ULONG record_memory[256*16], ticks;
static acquisition_status acquisition;
static logger_record live;
static uint32_t live_age;
static unsigned request_id;
static struct { uint8_t bytes[8192]; size_t length; unsigned kind, final; } result;

uint32_t HAL_GetTick(void) { return ticks*1000/TX_TIMER_TICKS_PER_SECOND; }
uint32_t HAL_GetUIDw0(void) { return 0x11223344; }
uint32_t HAL_GetUIDw1(void) { return 0x55667788; }
uint32_t HAL_GetUIDw2(void) { return 0x99aabbcc; }
ULONG tx_time_get(void) { return ticks; }
UINT tx_queue_create(TX_QUEUE *q, char *name, UINT words, VOID *memory, ULONG bytes)
{
    (void)name; *q=(TX_QUEUE){.words=words,.depth=bytes/(words*4),.storage=memory}; return TX_SUCCESS;
}
UINT tx_queue_delete(TX_QUEUE *q) { q->used=0; return TX_SUCCESS; }
UINT tx_queue_send(TX_QUEUE *q, VOID *p, ULONG wait)
{
    assert(wait==TX_NO_WAIT); if (q->used==q->depth) return TX_QUEUE_FULL;
    memcpy(q->storage+q->tail*q->words,p,q->words*4);
    q->tail=(q->tail+1)%q->depth; ++q->used; return TX_SUCCESS;
}
UINT tx_queue_receive(TX_QUEUE *q, VOID *p, ULONG wait)
{
    (void)wait; if (!q->used) return TX_QUEUE_EMPTY;
    memcpy(p,q->storage+q->head*q->words,q->words*4);
    q->head=(q->head+1)%q->depth; --q->used; return TX_SUCCESS;
}
UINT tx_queue_info_get(TX_QUEUE *q, char **name, ULONG *enqueued, ULONG *space,
                       TX_THREAD **first, ULONG *suspended, TX_QUEUE **next)
{
    (void)name; (void)space; (void)first; (void)suspended; (void)next;
    *enqueued=q->used; return TX_SUCCESS;
}
void acquisition_get_status(acquisition_status *out) { *out=acquisition; }
int acquisition_stopped(void)
{ return !acquisition.running && !acquisition.sensor_busy && acquisition.accepted==acquisition.processed; }
UINT acquisition_start(uint32_t id, float reference)
{
    assert(reference==100000);
    if (!acquisition_stopped()) return TX_NOT_AVAILABLE;
    acquisition.running=1; acquisition.session=id;
    acquisition.accepted=acquisition.processed=0; return TX_SUCCESS;
}
void acquisition_stop(void) { acquisition.running=0; }
int sensor_live_raw(logger_record *raw, uint32_t *age)
{ *raw=live; *age=live_age; return 1; }
static void consume(void)
{
    usb_response r;
    while (tx_queue_receive(&usb_responses,&r,TX_NO_WAIT)==TX_SUCCESS) {
        const uint8_t *data=usb_response_data(&r); assert(data);
        assert(result.length+r.length<sizeof(result.bytes));
        memcpy(result.bytes+result.length,data,r.length); result.length+=r.length;
        result.kind=r.kind; result.final=r.final; usb_response_release(&r);
    }
    result.bytes[result.length]=0;
}
static void enqueue(const char *text)
{
    usb_command c; assert(usb_parse_command(text,&c));
    c.connection=usb_connection; c.request=++request_id;
    assert(tx_queue_send(&usb_commands,&c,TX_NO_WAIT)==TX_SUCCESS);
    memset(&result,0,sizeof(result));
}
static void request(const char *text)
{
    enqueue(text);
    for (unsigned i=0; i<1000 && !result.final; ++i) { storage_service_step(); consume(); ++ticks; }
    assert(result.final);
}
static void start_over(void)
{
    memset(media,255,sizeof(media)); cut_after=-1;
    assert(storage_service_init(&model_io)==STORE_OK); storage_service_step();
    assert(acquisition.running && acquisition.session==1);
}
static void feed(unsigned sequence, int acceleration)
{
    sensor_raw raw={.flags=255,.temperature_adc=819200,.pressure_adc=1,.accel={0,0,0}};
    raw.accel[2]=(int16_t)acceleration;
    logger_record r; record_raw(&r,sequence,(uint64_t)sequence*10000,&raw);
    assert(tx_queue_send(&records_to_data,&r,TX_NO_WAIT)==TX_SUCCESS);
    ++acquisition.accepted; ++acquisition.processed;
    ticks+=TX_TIMER_TICKS_PER_SECOND/100;
    storage_service_step(); consume();
}

int main(void)
{
    (void)saved_media;
    assert(usb_service_init()==TX_SUCCESS);
    assert(tx_queue_create(&records_to_data,"Records",16,record_memory,sizeof(record_memory))==TX_SUCCESS);
    acquisition=(acquisition_status){.initialized=1,.devices=15};
    /* P5-only analytic model yields 100000 Pa at 25 C. */
    sensors_metadata.trim[2]=0; sensors_metadata.trim[3]=128;
    sensors_metadata.trim[5]=0; sensors_metadata.trim[6]=64;
    sensors_metadata.trim[7]=0; sensors_metadata.trim[8]=64;
    sensors_metadata.trim[11]=0xd4; sensors_metadata.trim[12]=0x30;
    sensor_raw raw={.flags=255,.temperature_adc=819200,.pressure_adc=1};
    record_raw(&live,1,0,&raw);
    start_over();
    request("STATUS");
    assert(strstr((char *)result.bytes,"state=LOGGING") && strstr((char *)result.bytes,"metadata=1"));
    logger_record r=live, processed; sensor_processed values;
    record_process(&processed,&r,&sensors_metadata,100000,&values);
    acquisition.accepted=1; acquisition.processed=0;
    assert(tx_queue_send(&records_to_data,&r,TX_NO_WAIT)==TX_SUCCESS);
    enqueue("STOP"); storage_service_step(); consume(); assert(!result.final);
    assert(!acquisition.running && storage_service_state()->sessions[0].records==0);
    assert(tx_queue_send(&records_to_data,&processed,TX_NO_WAIT)==TX_SUCCESS);
    acquisition.processed=1;
    for (unsigned i=0; i<10 && !result.final; ++i) { storage_service_step(); consume(); }
    assert(result.final && !strcmp((char *)result.bytes,"OK STOP\n"));
    request("LIST"); assert(!strcmp((char *)result.bytes,"OK SESSION 1 1 160 0\nOK END\n"));
    request("META 1"); assert(result.kind==USB_REPLY_DATA && result.length==256);
    assert(!memcmp(result.bytes,"DLH1",4) && model_u32(result.bytes+8)==1);
    request("READ 1 73 80"); assert(result.kind==USB_REPLY_DATA && result.length==80);
    request("ERASE_PREPARE 1"); assert(strstr((char *)result.bytes,"ERR PINNED"));
    request("UNPIN 1"); assert(!strcmp((char *)result.bytes,"OK UNPIN 1\n"));
    request("ERASE_PREPARE 1"); assert(strstr((char *)result.bytes,"OK ERASE_PREPARE"));
    request("READ 1 0 80"); assert(result.kind==USB_REPLY_DATA);
    request("ERASE_PREPARE 1"); assert(strstr((char *)result.bytes,"ERR PINNED"));
    ticks+=31*TX_TIMER_TICKS_PER_SECOND;
    request("ERASE_PREPARE 1"); assert(strstr((char *)result.bytes,"OK ERASE_PREPARE"));
    /* Disconnect releases export pins, and never stops ongoing recording. */
    usb_connection_changed(); storage_service_step();
    request("ERASE_PREPARE 1");
    unsigned id, generation, token;
    assert(sscanf((char *)result.bytes,"OK ERASE_PREPARE %u %u %u",&id,&generation,&token)==3);
    char confirm[128]; snprintf(confirm,sizeof(confirm),"ERASE_CONFIRM %u %u %u",id,generation,token);
    usb_connection_changed(); storage_service_step(); request(confirm);
    assert(strstr((char *)result.bytes,"ERR TOKEN"));
    request("ERASE_PREPARE 1");
    assert(sscanf((char *)result.bytes,"OK ERASE_PREPARE %u %u %u",&id,&generation,&token)==3);
    ticks+=31*TX_TIMER_TICKS_PER_SECOND;
    snprintf(confirm,sizeof(confirm),"ERASE_CONFIRM %u %u %u",id,generation,token);
    request(confirm); assert(strstr((char *)result.bytes,"ERR TOKEN"));
    request("ERASE_PREPARE 1");
    assert(sscanf((char *)result.bytes,"OK ERASE_PREPARE %u %u %u",&id,&generation,&token)==3);
    snprintf(confirm,sizeof(confirm),"ERASE_CONFIRM %u %u %u",id,generation,token);
    request(confirm); assert(!strcmp((char *)result.bytes,"OK ERASE_CONFIRM 1\n"));
    unsigned before=mutations;
    request(confirm); assert(!strcmp((char *)result.bytes,"OK ERASE_CONFIRM 1\n") && mutations==before);
    request("OP_STATUS 1"); assert(strstr((char *)result.bytes,"state=DONE"));
    request("LIST"); assert(!strcmp((char *)result.bytes,"OK END\n"));
    request("START"); assert(strstr((char *)result.bytes,"OK START 2"));
    usb_connection_changed(); storage_service_step(); assert(acquisition.running);

    /* A slow host holding both USB chunks must not stop record persistence. */
    usb_command fake={.connection=usb_connection};
    assert(usb_reply(&fake,USB_REPLY_TEXT,1,"OK A\n",5)==TX_SUCCESS);
    assert(usb_reply(&fake,USB_REPLY_TEXT,1,"OK B\n",5)==TX_SUCCESS);
    enqueue("STATUS");
    for (unsigned i=0; i<20; ++i) {
        assert(tx_queue_send(&records_to_data,&r,TX_NO_WAIT)==TX_SUCCESS);
        storage_service_step();
    }
    assert(storage_find((storage *)storage_service_state(),2)->records==32);
    consume(); memset(&result,0,sizeof(result)); storage_service_step(); consume();
    assert(result.final && strstr((char *)result.bytes,"OK STATUS"));

    /* SPI failure stops producers and preserves the open journal for recovery. */
    cut_after=0;
    for (unsigned i=0;i<4;++i) assert(tx_queue_send(&records_to_data,&r,TX_NO_WAIT)==TX_SUCCESS);
    storage_service_step(); cut_after=-1;
    assert(!acquisition.running && storage_service_state()->fault);
    request("STATUS"); assert(strstr((char *)result.bytes,"state=FAULT"));
    request("STOP"); assert(strstr((char *)result.bytes,"ERR STORAGE"));
    assert(storage_service_init(&model_io)==STORE_OK);
    storage_service_step(); assert(acquisition.running && acquisition.session==3);
    request("STOP");
    request("LIST"); assert(strstr((char *)result.bytes,"OK SESSION 2 2 2560 1"));

    /* Entirely unknown media stays unchanged and is reported through STATUS. */
    acquisition_stop(); memset(media,255,sizeof(media)); media[123456]=0;
    before=mutations;
    assert(storage_service_init(&model_io)==STORE_CORRUPT && mutations==before);
    request("STATUS"); assert(strstr((char *)result.bytes,"state=FAULT"));
    request("READ 1 0 80"); assert(strstr((char *)result.bytes,"ERR STATE"));
    request("LIST"); assert(!strcmp((char *)result.bytes,"OK END\n"));
    /* Boot waits for required sensors and a recent pressure, then starts once. */
    memset(media,255,sizeof(media)); acquisition.devices=3;
    assert(storage_service_init(&model_io)==STORE_OK); storage_service_step();
    assert(!acquisition.running && storage_service_state()->active==UINT32_MAX);
    request("STATUS"); assert(strstr((char *)result.bytes,"state=WAITING_SENSORS"));
    acquisition.devices=15; live_age=500;
    storage_service_step(); assert(!acquisition.running);
    live_age=0; storage_service_step(); assert(acquisition.running);
    acquisition.accepted=1; acquisition.processed=0;
    enqueue("STOP"); storage_service_step(); consume(); assert(!result.final);
    ticks+=2*TX_TIMER_TICKS_PER_SECOND+1;
    storage_service_step(); consume(); assert(result.final && strstr((char *)result.bytes,"ERR STORAGE"));
    acquisition.processed=1;
    request("START"); assert(strstr((char *)result.bytes,"ERR STATE"));
    request("STATUS"); assert(strstr((char *)result.bytes,"state=FAULT"));
    /* End-to-end launch mode: full 20-second history and 300-second future,
     * coalesced landing spike, then rolling overwrite without losing it. */
    start_over();
    for (unsigned i=0;i<2600;++i) feed(i,1365); /* about 1 g */
    for (unsigned i=2600;i<=2620;++i) feed(i,0);
    request("STATUS"); assert(strstr((char *)result.bytes,"state=CAPTURING"));
    request("STOP"); assert(strstr((char *)result.bytes,"ERR CAPTURING") && acquisition.running);
    request("EVENTS"); assert(strstr((char *)result.bytes,"CAPTURING"));
    for (unsigned i=2621;i<=2630;++i) feed(i,4096); /* 3 g */
    for (unsigned i=2631;i<=32611;++i) feed(i,1365);
    request("EVENTS");
    assert(strstr((char *)result.bytes,"OK EVENT 2 2 5120160 0 3 26100 20000 300000 CLOSED"));
    request("READ 2 0 160");
    assert(result.kind==USB_REPLY_DATA && model_u32(result.bytes+12)==610 && model_u32(result.bytes+16)==0);
    request("STOP"); assert(!strcmp((char *)result.bytes,"OK STOP\n"));
    request("START"); assert(acquisition.running);
    for (unsigned i=0;i<150000;++i) feed(i,1365);
    assert(acquisition.running && !storage_service_state()->fault);
    assert(storage_used_sectors(storage_service_state())<=1024);
    request("EVENTS"); assert(strstr((char *)result.bytes,"OK EVENT 2 2 5120160 0 3"));
    request("READ 2 0 160"); assert(model_u32(result.bytes+12)==610);
    puts("Storage service boot logging, USB commands, leases, tokens and fault tests passed.");
    return 0;
}
