#include "storage_service.h"
#include "acquisition.h"
#include "sensor_live.h"
#include "usb_service.h"
#include "main.h"
#include "compact_record.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define NO_ACTIVE UINT32_MAX
enum { CMD_SIMPLE, CMD_LIST, CMD_STOP, CMD_START, CMD_EVENTS };
static struct {
    storage disk;
    uint32_t connection, lease_until[STORAGE_MAX_SESSIONS];
    uint32_t token_id, token_generation, token, token_until, token_counter;
    uint32_t confirmed_id, confirmed_generation, confirmed_token, confirmed_operation;
    uint32_t boot_start, starting, stopping, stop_tick, full, forced_fault, storage_drops, last_sequence;
    float reference;
    uint32_t epoch, event_armed, pending_reasons, missed_events;
    uint64_t now_us, last_gap_us, file_gap_us;
    uint32_t have_raw, last_raw_sequence;
    launch_detector detector;
    uint32_t maintenance_tick;
    usb_command command;
    uint32_t pending, mode, list_index, reply_ready, reply_kind, reply_final;
    size_t reply_length;
    uint8_t reply[USB_CHUNK_SIZE];
} service;
static ULONG queued_records(void);

const storage *storage_service_state(void) { return &service.disk; }
int storage_service_debug_last(uint8_t out[80]) { return storage_latest(&service.disk,out); }
static int expired(uint32_t until) { return !until || (LONG)(tx_time_get()-until)>=0; }
static void reply(unsigned kind, int final, size_t size)
{
    service.reply_kind=kind; service.reply_final=final!=0;
    service.reply_length=size; service.reply_ready=1;
}
static void line(const char *format, ...)
{
    va_list args; va_start(args,format);
    int n=vsnprintf((char *)service.reply,sizeof(service.reply),format,args); va_end(args);
    if (n<1 || (size_t)n>=sizeof(service.reply)) {
        memcpy(service.reply,"ERR INTERNAL response_size\n",27); n=27;
    }
    reply(USB_REPLY_TEXT,1,(size_t)n);
}
static void error(int e)
{
    static const char *names[]={"OK","STORAGE","CORRUPT","FULL","STATE","BOUNDS","NOT_FOUND","PARAM"};
    line("ERR %s storage_code=%u\n", (unsigned)e<sizeof(names)/sizeof(names[0]) ? names[e] : "STORAGE",(unsigned)e);
}
static const char *state(void)
{
    if (service.disk.fault || service.forced_fault) return "FAULT";
    if (service.stopping) return "STOPPING";
    if (service.full) return "FULL";
    if (service.starting || service.boot_start) return "WAITING_SENSORS";
    if (service.disk.active!=NO_ACTIVE) {
        storage_event *v=storage_event_find(&service.disk,service.disk.sessions[service.disk.active].id);
        if (v && v->state==SESSION_OPEN) return "CAPTURING";
    }
    if (service.disk.delete_id && service.disk.active==NO_ACTIVE) return "DELETING";
    return service.disk.active==NO_ACTIVE ? "READY" : "LOGGING";
}
static void stop_recording(void)
{
    service.boot_start=service.starting=0;
    acquisition_stop();
    if (!service.stopping && (service.disk.active!=NO_ACTIVE || !acquisition_stopped() || queued_records())) {
        service.stopping=1; service.stop_tick=tx_time_get();
    }
}
static int pressure_reference(float *reference)
{
    acquisition_status a; acquisition_get_status(&a);
    if (!a.initialized || a.devices!=15 || !acquisition_stopped() || queued_records()) return 0;
    logger_record raw, result; uint32_t age;
    sensor_processed values;
    if (!sensor_live_raw(&raw,&age) || age>200) return 0;
    record_process(&result,&raw,&sensors_metadata,0,&values);
    if ((values.flags&12)!=12 || values.baro_age_us>200000) return 0;
    *reference=values.pressure_pa; return 1;
}
static int start_recording(void)
{
    if (service.disk.active!=NO_ACTIVE || service.stopping || service.disk.delete_id) return STORE_STATE;
    if (service.forced_fault || service.disk.fault) return STORE_STATE;
    if (!pressure_reference(&service.reference)) return STORE_STATE;
    uint32_t id;
    int e=storage_start_compact(&service.disk,&sensors_metadata,service.reference,0,0,256,&id);
    if (e==STORE_FULL) e=storage_start_compact(&service.disk,&sensors_metadata,service.reference,0,0,32,&id);
    if (e==STORE_FULL) e=storage_start_compact(&service.disk,&sensors_metadata,service.reference,0,0,4,&id);
    if (!e) {
        service.starting=id; service.epoch=id; service.full=0; service.storage_drops=0; service.last_sequence=0;
        service.now_us=service.last_gap_us=service.file_gap_us=0; service.have_raw=0;
        service.pending_reasons=0;
        launch_detector_reset(&service.detector);
    }
    else if (e==STORE_FULL) service.full=1;
    return e;
}
static void status(void)
{
    acquisition_status a; acquisition_get_status(&a);
    uint32_t durable=0, used=2, physical=8192;
    if (service.disk.mounted) {
        for (unsigned i=0; i<STORAGE_MAX_SESSIONS; ++i) {
            const storage_session *p=&service.disk.sessions[i];
            durable+=p->records;
            if (p->id) physical+=256*(1+(p->format==2 ? p->next_slot : (p->next_slot+2)/3));
        }
        used=storage_used_sectors(&service.disk);
    }
    uint32_t active=service.disk.active==NO_ACTIVE ? 0 : service.disk.sessions[service.disk.active].id;
    unsigned events=0;
    for (unsigned i=0;i<STORAGE_MAX_EVENTS;++i) if (service.disk.events[i].id) ++events;
    storage_event *v=storage_event_find(&service.disk,active);
    uint32_t remaining=v && v->state==SESSION_OPEN && v->target_us>service.now_us ?
        (uint32_t)((v->target_us-service.now_us+999999)/1000000) : 0;
    line("OK STATUS protocol=1 firmware=storage-2 metadata=1 unpin=1 events=1 state=%s active=%lu storage=%s "
         "durable=%lu phys_bytes=%lu used_sectors=%lu free_sectors=%lu proc_drop=%lu raw_drop=%lu processed_drop=%lu "
         "storage_drop=%lu missed=%lu last=%lu fault=%lu cmd_drop=%lu rx_drop=%lu tx_error=%lu "
         "launches=%u armed=%lu remaining_s=%lu missed_launches=%lu uptime_ms=%lu uid=%08lX%08lX%08lX\n",state(),(unsigned long)active,
         service.disk.fault ? "fault" : "ok",(unsigned long)durable,
         (unsigned long)physical,
         (unsigned long)used,(unsigned long)(1024-used),
         (unsigned long)a.processing_drops,(unsigned long)a.raw_log_drops,
         (unsigned long)a.processed_log_drops,(unsigned long)service.storage_drops,
         (unsigned long)a.missed_deadlines,(unsigned long)service.last_sequence,
         (unsigned long)(service.disk.fault ? service.disk.fault : service.forced_fault),
         (unsigned long)usb_command_drops,(unsigned long)usb_rx_drops,(unsigned long)usb_transfer_errors,
         events,(unsigned long)service.event_armed,(unsigned long)remaining,
         (unsigned long)service.missed_events,
         (unsigned long)HAL_GetTick(),(unsigned long)HAL_GetUIDw0(),
         (unsigned long)HAL_GetUIDw1(),(unsigned long)HAL_GetUIDw2());
}
static unsigned session_index(const storage_session *p)
{ return (unsigned)(p-service.disk.sessions); }
static void pin(storage_session *p)
{
    service.lease_until[session_index(p)]=tx_time_get()+30*TX_TIMER_TICKS_PER_SECOND;
    if (!service.lease_until[session_index(p)]) service.lease_until[session_index(p)]=1;
}
static void operation_status(uint32_t operation)
{
    if (!service.disk.mounted || !operation || operation!=service.disk.last_op) { error(STORE_NOT_FOUND); return; }
    static const char *names[]={"NONE","RUNNING","DONE","FAILED"};
    line("OK OP_STATUS %lu state=%s done=%lu total=%lu error=%lu\n",(unsigned long)operation,
         names[service.disk.op_state],(unsigned long)service.disk.delete_progress,
         (unsigned long)service.disk.op_total,(unsigned long)service.disk.op_error);
}
static void command(void)
{
    usb_command *c=&service.command;
    storage_session *p=storage_find(&service.disk,c->id);
    switch (c->opcode) {
    case USB_CMD_STATUS: status(); break;
    case USB_CMD_LIST: service.mode=CMD_LIST; service.list_index=0; break;
    case USB_CMD_EVENTS: service.mode=CMD_EVENTS; service.list_index=0; break;
    case USB_CMD_STOP:
        if (service.disk.active!=NO_ACTIVE && storage_event_find(&service.disk,
            service.disk.sessions[service.disk.active].id) && !service.disk.fault && !service.forced_fault) {
            line("ERR CAPTURING protected_five_minute_capture_in_progress\n"); break;
        }
        stop_recording();
        if (service.disk.fault || service.forced_fault) error(STORE_IO);
        else service.mode=CMD_STOP;
        break;
    case USB_CMD_START:
        service.boot_start=0;
        if (service.disk.fault || service.forced_fault) error(STORE_STATE);
        else if (service.disk.active!=NO_ACTIVE && !service.stopping && !service.starting) {
            line("OK START %lu\n",(unsigned long)service.disk.sessions[service.disk.active].id);
        } else if (service.starting) service.mode=CMD_START;
        else {
            int e=start_recording();
            if (e) {
                if (e==STORE_STATE && !service.disk.fault && !service.stopping && !service.disk.delete_id)
                    line("ERR NOT_READY sensors_or_pressure_unavailable\n");
                else error(e);
            } else service.mode=CMD_START;
        }
        break;
    case USB_CMD_READ:
    case USB_CMD_META: {
        if (!service.disk.mounted) { error(STORE_STATE); break; }
        if (!p) { error(STORE_NOT_FOUND); break; }
        int e=c->opcode==USB_CMD_META ? storage_metadata(&service.disk,c->id,service.reply) :
            storage_read(&service.disk,c->id,c->offset,service.reply,c->count);
        if (e) error(e);
        else { pin(p); reply(USB_REPLY_DATA,1,c->opcode==USB_CMD_META ? 256 : c->count); }
        break;
    }
    case USB_CMD_UNPIN:
        if (!service.disk.mounted) error(STORE_STATE);
        else if (!p) error(STORE_NOT_FOUND);
        else if (p->state!=SESSION_CLOSED) error(STORE_STATE);
        else {
            service.lease_until[session_index(p)]=0;
            line("OK UNPIN %lu\n",(unsigned long)p->id);
        }
        break;
    case USB_CMD_ERASE_PREPARE:
        if (service.disk.active!=NO_ACTIVE || service.disk.delete_id || service.stopping || service.disk.fault)
            error(STORE_STATE);
        else if (!p) error(STORE_NOT_FOUND);
        else if (p->state!=SESSION_CLOSED) error(STORE_STATE);
        else if (!expired(service.lease_until[session_index(p)])) line("ERR PINNED export_lease_active\n");
        else {
            service.token_id=p->id; service.token_generation=p->generation;
            if (++service.token_counter==0) ++service.token_counter;
            service.token=service.token_counter;
            service.token_until=tx_time_get()+30*TX_TIMER_TICKS_PER_SECOND;
            if (!service.token_until) service.token_until=1;
            line("OK ERASE_PREPARE %lu %lu %lu\n",(unsigned long)p->id,
                 (unsigned long)p->generation,(unsigned long)service.token);
        }
        break;
    case USB_CMD_ERASE_CONFIRM:
        if (c->id==service.confirmed_id && c->generation==service.confirmed_generation &&
            c->token==service.confirmed_token && service.confirmed_operation) {
            line("OK ERASE_CONFIRM %lu\n",(unsigned long)service.confirmed_operation);
        } else if (c->id!=service.token_id || c->generation!=service.token_generation ||
                   c->token!=service.token || !c->token || expired(service.token_until))
            line("ERR TOKEN invalid_or_expired_confirmation\n");
        else if (!p || p->generation!=c->generation) error(STORE_NOT_FOUND);
        else if (!expired(service.lease_until[session_index(p)])) line("ERR PINNED export_lease_active\n");
        else {
            uint32_t operation;
            int e=storage_delete_begin(&service.disk,c->id,&operation);
            if (e) error(e);
            else {
                service.confirmed_id=c->id; service.confirmed_generation=c->generation;
                service.confirmed_token=c->token; service.confirmed_operation=operation;
                service.token=0; service.token_until=0;
                line("OK ERASE_CONFIRM %lu\n",(unsigned long)operation);
            }
        }
        break;
    case USB_CMD_OP_STATUS: operation_status(c->id); break;
    default: line("ERR MALFORMED unsupported_storage_command\n"); break;
    }
}
static storage_event *active_event(void)
{
    if (service.disk.active==NO_ACTIVE) return NULL;
    storage_event *v=storage_event_find(&service.disk,service.disk.sessions[service.disk.active].id);
    return v && v->state==SESSION_OPEN ? v : NULL;
}
static int armed(void)
{
    if (service.disk.active==NO_ACTIVE || service.disk.fault || service.forced_fault || service.stopping)
        return 0;
    storage_session *p=&service.disk.sessions[service.disk.active];
    if (p->format!=2 || p->event_id) return 0;
    unsigned free_event=0;
    for (unsigned i=0;i<STORAGE_MAX_EVENTS;++i) if (!service.disk.events[i].id) ++free_event;
    uint32_t used=p->next_slot*8+service.disk.pending_page[5];
    uint32_t capacity=(p->sectors*16-1)*8;
    return free_event && capacity>=used && capacity-used>=30002;
}
static int open_segment(uint64_t time)
{
    uint32_t id;
    int e=storage_start_compact(&service.disk,&sensors_metadata,service.reference,service.epoch,time,256,&id);
    if (e==STORE_FULL) e=storage_start_compact(&service.disk,&sensors_metadata,service.reference,service.epoch,time,32,&id);
    if (e==STORE_FULL) e=storage_start_compact(&service.disk,&sensors_metadata,service.reference,service.epoch,time,4,&id);
    if (!e) service.file_gap_us=service.last_gap_us;
    return e;
}
static int record_sample(const logger_record *raw)
{
    if (raw->bytes[0]!=1) return STORE_OK;
    uint64_t time=record_timestamp(raw);
    uint32_t sequence=(uint32_t)raw->bytes[4] | (uint32_t)raw->bytes[5]<<8 |
        (uint32_t)raw->bytes[6]<<16 | (uint32_t)raw->bytes[7]<<24;
    if (service.have_raw && (sequence-service.last_raw_sequence!=1 || time<service.now_us ||
        time-service.now_us>20000)) service.last_gap_us=time;
    service.now_us=time; service.last_raw_sequence=sequence; service.have_raw=1;
    storage_event *v=active_event();
    if (v && time>v->target_us) {
        int e=storage_close(&service.disk,v->incomplete); if (e) return e;
        printf("LAUNCH complete id=%lu samples=%lu protected=1\n",(unsigned long)v->id,(unsigned long)v->samples);
        service.pending_reasons=0;
        e=open_segment(time); if (e) return e;
        v=NULL;
    }
    logger_record processed; sensor_processed values;
    record_process(&processed,raw,&sensors_metadata,service.reference,&values);
    unsigned reasons=launch_detector_update(&service.detector,time,&values);
    if (v) {
        if (service.last_gap_us && service.last_gap_us>=v->begin_us) v->incomplete=1;
        service.pending_reasons|=reasons&~v->reasons;
    } else if (armed() && reasons) {
        service.pending_reasons=0;
        int e=storage_trigger(&service.disk,raw,reasons); if (e) return e;
        v=active_event();
        if (v && service.last_gap_us && service.last_gap_us>=v->begin_us) v->incomplete=1;
        printf("LAUNCH trigger id=%lu reasons=%u pre=20s post=300s protected=1\n",
               (unsigned long)service.disk.sessions[service.disk.active].id,reasons);
        return STORE_OK;
    } else if (!v && reasons) ++service.missed_events;
    storage_session *p=&service.disk.sessions[service.disk.active];
    if (!v && (time-p->origin_us>=LAUNCH_PRE_US ||
        p->next_slot*8+service.disk.pending_page[5]+8>=(p->sectors*16-1)*8)) {
        int e=storage_close(&service.disk,service.last_gap_us!=service.file_gap_us); if (e) return e;
        e=open_segment(time); if (e) return e;
    }
    return storage_append(&service.disk,raw);
}
static ULONG queued_records(void)
{
    ULONG enqueued=0;
    tx_queue_info_get(&records_to_data,NULL,&enqueued,NULL,NULL,NULL,NULL);
    return enqueued;
}
static void reclaim_history(void)
{
    if (service.disk.active==NO_ACTIVE || service.disk.fault || service.forced_fault ||
        service.stopping || service.disk.delete_id || queued_records()>16 ||
        (ULONG)(tx_time_get()-service.maintenance_tick)<TX_TIMER_TICKS_PER_SECOND) return;
    service.maintenance_tick=tx_time_get();
    unsigned entries=0;
    for (unsigned i=0;i<STORAGE_MAX_SESSIONS;++i) if (service.disk.sessions[i].id) ++entries;
    if (entries<STORAGE_MAX_SESSIONS-2 && storage_can_reserve(&service.disk,256)) return;
    storage_session *oldest=NULL;
    for (unsigned i=0;i<STORAGE_MAX_SESSIONS;++i) {
        storage_session *p=&service.disk.sessions[i];
        if (!p->id || p->format!=2 || p->event_id || p->state!=SESSION_CLOSED ||
            !expired(service.lease_until[i])) continue;
        if (p->epoch==service.epoch && (service.now_us<LAUNCH_PRE_US || p->last_us>=service.now_us-LAUNCH_PRE_US))
            continue;
        if (!oldest || p->id<oldest->id) oldest=p;
    }
    if (oldest) {
        uint32_t operation;
        storage_reclaim_begin(&service.disk,oldest->id,&operation);
    }
}
int storage_service_init(const storage_io *io)
{
    memset(&service,0,sizeof(service)); service.connection=usb_connection;
    service.token_counter=HAL_GetUIDw0()^HAL_GetUIDw1()^HAL_GetTick();
    int e=storage_mount(&service.disk,io);
    service.boot_start=e==STORE_OK;
    usb_backend_set_ready(1); /* Faults still have a real STATUS/diagnostic path. */
    printf("FLASH recovery=%u sessions_next=%lu used_sectors=%lu\n",(unsigned)e,
           (unsigned long)service.disk.next_id,(unsigned long)storage_used_sectors(&service.disk));
    return e;
}
void storage_service_step(void)
{
    if (service.connection!=usb_connection) {
        service.connection=usb_connection; memset(service.lease_until,0,sizeof(service.lease_until));
        service.token=service.token_until=service.confirmed_operation=0;
        service.pending=service.reply_ready=0;
    }
    if (service.disk.fault || service.forced_fault || service.full) stop_recording();
    if (service.boot_start && !service.disk.fault && !service.full && !service.disk.delete_id) {
        if (pressure_reference(&service.reference)) {
            int e=start_recording();
            if (e!=STORE_STATE) service.boot_start=0;
        }
    }
    if (service.starting && !service.stopping && !service.disk.fault) {
        UINT e=acquisition_start(service.starting,service.reference);
        if (e==TX_SUCCESS) {
            printf("FLASH logging session=%lu\n",(unsigned long)service.starting);
            service.starting=0;
        }
    }
    logger_record record;
    for (unsigned i=0; i<8 && tx_queue_receive(&records_to_data,&record,TX_NO_WAIT)==TX_SUCCESS; ++i) {
        if (service.disk.active==NO_ACTIVE || service.disk.fault || service.full || service.forced_fault) {
            ++service.storage_drops; continue;
        }
        if (record.bytes[0]!=1) continue;
        int e=record_sample(&record);
        if (e) {
            ++service.storage_drops;
            if (e==STORE_FULL) service.full=1;
            else if (!service.disk.fault) service.forced_fault=(uint32_t)e;
            stop_recording();
        } else service.last_sequence=(uint32_t)record.bytes[4] | (uint32_t)record.bytes[5]<<8 |
            (uint32_t)record.bytes[6]<<16 | (uint32_t)record.bytes[7]<<24;
    }
    if (service.stopping) {
        ULONG enqueued=0;
        tx_queue_info_get(&records_to_data,NULL,&enqueued,NULL,NULL,NULL,NULL);
        if (acquisition_stopped() && !enqueued) {
            if (!service.disk.fault && !service.forced_fault) {
                acquisition_status a; acquisition_get_status(&a);
                int incomplete=service.full || service.storage_drops || a.raw_log_drops;
                storage_close(&service.disk,incomplete);
            }
            service.stopping=0;
        } else if ((ULONG)(tx_time_get()-service.stop_tick)>=2*TX_TIMER_TICKS_PER_SECOND) {
            service.forced_fault=STORE_STATE; service.stopping=0;
        }
    }
    if (!service.pending) {
        if (tx_queue_receive(&usb_commands,&service.command,TX_NO_WAIT)==TX_SUCCESS &&
            usb_command_is_current(&service.command)) {
            service.pending=1; service.mode=CMD_SIMPLE; command();
        }
    } else if (!service.reply_ready) {
        if (service.mode==CMD_LIST) {
            while (service.disk.mounted && service.list_index<STORAGE_MAX_SESSIONS) {
                storage_session *p=&service.disk.sessions[service.list_index++];
                if (p->id && !p->event_id && p->state==SESSION_CLOSED) {
                    line("OK SESSION %lu %lu %lu %lu\n",(unsigned long)p->id,(unsigned long)p->generation,
                         (unsigned long)(p->records*80),(unsigned long)p->incomplete);
                    service.reply_final=0; break;
                }
            }
            if (!service.reply_ready) line("OK END\n");
        } else if (service.mode==CMD_EVENTS) {
            while (service.disk.mounted && service.list_index<STORAGE_MAX_EVENTS) {
                storage_event *v=&service.disk.events[service.list_index++];
                if (!v->id || v->state==SESSION_DELETING) continue;
                uint32_t pre=(uint32_t)((v->trigger_us-v->begin_us)/1000);
                uint32_t post=v->end_us>v->trigger_us ? (uint32_t)((v->end_us-v->trigger_us)/1000) : 0;
                line("OK EVENT %lu %lu %lu %lu %lu %lu %lu %lu %s\n",(unsigned long)v->id,
                     (unsigned long)v->generation,(unsigned long)(v->samples*160),(unsigned long)v->incomplete,
                     (unsigned long)v->reasons,(unsigned long)(v->trigger_us/1000),(unsigned long)pre,
                     (unsigned long)post,v->state==SESSION_OPEN ? "CAPTURING" : "CLOSED");
                service.reply_final=0; break;
            }
            if (!service.reply_ready) line("OK END\n");
        } else if (service.mode==CMD_STOP && !service.stopping) {
            if (service.disk.fault || service.forced_fault) error(STORE_IO);
            else line("OK STOP\n");
        } else if (service.mode==CMD_START && !service.starting) {
            if (service.disk.fault || service.forced_fault) error(STORE_IO);
            else if (service.disk.active==NO_ACTIVE) error(STORE_STATE);
            else line("OK START %lu\n",(unsigned long)service.disk.sessions[service.disk.active].id);
        }
    }
    if (service.pending && service.reply_ready) {
        UINT e=usb_reply(&service.command,service.reply_kind,service.reply_final,service.reply,service.reply_length);
        if (e==TX_SUCCESS) {
            service.reply_ready=0;
            if (service.reply_final) service.pending=0;
        } else if (e==TX_DELETED) service.pending=service.reply_ready=0;
    }
    service.event_armed=(uint32_t)armed();
    if (service.pending_reasons && active_event() && queued_records()<=16) {
        storage_event_note(&service.disk,service.pending_reasons); service.pending_reasons=0;
    }
    reclaim_history();
    if (service.disk.delete_id && !service.disk.fault && !service.forced_fault && queued_records()<=16) {
        int e=storage_delete_step(&service.disk);
        if (!e && !service.disk.delete_id) service.full=0;
    }
}
