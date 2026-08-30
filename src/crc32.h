/* crc32.h - checksums used by the ZX297520 downloader protocol
 *
 * Reverse engineered from Downloader.exe (ZTE, 2016):
 *   sub_403CC0 - CRC32, reflected poly 0xEDB88320, init 0xFFFFFFFF,
 *                NO final XOR. Table lives at VA 0x430A38.
 *   sub_417A70 - 32-bit XOR checksum used for partition.bin. Despite the
 *                INI key being called "partition_crc" this is not a CRC.
 */
#ifndef ZXDL_CRC32_H
#define ZXDL_CRC32_H

#include <stddef.h>
#include <stdint.h>

/* Protocol CRC. Note the missing final inversion - a standard CRC32 will
 * NOT match what the device expects. */
uint32_t zx_crc32(const void *data, size_t len);

/* Streaming variant. Seed the first call with ZX_CRC32_INIT. */
#define ZX_CRC32_INIT 0xFFFFFFFFu
uint32_t zx_crc32_update(uint32_t crc, const void *data, size_t len);

/* partition.bin checksum: XOR of every little-endian dword. len must be a
 * multiple of 4. */
uint32_t zx_xorsum32(const void *data, size_t len);

/* Standard CRC32 (with final XOR) - only needed to validate U-Boot uImage
 * headers, which use the normal algorithm. */
uint32_t std_crc32(const void *data, size_t len);

#endif /* ZXDL_CRC32_H */
