#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
static uint8_t flash_record[32];
int tzflash_read(void *p, unsigned n, unsigned addr)
{
    assert(addr == 0x30B000 && n == sizeof(flash_record));
    memcpy(p, flash_record, n); return n;
}
#include "badge_identity.h"
static int resource_status;
static uint32_t resource_length=32;
int badge_resource_find(uint32_t id, uint32_t *address, uint32_t *length)
{
    assert(id==BR_IDENTITY);
    if (resource_status>0) { *address=BADGE_IDENTITY_ADDR; *length=resource_length; }
    return resource_status;
}
#include "badge_power_status.h"
int main(void)
{
    char name[32] = "Default", text[24];
    badge_power_state s = {0};
    uint16_t crc;
    unsigned i;
    assert(!strcmp(badge_mouse_local_name(), "EC-MOUSE"));
    memset(flash_record, 0xff, sizeof(flash_record));
    badge_identity_load(name); assert(!strcmp(name, "Default"));
    memset(flash_record, 0, sizeof(flash_record));
    memcpy(flash_record, "BID1", 4); flash_record[4]=1; flash_record[5]=7;
    memcpy(flash_record+8, "Example", 7);
    crc=badge_identity_crc(flash_record+4,2,0xffff);
    crc=badge_identity_crc(flash_record+8,7,crc);
    flash_record[6]=crc; flash_record[7]=crc>>8;
    resource_status=-1; badge_identity_load(name); assert(!strcmp(name,"Default"));
    resource_status=1; resource_length=31;
    badge_identity_load(name); assert(!strcmp(name,"Default"));
    resource_length=32;
    badge_identity_load(name); assert(!strcmp(name, "Example"));
    assert(!strcmp(badge_mouse_local_name(), "Example Mouse"));
    for (i=0; i<32; ++i) {
        uint8_t bad[32]; memcpy(bad,flash_record,32); bad[i]^=0x80;
        strcpy(name,"unchanged");
        if (i < 15) { assert(!badge_identity_parse(bad,name)); assert(!strcmp(name,"unchanged")); }
    }
    for (i=0;i<256;++i) {
        uint8_t bad[32]; memcpy(bad,flash_record,32); bad[5]=i;
        if (i!=7) assert(!badge_identity_parse(bad,name));
    }
    badge_power_sample(&s, 0); assert(!s.online);
    badge_power_sample(&s, 1); badge_power_sample(&s, 0); assert(!s.online);
    for(i=0;i<3;++i) badge_power_sample(&s,1);
    assert(s.online);
    for(i=0;i<3;++i) badge_power_sample(&s,0);
    assert(!s.online);
    badge_power_text(text,sizeof(text),46,0,1); assert(!strcmp(text,"46%  BATTERY"));
    badge_power_text(text,sizeof(text),46,1,0); assert(!strcmp(text,"46%  CHARGING"));
    badge_power_text(text,sizeof(text),255,1,1); assert(!strcmp(text,"100%  FULL"));
    assert(badge_power_text(text,sizeof(text),12,0,0)==0xffb000);
    puts("System identity/power native checks passed");
}
