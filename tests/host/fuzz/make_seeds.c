/* Writes the binary seed corpus (valid Art-Net and E1.31 packets) into
 * <dir>/artnet and <dir>/sacn:   make seeds   (fuzz/seeds is committed) */
#include "packets.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

static const char *dir;

static void put(const char *sub, const char *name, const uint8_t *b, int len)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, sub);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/%s/%s", dir, sub, name);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(b, 1, (size_t)len, f) != (size_t)len) {
        perror(path);
        exit(1);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <seed dir>\n", argv[0]);
        return 2;
    }
    dir = argv[1];
    static uint8_t b[700], lv[512];
    for (int i = 0; i < 512; i++) lv[i] = (uint8_t)i;

    /* ---- Art-Net ---- */
    put("artnet", "artdmx-512", b, pk_artdmx(b, 1, 0x0001, lv, 512));
    put("artnet", "artdmx-24", b, pk_artdmx(b, 0, 0x7FFF, lv, 24));
    put("artnet", "artpoll", b, pk_artpoll(b, 0x02, 0, 0, 0));
    put("artnet", "artpoll-targeted", b, pk_artpoll(b, 0x00, 1, 0x0100, 0x0000));
    pk_artaddress_t a = { .net = 0x81, .bind_index = 1, .short_name = "Node", .long_name = "Long name",
                          .swin = 0x7F, .swout = 0x82, .sub = 0x80, .acn_priority = 100,
                          .command = 0x04 };
    put("artnet", "artaddress", b, pk_artaddress(b, &a));
    put("artnet", "artsync", b, pk_artsync(b));

    memset(b, 0, sizeof(b));
    pk_art_header(b, 0x8000);              /* ArtTodRequest */
    b[23] = 2; b[24] = 0x00; b[25] = 0x01;
    put("artnet", "arttodrequest", b, 26);
    memset(b, 0, sizeof(b));
    pk_art_header(b, 0x8200);              /* ArtTodControl */
    b[22] = 0x01;
    put("artnet", "arttodcontrol", b, 24);
    memset(b, 0, sizeof(b));
    pk_art_header(b, 0x8300);              /* ArtRdm: GET DEVICE_INFO */
    static const uint8_t rdm[] = {0x01, 0x18, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                  0x7F, 0xF0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0x20, 0x00, 0x60, 0x00,
                                  0x04, 0x4B};
    b[12] = 0x01;
    memcpy(&b[24], rdm, sizeof(rdm));
    put("artnet", "artrdm", b, 24 + (int)sizeof(rdm));
    memset(b, 0, sizeof(b));
    pk_art_header(b, 0xF800);              /* ArtIpProg: set IP + mask */
    b[14] = 0x86;
    memcpy(&b[16], (uint8_t[]){2, 0, 0, 20}, 4);
    memcpy(&b[20], (uint8_t[]){255, 0, 0, 0}, 4);
    put("artnet", "artipprog", b, 34);

    /* ---- E1.31 ---- */
    static const uint8_t cid[16] = {0x5A, 0x1C, 0xE0, 0x01, 0x02, 0x03, 0x04, 0x05,
                                    0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D};
    pk_sacn_t s = { .cid = cid, .name = "Fuzz Console", .priority = 100, .sequence = 1,
                    .universe = 1, .start_code = 0x00 };
    put("sacn", "data-512", b, pk_sacn_data(b, &s, lv, 512));
    s.universe = 63999; s.sync_address = 7; s.priority = 200;
    put("sacn", "data-24-sync", b, pk_sacn_data(b, &s, lv, 24));
    s.start_code = 0xDD; s.universe = 1; s.sync_address = 0; s.priority = 100;
    put("sacn", "pap-512", b, pk_sacn_data(b, &s, lv, 512));
    s.start_code = 0x00; s.options = 0x40 | 0x80;
    put("sacn", "terminated-preview", b, pk_sacn_data(b, &s, lv, 0));
    put("sacn", "sync", b, pk_sacn_sync(b, cid, 3, 7));
    pk_sacn_sync(b, cid, 3, 7);
    pk_be32(&b[40], 0x00000002);           /* universe discovery */
    put("sacn", "discovery", b, 49);
    return 0;
}
