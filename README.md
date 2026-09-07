# TinyEMU

This repository hosts a modified version of Fabrice Bellard's [TinyEMU](https://bellard.org/tinyemu/) and it focus on its port to JavaScript/WebAssembly.

**Purpose:** provide a fully **MIT-licensed** base for a jsLinux-style emulator - a system emulator that can run Linux entirely in the browser via JavaScript/WebAssembly.
**Disclaimer:** parts of the code developed under this repository was writen by AI.

## Changes from Bellard's 2019-12-21 [TinyEMU](https://bellard.org/tinyemu/) release

- **New software x86 CPU** (`x86_cpu.c`) - a from-scratch, 32-bit protected-mode interpreter aimed at a WebAssembly target. It is now the **default** x86 backend and boots a Linux kernel to an interactive userspace shell. Upstream x86 support was KVM-only; this is the first backend that can target the browser.
- **KVM kept as an opt-in native-debug backend** (`USE_KVM=1`) for fast bring-up and cross-checking on a Linux host with `/dev/kvm`.
- **`x86-selftest` regression suite** for the software CPU (`tests/x86_cpu_selftest.c`).
- **Docker-based build** producing all release binaries under `dist/`.

## Features

- **RISC-V** system emulator - RV128IMAFDQC base ISA (user ISA 2.2, privileged 1.10):
  - 32/64/128-bit integer registers
  - 32/64/128-bit floating point (bit-exact, via the SoftFP library)
  - compressed instructions; dynamic XLEN change
- **x86** system emulator, two compile-time backends:
  - **software CPU** (default, `x86_cpu.c`) - the WebAssembly-targeted path
  - **KVM** (opt-in, `USE_KVM=1`) - native acceleration on a Linux host with `/dev/kvm`
- VirtIO console, network, block device, input, and 9P filesystem
- Graphical display with SDL
- JSON configuration file
- Remote HTTP block device and filesystem
- Small code, easy to modify, few external dependencies

## x86 scope

The software CPU is **32-bit protected mode only**. It boots a **Linux kernel image**
(bzImage) via a built-in protected-mode entry point; no BIOS image is necessary. A
configuration is a Linux boot when it has a `kernel:` entry.

It does **not** implement 16-bit real mode or the BIOS interrupt interface, so
real-mode / BIOS-booted guests (FreeDOS, MS-DOS, Windows, ReactOS - a `bios:` and boot
`drive0:` but no `kernel:`) are unsupported and exit with an explanatory message. Use
the KVM backend for those.

```
Works:    ./temu https://bellard.org/jslinux/alpine-x86.cfg   (has kernel:)
Rejected: ./temu https://bellard.org/jslinux/freedos.cfg      (bios:, no kernel:)
```

The x86 device model and images come from the JS/Linux project (2011), one of the first
emulators to run Linux fully in JavaScript. The images are hosted at
<https://bellard.org/jslinux>.

## Build

Dependencies (only needed for the native build): `libcurl`, OpenSSL, and SDL. On Fedora:

```bash
sudo dnf install openssl-devel libcurl-devel SDL-devel
```

On a 32-bit host, disable the 128-bit RISC-V target in the `Makefile` (it needs a
compiler with the `__int128` extension).

### Docker (recommended)

The supported build path. The repo is mounted at `/workspace`; artifacts land on the
host under `dist/` (`dist/linux/temu-soft`, `dist/linux/temu-kvm`, `dist/windows/temu.exe`):

```bash
docker compose run --rm build
```

### Native make

```bash
make                 # default: software x86 CPU + RISC-V -> ./temu
make USE_KVM=1       # x86 via KVM (Linux host with /dev/kvm)
make x86-selftest    # x86 CPU regression suite
make install         # optionally install to /usr/local/bin
```

## Usage

Use the VM images from <https://bellard.org/jslinux> directly (no need to download them):

```bash
./temu https://bellard.org/jslinux/alpine-x86.cfg          # x86, software CPU
./temu https://bellard.org/jslinux/buildroot-riscv64.cfg   # RISC-V, terminal
./temu https://bellard.org/jslinux/buildroot-x86-xwin.cfg  # graphical (SDL)
```

Or run a downloaded image:

```bash
./temu root-riscv64.cfg
./temu rv128test/rv128test.cfg
```

To expose the host `/tmp` inside a guest over 9P:

```bash
./temu root_9p-riscv64.cfg
```

then, in the guest: `mount -t 9p /dev/root /mnt` - the host `/tmp` appears under `/mnt`.

### Invocation

```
usage: temu [options] config_file
options are:
-m ram_size       set the RAM size in MB
-rw               allow write access to the disk image (default=snapshot)
-ctrlc            the C-c key stops the emulator instead of being sent to the
                  emulated software
-append cmdline   append cmdline to the kernel command line
-no-accel         disable VM acceleration (KVM, x86 machine only)
```

Console keys: press `C-a x` to exit the emulator, `C-a h` for help.

## Networking

The easiest option is the **user-mode** network driver - no configuration needed.

A **tap** driver is also supported to redirect a VirtIO adapter's traffic. See
`netinit.sh` for creating the tap interface and NAT redirection (exact steps depend on
your distribution and firewall). The VM config must include:

```
eth0: { driver: "tap", ifname: "tap0" }
```

and the guest is configured with:

```
ifconfig eth0 192.168.3.2
route add -net 0.0.0.0 gw 192.168.3.1 eth0
```

### Network filesystem (9P)

TinyEMU supports the VirtIO 9P filesystem for local or remote filesystems; for remote
ones it fetches files over HTTP (compatible with the vfsync utility). In `mount`, use
`/dev/rootN` as the device name, where `N` is the filesystem index (omitted when `N=0`).
The `build_filelist` tool builds the file list from a root directory - a simple web
server is enough to serve it. A `.preload` file lists files to preload when a given file
is opened.

### Network block device

TinyEMU supports an HTTP block device. The disk image is split into small files with the
`splitimg` utility; pass the URL of the JSON `blk.txt` file as the disk image filename.

## Technical notes

- **128-bit RISC-V** - the spec does not define all encodings for 128-bit integer and
  floating point ops; the missing ones were interpolated from the 32- and 64-bit forms.
  With no 128-bit toolchain or OS yet, `rv128test.bin` may be the first 128-bit RISC-V
  code.
- **Floating point** - bit-exact and complete for 32/64/128-bit, using the SoftFP library.
- **HTIF console** - TinyEMU uses registers at a fixed address (`0x40008000`) rather than
  ELF-symbol discovery; a small `riscv-pk` boot-loader change supports this. The HTIF
  console only shows boot messages and powers off the VM; the OS should use the VirtIO
  console.

## License / Credits

Released under the **MIT license**. If a file has no explicit license, `MIT-LICENSE.txt`
applies. The SLIRP library retains its own license (two-clause BSD).

- Original TinyEMU and the JS/Linux device model and images: © Fabrice Bellard.
- x86 → WebAssembly software-CPU fork and related changes: © GramThanos.
