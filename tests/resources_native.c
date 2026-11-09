/* Real catalogue implementation on simulated NOR. Interrupt erase/program,
 * reboot, check selection/CRC, migration, unavailable entries and write bounds. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define BADGE_RESOURCES_IMPLEMENTATION
#include "badge_resources.h"
static uint8_t flash[BR_END-BR_START], saved[BR_END-BR_START];
static int operation, fail_operation=-1, fail_after, read_fail;
static unsigned read_bytes;
volatile uint8_t g_badge_update_active;
static int update_open=1, refuse_lock;
int badge_resources_update_open(void) { return update_open; }
int badge_resources_lock_update(void)
{ if (!update_open || refuse_lock) return 0; g_badge_update_active=1; return 1; }
static uint8_t reply_command, reply[32];
static unsigned reply_length;
static void qix_send_frame(uint8_t cmd, const uint8_t *p, uint16_t n)
{ assert(n<=sizeof(reply)); reply_command=cmd; reply_length=n; memcpy(reply,p,n); }
#include "badge_resources_wire.h"
static uint8_t *at(unsigned address, unsigned n)
{
    assert(address>=BR_START && address<=BR_END && n<=BR_END-address);
    return flash+(address-BR_START);
}
int tzflash_read(void *buf, unsigned n, unsigned address)
{
    if (read_fail) return -1;
    memcpy(buf,at(address,n),n); read_bytes+=n; return (int)n;
}
int tzflash_write(const void *buf, unsigned n, unsigned address)
{
    const uint8_t *p=buf;
    uint8_t *out=at(address,n);
    unsigned i;
    int current=operation++;
    for(i=0;i<n;++i) {
        if(current==fail_operation && i==(unsigned)fail_after) return -1;
        assert((out[i]&p[i])==p[i]); out[i]&=p[i];
    }
    return (int)n;
}
int tzflash_ioctl(unsigned command, unsigned address)
{
    unsigned i;
    int current=operation++;
    uint8_t *out=at(address,BR_SECTOR);
    assert(command==200 && !(address&(BR_SECTOR-1)));
    for(i=0;i<BR_SECTOR;++i) {
        if(current==fail_operation && i==(unsigned)fail_after) return -1;
        out[i]=0xff;
    }
    return 0;
}
static void reboot(void)
{
    memset(&br_current,0,sizeof(br_current));
    memset(&br_upload,0,sizeof(br_upload));
    br_loaded=br_verified=br_failed=0; br_active_address=0;
    operation=0; fail_operation=-1; read_fail=0; read_bytes=0;
}
static void clean(void) { memset(flash,0xff,sizeof(flash)); reboot(); }
static br_entry wallpaper(unsigned address, unsigned color)
{
    uint8_t *p=at(address,528);
    br_entry e={BR_WALLPAPER,1,address,528,0,0};
    uint8_t h[16]={'L','K','M','G',16,0,16,0,0,2,0,0,0,0,0,0};
    memcpy(p,h,16); memset(p+16,(int)color,512);
    e.crc=~br_crc_update(~0u,p,528);
    return e;
}
static void check_power_cuts(void)
{
    br_entry e;
    br_entry identity={BR_IDENTITY,1,0x30B000,32,0x12345678,0};
    br_entry fonts={BR_FONTS,1,0x310000,188416,0xABCDEF12,0};
    uint32_t address, length, generation;
    unsigned op, byte, limit;
    clean(); e=wallpaper(0x2A0000,0x42);
    assert(br_publish(&e)==BR_OK);
    assert(br_publish(&e)==BR_OK && br_publish(&e)==BR_OK);
    assert(br_publish(&identity)==BR_OK && br_publish(&fonts)==BR_OK);
    assert(br_current.count==3 && br_current.length==104 &&
           br_current.entry[0].id==1 && br_current.entry[1].id==2 && br_current.entry[2].id==3);
    memcpy(saved,flash,sizeof(flash));
    /* Every programmed byte (including the commit marker), plus representative
     * partial sector erases. Active A must remain byte-identical throughout. */
    for(op=0;op<3;++op) {
        limit=op==0 ? BR_SECTOR : op==1 ? 104 : 4;
        for(byte=0;byte<limit;byte+=(op==0 ? 127 : 1)) {
            memcpy(flash,saved,sizeof(flash)); reboot();
            fail_operation=(int)op; fail_after=(int)byte;
            assert(br_publish(&e)==BR_IO);
            assert(!memcmp(at(BR_TABLE_A,BR_SECTOR),saved+(BR_TABLE_A-BR_START),BR_SECTOR));
            reboot(); generation=badge_resource_generation();
            assert(generation==5 && badge_resource_find(BR_WALLPAPER,&address,&length)==1);
            assert(!memcmp(&br_current.entry[1],&fonts,sizeof(fonts)) &&
                   !memcmp(&br_current.entry[2],&identity,sizeof(identity)));
            assert(address==e.address && length==e.length);
        }
    }
    memcpy(flash,saved,sizeof(flash)); reboot();
    assert(br_publish(&e)==BR_OK && badge_resource_generation()==6);
    at(BR_TABLE_B,56)[4]^=1; /* generation is CRC-protected */
    reboot(); assert(badge_resource_generation()==5);
}
static void check_upload(void)
{
    uint32_t address, length;
    unsigned before;
    br_entry e, bad;
    clean(); e=wallpaper(0x2A0000,0x37);
    assert(badge_resource_find(BR_WALLPAPER,&address,&length)==0); /* legacy */
    assert(!badge_resource_raw_allowed(0xD0,e.address,e.length));
    assert(!badge_resource_raw_allowed(0xD1,BR_TABLE_A,4));
    assert(badge_resource_begin(&e)==BR_OK);
    assert(badge_resource_begin(&e)==BR_BUSY);
    assert(badge_resource_find(BR_WALLPAPER,&address,&length)==-1);
    assert(badge_resource_raw_allowed(0xD0,e.address,e.length));
    assert(badge_resource_raw_allowed(0xD1,e.address,e.length));
    assert(!badge_resource_raw_allowed(0xD1,e.address-1,2));
    assert(!badge_resource_raw_allowed(0xD1,e.address,e.length+1));
    assert(!badge_resource_raw_allowed(0xD0,e.address+BR_SECTOR-1,2));
    assert(!badge_resource_raw_allowed(0xD0,BR_TABLE_A,BR_SECTOR));
    assert(!badge_resource_raw_allowed(0xD4,e.address,e.length));
    assert(!badge_resource_raw_allowed(0xD1,0xfffffffeu,10));
    assert(badge_resource_raw_allowed(0xD1,0x200000,10));
    assert(!badge_resource_raw_allowed(0xD0,BR_START-1,2)); /* erase-rounding protected */
    at(e.address,e.length)[500]^=1;
    assert(badge_resource_commit()==BR_BAD_CRC); /* header still valid, bad pixels */
    reboot(); assert(badge_resource_find(BR_WALLPAPER,&address,&length)==-1);
    assert(!badge_resource_raw_allowed(0xD1,e.address,10));
    e=wallpaper(0x2A0000,0x38);
    assert(badge_resource_begin(&e)==BR_OK && badge_resource_commit()==BR_OK);
    reboot(); assert(badge_resource_find(BR_WALLPAPER,&address,&length)==1);
    before=read_bytes;
    assert(badge_resource_find(BR_WALLPAPER,&address,&length)==1 && before==read_bytes); /* CRC cached */
    at(e.address,e.length)[e.length-1]^=1;
    reboot(); assert(badge_resource_find(BR_WALLPAPER,&address,&length)==-1);
    /* A disconnect after BEGIN keeps the durable unavailable entry. */
    e=wallpaper(0x2A0000,0x39); assert(badge_resource_begin(&e)==BR_OK);
    badge_resource_abort(); assert(badge_resource_commit()==BR_NO_SESSION);
    reboot(); assert(badge_resource_find(BR_WALLPAPER,&address,&length)==-1);
    /* Invalid geometry cannot even start catalogue mutation. */
    before=(unsigned)operation; bad=e; bad.address=BR_TABLE_A;
    assert(badge_resource_begin(&bad)==BR_BAD_VALUE);
    bad=e; bad.length=0xffffffffu; assert(badge_resource_begin(&bad)==BR_BAD_VALUE);
    bad=e; ++bad.address; assert(badge_resource_begin(&bad)==BR_BAD_VALUE);
    bad=e; bad.id=BR_FONTS; assert(badge_resource_begin(&bad)==BR_BAD_VALUE);
    assert(before==(unsigned)operation);
}
static void check_headers_and_wrap(void)
{
    br_entry e;
    br_table t;
    uint32_t address, length;
    clean(); e=wallpaper(0x2A0000,7);
    assert(br_publish(&e)==BR_OK);
    memcpy(&t,at(BR_TABLE_A,56),56);
    t.generation=0xffffffffu; t.crc=br_table_crc(&t);
    memcpy(at(BR_TABLE_A,56),&t,56); reboot();
    assert(badge_resource_generation()==0xffffffffu);
    assert(br_publish(&e)==BR_OK); reboot();
    assert(badge_resource_generation()==0 && br_active_address==BR_TABLE_B);
    /* Even a CRC-correct table cannot point at firmware, the other type, or A/B. */
    t=br_current; t.entry[0].address=BR_TABLE_A; t.crc=br_table_crc(&t);
    memcpy(at(BR_TABLE_B,56),&t,56); reboot();
    assert(badge_resource_generation()==0xffffffffu);
    read_fail=1; br_loaded=0;
    assert(badge_resource_find(BR_WALLPAPER,&address,&length)==-1);
    assert(badge_resource_begin(&e)==BR_IO);
    read_fail=0;
    /* Header semantics are checked even when the outer data CRC matches. */
    clean(); e=wallpaper(0x2A0000,9); at(e.address,e.length)[4]=0xff;
    e.crc=~br_crc_update(~0u,at(e.address,e.length),e.length);
    assert(badge_resource_begin(&e)==BR_OK && badge_resource_commit()==BR_BAD_CRC);
}
static void check_wire(void)
{
    br_entry e;
    uint8_t p[30]={'B','R',1,4,0x12,0x34};
    uint8_t raw[8]={0,0,0x2A,0,16,0,0,0};
    clean(); e=wallpaper(0x2A0000,4); reply_length=0;
    update_open=0;
    assert(badge_resource_command(0x94,p,6) && reply[6]==BR_INACTIVE && operation==0);
    update_open=1; reply_length=0;
    assert(!badge_resource_command(0x93,p,6));
    assert(badge_resource_command(0x94,p,0) && !reply_length);
    assert(badge_resource_command(0x94,p,6) && operation==0);
    assert(reply_command==0x95 && reply_length==11 && reply[3]==0x84 &&
           reply[4]==0x12 && reply[5]==0x34 && reply[6]==BR_OK);
    assert(badge_resource_protect_raw(0xD0,raw,8) && reply[0]==2 && reply_command==0xD8);
    p[3]=1; memcpy(p+6,&e,sizeof(e));
    assert(badge_resource_command(0x94,p,29) && reply[6]==BR_BAD_PACKET && operation==0);
    p[2]=2; assert(badge_resource_command(0x94,p,30) && reply[6]==BR_BAD_PACKET && operation==0);
    p[2]=1; refuse_lock=1;
    assert(badge_resource_command(0x94,p,30) && reply[6]==BR_INACTIVE && operation==0);
    refuse_lock=0; assert(badge_resource_command(0x94,p,30) && reply[6]==BR_OK && g_badge_update_active);
    assert(!badge_resource_protect_raw(0xD0,raw,8));
    raw[2]=0x2E; assert(badge_resource_protect_raw(0xD0,raw,8)); /* tables are never raw-writable */
    p[3]=2; assert(badge_resource_command(0x94,p,6) && reply[6]==BR_OK && br_u32(reply+7)==2);
    assert(badge_resource_command(0x94,p,6) && reply[6]==BR_NO_SESSION);
    p[3]=3; assert(badge_resource_command(0x94,p,6) && reply[6]==BR_OK);
}
int main(void)
{
    assert(~br_crc_update(~0u,"123456789",9)==0xCBF43926u);
    check_power_cuts(); check_upload(); check_headers_and_wrap(); check_wire();
    printf("Resources: A/B torn erase/program/commit, migration, CRC, bounds, wrap OK; static state %u bytes\n",
           (unsigned)(sizeof(br_current)+sizeof(br_upload)+sizeof(br_active_address)+3));
    return 0;
}
