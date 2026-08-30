/* simdev.c - a fake TBootPro device on a pseudo terminal.
 *
 * Lets you exercise zxdl end to end without touching real hardware. It
 * implements the protocol exactly as reconstructed from Downloader.exe, so a
 * successful run proves the host side is self consistent - it does NOT prove
 * the reconstruction matches real silicon.
 *
 *   ./simdev &            # prints e.g. /dev/pts/7
 *   ./zxdl -p /dev/pts/7 -y getvar
 *
 * Build: make simdev
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "crc32.h"
#include "partition.h"

#define SIM_FLASH_SIZE (8u * 1024 * 1024) /* small stand-in for the NAND */

static uint8_t *flash;
static int verbose;

static void say(int fd, const char *s)
{
    size_t n = strlen(s) + 1; /* device replies are NUL terminated too */
    if (verbose)
        fprintf(stderr, "[dev] --> %s\n", s);
    if (write(fd, s, n) < 0)
        perror("simdev write");
}

/* Read one NUL-terminated command, or a lone 0x5A sync byte. */
static ssize_t read_cmd(int fd, char *buf, size_t max)
{
    size_t n = 0;

    for (;;) {
        uint8_t c;
        ssize_t r = read(fd, &c, 1);
        if (r == 0)
            return -1;
        if (r < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                usleep(1000);
                continue;
            }
            return -1;
        }
        if (n == 0 && c == 0x5A) {
            buf[0] = (char)0x5A;
            buf[1] = '\0';
            return 1;
        }
        if (c == '\0') {
            buf[n] = '\0';
            return (ssize_t)n;
        }
        if (n + 1 < max)
            buf[n++] = (char)c;
    }
}

static int read_exact(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;
    size_t got = 0;
    while (got < len) {
        ssize_t r = read(fd, p + got, len - got);
        if (r == 0)
            return -1;
        if (r < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                usleep(1000);
                continue;
            }
            return -1;
        }
        got += (size_t)r;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int master;
    char *slave;
    struct termios tio;
    char cmd[512];
    uint32_t pending_entries = 0;

    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-v"))
            verbose = 1;

    flash = calloc(1, SIM_FLASH_SIZE);
    if (!flash) {
        perror("calloc");
        return 1;
    }

    master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) < 0 || unlockpt(master) < 0) {
        perror("posix_openpt");
        return 1;
    }
    slave = ptsname(master);
    if (!slave) {
        perror("ptsname");
        return 1;
    }

    if (tcgetattr(master, &tio) == 0) {
        cfmakeraw(&tio);
        tcsetattr(master, TCSANOW, &tio);
    }

    printf("%s\n", slave);
    fflush(stdout);

    for (;;) {
        ssize_t n = read_cmd(master, cmd, sizeof(cmd));
        if (n < 0) {
            usleep(2000);
            continue;
        }

        if (n == 1 && (uint8_t)cmd[0] == 0x5A) {
            if (verbose)
                fprintf(stderr, "[dev] <-- SYN\n");
            say(master, "OKAY");
            continue;
        }
        if (verbose)
            fprintf(stderr, "[dev] <-- %s\n", cmd);

        if (!strncmp(cmd, "getvar ", 7)) {
            const char *what = cmd + 7;
            if (!strcmp(what, "plat"))      say(master, "OKAY WF7520");
            else if (!strcmp(what, "boot")) say(master, "OKAY TBootPro V1.0");
            else if (!strcmp(what, "num"))  say(master, "OKAY 0123456789");
            else if (!strcmp(what, "nv"))   say(master, "OKAY nvrofs 2097152");
            else                            say(master, "FAIL");
            continue;
        }

        if (!strncmp(cmd, "set partitions ", 15)) {
            pending_entries = (uint32_t)strtoul(cmd + 15, NULL, 16);
            say(master, "OKAY RECV_TABLES");

            size_t want = PT_HDR_SIZE + (size_t)pending_entries * PT_ENTRY_SIZE;
            uint8_t *tbl = malloc(want);
            if (!tbl || read_exact(master, tbl, want) < 0) {
                free(tbl);
                say(master, "FAIL INVALID_PARTITION_TABLE");
                continue;
            }
            pt_table_t t;
            if (pt_parse(tbl, want, &t) == 0 && t.checksum == t.checksum_calc)
                say(master, "OKAY");
            else
                say(master, "FAIL INVALID_PARTITION_TABLE");
            free(tbl);
            continue;
        }

        if (!strncmp(cmd, "compat_write ", 13)) {
            char part[64];
            unsigned len = 0, crc = 0;
            if (sscanf(cmd + 13, "%63s %x %x", part, &len, &crc) != 3) {
                say(master, "FAIL INVALID SIZE");
                continue;
            }
            if (len == 0 || len > SIM_FLASH_SIZE) {
                say(master, "FAIL INVALID SIZE");
                continue;
            }
            char reply[64];
            snprintf(reply, sizeof(reply), "DATACRC %08X", len);
            say(master, reply);

            uint8_t *buf = malloc(len);
            if (!buf || read_exact(master, buf, len) < 0) {
                free(buf);
                say(master, "FAIL");
                continue;
            }
            uint32_t got = zx_crc32(buf, len);
            if (got != crc) {
                fprintf(stderr,
                        "[dev] CRC mismatch: host said %08X, computed %08X\n",
                        crc, got);
                free(buf);
                say(master, "FAIL");
                continue;
            }
            memcpy(flash, buf, len < SIM_FLASH_SIZE ? len : SIM_FLASH_SIZE);
            free(buf);
            fprintf(stderr, "[dev] wrote %u bytes to %s (crc %08X OK)\n",
                    len, part, crc);
            say(master, "OKAY");
            continue;
        }

        if (!strncmp(cmd, "compat_read ", 12)) {
            char part[64];
            unsigned off = 0, len = 0;
            if (sscanf(cmd + 12, "%63s %x %x", part, &off, &len) != 3) {
                say(master, "FAIL INVALID OFFSET");
                continue;
            }
            if (len == 0 || (uint64_t)off + len > SIM_FLASH_SIZE) {
                say(master, "FAIL INVALID OFFSET");
                continue;
            }
            char reply[64];
            snprintf(reply, sizeof(reply), "DATA %08X", len);
            say(master, reply);

            /* host acknowledges with OKAY before we stream */
            if (read_cmd(master, cmd, sizeof(cmd)) < 0)
                continue;
            if (write(master, flash + off, len) < 0)
                perror("simdev write payload");
            fprintf(stderr, "[dev] sent %u bytes from %s+0x%X\n", len, part, off);
            /* host sends a final OKAY */
            read_cmd(master, cmd, sizeof(cmd));
            say(master, "OKAY");
            continue;
        }

        if (!strcmp(cmd, "erase all") || !strcmp(cmd, "erase auto") ||
            !strncmp(cmd, "erase ", 6)) {
            memset(flash, 0xFF, SIM_FLASH_SIZE);
            fprintf(stderr, "[dev] %s\n", cmd);
            say(master, "OKAY");
            continue;
        }

        if (!strcmp(cmd, "reboot")) {
            say(master, "OKAY REBOOT");
            fprintf(stderr, "[dev] rebooting, bye\n");
            break;
        }

        say(master, "FAIL");
    }

    free(flash);
    close(master);
    return 0;
}
