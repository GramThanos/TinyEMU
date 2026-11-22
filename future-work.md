# Future work — TinyEMU

Open work, what's been tried, and what was measured. How to build, run, debug and the rules for changing the code are in `AGENTS.md`; history is in git. Last reviewed **2026-11-22**.

Related projects, each with its own repository: **jsLab** (the browser runtime: VMs, volumes, virtual network, demos), **tinyemu-agent** (the browser AI agent), **TinyEMU-distros** (guest images) and **bash.js**.

## Current state

- **x86 software CPU** (`x86_cpu.c`, 32-bit protected mode): one- and two-byte opcode maps, string ops, `cmpxchg8b`/`xadd`, `bt*`/`shld`/`shrd`/`bsf`/`bsr`, `sysenter`/`sysexit`, `cpuid`/`rdmsr`/`wrmsr`/`rdtsc`, a full x87 (80-bit), and SSE/SSE2. Paging: non-PAE 4 KB/4 MB and PAE, with A/D bits. Software TLB, deferred arithmetic flags, bulk `rep movs/stos`. 47 self-test functions in `tests/x86_cpu_selftest.c`.
- **WASM engine**: `Makefile.js` builds one engine with the PC and RISC-V machines into `dist/wasm/x86emu-wasm.{js,wasm}`, published on the project's GitHub Pages. jsLab and tinyemu-agent boot TinyEMU-distros images on it. The page-level x86→WASM JIT (`x86_jit.c`) is on by default.
- **RISC-V** (`riscv_cpu.c`): upstream TinyEMU, RV32/64/128 IMAFDQC, in the same engine. riscv64 Alpine/Debian boot through OpenSBI into Linux 6.12.

Alpine 3.20 x86 in the browser (2026-11-22, JIT on): cold boot 10-11 s, busybox shell loop 5.1/4.5/4.3 s, Python sum-of-squares 5.1/4.0/3.7 s (cold, then warm twice). JIT off: boot 16 s, shell 8.3 s, Python 7.4 s.

---

## P1 — Build and release · HIGH

`.github/workflows/test.yml` runs the native build, the self-tests, the inflate test, the WASM build and the JIT tests; `pages.yml` publishes the engine and binaries. Still missing:

- [ ] **A headless boot smoke test** in CI: boot a small distro under `temu-soft` and wait for a shell prompt.
- [ ] **A WASM speed check:** run the microbench under node and fail on a large drop, to catch WASM-only regressions such as the setjmp one (see Done).
- [ ] **Engine name.** `x86emu-wasm` also runs RISC-V. Renaming it breaks every page that loads it, so do it before other sites depend on the Pages URL, or not at all.

## P2 — Correctness gaps · MEDIUM

None affects the Linux guests booted so far (flat segments), but each differs from hardware. Add a selftest with each fix.

- [ ] **No segment-limit checks on data accesses.** Limits are loaded (`load_seg`, `desc_query`), but `ld_lin`/`st_lin` never check them, so an out-of-limit access doesn't raise `#GP`/`#SS`.
- [ ] **No TSS I/O-permission bitmap.** A port access at `CPL > IOPL` always faults instead of consulting the bitmap (`ioperm`).
- [ ] **Debug registers are stubs.** `mov r32, drN` reads 0 and `mov drN, r32` is ignored (`x86_cpu.c`, `b2 == 0x21`/`0x23`): no hardware breakpoints.
- [ ] **x87 RC under WASM.** Emscripten ignores `fesetround`, so rounding modes other than nearest don't take effect (the selftest's x87 RC case fails in every WASM build). Fixing it needs software rounding.

**Won't fix / out of scope** (so they aren't reported again):
- A/D bits may be set on the first page of a page-crossing store that faults on the second: harmless, A/D are sticky hints.
- No real mode, V86 or BIOS interrupts, so BIOS-booted guests (`bios:` + `drive0:`, no `kernel:`) fail with a message. (Long mode is a P4 idea, not out of scope.)
- MMX isn't advertised; its encodings `#UD`.
- Under WASM `long double` is 128-bit, so x87 results are more precise than hardware there. Bit-exact results would need a software float80.

## P3 — Speed · LOW

Measure every change with the browser A/B in `AGENTS.md` (shell loop, Python, boot), not only the microbench, which has saturated.

- [ ] **x86 JIT, still to try:**
      1. *Fewer cross-instance calls.* Direct calls between pages compiled into the same batch module, guarded by the target's generation. A hop's cost is the indirect call between module instances plus the exit's bookkeeping (see Rejected).
      2. *A cheaper exit.* Drop the i64 `cycle_count` update at every exit: count in a local the caller adds.
      3. *More native forms*, only for instructions that are both hot and often stepped. Still stepped, most first: movsb, `rep movs/stos`, bt/bts (reg, imm), shld/shrd, bsf/bsr, pushf, cli/sti, mov to a segment register, rdtsc. Steps are now well under 1% of instructions, so expect little.
      4. *Registers in locals*, but only for loop blocks or once runs between hops are longer (see Rejected).
- [ ] **RISC-V JIT: not needed yet.** Measured 2026-11-22 (browser, same session): riscv64 interpreter vs x86 with the JIT: boot 4 vs 13 s, shell loop 5.0 vs 7.3 s, Python 2.9 vs 4.5 s. Revisit once the x86 JIT catches up or guests do heavy compute. It would be a RISC-V front end to the same emitter, with `riscv_cpu.c` kept as the interpreter.

## P4 — New capabilities · IDEAS

Taken from [JSLinux's news](https://bellard.org/jslinux/news.html) (read 2026-11-22). JSLinux added 64-bit x86 in 2026-01 and AVX2, AVX-512 and APX in 2026-03, so these are known to work in a browser emulator. Big jobs: weigh each against P2/P3 first.

- [ ] **64-bit x86 (long mode).** i686 is fading: Debian 13 dropped the i386 kernel and installer, and most distros now ship only x86-64. Work: CPUID LM, EFER and 4-level paging (the TLB then keys on 48-bit linear addresses), REX prefixes and 64-bit registers, RIP-relative addressing, `syscall`/`sysret`, 64-bit kernel loading in `copy_kernel`, then the JIT. Keep 32-bit guests booting, and grow the selftest and fuzzer alongside.
- [ ] **AVX/AVX2 (VEX), later AVX-512 and APX.** Only after 64-bit. Needed for distros built for x86-64-v3 (RHEL 10 and its rebuilds); first measure whether target userlands use them, as was done for SSE (see Rejected).
- [ ] **Nested virtualization.** JSLinux runs TinyEMU inside its own Alpine guest through an emulated KVM API. A demo rather than a need here: it would mean emulating VMX (or a paravirtual `/dev/kvm`) in `x86_cpu.c`.

Reference sources (license-clean): **v86** (BSD-2), **blink** (ISC). Never copy from `cpux86.js` (JSLinux: proprietary, reference-only, git-ignored).

The browser virtual network (multi-VM switch, router, DHCP) moved to jsLab with the rest of the browser runtime. SLIRP user networking doesn't work in the build sandbox (`apk update` → `temporary error`); that's the environment, not a CPU bug.

---

## Done (newest first)

Numbers are wall-clock in the browser unless they say otherwise.

- **2026-11-22 — JIT native forms:** 16-bit (0x66) mov/ALU/test/shifts, cld/std, and the SSE2 moves gcc's i686 code uses for 64-bit values (`movd xmm, r/m32`, `punpckldq xmm, xmm`, `movq xmm/m64, xmm`) behind an inline `sse_check` test. Steps −49% (shell loop) / −69% (Python), time change below the noise. Mutation testing found the SSE guard never passed (`1 & 0x200` is 0), hidden by the correct stepped fallback; the fuzzer now seeds XMM registers and compares all 128 bits.
- **2026-11-22 — JIT on by default** in the web runtime (agent page: VM tab checkbox, `?jit=0`).
- **2026-11-22 — JIT profile round** (`-DX86_JIT_STATS` counts). Found: 26% of a shell loop interpreted after lookups with no block, libc recompiled in every process (ASLR), and ~95% of exits going to a page that already had code. Fixed with `K_STEP`, blocks cut at the next block (each instruction once), caps 256 blocks / 32 compiles, position-independent code keyed by frame, `run_chain`, and rationed recompiles. Boot 13-17 → 10-11 s, shell 7.3/6.9/7.0 → 5.1/4.5/4.3 s, Python 6.0/4.5/4.7 → 5.1/4.0/3.7 s.
- **2026-11-22 — Page-level JIT:** one WASM function per code page, `br_table` between blocks; coverage picked from a histogram of where blocks stopped. JIT off → on: boot 16 → 12 s, shell 8.3 → 6.8 s, Python 7.4 → 5.5 s. Fixed on the way: a fault storm never returned to the run loop (the budget now spans faults); fuzzer-found `xchg` and `not`/`setcc` ordering bugs.
- **2026-11-21 — First block JIT** (opt-in). Microbench 44 → ~1,100 Minstr/s; batched async compiles (one module per block made boot 23 s). The fuzzer found unmapped `phys_read`s returning all-ones at every width.
- **2026-11-21 — setjmp split:** the loop moved out of the function that calls `setjmp`. WASM microbench 9.5 → 48 Minstr/s, boot 59 → 14 s, `.wasm` 826 → 333 KB.
- **2026-11-21 — x87 rounding-mode skip:** native x87 loop 35.7 → 66.9 Minstr/s.
- **2026-11-21** — in-browser agent demo (now tinyemu-agent), RISC-V in the same engine, SSE/SSE2, software TLB, deferred flags, bulk `rep`, host-side kernel inflate, snapshots.
- **2026-09-11 — x87 80-bit precision:** `long double` registers, RC via `fenv`, PC rounding (`fpu_pc_round`), exact m80 round-trip; `test_x87_extended`.
- **2026-09-11 — `force_inline` alu/alu_incdec/test_cc:** microbench ~9%, awk boot ~5% (native).
- **2026-09-11 — `fetch8_fast` inlining:** awk boot −19% (native, 57.7 → 46.4 s).

## Rejected after measuring

Recorded so they aren't tried again without a new reason.

- **JIT registers in WASM locals** (2026-11-22): correct and mutation-checked, microbench 1,373 Minstr/s, but shell 5.7/4.9/4.2 → 9.1/6.8/5.2 s, Python 5.2/4.0/3.8 → 6.9/7.0/5.3 s, boot 10 → 12 s. A run averages 12-16 instructions between hops, so entry loads and exit stores cost more than they save, and the write-backs made every function bigger.
- **JIT tail-call chaining** (`x86_jit_next` + `return_call_indirect`): slower in Chrome 156. Kept as opt-in (`?jittail=1`) to re-check as browsers change.
- **JIT lookup cache in front of `chain_lookup`:** no difference.
- **JIT sweep** compiling the code after each ret/jmp, for all pages or only pages recompiled twice: slower on every workload.
- **Per-call timing with `performance.now()`:** 100 µs ticks; sampled sums exceeded the total. Count instead.
- **Threaded / computed-goto dispatch** (2026-09-11): `-fno-jump-tables` cost only ~5.6%, which GCC's jump table already recovers; the rest is below the noise.
- **Fetch-window reposition** on same-page branches: microbench 123 → 117 Minstr/s.
- **Inlining `decode_modrm`, `rm_read`/`rm_write`:** no gain, much bigger code.
- **Implementing SSE for speed:** boot + apk + sha/gzip ran 0% SSE. SSE was added later for correctness, because Alpine 3.20 needs it.
