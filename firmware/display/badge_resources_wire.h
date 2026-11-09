/* Included after the implementation and qix_send_frame in the E87 Qix server.
 * Kept separate so the exact wire dispatcher runs in the fake-flash harness. */
#ifndef BADGE_RESOURCES_WIRE_H
#define BADGE_RESOURCES_WIRE_H
extern int badge_resources_update_open(void);
extern int badge_resources_lock_update(void);
static int badge_resource_command(uint8_t cmd, const uint8_t *p, uint16_t n)
{
    uint8_t reply[11] = {'B','R',1,0,0,0,BR_BAD_PACKET};
    uint32_t generation;
    int status=BR_BAD_PACKET;
    unsigned i;
    br_entry entry;
    if (cmd != 0x94) return 0;
    if (n<6 || p[0]!='B' || p[1]!='R') return 1;
    reply[3]=p[3]|0x80; reply[4]=p[4]; reply[5]=p[5];
    if (!badge_resources_update_open()) status=BR_INACTIVE;
    else if (p[2]==1) {
        if (p[3]==1 && n==30) {
            entry.id=br_u32(p+6); entry.format=br_u32(p+10);
            entry.address=br_u32(p+14); entry.length=br_u32(p+18);
            entry.crc=br_u32(p+22); entry.reserved=br_u32(p+26);
            if (!br_entry_valid(&entry) || !entry.length) status=BR_BAD_VALUE;
            else if (br_upload.active) status=BR_BUSY;
            else if (!badge_resources_lock_update()) status=BR_INACTIVE;
            else status=badge_resource_begin(&entry);
        } else if (p[3]==2 && n==6) status=badge_resource_commit();
        else if (p[3]==3 && n==6) { badge_resource_abort(); status=BR_OK; }
        else if (p[3]==4 && n==6) status=BR_OK;
    }
    reply[6]=(uint8_t)status; generation=badge_resource_generation();
    for (i=0;i<4;++i) reply[7+i]=(uint8_t)(generation>>(8*i));
    qix_send_frame(0x95,reply,sizeof(reply));
    return 1;
}

static int badge_resource_protect_raw(uint8_t cmd, const uint8_t *p, uint16_t n)
{
    uint32_t address, length;
    uint8_t reply[5]={2,0,0,0,0};
    if (cmd!=0xD0 && cmd!=0xD1 && cmd!=0xD4) return 0;
    address=n>=4 ? br_u32(p) : 0;
    length=cmd==0xD1 ? (n>=4 ? (uint32_t)n-4u : 0u) : (n==8 ? br_u32(p+4) : 0);
    if (badge_resources_update_open() && badge_resource_raw_allowed(cmd,address,length)) return 0;
    qix_send_frame(cmd==0xD0 ? 0xD8 : cmd==0xD1 ? 0xD9 : 0xDC,reply,sizeof(reply));
    return 1;
}
#endif
