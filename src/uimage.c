#include "uimage.h"
#include "crc32.h"

#include <stdio.h>
#include <string.h>

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int uimage_parse(const void *buf, size_t len, uimage_t *u)
{
    const uint8_t *p = buf;
    uint8_t hdr[64];

    if (len < 64)
        return -1;
    memset(u, 0, sizeof(*u));

    u->magic = be32(p + 0);
    if (u->magic != UIMAGE_MAGIC)
        return -1;

    u->hcrc = be32(p + 4);
    u->time = be32(p + 8);
    u->size = be32(p + 12);
    u->load = be32(p + 16);
    u->ep   = be32(p + 20);
    u->dcrc = be32(p + 24);
    u->os   = p[28];
    u->arch = p[29];
    u->type = p[30];
    u->comp = p[31];
    memcpy(u->name, p + 32, 32);
    u->name[32] = '\0';

    /* header CRC is computed with the hcrc field zeroed, standard CRC32 */
    memcpy(hdr, p, 64);
    memset(hdr + 4, 0, 4);
    u->hcrc_ok = (std_crc32(hdr, 64) == u->hcrc);

    if (len >= 64 + (size_t)u->size)
        u->dcrc_ok = (std_crc32(p + 64, u->size) == u->dcrc);
    else
        u->dcrc_ok = -1;

    return 0;
}

void uimage_print(const uimage_t *u)
{
    printf("uImage\n");
    printf("  name        %s\n", u->name);
    printf("  data size   %u bytes\n", u->size);
    printf("  load addr   0x%08X\n", u->load);
    printf("  entry point 0x%08X\n", u->ep);
    printf("  os/arch     %u / %u   type %u   compression %u\n",
           u->os, u->arch, u->type, u->comp);
    printf("  header CRC  0x%08X  %s\n", u->hcrc, u->hcrc_ok ? "OK" : "BAD");
    if (u->dcrc_ok < 0)
        printf("  data CRC    0x%08X  (truncated file)\n", u->dcrc);
    else
        printf("  data CRC    0x%08X  %s\n", u->dcrc, u->dcrc_ok ? "OK" : "BAD");
}

int tloader_parse(const void *buf, size_t len, tloader_t *t)
{
    const uint8_t *p = buf;

    if (len < TLOADER_HDR_SIZE + 8)
        return -1;
    memset(t, 0, sizeof(*t));

    t->type = le32(p + 0x00);
    memcpy(t->chip, p + 0x04, 8);
    t->chip[8] = '\0';
    t->payload_len = le32(p + 0x0C);
    t->tag = le32(p + 0x8C);
    t->initial_sp    = le32(p + TLOADER_HDR_SIZE + 0);
    t->reset_handler = le32(p + TLOADER_HDR_SIZE + 4);
    t->size_consistent =
        ((size_t)TLOADER_HDR_SIZE + t->payload_len == len);

    /* Sanity: chip ID should be printable */
    for (int i = 0; i < 8; i++)
        if (t->chip[i] && (t->chip[i] < 0x20 || t->chip[i] > 0x7E))
            return -1;
    return 0;
}

void tloader_print(const tloader_t *t)
{
    printf("tloader container\n");
    printf("  type        0x%08X\n", t->type);
    printf("  chip id     %s\n", t->chip);
    printf("  payload     %u bytes  (header 0x%X)  %s\n",
           t->payload_len, TLOADER_HDR_SIZE,
           t->size_consistent ? "size OK" : "SIZE MISMATCH");
    printf("  tag @0x8C   0x%08X\n", t->tag);
    printf("  signature   256 bytes at 0x90 (RSA-2048, not verified here)\n");
    printf("  initial SP  0x%08X\n", t->initial_sp);
    printf("  reset       0x%08X  (Thumb: 0x%08X)\n",
           t->reset_handler, t->reset_handler & ~1u);
    printf("  => load address looks like 0x%08X\n",
           t->reset_handler & 0xFFFFF000u);
}
