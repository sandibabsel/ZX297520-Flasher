/* zxdl - open reimplementation of the ZTE ZX297520 "Downloader" flash tool
 *
 * Reverse engineered from Downloader 7510 V2.0B01 (Downloader.exe /
 * DownloaderENG.dll, ZTE 2016) purely for interoperability on Linux.
 *
 * WARNING: writing firmware can brick a device. Read the README before
 * using any subcommand that touches flash.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "crc32.h"
#include "partition.h"
#include "proto.h"
#include "serial.h"
#include "uimage.h"

#define DEFAULT_VID   0x19D2   /* ZTE Corporation */
#define DEFAULT_PID   0x0256
#define DEFAULT_IFNUM 0        /* MI_00 */
#define DEFAULT_BAUD  115200

static struct {
    const char *port;
    int         baud;
    unsigned    vid, pid;
    int         ifnum;
    int         verbose;
    int         wait_ms;
    int         assume_yes;
    int         no_nul;
} opt = {
    .baud = DEFAULT_BAUD,
    .vid = DEFAULT_VID,
    .pid = DEFAULT_PID,
    .ifnum = DEFAULT_IFNUM,
    .wait_ms = 30000,
};

/* ---- helpers ----------------------------------------------------------- */

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;

    if (!f) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = malloc((size_t)n ? (size_t)n : 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (n && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "%s: short read\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static int spew(const char *path, const void *buf, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        return -1;
    }
    if (fwrite(buf, 1, len, f) != len) {
        fprintf(stderr, "%s: short write\n", path);
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

static void progress(const char *what, uint64_t done, uint64_t total, void *u)
{
    (void)u;
    int pct = total ? (int)(done * 100 / total) : 100;
    fprintf(stderr, "\r  %-12s %3d%%  %llu / %llu bytes", what, pct,
            (unsigned long long)done, (unsigned long long)total);
    if (done >= total)
        fputc('\n', stderr);
    fflush(stderr);
}

static int confirm(const char *what)
{
    char line[16];
    if (opt.assume_yes)
        return 1;
    fprintf(stderr, "About to %s. This can brick the device. Continue? [y/N] ",
            what);
    if (!fgets(line, sizeof(line), stdin))
        return 0;
    return line[0] == 'y' || line[0] == 'Y';
}

static int open_link(serial_t *ser, zx_session_t *z)
{
    char path[256];

    if (opt.port) {
        snprintf(path, sizeof(path), "%s", opt.port);
    } else {
        fprintf(stderr, "Detect device port (%04x:%04x, interface %d)\n",
                opt.vid, opt.pid, opt.ifnum);
        if (serial_wait_port(path, sizeof(path), (uint16_t)opt.vid,
                             (uint16_t)opt.pid, opt.ifnum, opt.wait_ms) != 0) {
            fprintf(stderr,
                    "\nInvalid equipment! Wait for equipment\n"
                    "  (no tty for %04x:%04x; pass --port /dev/ttyUSBx)\n",
                    opt.vid, opt.pid);
            return -1;
        }
        fprintf(stderr, "\rPort found: %s                    \n", path);
    }

    if (serial_open(ser, path, opt.baud) < 0) {
        fprintf(stderr, "Failed to open port, please try again\n");
        return -1;
    }
    fprintf(stderr, "Succeed to open port (%s @ %d 8N1)\n", path, opt.baud);

    zx_init(z, ser);
    z->verbose = opt.verbose;
    if (opt.no_nul)
        z->send_nul = false;
    return 0;
}

/* ---- subcommands ------------------------------------------------------- */

static int cmd_ports(void)
{
    char path[256];
    if (serial_find_port(path, sizeof(path), (uint16_t)opt.vid,
                         (uint16_t)opt.pid, opt.ifnum) == 0) {
        printf("%s\n", path);
        return 0;
    }
    fprintf(stderr, "no matching port for %04x:%04x\n", opt.vid, opt.pid);
    return 1;
}

static int cmd_info(int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "usage: zxdl info <file.bin> ...\n");
        return 2;
    }
    for (int i = 0; i < argc; i++) {
        size_t len;
        uint8_t *b = slurp(argv[i], &len);
        uimage_t u;
        tloader_t t;
        pt_table_t pt;

        if (!b)
            return 1;
        printf("=== %s (%zu bytes)\n", argv[i], len);

        if (uimage_parse(b, len, &u) == 0) {
            uimage_print(&u);
        } else if (pt_probe(b, len, &pt) == 0) {
            pt_print(&pt);
        } else if (tloader_parse(b, len, &t) == 0) {
            tloader_print(&t);
        } else {
            printf("  unrecognised container\n");
        }
        printf("  zx_crc32    0x%08X   (protocol variant, no final XOR)\n",
               zx_crc32(b, len));
        printf("\n");
        free(b);
    }
    return 0;
}

static int cmd_ptable(int argc, char **argv)
{
    size_t len;
    uint8_t *b;
    pt_table_t t;

    if (argc < 1) {
        fprintf(stderr, "usage: zxdl ptable <partition.bin>\n");
        return 2;
    }
    b = slurp(argv[0], &len);
    if (!b)
        return 1;
    if (pt_parse(b, len, &t) < 0) {
        free(b);
        return 1;
    }
    pt_print(&t);
    free(b);
    return t.checksum == t.checksum_calc ? 0 : 1;
}

static int cmd_mkptable(int argc, char **argv)
{
    pt_table_t t;
    uint8_t buf[PT_HDR_SIZE + PT_MAX_ENTRIES * PT_ENTRY_SIZE];
    ssize_t n;

    if (argc < 2) {
        fprintf(stderr, "usage: zxdl mkptable <config.ini> <out.bin>\n");
        return 2;
    }
    if (pt_load_ini(argv[0], &t) < 0)
        return 1;
    n = pt_serialize(&t, buf, sizeof(buf));
    if (n < 0) {
        fprintf(stderr, "Failed to create partition BIN file!\n");
        return 1;
    }
    if (spew(argv[1], buf, (size_t)n) < 0)
        return 1;
    printf("Generate partition file successfully! (%zd bytes)\n", n);
    pt_parse(buf, (size_t)n, &t);
    pt_print(&t);
    return 0;
}

static int cmd_getvar(int argc, char **argv)
{
    serial_t ser;
    zx_session_t z;
    int rc = 1;
    static const char *all[] = { "plat", "nv", "num", "boot", NULL };

    if (open_link(&ser, &z) < 0)
        return 1;
    if (zx_sync(&z, 40) < 0)
        goto out;

    if (argc >= 1) {
        rc = zx_getvar(&z, argv[0]) < 0 ? 1 : 0;
    } else {
        rc = 0;
        for (int i = 0; all[i]; i++)
            if (zx_getvar(&z, all[i]) < 0)
                rc = 1;
    }
out:
    serial_close(&ser);
    return rc;
}

static int cmd_write(int argc, char **argv)
{
    serial_t ser;
    zx_session_t z;
    size_t len;
    uint8_t *data;
    int rc = 1;

    if (argc < 2) {
        fprintf(stderr, "usage: zxdl write <partition> <file.bin>\n");
        return 2;
    }
    data = slurp(argv[1], &len);
    if (!data)
        return 1;

    if (!confirm("write flash")) {
        free(data);
        return 1;
    }
    if (open_link(&ser, &z) < 0) {
        free(data);
        return 1;
    }
    if (zx_sync(&z, 40) < 0)
        goto out;

    printf("Start to send the write command! (%s, %zu bytes, %u chunk(s))\n",
           argv[0], len, (unsigned)((len + CHUNK_SIZE - 1) / CHUNK_SIZE));
    rc = zx_write_partition(&z, argv[0], data, len, progress, NULL) < 0 ? 1 : 0;
    if (rc == 0)
        printf("Succeed to download\n");
out:
    serial_close(&ser);
    free(data);
    return rc;
}

static int cmd_read(int argc, char **argv)
{
    serial_t ser;
    zx_session_t z;
    uint8_t *buf = NULL;
    size_t got = 0;
    uint32_t off, len;
    int rc = 1;

    if (argc < 4) {
        fprintf(stderr,
                "usage: zxdl read <partition> <offset> <length> <out.bin>\n");
        return 2;
    }
    off = (uint32_t)strtoul(argv[1], NULL, 0);
    len = (uint32_t)strtoul(argv[2], NULL, 0);

    if (open_link(&ser, &z) < 0)
        return 1;
    if (zx_sync(&z, 40) < 0)
        goto out;

    printf("Start to send the read command!\n");
    if (zx_read_partition(&z, argv[0], off, len, &buf, &got, progress, NULL) < 0)
        goto out;
    if (spew(argv[3], buf, got) < 0)
        goto out;
    printf("Succeed to upload (%zu bytes -> %s)\n", got, argv[3]);
    rc = 0;
out:
    free(buf);
    serial_close(&ser);
    return rc;
}

static int cmd_erase(int argc, char **argv)
{
    serial_t ser;
    zx_session_t z;
    int rc = 1;
    const char *what = argc >= 1 ? argv[0] : "auto";

    if (!confirm("erase flash"))
        return 1;
    if (open_link(&ser, &z) < 0)
        return 1;
    if (zx_sync(&z, 40) < 0)
        goto out;

    if (strcmp(what, "all") == 0)
        rc = zx_erase_all(&z) < 0 ? 1 : 0;
    else if (strcmp(what, "auto") == 0)
        rc = zx_erase_auto(&z) < 0 ? 1 : 0;
    else
        rc = zx_erase(&z, what) < 0 ? 1 : 0;
out:
    serial_close(&ser);
    return rc;
}

static int cmd_reboot(void)
{
    serial_t ser;
    zx_session_t z;
    int rc;

    if (open_link(&ser, &z) < 0)
        return 1;
    if (zx_sync(&z, 40) < 0) {
        serial_close(&ser);
        return 1;
    }
    rc = zx_reboot(&z) < 0 ? 1 : 0;
    serial_close(&ser);
    return rc;
}

static int cmd_ptupload(int argc, char **argv)
{
    serial_t ser;
    zx_session_t z;
    pt_table_t t;
    size_t len;
    uint8_t *b;
    int rc = 1, r;

    if (argc < 1) {
        fprintf(stderr, "usage: zxdl ptupload <partition.bin>\n");
        return 2;
    }
    b = slurp(argv[0], &len);
    if (!b)
        return 1;
    if (pt_parse(b, len, &t) < 0)
        goto done;
    if (t.checksum != t.checksum_calc)
        fprintf(stderr, "warning: checksum mismatch in %s "
                        "(stored 0x%08X, computed 0x%08X)\n",
                argv[0], t.checksum, t.checksum_calc);

    if (!confirm("upload a new partition table"))
        goto done;
    if (open_link(&ser, &z) < 0)
        goto done;
    if (zx_sync(&z, 40) < 0)
        goto out;

    /* Only the header + entries are sent, not the zero padding. */
    r = zx_set_partitions(&z, b, pt_image_size(&t), t.count);
    if (r == 0) {
        printf("Partition table accepted\n");
        rc = 0;
    } else if (r == 1) {
        if (confirm("erase every partition except nvro")) {
            rc = zx_erase_all(&z) < 0 ? 1 : 0;
        }
    }
out:
    serial_close(&ser);
done:
    free(b);
    return rc;
}

/* Stage 1 -> 2 -> 3, mirroring sub_403D80. */
static int cmd_flash(int argc, char **argv)
{
    serial_t ser;
    zx_session_t z;
    int rc = 1;

    if (argc < 2 || (argc % 2) != 0) {
        fprintf(stderr,
                "usage: zxdl flash <partition> <file> [<partition> <file> ...]\n");
        return 2;
    }
    if (!confirm("flash the device"))
        return 1;
    if (open_link(&ser, &z) < 0)
        return 1;

    printf("Start to download\n");
    if (zx_sync(&z, 40) < 0)
        goto out;

    for (int i = 0; i < argc; i += 2) {
        size_t len;
        uint8_t *data = slurp(argv[i + 1], &len);
        if (!data)
            goto out;
        printf("(%d/%d) %s <- %s (%zu bytes)\n",
               i / 2 + 1, argc / 2, argv[i], argv[i + 1], len);
        if (zx_write_partition(&z, argv[i], data, len, progress, NULL) < 0) {
            free(data);
            goto out;
        }
        free(data);
        printf("Succeed to download NO.%d file\n", i / 2 + 1);
    }
    printf("Succeed to download\n");
    rc = 0;
out:
    serial_close(&ser);
    return rc;
}

static int cmd_crc(int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        size_t len;
        uint8_t *b = slurp(argv[i], &len);
        if (!b)
            return 1;
        printf("%08X  %08X  %s\n", zx_crc32(b, len), std_crc32(b, len), argv[i]);
        free(b);
    }
    if (argc == 0)
        fprintf(stderr, "usage: zxdl crc <file> ...   (protocol, standard)\n");
    return argc ? 0 : 2;
}

/* ---- entry point ------------------------------------------------------- */

static void usage(void)
{
    fputs(
"zxdl - ZTE ZX297520 firmware downloader (open reimplementation)\n"
"\n"
"usage: zxdl [options] <command> [args]\n"
"\n"
"Offline commands (no device needed):\n"
"  info <file>...                  identify uImage / partition.bin / tloader\n"
"  ptable <partition.bin>          decode and verify a partition table\n"
"  mkptable <config.ini> <out.bin> build partition.bin from an INI\n"
"  crc <file>...                   protocol CRC32 and standard CRC32\n"
"\n"
"Device commands:\n"
"  ports                           print the detected download port\n"
"  getvar [plat|nv|num|boot]       query the device\n"
"  write <partition> <file>        write one partition\n"
"  flash <part> <file> [...]       write several partitions in order\n"
"  read <part> <off> <len> <out>   dump a region to a file\n"
"  ptupload <partition.bin>        push a new partition table\n"
"  erase [all|auto|<partition>]    erase (default: auto)\n"
"  reboot                          restart the device\n"
"\n"
"Options:\n"
"  -p, --port PATH     serial device (default: auto-detect)\n"
"  -b, --baud N        baud rate (default 115200)\n"
"      --vid HEX       USB vendor id  (default 19d2)\n"
"      --pid HEX       USB product id (default 0256)\n"
"      --interface N   USB interface  (default 0, -1 = any)\n"
"  -w, --wait MS       how long to wait for the device (default 30000)\n"
"  -y, --yes           skip confirmation prompts\n"
"      --no-nul        do not append the NUL byte to commands\n"
"  -v, --verbose       dump every exchange\n"
"  -h, --help          this text\n"
"\n"
"The device must already be in download mode (TBootPro). This tool does not\n"
"put it there. Writing flash can brick hardware; you have been warned.\n",
        stderr);
}

int main(int argc, char **argv)
{
    static struct option lo[] = {
        { "port",      required_argument, 0, 'p' },
        { "baud",      required_argument, 0, 'b' },
        { "vid",       required_argument, 0, 1000 },
        { "pid",       required_argument, 0, 1001 },
        { "interface", required_argument, 0, 1002 },
        { "wait",      required_argument, 0, 'w' },
        { "yes",       no_argument,       0, 'y' },
        { "no-nul",    no_argument,       0, 1003 },
        { "verbose",   no_argument,       0, 'v' },
        { "help",      no_argument,       0, 'h' },
        { 0, 0, 0, 0 }
    };
    int c;

    while ((c = getopt_long(argc, argv, "p:b:w:yvh", lo, NULL)) != -1) {
        switch (c) {
        case 'p':  opt.port = optarg; break;
        case 'b':  opt.baud = atoi(optarg); break;
        case 1000: opt.vid = (unsigned)strtoul(optarg, NULL, 16); break;
        case 1001: opt.pid = (unsigned)strtoul(optarg, NULL, 16); break;
        case 1002: opt.ifnum = atoi(optarg); break;
        case 1003: opt.no_nul = 1; break;
        case 'w':  opt.wait_ms = atoi(optarg); break;
        case 'y':  opt.assume_yes = 1; break;
        case 'v':  opt.verbose = 1; break;
        case 'h':  usage(); return 0;
        default:   usage(); return 2;
        }
    }

    if (optind >= argc) {
        usage();
        return 2;
    }

    const char *cmd = argv[optind++];
    int rest = argc - optind;
    char **rargv = argv + optind;

    if (!strcmp(cmd, "info"))     return cmd_info(rest, rargv);
    if (!strcmp(cmd, "ptable"))   return cmd_ptable(rest, rargv);
    if (!strcmp(cmd, "mkptable")) return cmd_mkptable(rest, rargv);
    if (!strcmp(cmd, "crc"))      return cmd_crc(rest, rargv);
    if (!strcmp(cmd, "ports"))    return cmd_ports();
    if (!strcmp(cmd, "getvar"))   return cmd_getvar(rest, rargv);
    if (!strcmp(cmd, "write"))    return cmd_write(rest, rargv);
    if (!strcmp(cmd, "flash"))    return cmd_flash(rest, rargv);
    if (!strcmp(cmd, "read"))     return cmd_read(rest, rargv);
    if (!strcmp(cmd, "ptupload")) return cmd_ptupload(rest, rargv);
    if (!strcmp(cmd, "erase"))    return cmd_erase(rest, rargv);
    if (!strcmp(cmd, "reboot"))   return cmd_reboot();

    fprintf(stderr, "unknown command: %s\n\n", cmd);
    usage();
    return 2;
}
