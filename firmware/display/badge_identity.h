/* Optional device-local identity. This record is provisioned separately from
 * firmware/source, in a sector after the existing 144x144 wallpaper slot. */
#ifndef BADGE_IDENTITY_H
#define BADGE_IDENTITY_H
#include <stdint.h>
#include <string.h>
#include "badge_resources.h"
#define BADGE_IDENTITY_ADDR 0x30B000u
#define BADGE_IDENTITY_SIZE 32
#define BADGE_NAME_MAX 20
static char badge_private_mouse_name[BADGE_NAME_MAX + 7];

static uint16_t badge_identity_crc(const uint8_t *p, unsigned n, uint16_t crc)
{
    unsigned i;
    while (n--) {
        crc ^= (uint16_t)*p++ << 8;
        for (i = 0; i < 8; ++i)
            crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    return crc;
}

static int badge_identity_parse(const uint8_t *p, char *name)
{
    unsigned i, n = p[5];
    uint16_t crc;
    if (memcmp(p, "BID1", 4) || p[4] != 1 || !n || n > BADGE_NAME_MAX) return 0;
    for (i = 0; i < n; ++i) if (p[8+i] < 32 || p[8+i] > 126) return 0;
    crc = badge_identity_crc(p+4, 2, 0xffff);
    crc = badge_identity_crc(p+8, n, crc);
    if (crc != ((uint16_t)p[6] | ((uint16_t)p[7] << 8))) return 0;
    memcpy(name, p+8, n); name[n] = 0;
    return 1;
}

static void badge_identity_load(char *name)
{
    extern int tzflash_read(void *buf, unsigned int len, unsigned int offset);
    uint8_t record[BADGE_IDENTITY_SIZE];
    char local[BADGE_NAME_MAX+1];
    uint32_t address=BADGE_IDENTITY_ADDR, length=BADGE_IDENTITY_SIZE;
    int resource=badge_resource_find(BR_IDENTITY, &address, &length);
    if (resource<0 || length!=sizeof(record) || tzflash_read(record, sizeof(record), address) < 0 ||
        !badge_identity_parse(record, local)) return;
    strcpy(name, local); /* SDK LOCAL_NAME_LEN is 32; local is at most 20 bytes. */
    strcpy(badge_private_mouse_name, local);
    strcat(badge_private_mouse_name, " Mouse");
}

const char *badge_mouse_local_name(void)
{
    return badge_private_mouse_name[0] ? badge_private_mouse_name : "EC-MOUSE";
}
#endif
