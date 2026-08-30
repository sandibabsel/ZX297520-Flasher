/* partition.h - partition.bin container used by the ZX297520 downloader
 *
 * Layout confirmed byte-for-byte against a real partition.bin and against
 * sub_417210 in Downloader.exe:
 *
 *   header, 32 bytes
 *     0x00  u32   magic          (e.g. 0x31594876, bytes "vHY1")
 *     0x04  char  platform[16]   (e.g. "WF7520", NUL padded)
 *     0x14  u32   version        (e.g. 0x00201304)
 *     0x18  u32   entries
 *     0x1C  u32   checksum       XOR of every dword of the entry array
 *
 *   entry, 40 bytes, repeated `entries` times
 *     0x00  char  name[16]
 *     0x10  char  type[16]       "nand" / "ddr" / "raw" / ...
 *     0x20  u32   addr
 *     0x24  u32   size
 */
#ifndef ZXDL_PARTITION_H
#define ZXDL_PARTITION_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>   /* ssize_t */

#define PT_HDR_SIZE   32
#define PT_ENTRY_SIZE 40
#define PT_NAME_LEN   16
#define PT_MAX_ENTRIES 64

typedef struct {
    char     name[PT_NAME_LEN + 1];
    char     type[PT_NAME_LEN + 1];
    uint32_t addr;
    uint32_t size;
} pt_entry_t;

typedef struct {
    uint32_t   magic;
    char       platform[PT_NAME_LEN + 1];
    uint32_t   version;
    uint32_t   count;
    uint32_t   checksum;      /* value stored in the file   */
    uint32_t   checksum_calc; /* value we recomputed        */
    pt_entry_t entry[PT_MAX_ENTRIES];
} pt_table_t;

/* Parse a partition.bin image held in memory. Returns 0 on success.
 * Does not fail on checksum mismatch - compare checksum vs checksum_calc. */
int pt_parse(const void *buf, size_t len, pt_table_t *t);

/* Same, but silent - for probing a file whose type is not yet known. */
int pt_probe(const void *buf, size_t len, pt_table_t *t);

/* Serialise into buf (must be >= pt_image_size(t)). Recomputes the checksum.
 * Returns the number of bytes written, or -1. */
ssize_t pt_serialize(const pt_table_t *t, void *buf, size_t bufsz);

size_t pt_image_size(const pt_table_t *t);

/* Build a table from an INI file in the format the Windows tool expects
 * ([PartitionHead] + [Partition0]..[PartitionN-1]). Returns 0 on success. */
int pt_load_ini(const char *path, pt_table_t *t);

const pt_entry_t *pt_find(const pt_table_t *t, const char *name);

void pt_print(const pt_table_t *t);

#endif /* ZXDL_PARTITION_H */
