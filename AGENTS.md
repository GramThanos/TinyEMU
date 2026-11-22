# AGENTS.md — TinyEMU

Guidance for AI agents (and humans) working in this repository. Open work and ideas are in `future-work.md`; history is in git.

## What this project is

A fork of Fabrice Bellard's [TinyEMU](https://bellard.org/tinyemu/) system emulator (2019-12-21 release), aimed at running Linux **in the browser**. It builds two ways from the same C sources:

- **native** `temu` (Linux, WSL, Windows cross build), for development and debugging;
- **WebAssembly** engine (`dist/wasm/x86emu-wasm.{js,wasm}`), loaded by the browser runtime in jsLab.

It emulates two machines, chosen by the config's `machine:` field:

| Machine | `machine:` | CPU | Guests |
|---|---|---|---|
| PC | `pc` | `x86_cpu.c`: from-scratch 32-bit protected-mode interpreter (x87, SSE/SSE2), plus an x86 → WASM JIT in browser builds; KVM as an opt-in native backend | Linux i686 (Alpine, Debian) booted directly from a kernel image |
| RISC-V | `riscv32`, `riscv64`, `riscv128` | `riscv_cpu.c`: Bellard's interpreter, RV32/64/128 IMAFDQC, SoftFP floating point | Linux 6.12 via OpenSBI `fw_jump`, Buildroot, `rv128test` |

Both machines share the device layer: VirtIO (console, block, network, 9P, input), simple framebuffer, JSON config, HTTP-backed block devices and 9P filesystems, user-mode networking (SLIRP).

**Scope:** booting Linux. The PC has no BIOS or real mode, so DOS/Windows guests are out of scope and fail gracefully. The browser build covers riscv32/riscv64 (not riscv128).

**License:** MIT. Parts of the code were written by AI.

## Related repositories

- **TinyEMU** (this repo) — the emulator, https://github.com/GramThanos/TinyEMU
- **TinyEMU-distros** — recipes that build guest kernels, rootfs and firmware for both machines, https://github.com/GramThanos/TinyEMU-distros
- **jsLab** — the browser runtime that drives the engine (VMs, volumes, snapshots, virtual network) and demos, https://github.com/GramThanos/jsLab
- **tinyemu-agent** — a browser AI agent whose computer is a VM on this engine, https://github.com/GramThanos/tinyemu-agent

jsLab and tinyemu-agent are GPL-3.0-or-later: never copy code from them here.

## Source map

| Area | Files |
|---|---|
| Front ends | `temu.c` (native CLI, console, SDL, split-disk reader), `jsemu.c` (browser entry points and run loop) |
| Machine framework | `machine.c`/`machine.h` (config parsing, `VirtMachineClass`, `host_clock_ns()`), `iomem.c`/`iomem.h` (physical memory map, MMIO) |
| Devices | `virtio.c`, `pci.c`, `simplefb.c`, `vga.c`, `ide.c`, `ps2.c`, `pckbd.c`, `vmmouse.c` |
| Storage / FS | `fs.c`, `fs_disk.c`, `fs_net.c`, `fs_wget.c`, `fs_utils.c`, `block_net.c`; browser-side `fs_js.c`, `block_js.c` |
| Networking | `slirp/` (user-mode network), `netinit.sh` (tap setup) |
| PC machine | `x86_machine.c` (PIC, PIT, CMOS, PCI, kernel loading), `x86_cpu.c`/`x86_cpu_priv.h`/`x86_cpu.h` (interpreter), `x86_jit.c` + `wasm/x86jit.js` (JIT) |
| RISC-V machine | `riscv_machine.c` (CLINT, PLIC, HTIF, virtio-mmio, device tree), `riscv_cpu.c` + `riscv_cpu_template.h` (built once per XLEN), `riscv_cpu_fp_template.h`, `softfp*` |
| Shared helpers | `inflate.c` (gzip: kernels and disk chunks), `json.c`, `cutils.c`, `aes.c`, `sha256.c` |
| Tools | `build_filelist.c` (9P file lists), `splitimg.c` (split a disk into `blk.txt` + chunks) |
| Browser glue | `Makefile.js`, `wasm/lib.js` (the `Module.api` binding Emscripten links in) |
| Tests | `tests/x86_cpu_selftest.c`, `tests/x86_jit_fuzz.c` + `tests/x86_jit_test.sh`, `tests/inflate_test.*`, `tests/alpine-x86.cfg` |
| Benchmarks | `bench/microbench.c` (`make microbench`), `bench/bootbench.sh` |
| CI / release | `.github/workflows/test.yml` (build + tests), `pages.yml` (publishes engine and binaries to GitHub Pages on a `v*` tag, onto `gh-pages` so older versions stay online); `pages/index.html` is the site's landing page |

## Quick start

From a fresh clone, with only Docker installed:

```bash
docker compose build build                                    # the build image (Fedora 40 + MinGW)
docker compose run --rm -T --entrypoint bash build -c 'make x86-selftest'   # builds and runs it: SELFTEST PASSED
docker compose run --rm build                                 # release binaries into dist/
docker compose run --rm -e TEMU_CONFIG=/workspace/tests/alpine-x86.cfg run  # boot Alpine x86 to a shell
docker run --rm -v "$PWD":/src -w /src emscripten/emsdk:3.1.74 make -f Makefile.js   # the WASM engine
```

The compose services mount the repository at `/workspace`, so paths given to them (`TEMU_CONFIG`) are `/workspace/...`. Build outputs land in your checkout (`dist/`, `*.o`), owned by the container's user. Run `make clean` between native and Docker builds of the same checkout, since they share the object files.

## Building and running

### Prerequisites

- **Docker** — the supported path; the image (`Dockerfile`) has every native and Windows-cross dependency.
- **Native Linux or WSL** — `gcc`, `make`, and the libraries for the default features: libcurl and OpenSSL headers (`CONFIG_FS_NET`), SDL 1.2 (`CONFIG_SDL`). On Debian/Ubuntu: `build-essential libcurl4-openssl-dev libssl-dev libsdl1.2-dev`. Turn off what you lack: `make CONFIG_SDL=` or `make CONFIG_FS_NET=`.
- **WASM** — the `emscripten/emsdk:3.1.74` image, or a local emsdk 3.1.74 with `emcc` and `node` on `PATH`.
- Native builds on Windows itself aren't supported: use Docker or WSL. The Windows `.exe` is cross-compiled.

### Native (Linux / WSL)

```bash
make                 # ./temu: software x86 + RISC-V
make USE_KVM=1       # x86 via KVM (needs /dev/kvm)
./temu <config.cfg>  # or an https:// config URL (needs CONFIG_FS_NET)
```

`Makefile` switches: `CONFIG_FS_NET`, `CONFIG_SDL`, `CONFIG_SLIRP`, `CONFIG_INT128` (riscv128), `CONFIG_X86EMU`, `CONFIG_KERNEL_INFLATE`. Extra defines go in `EXTRA_CFLAGS`. A native build is the fastest edit → boot loop.

### Release binaries (`docker compose run --rm build` → `make dist`)

- `dist/linux/temu-soft` — software x86 CPU + RISC-V, with FS_NET, SLIRP, SDL.
- `dist/linux/temu-kvm` — x86 via KVM (native debugging; needs `/dev/kvm`).
- `dist/windows/temu.exe` — Windows cross build with FS_NET (bundled `ca-bundle.crt`) and SLIRP, without SDL (headless).

### Browser engine (WASM)

```bash
docker run --rm -v "$PWD":/src -w /src emscripten/emsdk:3.1.74 make -f Makefile.js
```

Output: `dist/wasm/x86emu-wasm.{js,wasm}` — one engine with both machines (the name is historical). Needs browsers with WASM exception handling (Chrome 95+, Safari 15.4+, Firefox 131+). `Makefile.js` doesn't track flags: run `make -f Makefile.js clean` before changing `CONFIG_*` or `EXTRA_CFLAGS`. Exports are listed in `Makefile.js` (`EXPORTED_FUNCTIONS`); adding a C entry point for JS means adding it there.

This repo has no page of its own that boots a VM. To try the engine in a browser, use it from jsLab or tinyemu-agent (both load `x86emu-wasm.js`; see their READMEs for where a local build goes), or the published copy on this repo's GitHub Pages.

### Booting guests

- **x86:** `tests/alpine-x86.cfg` boots Alpine 3.12 i686 over the network (Bellard's jslinux kernel + 9P rootfs) on `hvc0`. A good boot reaches a root shell (`uname -a` → `4.12.0-rc6 ... i686`). The fetch can be slow or stall; retry.
- **RISC-V:** `./temu https://bellard.org/jslinux/buildroot-riscv64.cfg`, or the TinyEMU-distros riscv64 images (OpenSBI `bios:` + Linux 6.12 `kernel:`).
- **Current images** for both machines (Alpine 3.20, Debian) come from TinyEMU-distros' releases.
- **Offline, fast loop:** a config whose `kernel:` is a local `kernel-x86.bin` and whose `fs0:` is a local directory holding a static i686 busybox (`/bin/sh` → busybox, `dev/console`), with `init=/bin/sh` in `cmdline`, boots in about 12 s.

Console notes: only `hvc0` (the VirtIO console) receives stdin — x86 `ttyS0` and the RISC-V HTIF console are output-only, so use `console=hvc0`. Keep stdin open (not `/dev/null`), or temu exits on console EOF. `C-a x` exits, `C-a h` lists the keys.

## Testing

| Test | Command | Covers |
|---|---|---|
| x86 self-test | `make x86-selftest` (builds and runs it) → `SELFTEST PASSED` | x86 interpreter: faults/restarts, page crossing, TLB, `rep`, deferred flags, x87, SSE |
| JIT tests | `sh tests/x86_jit_test.sh [programs] [seed]` | selftest with every block compiled, interpreter-vs-JIT fuzzer, tail-call pass, microbench |
| gzip decoder | `make inflate-test && sh tests/inflate_test.sh ./inflate-test` | `inflate.c` |
| Boot | the configs above | end to end |

Run the native ones in Docker with `docker compose run --rm -T --entrypoint bash build -c '<command>'`, and the JIT tests with `docker run --rm -v "$PWD":/src -w /src emscripten/emsdk:3.1.74 sh tests/x86_jit_test.sh`. Under WASM the x87 rounding-mode case of the self-test always fails (Emscripten ignores `fesetround`); `x86_jit_test.sh` expects exactly that one failure.

CI (`test.yml`) runs all but the boot test on every push and pull request. There is no automated RISC-V test; check a riscv64 boot when touching `riscv_*`, the shared devices or `jsemu.c`.

### Releasing

Bump `VERSION` (a date, e.g. `2026-11-22`) and push a `v*` tag: `pages.yml` tests, builds and publishes `engine/<version>/` and `bin/<version>/` to the `gh-pages` branch. It can also be run by hand from the Actions tab.

## Conventions

- **Do not commit or push unless explicitly asked.**
- **Keep it MIT.** New dependencies must be MIT/BSD/ISC. Never copy code from jsLab or tinyemu-agent (GPL), or from `cpux86.js` (JSLinux: proprietary, reference only).
- Grow `tests/x86_cpu_selftest.c` with every x86 opcode or semantics fix, and the fuzzer with every instruction the JIT learns.
- Changes to shared code (devices, `machine.c`, `jsemu.c`, `iomem.c`) affect both machines and both builds: build native and WASM, and boot one guest of each machine.

## Engine-wide rules

- **Time:** emulated timers (PIT, RTC, TSC, RISC-V CLINT) read `host_clock_ns()` (`machine.c`), never `clock_gettime`. Snapshot restore shifts that clock; `jsemu.c` also drops gaps > 5 s between ticks (`skip_pause_gap`) so Linux doesn't report RCU stalls.
- **Snapshots** save the whole WASM memory, so C state needs no save code. Keep machine state in that memory, not in JS-side tables (the only one carried is `wasm/lib.js`'s file-buffer table).
- **Host writes to guest RAM must notify.** Device DMA goes through `PhysMemoryMap`'s `notify_ram_write` (`x86_cpu_note_ram_write()`), which drops JIT code on that page. Any new host → RAM path must do the same.
- **Physical reads are exactly `size` bytes, zero-extended;** unmapped memory reads all-ones at the access width.
- **Chunked disks** (`blk.txt` + `blk%09u.bin`, or `.bin.gz` with `"compression": "gzip"`): `block_net.c` (browser) and `temu.c` (native) must both keep reading raw and gzip chunks. A gzip chunk without the gzip magic is taken as already decoded. Gzip excludes prefetch groups and native read-write mode.
- **Statistics exports** (`jsemu.c`): `vm_cycles` (via the machine's `vm_get_cycles`; -1 if none), `vm_disk_chunks`, `vm_disk_bytes`, `vm_jit_blocks` — doubles, read by the page (tinyemu-agent's VM tab shows them).

## PC (x86) machine

### Boot

`copy_kernel` (`x86_machine.c`) loads the kernel directly, no BIOS:

- a gzip bzImage is inflated on the host (`inflate.c`) and its vmlinux ELF segments loaded at their physical addresses; an uncompressed ELF payload or a bare 32-bit `vmlinux` loads the same way; other payloads (LZ4, XZ, …) run the kernel's own decompressor. `CONFIG_KERNEL_INFLATE=` turns host inflate off.
- The kernel runs at its default physical address (no KASLR).

### Interpreter rules (`x86_cpu.c`)

- **No hot code in a function that calls `setjmp`.** `x86_cpu_interp` holds only the fault landing; the loop is the `no_inline` `x86_interp_loop`. Emscripten rewrites whole `setjmp` functions, which makes WASM ~5× slower (native builds don't show it — check WASM when touching this).
- **The run budget covers faults** (each costs ≥ 1 cycle), so a guest stuck faulting still returns to the run loop.
- **Restartable instructions:** compute faulting reads and effective addresses before changing registers or memory, so a mid-instruction `#PF` restarts cleanly. `push_val` commits ESP only after the write succeeds.
- **Page-crossing accesses are translated per page** (`ld_lin`/`st_lin`): adjacent linear pages map to unrelated physical pages.
- **Software TLB** (`tlb_read`/`tlb_write`, `x86_cpu_priv.h`), linear → host:
  - invalidate on **any** CR3 write (same-value reloads too), CR0.PG/WP and CR4.PAE changes, and `invlpg` (one page, both MMU indexes);
  - entries are tagged by MMU index (0 supervisor, 1 user); `mmu_index()` is computed live — don't cache it or merge the index sets (privilege escape);
  - only RAM is cached; write entries skip ROM and dirty-tracked framebuffer RAM.
- **Deferred arithmetic flags** (CF PF AF ZF SF OF): `alu()` records `cc_a`/`cc_b`/`cc_r`/`cc_op`. `s->eflags_` has a trailing underscore so direct uses stand out:
  - non-arithmetic bits (IF, DF, TF, IOPL) may be read from it directly;
  - arithmetic bits are read via `get_cf()`/`get_zf()`/… or `get_eflags()`;
  - every eager flag producer (shifts, rotates, mul, bt*, BCD, x87 compares, stc/clc/cmc, sahf, …) calls `flags_sync(s)` first; whole-word writes (popf/iret) use `set_eflags()`.
- **Bulk `rep movs`/`stos`** (`string_bulk`) only touches pages already in the TLB, falls back to scalar for overlap, DF=1 and 16-bit addressing, and clamps runs to whole elements in both pages. `string_op` caps work per entry and charges `cycle_count` per element.
- **x87/SSE rounding:** the host rounding mode is switched only for non-nearest RC; Emscripten ignores `fesetround`, so other modes don't work in WASM.
- **CPUID advertises FXSR, SSE, SSE2, not MMX.** Unknown `0F xx` raises `#UD`; never call `x86_stop()` for an unimplemented opcode.

### JIT (`x86_jit.c`, `wasm/x86jit.js`, browser builds only)

- **Switches:** `make -f Makefile.js CONFIG_X86_JIT=` builds without it (`-DX86_NO_JIT`; debug macros also disable it). At run time it runs only when `Module.x86jit === true` (jsLab sets it unless the VM config has `x86jit: false`). `x86jitThreshold`, `x86_jit_steps = 0` and `x86_jit_chain = 0` help bisecting.
- **Model:** a branch target seen `x86_jit_threshold` (40) times gets its code page compiled — all known blocks of the page in one WASM function with a `br_table`. Pages are compiled in batches by async `WebAssembly.instantiate()` and installed at `x86_jit_flush()` (end of each tick). Code is position-independent and keyed by host frame + MMU index. `run_chain` calls the next page's function directly until the interpreter has work (interrupt, HLT, `jit_poke`, loop limit). Instructions the compiler doesn't know are stepped on the interpreter (`x86_jit_step`) without ending the block.
- **The interpreter is the reference:** generated code mirrors it exactly — inline TLB checks with `x86_jit_ld`/`x86_jit_st` on a miss, faulting reads before commits, the same `cc_*` record as `alu()`. Before a helper that may fault, store `cur_eip` and `jit_pending`.
- **`L_FA`/`L_FB`/`L_FR`** hold the last flag producer's values: never scratch. A helper that sets flags eagerly sets `cc_known = 0`.
- **Code pages are write-tracked** (`x86_jit_code_bits`): `tlb_fill` never caches a write translation to them, so CPU writes reach `phys_write()`, which drops the code.
- **Compiled code isn't in a snapshot:** `vm_restore` calls `x86_jit_forget()`.
- **Testing:** on a fuzzer mismatch it reruns lock-step and names the first differing block (build `fuzz.js` as `tests/x86_jit_test.sh` does, then `node fuzz.js 1 <seed> <program> show` prints one program). Mutation-check every new native form: break it on purpose and see the suite fail. Fuzz programs must not write live page tables.

## RISC-V machine

- **Layout** (`riscv_machine.c`): RAM at `0x80000000`, CLINT `0x02000000`, HTIF `0x40008000`, virtio-mmio from `0x40010000`, PLIC `0x40100000`, framebuffer `0x41000000`. The machine builds the device tree passed to the firmware.
- **CPU** (`riscv_cpu.c`): compiled once per XLEN (`riscv_cpu32/64/128.o`) from the templates; `riscv_cpu_init()` picks the one the machine asks for. Floating point is bit-exact via SoftFP. The WASM build has riscv32/riscv64 only (`CONFIG_RISCV_MAX_XLEN=64`). It has its own TLB; there is no RISC-V JIT.
- **Linux 6.x via OpenSBI** (`fw_jump` as `bios:`) needs all of:
  - `riscv,isa` in canonical order (`rv64imafdc`), or Linux rejects the hart;
  - a `reg` on the HTIF node (fromhost, then tohost), or OpenSBI's console spins;
  - a `reserved-memory` node over the firmware (no PMP here, so OpenSBI doesn't add one, and Linux would reuse its pages);
  - `mvendorid`/`marchid`/`mimpid` reading 0 and `mcounteren.TM` kept, so OpenSBI emulates `rdtime` (there is no `time` CSR).
- **No RTC device:** guests boot in 1970.
- **Debugging a boot:** add `earlycon=sbi` to the cmdline to see the kernel from its first line.

## Debugging and profiling

### x86 debug macros (off by default; `make clean && make EXTRA_CFLAGS="-DX86_…"`)

All but `X86_JIT_STATS` are meant for native builds. In a WASM build the watches, traces, `X86_OPHIST` and `X86_NO_TLB` also leave the JIT out (they need every access on the interpreter's paths; see `x86_cpu_priv.h`).

- `X86_PROGRESS` — `[exc]/[irq]/[prog]/[user]` logging and a scan of the guest printk buffer. With `earlyprintk=ttyS0` it shows kernel panic reasons. Start here.
- `X86_UFAULT` — CPL3 instruction/register ring, dumped on the first user `#PF` with a small `cr2`. Best for userspace crashes.
- `X86_TRACE` — 2048-entry instruction ring, dumped on a low-address user `#PF`.
- `X86_SYSLOG` — every `int 0x80` and its return value.
- `X86_UWWATCH` — ring of CPL3 writes, dumped into the faulting stack frame.
- `X86_PWATCH1` + `X86_PWATCH1_ADDR=<phys>` — every write to one physical dword (catches kernel `copy_to_user`).
- `X86_WATCH` + `X86_WATCH_ADDR`, `X86_PF_DEBUG` — physical write watch, page-fault tracing.
- `X86_OPHIST` — opcode histogram every 100M instructions and at exit.
- `X86_NO_TLB` — walk page tables on every access (bisect stale translations).
- `X86_JIT_STATS` — WASM only (`make -f Makefile.js clean && make -f Makefile.js EXTRA_CFLAGS=-DX86_JIT_STATS`): a JIT profile on the browser console's stderr (`[jit-prof]`, `[jit-steps]`, `[jit-stops]`). Count, don't time: `performance.now()` is too coarse for per-call timing.

### Benchmarking

- **Native microbench** (deterministic, no network): `make microbench && ./microbench 200000000`.
- **Native boot benchmark:** `bench/bootbench.sh <temu-binary> <cfg> <tag>` boots a config and times three awk loops in the guest. `bench/run.sh` A/Bs two binaries inside the build container (copy them to `bench/temu-base` and `bench/temu-new` first).
- **Browser:** boot an Alpine 3.20 x86 image (from TinyEMU-distros) in jsLab or tinyemu-agent and time, in the guest, a 20000-iteration shell loop, Python `sum(i*i for i in range(200000))` and 20 × `/bin/ls`, three runs each (cold, then warm). Expect ±15% noise: compare two builds back to back in the same browser, not while something else is building, and keep the tab in the foreground (background tabs are throttled).
