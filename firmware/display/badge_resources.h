/* E87 resource catalogue. Implementation lives once in qix_ota_server.c.
 * On-flash words are little-endian (E87/pi32v2 and the native test targets).
 * A/B commit is last; no full-resource RAM buffer or dynamic allocation. */
#ifndef BADGE_RESOURCES_H
#define BADGE_RESOURCES_H
#include <stdint.h>
#include <string.h>
#define BR_TABLE_A 0x2E0000u
#define BR_TABLE_B 0x2E1000u
#define BR_SECTOR 4096u
#define BR_START 0x2A0000u
#define BR_END 0x33E000u
#define BR_WALLPAPER 1u
#define BR_FONTS 2u
#define BR_IDENTITY 3u
#define BR_MAX_ENTRIES 3u
#define BR_MAGIC 0x31435242u /* BRC1 */
#define BR_COMMIT 0x454E4F44u /* DONE */
enum { BR_OK, BR_BAD_PACKET, BR_BAD_VALUE, BR_BUSY, BR_IO, BR_BAD_CRC, BR_NO_SESSION, BR_INACTIVE };
typedef struct { uint32_t id, format, address, length, crc, reserved; } br_entry;
typedef struct {
    uint32_t magic, generation, count, length, crc, reserved[2], commit;
    br_entry entry[BR_MAX_ENTRIES];
} br_table;
typedef char br_entry_is_24_bytes[sizeof(br_entry) == 24 ? 1 : -1];
typedef char br_table_is_104_bytes[sizeof(br_table) == 104 ? 1 : -1];

/* 1 = catalogue resource with verified full CRC; 0 = not registered (legacy
 * loader may try fixed addresses); -1 = registered but unavailable/corrupt. */
int badge_resource_find(uint32_t id, uint32_t *address, uint32_t *length);
int badge_resource_begin(const br_entry *entry);
int badge_resource_commit(void);
void badge_resource_abort(void);
uint32_t badge_resource_generation(void);
int badge_resource_raw_allowed(uint8_t command, uint32_t address, uint32_t length);

#ifdef BADGE_RESOURCES_IMPLEMENTATION
#if defined(TCFG_LCD_SPI_BADGE2_ENABLE) && TCFG_LCD_SPI_BADGE2_ENABLE
#error "The resource catalogue layout is E87-only; do not use it on DG01"
#endif
extern int tzflash_read(void *buf, unsigned int len, unsigned int offset);
extern int tzflash_write(const void *buf, unsigned int len, unsigned int offset);
extern int tzflash_ioctl(unsigned int cmd, unsigned int arg);
#ifndef BR_WATCHDOG
#define BR_WATCHDOG() ((void)0)
#endif
static br_table br_current;
static uint32_t br_active_address;
static uint8_t br_loaded, br_verified, br_failed;
static struct { br_entry entry; uint8_t active; } br_upload;

static uint32_t br_u32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }

static uint32_t br_crc_update(uint32_t crc, const void *data, unsigned n)
{
    const uint8_t *p = (const uint8_t *)data;
    unsigned i;
    while (n--) {
        crc ^= *p++;
        for (i=0; i<8; ++i) crc = (crc>>1) ^ (0xEDB88320u & (0u-(crc&1u)));
    }
    return crc;
}

static uint32_t br_table_crc(const br_table *t)
{
    uint32_t crc = br_crc_update(~0u, t, 16);
    crc = br_crc_update(crc, (const uint8_t *)t+20, 8);
    return ~br_crc_update(crc, t->entry, t->count*sizeof(br_entry));
}

static int br_entry_valid(const br_entry *e)
{
    uint32_t start, end, maximum;
    if (e->format != 1 || e->reserved || e->id < BR_WALLPAPER || e->id > BR_IDENTITY) return 0;
    if (!e->length) return !e->address && !e->crc; /* durable unavailable entry */
    if (e->id == BR_WALLPAPER) {
        start=0x2A0000u; end=0x2E0000u; maximum=259216u;
        if (e->address >= 0x300000u) { start=0x300000u; end=0x30B000u; maximum=41488u; }
    } else if (e->id == BR_FONTS) { start=0x310000u; end=0x33E000u; maximum=0x2E000u; }
    else { start=0x30B000u; end=0x30C000u; maximum=32; }
    return !(e->address & (BR_SECTOR-1)) && e->address >= start && e->address < end &&
           e->length <= maximum && e->length <= end-e->address;
}

static int br_read_table(uint32_t address, br_table *t, int committed)
{
    unsigned i;
    memset(t, 0, sizeof(*t));
    if (tzflash_read(t, 32, address) < 0) return -1;
    if (t->magic != BR_MAGIC || t->count > BR_MAX_ENTRIES || t->reserved[0] || t->reserved[1] ||
        t->length != 32+t->count*sizeof(br_entry) ||
        t->commit != (committed ? BR_COMMIT : ~0u)) return 0;
    if (t->count && tzflash_read(t->entry, t->count*sizeof(br_entry), address+32) < 0) return -1;
    for (i=0; i<t->count; ++i)
        if (!br_entry_valid(&t->entry[i]) || (i && t->entry[i-1].id >= t->entry[i].id)) return 0;
    return t->crc == br_table_crc(t);
}

static int br_load(void)
{
    br_table candidate;
    int a, b;
    if (br_loaded) return 1;
    a=br_read_table(BR_TABLE_A, &br_current, 1);
    b=br_read_table(BR_TABLE_B, &candidate, 1);
    if (a < 0 || b < 0) return 0; /* do not overwrite a catalogue hidden by an I/O failure */
    br_active_address = a ? BR_TABLE_A : 0;
    if (b && (!a || (int32_t)(candidate.generation-br_current.generation)>0)) {
        br_current=candidate; br_active_address=BR_TABLE_B;
    }
    if (!br_active_address) memset(&br_current, 0, sizeof(br_current));
    br_verified=br_failed=0; br_loaded=1;
    return 1;
}

static int br_data_crc(const br_entry *e)
{
    uint8_t buf[128];
    uint32_t offset=0, crc=~0u;
    unsigned n;
    while (offset < e->length) {
        n=e->length-offset < sizeof(buf) ? (unsigned)(e->length-offset) : sizeof(buf);
        if (tzflash_read(buf, n, e->address+offset) < 0) return 0;
        crc=br_crc_update(crc, buf, n); offset+=n; BR_WATCHDOG();
    }
    return ~crc == e->crc;
}

static int br_data_header(const br_entry *e)
{
    uint8_t h[16];
    uint32_t w, height, limit;
    if (e->length < 16 || tzflash_read(h, 16, e->address) < 0) return 0;
    if (e->id == BR_WALLPAPER) {
        w=(unsigned)h[4] | (unsigned)h[5]<<8; height=(unsigned)h[6] | (unsigned)h[7]<<8;
        limit=e->address >= 0x300000u ? 144 : 360;
        return !memcmp(h,"LKMG",4) && w>=16 && height>=16 && w<=limit && height<=limit &&
               br_u32(h+8)==w*height*2u && e->length==16u+w*height*2u;
    }
    if (e->id == BR_FONTS) return !memcmp(h,"BFN1",4) && h[4]==1 && br_u32(h+8)==e->length;
    return e->length==32 && !memcmp(h,"BID1",4) && h[4]==1 && h[5]>=1 && h[5]<=20;
}

int badge_resource_find(uint32_t id, uint32_t *address, uint32_t *length)
{
    unsigned i, bit;
    if (!br_load()) return -1;
    for (i=0; i<br_current.count; ++i) if (br_current.entry[i].id == id) {
        const br_entry *e=&br_current.entry[i]; bit=1u<<i;
        if (!e->length || (br_failed&bit)) return -1;
        if (!(br_verified&bit)) {
            if (!br_data_crc(e) || !br_data_header(e)) { br_failed|=(uint8_t)bit; return -1; }
            br_verified|=(uint8_t)bit;
        }
        *address=e->address; *length=e->length;
        return 1;
    }
    return 0;
}

/* Only the inactive sector is erased. Verify the uncommitted record, then write
 * DONE last and verify again. A torn program, including DONE, fails validation. */
static int br_publish(const br_entry *entry)
{
    br_table next, check;
    uint32_t target, marker=BR_COMMIT;
    unsigned i, j;
    if (!br_load()) return BR_IO;
    next=br_current;
    for (i=0; i<next.count && next.entry[i].id<entry->id; ++i) {}
    if (i == next.count || next.entry[i].id != entry->id) {
        if (next.count == BR_MAX_ENTRIES) return BR_BAD_VALUE;
        for (j=next.count; j>i; --j) next.entry[j]=next.entry[j-1];
        ++next.count;
    }
    next.entry[i]=*entry;
    next.magic=BR_MAGIC; ++next.generation; next.length=32+next.count*sizeof(br_entry);
    next.commit=~0u; next.crc=br_table_crc(&next);
    target=br_active_address==BR_TABLE_A ? BR_TABLE_B : BR_TABLE_A;
    if (tzflash_ioctl(200, target) != 0 || tzflash_write(&next, next.length, target)<0 ||
        br_read_table(target, &check, 0)!=1 || memcmp(&next,&check,next.length) ||
        tzflash_write(&marker,4,target+28)<0 || br_read_table(target,&check,1)!=1) {
        br_loaded=0; br_upload.active=0; return BR_IO;
    }
    br_current=check; br_active_address=target; br_verified=br_failed=0;
    return BR_OK;
}

int badge_resource_begin(const br_entry *entry)
{
    br_entry unavailable;
    int result;
    if (br_upload.active) return BR_BUSY;
    if (!br_entry_valid(entry) || !entry->length) return BR_BAD_VALUE;
    /* Invalidate BEFORE erasing payload. This also migrates an unregistered
     * legacy slot safely: after a torn first upload it must not be reinterpreted
     * as an unchecked legacy resource. No second payload slot is reserved. */
    unavailable=*entry; unavailable.address=unavailable.length=unavailable.crc=0;
    result=br_publish(&unavailable);
    if (result != BR_OK) return result;
    br_upload.entry=*entry; br_upload.active=1;
    return BR_OK;
}

int badge_resource_commit(void)
{
    int result;
    if (!br_upload.active) return BR_NO_SESSION;
    if (!br_data_crc(&br_upload.entry) || !br_data_header(&br_upload.entry)) return BR_BAD_CRC;
    result=br_publish(&br_upload.entry);
    br_upload.active=0;
    return result;
}

void badge_resource_abort(void) { br_upload.active=0; }
uint32_t badge_resource_generation(void) { return br_load() ? br_current.generation : 0; }

int badge_resource_raw_allowed(uint8_t command, uint32_t address, uint32_t length)
{
    uint32_t start=address, end;
    if (!length || address>=0x400000u || length>0x400000u-address) return 0;
    end=address+length;
    if (command==0xD0) { start &= ~(BR_SECTOR-1); end=(end+BR_SECTOR-1)&~(BR_SECTOR-1); }
    if (start>=BR_END || end<=BR_START) return 1;
    if (!br_upload.active || command==0xD4) return 0;
    if (command==0xD0)
        return start>=br_upload.entry.address && end<=((br_upload.entry.address+br_upload.entry.length+BR_SECTOR-1)&~(BR_SECTOR-1));
    return command==0xD1 && start>=br_upload.entry.address &&
           end<=br_upload.entry.address+br_upload.entry.length;
}
#endif /* implementation */
#endif
