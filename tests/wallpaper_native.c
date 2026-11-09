/* Exercise the actual streaming decoder without SDK/radio or a full-image RAM buffer. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int lv_res_t;
typedef int16_t lv_coord_t;
typedef struct { unsigned always_zero,w,h,cf; } lv_img_header_t;
typedef struct { lv_img_header_t header; unsigned data_size; const uint8_t *data; } lv_img_dsc_t;
typedef struct { const void *src; lv_img_header_t header; const uint8_t *img_data; } lv_img_decoder_dsc_t;
typedef struct { int unused; } lv_img_decoder_t;
#define LV_RES_OK 0
#define LV_RES_INV 1
#define LV_IMG_CF_TRUE_COLOR 4
#define LV_IMG_CF_USER_ENCODED_0 18
static lv_img_decoder_t decoder;
static unsigned creates, reads, max_read;
static int fail_read, fail_create;
static lv_img_decoder_t *lv_img_decoder_create(void) { ++creates; return fail_create ? NULL : &decoder; }
static void lv_img_decoder_set_info_cb(lv_img_decoder_t *d, lv_res_t (*f)(lv_img_decoder_t *,const void *,lv_img_header_t *)) { (void)d;(void)f; }
static void lv_img_decoder_set_open_cb(lv_img_decoder_t *d, lv_res_t (*f)(lv_img_decoder_t *,lv_img_decoder_dsc_t *)) { (void)d;(void)f; }
static void lv_img_decoder_set_read_line_cb(lv_img_decoder_t *d, lv_res_t (*f)(lv_img_decoder_t *,lv_img_decoder_dsc_t *,lv_coord_t,lv_coord_t,lv_coord_t,uint8_t *)) { (void)d;(void)f; }
#include "badge_wallpaper.h"
static int resource_state;
int badge_resource_find(uint32_t id, uint32_t *address, uint32_t *length)
{
    assert(id==BR_WALLPAPER);
    if(resource_state>0) { *address=BADGE_WP_SLOT; *length=BADGE_WP_SIZE; }
    return resource_state;
}
static badge_wp_header full_header, legacy_header;
int tzflash_read(void *buf, unsigned n, unsigned address)
{
    unsigned base, width, i, offset;
    ++reads; if(n>max_read)max_read=n;
    if(fail_read) return -1;
    if(address==BADGE_WP_SLOT && n==16) { memcpy(buf,&full_header,16); return n; }
    if(address==BADGE_WP_LEGACY_SLOT && n==16) { memcpy(buf,&legacy_header,16); return n; }
    if(address>=BADGE_WP_SLOT+16 && address+n<=BADGE_WP_SLOT+BADGE_WP_SIZE) {
        base=BADGE_WP_SLOT; width=full_header.w;
    } else {
        assert(address>=BADGE_WP_LEGACY_SLOT+16 && address+n<=BADGE_WP_LEGACY_SLOT+16+144*144*2);
        base=BADGE_WP_LEGACY_SLOT; width=legacy_header.w;
    }
    assert(width && n<=720 && !(n%2));
    offset=(address-base-16)/2;
    assert(offset/width==(offset+n/2-1)/width); /* never cross a source row */
    for(i=0;i<n/2;++i) {
        uint16_t pixel=(uint16_t)(offset+i);
        ((uint8_t *)buf)[2*i]=pixel; ((uint8_t *)buf)[2*i+1]=pixel>>8;
    }
    return n;
}
static uint16_t pixel(const uint8_t *p) { return p[0] | (uint16_t)p[1]<<8; }
int main(void)
{
    uint8_t row[720];
    lv_img_decoder_dsc_t session={0};
    unsigned i, before;
    assert(sizeof(badge_wp_header)==16 && sizeof(badge_wp_row)==720);
    assert(lock_img_resolve()==NULL && creates==0);
    full_header=(badge_wp_header){BADGE_WP_MAGIC,360,360,259200,0};
    resource_state=-1; assert(lock_img_resolve()==NULL); /* no unchecked legacy revival */
    resource_state=1; assert(lock_img_resolve()==&badge_wp_source);
    resource_state=0;
    assert(lock_img_resolve()==&badge_wp_source && creates==1);
    assert(badge_wp_source.header.w==360 && badge_wp_source.header.h==360);
    assert(badge_wp_source.data && badge_wp_source.data_size==4 && badge_wp_source.header.cf==LV_IMG_CF_USER_ENCODED_0);
    session.src=&badge_wp_source;
    assert(badge_wp_open(&decoder,&session)==LV_RES_OK && session.img_data==NULL);
    assert(session.header.cf==LV_IMG_CF_TRUE_COLOR);
    for(i=0;i<360;++i) {
        assert(badge_wp_read_line(&decoder,&session,0,i,360,row)==LV_RES_OK);
        assert(pixel(row)==(uint16_t)(i*360));
        assert(pixel(row+718)==(uint16_t)(i*360+359));
    }
    assert(badge_wp_read_line(&decoder,&session,350,359,10,row)==LV_RES_OK);
    assert(pixel(row)==(uint16_t)(359*360+350));
    before=reads;
    assert(badge_wp_read_line(&decoder,&session,-1,0,1,row)==LV_RES_INV);
    assert(badge_wp_read_line(&decoder,&session,359,0,2,row)==LV_RES_INV);
    assert(badge_wp_read_line(&decoder,&session,0,360,1,row)==LV_RES_INV);
    assert(badge_wp_read_line(&decoder,&session,0,0,0,row)==LV_RES_INV);
    assert(badge_wp_read_line(&decoder,&session,0,0,360,NULL)==LV_RES_INV);
    assert(reads==before);
    fail_read=1; assert(badge_wp_read_line(&decoder,&session,0,0,1,row)==LV_RES_INV); fail_read=0;
    full_header.w=361; /* invalid new record -> old 144px wallpaper still works */
    legacy_header=(badge_wp_header){BADGE_WP_MAGIC,144,144,41472,0};
    assert(lock_img_resolve()==&badge_wp_source && creates==1);
    assert(badge_wp_read_line(&decoder,&session,0,180,360,row)==LV_RES_OK);
    assert(pixel(row)==72*144 && pixel(row+360)==72*144+72);
    for(i=0;i<10000;++i) {
        badge_wp_header bad={BADGE_WP_MAGIC,(uint16_t)(i%1024),(uint16_t)((i*7)%1024),i,0};
        if(badge_wp_valid(&bad,360)) assert(bad.w<=360 && bad.h<=360 && bad.size==(unsigned)bad.w*bad.h*2);
    }
    legacy_header.magic=0; assert(lock_img_resolve()==NULL);
    full_header=(badge_wp_header){BADGE_WP_MAGIC,360,360,259200,0};
    badge_wp_decoder=NULL; fail_create=1; assert(lock_img_resolve()==NULL);
    assert(max_read==720);
    puts("Wallpaper: full resolution, legacy fallback, bounds, failed reads, bounded RAM OK");
}
