#define _GNU_SOURCE
#include "serial.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static speed_t baud_to_speed(int baud)
{
    switch (baud) {
    case 1200:   return B1200;
    case 2400:   return B2400;
    case 4800:   return B4800;
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 921600: return B921600;
    default:     return 0;
    }
}

int serial_open(serial_t *s, const char *path, int baud)
{
    struct termios tio;
    speed_t sp;

    memset(s, 0, sizeof(*s));
    s->fd = -1;

    sp = baud_to_speed(baud);
    if (!sp) {
        fprintf(stderr, "serial: unsupported baud rate %d\n", baud);
        return -1;
    }

    s->fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (s->fd < 0) {
        fprintf(stderr, "serial: open %s: %s\n", path, strerror(errno));
        return -1;
    }
    snprintf(s->path, sizeof(s->path), "%s", path);

    if (tcgetattr(s->fd, &tio) < 0) {
        fprintf(stderr, "serial: tcgetattr: %s\n", strerror(errno));
        goto fail;
    }

    cfmakeraw(&tio);
    cfsetispeed(&tio, sp);
    cfsetospeed(&tio, sp);

    /* 8N1, no flow control, ignore modem lines - matches the DCB the Windows
     * tool builds in sub_402F10 with the default UI settings. */
    tio.c_cflag &= ~(unsigned)(CSIZE | PARENB | CSTOPB | CRTSCTS);
    tio.c_cflag |= CS8 | CLOCAL | CREAD;
    tio.c_iflag &= ~(unsigned)(IXON | IXOFF | IXANY);
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 0; /* we do our own timing with poll() */

    if (tcsetattr(s->fd, TCSANOW, &tio) < 0) {
        fprintf(stderr, "serial: tcsetattr: %s\n", strerror(errno));
        goto fail;
    }

    tcflush(s->fd, TCIOFLUSH);
    return 0;

fail:
    close(s->fd);
    s->fd = -1;
    return -1;
}

void serial_close(serial_t *s)
{
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
}

int serial_purge(serial_t *s)
{
    return tcflush(s->fd, TCIOFLUSH);
}

ssize_t serial_write(serial_t *s, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t done = 0;
    uint64_t deadline = now_ms() + SER_WRITE_TOTAL_MS + len / 8;

    while (done < len) {
        struct pollfd pfd = { .fd = s->fd, .events = POLLOUT };
        int64_t left = (int64_t)deadline - (int64_t)now_ms();
        int r;

        if (left <= 0) {
            fprintf(stderr, "serial: write timeout (%zu/%zu)\n", done, len);
            return -1;
        }
        r = poll(&pfd, 1, (int)left);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            continue;

        ssize_t n = write(s->fd, p + done, len - done);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            fprintf(stderr, "serial: write: %s\n", strerror(errno));
            return -1;
        }
        done += (size_t)n;
    }
    tcdrain(s->fd);
    return (ssize_t)done;
}

ssize_t serial_read(serial_t *s, void *buf, size_t max, int total_ms)
{
    uint8_t *p = buf;
    size_t got = 0;
    uint64_t start = now_ms();
    uint64_t last = start;

    while (got < max) {
        struct pollfd pfd = { .fd = s->fd, .events = POLLIN };
        uint64_t t = now_ms();
        int64_t total_left = (int64_t)total_ms - (int64_t)(t - start);
        int64_t idle_left;
        int wait, r;

        if (total_left <= 0)
            break;

        /* Once we have something, stop after an idle gap instead of waiting
         * out the full deadline - the device sends short ASCII replies. */
        if (got) {
            idle_left = SER_READ_INTERVAL_MS - (int64_t)(t - last);
            if (idle_left <= 0)
                break;
            wait = (int)(idle_left < total_left ? idle_left : total_left);
        } else {
            wait = (int)total_left;
        }

        r = poll(&pfd, 1, wait);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0) {
            if (got)
                break;
            continue;
        }

        ssize_t n = read(s->fd, p + got, max - got);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            fprintf(stderr, "serial: read: %s\n", strerror(errno));
            return -1;
        }
        if (n == 0)
            break;
        got += (size_t)n;
        last = now_ms();
    }
    return (ssize_t)got;
}

ssize_t serial_read_exact(serial_t *s, void *buf, size_t len, int total_ms)
{
    uint8_t *p = buf;
    size_t got = 0;
    uint64_t start = now_ms();

    while (got < len) {
        struct pollfd pfd = { .fd = s->fd, .events = POLLIN };
        int64_t left = (int64_t)total_ms - (int64_t)(now_ms() - start);
        int r;

        if (left <= 0)
            break;
        r = poll(&pfd, 1, (int)left);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            break;

        ssize_t n = read(s->fd, p + got, len - got);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return -1;
        }
        if (n == 0)
            break;
        got += (size_t)n;
    }
    return (ssize_t)got;
}

/* ---- port discovery ---------------------------------------------------- */

static int read_hex_file(const char *dir, const char *leaf, unsigned *out)
{
    char path[PATH_MAX];
    FILE *f;
    unsigned v;

    if (snprintf(path, sizeof(path), "%s/%s", dir, leaf) >= (int)sizeof(path))
        return -1;
    f = fopen(path, "r");
    if (!f)
        return -1;
    if (fscanf(f, "%x", &v) != 1) {
        fclose(f);
        return -1;
    }
    fclose(f);
    *out = v;
    return 0;
}

/* Strip the last path component in place. Returns 0 if there was one. */
static int path_up(char *p)
{
    char *slash = strrchr(p, '/');
    if (!slash || slash == p)
        return -1;
    *slash = '\0';
    return 0;
}

int serial_find_port(char *out, size_t outsz, uint16_t vid, uint16_t pid, int ifnum)
{
    static const char *const prefixes[] = { "ttyUSB", "ttyACM", NULL };
    DIR *d = opendir("/sys/class/tty");
    struct dirent *de;
    int found = -1;

    if (!d)
        return -1;

    while ((de = readdir(d)) && found != 0) {
        char link[PATH_MAX], node[PATH_MAX], probe[PATH_MAX];
        unsigned v, p, ifn = (unsigned)-1;
        int match = 0;

        for (int i = 0; prefixes[i]; i++)
            if (strncmp(de->d_name, prefixes[i], strlen(prefixes[i])) == 0)
                match = 1;
        if (!match)
            continue;

        if (snprintf(link, sizeof(link), "/sys/class/tty/%s/device",
                     de->d_name) >= (int)sizeof(link))
            continue;
        if (!realpath(link, node))
            continue;

        /* `node` is the USB interface dir for ttyUSB, or one level below it
         * for ttyACM. Find the nearest ancestor holding bInterfaceNumber. */
        snprintf(probe, sizeof(probe), "%s", node);
        for (int up = 0; up < 3; up++) {
            if (read_hex_file(probe, "bInterfaceNumber", &ifn) == 0)
                break;
            if (path_up(probe) != 0)
                break;
        }

        /* Then keep climbing to the USB device that owns idVendor/idProduct. */
        snprintf(probe, sizeof(probe), "%s", node);
        for (int up = 0; up < 5; up++) {
            if (read_hex_file(probe, "idVendor", &v) == 0 &&
                read_hex_file(probe, "idProduct", &p) == 0) {
                if (v == vid && p == pid &&
                    (ifnum < 0 || ifn == (unsigned)ifnum)) {
                    snprintf(out, outsz, "/dev/%s", de->d_name);
                    found = 0;
                }
                break;
            }
            if (path_up(probe) != 0)
                break;
        }
    }
    closedir(d);
    return found;
}

int serial_wait_port(char *out, size_t outsz, uint16_t vid, uint16_t pid,
                     int ifnum, int timeout_ms)
{
    uint64_t start = now_ms();
    int ticks = 0;

    for (;;) {
        if (serial_find_port(out, outsz, vid, pid, ifnum) == 0)
            return 0;
        if (timeout_ms >= 0 && (int64_t)(now_ms() - start) >= timeout_ms)
            return -1;
        /* the Windows thread polls every 50 ms and nags every 500 ms */
        usleep(50 * 1000);
        if (++ticks % 10 == 0)
            fprintf(stderr, "\rwaiting for device %04x:%04x ...", vid, pid);
    }
}
