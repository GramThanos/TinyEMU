/*
 * x86 CPU emulator - internal state (software interpreter)
 *
 * Copyright (c) 2011-2017 Fabrice Bellard
 * Copyright (c) 2026 TinyEMU x86 software-CPU project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction ... (MIT, see LICENSE)
 *
 * This is the foundation for a from-scratch, WASM-targetable software x86 core.
 * It is intentionally an interpreter first; a shared WASM JIT is a later phase.
 * The instruction set is being filled in incrementally.
 */
#ifndef X86_CPU_PRIV_H
#define X86_CPU_PRIV_H

#include <setjmp.h>
#include "iomem.h"
#include "x86_cpu.h"

/* General-purpose register indices (x86 encoding order). */
enum {
    REG_EAX = 0, REG_ECX, REG_EDX, REG_EBX,
    REG_ESP,     REG_EBP, REG_ESI, REG_EDI,
    X86_NB_REGS
};

/* ---- deferred ("lazy") arithmetic flags ---------------------------------
 *
 * The six arithmetic flags (CF PF AF ZF SF OF) are the most frequently
 * *written* and the most rarely *read* piece of machine state: nearly every
 * ALU instruction sets all six, while only a following Jcc/setcc/cmov/adc
 * ever consumes them -- and such a consumer typically wants exactly one of
 * them. Computing all six eagerly costs six branches plus the parity fold on
 * every add/sub/cmp/test/and, most of it thrown away unread.
 *
 * So alu() does not compute them. It records the (masked) operands, the
 * result and the operation, and the flags are reconstructed on demand:
 * flags_sync() materializes all six, while cc_cf()/cc_zf()/... derive a
 * single flag without touching the others. Straight-line ALU code overwrites
 * the pending record without ever materializing it, and `cmp` + `jne` costs
 * one zero-test instead of a full flag word.
 *
 * Invariants:
 *  - cc_op == CC_OP_EFLAGS: `eflags_` is authoritative for every bit.
 *  - otherwise: `eflags_` is authoritative for the *non*-arithmetic bits
 *    (IF, DF, TF, IOPL, ...) and its six arithmetic bits are stale; those
 *    must be read via flags_sync()/get_eflags()/cc_*().
 *
 * `eflags_` carries a trailing underscore so that the compiler flags every
 * direct use for review: reading a non-arithmetic bit from it is correct and
 * fast, reading an arithmetic bit from it is a bug.
 *
 * Only alu() defers. Every other flag producer (shifts, rotates, mul, bt,
 * BCD, x87 compares) writes `eflags_` directly, and so must flags_sync()
 * first -- otherwise a still-pending record would later overwrite the bits it
 * just set.
 */
enum {
    CC_OP_EFLAGS = 0,   /* nothing pending; eflags_ is authoritative */
    CC_OP_ADD,
    CC_OP_ADC,          /* uses cc_cin */
    CC_OP_SUB,          /* also cmp */
    CC_OP_SBB,          /* uses cc_cin */
    CC_OP_LOGIC,        /* and/or/xor/test: CF=OF=AF=0 */
    CC_OP_INC,          /* add of 1 with CF preserved in cc_cin */
    CC_OP_DEC,          /* sub of 1 with CF preserved in cc_cin */
};

/* EFLAGS bit positions. */
#define EFL_CF   0x00000001
#define EFL_PF   0x00000004
#define EFL_AF   0x00000010
#define EFL_ZF   0x00000040
#define EFL_SF   0x00000080
#define EFL_TF   0x00000100
#define EFL_IF   0x00000200
#define EFL_DF   0x00000400
#define EFL_OF   0x00000800

#define CR0_PE   0x00000001
#define CR0_MP   0x00000002
#define CR0_EM   0x00000004
#define CR0_TS   0x00000008
#define CR0_WP   0x00010000
#define CR0_PG   0x80000000
#define CR4_PAE  0x00000020
#define CR4_OSFXSR     0x00000200  /* OS saves XMM state; SSE ops enabled */
#define CR4_OSXMMEXCPT 0x00000400

/* EFER (MSR 0xC0000080). Only NXE (no-execute enable) is modelled: it gates
 * whether the XD/NX bit (bit 63) of a PAE paging entry is enforced. */
#define EFER_NXE 0x00000800

/* Hidden segment descriptor cache (matches the fields the machine sets). */
typedef struct {
    uint16_t sel;
    uint16_t flags;   /* access-rights/flags word as passed by the machine */
    uint32_t base;
    uint32_t limit;
} X86Seg;

/* ---- software TLB -------------------------------------------------------
 *
 * Direct-mapped linear->host translation cache. Without it every operand
 * access and every instruction-fetch *byte* walks the page tables (two
 * phys_read()s, each a linear scan of the PhysMemoryRange table), which
 * dominates the interpreter's cost.
 *
 * `vaddr` is the page-aligned linear tag, or X86_TLB_EMPTY(slot) when the slot is
 * unused. A hit yields the host pointer as `mem_addend + la`.
 *
 * Entries are tagged by MMU index (0 = supervisor, 1 = user) rather than
 * flushed on every CPL change: a user/supervisor transition happens on every
 * syscall and every interrupt, so flushing there would throw the cache away
 * far too often. See mmu_index() in x86_cpu.c.
 *
 * Only RAM-backed pages are cached, so device MMIO keeps going through the
 * unsplit phys_read/phys_write path. Instruction fetch has its own tlb_code
 * (filled by the fetch path, which translates with ACC_FETCH and enforces the
 * PAE NX bit); a data read of a readable-but-non-executable page fills tlb_read
 * only, so a later fetch of that page still misses tlb_code and takes the NX
 * fault instead of hitting a data translation.
 */
#define X86_TLB_BITS  8
#define X86_TLB_SIZE  (1 << X86_TLB_BITS)

/* Tag parked in an unused slot.
 *
 * The hit test (see TLB_HIT in x86_cpu.c) is a single unsigned compare of
 * `la ^ vaddr` against a bound, which is only sound if an empty slot can never
 * satisfy it. That test matches only when `la` and the tag lie on the same
 * page, so an empty slot is made unmatchable by parking its tag on a page that
 * hashes to a *different* slot: an `la` reaching slot `i` has
 * `(la >> 12) & (SIZE-1) == i`, while this marker's page hashes to `i + 1`, so
 * "same page" and "reached slot i" cannot both hold.
 *
 * (The previous 0xffffffff marker relied on the tag never comparing equal to a
 * page base, which the xor test no longer requires -- and would not survive:
 * 0xffffffff is within a page-crossing distance of the top of the address
 * space.) */
#define X86_TLB_EMPTY(slot) \
    ((uint32_t)(((slot) + 1) & (X86_TLB_SIZE - 1)) << 12)

typedef struct {
    uint32_t  vaddr;        /* linear page base, or X86_TLB_EMPTY(slot) */
    uintptr_t mem_addend;   /* host pointer = mem_addend + linear address */
} X86TLBEntry;

/* One 128-bit XMM register, viewed as lanes of each SSE/SSE2 element type. */
typedef union {
    uint8_t  b[16];  int8_t  sb[16];
    uint16_t w[8];   int16_t sw[8];
    uint32_t d[4];   int32_t sd[4];
    uint64_t q[2];   int64_t sq[2];
    float    f[4];   double  fd[2];
} X86XMM;

struct X86CPUState {
    /* Architectural state */
    uint32_t regs[X86_NB_REGS];
    uint32_t eip;
    uint32_t eflags_;       /* see "deferred arithmetic flags" above */
    uint32_t cr0, cr2, cr3, cr4;

    /* Pending arithmetic-flag computation (see above). cc_a/cc_b/cc_r are
     * masked to cc_size, so the derivations need no further masking. */
    uint32_t cc_a, cc_b, cc_r;
    uint8_t  cc_op;         /* CC_OP_* */
    uint8_t  cc_size;       /* 1, 2 or 4 */
    uint8_t  cc_cin;        /* carry in (adc/sbb) or preserved CF (inc/dec) */
    X86Seg   seg[10];   /* ES,CS,SS,DS,FS,GS,LDT,TR,GDT,IDT (see x86_cpu.h) */

    /* SYSENTER/SYSEXIT fast-syscall MSRs (0x174/0x175/0x176). musl/glibc use the
     * sysenter path via the kernel vDSO, so these must be honored. */
    uint32_t sysenter_cs;
    uint32_t sysenter_esp;
    uint32_t sysenter_eip;

    /* EFER: only the low 32 bits are kept; NXE is the only bit that changes
     * translation. Its high half reads back as 0 via RDMSR. */
    uint32_t efer;

    /* x87 FPU (register stack modelled with 80-bit extended = C long double,
     * which is the native x87 format on x86 hosts; see the x87 section notes) */
    long double fpst[8];     /* physical registers; ST(i) = fpst[(fpu_top+i)&7] */
    int      fpu_top;        /* 0-7 */
    uint16_t fpu_cw;         /* control word  */
    uint16_t fpu_sw;         /* status word (condition codes; TOP kept separate) */
    uint8_t  fpu_tag[8];     /* per physical reg: 0=valid, 3=empty */

    /* SSE/SSE2 register file. Lanes go through the union in host byte order,
     * which matches the guest on the little-endian hosts we target (x86,
     * WASM). MXCSR: RC is honored, exception flags are not accumulated. */
    X86XMM   xmm[8];
    uint32_t mxcsr;

    /* Execution bookkeeping */
    int64_t  cycle_count;
    BOOL     halted;
    int      hard_irq;      /* level of the INTR pin from the PIC */
    int      addr_size;     /* address size of the instruction being decoded: 2 or 4 */
    int      int_shadow;    /* inhibit maskable IRQs for one instruction (mov ss/sti) */

    /* Instruction-fetch window: host pointers for the bytes from CS.base+eip up
     * to the end of that guest page. Refilled once per instruction by
     * fetch_refill(); without it every fetched byte costs a full ld_lin() (mmu
     * index, hash, tag compare, bounds compare), so a 5-byte instruction pays
     * that five times before any work happens.
     *
     * The window is now *carried across straight-line instructions*: the interp
     * loop re-runs fetch_refill() only when the window can no longer serve the
     * next opcode. That is decided by a self-checking invariant that needs no
     * per-handler bookkeeping. While fetching stays sequential, fetch8() bumps
     * fetch_ptr and eip in lockstep, so (uintptr_t)fetch_ptr - eip is invariant
     * across the window; fetch_refill() records it as win_eip_addend along with
     * win_cs_base. At the top of each instruction the loop keeps the window iff
     *   fetch_ptr < fetch_end   (not exhausted / page-crossed)
     *   && CS.base == win_cs_base
     *   && (uintptr_t)fetch_ptr == win_eip_addend + eip   (eip advanced only by fetch)
     * Any control transfer moves eip without moving fetch_ptr, so the last test
     * fails and the window is rebuilt; a far transfer also changes CS.base. The
     * one class the eip/CS test cannot see is a translation change with eip/CS
     * unchanged (CR3/CR0/CR4/EFER write, invlpg, external SMC/DMA flush), so the
     * TLB-flush paths set fetch_dirty to force a refill. Those flush instructions
     * finish fetching their own bytes before the flush runs, so setting the flag
     * (rather than clobbering fetch_ptr) never disturbs an in-flight fetch.
     *
     * When the window cannot be established both point at x86_fetch_none, so
     * `fetch_ptr < fetch_end` is false and every byte falls back to ld_lin() --
     * which keeps faults, MMIO and page-crossing fetches exactly as they were. */
    const uint8_t *fetch_ptr;
    const uint8_t *fetch_end;
    uintptr_t win_eip_addend;   /* (uintptr_t)fetch_ptr - eip for the live window */
    uint32_t  win_cs_base;      /* CS.base the live window was built against */
    int       fetch_dirty;      /* a TLB flush invalidated the window; force refill */

    /* Set when a write dropped JIT-compiled code (x86_jit.c); a compiled block
     * that stored leaves right after that instruction. */
    int       jit_abort;
    /* Instructions a compiled block had completed when it called a helper
     * that may fault; the fault landing adds them to cycle_count. Every
     * block exit clears it. */
    uint32_t  jit_pending;
    /* A compiled block stopped before an instruction the JIT does not handle:
     * once the interpreter has run that one instruction, it asks the JIT
     * again instead of interpreting on to the next branch. */
    int       jit_poke;
    /* x86_jit_step() is running one instruction for a compiled block: the
     * interpreter loop then neither delivers interrupts nor asks the JIT. */
    int       jit_step;
    /* Instructions run by the page functions a chain went through before the
     * current one (x86_jit_next, for run_chain to add up). */
    int       jit_chain_n;

    /* Fault/exception unwinding (see raise_exception / interp loop) */
    uint32_t cur_eip;       /* EIP at the start of the current instruction */
    jmp_buf  excbuf;
    int      exc_intno;
    uint32_t exc_error;
    int      exc_has_error;

    /* Wiring provided by the machine */
    PhysMemoryMap *mem_map;
    DeviceReadFunc  *port_read;
    DeviceWriteFunc *port_write;
    void  *port_opaque;
    uint64_t (*get_tsc)(void *opaque);
    void  *get_tsc_opaque;
    uint64_t tsc_base;      /* value of get_tsc() at first RDTSC (TSC starts ~0) */
    int      tsc_init;
    int   (*get_hard_intno)(void *opaque);
    void  *get_hard_intno_opaque;

    /* Software TLB. Last, and indexed [mmu_idx][slot], so the hot scalar state
     * above stays packed into the first cache lines. */
    X86TLBEntry tlb_read[2][X86_TLB_SIZE];
    X86TLBEntry tlb_write[2][X86_TLB_SIZE];
    X86TLBEntry tlb_code[2][X86_TLB_SIZE];   /* instruction fetch (NX-checked) */
};

/* ---- WebAssembly block JIT (x86_jit.c) ----------------------------------
 * Browser builds only: it emits WebAssembly modules at run time, which needs
 * the JS side (wasm/x86jit.js). -DX86_NO_JIT leaves it out. The debug
 * watches need every access on the interpreter's paths, so they do too. */
#if defined(__EMSCRIPTEN__) && !defined(X86_NO_JIT) && !defined(X86_TLB_DISABLE) && \
    !defined(X86_WATCH) && !defined(X86_PWATCH1) && !defined(X86_RWATCH) && \
    !defined(X86_NO_TLB) && !defined(X86_TRACE) && !defined(X86_UFAULT) && \
    !defined(X86_UWWATCH) && !defined(X86_OPHIST)
#define X86_JIT 1
#endif

#ifdef X86_JIT
extern uint32_t x86_jit_code_bits[];   /* one bit per 4 KB physical page */
int  x86_jit_run(X86CPUState *s);
#ifdef X86_JIT_STATS
/* debug: which instructions the interpreter runs for compiled blocks */
void x86_jit_note_stop(const uint8_t *p, const uint8_t *end);
void x86_jit_note_step(const uint8_t *p, const uint8_t *end);
extern uint64_t x86_jit_prof_interp;   /* instructions the interpreter ran */
extern uint64_t x86_jit_prof_by[];     /* ... by why the JIT didn't (x86_jit.c) */
extern int x86_jit_prof_cause;
extern uint64_t x86_jit_prof_steps, x86_jit_prof_step_exits;
#endif
void x86_jit_invalidate_page(X86CPUState *s, uint32_t pa);

/* A write to physical `pa` is about to land (CPU or DMA): drop compiled code
 * on that page. */
static inline void x86_jit_note_write(X86CPUState *s, uint32_t pa)
{
    if (x86_jit_code_bits[pa >> 17] & (1u << ((pa >> 12) & 31)))
        x86_jit_invalidate_page(s, pa);
}
static inline int x86_jit_page_has_code(uint32_t pa)
{
    return (x86_jit_code_bits[pa >> 17] >> ((pa >> 12) & 31)) & 1;
}
#endif

#endif /* X86_CPU_PRIV_H */
