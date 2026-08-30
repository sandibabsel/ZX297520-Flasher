# zxdl

An open reimplementation, for Linux, of the ZTE **Downloader 7510 V2.0B01**
firmware flashing tool. Written from a clean-room reverse engineering of
`Downloader.exe` and `DownloaderENG.dll` (ZTE, 2016) for the **ZX297520**
LTE baseband SoC.

The original is a Windows MFC application that talks to the device's
`TBootPro` bootloader over a USB CDC serial link. `zxdl` speaks the same
protocol from a terminal.

> **This tool writes to flash. It can brick your device.**
> Read [Status and caveats](#status-and-caveats) before using any command
> that touches hardware. Dump before you write.

## Build

No dependencies beyond libc and a C11 compiler.

```sh
make            # builds ./zxdl
make sim        # also builds ./simdev, the device simulator
make test       # end-to-end tests, no hardware required
make strict     # rebuild with -Werror -Wconversion -Wpedantic
sudo make install
```

## Quick start

```sh
# Identify what you have
./zxdl info tboot.bin tloader.bin partition.bin

# Decode and verify a partition table
./zxdl ptable partition.bin

# Find the device (must already be in download mode)
./zxdl ports

# Query it
./zxdl getvar

# Dump a partition before touching anything
./zxdl read nvrofs 0 0x200000 nvrofs-backup.bin

# Write
./zxdl write imagefs imagefs.bin
./zxdl flash partition partition.bin tboot tboot.bin rootfs rootfs.bin
```

## Commands

### Offline (no device needed)

| Command | What it does |
|---|---|
| `info <file>...` | Identify a uImage, `partition.bin`, or `tloader.bin` |
| `ptable <partition.bin>` | Decode a partition table and verify its checksum |
| `mkptable <config.ini> <out.bin>` | Build `partition.bin` from an INI |
| `crc <file>...` | Print the protocol CRC32 and the standard CRC32 |

### Device

| Command | What it does |
|---|---|
| `ports` | Print the detected download port |
| `getvar [plat\|nv\|num\|boot]` | Query the device (all four if omitted) |
| `write <partition> <file>` | Write one partition |
| `flash <part> <file> [...]` | Write several partitions in order |
| `read <part> <off> <len> <out>` | Dump a region to a file |
| `ptupload <partition.bin>` | Push a new partition table |
| `erase [all\|auto\|<partition>]` | Erase (default `auto`) |
| `reboot` | Restart the device |

### Options

```
-p, --port PATH     serial device (default: auto-detect)
-b, --baud N        baud rate (default 115200)
    --vid HEX       USB vendor id  (default 19d2, ZTE)
    --pid HEX       USB product id (default 0256)
    --interface N   USB interface  (default 0, -1 = any)
-w, --wait MS       how long to wait for the device (default 30000)
-y, --yes           skip confirmation prompts
    --no-nul        do not append the NUL byte to commands
-v, --verbose       dump every exchange
```

## Device access

The Windows tool walks the registry to map a USB PID to a COM port. On Linux
the same data is in sysfs, so `zxdl` scans `/sys/class/tty` for a `ttyUSB*` or
`ttyACM*` whose USB parent matches the vendor and product id.

To use it without root, install the udev rule:

```sh
sudo cp udev/99-zxdl.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

If `ModemManager` grabs the port before you do, either stop it or add the
device to its ignore list — the rule shipped here already tags it.

## The protocol

A fastboot-like, plain-ASCII request/response protocol over serial.

| Host sends | Device replies |
|---|---|
| `0x5A` (single byte) | any response = alive |
| `getvar plat` / `nv` / `num` / `boot` | `OKAY <value>` |
| `set partitions <count hex>` | `OKAY RECV_TABLES`, then expects the raw table |
| `compat_write <part> <%08X len> <crc>` | `DATA <n>` or `DATACRC <n>`, then expects the payload, then `OKAY` |
| `compat_read <part> <off> <len>` | `DATA <n>`, host sends `OKAY`, device streams `n` bytes |
| `erase all` / `erase auto` / `erase <part>` | `OKAY` or `FAIL ERASE` |
| `reboot` | `OKAY REBOOT` |

Failure replies seen in the original: `FAIL`, `FAIL ERASE`,
`FAIL INVALID PARTITION`, `FAIL INVALID OFFSET`, `FAIL INVALID SIZE`,
`FAIL INVALID_PARTITION_TABLE`, `FAIL ACCEPTABLE_PARTITION_CHANGE`,
`FAIL UNACCEPTABLE_PARTITION_CHANGE`.

Three details that are easy to get wrong, all confirmed from the disassembly:

1. **Commands include their terminating NUL byte.** The original computes
   `strlen + 1` and hands that to `WriteFile`. Use `--no-nul` if your device
   turns out to disagree.
2. **The CRC32 has no final XOR.** Init is `0xFFFFFFFF`, polynomial is the
   usual reflected `0xEDB88320`, but the result is not inverted. A standard
   CRC32 will be rejected. Compare the two columns of `zxdl crc`.
3. **Files are sent in 2 MiB chunks**, each with its own `compat_write`
   header and its own CRC.

## partition.bin

Header, 32 bytes:

| Offset | Size | Field |
|---|---|---|
| `0x00` | 4 | magic (`0x31594876` on the reference device) |
| `0x04` | 16 | platform string, e.g. `WF7520` |
| `0x14` | 4 | version |
| `0x18` | 4 | entry count |
| `0x1C` | 4 | checksum |

Entry, 40 bytes, repeated:

| Offset | Size | Field |
|---|---|---|
| `0x00` | 16 | name |
| `0x10` | 16 | type (`nand`, `ddr`, `raw`) |
| `0x20` | 4 | address |
| `0x24` | 4 | size |

**The `partition_crc` field is not a CRC.** Despite the name, the original
computes a plain 32-bit XOR of every dword in the entry array. This was
verified numerically against a real `partition.bin`. It is a weak check: it
will not catch reordered dwords or paired errors.

`examples/partition_wf7520.ini` is the reference device's table. Feeding it to
`mkptable` reproduces the original file byte for byte.

### A note on hex in INI files

The original reads `partition_addr`, `partition_size`, `partition_magic`,
`partition_version`, `partition_entrys` and `TLoaderAddr` with
`GetPrivateProfileIntW`, which parses decimal only. A value written as
`0x00082000` silently becomes `0`. `zxdl` accepts both `0x` hex and decimal
and does **not** reproduce that bug — so an INI that worked around it by using
decimal will still work, and one written in hex will now work properly.

## Testing without hardware

`simdev` is a fake device on a pseudo terminal that implements the protocol,
verifies CRCs, and stores writes in a RAM buffer.

```sh
make sim
./simdev -v &          # prints e.g. /dev/pts/7
./zxdl -p /dev/pts/7 -y write testpart somefile.bin
```

`make test` drives it automatically: it round-trips a partition table through
`mkptable`, checks the CRC variants against a known vector, then writes and
reads back a blob over the simulated link.

A green run proves the host side is internally consistent. It does **not**
prove the reconstruction matches real silicon.

## Status and caveats

Verified against real artifacts:

- `partition.bin` parsing and generation — byte-identical round trip
- The XOR checksum — matches the stored value exactly
- uImage header and data CRC of `tboot.bin` — both validate
- `tloader.bin` container layout — sizes add up exactly, and the derived load
  address matches `TLoaderAddr` from the original config

Reconstructed from disassembly but **not tested against hardware**:

- Everything that talks to a device. No ZX297520 was available.
- The exact argument formatting of `compat_read`. The original uses
  `"%s %s %s %s"`; `zxdl` sends `%08X` for offset and length, which is the
  most likely reading but is a guess.
- Whether the trailing NUL is really expected on the wire. The disassembly is
  clear that the Windows tool sends it; whether the device requires it is
  unknown. Hence `--no-nul`.
- The handshake. The original sends a single `0x5A` and accepts any reply;
  what a real device answers is not known.
- The three-stage `tloader` → `tboot` → payload boot sequence is **not**
  implemented. `zxdl` assumes the device is already in download mode. Staging
  involves timing and state that could not be pinned down from static
  analysis alone.

Not implemented: the multi-bin container format (208-byte header with an
embedded MD5), NV backup, and the merge/split tooling.

If you get this working against real hardware, the two things worth reporting
back are what the device answers to `0x5A` and whether `--no-nul` was needed.

## Layout

```
src/crc32.c      protocol CRC32 (no final XOR), XOR checksum, standard CRC32
src/serial.c     termios transport, sysfs port discovery
src/partition.c  partition.bin parse/serialise, INI loader
src/proto.c      the protocol state machine
src/uimage.c     uImage and tloader container inspection
src/main.c       CLI
tools/simdev.c   fake device on a PTY
tests/run.sh     end-to-end smoke test
```

## Legal

Clean-room reverse engineering for interoperability. Contains no code from
ZTE or Microsoft. ZTE, ZX297520 and TBootPro are trademarks of their
respective owners; this project is not affiliated with or endorsed by ZTE.

Whether reverse engineering for interoperability is permitted depends on your
jurisdiction and on any agreement you accepted with the original software.
In the EU, Directive 2009/24/EC Article 6 covers decompilation for
interoperability; in the US, 17 U.S.C. §1201(f) has a similar carve-out. This
is not legal advice — if it matters to you, ask a lawyer.

Released under the MIT License. See `LICENSE`.
