/* serial.h - serial transport, Linux replacement for CUartComm */
#ifndef ZXDL_SERIAL_H
#define ZXDL_SERIAL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>   /* ssize_t */

typedef struct {
    int  fd;
    char path[256];
} serial_t;

/* Timeouts lifted from sub_403000 (SetCommTimeouts). */
#define SER_READ_INTERVAL_MS 500  /* max idle gap inside one response  */
#define SER_READ_TOTAL_MS   1000  /* default overall read deadline     */
#define SER_WRITE_TOTAL_MS  2000  /* write deadline                    */

int  serial_open(serial_t *s, const char *path, int baud);
void serial_close(serial_t *s);

/* Discard anything queued in both directions (PurgeComm equivalent). */
int  serial_purge(serial_t *s);

/* Write the whole buffer or fail. Returns bytes written, or -1. */
ssize_t serial_write(serial_t *s, const void *buf, size_t len);

/* Read up to max bytes. Returns as soon as the line has been idle for
 * SER_READ_INTERVAL_MS, or when total_ms elapses, or when max is reached.
 * Returns byte count (may be 0 on timeout), or -1 on error. */
ssize_t serial_read(serial_t *s, void *buf, size_t max, int total_ms);

/* Read exactly len bytes or time out. */
ssize_t serial_read_exact(serial_t *s, void *buf, size_t len, int total_ms);

/* Locate the download port.
 *
 * The Windows tool walks HKLM\SYSTEM\CurrentControlSet\Enum\USB\<PID> to find
 * the COM port. On Linux the same information lives in sysfs, so we scan
 * /sys/class/tty for a ttyUSB/ttyACM node whose USB parent matches vid:pid
 * and, when >= 0, the given interface number.
 *
 * Writes the device path into out. Returns 0 on success, -1 if not found. */
int serial_find_port(char *out, size_t outsz, uint16_t vid, uint16_t pid, int ifnum);

/* Block until serial_find_port succeeds or timeout_ms expires (-1 = forever). */
int serial_wait_port(char *out, size_t outsz, uint16_t vid, uint16_t pid,
                     int ifnum, int timeout_ms);

/* Milliseconds since an arbitrary epoch, monotonic. */
uint64_t now_ms(void);

#endif /* ZXDL_SERIAL_H */
