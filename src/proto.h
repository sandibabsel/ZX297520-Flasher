/* proto.h - TBootPro download protocol (ZTE ZX297520)
 *
 * A fastboot-like, plain-ASCII request/response protocol over a USB CDC
 * serial link. Reconstructed from the state machines at sub_407030 (sender)
 * and sub_408F90 (response parser) in Downloader.exe.
 *
 * Wire conventions established from the disassembly:
 *   - Commands are ASCII and INCLUDE their terminating NUL byte. The Windows
 *     tool computes strlen+1 and hands that to WriteFile (sub_406F30).
 *   - Replies are ASCII, not newline terminated. The tool sizes each read
 *     from the longest possible reply for that command.
 *   - Bulk payload is raw binary, no framing.
 *   - Files are sent in 2 MiB chunks (sub_408A90: shr eax,0x15).
 */
#ifndef ZXDL_PROTO_H
#define ZXDL_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "serial.h"

/* Protocol states, named after the CMD_* identifiers that leak through the
 * error strings in DownloaderENG.dll. Values are the originals. */
enum {
    CMD_SYN             = 0x5A,
    CMD_WRITE           = 0x93,
    CMD_SEND_DATA       = 0x94,
    CMD_READ            = 0x95,
    CMD_SEND_OKAY       = 0x96,
    CMD_SEND_OKAY_AGAIN = 0x98,
    CMD_ERASE           = 0x99,
    CMD_REBOOT          = 0x9A,
    CMD_DONE            = 0x9B,
    CMD_SET_PARTITION   = 0x9C,
    CMD_SEND_PAR_DATA   = 0x9D,
    CMD_AUTO_ERASE      = 0x9E,
    CMD_ERASE_ALL       = 0x9F
};

/* Per-command timeouts, in ms, taken from the state handlers. */
#define TMO_SYN         500
#define TMO_SET_PART    300
#define TMO_WRITE     10000
#define TMO_READ      10000
#define TMO_OKAY        200
#define TMO_ERASE    120000
#define TMO_AUTOERASE 80000
#define TMO_REBOOT      100

#define CHUNK_SIZE (2u * 1024 * 1024) /* 2 MiB */

typedef struct {
    serial_t *ser;
    int       verbose;
    bool      send_nul;   /* append the NUL byte to commands (default true) */
    char      reply[512]; /* last reply, NUL terminated                     */
    size_t    reply_len;
} zx_session_t;

void zx_init(zx_session_t *z, serial_t *ser);

/* Low level: send a NUL-terminated command, read the reply into z->reply. */
int zx_command(zx_session_t *z, const char *cmd, int timeout_ms);

/* Handshake. Sends the single byte 0x5A and waits for any answer.
 * `tries` attempts, TMO_SYN each. Returns 0 on success. */
int zx_sync(zx_session_t *z, int tries);

/* getvar plat | nv | num | boot. Result lands in z->reply. */
int zx_getvar(zx_session_t *z, const char *what);

/* Upload the partition table:
 *   "set partitions <count hex>"  -> "OKAY RECV_TABLES"
 *   <raw table bytes>             -> "OKAY"
 * Returns 0 ok, 1 if the device reports an acceptable change (caller should
 * decide whether to erase), -1 on failure. */
int zx_set_partitions(zx_session_t *z, const void *table, size_t len,
                      uint32_t entry_count);

/* Write one partition. Splits into CHUNK_SIZE pieces, each preceded by
 * "compat_write <name> <%08X len> <crc>". */
typedef void (*zx_progress_fn)(const char *what, uint64_t done, uint64_t total,
                               void *user);

int zx_write_partition(zx_session_t *z, const char *part,
                       const void *data, size_t len,
                       zx_progress_fn cb, void *user);

/* Read back a region: "compat_read <name> <off> <len>" -> "DATA n" -> OKAY ->
 * n raw bytes -> CRC check -> OKAY. Caller owns *out (malloc'd). */
int zx_read_partition(zx_session_t *z, const char *part,
                      uint32_t off, uint32_t len,
                      uint8_t **out, size_t *outlen,
                      zx_progress_fn cb, void *user);

int zx_erase_all(zx_session_t *z);
int zx_erase_auto(zx_session_t *z);
int zx_erase(zx_session_t *z, const char *part); /* "erase <name>" */
int zx_reboot(zx_session_t *z);

#endif /* ZXDL_PROTO_H */
