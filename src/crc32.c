#include "crc32.h"

static uint32_t tbl[256];
static int tbl_ready;

static void build_table(void)
{
    for (unsigned i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
        tbl[i] = c;
    }
    tbl_ready = 1;
}

uint32_t zx_crc32_update(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = data;
    if (!tbl_ready)
        build_table();
    while (len--)
        crc = (crc >> 8) ^ tbl[(crc ^ *p++) & 0xFF];
    return crc;
}

uint32_t zx_crc32(const void *data, size_t len)
{
    /* deliberately no ^ 0xFFFFFFFF at the end - matches sub_403CC0 */
    return zx_crc32_update(ZX_CRC32_INIT, data, len);
}

uint32_t std_crc32(const void *data, size_t len)
{
    return zx_crc32_update(ZX_CRC32_INIT, data, len) ^ 0xFFFFFFFFu;
}

uint32_t zx_xorsum32(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t acc = 0;

    for (size_t i = 0; i + 4 <= len; i += 4) {
        uint32_t v = (uint32_t)p[i] | ((uint32_t)p[i + 1] << 8) |
                     ((uint32_t)p[i + 2] << 16) | ((uint32_t)p[i + 3] << 24);
        acc ^= v;
    }
    return acc;
}
