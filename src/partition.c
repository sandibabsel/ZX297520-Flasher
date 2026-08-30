#define _GNU_SOURCE
#include "partition.h"
#include "crc32.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void copy_fixed(char *dst, const uint8_t *src, size_t n)
{
    memcpy(dst, src, n);
    dst[n] = '\0';
}

size_t pt_image_size(const pt_table_t *t)
{
    return PT_HDR_SIZE + (size_t)t->count * PT_ENTRY_SIZE;
}

static int pt_parse_q(const void *buf, size_t len, pt_table_t *t, int quiet)
{
    const uint8_t *p = buf;
    uint32_t n;

    if (len < PT_HDR_SIZE) {
        if (!quiet)
            fprintf(stderr, "partition: image too small (%zu bytes)\n", len);
        return -1;
    }

    memset(t, 0, sizeof(*t));
    t->magic = rd32(p + 0x00);
    copy_fixed(t->platform, p + 0x04, PT_NAME_LEN);
    t->version  = rd32(p + 0x14);
    n           = rd32(p + 0x18);
    t->checksum = rd32(p + 0x1C);

    if (n == 0 || n > PT_MAX_ENTRIES) {
        if (!quiet)
            fprintf(stderr, "partition: implausible entry count %u\n", n);
        return -1;
    }
    if (len < PT_HDR_SIZE + (size_t)n * PT_ENTRY_SIZE) {
        if (!quiet)
            fprintf(stderr, "partition: truncated (need %zu, have %zu)\n",
                    PT_HDR_SIZE + (size_t)n * PT_ENTRY_SIZE, len);
        return -1;
    }
    t->count = n;

    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = p + PT_HDR_SIZE + (size_t)i * PT_ENTRY_SIZE;
        copy_fixed(t->entry[i].name, e + 0x00, PT_NAME_LEN);
        copy_fixed(t->entry[i].type, e + 0x10, PT_NAME_LEN);
        t->entry[i].addr = rd32(e + 0x20);
        t->entry[i].size = rd32(e + 0x24);
    }

    t->checksum_calc = zx_xorsum32(p + PT_HDR_SIZE, (size_t)n * PT_ENTRY_SIZE);
    return 0;
}

int pt_parse(const void *buf, size_t len, pt_table_t *t)
{
    return pt_parse_q(buf, len, t, 0);
}

int pt_probe(const void *buf, size_t len, pt_table_t *t)
{
    /* A real table starts with a printable platform string and a first
     * entry that has a name; use that to reject other containers. */
    if (pt_parse_q(buf, len, t, 1) < 0)
        return -1;
    if (!t->entry[0].name[0])
        return -1;
    for (const char *s = t->platform; *s; s++)
        if (*s < 0x20 || *s > 0x7E)
            return -1;
    return 0;
}

ssize_t pt_serialize(const pt_table_t *t, void *buf, size_t bufsz)
{
    uint8_t *p = buf;
    size_t need = pt_image_size(t);

    if (bufsz < need)
        return -1;
    memset(p, 0, need);

    wr32(p + 0x00, t->magic);
    memcpy(p + 0x04, t->platform, strnlen(t->platform, PT_NAME_LEN));
    wr32(p + 0x14, t->version);
    wr32(p + 0x18, t->count);

    for (uint32_t i = 0; i < t->count; i++) {
        uint8_t *e = p + PT_HDR_SIZE + (size_t)i * PT_ENTRY_SIZE;
        memcpy(e + 0x00, t->entry[i].name,
               strnlen(t->entry[i].name, PT_NAME_LEN));
        memcpy(e + 0x10, t->entry[i].type,
               strnlen(t->entry[i].type, PT_NAME_LEN));
        wr32(e + 0x20, t->entry[i].addr);
        wr32(e + 0x24, t->entry[i].size);
    }

    wr32(p + 0x1C, zx_xorsum32(p + PT_HDR_SIZE,
                               (size_t)t->count * PT_ENTRY_SIZE));
    return (ssize_t)need;
}

const pt_entry_t *pt_find(const pt_table_t *t, const char *name)
{
    for (uint32_t i = 0; i < t->count; i++)
        if (strcmp(t->entry[i].name, name) == 0)
            return &t->entry[i];
    return NULL;
}

void pt_print(const pt_table_t *t)
{
    printf("magic     0x%08X  (\"%.4s\")\n", t->magic, (const char *)&t->magic);
    printf("platform  \"%s\"\n", t->platform);
    printf("version   0x%08X\n", t->version);
    printf("entries   %u\n", t->count);
    printf("checksum  0x%08X stored / 0x%08X computed %s\n",
           t->checksum, t->checksum_calc,
           t->checksum == t->checksum_calc ? "OK" : "MISMATCH");
    printf("\n%-4s %-16s %-8s %-12s %-12s %s\n",
           "#", "name", "type", "addr", "size", "size");
    for (uint32_t i = 0; i < t->count; i++) {
        const pt_entry_t *e = &t->entry[i];
        char human[32];
        if (e->size == 0xFFFFFFFFu)
            snprintf(human, sizeof(human), "-");
        else if (e->size >= (1u << 20) && e->size % (1u << 20) == 0)
            snprintf(human, sizeof(human), "%u MiB", e->size >> 20);
        else if (e->size >= (1u << 10) && e->size % (1u << 10) == 0)
            snprintf(human, sizeof(human), "%u KiB", e->size >> 10);
        else
            snprintf(human, sizeof(human), "%u B", e->size);
        printf("%-4u %-16s %-8s 0x%08X   0x%08X   %s\n",
               i, e->name, e->type, e->addr, e->size, human);
    }
}

/* ---- INI loading ------------------------------------------------------- */

/* Copy at most cap-1 bytes and always NUL terminate, without the
 * format-truncation noise snprintf("%s") generates. */
static void copy_field(char *dst, size_t cap, const char *src)
{
    size_t n = strnlen(src, cap - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static char *trim(char *s)
{
    char *e;
    while (*s && isspace((unsigned char)*s))
        s++;
    if (!*s)
        return s;
    e = s + strlen(s) - 1;
    while (e > s && isspace((unsigned char)*e))
        *e-- = '\0';
    return s;
}

/* Accepts both "0x1234" and plain decimal. The original tool used
 * GetPrivateProfileIntW for the numeric fields, which silently truncates
 * "0x..." to 0 - we deliberately do NOT reproduce that bug. */
static uint32_t parse_num(const char *s)
{
    if (!s)
        return 0;
    while (*s && isspace((unsigned char)*s))
        s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        return (uint32_t)strtoul(s + 2, NULL, 16);
    return (uint32_t)strtoul(s, NULL, 10);
}

static int ini_get(const char *path, const char *section, const char *key,
                   char *out, size_t outsz)
{
    FILE *f = fopen(path, "r");
    char line[512];
    int in_section = 0, found = 0;

    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (*s == ';' || *s == '#' || !*s)
            continue;
        if (*s == '[') {
            char *close = strchr(s, ']');
            if (!close)
                continue;
            *close = '\0';
            in_section = (strcasecmp(s + 1, section) == 0);
            continue;
        }
        if (!in_section)
            continue;
        char *eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = '\0';
        if (strcasecmp(trim(s), key) == 0) {
            snprintf(out, outsz, "%s", trim(eq + 1));
            found = 1;
            break;
        }
    }
    fclose(f);
    return found ? 0 : -1;
}

int pt_load_ini(const char *path, pt_table_t *t)
{
    char v[256];

    memset(t, 0, sizeof(*t));

    if (ini_get(path, "PartitionHead", "partition_entrys", v, sizeof(v)) < 0) {
        fprintf(stderr, "partition: %s: [PartitionHead] partition_entrys missing\n",
                path);
        return -1;
    }
    t->count = parse_num(v);
    if (t->count == 0 || t->count > PT_MAX_ENTRIES) {
        fprintf(stderr, "partition: bad entry count %u\n", t->count);
        return -1;
    }

    if (ini_get(path, "PartitionHead", "partition_magic", v, sizeof(v)) == 0)
        t->magic = parse_num(v);
    if (ini_get(path, "PartitionHead", "partition_version", v, sizeof(v)) == 0)
        t->version = parse_num(v);
    if (ini_get(path, "PartitionHead", "partition_platform", v, sizeof(v)) == 0)
        copy_field(t->platform, sizeof(t->platform), v);

    for (uint32_t i = 0; i < t->count; i++) {
        char sect[32];
        snprintf(sect, sizeof(sect), "Partition%u", i);

        if (ini_get(path, sect, "partition_name", v, sizeof(v)) < 0) {
            fprintf(stderr, "partition: [%s] partition_name missing\n", sect);
            return -1;
        }
        copy_field(t->entry[i].name, sizeof(t->entry[i].name), v);

        if (ini_get(path, sect, "partition_type", v, sizeof(v)) == 0)
            copy_field(t->entry[i].type, sizeof(t->entry[i].type), v);
        else
            copy_field(t->entry[i].type, sizeof(t->entry[i].type), "nand");

        if (ini_get(path, sect, "partition_addr", v, sizeof(v)) == 0)
            t->entry[i].addr = parse_num(v);
        if (ini_get(path, sect, "partition_size", v, sizeof(v)) == 0)
            t->entry[i].size = parse_num(v);
    }
    return 0;
}
