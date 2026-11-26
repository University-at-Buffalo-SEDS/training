#include "compact_record.h"
#include "usb_protocol.h"
#include <string.h>

static uint32_t get32(const uint8_t *p)
{ return p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static void put32(uint8_t *p, uint32_t v)
{ for (unsigned i=0;i<4;++i) p[i]=(uint8_t)(v>>(8*i)); }
static uint16_t get16(const uint8_t *p) { return p[0] | (uint16_t)p[1]<<8; }
static void put16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
uint64_t record_timestamp(const logger_record *r)
{ return get32(r->bytes+8) | (uint64_t)get32(r->bytes+12)<<32; }
void compact_page_init(uint8_t b[256], uint32_t session)
{ memset(b,0,256); memcpy(b,"DRP2",4); b[4]=2; put32(b+20,session); }
int compact_page_add(uint8_t b[256], const logger_record *r, unsigned trigger)
{
    const uint8_t *p=r->bytes+16;
    if (r->bytes[0]!=1 || r->bytes[1]!=1 || r->bytes[2]!=32 || r->bytes[3] ||
        get32(p)>255 || get32(p+16)>0xffffff || get32(p+20)>0xffffff ||
        get32(p+28)>15 || trigger>7 || b[5]>=8) return 0;
    uint64_t time=record_timestamp(r);
    uint32_t sequence=get32(r->bytes+4);
    if (!b[5]) { memcpy(b+8,r->bytes+4,4); memcpy(b+12,r->bytes+8,8); }
    uint64_t first=get32(b+12) | (uint64_t)get32(b+16)<<32;
    uint32_t delta=sequence-get32(b+8);
    if (time<first || (time-first)%1000 || (time-first)/1000>65535 || delta>65535) return 0;
    unsigned index=b[5]; uint8_t *d=b+24+index*28;
    d[0]=(uint8_t)get32(p); memcpy(d+1,p+4,12);
    memcpy(d+13,p+16,3); memcpy(d+16,p+20,3); memcpy(d+19,p+24,4);
    d[23]=(uint8_t)get32(p+28); put16(d+24,(uint16_t)((time-first)/1000));
    put16(d+26,(uint16_t)delta);
    if (trigger && !b[6]) { b[6]=(uint8_t)trigger; b[7]=(uint8_t)index; }
    ++b[5]; return 1;
}
int compact_page_valid(const uint8_t b[256], uint32_t session)
{
    if (memcmp(b,"DRP2",4) || b[4]!=2 || !b[5] || b[5]>8 || b[6]>7 ||
        (b[6] ? b[7]>=b[5] : b[7]!=0) || get32(b+20)!=session || get32(b+252)!=0 ||
        get32(b+248)!=usb_crc32(b,248)) return 0;
    for (unsigned i=0;i<b[5];++i) if (b[24+i*28+23]>15) return 0;
    return 1;
}
int compact_page_raw(const uint8_t b[256], unsigned index, logger_record *r)
{
    if (index>=b[5] || b[5]>8) return 0;
    const uint8_t *p=b+24+index*28;
    sensor_raw s={.flags=p[0],.pressure_adc=p[13] | (uint32_t)p[14]<<8 | (uint32_t)p[15]<<16,
        .temperature_adc=p[16] | (uint32_t)p[17]<<8 | (uint32_t)p[18]<<16,
        .baro_age_us=get32(p+19),.errors=p[23]};
    for (unsigned i=0;i<3;++i) {
        s.accel[i]=(int16_t)get16(p+1+i*2); s.gyro[i]=(int16_t)get16(p+7+i*2);
    }
    uint64_t time=get32(b+12) | (uint64_t)get32(b+16)<<32;
    record_raw(r,get32(b+8)+get16(p+26),time+(uint64_t)get16(p+24)*1000,&s);
    return 1;
}
void compact_page_seal(uint8_t b[256])
{ put32(b+248,usb_crc32(b,248)); memset(b+252,255,4); }
