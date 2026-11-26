#include "storage.h"
#include "usb_protocol.h"
#include "compact_record.h"
#include <math.h>
#include <string.h>

#define NO_ACTIVE UINT32_MAX
#define JOURNAL_CRC 4088U
#define JOURNAL_COMMIT 4092U
#define RESERVE_SECTORS 32U
static uint32_t get32(const uint8_t *p)
{ return p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static void put32(uint8_t *p, uint32_t v)
{ for (unsigned i=0; i<4; ++i) p[i]=(uint8_t)(v>>(8*i)); }
static uint64_t get64(const uint8_t *p) { return get32(p) | (uint64_t)get32(p+4)<<32; }
static void put64(uint8_t *p, uint64_t v) { put32(p,(uint32_t)v); put32(p+4,(uint32_t)(v>>32)); }
static int protect_event(storage *s, uint32_t id, uint64_t time, unsigned reasons, int recovered);
static int packed_raw(storage *s, storage_session *p, uint32_t sample, logger_record *raw);
static int packed_read(storage *s, storage_session *p, storage_event *event,
                       uint32_t offset, uint8_t *out, size_t count);
static uint32_t deletion_total(storage *s, storage_session *p);
static int event_references(storage *s, uint32_t source, uint32_t except);
static int blank(const uint8_t *p, size_t n)
{ for (size_t i=0; i<n; ++i) if (p[i]!=255) return 0; return 1; }
static int fail(storage *s, int error) { s->fault=(uint32_t)error; return error; }
static int read_bytes(storage *s, uint32_t address, uint8_t *out, size_t n)
{
    if (s->io.read(s->io.context,address,out,n)) return STORE_IO;
    s->io_bytes+=(uint32_t)n;
    if (s->io_bytes>=16384) {
        s->io_bytes=0;
        if (s->io.yield) s->io.yield(s->io.context);
    }
    return STORE_OK;
}
static int erased(storage *s, uint32_t address, uint32_t n)
{
    while (n) {
        size_t count=n>256 ? 256 : n;
        int e=read_bytes(s,address,s->scratch,count);
        if (e) return e;
        if (!blank(s->scratch,count)) return STORE_CORRUPT;
        address+=(uint32_t)count; n-=(uint32_t)count;
    }
    return STORE_OK;
}
/* Verify body before the separate commit program, then verify the whole object.
 * The fitted NOR supports partial page programs; no byte is programmed twice. */
static int committed_write(storage *s, uint32_t address, uint8_t *bytes, size_t n)
{
    const uint8_t zero[4]={0};
    size_t body=n-4, done=0;
    while (done<body) {
        size_t count=256-((address+(uint32_t)done)&255);
        if (count>body-done) count=body-done;
        if (s->io.program(s->io.context,address+(uint32_t)done,bytes+done,count)) return STORE_IO;
        int e=read_bytes(s,address+(uint32_t)done,s->scratch,count);
        if (e) return e;
        if (memcmp(s->scratch,bytes+done,count)) return STORE_CORRUPT;
        done+=count;
    }
    if (s->io.program(s->io.context,address+(uint32_t)body,zero,4)) return STORE_IO;
    memcpy(bytes+body,zero,4);
    done=0;
    while (done<n) {
        size_t count=n-done>256 ? 256 : n-done;
        int e=read_bytes(s,address+(uint32_t)done,s->scratch,count);
        if (e) return e;
        if (memcmp(bytes+done,s->scratch,count)) return STORE_CORRUPT;
        done+=count;
    }
    return STORE_OK;
}
static int journal_valid(const uint8_t *b)
{
    return ((!memcmp(b,"DLJ1",4) && get32(b+4)==1) ||
            (!memcmp(b,"DLJ2",4) && get32(b+4)==2)) && get32(b+8)!=0 &&
        get32(b+JOURNAL_COMMIT)==0 && get32(b+JOURNAL_CRC)==usb_crc32(b,JOURNAL_CRC);
}
static int save_journal(storage *s)
{
    if (s->journal_generation==UINT32_MAX) return fail(s,STORE_FULL);
    uint8_t *b=s->journal;
    memset(b,0,STORAGE_SECTOR_SIZE); memcpy(b,"DLJ2",4);
    put32(b+4,2); put32(b+8,s->journal_generation+1); put32(b+12,s->next_id);
    put32(b+16,s->delete_id); put32(b+20,s->delete_op); put32(b+24,s->delete_progress);
    put32(b+28,s->last_op); put32(b+32,s->op_state); put32(b+36,s->op_session);
    put32(b+40,s->op_generation); put32(b+44,s->op_error);
    put32(b+48,s->op_total);
    for (unsigned i=0; i<STORAGE_MAX_SESSIONS; ++i) {
        const storage_session *p=&s->sessions[i]; uint8_t *d=b+64+i*32;
        put32(d,p->id); put32(d+4,p->generation); put32(d+8,p->first_sector);
        put32(d+12,p->sectors); put32(d+16,p->records);
        put32(d+20,p->state | (p->format==2 ? 0x200U : 0) | (p->event_suppressed ? 0x10000U : 0));
        put32(d+24,p->incomplete); put32(d+28,p->event_id);
    }
    for (unsigned i=0;i<STORAGE_MAX_EVENTS;++i) {
        const storage_event *e=&s->events[i]; uint8_t *d=b+2112+i*96;
        put32(d,e->id); put32(d+4,e->generation); put32(d+8,e->epoch); put32(d+12,e->reasons);
        put32(d+16,e->state); put32(d+20,e->incomplete); put32(d+24,e->skip_samples);
        put32(d+28,e->samples); put64(d+32,e->begin_us); put64(d+40,e->trigger_us);
        put64(d+48,e->target_us); put64(d+56,e->end_us); put32(d+64,e->source_count);
        for (unsigned k=0;k<STORAGE_EVENT_SOURCES;++k) put32(d+68+k*4,e->sources[k]);
    }
    put32(b+JOURNAL_CRC,usb_crc32(b,JOURNAL_CRC));
    memset(b+JOURNAL_COMMIT,255,4);
    uint32_t target=s->journal_slot^1U;
    if (s->io.erase(s->io.context,target*4096)) return fail(s,STORE_IO);
    int e=committed_write(s,target*4096,b,4096);
    if (e) return fail(s,e);
    s->journal_slot=target; ++s->journal_generation;
    return STORE_OK;
}
storage_session *storage_find(storage *s, uint32_t id)
{
    if (!id) return NULL;
    for (unsigned i=0; i<STORAGE_MAX_SESSIONS; ++i)
        if (s->sessions[i].id==id) return &s->sessions[i];
    return NULL;
}
static uint32_t slot_address(const storage_session *p, uint32_t slot)
{ return p->first_sector*4096+256+(slot/3)*256+(slot%3)*80; }
static unsigned page_state(const storage *s, uint32_t page)
{ return (s->page_state[page/2]>>((page&1U)*4))&15U; }
static void set_page_state(storage *s, uint32_t page, unsigned value)
{
    unsigned shift=(page&1U)*4;
    s->page_state[page/2]=(uint8_t)((s->page_state[page/2]&~(15U<<shift)) | (value<<shift));
}
static int is_valid(const storage *s, uint32_t address)
{ return (page_state(s,address/256)>>((address%256)/80))&1; }
static void mark_valid(storage *s, uint32_t address)
{
    set_page_state(s,address/256,page_state(s,address/256) | (1U<<((address%256)/80)));
    ++s->sector_records[address/4096];
}
static int record_valid(const uint8_t *b, uint32_t id)
{
    if (memcmp(b,"DLOG",4) || b[4]!=1 || (b[5]!=1 && b[5]!=2) ||
        b[6]!=(b[5]==1 ? 32 : 48) || b[7] || get32(b+8)!=id ||
        get32(b+76)!=0 || get32(b+72)!=usb_crc32(b,72)) return 0;
    if (b[5]==1) for (unsigned i=56; i<72; ++i) if (b[i]) return 0;
    return 1;
}
static int header_valid(const uint8_t *b, const storage_session *p)
{
    float reference; uint32_t bits=get32(b+16); memcpy(&reference,&bits,4);
    return !memcmp(b,"DLH1",4) && get32(b+4)==1 && get32(b+8)==p->id &&
        get32(b+12)==p->generation && isfinite(reference) && reference>0 &&
        get32(b+252)==0 && get32(b+248)==usb_crc32(b,248);
}
static int occupied(const storage *s, uint32_t sector)
{
    if (sector<2 || sector>=STORAGE_SECTORS) return 1;
    for (unsigned i=0; i<STORAGE_MAX_SESSIONS; ++i) {
        const storage_session *p=&s->sessions[i];
        if (p->id && sector>=p->first_sector && sector-p->first_sector<p->sectors) return 1;
    }
    return 0;
}
static uint32_t free_extent(const storage *s, uint32_t first)
{
    uint32_t count=0;
    while (count<RESERVE_SECTORS && !occupied(s,first+count)) ++count;
    return count;
}
static int trim_tail(storage *s, storage_session *p)
{
    uint32_t pages=p->format==2 ? p->next_slot : (p->next_slot+2)/3;
    uint32_t needed=(256+pages*256+4095)/4096;
    if (needed<p->sectors) {
        if (p->format!=2) {
            int e=erased(s,(p->first_sector+needed)*4096,(p->sectors-needed)*4096);
            if (e) return e;
        } /* Packed pages are checked before use; untouched reservations can
           * be released without erasing or trusting their unknown contents. */
        p->sectors=needed;
    }
    return STORE_OK;
}
static int mount_impl(storage *s)
{
    int e=read_bytes(s,0,s->journal,4096); if (e) return e;
    int a=journal_valid(s->journal); uint32_t ga=a ? get32(s->journal+8) : 0;
    uint32_t ca=get32(s->journal+JOURNAL_CRC);
    e=read_bytes(s,4096,s->journal,4096); if (e) return e;
    int b=journal_valid(s->journal); uint32_t gb=b ? get32(s->journal+8) : 0;
    if (!a && !b) {
        /* Blank metadata alone does not authorize overwriting unknown logs. */
        e=erased(s,0,STORAGE_SECTORS*4096); if (e) return e;
        s->next_id=1; s->journal_slot=1; s->mounted=1;
        return save_journal(s);
    }
    if (a && b && ga==gb && ca!=get32(s->journal+JOURNAL_CRC)) return STORE_CORRUPT;
    s->journal_slot=(b && gb>=ga) ? 1 : 0;
    if (!s->journal_slot) { e=read_bytes(s,0,s->journal,4096); if (e) return e; }
    uint8_t *j=s->journal;
    s->journal_generation=get32(j+8); s->next_id=get32(j+12);
    s->delete_id=get32(j+16); s->delete_op=get32(j+20); s->delete_progress=get32(j+24);
    s->last_op=get32(j+28); s->op_state=get32(j+32); s->op_session=get32(j+36);
    s->op_generation=get32(j+40); s->op_error=get32(j+44);
    s->op_total=get32(j+48);
    if (!s->next_id || s->op_state>STORE_OP_FAILED) return STORE_CORRUPT;
    for (unsigned i=0; i<STORAGE_MAX_SESSIONS; ++i) {
        storage_session *p=&s->sessions[i]; const uint8_t *d=j+64+i*32;
        p->id=get32(d); p->generation=get32(d+4); p->first_sector=get32(d+8);
        p->sectors=get32(d+12); p->records=get32(d+16); p->state=get32(d+20)&255;
        p->format=(get32(d+20)>>8)&255; if (!p->format) p->format=1;
        p->event_suppressed=(get32(d+20)>>16)&1U;
        p->incomplete=get32(d+24); p->event_id=get32(d+28);
        if (!p->id) {
            if (p->generation || p->first_sector || p->sectors || p->records || get32(d+20) || p->incomplete || p->event_id)
                return STORE_CORRUPT;
            continue;
        }
        if (p->id>=s->next_id || !p->generation || p->first_sector<2 ||
            p->first_sector>=STORAGE_SECTORS || !p->sectors ||
            p->sectors>STORAGE_SECTORS-p->first_sector ||
            (p->format!=1 && p->format!=2) || (get32(d+20)&0xfffe0000U) ||
            (p->format==2 && (p->records&1U)) ||
            p->records>(p->sectors*16-1)*(p->format==2 ? 16U : 3U) || p->state<SESSION_OPEN ||
            p->state>SESSION_DELETING || p->incomplete>1) return STORE_CORRUPT;
        for (unsigned k=0; k<i; ++k) {
            storage_session *q=&s->sessions[k];
            if (q->id && (p->id==q->id ||
                (p->first_sector<q->first_sector+q->sectors && q->first_sector<p->first_sector+p->sectors)))
                return STORE_CORRUPT;
        }
    }
    if (get32(j+4)==2) for (unsigned i=0;i<STORAGE_MAX_EVENTS;++i) {
        storage_event *v=&s->events[i]; const uint8_t *d=j+2112+i*96;
        v->id=get32(d); if (!v->id) continue;
        v->generation=get32(d+4); v->epoch=get32(d+8); v->reasons=get32(d+12);
        v->state=get32(d+16); v->incomplete=get32(d+20); v->skip_samples=get32(d+24);
        v->samples=get32(d+28); v->begin_us=get64(d+32); v->trigger_us=get64(d+40);
        v->target_us=get64(d+48); v->end_us=get64(d+56); v->source_count=get32(d+64);
        if (!v->generation || !v->epoch || !v->reasons || v->reasons>7 || v->source_count>STORAGE_EVENT_SOURCES ||
            !v->source_count || v->incomplete>1 || v->state<SESSION_OPEN || v->state>SESSION_DELETING ||
            v->begin_us>v->trigger_us || v->target_us<v->trigger_us ||
            v->target_us-v->trigger_us!=LAUNCH_POST_US) return STORE_CORRUPT;
        for (unsigned k=0;k<STORAGE_EVENT_SOURCES;++k) {
            v->sources[k]=get32(d+68+k*4);
            if (k<v->source_count) {
                storage_session *p=storage_find(s,v->sources[k]);
                if (!p || p->format!=2 || !p->event_id ||
                    (k && v->sources[k]<=v->sources[k-1])) return STORE_CORRUPT;
            } else if (v->sources[k]) return STORE_CORRUPT;
        }
        if (v->sources[v->source_count-1]!=v->id) return STORE_CORRUPT;
        if (storage_find(s,v->id)->generation!=v->generation) return STORE_CORRUPT;
        for (unsigned k=0;k<i;++k) if (s->events[k].id==v->id) return STORE_CORRUPT;
    }
    storage_session *deleting=storage_find(s,s->delete_id);
    storage_event *delete_event=storage_event_find(s,s->delete_id);
    if (s->delete_id && (!deleting || (deleting->state!=SESSION_DELETING &&
        (!delete_event || delete_event->state!=SESSION_DELETING)) ||
        s->delete_progress>deletion_total(s,deleting) || !s->delete_op || s->op_state!=STORE_OP_RUNNING ||
        s->delete_op!=s->last_op || s->op_session!=s->delete_id ||
        s->op_generation!=deleting->generation || s->op_total!=deletion_total(s,deleting))) return STORE_CORRUPT;
    int changed=0;
    struct { uint32_t id, reasons; uint64_t time; } triggers[STORAGE_MAX_EVENTS];
    unsigned trigger_count=0;
    for (unsigned i=0; i<STORAGE_MAX_SESSIONS; ++i) {
        storage_session *p=&s->sessions[i]; if (!p->id) continue;
        if (p->state==SESSION_DELETING) {
            storage_event *v=storage_event_find(s,s->delete_id);
            if (p->id!=s->delete_id && (!v || p->event_id!=v->id)) return STORE_CORRUPT;
            continue;
        }
        e=read_bytes(s,p->first_sector*4096,s->scratch,256); if (e) return e;
        int header_ok=header_valid(s->scratch,p);
        if (header_ok && p->format==2) {
            header_ok=get32(s->scratch+124)==2;
            p->epoch=get32(s->scratch+128); p->origin_us=get64(s->scratch+132);
            if (!p->epoch) header_ok=0;
        }
        uint32_t count=0;
        uint32_t slots=(p->sectors*16-1)*(p->format==2 ? 1U : 3U);
        for (uint32_t slot=0; slot<slots; ++slot) {
            uint32_t address=p->format==2 ? p->first_sector*4096+256+slot*256 : slot_address(p,slot);
            unsigned size=p->format==2 ? 256 : 80;
            e=read_bytes(s,address,s->scratch,size); if (e) return e;
            if (blank(s->scratch,size)) continue;
            p->next_slot=slot+1;
            if (header_ok && (p->format==2 ? compact_page_valid(s->scratch,p->id) : record_valid(s->scratch,p->id))) {
                if (p->format==2) {
                    unsigned samples=s->scratch[5];
                    set_page_state(s,address/256,samples); s->sector_records[address/4096]+=(uint8_t)samples;
                    logger_record r;
                    compact_page_raw(s->scratch,0,&r); if (!count) p->first_us=record_timestamp(&r);
                    compact_page_raw(s->scratch,samples-1,&r); p->last_us=record_timestamp(&r);
                    count+=samples*2;
                    if (s->scratch[6] && !p->event_suppressed && !storage_event_find(s,p->id)) {
                        compact_page_raw(s->scratch,s->scratch[7],&r);
                        if (trigger_count==STORAGE_MAX_EVENTS) return STORE_CORRUPT;
                        triggers[trigger_count].id=p->id; triggers[trigger_count].reasons=s->scratch[6];
                        triggers[trigger_count++].time=record_timestamp(&r);
                    }
                } else { mark_valid(s,address); ++count; }
            } else {
                if (p->state==SESSION_CLOSED && !p->incomplete) return STORE_CORRUPT;
                p->incomplete=1; /* Torn slots are quarantined, never retried. */
            }
        }
        if (p->state==SESSION_CLOSED &&
            ((!header_ok && !(p->incomplete && p->records==0)) || p->records!=count)) return STORE_CORRUPT;
        if (p->state==SESSION_OPEN) {
            e=trim_tail(s,p); if (e) return e;
            p->state=SESSION_CLOSED; p->incomplete=1; p->records=count; changed=1;
        }
    }
    for (unsigned i=0;i<trigger_count;++i) {
        e=protect_event(s,triggers[i].id,triggers[i].time,triggers[i].reasons,1);
        if (e) return e;
        changed=1;
    }
    for (unsigned i=0;i<STORAGE_MAX_EVENTS;++i) {
        storage_event *v=&s->events[i]; if (!v->id || v->state==SESSION_DELETING) continue;
        uint32_t samples=0;
        for (unsigned k=0;k<v->source_count;++k) {
            storage_session *p=storage_find(s,v->sources[k]);
            if (!p || p->epoch!=v->epoch) return STORE_CORRUPT;
            samples+=p->records/2;
            if (k==v->source_count-1) v->end_us=p->last_us;
        }
        if (samples<v->skip_samples) return STORE_CORRUPT;
        samples-=v->skip_samples;
        if (v->state==SESSION_CLOSED && samples!=v->samples) return STORE_CORRUPT;
        v->samples=samples;
        if (v->state==SESSION_OPEN) { v->state=SESSION_CLOSED; v->incomplete=1; changed=1; }
    }
    for (unsigned i=0;i<STORAGE_MAX_SESSIONS;++i) {
        storage_session *p=&s->sessions[i];
        if (p->id && p->event_id && !event_references(s,p->id,0)) return STORE_CORRUPT;
    }
    s->mounted=1;
    return changed ? save_journal(s) : STORE_OK;
}
int storage_mount(storage *s, const storage_io *io)
{
    if (!s || !io || !io->read || !io->program || !io->erase) return STORE_PARAM;
    memset(s,0,sizeof(*s)); s->io=*io; s->active=NO_ACTIVE;
    int e=mount_impl(s); if (e) s->fault=(uint32_t)e;
    return e;
}
static int start_impl(storage *s, const sensor_metadata *m, float reference,
                      uint32_t epoch, uint64_t origin, unsigned reserve, unsigned format, uint32_t *id)
{
    if (!m || !id || !isfinite(reference) || reference<=0) return STORE_PARAM;
    if (!s->mounted || s->fault || s->active!=NO_ACTIVE) return STORE_STATE;
    if (s->next_id==UINT32_MAX) return STORE_FULL;
    unsigned index=0; while (index<STORAGE_MAX_SESSIONS && s->sessions[index].id) ++index;
    uint32_t sector=2;
    for (;sector<STORAGE_SECTORS;++sector) {
        if (occupied(s,sector)) continue;
        if (!reserve) break;
        unsigned n=0; while (n<reserve && !occupied(s,sector+n)) ++n;
        if (n==reserve) break;
        sector+=n;
    }
    if (index==STORAGE_MAX_SESSIONS || sector>=STORAGE_SECTORS) return STORE_FULL;
    uint32_t reserved=reserve ? reserve : free_extent(s,sector);
    int e=erased(s,sector*4096,format==2 ? 256 : reserved*4096); if (e) return fail(s,e);
    storage_session *p=&s->sessions[index];
    *p=(storage_session){.id=s->next_id,.generation=s->next_id,.first_sector=sector,
                         .sectors=reserved,.state=SESSION_OPEN,.format=format,.epoch=epoch ? epoch : s->next_id,
                         .origin_us=origin};
    ++s->next_id; s->active=index;
    /* Allocation/ID is durable before any header/data write. */
    e=save_journal(s); if (e) return e;
    uint8_t header[256]={0}; memcpy(header,"DLH1",4); put32(header+4,1);
    put32(header+8,p->id); put32(header+12,p->generation);
    uint32_t bits; memcpy(&bits,&reference,4); put32(header+16,bits); put32(header+20,m->version);
    header[24]=m->accel_id; header[25]=m->gyro_id; header[26]=m->baro_id;
    header[27]=m->accel_conf; header[28]=m->accel_range; header[29]=m->gyro_bw;
    header[30]=m->gyro_range; header[31]=m->baro_osr; header[32]=m->baro_odr; header[33]=m->baro_iir;
    memcpy(header+34,m->trim,21);
    if (format==2) { put32(header+124,2); put32(header+128,p->epoch); put64(header+132,origin); }
    /* Identity mounting matrix at 64; bias vectors at 100/112 are zero. */
    float one=1; memcpy(&bits,&one,4);
    for (unsigned i=0; i<3; ++i) put32(header+64+(i*3+i)*4,bits);
    put32(header+248,usb_crc32(header,248)); memset(header+252,255,4);
    e=committed_write(s,sector*4096,header,256); if (e) return fail(s,e);
    if (format==2) compact_page_init(s->pending_page,p->id);
    *id=p->id; return STORE_OK;
}
int storage_start(storage *s, const sensor_metadata *m, float reference, uint32_t *id)
{ return start_impl(s,m,reference,0,0,0,1,id); }
int storage_start_compact(storage *s, const sensor_metadata *m, float reference,
                          uint32_t epoch, uint64_t origin, unsigned reserve, uint32_t *id)
{
    if (!reserve || reserve>STORAGE_SECTORS-2) return STORE_PARAM;
    return start_impl(s,m,reference,epoch,origin,reserve,2,id);
}
int storage_flush(storage *s)
{
    if (s->fault || s->active==NO_ACTIVE) return STORE_STATE;
    storage_session *p=&s->sessions[s->active];
    if (p->format!=2 || !s->pending_page[5]) return STORE_OK;
    if (p->next_slot>=p->sectors*16-1) return STORE_FULL;
    uint32_t address=p->first_sector*4096+256+p->next_slot*256;
    int e=erased(s,address,256); if (e) return fail(s,e);
    unsigned n=s->pending_page[5];
    compact_page_seal(s->pending_page);
    e=committed_write(s,address,s->pending_page,256); if (e) return fail(s,e);
    set_page_state(s,address/256,n); s->sector_records[address/4096]+=(uint8_t)n;
    ++p->next_slot; p->records+=n*2;
    storage_event *v=storage_event_find(s,p->id);
    if (v && v->state==SESSION_OPEN) { v->samples+=n; v->end_us=p->last_us; }
    compact_page_init(s->pending_page,p->id);
    return STORE_OK;
}
static int append_compact(storage *s, const logger_record *r, unsigned trigger)
{
    storage_session *p=&s->sessions[s->active];
    if (r->bytes[0]==2) return STORE_OK; /* Derived values are reconstructed on export. */
    if (!compact_page_add(s->pending_page,r,trigger)) {
        if (!s->pending_page[5]) return STORE_PARAM;
        int e=storage_flush(s); if (e) return e;
        if (!compact_page_add(s->pending_page,r,trigger)) return STORE_PARAM;
    }
    if (!p->records && s->pending_page[5]==1) p->first_us=record_timestamp(r);
    p->last_us=record_timestamp(r);
    return s->pending_page[5]==8 ? storage_flush(s) : STORE_OK;
}
int storage_append(storage *s, const logger_record *record)
{
    if (!record) return STORE_PARAM;
    if (!s->mounted || s->fault || s->active==NO_ACTIVE) return STORE_STATE;
    const uint8_t *r=record->bytes;
    if ((r[0]!=1 && r[0]!=2) || r[1]!=1 || r[2]!=(r[0]==1 ? 32 : 48) || r[3]) return STORE_PARAM;
    if (r[0]==1) for (unsigned i=48; i<64; ++i) if (r[i]) return STORE_PARAM;
    storage_session *p=&s->sessions[s->active];
    if (p->format==2) return append_compact(s,record,0);
    if (p->next_slot==(p->sectors*16-1)*3) {
        uint32_t sector=p->first_sector+p->sectors;
        if (occupied(s,sector)) return STORE_FULL;
        uint32_t reserved=free_extent(s,sector);
        int e=erased(s,sector*4096,reserved*4096); if (e) return fail(s,e);
        p->sectors+=reserved; e=save_journal(s); if (e) return e;
    }
    uint32_t address=slot_address(p,p->next_slot);
    int e=erased(s,address,80); if (e) return fail(s,e);
    uint8_t b[80]={0}; memcpy(b,"DLOG",4); b[4]=1; b[5]=r[0]; b[6]=r[2];
    put32(b+8,p->id); memcpy(b+12,r+4,12); memcpy(b+24,r+16,48);
    put32(b+72,usb_crc32(b,72)); memset(b+76,255,4);
    e=committed_write(s,address,b,80); if (e) return fail(s,e);
    mark_valid(s,address); ++p->next_slot; ++p->records;
    return STORE_OK;
}
int storage_close(storage *s, int incomplete)
{
    if (!s->mounted || s->fault) return STORE_STATE;
    if (s->active==NO_ACTIVE) return STORE_OK;
    storage_session *p=&s->sessions[s->active];
    int flushed=storage_flush(s);
    if (flushed==STORE_FULL) { incomplete=1; compact_page_init(s->pending_page,p->id); }
    else if (flushed) return flushed;
    /* Return only verified-blank reservation tails. Batch allocation avoids
     * wearing the two metadata sectors once per 48 samples. */
    int trimmed=trim_tail(s,p); if (trimmed) return fail(s,trimmed);
    p->state=SESSION_CLOSED; p->incomplete=incomplete!=0;
    storage_event *v=storage_event_find(s,p->id);
    if (v && v->state==SESSION_OPEN) {
        v->state=SESSION_CLOSED;
        v->incomplete=v->incomplete || incomplete || v->end_us+20000<v->target_us;
    }
    int e=save_journal(s); if (e) return e;
    s->active=NO_ACTIVE; return STORE_OK;
}
int storage_metadata(storage *s, uint32_t id, uint8_t out[256])
{
    if (!s->mounted) return STORE_STATE;
    storage_session *p=storage_find(s,id);
    if (!out) return STORE_PARAM;
    if (!p) return STORE_NOT_FOUND;
    storage_event *v=storage_event_find(s,id);
    if (v && v->state!=SESSION_CLOSED) return STORE_STATE;
    if (!v && p->event_id) return STORE_STATE;
    if (p->state!=SESSION_CLOSED) return STORE_STATE;
    int e=read_bytes(s,p->first_sector*4096,out,256); if (e) return fail(s,e);
    if (header_valid(out,p)) {
        if (v) {
            put64(out+132,v->begin_us); put32(out+140,3); put32(out+144,v->reasons);
            put64(out+148,v->trigger_us); put64(out+156,v->target_us); put64(out+164,v->end_us);
            put32(out+172,(uint32_t)LAUNCH_PRE_US); put32(out+176,(uint32_t)LAUNCH_POST_US);
            put32(out+180,(uint32_t)(v->trigger_us-v->begin_us)); put32(out+184,v->incomplete);
            put32(out+248,usb_crc32(out,248));
        }
        return STORE_OK;
    }
    /* A reset during START can leave a catalogued, empty quarantined header.
     * Report its missing metadata without disabling deletion of that session. */
    if (p->incomplete && !p->records) return STORE_CORRUPT;
    return fail(s,STORE_CORRUPT);
}
static int locate(storage *s, storage_session *p, uint32_t record, uint32_t *address)
{
    for (uint32_t sector=p->first_sector; sector<p->first_sector+p->sectors; ++sector) {
        if (record>=s->sector_records[sector]) { record-=s->sector_records[sector]; continue; }
        for (unsigned slot=0; slot<48; ++slot) {
            uint32_t a=sector*4096+(slot/3)*256+(slot%3)*80;
            if (is_valid(s,a) && record--==0) { *address=a; return STORE_OK; }
        }
        break;
    }
    return STORE_CORRUPT;
}
int storage_read(storage *s, uint32_t id, uint32_t offset, uint8_t *out, size_t count)
{
    if (!s->mounted) return STORE_STATE;
    storage_session *p=storage_find(s,id);
    if (!out || !count || count>512) return STORE_PARAM;
    if (!p) return STORE_NOT_FOUND;
    storage_event *v=storage_event_find(s,id);
    if (v && v->state!=SESSION_CLOSED) return STORE_STATE;
    if (!v && p->event_id) return STORE_STATE;
    if (p->state!=SESSION_CLOSED) return STORE_STATE;
    uint32_t length=v ? v->samples*160 : p->records*80;
    if (offset>length || count>length-offset) return STORE_BOUNDS;
    if (p->format==2) return packed_read(s,p,v,offset,out,count);
    while (count) {
        uint32_t address; int e=locate(s,p,offset/80,&address); if (e) return fail(s,e);
        e=read_bytes(s,address,s->scratch,80); if (e) return fail(s,e);
        if (!record_valid(s->scratch,id)) return fail(s,STORE_CORRUPT);
        size_t n=80-offset%80; if (n>count) n=count;
        memcpy(out,s->scratch+offset%80,n);
        out+=n; offset+=(uint32_t)n; count-=n;
    }
    return STORE_OK;
}
static int event_references(storage *s, uint32_t source, uint32_t except)
{
    for (unsigned i=0;i<STORAGE_MAX_EVENTS;++i) {
        storage_event *v=&s->events[i]; if (!v->id || v->id==except) continue;
        for (unsigned k=0;k<v->source_count;++k) if (v->sources[k]==source) return 1;
    }
    return 0;
}
static int deletion_owns(storage *s, storage_session *p, uint32_t target)
{
    storage_event *v=storage_event_find(s,target);
    if (!v) return p->id==target;
    for (unsigned k=0;k<v->source_count;++k)
        if (v->sources[k]==p->id) return !event_references(s,p->id,target);
    return 0;
}
static uint32_t deletion_total(storage *s, storage_session *primary)
{
    uint32_t count=0;
    for (unsigned i=0;i<STORAGE_MAX_SESSIONS;++i) {
        storage_session *p=&s->sessions[i];
        if (p->id && deletion_owns(s,p,primary->id)) count+=p->sectors;
    }
    return count;
}
static int delete_begin(storage *s, uint32_t id, uint32_t *operation, int reclaim)
{
    if (!operation) return STORE_PARAM;
    if (!s->mounted || s->fault || (!reclaim && s->active!=NO_ACTIVE) || s->delete_id) return STORE_STATE;
    storage_session *p=storage_find(s,id); if (!p) return STORE_NOT_FOUND;
    if (p->state!=SESSION_CLOSED) return STORE_STATE;
    storage_event *v=storage_event_find(s,id);
    if ((!v && p->event_id) || (v && v->state!=SESSION_CLOSED) ||
        (reclaim && (v || p->event_id || p->format!=2))) return STORE_STATE;
    if (s->last_op==UINT32_MAX) return STORE_FULL;
    s->delete_id=id; s->delete_op=++s->last_op; s->delete_progress=0;
    s->op_state=STORE_OP_RUNNING; s->op_session=id; s->op_generation=p->generation; s->op_error=0;
    s->op_total=deletion_total(s,p);
    if (v) v->state=SESSION_DELETING;
    for (unsigned i=0;i<STORAGE_MAX_SESSIONS;++i) {
        storage_session *q=&s->sessions[i];
        if (q->id && deletion_owns(s,q,id)) {
            q->state=SESSION_DELETING;
            if (v) q->event_id=id;
        }
    }
    int e=save_journal(s); if (e) return e;
    *operation=s->delete_op; return STORE_OK;
}
int storage_delete_begin(storage *s, uint32_t id, uint32_t *operation)
{ return delete_begin(s,id,operation,0); }
int storage_reclaim_begin(storage *s, uint32_t id, uint32_t *operation)
{ return delete_begin(s,id,operation,1); }
int storage_delete_step(storage *s)
{
    if (!s->delete_id) return STORE_OK;
    if (s->fault) return STORE_STATE;
    storage_session *p=storage_find(s,s->delete_id);
    if (!p || s->delete_progress>s->op_total) return fail(s,STORE_CORRUPT);
    if (s->delete_progress<s->op_total) {
        uint32_t left=s->delete_progress, sector=STORAGE_SECTORS;
        for (unsigned i=0;i<STORAGE_MAX_SESSIONS;++i) {
            storage_session *q=&s->sessions[i];
            if (!q->id || !deletion_owns(s,q,s->delete_id)) continue;
            if (left>=q->sectors) left-=q->sectors;
            else { sector=q->first_sector+left; break; }
        }
        if (sector>=STORAGE_SECTORS) return fail(s,STORE_CORRUPT);
        int empty=erased(s,sector*4096,4096);
        if (empty!=STORE_OK && empty!=STORE_CORRUPT) return fail(s,empty);
        if (empty!=STORE_OK && s->io.erase(s->io.context,sector*4096)) {
            s->op_state=STORE_OP_FAILED; s->op_error=STORE_IO;
            return fail(s,STORE_IO); /* Durable intent remains available to recover. */
        }
        int e=erased(s,sector*4096,4096); if (e) return fail(s,e);
        memset(s->page_state+sector*8,0,8); s->sector_records[sector]=0;
        ++s->delete_progress;
    }
    if (s->delete_progress==s->op_total) {
        uint32_t id=s->delete_id;
        storage_event *v=storage_event_find(s,id);
        if (v) {
            storage_session *primary=storage_find(s,id);
            if (primary) primary->event_suppressed=1;
        }
        for (unsigned i=0;i<STORAGE_MAX_SESSIONS;++i) {
            storage_session *q=&s->sessions[i];
            if (q->id && deletion_owns(s,q,id)) memset(q,0,sizeof(*q));
        }
        if (v) memset(v,0,sizeof(*v));
        for (unsigned i=0;i<STORAGE_MAX_SESSIONS;++i) {
            storage_session *q=&s->sessions[i]; if (!q->id || !q->event_id) continue;
            q->event_id=0;
            for (unsigned k=0;k<STORAGE_MAX_EVENTS;++k) {
                storage_event *other=&s->events[k];
                for (unsigned n=0;n<other->source_count;++n)
                    if (other->sources[n]==q->id) q->event_id=other->id;
            }
        }
        s->delete_id=0; s->op_state=STORE_OP_DONE;
    }
    /* Intent already owns the whole extent durably. After reset, blank sectors
     * are skipped; no per-sector metadata rewrite/erase is necessary. */
    return s->delete_id ? STORE_OK : save_journal(s);
}
uint32_t storage_used_sectors(const storage *s)
{
    uint32_t count=2;
    for (unsigned i=0; i<STORAGE_MAX_SESSIONS; ++i) count+=s->sessions[i].sectors;
    return count;
}
int storage_latest(storage *s, uint8_t out[80])
{
    if (!out) return STORE_PARAM;
    if (!s->mounted || s->fault || s->active==NO_ACTIVE) return STORE_STATE;
    storage_session *p=&s->sessions[s->active];
    if (!p->records || !p->next_slot) return STORE_NOT_FOUND;
    if (p->format==2) {
        logger_record raw; int e=packed_raw(s,p,p->records/2-1,&raw); if (e) return e;
        memset(out,0,80); memcpy(out,"DLOG",4); out[4]=1; out[5]=1; out[6]=32;
        put32(out+8,p->id); memcpy(out+12,raw.bytes+4,12); memcpy(out+24,raw.bytes+16,48);
        put32(out+72,usb_crc32(out,72)); return STORE_OK;
    }
    uint32_t address=slot_address(p,p->next_slot-1);
    int e=read_bytes(s,address,out,80); if (e) return fail(s,e);
    return record_valid(out,p->id) ? STORE_OK : fail(s,STORE_CORRUPT);
}

static int packed_raw(storage *s, storage_session *p, uint32_t sample, logger_record *raw)
{
    if (sample>=p->records/2) return STORE_BOUNDS;
    for (uint32_t sector=p->first_sector;sector<p->first_sector+p->sectors;++sector) {
        if (sample>=s->sector_records[sector]) { sample-=s->sector_records[sector]; continue; }
        for (unsigned page=0;page<16;++page) {
            uint32_t address=sector*4096+page*256;
            unsigned n=page_state(s,address/256);
            if (sample>=n) { sample-=n; continue; }
            int e=read_bytes(s,address,s->scratch,256); if (e) return fail(s,e);
            if (!compact_page_valid(s->scratch,p->id) || n!=s->scratch[5]) return fail(s,STORE_CORRUPT);
            return compact_page_raw(s->scratch,sample,raw) ? STORE_OK : fail(s,STORE_CORRUPT);
        }
        break;
    }
    return fail(s,STORE_CORRUPT);
}
static int packed_read(storage *s, storage_session *p, storage_event *v,
                       uint32_t offset, uint8_t *out, size_t count)
{
    uint8_t header[256]; int e=storage_metadata(s,p->id,header); if (e) return e;
    sensor_metadata m={.version=get32(header+20),.accel_id=header[24],.gyro_id=header[25],
        .baro_id=header[26],.accel_conf=header[27],.accel_range=header[28],.gyro_bw=header[29],
        .gyro_range=header[30],.baro_osr=header[31],.baro_odr=header[32],.baro_iir=header[33]};
    memcpy(m.trim,header+34,21);
    float reference; uint32_t bits=get32(header+16); memcpy(&reference,&bits,4);
    uint64_t origin=v ? v->begin_us : p->origin_us;
    while (count) {
        uint32_t frame=offset/80, sample=frame/2;
        storage_session *source=p;
        if (v) {
            sample+=v->skip_samples; source=NULL;
            for (unsigned k=0;k<v->source_count;++k) {
                storage_session *q=storage_find(s,v->sources[k]);
                if (!q) return fail(s,STORE_CORRUPT);
                if (sample<q->records/2) { source=q; break; }
                sample-=q->records/2;
            }
            if (!source) return fail(s,STORE_CORRUPT);
        }
        logger_record raw, processed; sensor_processed values;
        e=packed_raw(s,source,sample,&raw); if (e) return e;
        uint64_t time=record_timestamp(&raw);
        if (time<origin) return fail(s,STORE_CORRUPT);
        put64(raw.bytes+8,time-origin);
        if (frame&1) { record_process(&processed,&raw,&m,reference,&values); raw=processed; }
        uint8_t b[80]={0}; memcpy(b,"DLOG",4); b[4]=1; b[5]=raw.bytes[0]; b[6]=raw.bytes[2];
        put32(b+8,v ? v->id : p->id); memcpy(b+12,raw.bytes+4,12); memcpy(b+24,raw.bytes+16,48);
        put32(b+72,usb_crc32(b,72));
        size_t n=80-offset%80; if (n>count) n=count;
        memcpy(out,b+offset%80,n); out+=n; offset+=(uint32_t)n; count-=n;
    }
    return STORE_OK;
}
storage_event *storage_event_find(storage *s, uint32_t id)
{
    if (!id) return NULL;
    for (unsigned i=0;i<STORAGE_MAX_EVENTS;++i) if (s->events[i].id==id) return &s->events[i];
    return NULL;
}
static int protect_event(storage *s, uint32_t id, uint64_t time, unsigned reasons, int recovered)
{
    storage_session *primary=storage_find(s,id);
    if (!primary || primary->format!=2 || !primary->records || !reasons || reasons>7) return STORE_STATE;
    storage_event *v=NULL;
    for (unsigned i=0;i<STORAGE_MAX_EVENTS;++i) if (!s->events[i].id) { v=&s->events[i]; break; }
    if (!v) return STORE_FULL;
    *v=(storage_event){.id=id,.generation=primary->generation,.epoch=primary->epoch,.reasons=reasons,
        .state=recovered ? SESSION_CLOSED : SESSION_OPEN,.incomplete=recovered!=0,
        .trigger_us=time,.target_us=time+LAUNCH_POST_US,.end_us=primary->last_us};
    uint64_t begin=time>LAUNCH_PRE_US ? time-LAUNCH_PRE_US : 0;
    uint32_t previous=0;
    for (;;) {
        storage_session *selected=NULL;
        for (unsigned i=0;i<STORAGE_MAX_SESSIONS;++i) {
            storage_session *p=&s->sessions[i];
            if (p->id>previous && p->id<=id && p->format==2 && p->epoch==primary->epoch && p->records &&
                p->state!=SESSION_DELETING && p->last_us>=begin && p->first_us<=time &&
                (!selected || p->id<selected->id)) selected=p;
        }
        if (!selected) break;
        if (v->source_count==STORAGE_EVENT_SOURCES) { memset(v,0,sizeof(*v)); return STORE_FULL; }
        v->sources[v->source_count++]=selected->id; previous=selected->id;
    }
    if (!v->source_count || v->sources[v->source_count-1]!=id) { memset(v,0,sizeof(*v)); return STORE_CORRUPT; }
    storage_session *first=storage_find(s,v->sources[0]);
    if (!first || !first->records) { memset(v,0,sizeof(*v)); return STORE_CORRUPT; }
    uint32_t low=0, high=first->records/2;
    logger_record r;
    while (low<high) {
        uint32_t mid=low+(high-low)/2;
        int e=packed_raw(s,first,mid,&r); if (e) return e;
        if (record_timestamp(&r)<begin) low=mid+1; else high=mid;
    }
    if (low==first->records/2) return STORE_CORRUPT;
    int e=packed_raw(s,first,low,&r); if (e) return e;
    v->skip_samples=low; v->begin_us=record_timestamp(&r);
    if (v->trigger_us-v->begin_us+10000<LAUNCH_PRE_US) v->incomplete=1;
    uint32_t total=0;
    for (unsigned k=0;k<v->source_count;++k) {
        storage_session *p=storage_find(s,v->sources[k]);
        if (!p->event_id) p->event_id=id;
        total+=p->records/2;
    }
    v->samples=total-v->skip_samples;
    return STORE_OK;
}
int storage_trigger(storage *s, const logger_record *raw, unsigned reasons)
{
    if (!raw || !reasons || reasons>7 || s->fault || s->active==NO_ACTIVE) return STORE_STATE;
    storage_session *p=&s->sessions[s->active];
    if (p->format!=2 || p->event_id) return STORE_STATE;
    int e=append_compact(s,raw,reasons); if (e) return e;
    e=storage_flush(s); if (e) return e;
    e=protect_event(s,p->id,record_timestamp(raw),reasons,0); if (e) return fail(s,e);
    return save_journal(s);
}
int storage_event_note(storage *s, unsigned reasons)
{
    if (s->fault || s->active==NO_ACTIVE) return STORE_STATE;
    storage_event *v=storage_event_find(s,s->sessions[s->active].id);
    if (!v || v->state!=SESSION_OPEN) return STORE_STATE;
    unsigned combined=v->reasons|(reasons&7U);
    if (combined==v->reasons) return STORE_OK;
    v->reasons=combined; return save_journal(s);
}
int storage_can_reserve(const storage *s, unsigned wanted)
{
    if (!wanted || wanted>STORAGE_SECTORS-2) return 0;
    unsigned run=0;
    for (uint32_t sector=2;sector<STORAGE_SECTORS;++sector) {
        int used=0;
        for (unsigned i=0;i<STORAGE_MAX_SESSIONS;++i) {
            const storage_session *p=&s->sessions[i]; if (!p->id) continue;
            uint32_t extent=p->sectors;
            if (i==s->active && p->format==2 && !p->event_id)
                extent=(256+(p->next_slot+(s->pending_page[5]!=0))*256+4095)/4096;
            if (sector>=p->first_sector && sector-p->first_sector<extent) { used=1; break; }
        }
        run=used ? 0 : run+1;
        if (run>=wanted) return 1;
    }
    return 0;
}
