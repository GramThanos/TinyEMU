# TinyEMU

This repository hosts a modified version of Fabrice Bellard's [TinyEMU](https://bellard.org/tinyemu/) and it focus on its port to JavaScript/WebAssembly.

**Purpose:** provide a fully **MIT-licensed** base for a jsLinux-style emulator - a system emulator that can run Linux entirely in the browser via JavaScript/WebAssembly.
**Disclaimer:** parts of the code developed under this repository was writen by AI.

## Changes from Bellard's 2019-12-21 [TinyEMU](https://bellard.org/tinyemu/) release

- **New software x86 CPU** (`x86_cpu.c`) - a from-scratch, 32-bit protected-mode interpreter aimed at a WebAssembly target. It is now the **default** x86 backend and boots a Linux kernel to an interactive userspace shell. Upstream x86 support was KVM-only; this is the first backend that can target the browser.
- **KVM kept as an opt-in native-debug backend** (`USE_KVM=1`) for fast bring-up and cross-checking on a Linux host with `/dev/kvm`.
- **`x86-selftest` regression suite** for the software CPU (`tests/x86_cpu_selftest.c`).
- **Docker-based build** producing all release binaries under `dist/`.
- **Interpreter performance work** on the software x86 CPU - a software TLB, deferred
  arithmetic flags, bulk `rep` string moves, a per-instruction fetch window, and
  WebAssembly build flags (plus `-O3 -flto` for the RISC-V JS build) - see
  [Performance](#performance).

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

## Performance

Interpreter and toolchain work on the software x86 CPU, aimed at the WebAssembly target.
Each entry names the code so it can be found; the measurements are at the end, attached
to what they actually measured.

### Toolchain and build flags

- **Wasm `setjmp`/`longjmp`** (`-sSUPPORT_LONGJMP=wasm`, `Makefile.js`) - `x86_cpu_interp()`
  calls `setjmp` and faults unwind via `longjmp`. Under Emscripten's default
  (`=emscripten`) every non-inlined call inside a `setjmp`-containing function is rewritten
  into an indirect call through a JavaScript `invoke_*` trampoline with try/catch, so the
  whole interpreter hot loop round-tripped through JavaScript on every helper call. The
  wasm variant uses native Wasm exception handling and stays inside the module; the built
  glue no longer contains any `invoke_*`, `saveSetjmp` or `testSetjmp`. Needs a browser
  with Wasm EH: Chrome 95+, Safari 15.4+, Firefox 131+.
- **`-O3 -flto`** - the hot path spans translation units (`x86_cpu.c` -> `iomem.h` /
  `cutils`), so cross-TU inlining of the memory accessors is worth more than the code-size
  cost. Applied to the RISC-V JS build as well, which was still on `-O2`.
- **Wasm feature flags** - `-mbulk-memory` (gives `memory.copy`/`memory.fill`, which is
  what the `rep movs`/`rep stos` bulk path spends its time in) and `-mnontrapping-fptoint`
  (single-instruction float->int for x87 and RISC-V `fcvt`). Both are older WebAssembly
  features than the Wasm EH already required above, so they do not narrow browser support.
  Deliberately not set in `Makefile_riscv.js`, whose objects also feed a `-s WASM=0`
  (asm.js) output.

### Interpreter

- **Software TLB** (`tlb_read`/`tlb_write`, `x86_cpu_priv.h`) - a direct-mapped
  linear->host translation cache. Before it, every operand access *and every
  instruction-fetch byte* walked the page tables, each walk costing two `phys_read()`s
  that were themselves a linear scan of the `PhysMemoryRange` table; a single
  `mov eax,[ebx+8]` cost four walks. Entries are tagged by MMU index (0 = supervisor,
  1 = user) rather than flushed on CPL change, so syscalls and interrupts keep their
  translations - and a supervisor translation can never be reused at CPL 3. Only
  RAM-backed pages are cached, so device MMIO keeps its unsplit single-access path, and
  write entries are additionally skipped for ROM and dirty-bit-tracked framebuffer RAM.
  Invalidated on any CR3 write (including a same-value reload, which is how Linux
  requests a flush), on CR0.PG/CR0.WP and CR4.PAE changes, and per-page on `invlpg`.
  Build with `-DX86_NO_TLB` to restore walk-every-access behaviour when bisecting a
  suspected stale-translation bug.
- **Single-compare TLB hit test** (`TLB_HIT`) - `la ^ tag <= 0x1000 - size` decides
  "right page" and "does not cross a page" together. Unused slots are parked on a page
  that hashes to a different slot, so an empty slot can never match.
- **Per-instruction fetch window** (`fetch_refill`) - instruction bytes are read through a
  host pointer valid to the end of the current guest page, instead of translating every
  fetched byte through the TLB. A 4-byte displacement cost four TLB lookups before; it now
  costs one refill per instruction. The window is re-derived each instruction, so every
  existing invalidation already covers it.
- **Size-specialised memory accessors** - `ld_lin`/`st_lin` branch once on the access
  width, so the TLB bound check and the load/store width are compile-time constants in
  each arm instead of runtime work on every access.
- **Bulk `rep movs`/`rep stos`** (`string_bulk`) - once the TLB holds the pages, a run is
  moved with `memcpy`/`memset` instead of translating every element. Runs are clamped to
  whole elements remaining in *both* the source and destination page, since the next
  linear page is an unrelated frame; overlapping host ranges fall back to the scalar step
  (`rep movsb` with `EDI = ESI+1` propagates a byte, which `memcpy` would not), as do DF=1
  and 16-bit addressing. The bulk path only touches pages already cached, so it cannot
  fault. `string_op` also bounds the work per entry and then restarts the instruction, so
  a large `rep` can no longer block interrupt delivery or overshoot the cycle slice, and
  `cycle_count` is charged per element so the emulated TSC advances during long copies.
- **Deferred ("lazy") arithmetic flags** (`cc_op`/`cc_a`/`cc_b`/`cc_r`, `flags_sync`) - the
  six arithmetic flags are the most frequently written and most rarely read piece of
  machine state: nearly every ALU instruction set all six, while only a following
  Jcc/setcc/cmov/adc consumes them, and usually wants one. `alu()` now records the masked
  operands, result and operation instead of computing them; `flags_sync()` materialises all
  six on demand and `cc_cf()`/`cc_zf()`/... derive a single flag without touching the
  others. Straight-line ALU code overwrites the pending record without ever materialising
  it. The `eflags_` field carries a trailing underscore so that every direct use is flagged
  for review: reading a non-arithmetic bit from it is correct, reading an arithmetic bit is
  a bug. Only `alu()` defers - every other flag producer (shifts, rotates, mul, bt, BCD,
  x87 compares) writes `eflags_` directly and must `flags_sync()` first.

### Measurements

Two rounds were measured against different baselines, so the figures are **not additive**:

- **The software TLB alone**, against the same build with `-DX86_NO_TLB`: 13.9 -> 56
  Minsn/s (**4.0x**) on a native microbenchmark (paged guest loop, 16 KB working set across
  8 pages, load/store/ALU/branch mix; three runs each). Alpine boot-to-shell went from
  8.4/9.1 s to 5.5/5.9 s - that boot is dominated by the HTTP fetch of the kernel and 9p
  rootfs, so it understates the interpreter gain.
- **The fetch window, size-specialised accessors, `TLB_HIT` and the wasm feature flags
  together**: **+7%** on a CPU-bound guest workload (busybox `awk` loop under Alpine),
  timed inside the guest with the old and new binaries booted back to back.
  `x86emu-wasm.wasm` grows from 380,668 to 519,103 bytes.

The bulk `rep` path and the deferred flags have no isolated speed measurement; both are
covered by correctness tests rather than a benchmark. There is no WebAssembly benchmark
harness yet, so every speed figure above comes from the native build. `bench/run.sh` is
the guest-side timing harness.

### Notes

`fetch8`/`fetch16`/`fetch32` are deliberately marked `no_inline`. Without it LLVM inlines
the fetch path into all ~50 fetch sites inside `x86_cpu_interp`: ~+17% instead of +7%, but
the WebAssembly module nearly doubles (751,989 bytes). GCC does not inline them - the same
source grows the native object by 5.6% - so that cost is specific to the Emscripten build
and is not an optimisation-level artefact.

The TLB and the bulk `rep` path both trade correctness-critical invariants for speed, so
they are pinned by `x86-selftest` cases rather than left to review: TLB invalidation via
`invlpg` and via a same-value CR3 reload, MMU-index tagging across CPL, and for `rep` the
overlap, page-edge split, `stos` spill, budget restart and DF=1 cases. The deferred flags
are checked against an eager reference across 5070 operation/size/operand/carry
combinations, plus an ordering case covering eager producers meeting a pending record.

## License / Credits

Released under the **MIT license**. If a file has no explicit license, `MIT-LICENSE.txt`
applies. The SLIRP library retains its own license (two-clause BSD).

- Original TinyEMU and the JS/Linux device model and images: © Fabrice Bellard.
- x86 → WebAssembly software-CPU fork and related changes: © GramThanos.
