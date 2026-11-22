/* Builders for valid Art-Net 4 and E1.31 packets, shared by the unit tests
 * and the fuzz seed generator. Each returns the packet length. */
#pragma once
#include <stdint.h>
#include <string.h>

static inline void pk_be16(uint8_t *b, uint16_t v) { b[0] = v >> 8; b[1] = v & 0xFF; }
static inline void pk_be32(uint8_t *b, uint32_t v)
{
    b[0] = v >> 24; b[1] = (v >> 16) & 0xFF; b[2] = (v >> 8) & 0xFF; b[3] = v & 0xFF;
}

/* ---- Art-Net ------------------------------------------------------------ */

static inline void pk_art_header(uint8_t *b, uint16_t opcode)
{
    memcpy(b, "Art-Net\0", 8);
    b[8] = opcode & 0xFF;          /* OpCode is little-endian */
    b[9] = opcode >> 8;
    b[10] = 0;                     /* ProtVerHi */
    b[11] = 14;                    /* ProtVerLo */
}

static inline int pk_artdmx(uint8_t *b, uint8_t seq, uint16_t port_address,
                            const uint8_t *data, uint16_t len)
{
    pk_art_header(b, 0x5000);
    b[12] = seq;
    b[13] = 0;                     /* Physical */
    b[14] = port_address & 0xFF;   /* SubUni */
    b[15] = (port_address >> 8) & 0x7F;   /* Net */
    pk_be16(&b[16], len);          /* Length is big-endian */
    if (len) memcpy(&b[18], data, len);
    return 18 + len;
}

/* flags: TalkToMe. targeted: Art-Net 4 targeted mode (TalkToMe bit 5) with
 * TargetPortAddressTop/Bottom; the packet is then the full 22 bytes. */
static inline int pk_artpoll(uint8_t *b, uint8_t flags, int targeted,
                             uint16_t top, uint16_t bottom)
{
    memset(b, 0, 22);
    pk_art_header(b, 0x2000);
    b[12] = flags | (targeted ? 0x20 : 0);
    b[13] = 0;                     /* DiagPriority */
    if (!targeted) return 14;
    pk_be16(&b[14], top);
    pk_be16(&b[16], bottom);
    return 22;
}

typedef struct {
    uint8_t net;                   /* NetSwitch (bit 7 = program) */
    uint8_t bind_index;
    const char *short_name;        /* NULL = leave the field empty (no change) */
    const char *long_name;
    uint8_t swin;                  /* SwIn[0] */
    uint8_t swout;                 /* SwOut[0] */
    uint8_t sub;                   /* SubSwitch */
    uint8_t acn_priority;
    uint8_t command;
} pk_artaddress_t;

static inline int pk_artaddress(uint8_t *b, const pk_artaddress_t *a)
{
    memset(b, 0, 107);
    pk_art_header(b, 0x6000);
    b[12] = a->net;
    b[13] = a->bind_index;
    if (a->short_name) strncpy((char *)&b[14], a->short_name, 18);
    if (a->long_name) strncpy((char *)&b[32], a->long_name, 64);
    b[96] = a->swin;
    b[100] = a->swout;
    b[104] = a->sub;
    b[105] = a->acn_priority;
    b[106] = a->command;
    return 107;
}

static inline int pk_artsync(uint8_t *b)
{
    pk_art_header(b, 0x5200);
    b[12] = b[13] = 0;             /* Aux1, Aux2 */
    return 14;
}

/* ---- E1.31 -------------------------------------------------------------- */

#define PK_SACN_HDR 125            /* bytes before the DMP start code */

static inline void pk_sacn_root(uint8_t *b, int total, uint32_t vector, const uint8_t cid[16])
{
    pk_be16(&b[0], 0x0010);        /* preamble */
    pk_be16(&b[2], 0x0000);        /* postamble */
    memcpy(&b[4], "ASC-E1.17\0\0\0", 12);
    pk_be16(&b[16], 0x7000 | (uint16_t)(total - 16));
    pk_be32(&b[18], vector);
    memcpy(&b[22], cid, 16);
}

typedef struct {
    const uint8_t *cid;            /* 16 bytes */
    const char *name;
    uint8_t priority;
    uint16_t sync_address;
    uint8_t sequence;
    uint8_t options;
    uint16_t universe;
    uint8_t start_code;
} pk_sacn_t;

static inline int pk_sacn_data(uint8_t *b, const pk_sacn_t *s, const uint8_t *data, uint16_t slots)
{
    int total = PK_SACN_HDR + 1 + slots;
    memset(b, 0, (size_t)total);
    pk_sacn_root(b, total, 0x00000004, s->cid);
    /* framing layer */
    pk_be16(&b[38], 0x7000 | (uint16_t)(total - 38));
    pk_be32(&b[40], 0x00000002);
    if (s->name) strncpy((char *)&b[44], s->name, 64);
    b[108] = s->priority;
    pk_be16(&b[109], s->sync_address);
    b[111] = s->sequence;
    b[112] = s->options;
    pk_be16(&b[113], s->universe);
    /* DMP layer */
    pk_be16(&b[115], 0x7000 | (uint16_t)(total - 115));
    b[117] = 0x02;                 /* vector */
    b[118] = 0xA1;                 /* address & data type */
    pk_be16(&b[119], 0);           /* first property address */
    pk_be16(&b[121], 1);           /* address increment */
    pk_be16(&b[123], (uint16_t)(slots + 1));
    b[125] = s->start_code;
    if (slots) memcpy(&b[126], data, slots);
    return total;
}

/* E1.31 universe discovery page listing @p count universes */
static inline int pk_sacn_discovery(uint8_t *b, const uint8_t cid[16], const char *name,
                                    uint8_t page, uint8_t last_page,
                                    const uint16_t *universes, int count)
{
    int total = 120 + 2 * count;
    memset(b, 0, (size_t)total);
    pk_sacn_root(b, total, 0x00000008, cid);
    pk_be16(&b[38], (uint16_t)(0x7000 | (total - 38)));
    pk_be32(&b[40], 0x00000002);   /* VECTOR_E131_EXTENDED_DISCOVERY */
    strncpy((char *)&b[44], name, 64);
    pk_be16(&b[112], (uint16_t)(0x7000 | (total - 112)));
    pk_be32(&b[114], 0x00000001);  /* VECTOR_UNIVERSE_DISCOVERY_UNIVERSE_LIST */
    b[118] = page;
    b[119] = last_page;
    for (int i = 0; i < count; i++) pk_be16(&b[120 + 2 * i], universes[i]);
    return total;
}

static inline int pk_sacn_sync(uint8_t *b, const uint8_t cid[16], uint8_t seq, uint16_t sync_universe)
{
    memset(b, 0, 49);
    pk_sacn_root(b, 49, 0x00000008, cid);
    pk_be16(&b[38], 0x7000 | (49 - 38));
    pk_be32(&b[40], 0x00000001);   /* VECTOR_E131_EXTENDED_SYNCHRONIZATION */
    b[44] = seq;
    pk_be16(&b[45], sync_universe);
    return 49;
}
