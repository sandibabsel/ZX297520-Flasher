#define _GNU_SOURCE
#include "proto.h"
#include "crc32.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void zx_init(zx_session_t *z, serial_t *ser)
{
    memset(z, 0, sizeof(*z));
    z->ser = ser;
    z->send_nul = true;
}

static void dump(const zx_session_t *z, const char *dir, const void *b, size_t n)
{
    const uint8_t *p = b;
    if (!z->verbose)
        return;
    fprintf(stderr, "  %s %zu: ", dir, n);
    for (size_t i = 0; i < n && i < 64; i++)
        fputc(p[i] >= 0x20 && p[i] < 0x7F ? p[i] : '.', stderr);
    if (n > 64)
        fputs("...", stderr);
    fputc('\n', stderr);
}

static int recv_reply(zx_session_t *z, int timeout_ms)
{
    ssize_t n = serial_read(z->ser, z->reply, sizeof(z->reply) - 1, timeout_ms);
    if (n < 0)
        return -1;
    z->reply_len = (size_t)n;
    z->reply[n] = '\0';
    /* strip trailing NUL/CR/LF/space the way CString::Trim would */
    while (z->reply_len &&
           (z->reply[z->reply_len - 1] == '\0' ||
            z->reply[z->reply_len - 1] == '\r' ||
            z->reply[z->reply_len - 1] == '\n' ||
            z->reply[z->reply_len - 1] == ' '))
        z->reply[--z->reply_len] = '\0';
    dump(z, "<--", z->reply, z->reply_len);
    if (n == 0) {
        fprintf(stderr, "Teminal response timeout!\n"); /* string 122, verbatim */
        return -1;
    }
    return 0;
}

static int send_raw(zx_session_t *z, const void *b, size_t n)
{
    dump(z, "-->", b, n);
    return serial_write(z->ser, b, n) == (ssize_t)n ? 0 : -1;
}

int zx_command(zx_session_t *z, const char *cmd, int timeout_ms)
{
    size_t n = strlen(cmd) + (z->send_nul ? 1 : 0);
    if (send_raw(z, cmd, n) < 0)
        return -1;
    return recv_reply(z, timeout_ms);
}

static bool has(const zx_session_t *z, const char *needle)
{
    return strstr(z->reply, needle) != NULL;
}

/* "DATA 00001000" / "DATACRC 12345678" -> numeric argument */
static bool reply_value(const zx_session_t *z, const char *tag, uint32_t *out)
{
    const char *p = strstr(z->reply, tag);
    if (!p)
        return false;
    p += strlen(tag);
    while (*p == ' ')
        p++;
    *out = (uint32_t)strtoul(p, NULL, 16);
    return true;
}

int zx_sync(zx_session_t *z, int tries)
{
    uint8_t b = CMD_SYN;

    printf("Start to synchronize\n");
    for (int i = 0; i < tries; i++) {
        serial_purge(z->ser);
        if (send_raw(z, &b, 1) < 0)
            return -1;
        if (serial_read(z->ser, z->reply, sizeof(z->reply) - 1, TMO_SYN) > 0) {
            printf("Receiving terminal synchronous response\n");
            return 0;
        }
    }
    fprintf(stderr, "No response(for SYN)\n"); /* string 162 */
    return -1;
}

int zx_getvar(zx_session_t *z, const char *what)
{
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "getvar %s", what);
    if (zx_command(z, cmd, TMO_WRITE) < 0)
        return -1;
    printf("%s -> %s\n", cmd, z->reply);
    return 0;
}

int zx_set_partitions(zx_session_t *z, const void *table, size_t len,
                      uint32_t entry_count)
{
    char cmd[64];

    snprintf(cmd, sizeof(cmd), "set partitions %X", entry_count);
    if (zx_command(z, cmd, TMO_SET_PART) < 0) {
        fprintf(stderr, "No response(for configuring partition)\n");
        return -1;
    }
    if (!has(z, "OKAY RECV_TABLES")) {
        fprintf(stderr, "Terminal response format error(CMD_SET_PARTITION): %s\n",
                z->reply);
        return -1;
    }

    if (send_raw(z, table, len) < 0)
        return -1;
    if (recv_reply(z, TMO_SET_PART) < 0) {
        fprintf(stderr, "No response(for comparing partition)\n");
        return -1;
    }

    if (has(z, "OKAY"))
        return 0;
    if (has(z, "FAIL INVALID_PARTITION_TABLE")) {
        fprintf(stderr, "Invalid partition table!\n");
        return -1;
    }
    if (has(z, "FAIL ACCEPTABLE_PARTITION_CHANGE")) {
        fprintf(stderr,
                "Partition table differs but the NV partition matches.\n"
                "Continuing requires erasing every partition except nvro.\n");
        return 1;
    }
    if (has(z, "FAIL UNACCEPTABLE_PARTITION_CHANGE")) {
        fprintf(stderr, "Partition table mismatch, NV partition inconsistent\n");
        return -1;
    }
    fprintf(stderr, "Terminal response format error: %s\n", z->reply);
    return -1;
}

int zx_write_partition(zx_session_t *z, const char *part,
                       const void *data, size_t len,
                       zx_progress_fn cb, void *user)
{
    const uint8_t *p = data;
    size_t off = 0;
    unsigned chunk_no = 0;

    if (len == 0) {
        fprintf(stderr, "%s: nothing to write\n", part);
        return -1;
    }

    while (off < len) {
        size_t n = len - off;
        uint32_t crc;
        char cmd[128];

        if (n > CHUNK_SIZE)
            n = CHUNK_SIZE;
        crc = zx_crc32(p + off, n);

        /* "%s %s %08X %s" -> compat_write <part> <len> <crc> */
        snprintf(cmd, sizeof(cmd), "compat_write %s %08X %x",
                 part, (unsigned)n, crc);

        if (zx_command(z, cmd, TMO_WRITE) < 0) {
            fprintf(stderr, "Failed to send written instructions to terminal\n");
            return -1;
        }

        if (has(z, "FAIL INVALID PARTITION")) {
            fprintf(stderr, "Invalid partition name: %s\n", part);
            return -1;
        }
        if (has(z, "FAIL INVALID OFFSET")) {
            fprintf(stderr, "Invalid offset address, or the file is beyond "
                            "the scope of partition.\n");
            return -1;
        }
        if (has(z, "FAIL INVALID SIZE")) {
            fprintf(stderr, "Invalid Size\n");
            return -1;
        }

        uint32_t want = 0;
        if (!reply_value(z, "DATACRC", &want) && !reply_value(z, "DATA", &want)) {
            fprintf(stderr, "Terminal response format error(CMD_WRITE): %s\n",
                    z->reply);
            return -1;
        }
        if (z->verbose)
            fprintf(stderr, "  device will accept %u bytes\n", want);

        if (send_raw(z, p + off, n) < 0) {
            fprintf(stderr, "Data transmission error\n");
            return -1;
        }
        if (recv_reply(z, TMO_WRITE) < 0)
            return -1;
        if (!has(z, "OKAY")) {
            fprintf(stderr, "Terminal response format error after data: %s\n",
                    z->reply);
            return -1;
        }

        chunk_no++;
        off += n;
        if (cb)
            cb(part, off, len, user);
    }
    return 0;
}

int zx_read_partition(zx_session_t *z, const char *part,
                      uint32_t off, uint32_t len,
                      uint8_t **out, size_t *outlen,
                      zx_progress_fn cb, void *user)
{
    char cmd[128];
    uint8_t *buf;
    uint32_t avail = 0;
    size_t got = 0;

    if (len == 0) {
        fprintf(stderr, "Upload size Invalid\n");
        return -1;
    }

    /* "%s %s %s %s" -> compat_read <part> <off> <len> */
    snprintf(cmd, sizeof(cmd), "compat_read %s %08X %08X", part, off, len);
    if (zx_command(z, cmd, TMO_READ) < 0)
        return -1;

    if (has(z, "FAIL INVALID PARTITION")) {
        fprintf(stderr, "Invalid partition name: %s\n", part);
        return -1;
    }
    if (has(z, "FAIL INVALID OFFSET")) {
        fprintf(stderr, "Invalid offset address\n");
        return -1;
    }
    if (!reply_value(z, "DATACRC", &avail) && !reply_value(z, "DATA", &avail)) {
        fprintf(stderr, "Terminal response format error: %s\n", z->reply);
        return -1;
    }
    if (avail == 0 || avail > len) {
        fprintf(stderr, "Upload size Invalid (device says %u)\n", avail);
        return -1;
    }
    printf("Receive the first packet length response command correctly (%u)\n",
           avail);

    buf = malloc(avail);
    if (!buf) {
        fprintf(stderr, "Failed to malloc memory!\n");
        return -1;
    }

    /* Acknowledge, then stream the payload in. */
    if (send_raw(z, "OKAY", z->send_nul ? 5 : 4) < 0) {
        free(buf);
        return -1;
    }

    while (got < avail) {
        ssize_t n = serial_read_exact(z->ser, buf + got, avail - got, TMO_READ);
        if (n <= 0) {
            fprintf(stderr, "Teminal response timeout! (%zu/%u received)\n",
                    got, avail);
            free(buf);
            return -1;
        }
        got += (size_t)n;
        if (cb)
            cb(part, got, avail, user);
    }
    printf("Upload data received correctly\n");

    /* Final OKAY closes the transaction. */
    if (send_raw(z, "OKAY", z->send_nul ? 5 : 4) < 0) {
        free(buf);
        return -1;
    }
    recv_reply(z, TMO_OKAY); /* best effort, some builds stay silent */

    *out = buf;
    *outlen = got;
    return 0;
}

static int erase_common(zx_session_t *z, const char *cmd, int tmo)
{
    if (zx_command(z, cmd, tmo) < 0)
        return -1;
    if (has(z, "OKAY")) {
        printf("Succeed to erase!\n");
        return 0;
    }
    if (has(z, "FAIL ERASE")) {
        fprintf(stderr, "Failed to erase\n");
        return -1;
    }
    if (has(z, "FAIL INVALID PARTITION")) {
        fprintf(stderr, "Invalid partition!\n");
        return -1;
    }
    fprintf(stderr, "The format of erasing response is incorrect: %s\n",
            z->reply);
    return -1;
}

int zx_erase_all(zx_session_t *z)
{
    printf("Start to send the erase all command\n");
    return erase_common(z, "erase all", TMO_ERASE);
}

int zx_erase_auto(zx_session_t *z)
{
    printf("Start to send the auto erase command\n");
    return erase_common(z, "erase auto", TMO_AUTOERASE);
}

int zx_erase(zx_session_t *z, const char *part)
{
    char cmd[128];
    printf("Start to send the erase command\n");
    snprintf(cmd, sizeof(cmd), "erase %s", part);
    return erase_common(z, cmd, TMO_ERASE);
}

int zx_reboot(zx_session_t *z)
{
    printf("Start to send the restart command\n");
    if (zx_command(z, "reboot", TMO_REBOOT) < 0) {
        /* Devices frequently drop the link before the reply lands. */
        fprintf(stderr, "no reply to reboot (device probably already reset)\n");
        return 0;
    }
    if (has(z, "OKAY REBOOT"))
        return 0;
    fprintf(stderr, "The format of restarting response is incorrect: %s\n",
            z->reply);
    return -1;
}
