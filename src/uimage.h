/* uimage.h - U-Boot legacy uImage inspection (tboot.bin) and the ZTE
 * tloader.bin container. Both are read-only helpers used by `zxdl info`. */
#ifndef ZXDL_UIMAGE_H
#define ZXDL_UIMAGE_H

#include <stddef.h>
#include <stdint.h>

#define UIMAGE_MAGIC 0x27051956u

typedef struct {
    uint32_t magic, hcrc, time, size, load, ep, dcrc;
    uint8_t  os, arch, type, comp;
    char     name[33];
    int      hcrc_ok, dcrc_ok;
} uimage_t;

/* Returns 0 if buf holds a valid uImage header. */
int uimage_parse(const void *buf, size_t len, uimage_t *u);
void uimage_print(const uimage_t *u);

/* tloader.bin container:
 *   0x00 u32  type
 *   0x04 char chip[16]      e.g. "ZX7521V1"
 *   0x0C u32  payload_len   (overlaps chip[8..11]; the ID is 8 chars)
 *   0x8C u32  0x00010001
 *   0x90 256  RSA-2048 signature
 *   0x190     4 bytes padding
 *   0x194     payload (ARM Cortex-M vector table) */
typedef struct {
    uint32_t type;
    char     chip[9];
    uint32_t payload_len;
    uint32_t tag;
    uint32_t initial_sp;
    uint32_t reset_handler;
    int      size_consistent;
} tloader_t;

#define TLOADER_HDR_SIZE 0x194

int tloader_parse(const void *buf, size_t len, tloader_t *t);
void tloader_print(const tloader_t *t);

#endif /* ZXDL_UIMAGE_H */
