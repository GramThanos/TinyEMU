# TinyEMU

[![test](https://github.com/GramThanos/TinyEMU/actions/workflows/test.yml/badge.svg)](https://github.com/GramThanos/TinyEMU/actions/workflows/test.yml)

A fork of Fabrice Bellard's [TinyEMU](https://bellard.org/tinyemu/) system emulator for running Linux **in the browser**. It adds a from-scratch software x86 CPU with an x86 → WebAssembly JIT, and builds one WebAssembly engine that runs both a PC (x86) and a RISC-V machine. The same sources also build a native `temu` for Linux and Windows.

The goal is a fully **MIT-licensed** base for a JSLinux-style emulator.

- **x86:** 32-bit protected-mode CPU with x87 and SSE/SSE2; boots Alpine and Debian i686 to a shell.
- **RISC-V:** RV32/64/128 IMAFDQC; boots Linux 6.12 through OpenSBI.
- **Browser engine:** snapshots, pause/resume, browser-backed disks and 9P folders, several VMs per page.
- **Devices:** VirtIO console, block, network, 9P and input; framebuffer; HTTP-streamed disks and filesystems; user-mode networking.

## Quick start

Build with Docker and boot Alpine Linux x86 (the kernel and root filesystem are streamed from Bellard's JSLinux site):

```bash
git clone https://github.com/GramThanos/TinyEMU.git
cd TinyEMU
docker compose run --rm build
docker compose run --rm -e TEMU_CONFIG=/workspace/tests/alpine-x86.cfg run
```

You get a root shell; `C-a x` exits. On Linux or WSL with the dependencies installed, `make && ./temu tests/alpine-x86.cfg` does the same without Docker.

## Using the engine in a web page

Every release is published to GitHub Pages at <https://gramthanos.github.io/TinyEMU/>, which allows cross-origin loading:

| Path | Contents |
|---|---|
| `engine/<version>/x86emu-wasm.{js,wasm}` | the WebAssembly engine of that release |
| `engine/latest/` | the newest release |
| `bin/<version>/` | native binaries: `temu-linux-x86_64.tar.gz` (temu-soft, temu-kvm), `temu-windows.zip` |
| `<dir>/SHA256SUMS` | checksums of every file in a release directory |
| `index.json` | the published versions, newest first |

Pin a version rather than `latest/` in production: a release can change the engine's JS API. The engine is a low-level module; [jsLab](https://github.com/GramThanos/jsLab) is the JavaScript runtime that drives it (VMs, volumes, snapshots, virtual networks). The engine needs a browser with WebAssembly exception handling: Chrome 95+, Safari 15.4+, Firefox 131+.

## Building

### Docker (recommended)

```bash
docker compose run --rm build    # make dist
```

| Output | What it is |
|---|---|
| `dist/linux/temu-soft` | software x86 CPU + RISC-V, with HTTP, user-mode networking and SDL |
| `dist/linux/temu-kvm` | x86 through KVM (needs `/dev/kvm`) |
| `dist/windows/temu.exe` | Windows build with HTTP(S) and user-mode networking, headless |

### Native (Linux / WSL)

Dependencies: gcc, make, libcurl, OpenSSL and SDL 1.2 headers.

```bash
sudo apt install build-essential libcurl4-openssl-dev libssl-dev libsdl1.2-dev   # Debian/Ubuntu
sudo dnf install gcc make libcurl-devel openssl-devel SDL-devel                  # Fedora
```

```bash
make                 # ./temu: software x86 CPU + RISC-V
make USE_KVM=1       # x86 through KVM instead
make CONFIG_SDL=     # without SDL (likewise CONFIG_FS_NET=, CONFIG_SLIRP=)
make install         # install to /usr/local/bin
```

On a 32-bit host, build with `CONFIG_INT128=` (the 128-bit RISC-V target needs `__int128`).

### WebAssembly engine

```bash
docker run --rm -v "$PWD":/src -w /src emscripten/emsdk:3.1.74 make -f Makefile.js
```

Output: `dist/wasm/x86emu-wasm.js` and `dist/wasm/x86emu-wasm.wasm` (one engine for both machines; the name is historical). Run `make -f Makefile.js clean` before changing build flags.

## Usage

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

The config file can be a local path or a URL. The JSLinux images work directly:

```bash
./temu https://bellard.org/jslinux/alpine-x86.cfg          # x86, terminal
./temu https://bellard.org/jslinux/buildroot-riscv64.cfg   # RISC-V, terminal
./temu https://bellard.org/jslinux/buildroot-x86-xwin.cfg  # x86, graphical (SDL)
```

Current Alpine and Debian images for both machines are built by [TinyEMU-distros](https://github.com/GramThanos/TinyEMU-distros).

A config is a JSON-like file. This one is [tests/alpine-x86.cfg](tests/alpine-x86.cfg):

```
{
    version: 1,
    machine: "pc",                 /* or riscv32, riscv64, riscv128 */
    memory_size: 256,
    kernel: "https://bellard.org/jslinux/kernel-x86.bin",
    cmdline: "console=hvc0 loglevel=4 root=root rootfstype=9p rootflags=trans=virtio ro",
    fs0: { file: "https://vfsync.org/u/os/alpine-x86" },
    eth0: { driver: "user" },
}
```

Console: the guest's interactive console must be the VirtIO console (`console=hvc0`); `C-a x` exits the emulator, `C-a h` shows the other keys.

### Networking

- **User mode** (`eth0: { driver: "user" }`) — no setup needed.
- **Tap** (`eth0: { driver: "tap", ifname: "tap0" }`) — `netinit.sh` shows how to create the tap interface and NAT (the steps depend on your distribution and firewall). In the guest: `ifconfig eth0 192.168.3.2` and `route add -net 0.0.0.0 gw 192.168.3.1 eth0`.

### Filesystems and disks

- **9P filesystems** (`fs0:` …) can be a local directory or a remote tree fetched over HTTP (vfsync-compatible). `build_filelist` builds the file list for a directory, which any web server can then serve; a `.preload` file lists files to fetch together. In the guest, mount `/dev/rootN` (`N` is the index, omitted for 0), e.g. `mount -t 9p /dev/root /mnt`.
- **Block devices** (`drive0:` …) can be a local image or an HTTP image split into chunks by `splitimg`: pass the URL (or local path) of its `blk.txt`. Chunks may be gzip-compressed (`"compression": "gzip"` in `blk.txt`).
- Disks open in snapshot mode: writes are kept in memory unless you pass `-rw`.

## Supported guests

- **x86** boots a Linux kernel directly (`kernel:`, a bzImage or 32-bit `vmlinux`); no BIOS image is needed. The software CPU is 32-bit protected mode only, so real-mode and BIOS-booted guests (FreeDOS, Windows; a `bios:` and `drive0:` but no `kernel:`) exit with a message. The KVM build can run those.
- **RISC-V** boots through firmware (`bios:`, e.g. OpenSBI `fw_jump` or BBL) or `rv128test`. The browser engine includes riscv32 and riscv64; riscv128 is native only.

## Changes from Bellard's 2019-12-21 release

### x86 (PC machine)

- **Software x86 CPU** (`x86_cpu.c`) - a from-scratch 32-bit protected-mode interpreter, now the **default** x86 backend. Upstream x86 was KVM-only, so it could not run in a browser; this CPU boots Linux i686 to userspace there.
  - **SSE/SSE2 and an 80-bit x87**, so current i686 userlands (Alpine 3.20, Debian 12) run.
  - **Performance work**: a software TLB, deferred arithmetic flags, bulk `rep` string moves, a per-instruction fetch window.
- **x86 → WebAssembly JIT** (`x86_jit.c`, `wasm/x86jit.js`, browser builds only): hot guest code pages are compiled to WebAssembly at run time.
- **KVM kept as an opt-in backend** (`USE_KVM=1`) for native debugging and cross-checking on a Linux host with `/dev/kvm`.
- **Faster kernel loading**: a gzip bzImage is inflated by the emulator (`inflate.c`) and its ELF segments loaded directly, instead of running the kernel's own decompressor on the emulated CPU. Uncompressed bzImages and bare 32-bit `vmlinux` files load the same way.

### RISC-V machine

- **Boots current Linux (6.12) through OpenSBI** (`fw_jump` as `bios:`): the device tree now has `riscv,isa` in canonical order, a `reg` on the HTIF node and a `reserved-memory` node over the firmware; `mvendorid`/`marchid`/`mimpid` read 0 and `mcounteren.TM` is kept, so OpenSBI can emulate `rdtime`.

### Browser engine (WebAssembly)

- **One engine for both machines** (`Makefile.js`, `dist/wasm/x86emu-wasm.*`): the PC and RISC-V (riscv32/riscv64), chosen by the config's `machine:`. Upstream built a separate engine per machine.
- **A per-instance JS binding** (`wasm/lib.js`, `Module.api`), so a page can run several isolated VMs, inline or in workers.
- **Run control and snapshots**: `vm_pause`/`vm_resume`/`vm_stop`, and save/restore of the whole WASM memory with the guest clock carried over (`host_clock_ns()`); long pauses (a frozen tab, a sleeping laptop) are skipped so the guest doesn't report stalls.
- **Storage the page provides**: `js:<name>` drives (`block_js.c`) and 9P filesystems (`fs_js.c`), backed by IndexedDB/OPFS in jsLab; several 9P mounts per VM.
- **Statistics exports** for the page: instructions run, disk chunks and bytes fetched, JIT blocks compiled.
- **WebAssembly build flags**: native WASM `setjmp`/`longjmp`, `-O3 -flto`, bulk memory and non-trapping float-to-int.

### Disks

- **Gzip-compressed disk chunks** (`"compression": "gzip"` in `blk.txt`), over HTTP and from local files; raw chunks still work.
- **Local split disks** in the native `temu`: a `blk.txt` manifest on disk is read chunk by chunk (with a copy-on-write overlay in snapshot mode).

### Native builds, tests and releases

- **Windows build** (`dist/windows/temu.exe`) with a native console, HTTP(S) configs (a bundled CA store) and user-mode networking; headless (no SDL).
- **Docker-based build** producing all release binaries under `dist/`.
- **Tests**: the `x86-selftest` regression suite (`tests/x86_cpu_selftest.c`), a JIT fuzzer comparing compiled and interpreted runs (`tests/x86_jit_fuzz.c`), a gzip decoder test, and benchmarks (`bench/`); run in CI on every push.
- **Published builds**: each release's engine and binaries go to GitHub Pages - see [Using the engine in a web page](#using-the-engine-in-a-web-page).

## Development

- [AGENTS.md](AGENTS.md) — the guide for developers and AI agents: source map, how to build, test, debug and benchmark, and the rules each part of the code must keep.
- [future-work.md](future-work.md) — open work, ideas, what has been tried and measured.
- Tests: `make x86-selftest` (x86 interpreter), `sh tests/x86_jit_test.sh` (JIT, under emsdk), `make inflate-test && sh tests/inflate_test.sh ./inflate-test`. CI runs them on every push and pull request.

## Technical notes

- **128-bit RISC-V** - the spec does not define all encodings for 128-bit integer and floating point ops; the missing ones were interpolated from the 32- and 64-bit forms.
- **Floating point** - RISC-V floating point is bit-exact for 32/64/128-bit, using the SoftFP library.
- **HTIF console** - registers at a fixed address (`0x40008000`) rather than found through ELF symbols. It only shows early boot messages and powers off the VM; the OS should use the VirtIO console.

## Related projects

- [TinyEMU-distros](https://github.com/GramThanos/TinyEMU-distros) - recipes that build guest images (kernel, rootfs, firmware) for TinyEMU (MIT).
- [jsLab](https://github.com/GramThanos/jsLab) - virtual machines and networks in the browser on this engine (GPL-3.0-or-later).
- [tinyemu-agent](https://github.com/GramThanos/tinyemu-agent) - a browser AI agent whose computer is a VM on this engine (GPL-3.0-or-later).
- [bash.js](https://github.com/GramThanos/bash.js) - a bash interpreter in JavaScript (MIT).

## License and credits

Released under the **MIT license** ([LICENSE](LICENSE)); a file without its own license header falls under it. The SLIRP library keeps its own two-clause BSD license.

- Original TinyEMU, and the JSLinux device model and images: © Fabrice Bellard.
- The software x86 CPU, the WebAssembly JIT and the other changes in this fork: © GramThanos.

This fork's code was written with the assstance of AI.
