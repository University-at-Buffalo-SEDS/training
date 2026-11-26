#include "storage_model.h"
#include "compact_record.h"
#include "usb_protocol.h"
#include <stdio.h>

static storage disk, reboot, snapshot;
static sensor_metadata metadata={.version=1,.accel_id=0x1e,.gyro_id=0x0f,.baro_id=0x60,
    .accel_conf=0xa8,.accel_range=3,.gyro_bw=4,.baro_osr=3,.baro_odr=3,.baro_iir=4};
static logger_record sample(uint32_t sequence)
{
    logger_record r; sensor_raw raw={.flags=255,.accel={-1234,30000,-32768},.gyro={1,-2,32767},
        .pressure_adc=0x123456,.temperature_adc=819200,.baro_age_us=12345,.errors=2};
    record_raw(&r,sequence,(uint64_t)sequence*10000,&raw); return r;
}
int main(void)
{
    metadata.trim[3]=128; metadata.trim[6]=64; metadata.trim[8]=64;
    metadata.trim[11]=0xd4; metadata.trim[12]=0x30;
    uint8_t page[256]; compact_page_init(page,7);
    for (unsigned i=0;i<8;++i) {
        logger_record a=sample(100+i*2), b;
        assert(compact_page_add(page,&a,i==3 ? LAUNCH_FREEFALL : 0));
        assert(compact_page_raw(page,i,&b) && !memcmp(&a,&b,sizeof(a)));
    }
    compact_page_seal(page); memset(page+252,0,4);
    assert(compact_page_valid(page,7) && page[6]==LAUNCH_FREEFALL && page[7]==3);
    page[100]^=1; assert(!compact_page_valid(page,7));

    memset(media,255,sizeof(media)); assert(!storage_mount(&disk,&model_io));
    uint32_t first, event;
    assert(!storage_start_compact(&disk,&metadata,100000,0,0,256,&first));
    for (unsigned i=0;i<2000;++i) { logger_record r=sample(i); assert(!storage_append(&disk,&r)); }
    assert(!storage_close(&disk,0));
    assert(storage_find(&disk,first)->sectors==16);
    assert(!storage_start_compact(&disk,&metadata,100000,first,20000000,256,&event));
    for (unsigned i=2000;i<2500;++i) { logger_record r=sample(i); assert(!storage_append(&disk,&r)); }
    logger_record trigger=sample(2500);
    assert(!storage_trigger(&disk,&trigger,LAUNCH_FREEFALL));
    storage_event *v=storage_event_find(&disk,event);
    assert(v && v->source_count==2 && v->begin_us==5000000 && v->trigger_us==25000000);
    assert(v->target_us==325000000 && v->samples==2001 && v->skip_samples==500);
    unsigned operation;
    assert(storage_reclaim_begin(&disk,first,&operation)==STORE_STATE);
    for (unsigned i=2501;i<=32500;++i) { logger_record r=sample(i); assert(!storage_append(&disk,&r)); }
    assert(!storage_close(&disk,0));
    v=storage_event_find(&disk,event);
    assert(v->samples==32001 && !v->incomplete && v->state==SESSION_CLOSED);
    uint32_t occupied_bytes=storage_used_sectors(&disk)*4096;
    assert(occupied_bytes<1200000); /* Full-rate 320-second file, about 1 MiB. */
    uint8_t data[512];
    assert(!storage_read(&disk,event,0,data,160));
    assert(!memcmp(data,"DLOG",4) && model_u32(data+8)==event && model_u32(data+12)==500);
    assert(model_u32(data+16)==0 && data[5]==1 && data[85]==2);
    assert(model_u32(data+72)==usb_crc32(data,72));
    assert(!storage_read(&disk,event,(v->samples*2-1)*80,data,80));
    assert(model_u32(data+12)==32500 && model_u32(data+16)==320000000);
    assert(!storage_metadata(&disk,event,data));
    assert(model_u32(data+140)==3 && model_u32(data+144)==LAUNCH_FREEFALL);
    assert(model_u32(data+180)==20000000);
    assert(!storage_mount(&reboot,&model_io));
    assert(storage_event_find(&reboot,event)->samples==32001);

    /* A second event shares the last 15 seconds of the first one's post-flight
     * source, rather than copying it. Deleting the first must preserve it. */
    uint32_t second;
    assert(!storage_start_compact(&disk,&metadata,100000,first,325010000,256,&second));
    for (unsigned i=32501;i<33000;++i) { logger_record r=sample(i); assert(!storage_append(&disk,&r)); }
    trigger=sample(33000); assert(!storage_trigger(&disk,&trigger,LAUNCH_ACCEL));
    for (unsigned i=33001;i<33010;++i) { logger_record r=sample(i); assert(!storage_append(&disk,&r)); }
    assert(!storage_close(&disk,1));
    assert(!storage_read(&disk,second,0,data,80) && model_u32(data+12)==31000);
    snapshot=disk; memcpy(saved_media,media,sizeof(media));
    unsigned before=mutations;
    assert(!storage_delete_begin(&disk,event,&operation));
    while (disk.delete_id) assert(!storage_delete_step(&disk));
    unsigned delete_mutations=mutations-before;
    /* Every intent/erase/completion boundary must preserve the shared event,
     * and a durable delete intent must remain resumable after reset. */
    for (unsigned cut=0;cut<=delete_mutations;++cut) {
        disk=snapshot; memcpy(media,saved_media,sizeof(media)); cut_after=(int)cut;
        int result=storage_delete_begin(&disk,event,&operation);
        while (!result && disk.delete_id) result=storage_delete_step(&disk);
        cut_after=-1;
        assert(!storage_mount(&reboot,&model_io));
        while (reboot.delete_id) assert(!storage_delete_step(&reboot));
        assert(storage_event_find(&reboot,second));
        assert(!storage_read(&reboot,second,0,data,80) && model_u32(data+12)==31000);
    }
    disk=reboot;
    assert(!storage_event_find(&disk,event) && storage_find(&disk,event));
    assert(!storage_mount(&reboot,&model_io));
    assert(!storage_event_find(&reboot,event)); /* Deleted trigger marker must not resurrect. */
    assert(!storage_read(&reboot,second,0,data,80) && model_u32(data+12)==31000);
    assert(!storage_delete_begin(&reboot,second,&operation));
    while (reboot.delete_id) assert(!storage_delete_step(&reboot));
    assert(storage_used_sectors(&reboot)==2);
    assert(!storage_mount(&disk,&model_io));
    assert(!storage_event_find(&disk,event) && !storage_event_find(&disk,second));

    /* A committed page trigger survives a torn protection-journal update. */
    assert(!storage_start_compact(&disk,&metadata,100000,0,0,256,&event));
    for (unsigned i=0;i<10;++i) { logger_record r=sample(i); assert(!storage_append(&disk,&r)); }
    trigger=sample(10); snapshot=disk; memcpy(saved_media,media,sizeof(media));
    for (unsigned cut=0;cut<=20;++cut) {
        disk=snapshot; memcpy(media,saved_media,sizeof(media)); cut_after=(int)cut;
        (void)storage_trigger(&disk,&trigger,LAUNCH_BARO); cut_after=-1;
        assert(!storage_mount(&reboot,&model_io));
        v=storage_event_find(&reboot,event);
        if (cut<2) assert(!v); /* Trigger page itself never committed. */
        else {
            assert(v && v->incomplete && v->samples==11 && v->reasons==LAUNCH_BARO);
            assert(storage_reclaim_begin(&reboot,event,&operation)==STORE_STATE);
        }
    }
    puts("Compact full-rate 20s/300s capture, shared protection and recovery tests passed.");
    return 0;
}
