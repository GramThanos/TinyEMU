/*
 * x86 CPU emulator - software interpreter
 *
 * Copyright (c) 2011-2017 Fabrice Bellard
 * Copyright (c) 2026 TinyEMU x86 software-CPU project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction ... (MIT, see LICENSE)
 *
 * FOUNDATION. This is a from-scratch, WASM-targetable
 * interpreter, filled in incrementally. Implemented so far:
 *   - flat 32-bit execution (paging OFF); linear == physical
 *   - ModR/M + SIB decode (32-bit addressing)
 *   - operand sizes 8/16/32 with 0x66/0x67 prefixes and segment overrides
 *   - ALU family (add/or/adc/sbb/and/sub/xor/cmp) with full flag model
 *   - mov (r/m<->r, r<->imm, r/m<->imm), inc/dec, push/pop, lea
 *   - jmp/Jcc (rel8/rel32), nop, hlt
 * NOT YET IMPLEMENTED (next increments): paging (CR0.PG), 16-bit addressing,
 * segmentation limit checks, interrupt/exception delivery (IDT), string ops,
 * x87/SSE, and the WASM JIT. Unknown opcodes stop the CPU with a diagnostic.
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#include <math.h>

#include "cutils.h"
#include "x86_cpu.h"
#include "x86_cpu_priv.h"

/* ---- diagnostics ------------------------------------------------------- */

static void x86_stop(X86CPUState *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "x86_cpu: STOP at CS:EIP=%04x:%08x: ",
            s->seg[X86_CPU_SEG_CS].sel, s->eip);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    s->halted = TRUE;
}

/* ---- physical / linear memory ----------------------------------------- */

static uint32_t phys_read(X86CPUState *s, uint32_t addr, int size)
{
#ifdef X86_RWATCH
    /* Optional physical read-watch: set X86_RWATCH_ADDR to a phys address to log
     * 4-byte reads of it (debugging aid; unset by default). */
#ifdef X86_RWATCH_ADDR
    if (size == 4 && addr == (X86_RWATCH_ADDR)) {
        PhysMemoryRange *rp = get_phys_mem_range(s->mem_map, addr);
        uint32_t v = 0xffffffffu;
        if (rp && rp->is_ram) { uint8_t *q = rp->phys_mem + (addr - rp->addr);
            v = q[0] | (q[1]<<8) | (q[2]<<16) | ((uint32_t)q[3]<<24); }
        fprintf(stderr, "[rwatch] rd %08x = %08x eip=%08x\n", addr, v, s->cur_eip);
    }
#endif
#endif
    PhysMemoryRange *pr = get_phys_mem_range(s->mem_map, addr);
    if (pr && pr->is_ram) {
        uint8_t *p = pr->phys_mem + (addr - pr->addr);
        switch (size) {
        case 1: return p[0];
        case 2: return p[0] | (p[1] << 8);
        default: return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
        }
    } else if (pr && pr->read_func) {
        int sl = (size == 1) ? 0 : (size == 2) ? 1 : 2;
        return pr->read_func(pr->opaque, (uint32_t)(addr - pr->addr), sl);
    }
    return 0xffffffff;
}

static void phys_write(X86CPUState *s, uint32_t addr, uint32_t val, int size)
{
#ifdef X86_WATCH
    /* Optional physical write-watch: set X86_WATCH_ADDR to a phys address to log
     * writes to it (debugging aid; unset by default). */
#ifdef X86_WATCH_ADDR
    if (addr == (X86_WATCH_ADDR))
        fprintf(stderr, "[watch] wr %08x = %08x (size %d) eip=%08x\n",
                addr, val, size, s->cur_eip);
#endif
#endif
#ifdef X86_PWATCH1
    /* Optional: set X86_PWATCH1_ADDR to a physical address to log all writes
     * (any size, any CPL) to that dword — used to trace who corrupts a slot. */
#ifdef X86_PWATCH1_ADDR
    if ((addr >= (X86_PWATCH1_ADDR) - 3u) && (addr <= (X86_PWATCH1_ADDR) + 3u))
        fprintf(stderr, "[pw1] wr phys=%08x val=%08x sz=%d eip=%08x cpl=%d\n",
                addr, val, size, s->cur_eip, s->seg[X86_CPU_SEG_CS].sel & 3);
#endif
#endif
    PhysMemoryRange *pr = get_phys_mem_range(s->mem_map, addr);
    if (pr && pr->is_ram) {
        if (pr->devram_flags & DEVRAM_FLAG_ROM)
            return;
        uint8_t *p = pr->phys_mem + (addr - pr->addr);
        switch (size) {
        case 1: p[0] = val; break;
        case 2: p[0] = val; p[1] = val >> 8; break;
        default: p[0] = val; p[1] = val >> 8; p[2] = val >> 16; p[3] = val >> 24; break;
        }
        phys_mem_set_dirty_bit(pr, (size_t)(addr - pr->addr));
    } else if (pr && pr->write_func) {
        int sl = (size == 1) ? 0 : (size == 2) ? 1 : 2;
        pr->write_func(pr->opaque, (uint32_t)(addr - pr->addr), val, sl);
    }
}

/* ---- exceptions -------------------------------------------------------- */

#define EXC_DE  0    /* divide error            */
#define EXC_UD  6    /* invalid opcode          */
#define EXC_BR  5    /* BOUND range exceeded    */
#define EXC_GP  13   /* general protection      */
#define EXC_PF  14   /* page fault              */

static void raise_exception_err(X86CPUState *s, int intno, uint32_t error)
{
    s->exc_intno = intno;
    s->exc_error = error;
    s->exc_has_error = 1;
    longjmp(s->excbuf, 1);
}
static void raise_exception(X86CPUState *s, int intno)
{
    s->exc_intno = intno;
    s->exc_error = 0;
    s->exc_has_error = 0;
    longjmp(s->excbuf, 1);
}

/* ---- linear -> physical (32-bit non-PAE paging; 4K & 4M pages) ---------- */

#define PG_P  0x001
#define PG_W  0x002
#define PG_U  0x004
#define PG_PS 0x080
#define PG_A  0x020
#define PG_D  0x040

/* Enforce page R/W (with CR0.WP) and U/S protection. Raises #PF on violation. */
static void pg_check(X86CPUState *s, uint32_t la, int is_write, int is_user,
                     int pg_wr, int pg_us)
{
    int fault = 0;
    if (is_user) {
        if (!pg_us) fault = 1;                       /* user access to supervisor page */
        else if (is_write && !pg_wr) fault = 1;      /* user write to read-only page   */
    } else if (is_write && !pg_wr && (s->cr0 & CR0_WP)) {
        fault = 1;                                   /* supervisor write to RO w/ WP=1 */
    }
    if (fault) {
        s->cr2 = la;
        raise_exception_err(s, EXC_PF, 1 | (is_write ? 2 : 0) | (is_user ? 4 : 0));
    }
}

/* Set while the CPU (not the guest program) reads a segment/gate descriptor or a
 * system table (GDT/LDT/IDT/TSS). Such accesses are always supervisor on real
 * hardware, regardless of CPL — critical because Linux maps the GDT/IDT read-only
 * in the supervisor fixmap, and `iret` loads the user CS (raising CPL to 3) before
 * reading the SS descriptor. Without this the descriptor read would be treated as a
 * user access to a supervisor page and #PF. */
static int g_sys_access;

static uint32_t lin_to_phys(X86CPUState *s, uint32_t la, int is_write)
{
    uint32_t pde_addr, pde, pte_addr, pte, phys;
    int is_user;

    if (!(s->cr0 & CR0_PG))
        return la;
    is_user = !g_sys_access && ((s->seg[X86_CPU_SEG_CS].sel & 3) == 3);

    if (s->cr4 & CR4_PAE) {
        /* PAE: 3-level walk, 8-byte entries (we only need each entry's low dword
         * since guest physical memory is 32-bit here). 2 MB large pages. */
        uint32_t pdpte, pde2, pte2;
        uint32_t pdpt_base = s->cr3 & ~0x1fu;
        pdpte = phys_read(s, pdpt_base + (((la >> 30) & 0x3) << 3), 4);
        if (!(pdpte & PG_P)) { s->cr2 = la;
            raise_exception_err(s, EXC_PF, (is_write ? 2 : 0) | (is_user ? 4 : 0)); }
        pde2 = phys_read(s, (pdpte & ~0xfffu) + (((la >> 21) & 0x1ff) << 3), 4);
        if (!(pde2 & PG_P)) { s->cr2 = la;
            raise_exception_err(s, EXC_PF, (is_write ? 2 : 0) | (is_user ? 4 : 0)); }
        if (pde2 & PG_PS) { /* 2 MB page */
            pg_check(s, la, is_write, is_user, (pde2 & PG_W) != 0, (pde2 & PG_U) != 0);
            return (pde2 & ~0x1fffffu) | (la & 0x1fffffu);
        }
        pte2 = phys_read(s, (pde2 & ~0xfffu) + (((la >> 12) & 0x1ff) << 3), 4);
        if (!(pte2 & PG_P)) { s->cr2 = la;
            raise_exception_err(s, EXC_PF, (is_write ? 2 : 0) | (is_user ? 4 : 0)); }
#ifdef X86_PF_DEBUG
        if (is_write && !((pde2 & pte2) & PG_W)) { static int n; if (n++ < 4)
            fprintf(stderr, "[wpfault] la=%08x pdpte=%08x pde2=%08x pte2=%08x cr0=%08x cpl=%d\n",
                    la, pdpte, pde2, pte2, s->cr0, s->seg[X86_CPU_SEG_CS].sel & 3); }
#endif
        pg_check(s, la, is_write, is_user, ((pde2 & pte2) & PG_W) != 0,
                 ((pde2 & pte2) & PG_U) != 0);
        return (pte2 & ~0xfffu) | (la & 0xfffu);
    }

    pde_addr = (s->cr3 & ~0xfffu) + (((la >> 22) & 0x3ff) << 2);
    pde = phys_read(s, pde_addr, 4);
    if (!(pde & PG_P)) {
        s->cr2 = la;
#ifdef X86_PF_DEBUG
        { static int n; if (n++ < 8) fprintf(stderr,
            "#PF la=%08x cr3=%08x [PDE@%08x=%08x not present] w=%d u=%d\n",
            la, s->cr3, pde_addr, pde, is_write, is_user); }
#endif
        raise_exception_err(s, EXC_PF, (is_write ? 2 : 0) | (is_user ? 4 : 0));
    }

    if (pde & PG_PS) { /* 4 MB page */
        pg_check(s, la, is_write, is_user, (pde & PG_W) != 0, (pde & PG_U) != 0);
        if (!(pde & PG_A)) phys_write(s, pde_addr, pde | PG_A, 4);
        phys = (pde & 0xffc00000u) | (la & 0x003fffffu);
    } else {
        pte_addr = (pde & ~0xfffu) + (((la >> 12) & 0x3ff) << 2);
        pte = phys_read(s, pte_addr, 4);
        if (!(pte & PG_P)) {
            s->cr2 = la;
#ifdef X86_PF_DEBUG
            { static int n; if (n++ < 8) fprintf(stderr,
                "#PF la=%08x cr3=%08x PDE@%08x=%08x [PTE@%08x=%08x not present] w=%d u=%d\n",
                la, s->cr3, pde_addr, pde, pte_addr, pte, is_write, is_user); }
#endif
            raise_exception_err(s, EXC_PF, (is_write ? 2 : 0) | (is_user ? 4 : 0));
        }
        pg_check(s, la, is_write, is_user, ((pde & pte) & PG_W) != 0,
                 ((pde & pte) & PG_U) != 0);
        {
            uint32_t need = PG_A | (is_write ? PG_D : 0);
            if ((pte & need) != need) phys_write(s, pte_addr, pte | need, 4);
        }
        if (!(pde & PG_A)) phys_write(s, pde_addr, pde | PG_A, 4);
        phys = (pte & ~0xfffu) | (la & 0xfffu);
    }
    return phys;
}

/* True when a `size`-byte access at linear address `la` spans two pages. Such
 * accesses must be translated per page: physical pages backing consecutive
 * linear pages are unrelated, so translating only the first byte and then
 * reading/writing `size` contiguous *physical* bytes silently touches the
 * wrong page (and skips the second page's present/permission checks). */
static int page_crosses(X86CPUState *s, uint32_t la, int size)
{
    /* With paging off, linear == physical, so a contiguous access is correct
     * (and must stay unsplit so device MMIO still sees one access). */
    if (!(s->cr0 & CR0_PG))
        return 0;
    return ((la & 0xfffu) + (uint32_t)size) > 0x1000u;
}

static uint32_t ld_lin(X86CPUState *s, uint32_t la, int size)
{
    if (page_crosses(s, la, size)) {
        uint32_t v = 0;
        int i;
        for (i = 0; i < size; i++)
            v |= (uint32_t)(phys_read(s, lin_to_phys(s, la + i, 0), 1) & 0xff) << (i * 8);
        return v;
    }
    return phys_read(s, lin_to_phys(s, la, 0), size);
}
#ifdef X86_UWWATCH
uint32_t g_uw_eip[4096], g_uw_la[4096], g_uw_val[4096];
uint8_t  g_uw_sz[4096];
int g_uw_i;
#endif
#ifdef X86_SYSLOG
int g_sys_pending; uint32_t g_sys_nr;
#endif
static void st_lin(X86CPUState *s, uint32_t la, uint32_t val, int size)
{
#ifdef X86_UWWATCH
    if ((s->seg[X86_CPU_SEG_CS].sel & 3) == 3) {
        g_uw_eip[g_uw_i & 4095] = s->cur_eip;
        g_uw_la[g_uw_i & 4095]  = la;
        g_uw_val[g_uw_i & 4095] = val;
        g_uw_sz[g_uw_i & 4095]  = (uint8_t)size;
        g_uw_i++;
    }
#endif
    if (page_crosses(s, la, size)) {
        /* Translate every byte first: a #PF on the second page must be raised
         * before any byte is committed, so the instruction restarts cleanly. */
        uint32_t pa[4];
        int i;
        for (i = 0; i < size; i++)
            pa[i] = lin_to_phys(s, la + i, 1);
        for (i = 0; i < size; i++)
            phys_write(s, pa[i], (val >> (i * 8)) & 0xff, 1);
        return;
    }
    phys_write(s, lin_to_phys(s, la, 1), val, size);
}

/* Translate every page touched by a `size`-byte write at `la` without storing
 * anything.  Instructions that commit more than one st_lin (cmpxchg8b, the
 * 64/80-bit x87 stores, `enter`) must probe the whole range first: otherwise a
 * #PF on a later page restarts the instruction after earlier bytes already
 * landed, and the restart sees mutated memory. */
static void probe_write(X86CPUState *s, uint32_t la, int size)
{
    uint32_t a = la, end = la + (uint32_t)size - 1;
    if (!(s->cr0 & CR0_PG))
        return;
    for (;;) {
        (void)lin_to_phys(s, a, 1);
        if ((a | 0xfffu) >= end)
            break;
        a = (a | 0xfffu) + 1;
    }
}

/* ---- instruction fetch ------------------------------------------------- */

static uint8_t fetch8(X86CPUState *s)
{
    uint32_t la = s->seg[X86_CPU_SEG_CS].base + s->eip;
    s->eip++;
    return (uint8_t)ld_lin(s, la, 1);
}
static uint16_t fetch16(X86CPUState *s)
{
    uint16_t v = fetch8(s);
    v |= (uint16_t)fetch8(s) << 8;
    return v;
}
static uint32_t fetch32(X86CPUState *s)
{
    uint32_t v = fetch16(s);
    v |= (uint32_t)fetch16(s) << 16;
    return v;
}
static uint32_t fetch_imm(X86CPUState *s, int size)
{
    if (size == 1) return (uint8_t)fetch8(s);
    if (size == 2) return fetch16(s);
    return fetch32(s);
}

/* ---- register access --------------------------------------------------- */

static uint32_t get_reg8(X86CPUState *s, int r)
{
    return (r < 4) ? (s->regs[r] & 0xff) : ((s->regs[r - 4] >> 8) & 0xff);
}
static void set_reg8(X86CPUState *s, int r, uint8_t v)
{
    if (r < 4)
        s->regs[r] = (s->regs[r] & ~0xffu) | v;
    else
        s->regs[r - 4] = (s->regs[r - 4] & ~0xff00u) | ((uint32_t)v << 8);
}
static uint32_t get_regv(X86CPUState *s, int r, int size)
{
    if (size == 1) return get_reg8(s, r);
    if (size == 2) return s->regs[r] & 0xffff;
    return s->regs[r];
}
static void set_regv(X86CPUState *s, int r, uint32_t v, int size)
{
    if (size == 1) set_reg8(s, r, (uint8_t)v);
    else if (size == 2) s->regs[r] = (s->regs[r] & ~0xffffu) | (v & 0xffff);
    else s->regs[r] = v;
}

/* ---- flags ------------------------------------------------------------- */

static int parity8(uint8_t v)
{
    v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return (~v) & 1;
}
static void set_szp(X86CPUState *s, uint32_t r, int size)
{
    uint32_t sbit = (size == 1) ? 0x80 : (size == 2) ? 0x8000 : 0x80000000u;
    s->eflags &= ~(EFL_SF | EFL_ZF | EFL_PF);
    if (r & sbit) s->eflags |= EFL_SF;
    if (r == 0)   s->eflags |= EFL_ZF;
    if (parity8((uint8_t)r)) s->eflags |= EFL_PF;
}

/* ALU op codes 0..7 = add,or,adc,sbb,and,sub,xor,cmp */
static uint32_t alu(X86CPUState *s, int op, uint32_t a, uint32_t b, int size)
{
    uint32_t mask = (size == 1) ? 0xff : (size == 2) ? 0xffff : 0xffffffffu;
    uint32_t sbit = (size == 1) ? 0x80 : (size == 2) ? 0x8000 : 0x80000000u;
    uint32_t r;
    int cf = 0, af = 0, of = 0;
    int cin = (s->eflags & EFL_CF) ? 1 : 0;
    uint64_t wide;

    switch (op) {
    case 0: /* add */
        wide = (uint64_t)a + b; r = (uint32_t)wide & mask;
        cf = (wide & ((uint64_t)mask + 1)) != 0;
        af = ((a ^ b ^ r) & 0x10) != 0;
        of = ((~(a ^ b)) & (a ^ r) & sbit) != 0;
        break;
    case 2: /* adc */
        wide = (uint64_t)a + b + cin; r = (uint32_t)wide & mask;
        cf = (wide & ((uint64_t)mask + 1)) != 0;
        af = ((a ^ b ^ r) & 0x10) != 0;
        of = ((~(a ^ b)) & (a ^ r) & sbit) != 0;
        break;
    case 5: /* sub */
    case 7: /* cmp */
        wide = (uint64_t)a - b; r = (uint32_t)wide & mask;
        cf = a < b;
        af = ((a ^ b ^ r) & 0x10) != 0;
        of = ((a ^ b) & (a ^ r) & sbit) != 0;
        break;
    case 3: /* sbb */
        wide = (uint64_t)a - b - cin; r = (uint32_t)wide & mask;
        cf = (uint64_t)a < (uint64_t)b + cin;
        af = ((a ^ b ^ r) & 0x10) != 0;
        of = ((a ^ b) & (a ^ r) & sbit) != 0;
        break;
    case 1: r = (a | b) & mask; break; /* or  */
    case 4: r = (a & b) & mask; break; /* and */
    case 6: r = (a ^ b) & mask; break; /* xor */
    default: r = 0; break;
    }

    s->eflags &= ~(EFL_CF | EFL_AF | EFL_OF);
    if (op == 1 || op == 4 || op == 6) {
        /* logic ops clear CF/OF, leave AF undefined (we clear it) */
    } else {
        if (cf) s->eflags |= EFL_CF;
        if (af) s->eflags |= EFL_AF;
        if (of) s->eflags |= EFL_OF;
    }
    set_szp(s, r, size);
    return r;
}

/* ---- ModR/M (32-bit addressing) --------------------------------------- */

typedef struct {
    int      is_reg;
    int      reg;      /* register index (rm), when is_reg */
    int      esp_base; /* ESP was the SIB base register of the address */
    uint32_t addr;     /* linear address, when memory */
    uint32_t ea;       /* effective address (segment offset), when memory */
} Modrm;

/* Address-size aware access to an address/count register (ESI/EDI/ECX): with a
 * 0x67 prefix the string ops and loop/jcxz use the 16-bit halves and wrap. */
static uint32_t adr_get(X86CPUState *s, int r)
{
    return (s->addr_size == 2) ? (s->regs[r] & 0xffffu) : s->regs[r];
}
static void adr_add(X86CPUState *s, int r, int32_t d)
{
    if (s->addr_size == 2)
        s->regs[r] = (s->regs[r] & ~0xffffu) | ((s->regs[r] + (uint32_t)d) & 0xffffu);
    else
        s->regs[r] += (uint32_t)d;
}

static uint32_t seg_base(X86CPUState *s, int seg_override, int default_seg)
{
    int seg = (seg_override >= 0) ? seg_override : default_seg;
    return s->seg[seg].base;
}

/* decode ModR/M byte (already fetched into m). reg field returned via *reg. */
static void decode_modrm(X86CPUState *s, uint8_t m, int seg_override,
                         int *reg, Modrm *rm)
{
    int mod = m >> 6, rmf = m & 7;
    *reg = (m >> 3) & 7;

    if (mod == 3) {
        rm->is_reg = 1;
        rm->reg = rmf;
        rm->esp_base = 0;
        return;
    }
    rm->is_reg = 0;
    rm->esp_base = 0;

    uint32_t ea = 0;
    int def_seg = X86_CPU_SEG_DS;

    if (s->addr_size == 2) {
        /* 16-bit addressing (0x67 in 32-bit code): no SIB, fixed base/index
         * pairs, and the offset wraps within 64K. */
        static const uint8_t bse[8] = { REG_EBX, REG_EBX, REG_EBP, REG_EBP,
                                        REG_ESI, REG_EDI, REG_EBP, REG_EBX };
        static const int8_t  idx[8] = { REG_ESI, REG_EDI, REG_ESI, REG_EDI,
                                        -1, -1, -1, -1 };
        if (rmf == 6 && mod == 0) {
            ea = fetch16(s);
        } else {
            ea = s->regs[bse[rmf]];
            if (idx[rmf] >= 0) ea += s->regs[idx[rmf]];
            if (bse[rmf] == REG_EBP) def_seg = X86_CPU_SEG_SS;
        }
        if (mod == 1) ea += (int32_t)(int8_t)fetch8(s);
        else if (mod == 2) ea += fetch16(s);
        ea &= 0xffffu;
        rm->ea = ea;
        rm->addr = seg_base(s, seg_override, def_seg) + ea;
        return;
    }

    if (rmf == 4) { /* SIB */
        uint8_t sib = fetch8(s);
        int scale = sib >> 6, index = (sib >> 3) & 7, base = sib & 7;
        if (index != 4)
            ea += s->regs[index] << scale;
        if (base == 5 && mod == 0) {
            ea += fetch32(s);
        } else {
            ea += s->regs[base];
            if (base == 4) rm->esp_base = 1;
            if (base == 4 || base == 5) def_seg = X86_CPU_SEG_SS;
        }
    } else if (rmf == 5 && mod == 0) {
        ea = fetch32(s);
    } else {
        ea = s->regs[rmf];
        if (rmf == 5) def_seg = X86_CPU_SEG_SS; /* [ebp] */
    }

    if (mod == 1) ea += (int32_t)(int8_t)fetch8(s);
    else if (mod == 2) ea += fetch32(s);

    rm->ea = ea;
    rm->addr = seg_base(s, seg_override, def_seg) + ea;
}

static uint32_t rm_read(X86CPUState *s, const Modrm *rm, int size)
{
    if (rm->is_reg) return get_regv(s, rm->reg, size);
    return ld_lin(s, rm->addr, size);
}
static void rm_write(X86CPUState *s, const Modrm *rm, uint32_t v, int size)
{
    if (rm->is_reg) set_regv(s, rm->reg, v, size);
    else st_lin(s, rm->addr, v, size);
}

/* ---- stack ------------------------------------------------------------- */

static void push_val(X86CPUState *s, uint32_t v, int size)
{
    /* Compute the new stack pointer but commit it only after the memory write
     * succeeds. If the write faults (e.g. a not-present stack page that the
     * kernel will demand-page), the instruction must restart cleanly: ESP must
     * be unchanged so the push is not applied twice. */
    uint32_t nsp = s->regs[REG_ESP] - size;
    st_lin(s, s->seg[X86_CPU_SEG_SS].base + nsp, v, size);
    s->regs[REG_ESP] = nsp;
}
static uint32_t pop_val(X86CPUState *s, int size)
{
    uint32_t v = ld_lin(s, s->seg[X86_CPU_SEG_SS].base + s->regs[REG_ESP], size);
    s->regs[REG_ESP] += size;
    return v;
}

/* ---- condition codes for Jcc ------------------------------------------ */

static int test_cc(X86CPUState *s, int cc)
{
    uint32_t f = s->eflags;
    int res;
    switch (cc >> 1) {
    case 0: res = (f & EFL_OF) != 0; break;                     /* O  */
    case 1: res = (f & EFL_CF) != 0; break;                     /* B  */
    case 2: res = (f & EFL_ZF) != 0; break;                     /* Z  */
    case 3: res = (f & (EFL_CF | EFL_ZF)) != 0; break;          /* BE */
    case 4: res = (f & EFL_SF) != 0; break;                     /* S  */
    case 5: res = (f & EFL_PF) != 0; break;                     /* P  */
    case 6: res = !!(f & EFL_SF) != !!(f & EFL_OF); break;      /* L  */
    default: res = (!!(f & EFL_SF) != !!(f & EFL_OF)) ||
                   (f & EFL_ZF); break;                          /* LE */
    }
    return (cc & 1) ? !res : res;
}

/* ---- segment loading (GDT/LDT descriptor lookup) ----------------------- */

static void load_seg(X86CPUState *s, int seg, uint16_t sel)
{
    X86Seg *sc = &s->seg[seg];
    uint32_t table_base, desc_addr, lo, hi, base, limit;
    uint16_t flags;
#ifdef X86_PROGRESS
    if (seg == X86_CPU_SEG_CS && (sel & 3) == 3 && (sc->sel & 3) != 3) {
        static int n; if (n++ < 4) {
            uint32_t pa = (s->cur_eip - 0xc0000000u) - 8; int k;
            fprintf(stderr, "[cs0->3] newcs=%04x @cur_eip=%08x oldcs=%04x ss=%04x esp=%08x\n  code@%08x:",
                    sel, s->cur_eip, sc->sel, s->seg[X86_CPU_SEG_SS].sel, s->regs[REG_ESP], pa);
            for (k = 0; k < 24; k++) fprintf(stderr, " %02x", phys_read(s, pa + k, 1));
            fprintf(stderr, "\n  stack@esp:");
            for (k = 0; k < 6; k++) fprintf(stderr, " %08x",
                phys_read(s, (s->seg[X86_CPU_SEG_SS].base + s->regs[REG_ESP]) + k*4, 4));
            fprintf(stderr, "\n");
        }
    }
#endif

    if ((sel & 0xfffc) == 0) {
        /* null selector: permitted for data segments (not enforced further yet) */
        sc->sel = sel; sc->base = 0; sc->limit = 0; sc->flags = 0;
        return;
    }
    table_base = (sel & 4) ? s->seg[X86_CPU_SEG_LDT].base
                           : s->seg[X86_CPU_SEG_GDT].base;
    desc_addr = table_base + (sel & 0xfff8);
    { int sv = g_sys_access; g_sys_access = 1;   /* descriptor read is supervisor */
      lo = ld_lin(s, desc_addr, 4);
      hi = ld_lin(s, desc_addr + 4, 4);
      g_sys_access = sv; }
    base  = (lo >> 16) | ((hi & 0xff) << 16) | (hi & 0xff000000u);
    limit = (lo & 0xffff) | (hi & 0x000f0000u);
    if (hi & 0x00800000u) /* G */ limit = (limit << 12) | 0xfff;
    flags = ((hi >> 8) & 0xff) | (((hi >> 20) & 0xf) << 12);
    sc->sel = sel; sc->base = base; sc->limit = limit; sc->flags = flags;
#ifdef X86_PROGRESS
    if (seg == X86_CPU_SEG_FS || seg == X86_CPU_SEG_GS) {
        static int n; if (n++ < 20)
            fprintf(stderr, "[loadseg] %s sel=%04x base=%08x limit=%08x @eip=%08x\n",
                    seg == X86_CPU_SEG_FS ? "FS" : "GS", sel, base, limit, s->cur_eip);
    }
#endif
}

/* Read (and discard) the descriptor `sel` refers to.  load_seg() can only fault
 * on that read, so probing it first lets an instruction take the fault before
 * committing any side effect (push, ESP update) and restart cleanly. */
static void probe_seg(X86CPUState *s, uint16_t sel)
{
    uint32_t table_base, desc_addr;
    int sv;
    if ((sel & 0xfffc) == 0)
        return;
    table_base = (sel & 4) ? s->seg[X86_CPU_SEG_LDT].base
                           : s->seg[X86_CPU_SEG_GDT].base;
    desc_addr = table_base + (sel & 0xfff8);
    sv = g_sys_access; g_sys_access = 1;
    (void)ld_lin(s, desc_addr, 4);
    (void)ld_lin(s, desc_addr + 4, 4);
    g_sys_access = sv;
}

/* Pop a segment selector: the descriptor is probed before ESP is committed so a
 * faulting descriptor read does not leave the stack popped. */
static void pop_seg(X86CPUState *s, int seg, int op_size)
{
    uint32_t osp = s->regs[REG_ESP];
    uint16_t sel = (uint16_t)ld_lin(s, s->seg[X86_CPU_SEG_SS].base + osp, op_size);
    probe_seg(s, sel);
    s->regs[REG_ESP] = osp + op_size;
    load_seg(s, seg, sel);
    if (seg == X86_CPU_SEG_SS)
        s->int_shadow = 1;      /* no IRQ on the transiently invalid SS:ESP */
}

/* Take a relative near branch.  With a 16-bit operand size the target is
 * truncated to IP: the top half of EIP is cleared (SDM, JMP/Jcc/CALL/LOOP). */
static void jmp_rel(X86CPUState *s, int32_t d, int op_size)
{
    uint32_t t = s->eip + (uint32_t)d;
    s->eip = (op_size == 2) ? (t & 0xffffu) : t;
}

/* Raise a fault whose saved EIP must point at the current instruction (a fault,
 * not a trap), undoing the fetches the decoder already did. */
static void raise_fault(X86CPUState *s, int intno)
{
    s->eip = s->cur_eip;
    raise_exception(s, intno);
}
static void raise_fault_err(X86CPUState *s, int intno, uint32_t err)
{
    s->eip = s->cur_eip;
    raise_exception_err(s, intno, err);
}

/* IOPL-sensitive instructions (cli/sti, in/out) #GP(0) at CPL > IOPL.
 * The TSS I/O permission bitmap is not modelled, so a port access above IOPL
 * always faults rather than consulting the bitmap. */
static void check_iopl(X86CPUState *s)
{
    int cpl = s->seg[X86_CPU_SEG_CS].sel & 3;
    int iopl = (s->eflags >> 12) & 3;
    if (cpl > iopl)
        raise_fault_err(s, EXC_GP, 0);
}

/* ---- shifts / rotates (group2) ---------------------------------------- */
/* op: 0 rol,1 ror,2 rcl,3 rcr,4 shl,5 shr,6 sal(=shl),7 sar */
static uint32_t do_shift(X86CPUState *s, int op, uint32_t v, int count, int size)
{
    uint32_t mask = (size == 1) ? 0xff : (size == 2) ? 0xffff : 0xffffffffu;
    uint32_t sbit = (size == 1) ? 0x80 : (size == 2) ? 0x8000 : 0x80000000u;
    int bits = size * 8;
    uint32_t res;
    int cf = 0, of = 0;

    count &= 0x1f;
    if (count == 0) return v & mask;
    v &= mask;
    res = v;

    switch (op) {
    case 4: case 6: /* shl */
        cf = (count <= bits) ? ((v >> (bits - count)) & 1) : 0;
        res = (count < bits) ? ((v << count) & mask) : 0;
        of = ((res & sbit) != 0) ^ (cf != 0);
        set_szp(s, res, size);
        break;
    case 5: /* shr */
        cf = (v >> (count - 1)) & 1;
        res = v >> count;
        of = (v & sbit) != 0;
        set_szp(s, res, size);
        break;
    case 7: { /* sar */
        int32_t sv = (size == 1) ? (int8_t)v : (size == 2) ? (int16_t)v : (int32_t)v;
        cf = (sv >> (count - 1)) & 1;
        res = (uint32_t)(sv >> count) & mask;
        of = 0;
        set_szp(s, res, size);
    } break;
    case 0: /* rol */
        count %= bits; if (count == 0) return v;
        res = ((v << count) | (v >> (bits - count))) & mask;
        cf = res & 1;
        of = ((res & sbit) != 0) ^ (cf != 0);
        break;
    case 1: /* ror */
        count %= bits; if (count == 0) return v;
        res = ((v >> count) | (v << (bits - count))) & mask;
        cf = (res & sbit) != 0;
        of = ((res & sbit) != 0) ^ ((res & (sbit >> 1)) != 0);
        break;
    case 2: case 3: { /* rcl / rcr */
        int cin = (s->eflags & EFL_CF) ? 1 : 0, k, newc;
        for (k = 0; k < count; k++) {
            if (op == 2) { newc = (v & sbit) != 0; v = ((v << 1) | cin) & mask; cin = newc; }
            else { newc = v & 1; v = (v >> 1) | ((uint32_t)cin << (bits - 1)); v &= mask; cin = newc; }
        }
        res = v; cf = cin;
        of = ((res & sbit) != 0) ^ (cf != 0);
    } break;
    }

    s->eflags &= ~(EFL_CF | EFL_OF);
    if (cf) s->eflags |= EFL_CF;
    if (count == 1 && of) s->eflags |= EFL_OF;
    return res;
}

/* ---- string operations ------------------------------------------------ */
/* op: 0 movs, 1 stos, 2 lods, 3 scas, 4 cmps */
static void string_step(X86CPUState *s, int op, int size, int seg_override)
{
    int delta = (s->eflags & EFL_DF) ? -size : size;
    uint32_t sbase = seg_base(s, seg_override, X86_CPU_SEG_DS);
    uint32_t ebase = s->seg[X86_CPU_SEG_ES].base;
    uint32_t si = adr_get(s, REG_ESI), di = adr_get(s, REG_EDI);
    switch (op) {
    case 0: /* movs */
        st_lin(s, ebase + di, ld_lin(s, sbase + si, size), size);
        adr_add(s, REG_ESI, delta); adr_add(s, REG_EDI, delta);
        break;
    case 1: /* stos */
        st_lin(s, ebase + di, get_regv(s, REG_EAX, size), size);
        adr_add(s, REG_EDI, delta);
        break;
    case 2: /* lods */
        set_regv(s, REG_EAX, ld_lin(s, sbase + si, size), size);
        adr_add(s, REG_ESI, delta);
        break;
    case 3: /* scas: eAX - [ES:EDI] */
        (void)alu(s, 7, get_regv(s, REG_EAX, size), ld_lin(s, ebase + di, size), size);
        adr_add(s, REG_EDI, delta);
        break;
    case 4: /* cmps: [DS:ESI] - [ES:EDI] */
        (void)alu(s, 7, ld_lin(s, sbase + si, size), ld_lin(s, ebase + di, size), size);
        adr_add(s, REG_ESI, delta); adr_add(s, REG_EDI, delta);
        break;
    }
}

static void string_op(X86CPUState *s, int op, int size, int seg_override, int rep)
{
    if (!rep) { string_step(s, op, size, seg_override); return; }
    while (adr_get(s, REG_ECX) != 0) {
        string_step(s, op, size, seg_override);
        adr_add(s, REG_ECX, -1);
        if (op == 3 || op == 4) { /* scas/cmps honor ZF for repe/repne */
            int zf = (s->eflags & EFL_ZF) != 0;
            if (rep == 1 && !zf) break;   /* repe:  stop when ZF=0 */
            if (rep == 2 && zf) break;    /* repne: stop when ZF=1 */
        }
    }
}

/* ---- x87 FPU ----------------------------------------------------------- */
/* Modelled with C doubles on an 8-entry register stack. ST(i) is the logical
 * register i from the top of stack. State save/restore (FSAVE/FRSTOR) marshals
 * to/from the 80-bit extended layout the kernel expects. Precision is reduced
 * to double, which is sufficient to boot and run typical userspace. */

#define FPU_C0 0x0100
#define FPU_C1 0x0200
#define FPU_C2 0x0400
#define FPU_C3 0x4000

static double fpu_get(X86CPUState *s, int i) { return s->fpst[(s->fpu_top + i) & 7]; }
static void   fpu_set(X86CPUState *s, int i, double v) { s->fpst[(s->fpu_top + i) & 7] = v; }
static void   fpu_push(X86CPUState *s, double v)
{
    s->fpu_top = (s->fpu_top - 1) & 7;
    s->fpst[s->fpu_top] = v;
    s->fpu_tag[s->fpu_top] = 0;
}
static void fpu_pop(X86CPUState *s)
{
    s->fpu_tag[s->fpu_top] = 3;
    s->fpu_top = (s->fpu_top + 1) & 7;
}
static void fpu_init(X86CPUState *s)
{
    int i;
    s->fpu_cw = 0x037f; s->fpu_sw = 0; s->fpu_top = 0;
    for (i = 0; i < 8; i++) { s->fpst[i] = 0.0; s->fpu_tag[i] = 3; }
}
static void fpu_setcc(X86CPUState *s, double a, double b)
{
    s->fpu_sw &= ~(FPU_C0 | FPU_C2 | FPU_C3);
    if (isnan(a) || isnan(b)) s->fpu_sw |= FPU_C0 | FPU_C2 | FPU_C3;
    else if (a < b) s->fpu_sw |= FPU_C0;
    else if (a == b) s->fpu_sw |= FPU_C3;
}

/* IEEE double <-> 80-bit extended */
static void dbl_to_f80(double d, uint8_t *p)
{
    union { double dd; uint64_t u; } u; u.dd = d;
    uint64_t bits = u.u, frac = bits & 0xfffffffffffffULL, mant;
    int sign = (bits >> 63) & 1, exp = (bits >> 52) & 0x7ff, e80;
    if (exp == 0) { mant = 0; e80 = 0; }               /* zero/subnormal -> 0 */
    else if (exp == 0x7ff) { e80 = 0x7fff; mant = 0x8000000000000000ULL | (frac << 11); }
    else { e80 = exp - 1023 + 16383; mant = 0x8000000000000000ULL | (frac << 11); }
    { int i; for (i = 0; i < 8; i++) p[i] = (mant >> (8 * i)) & 0xff; }
    { uint16_t se = (sign << 15) | (e80 & 0x7fff); p[8] = se & 0xff; p[9] = se >> 8; }
}
static double f80_to_dbl(const uint8_t *p)
{
    uint64_t mant = 0; int i;
    for (i = 0; i < 8; i++) mant |= (uint64_t)p[i] << (8 * i);
    uint16_t se = p[8] | (p[9] << 8);
    int sign = (se >> 15) & 1, e80 = se & 0x7fff, exp;
    union { double dd; uint64_t u; } u;
    if (e80 == 0) { u.u = (uint64_t)sign << 63; return u.dd; }
    if (e80 == 0x7fff) { u.u = ((uint64_t)sign << 63) | (0x7ffULL << 52) |
                               (((mant << 1) >> 12)); return u.dd; }
    exp = e80 - 16383 + 1023;
    if (exp <= 0) { u.u = (uint64_t)sign << 63; return u.dd; }
    if (exp >= 0x7ff) { u.u = ((uint64_t)sign << 63) | (0x7ffULL << 52); return u.dd; }
    u.u = ((uint64_t)sign << 63) | ((uint64_t)exp << 52) |
          ((mant & 0x7fffffffffffffffULL) >> 11);
    return u.dd;
}

/* load/store a 32/64-bit float from linear memory */
static double fpu_ld_mem(X86CPUState *s, uint32_t la, int size)
{
    if (size == 4) { union { float f; uint32_t u; } u; u.u = ld_lin(s, la, 4); return u.f; }
    { union { double d; uint64_t u; } u;
      u.u = (uint64_t)ld_lin(s, la, 4) | ((uint64_t)ld_lin(s, la + 4, 4) << 32);
      return u.d; }
}
static void fpu_st_mem(X86CPUState *s, uint32_t la, double v, int size)
{
    if (size == 4) { union { float f; uint32_t u; } u; u.f = (float)v; st_lin(s, la, u.u, 4); }
    else { union { double d; uint64_t u; } u; u.d = v;
           probe_write(s, la, 8);   /* both dwords must fault before either lands */
           st_lin(s, la, (uint32_t)u.u, 4); st_lin(s, la + 4, (uint32_t)(u.u >> 32), 4); }
}

/* FSAVE/FRSTOR 32-bit (108-byte) image */
static void fpu_save(X86CPUState *s, uint32_t la)
{
    uint8_t buf[10]; int i;
    uint16_t tw = 0;
    probe_write(s, la, 108);        /* the whole 108-byte image, before any store */
    st_lin(s, la + 0, s->fpu_cw, 4);
    st_lin(s, la + 4, s->fpu_sw | ((s->fpu_top & 7) << 11), 4);
    for (i = 0; i < 8; i++) tw |= (uint16_t)(s->fpu_tag[i] & 3) << (i * 2);
    st_lin(s, la + 8, tw, 4);
    for (i = 12; i < 28; i += 4) st_lin(s, la + i, 0, 4);
    for (i = 0; i < 8; i++) {
        int k;
        dbl_to_f80(s->fpst[(s->fpu_top + i) & 7], buf);
        for (k = 0; k < 10; k++) st_lin(s, la + 28 + i * 10 + k, buf[k], 1);
    }
    fpu_init(s); /* FSAVE reinitializes the FPU */
}
static void fpu_restore(X86CPUState *s, uint32_t la)
{
    uint8_t buf[10]; int i, k;
    uint16_t sw, tw;
    s->fpu_cw = ld_lin(s, la + 0, 2);
    sw = ld_lin(s, la + 4, 2);
    s->fpu_top = (sw >> 11) & 7;
    s->fpu_sw = sw & ~0x3800;
    tw = ld_lin(s, la + 8, 2);
    for (i = 0; i < 8; i++) {
        for (k = 0; k < 10; k++) buf[k] = ld_lin(s, la + 28 + i * 10 + k, 1);
        s->fpst[(s->fpu_top + i) & 7] = f80_to_dbl(buf);
    }
    for (i = 0; i < 8; i++) s->fpu_tag[i] = (tw >> (i * 2)) & 3;
}

/* Execute an x87 escape opcode (0xD8..0xDF). `b` is the opcode byte. */
static void fpu_exec(X86CPUState *s, int b, int seg_override)
{
    uint8_t modrm = fetch8(s);
    int reg, mod = modrm >> 6;
    Modrm rm;

    if ((s->cr0 & (CR0_EM | CR0_TS))) { /* fault before touching state */
        s->eip = s->cur_eip; raise_exception(s, 7);
    }
    decode_modrm(s, modrm, seg_override, &reg, &rm);

    if (mod != 3) {
        uint32_t la = rm.addr;
        switch (b) {
        case 0xd9: /* m32 real + control */
            switch (reg) {
            case 0: fpu_push(s, fpu_ld_mem(s, la, 4)); return;          /* fld m32  */
            case 2: fpu_st_mem(s, la, fpu_get(s, 0), 4); return;        /* fst m32  */
            case 3: fpu_st_mem(s, la, fpu_get(s, 0), 4); fpu_pop(s); return; /* fstp m32 */
            case 5: s->fpu_cw = ld_lin(s, la, 2); return;               /* fldcw    */
            case 6: return;                                             /* fnstenv (ignore) */
            case 7: st_lin(s, la, s->fpu_cw, 2); return;                /* fnstcw   */
            case 4: return;                                             /* fldenv (ignore) */
            }
            break;
        case 0xdd: /* m64 real + state */
            switch (reg) {
            case 0: fpu_push(s, fpu_ld_mem(s, la, 8)); return;          /* fld m64  */
            case 2: fpu_st_mem(s, la, fpu_get(s, 0), 8); return;        /* fst m64  */
            case 3: fpu_st_mem(s, la, fpu_get(s, 0), 8); fpu_pop(s); return; /* fstp m64 */
            case 4: fpu_restore(s, la); return;                         /* frstor   */
            case 6: fpu_save(s, la); return;                            /* fnsave   */
            case 7: st_lin(s, la, s->fpu_sw | ((s->fpu_top & 7) << 11), 2); return; /* fnstsw */
            }
            break;
        case 0xdb: /* m32 int + m80 */
            switch (reg) {
            case 0: fpu_push(s, (double)(int32_t)ld_lin(s, la, 4)); return; /* fild m32 */
            case 2: st_lin(s, la, (int32_t)llrint(fpu_get(s, 0)), 4); return; /* fist m32 */
            case 3: st_lin(s, la, (int32_t)llrint(fpu_get(s, 0)), 4); fpu_pop(s); return;
            case 5: { uint8_t buf[10]; int k; for (k=0;k<10;k++) buf[k]=ld_lin(s,la+k,1);
                      fpu_push(s, f80_to_dbl(buf)); return; }           /* fld m80  */
            case 7: { uint8_t buf[10]; int k; dbl_to_f80(fpu_get(s,0),buf);
                      probe_write(s, la, 10);
                      for (k=0;k<10;k++) st_lin(s,la+k,buf[k],1);
                      fpu_pop(s); return; } /* fstp m80 */
            }
            break;
        case 0xdf: /* m16 int + m64 int */
            switch (reg) {
            case 0: fpu_push(s, (double)(int16_t)ld_lin(s, la, 2)); return; /* fild m16 */
            case 2: st_lin(s, la, (int16_t)llrint(fpu_get(s, 0)), 2); return;
            case 3: st_lin(s, la, (int16_t)llrint(fpu_get(s, 0)), 2); fpu_pop(s); return;
            case 5: { int64_t v = (int64_t)ld_lin(s,la,4) | ((int64_t)ld_lin(s,la+4,4)<<32);
                      fpu_push(s, (double)v); return; }                 /* fild m64 */
            case 7: { int64_t v = (int64_t)llrint(fpu_get(s,0));
                      probe_write(s, la, 8);
                      st_lin(s,la,(uint32_t)v,4); st_lin(s,la+4,(uint32_t)(v>>32),4);
                      fpu_pop(s); return; }                             /* fistp m64 */
            }
            break;
        case 0xda: case 0xd8: case 0xdc: case 0xde: { /* arithmetic with memory operand */
            double m = (b == 0xd8) ? fpu_ld_mem(s, la, 4)
                     : (b == 0xdc) ? fpu_ld_mem(s, la, 8)
                     : (b == 0xda) ? (double)(int32_t)ld_lin(s, la, 4)  /* m32int */
                     : (double)(int16_t)ld_lin(s, la, 2); /* 0xde: m16int */
            double st0 = fpu_get(s, 0), r = st0;
            switch (reg) {
            case 0: r = st0 + m; break;
            case 1: r = st0 * m; break;
            case 2: fpu_setcc(s, st0, m); return;               /* fcom  */
            case 3: fpu_setcc(s, st0, m); fpu_pop(s); return;   /* fcomp */
            case 4: r = st0 - m; break;
            case 5: r = m - st0; break;
            case 6: r = st0 / m; break;
            case 7: r = m / st0; break;
            }
            fpu_set(s, 0, r); return;
        }
        }
        x86_stop(s, "x87 mem opcode %02x /%d", b, reg);
        return;
    }

    /* mod == 3: register / no-operand forms */
    {
        int rr = modrm & 7;
        switch (b) {
        case 0xd9:
            if (modrm >= 0xc0 && modrm <= 0xc7) { fpu_push(s, fpu_get(s, rr)); return; } /* fld st(i) */
            if (modrm >= 0xc8 && modrm <= 0xcf) { double t = fpu_get(s,0); fpu_set(s,0,fpu_get(s,rr)); fpu_set(s,rr,t); return; } /* fxch */
            switch (modrm) {
            case 0xd0: return;                                  /* fnop */
            case 0xe0: fpu_set(s, 0, -fpu_get(s, 0)); return;   /* fchs */
            case 0xe1: fpu_set(s, 0, fabs(fpu_get(s, 0))); return; /* fabs */
            case 0xe4: fpu_setcc(s, fpu_get(s, 0), 0.0); return; /* ftst */
            case 0xe5: { double v = fpu_get(s,0); s->fpu_sw &= ~(FPU_C0|FPU_C1|FPU_C2|FPU_C3); /* fxam */
                         if (isnan(v)) s->fpu_sw |= FPU_C0;
                         else if (isinf(v)) s->fpu_sw |= FPU_C0|FPU_C2;
                         else if (v==0) s->fpu_sw |= FPU_C3;
                         else s->fpu_sw |= FPU_C2;
                         if (signbit(v)) s->fpu_sw |= FPU_C1; return; }
            case 0xe8: fpu_push(s, 1.0); return;                /* fld1 */
            case 0xe9: fpu_push(s, 3.3219280948873623); return; /* fldl2t */
            case 0xea: fpu_push(s, 1.4426950408889634); return; /* fldl2e */
            case 0xeb: fpu_push(s, 3.141592653589793); return;  /* fldpi */
            case 0xec: fpu_push(s, 0.30102999566398120); return;/* fldlg2 */
            case 0xed: fpu_push(s, 0.69314718055994531); return;/* fldln2 */
            case 0xee: fpu_push(s, 0.0); return;                /* fldz */
            case 0xfa: fpu_set(s, 0, sqrt(fpu_get(s, 0))); return; /* fsqrt */
            case 0xfc: fpu_set(s, 0, rint(fpu_get(s, 0))); return; /* frndint */
            case 0xfe: fpu_set(s, 0, sin(fpu_get(s, 0))); return;  /* fsin */
            case 0xff: fpu_set(s, 0, cos(fpu_get(s, 0))); return;  /* fcos */
            case 0xf1: { double y=fpu_get(s,1),x=fpu_get(s,0); fpu_set(s,1,y*log2(x)); fpu_pop(s); return; } /* fyl2x */
            case 0xf8: { double a=fpu_get(s,0),bb=fpu_get(s,1); fpu_set(s,0,fmod(a,bb)); s->fpu_sw&=~FPU_C2; return; } /* fprem */
            case 0xfd: { double st0=fpu_get(s,0),st1=fpu_get(s,1); fpu_set(s,0, st0*pow(2.0, trunc(st1))); return; } /* fscale */
            case 0xf6: s->fpu_top = (s->fpu_top+1)&7; return;   /* fdecstp (approx) */
            case 0xf7: s->fpu_top = (s->fpu_top-1)&7; return;   /* fincstp (approx) */
            }
            break;
        case 0xda:
            if (modrm == 0xe9) { fpu_setcc(s, fpu_get(s,0), fpu_get(s,1)); fpu_pop(s); fpu_pop(s); return; } /* fucompp */
            { int cond = -1;
              if (modrm >= 0xc0 && modrm <= 0xc7) cond = (s->eflags & EFL_CF) != 0;       /* fcmovb  */
              else if (modrm >= 0xc8 && modrm <= 0xcf) cond = (s->eflags & EFL_ZF) != 0;  /* fcmove  */
              else if (modrm >= 0xd0 && modrm <= 0xd7) cond = (s->eflags & (EFL_CF|EFL_ZF)) != 0; /* fcmovbe */
              else if (modrm >= 0xd8 && modrm <= 0xdf) cond = (s->eflags & EFL_PF) != 0;  /* fcmovu  */
              if (cond >= 0) { if (cond) fpu_set(s, 0, fpu_get(s, rr)); return; } }
            break;
        case 0xdb:
            switch (modrm) {
            case 0xe0: case 0xe1: case 0xe2: case 0xe4: return; /* fneni/fndisi/fnclex/fnsetpm */
            case 0xe3: fpu_init(s); return;                     /* fninit */
            }
            { int cond = -1; /* fcmovNcc */
              if (modrm >= 0xc0 && modrm <= 0xc7) cond = (s->eflags & EFL_CF) == 0;
              else if (modrm >= 0xc8 && modrm <= 0xcf) cond = (s->eflags & EFL_ZF) == 0;
              else if (modrm >= 0xd0 && modrm <= 0xd7) cond = (s->eflags & (EFL_CF|EFL_ZF)) == 0;
              else if (modrm >= 0xd8 && modrm <= 0xdf) cond = (s->eflags & EFL_PF) == 0;
              if (cond >= 0) { if (cond) fpu_set(s, 0, fpu_get(s, rr)); return; } }
            if (modrm >= 0xe8 && modrm <= 0xf7) { /* fcomi/fucomi -> set EFLAGS */
                double a = fpu_get(s, 0), c = fpu_get(s, rr);
                s->eflags &= ~(EFL_ZF | EFL_PF | EFL_CF);
                if (isnan(a) || isnan(c)) s->eflags |= EFL_ZF | EFL_PF | EFL_CF;
                else if (a < c) s->eflags |= EFL_CF;
                else if (a == c) s->eflags |= EFL_ZF;
                return;
            }
            break;
        case 0xdf:
            if (modrm == 0xe0) { set_regv(s, REG_EAX, s->fpu_sw | ((s->fpu_top & 7) << 11), 2); return; } /* fnstsw ax */
            if (modrm >= 0xe8 && modrm <= 0xf7) { /* fcomip/fucomip */
                double a = fpu_get(s, 0), c = fpu_get(s, rr);
                s->eflags &= ~(EFL_ZF | EFL_PF | EFL_CF);
                if (isnan(a) || isnan(c)) s->eflags |= EFL_ZF | EFL_PF | EFL_CF;
                else if (a < c) s->eflags |= EFL_CF;
                else if (a == c) s->eflags |= EFL_ZF;
                fpu_pop(s); return;
            }
            break;
        case 0xdd:
            if (modrm >= 0xc0 && modrm <= 0xc7) { s->fpu_tag[(s->fpu_top+rr)&7] = 3; return; } /* ffree */
            if (modrm >= 0xd0 && modrm <= 0xd7) { fpu_set(s, rr, fpu_get(s, 0)); return; }     /* fst st(i) */
            if (modrm >= 0xd8 && modrm <= 0xdf) { fpu_set(s, rr, fpu_get(s, 0)); fpu_pop(s); return; } /* fstp st(i) */
            if (modrm >= 0xe0 && modrm <= 0xe7) { fpu_setcc(s, fpu_get(s,0), fpu_get(s,rr)); return; } /* fucom */
            if (modrm >= 0xe8 && modrm <= 0xef) { fpu_setcc(s, fpu_get(s,0), fpu_get(s,rr)); fpu_pop(s); return; } /* fucomp */
            break;
        case 0xd8: case 0xdc: case 0xde: { /* register arithmetic */
            /* D8: st0 op= st(i);  DC: st(i) op= st0;  DE: st(i) op= st0 then pop */
            int dst = (b == 0xd8) ? 0 : rr;
            double a = fpu_get(s, dst), c = (b == 0xd8) ? fpu_get(s, rr) : fpu_get(s, 0), r = a;
            int grp = (modrm >> 3) & 7;
            if ((b == 0xde && modrm == 0xd9)) { /* fcompp */
                fpu_setcc(s, fpu_get(s,0), fpu_get(s,1)); fpu_pop(s); fpu_pop(s); return;
            }
            switch (grp) {
            case 0: r = a + c; break;                            /* fadd  */
            case 1: r = a * c; break;                            /* fmul  */
            case 2: fpu_setcc(s, fpu_get(s,0), fpu_get(s,rr)); return;              /* fcom  */
            case 3: fpu_setcc(s, fpu_get(s,0), fpu_get(s,rr)); fpu_pop(s); return;  /* fcomp */
            case 4: r = (b == 0xd8) ? a - c : c - a; break;      /* fsub(D8)/fsubr(DC/DE) */
            case 5: r = (b == 0xd8) ? c - a : a - c; break;      /* fsubr(D8)/fsub(DC/DE) */
            case 6: r = (b == 0xd8) ? a / c : c / a; break;      /* fdiv / fdivr */
            case 7: r = (b == 0xd8) ? c / a : a / c; break;      /* fdivr / fdiv */
            }
            fpu_set(s, dst, r);
            if (b == 0xde) fpu_pop(s);                           /* the P (pop) forms */
            return;
        }
        }
        x86_stop(s, "x87 reg opcode %02x modrm=%02x", b, modrm);
    }
}

/* ---- interrupt / exception delivery (protected mode, 32-bit gates) ----- */

/* `is_software` marks the INT n / INT3 / INTO opcodes.  Those, unlike hardware
 * IRQs and exceptions, are subject to the gate's DPL: without the check any
 * ring-3 program could invoke an arbitrary IDT vector (e.g. `int $14` to fake a
 * page fault). */
static BOOL deliver_interrupt(X86CPUState *s, int intno, uint32_t error,
                              int has_error, int is_software)
{
    X86Seg *idt = &s->seg[X86_CPU_SEG_IDT];
    uint32_t gaddr, lo, hi, off;
    uint16_t sel;
    int type, present, old_cpl, dpl;
    uint32_t old_eflags = s->eflags;
    uint16_t old_cs = s->seg[X86_CPU_SEG_CS].sel;
    uint32_t old_eip = s->eip;
    uint32_t old_esp = s->regs[REG_ESP];
    uint16_t old_ss = s->seg[X86_CPU_SEG_SS].sel;

    if (!(s->cr0 & CR0_PE))
        return FALSE;                       /* real-mode IVT not yet supported */
    if ((uint32_t)intno * 8 + 7 > idt->limit)
        return FALSE;

    gaddr = idt->base + (uint32_t)intno * 8;
    { int sv = g_sys_access; g_sys_access = 1;   /* IDT gate read is supervisor */
      lo = ld_lin(s, gaddr, 4);
      hi = ld_lin(s, gaddr + 4, 4);
      g_sys_access = sv; }
    sel = (lo >> 16) & 0xffff;
    off = (lo & 0xffff) | (hi & 0xffff0000u);
    type = (hi >> 8) & 0x1f;
    present = (hi >> 15) & 1;
    if (!present || (type != 0x0e && type != 0x0f))
        return FALSE;

    old_cpl = old_cs & 3;
    if (is_software && (int)((hi >> 13) & 3) < old_cpl)
        raise_fault_err(s, EXC_GP, (uint32_t)intno * 8 + 2);
    load_seg(s, X86_CPU_SEG_CS, sel);
    dpl = (s->seg[X86_CPU_SEG_CS].flags >> 5) & 3;
#ifdef X86_PROGRESS
    { static int n; if (old_eip >= 0xc0520000u && old_eip <= 0xc0530000u && n++ < 8)
        fprintf(stderr,
        "[deliver] intno=%d old_cs=%04x(cpl%d) old_eip=%08x gate_sel=%04x dpl=%d cr2=%08x\n",
        intno, old_cs, old_cpl, old_eip, sel, dpl, s->cr2); }
#endif

    if (dpl < old_cpl) {
        /* privilege change: switch to the ring-`dpl` stack from the TSS */
        uint32_t tss = s->seg[X86_CPU_SEG_TR].base;
        uint32_t new_esp, new_ss16;
        { int sv = g_sys_access; g_sys_access = 1;   /* TSS read is supervisor */
          new_esp = ld_lin(s, tss + 4 + dpl * 8, 4);
          new_ss16 = ld_lin(s, tss + 8 + dpl * 8, 4);
          g_sys_access = sv; }
        uint16_t new_ss = (uint16_t)new_ss16;
        load_seg(s, X86_CPU_SEG_SS, new_ss);
        s->regs[REG_ESP] = new_esp;
        push_val(s, old_ss, 4);
        push_val(s, old_esp, 4);
#ifdef X86_PROGRESS
        { static int n; if (n++ < 8) fprintf(stderr,
            "[int->ring%d] intno=%d from cpl%d oldcs=%04x oldeip=%08x oldss=%04x oldesp=%08x -> kss=%04x kesp=%08x\n",
            dpl, intno, old_cpl, old_cs, old_eip, old_ss, old_esp, new_ss, new_esp); }
#endif
    }
    push_val(s, old_eflags, 4);
    push_val(s, old_cs, 4);
    push_val(s, old_eip, 4);
    if (has_error)
        push_val(s, error, 4);

    s->eip = off;
    if (type == 0x0e)               /* interrupt gate clears IF */
        s->eflags &= ~EFL_IF;
    s->eflags &= ~EFL_TF;
    return TRUE;
}

/* IRET (32-bit) */
static void do_iret(X86CPUState *s)
{
    uint32_t neip = pop_val(s, 4);
    uint16_t ncs  = (uint16_t)pop_val(s, 4);
    uint32_t nefl = pop_val(s, 4);
    int old_cpl = s->seg[X86_CPU_SEG_CS].sel & 3;
    int new_rpl = ncs & 3;

    if (new_rpl > old_cpl) {
        uint32_t nesp = pop_val(s, 4);
        uint16_t nss  = (uint16_t)pop_val(s, 4);
        load_seg(s, X86_CPU_SEG_CS, ncs);
        load_seg(s, X86_CPU_SEG_SS, nss);
        s->regs[REG_ESP] = nesp;
#ifdef X86_PROGRESS
        { static int n; if (n++ < 8) fprintf(stderr,
            "[iret->ring%d] neip=%08x ncs=%04x nss=%04x nesp=%08x nefl=%08x (from cpl%d)\n",
            new_rpl, neip, ncs, nss, nesp, nefl, old_cpl); }
#endif
    } else {
        load_seg(s, X86_CPU_SEG_CS, ncs);
    }
    s->eip = neip;
    {   /* IOPL is writable only at CPL 0, and IF only at CPL <= IOPL. */
        uint32_t mask = 0x00257fd5u;
        if (old_cpl > 0) mask &= ~0x3000u;
        if (old_cpl > (int)((s->eflags >> 12) & 3)) mask &= ~(uint32_t)EFL_IF;
        s->eflags = (s->eflags & ~mask) | (nefl & mask) | 0x2;
    }
}

/* ---- main interpreter loop -------------------------------------------- */

#ifdef X86_PROGRESS
static void scan_log(X86CPUState *s)
{
    PhysMemoryRange *pr = get_phys_mem_range(s->mem_map, 0x200000);
    const char *needles[] = { "Linux version 4", "Command line:", "Kernel panic",
                              "Oops", "BUG:", "Call Trace", 0 };
    size_t lim, i, j; int ni;
    if (!pr || !pr->is_ram) return;
    lim = pr->org_size; if (lim > 0x4000000) lim = 0x4000000;
    for (ni = 0; needles[ni]; ni++) {
        size_t nl = strlen(needles[ni]); int hits = 0;
        for (i = 0; i + nl < lim && hits < 4; i++) {
            if (memcmp(pr->phys_mem + i, needles[ni], nl) == 0) {
                hits++;
                fprintf(stderr, "[log] '%s' #%d @%08lx: ", needles[ni], hits,
                        (unsigned long)(pr->addr + i));
                for (j = i; j < i + 240 && j < lim; j++) {
                    int ch = pr->phys_mem[j];
                    fputc((ch >= 32 && ch < 127) || ch == '\n' ? ch : '.', stderr);
                }
                fprintf(stderr, "\n");
            }
        }
    }
}
#endif

#ifdef X86_UFAULT
/* Userspace-only (CPL3) instruction ring — not swamped by kernel syscall/IRQ/idle
 * instructions the way the full trace ring is. Dumped on the first small-cr2 user
 * #PF (a null/near-null pointer deref), to see how the bad pointer was produced. */
#define UF_N 512
static uint32_t g_uf_eip[UF_N], g_uf_eax[UF_N], g_uf_ecx[UF_N], g_uf_edx[UF_N],
                g_uf_ebx[UF_N], g_uf_esp[UF_N], g_uf_ebp[UF_N], g_uf_esi[UF_N],
                g_uf_edi[UF_N];
static uint8_t  g_uf_op[UF_N];
static int g_uf_i, g_uf_dumped;
static void x86_ufault_dump(X86CPUState *s)
{
    int k;
    if (g_uf_dumped) return;
    g_uf_dumped = 1;
    fprintf(stderr, "x86_ufault: user #PF cr2=%08x eip=%08x cs=%04x ss=%04x esp=%08x\n"
            "  eax=%08x ecx=%08x edx=%08x ebx=%08x ebp=%08x esi=%08x edi=%08x\n",
            s->cr2, s->cur_eip, s->seg[X86_CPU_SEG_CS].sel, s->seg[X86_CPU_SEG_SS].sel,
            s->regs[REG_ESP], s->regs[REG_EAX], s->regs[REG_ECX], s->regs[REG_EDX],
            s->regs[REG_EBX], s->regs[REG_EBP], s->regs[REG_ESI], s->regs[REG_EDI]);
    /* code bytes at the faulting instruction (translate eip via the page tables) */
    fprintf(stderr, "  code@eip:");
    for (k = 0; k < 16; k++) fprintf(stderr, " %02x", ld_lin(s, s->cur_eip + k, 1));
    fprintf(stderr, "\n  last %d userspace instrs (oldest..newest):\n", UF_N);
    for (k = 0; k < UF_N; k++) {
        int idx = (g_uf_i + k) & (UF_N - 1);
        if (g_uf_eip[idx] || g_uf_op[idx])
            fprintf(stderr, "  eip=%08x op=%02x eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n",
                    g_uf_eip[idx], g_uf_op[idx], g_uf_eax[idx], g_uf_ecx[idx],
                    g_uf_edx[idx], g_uf_ebx[idx], g_uf_esp[idx], g_uf_ebp[idx],
                    g_uf_esi[idx], g_uf_edi[idx]);
    }
    fprintf(stderr, "  code bytes at the last 28 userspace eips:\n");
    for (k = UF_N - 28; k < UF_N; k++) {
        int idx = (g_uf_i + k) & (UF_N - 1), j;
        if (!(g_uf_eip[idx] || g_uf_op[idx])) continue;
        fprintf(stderr, "    %08x:", g_uf_eip[idx]);
        for (j = 0; j < 10; j++) fprintf(stderr, " %02x", ld_lin(s, g_uf_eip[idx] + j, 1));
        fprintf(stderr, "\n");
    }
}
#endif

#ifdef X86_TRACE
static uint32_t g_trace_eip[2048];
static uint8_t  g_trace_op[2048];
static uint32_t g_trace_edi[2048];
static uint32_t g_trace_esi[2048];
static uint32_t g_trace_eax[2048];
static uint32_t g_trace_ecx[2048];
static uint32_t g_trace_edx[2048];
static uint32_t g_trace_ebx[2048];
static uint32_t g_trace_ebp[2048];
static uint32_t g_trace_efl[2048];
static int g_trace_i;
static int g_trace_dumped;
static void x86_trace_dump(X86CPUState *s)
{
    int k;
    if (g_trace_dumped) return;
    g_trace_dumped = 1;
    fprintf(stderr, "x86_cpu: recent instruction trace (oldest..newest):\n");
    for (k = 0; k < 2048; k++) {
        int idx = (g_trace_i + k) & 2047;
        if (g_trace_eip[idx] || g_trace_op[idx])
            fprintf(stderr, "  eip=%08x op=%02x efl=%08x eax=%08x ecx=%08x edx=%08x ebx=%08x ebp=%08x esi=%08x edi=%08x\n",
                    g_trace_eip[idx], g_trace_op[idx], g_trace_efl[idx], g_trace_eax[idx], g_trace_ecx[idx],
                    g_trace_edx[idx], g_trace_ebx[idx], g_trace_ebp[idx], g_trace_esi[idx], g_trace_edi[idx]);
    }
    { uint32_t pa0 = (s->cur_eip >= 0xc0000000u) ? s->cur_eip - 0xc0000000u : s->cur_eip;
      uint32_t pa = pa0 - 0x40;
      fprintf(stderr, "x86_cpu: code bytes @phys %08x (fault at +0x40):", pa);
      for (k = 0; k < 0x54; k++) {
          if ((k & 0xf) == 0) fprintf(stderr, "\n  %08x:", pa + k);
          fprintf(stderr, " %02x", phys_read(s, pa + k, 1));
      }
      fprintf(stderr, "\n"); }
    {
        uint32_t pd = s->cr3 & ~0xfffu;
        uint32_t pdei = (s->cr2 >> 22) & 0x3ff;
        uint32_t pde = phys_read(s, pd + pdei * 4, 4);
        fprintf(stderr, "x86_cpu: cr2=%08x cr3=%08x PDE[%u]=%08x\n", s->cr2, s->cr3, pdei, pde);
        if (pde & 1 && !(pde & 0x80)) {
            uint32_t pt = pde & ~0xfffu, ptei = (s->cr2 >> 12) & 0x3ff;
            int j;
            fprintf(stderr, "x86_cpu: PT@%08x [%u..]:", pt, ptei - 2);
            for (j = -2; j <= 3; j++)
                fprintf(stderr, " %08x", phys_read(s, pt + (ptei + j) * 4, 4));
            fprintf(stderr, "\n");
        }
    }
}
#endif

void x86_cpu_interp(X86CPUState *s, int max_cycles)
{
    int i;

    if (setjmp(s->excbuf)) {
        /* A fault/exception was raised mid-instruction; the restart/report point
         * is the faulting instruction. Vector it through the IDT. */
        s->eip = s->cur_eip;
#ifdef X86_UFAULT
        if (s->exc_intno == 14 && (s->seg[X86_CPU_SEG_CS].sel & 3) == 3 &&
            s->cr2 < 0x1000u)
            x86_ufault_dump(s);
#endif
#ifdef X86_PROGRESS
        { static int n; if (s->exc_intno < 32 && n++ < 60)
            fprintf(stderr, "[exc] intno=%d err=0x%x @eip=%08x cs=%04x(cpl%d) ss=%04x cr2=%08x eax=%08x cr0=%08x\n",
                    s->exc_intno, s->exc_error, s->cur_eip,
                    s->seg[X86_CPU_SEG_CS].sel, s->seg[X86_CPU_SEG_CS].sel & 3,
                    s->seg[X86_CPU_SEG_SS].sel, s->cr2, s->regs[REG_EAX], s->cr0); }
#ifdef X86_TRACE
        { static int dd; if (s->exc_intno == 14 && !dd &&
              (s->seg[X86_CPU_SEG_CS].sel & 3) == 3 &&
              (s->cr2 < 0x10000u || s->cur_eip < 0x10000u)) { dd = 1; x86_trace_dump(s); } }
#endif
#ifdef X86_UWWATCH
        { static int dd; if (s->exc_intno == 14 && !dd &&
              (s->seg[X86_CPU_SEG_CS].sel & 3) == 3 &&
              (s->cr2 < 0x10000u || s->cur_eip < 0x10000u)) { dd = 1;
            uint32_t esp = s->regs[REG_ESP], lo = esp - 0x40, hi = esp + 0x200;
            int k;
            fprintf(stderr, "[uww] fault cr2=%08x eip=%08x esp=%08x; CPL3 writes into [%08x..%08x]:\n",
                    s->cr2, s->cur_eip, esp, lo, hi);
            for (k = 0; k < 4096; k++) {
                int idx = (g_uw_i + k) & 4095;
                if (g_uw_eip[idx] && g_uw_la[idx] >= lo && g_uw_la[idx] <= hi)
                    fprintf(stderr, "  eip=%08x wrote la=%08x val=%08x sz=%d\n",
                            g_uw_eip[idx], g_uw_la[idx], g_uw_val[idx], g_uw_sz[idx]);
            } } }
#endif
#endif
        if (!deliver_interrupt(s, s->exc_intno, s->exc_error, s->exc_has_error, 0)) {
#ifdef X86_TRACE
            x86_trace_dump(s);
#endif
            x86_stop(s, "unhandled exception %d (err=0x%x)",
                     s->exc_intno, s->exc_error);
        }
    }

    for (i = 0; i < max_cycles && !s->halted; i++) {
        int op_size, seg_override = -1, rep = 0;
        int b, reg, def_op;
        Modrm rm;

        s->cur_eip = s->eip;
#ifdef X86_SYSLOG
        if (g_sys_pending && (s->seg[X86_CPU_SEG_CS].sel & 3) == 3) {
            g_sys_pending = 0;
            fprintf(stderr, "[sys]   ret nr=%u -> eax=%08x eip=%08x\n",
                    g_sys_nr, s->regs[REG_EAX], s->cur_eip);
        }
#endif
#ifdef X86_PROGRESS
        { static uint64_t c = 0;
          if ((++c % 5000000) == 0)
              fprintf(stderr, "[prog] n=%lluM eip=%08x cs=%04x eflags=%08x irq=%d\n",
                      (unsigned long long)(c / 1000000), s->eip,
                      s->seg[X86_CPU_SEG_CS].sel, s->eflags, s->hard_irq); }
#endif

        /* Deliver a pending hardware interrupt when interrupts are enabled.
         * `sti` and stack-segment loads inhibit delivery for exactly one
         * instruction, so an IRQ is never taken on a transiently invalid
         * SS:ESP (the architectural "interrupt shadow"). */
        if (s->int_shadow) {
            s->int_shadow = 0;
        } else if (s->hard_irq && (s->eflags & EFL_IF) && s->get_hard_intno) {
            int intno = s->get_hard_intno(s->get_hard_intno_opaque);
            if (intno >= 0) {
#ifdef X86_PROGRESS
                { static uint64_t q = 0; if (intno < 0x20 || (++q % 500) == 0)
                    fprintf(stderr, "[irq] intno=%d -> eip=%08x eflags=%08x\n",
                            intno, s->cur_eip, s->eflags); }
#endif
                deliver_interrupt(s, intno, 0, 0, 0);
                s->cur_eip = s->eip;
            }
        }

        def_op = (s->seg[X86_CPU_SEG_CS].flags & 0x4000) ? 4 : 2; /* D/B bit */
        op_size = def_op;
        s->addr_size = def_op;

        /* prefixes */
        for (;;) {
            b = fetch8(s);
            if (b == 0x66) { op_size = (def_op == 4) ? 2 : 4; }
            else if (b == 0x67) { s->addr_size = (def_op == 4) ? 2 : 4; }
            else if (b == 0xf0) { /* lock: ignore */ }
            else if (b == 0xf3) rep = 1;  /* rep / repe  */
            else if (b == 0xf2) rep = 2;  /* repne       */
            else if (b == 0x2e) seg_override = X86_CPU_SEG_CS;
            else if (b == 0x36) seg_override = X86_CPU_SEG_SS;
            else if (b == 0x3e) seg_override = X86_CPU_SEG_DS;
            else if (b == 0x26) seg_override = X86_CPU_SEG_ES;
            else if (b == 0x64) seg_override = X86_CPU_SEG_FS;
            else if (b == 0x65) seg_override = X86_CPU_SEG_GS;
            else break;
        }

#ifdef X86_TRACE
        g_trace_eip[g_trace_i] = s->cur_eip;
        g_trace_op[g_trace_i] = (uint8_t)b;
        g_trace_eax[g_trace_i] = s->regs[REG_EAX];
        g_trace_ecx[g_trace_i] = s->regs[REG_ECX];
        g_trace_edx[g_trace_i] = s->regs[REG_EDX];
        g_trace_ebx[g_trace_i] = s->regs[REG_EBX];
        g_trace_ebp[g_trace_i] = s->regs[REG_EBP];
        g_trace_efl[g_trace_i] = s->eflags;
        g_trace_esi[g_trace_i] = s->regs[REG_ESI];
        g_trace_edi[g_trace_i] = s->regs[REG_EDI];
        g_trace_i = (g_trace_i + 1) & 2047;
#endif
#ifdef X86_UFAULT
        if ((s->seg[X86_CPU_SEG_CS].sel & 3) == 3) {
            int j = g_uf_i & (UF_N - 1);
            g_uf_eip[j] = s->cur_eip; g_uf_op[j] = (uint8_t)b;
            g_uf_eax[j] = s->regs[REG_EAX]; g_uf_ecx[j] = s->regs[REG_ECX];
            g_uf_edx[j] = s->regs[REG_EDX]; g_uf_ebx[j] = s->regs[REG_EBX];
            g_uf_esp[j] = s->regs[REG_ESP]; g_uf_ebp[j] = s->regs[REG_EBP];
            g_uf_esi[j] = s->regs[REG_ESI]; g_uf_edi[j] = s->regs[REG_EDI];
            g_uf_i++;
        }
#endif
#ifdef X86_PROGRESS
        /* Track entry into user mode (ring 3) and userspace progress. */
        { static int seen_u3; static uint64_t uc;
          int cpl = s->seg[X86_CPU_SEG_CS].sel & 3;
          if (cpl == 3) {
              if (!seen_u3) { seen_u3 = 1;
                  fprintf(stderr, "[user] FIRST ring3: eip=%08x cs=%04x esp=%08x ss=%04x\n",
                          s->cur_eip, s->seg[X86_CPU_SEG_CS].sel,
                          s->regs[REG_ESP], s->seg[X86_CPU_SEG_SS].sel); }
              if ((++uc % 2000000) == 0)
                  fprintf(stderr, "[user] ring3 n=%lluM eip=%08x\n",
                          (unsigned long long)(uc/1000000), s->cur_eip);
          } }
#endif

        /* ALU family: 0x00..0x3B in blocks of 8 (add,or,adc,sbb,and,sub,xor,cmp) */
        if (b < 0x40 && (b & 7) < 6 && (b & 0xc0) == 0) {
            int aluop = b >> 3;
            int form = b & 7;
            uint32_t a, v, r;
            switch (form) {
            case 0: { /* r/m8, r8 */
                uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
                a = rm_read(s, &rm, 1); v = get_reg8(s, reg);
                r = alu(s, aluop, a, v, 1);
                if (aluop != 7) rm_write(s, &rm, r, 1);
            } break;
            case 1: { /* r/m, r */
                uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
                a = rm_read(s, &rm, op_size); v = get_regv(s, reg, op_size);
                r = alu(s, aluop, a, v, op_size);
                if (aluop != 7) rm_write(s, &rm, r, op_size);
            } break;
            case 2: { /* r8, r/m8 */
                uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
                a = get_reg8(s, reg); v = rm_read(s, &rm, 1);
                r = alu(s, aluop, a, v, 1);
                if (aluop != 7) set_reg8(s, reg, r);
            } break;
            case 3: { /* r, r/m */
                uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
                a = get_regv(s, reg, op_size); v = rm_read(s, &rm, op_size);
                r = alu(s, aluop, a, v, op_size);
                if (aluop != 7) set_regv(s, reg, r, op_size);
            } break;
            case 4: { /* AL, imm8 */
                a = get_reg8(s, REG_EAX); v = (uint8_t)fetch8(s);
                r = alu(s, aluop, a, v, 1);
                if (aluop != 7) set_reg8(s, REG_EAX, r);
            } break;
            case 5: { /* eAX, imm */
                a = get_regv(s, REG_EAX, op_size); v = fetch_imm(s, op_size);
                r = alu(s, aluop, a, v, op_size);
                if (aluop != 7) set_regv(s, REG_EAX, r, op_size);
            } break;
            }
            s->cycle_count++;
            continue;
        }

        switch (b) {
        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44: case 0x45: case 0x46: case 0x47: { /* inc reg */
            int r = b & 7, cf = s->eflags & EFL_CF;
            uint32_t v = alu(s, 0, get_regv(s, r, op_size), 1, op_size);
            set_regv(s, r, v, op_size);
            s->eflags = (s->eflags & ~EFL_CF) | cf; /* inc preserves CF */
        } break;
        case 0x48: case 0x49: case 0x4a: case 0x4b:
        case 0x4c: case 0x4d: case 0x4e: case 0x4f: { /* dec reg */
            int r = b & 7, cf = s->eflags & EFL_CF;
            uint32_t v = alu(s, 5, get_regv(s, r, op_size), 1, op_size);
            set_regv(s, r, v, op_size);
            s->eflags = (s->eflags & ~EFL_CF) | cf;
        } break;
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57: /* push reg */
            push_val(s, get_regv(s, b & 7, op_size), op_size);
            break;
        case 0x58: case 0x59: case 0x5a: case 0x5b:
        case 0x5c: case 0x5d: case 0x5e: case 0x5f: /* pop reg */
            set_regv(s, b & 7, pop_val(s, op_size), op_size);
            break;
        case 0x68: push_val(s, fetch_imm(s, op_size), op_size); break; /* push imm32 */
        case 0x6a: push_val(s, (int32_t)(int8_t)fetch8(s), op_size); break; /* push imm8 */

        case 0x80: case 0x82: { /* group1 r/m8, imm8 */
            uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
            uint32_t a = rm_read(s, &rm, 1), v = (uint8_t)fetch8(s);
            uint32_t r = alu(s, reg, a, v, 1);
            if (reg != 7) rm_write(s, &rm, r, 1);
        } break;
        case 0x81: { /* group1 r/m, imm */
            uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
            uint32_t a = rm_read(s, &rm, op_size), v = fetch_imm(s, op_size);
            uint32_t r = alu(s, reg, a, v, op_size);
            if (reg != 7) rm_write(s, &rm, r, op_size);
        } break;
        case 0x83: { /* group1 r/m, imm8 (sign-extended) */
            uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
            uint32_t a = rm_read(s, &rm, op_size);
            uint32_t v = (uint32_t)(int32_t)(int8_t)fetch8(s);
            uint32_t r = alu(s, reg, a, v, op_size);
            if (reg != 7) rm_write(s, &rm, r, op_size);
        } break;

        case 0x88: { /* mov r/m8, r8 */
            uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
            rm_write(s, &rm, get_reg8(s, reg), 1);
        } break;
        case 0x89: { /* mov r/m, r */
            uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
            rm_write(s, &rm, get_regv(s, reg, op_size), op_size);
        } break;
        case 0x8a: { /* mov r8, r/m8 */
            uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
            set_reg8(s, reg, rm_read(s, &rm, 1));
        } break;
        case 0x8b: { /* mov r, r/m */
            uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
            set_regv(s, reg, rm_read(s, &rm, op_size), op_size);
        } break;
        case 0x8d: { /* lea r, m */
            uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
            if (rm.is_reg) { x86_stop(s, "lea with register operand"); break; }
            /* The effective address is the segment *offset*: decode_modrm may have
             * formed rm.addr with the SS base ([esp+n] / [ebp+n]), so subtracting
             * the DS base here would be wrong under a non-flat model. */
            set_regv(s, reg, rm.ea, op_size);
        } break;

        case 0xb0: case 0xb1: case 0xb2: case 0xb3:
        case 0xb4: case 0xb5: case 0xb6: case 0xb7: /* mov r8, imm8 */
            set_reg8(s, b & 7, (uint8_t)fetch8(s));
            break;
        case 0xb8: case 0xb9: case 0xba: case 0xbb:
        case 0xbc: case 0xbd: case 0xbe: case 0xbf: /* mov r, imm */
            set_regv(s, b & 7, fetch_imm(s, op_size), op_size);
            break;
        case 0xc6: { /* mov r/m8, imm8 */
            uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
            rm_write(s, &rm, (uint8_t)fetch8(s), 1);
        } break;
        case 0xc7: { /* mov r/m, imm */
            uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
            rm_write(s, &rm, fetch_imm(s, op_size), op_size);
        } break;

        case 0x90: /* nop */ break;

        case 0xeb: { /* jmp rel8 */
            int32_t d = (int8_t)fetch8(s); jmp_rel(s, d, op_size);
        } break;
        case 0xe9: { /* jmp rel */
            int32_t d = (op_size == 2) ? (int16_t)fetch16(s) : (int32_t)fetch32(s);
            jmp_rel(s, d, op_size);
        } break;
        case 0x70: case 0x71: case 0x72: case 0x73:
        case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7a: case 0x7b:
        case 0x7c: case 0x7d: case 0x7e: case 0x7f: { /* Jcc rel8 */
            int32_t d = (int8_t)fetch8(s);
            if (test_cc(s, b & 0xf)) jmp_rel(s, d, op_size);
        } break;
        case 0x0f: { /* two-byte */
            uint8_t b2 = fetch8(s);
            if (b2 >= 0x80 && b2 <= 0x8f) { /* Jcc rel */
                int32_t d = (op_size == 2) ? (int16_t)fetch16(s) : (int32_t)fetch32(s);
                if (test_cc(s, b2 & 0xf)) jmp_rel(s, d, op_size);
            } else if (b2 >= 0x90 && b2 <= 0x9f) { /* setcc r/m8 */
                uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
                rm_write(s, &rm, test_cc(s, b2 & 0xf) ? 1 : 0, 1);
            } else if (b2 == 0xb6 || b2 == 0xb7) { /* movzx r, r/m8|16 */
                int ssz = (b2 == 0xb6) ? 1 : 2;
                uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
                set_regv(s, reg, rm_read(s, &rm, ssz), op_size);
            } else if (b2 == 0xbe || b2 == 0xbf) { /* movsx r, r/m8|16 */
                int ssz = (b2 == 0xbe) ? 1 : 2;
                uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
                uint32_t v = rm_read(s, &rm, ssz);
                int32_t sv = (ssz == 1) ? (int8_t)v : (int16_t)v;
                set_regv(s, reg, (uint32_t)sv, op_size);
            } else if (b2 == 0x20) { /* mov r32, crN */
                uint8_t m = fetch8(s); int cr = (m >> 3) & 7, r = m & 7;
                uint32_t v = (cr == 0) ? s->cr0 : (cr == 2) ? s->cr2 :
                             (cr == 3) ? s->cr3 : (cr == 4) ? s->cr4 : 0;
                s->regs[r] = v;
            } else if (b2 == 0x22) { /* mov crN, r32 */
                uint8_t m = fetch8(s); int cr = (m >> 3) & 7, r = m & 7;
                uint32_t v = s->regs[r];
                switch (cr) {
                case 0: s->cr0 = v; break;
                case 2: s->cr2 = v; break;
                case 3: s->cr3 = v; break;
                case 4: s->cr4 = v; break;
                }
            } else if (b2 == 0xaf) { /* imul r, r/m */
                uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
                int64_t a = (op_size == 2) ? (int16_t)get_regv(s, reg, 2) : (int32_t)get_regv(s, reg, 4);
                int64_t c = (op_size == 2) ? (int16_t)rm_read(s, &rm, 2) : (int32_t)rm_read(s, &rm, 4);
                int64_t r = a * c;
                set_regv(s, reg, (uint32_t)r, op_size);
                s->eflags &= ~(EFL_CF | EFL_OF);
                if ((op_size == 2 && r != (int16_t)r) || (op_size == 4 && r != (int32_t)r))
                    s->eflags |= EFL_CF | EFL_OF;
            } else if (b2 == 0x01) { /* group7: [ls]gdt/[ls]idt/[ls]msw/invlpg */
                uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
                switch (reg) {
                case 0: st_lin(s, rm.addr, s->seg[X86_CPU_SEG_GDT].limit, 2);
                        st_lin(s, rm.addr + 2, s->seg[X86_CPU_SEG_GDT].base, 4); break; /* sgdt */
                case 1: st_lin(s, rm.addr, s->seg[X86_CPU_SEG_IDT].limit, 2);
                        st_lin(s, rm.addr + 2, s->seg[X86_CPU_SEG_IDT].base, 4); break; /* sidt */
                case 2: s->seg[X86_CPU_SEG_GDT].limit = ld_lin(s, rm.addr, 2);
                        s->seg[X86_CPU_SEG_GDT].base  = ld_lin(s, rm.addr + 2, 4); break; /* lgdt */
                case 3: s->seg[X86_CPU_SEG_IDT].limit = ld_lin(s, rm.addr, 2);
                        s->seg[X86_CPU_SEG_IDT].base  = ld_lin(s, rm.addr + 2, 4); break; /* lidt */
                case 4: rm_write(s, &rm, s->cr0 & 0xffff, 2); break;                     /* smsw */
                case 6: s->cr0 = (s->cr0 & ~0xfu) | (rm_read(s, &rm, 2) & 0xf); break;   /* lmsw */
                case 7: break;                                                          /* invlpg */
                default: x86_stop(s, "0F 01 /%d", reg); break;
                }
            } else if (b2 == 0x00) { /* group6: [ls]ldt / [ls]tr */
                uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
                switch (reg) {
                case 0: rm_write(s, &rm, s->seg[X86_CPU_SEG_LDT].sel, 2); break; /* sldt */
                case 1: rm_write(s, &rm, s->seg[X86_CPU_SEG_TR].sel, 2); break;  /* str  */
                case 2: load_seg(s, X86_CPU_SEG_LDT, (uint16_t)rm_read(s, &rm, 2)); break; /* lldt */
                case 3: load_seg(s, X86_CPU_SEG_TR,  (uint16_t)rm_read(s, &rm, 2)); break; /* ltr  */
                default: x86_stop(s, "0F 00 /%d", reg); break;
                }
            } else if (b2 == 0x06) { /* clts */
                s->cr0 &= ~CR0_TS;
            } else if (b2 == 0xa2) { /* cpuid */
                uint32_t leaf = s->regs[REG_EAX], a = 0, bb = 0, c = 0, d = 0;
                switch (leaf) {
                case 0: /* max leaf + "GenuineIntel" */
                    a = 1; bb = 0x756e6547; d = 0x49656e69; c = 0x6c65746e;
                    break;
                case 1:
                    /* Family 6, model 3. Feature set a PAE Linux needs, minus
                     * things we don't emulate (no FPU: image uses math-emu; no
                     * APIC/ACPI; no MMX/SSE). Bits: VME DE PSE TSC MSR PAE MCE
                     * CX8 SEP MTRR PGE MCA CMOV PAT. */
                    a = 0x00000633;
                    d = (1u<<0)|(1u<<1)|(1u<<2)|(1u<<3)|(1u<<4)|(1u<<5)|(1u<<6)|(1u<<7)|
                        (1u<<8)|(1u<<11)|(1u<<12)|(1u<<13)|(1u<<14)|(1u<<15)|(1u<<16);
                    /* FPU bit set; FXSR/SSE deliberately NOT advertised so the
                     * kernel uses the simpler FSAVE/FRSTOR (108-byte) FPU state. */
                    break;
                default: break;
                }
                s->regs[REG_EAX] = a; s->regs[REG_EBX] = bb;
                s->regs[REG_ECX] = c; s->regs[REG_EDX] = d;
            } else if (b2 == 0x31) { /* rdtsc */
                uint64_t t = s->get_tsc ? s->get_tsc(s->get_tsc_opaque)
                                        : (uint64_t)s->cycle_count;
                if (!s->tsc_init) { s->tsc_base = t; s->tsc_init = 1; }
                t -= s->tsc_base;   /* TSC starts near 0 at boot, like real hw */
                s->regs[REG_EAX] = (uint32_t)t; s->regs[REG_EDX] = (uint32_t)(t >> 32);
            } else if (b2 == 0x32) { /* rdmsr */
                uint32_t ecx = s->regs[REG_ECX], lo = 0, hi = 0;
                switch (ecx) {
                case 0x174: lo = s->sysenter_cs;  break;
                case 0x175: lo = s->sysenter_esp; break;
                case 0x176: lo = s->sysenter_eip; break;
                default: break; /* unknown MSRs read as 0 */
                }
                s->regs[REG_EAX] = lo; s->regs[REG_EDX] = hi;
            } else if (b2 == 0x30) { /* wrmsr */
                uint32_t ecx = s->regs[REG_ECX], lo = s->regs[REG_EAX];
                switch (ecx) {
                case 0x174: s->sysenter_cs  = lo; break;
                case 0x175: s->sysenter_esp = lo; break;
                case 0x176: s->sysenter_eip = lo; break;
                default: break; /* unknown MSRs ignored */
                }
            } else if (b2 == 0x34) { /* sysenter */
                /* Fast ring3->ring0 transition. Load flat ring0 CS/SS from the
                 * SYSENTER_CS MSR and jump to the kernel entry (SYSENTER_EIP)
                 * with SYSENTER_ESP. Segment descriptors are architecturally
                 * fixed flat 32-bit (not read from the GDT). */
                X86Seg *cs = &s->seg[X86_CPU_SEG_CS], *ss = &s->seg[X86_CPU_SEG_SS];
                s->eflags &= ~(EFL_IF | EFL_TF);
                cs->sel = s->sysenter_cs & 0xfffc;
                cs->base = 0; cs->limit = 0xffffffffu; cs->flags = 0xc09b;
                ss->sel = (s->sysenter_cs + 8) & 0xfffc;
                ss->base = 0; ss->limit = 0xffffffffu; ss->flags = 0xc093;
                s->regs[REG_ESP] = s->sysenter_esp;
                s->eip = s->sysenter_eip;
            } else if (b2 == 0x35) { /* sysexit */
                /* Fast ring0->ring3 return: CS/SS from SYSENTER_CS MSR (+16/+24,
                 * RPL 3), EIP=EDX, ESP=ECX. */
                X86Seg *cs = &s->seg[X86_CPU_SEG_CS], *ss = &s->seg[X86_CPU_SEG_SS];
                cs->sel = ((s->sysenter_cs + 16) & 0xfffc) | 3;
                cs->base = 0; cs->limit = 0xffffffffu; cs->flags = 0xc0fb;
                ss->sel = ((s->sysenter_cs + 24) & 0xfffc) | 3;
                ss->base = 0; ss->limit = 0xffffffffu; ss->flags = 0xc0f3;
                s->regs[REG_ESP] = s->regs[REG_ECX];
                s->eip = s->regs[REG_EDX];
            } else if (b2 >= 0x40 && b2 <= 0x4f) { /* cmovcc */
                uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
                uint32_t v = rm_read(s, &rm, op_size);
                if (test_cc(s, b2 & 0xf)) set_regv(s, reg, v, op_size);
            } else if (b2 == 0xbc || b2 == 0xbd) { /* bsf / bsr */
                uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
                uint32_t mask = (op_size == 2) ? 0xffff : 0xffffffffu;
                uint32_t v = rm_read(s, &rm, op_size) & mask;
                if (!v) { s->eflags |= EFL_ZF; }
                else {
                    int idx;
                    s->eflags &= ~EFL_ZF;
                    if (b2 == 0xbc) { idx = 0; while (!((v >> idx) & 1)) idx++; }
                    else { idx = (op_size == 2) ? 15 : 31; while (!((v >> idx) & 1)) idx--; }
                    set_regv(s, reg, idx, op_size);
                }
            } else if (b2 == 0xa3 || b2 == 0xab || b2 == 0xb3 || b2 == 0xbb || b2 == 0xba) {
                /* bt / bts / btr / btc  (reg form, and 0F BA imm form) */
                uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
                int kind, bit;
                /* The immediate form takes the offset modulo the operand size
                 * (even for a memory bit base); only the register-offset form
                 * addresses a bit string beyond the addressed word. */
                if (b2 == 0xba) { kind = reg; bit = (uint8_t)fetch8(s) & (op_size * 8 - 1); } /* /4 bt /5 bts /6 btr /7 btc */
                else { kind = (b2 == 0xa3) ? 4 : (b2 == 0xab) ? 5 : (b2 == 0xb3) ? 6 : 7;
                       bit = get_regv(s, reg, op_size); }
                int cf;
                if (rm.is_reg) {
                    int bi = bit & (op_size * 8 - 1);
                    uint32_t v = get_regv(s, rm.reg, op_size);
                    cf = (v >> bi) & 1;
                    if (kind == 5) v |= (1u << bi);
                    else if (kind == 6) v &= ~(1u << bi);
                    else if (kind == 7) v ^= (1u << bi);
                    if (kind != 4) set_regv(s, rm.reg, v, op_size);
                } else {
                    uint32_t addr = rm.addr + ((int32_t)bit >> 3);
                    int bi = bit & 7;
                    uint8_t v = ld_lin(s, addr, 1);
                    cf = (v >> bi) & 1;
                    if (kind == 5) v |= (1 << bi);
                    else if (kind == 6) v &= ~(1 << bi);
                    else if (kind == 7) v ^= (1 << bi);
                    if (kind != 4) st_lin(s, addr, v, 1);
                }
                s->eflags = (s->eflags & ~EFL_CF) | (cf ? EFL_CF : 0);
            } else if (b2 == 0xa4 || b2 == 0xa5 || b2 == 0xac || b2 == 0xad) {
                /* shld / shrd */
                int is_shld = (b2 == 0xa4 || b2 == 0xa5);
                uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
                uint32_t dst = rm_read(s, &rm, op_size), src = get_regv(s, reg, op_size);
                int cnt = (b2 == 0xa4 || b2 == 0xac) ? (uint8_t)fetch8(s) : get_reg8(s, REG_ECX);
                int bits = op_size * 8;
                cnt &= 31;
                if (cnt != 0 && cnt <= bits) {
                    uint32_t res, mask = (op_size == 2) ? 0xffff : 0xffffffffu;
                    int cf;
                    if (is_shld) {
                        cf = (dst >> (bits - cnt)) & 1;
                        res = (cnt == bits) ? src : ((dst << cnt) | (src >> (bits - cnt)));
                    } else {
                        cf = (dst >> (cnt - 1)) & 1;
                        res = (cnt == bits) ? src : ((dst >> cnt) | (src << (bits - cnt)));
                    }
                    res &= mask;
                    rm_write(s, &rm, res, op_size);
                    s->eflags = (s->eflags & ~EFL_CF) | (cf ? EFL_CF : 0);
                    set_szp(s, res, op_size);
                    if (cnt == 1) {
                        /* OF is architecturally defined only for a 1-bit shift:
                         * set when the sign of the destination changed. */
                        uint32_t sb = (op_size == 2) ? 0x8000u : 0x80000000u;
                        s->eflags = (s->eflags & ~EFL_OF) |
                                    (((res ^ dst) & sb) ? EFL_OF : 0);
                    }
                }
            } else if (b2 == 0xc0 || b2 == 0xc1) { /* xadd */
                int sz = (b2 == 0xc0) ? 1 : op_size;
                uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
                uint32_t d = rm_read(s, &rm, sz);
                uint32_t sr = (sz == 1) ? get_reg8(s, reg) : get_regv(s, reg, sz);
                uint32_t sum = alu(s, 0, d, sr, sz);
                /* Store first: the write can #PF (e.g. a COW / zero page that is
                 * readable but not yet writable), and the instruction is then
                 * restarted.  Updating the source register before the store
                 * would make the restarted xadd add the old destination to
                 * itself. */
                rm_write(s, &rm, sum, sz);
                if (sz == 1) set_reg8(s, reg, d); else set_regv(s, reg, d, sz);
            } else if (b2 == 0xb0 || b2 == 0xb1) { /* cmpxchg */
                int sz = (b2 == 0xb0) ? 1 : op_size;
                uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
                uint32_t mask = (sz == 1) ? 0xff : (sz == 2) ? 0xffff : 0xffffffffu;
                uint32_t d = rm_read(s, &rm, sz), acc = get_regv(s, REG_EAX, sz) & mask;
                (void)alu(s, 7, acc, d, sz);
                if (acc == (d & mask)) {
                    uint32_t sr = (sz == 1) ? get_reg8(s, reg) : get_regv(s, reg, sz);
                    rm_write(s, &rm, sr, sz);
                } else {
                    set_regv(s, REG_EAX, d, sz);
                }
            } else if (b2 == 0xc7) { /* cmpxchg8b m64 */
                uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
                uint32_t lo = ld_lin(s, rm.addr, 4), hi = ld_lin(s, rm.addr + 4, 4);
                if (lo == s->regs[REG_EAX] && hi == s->regs[REG_EDX]) {
                    /* Two separate stores: probe both dwords writable first, or a
                     * #PF on the high one restarts the instruction against an
                     * already-updated low dword and takes the wrong branch. */
                    probe_write(s, rm.addr, 8);
                    st_lin(s, rm.addr, s->regs[REG_EBX], 4);
                    st_lin(s, rm.addr + 4, s->regs[REG_ECX], 4);
                    s->eflags |= EFL_ZF;
                } else {
                    s->regs[REG_EAX] = lo; s->regs[REG_EDX] = hi;
                    s->eflags &= ~EFL_ZF;
                }
            } else if (b2 >= 0xc8 && b2 <= 0xcf) { /* bswap r32 */
                int r = b2 & 7;
                if (op_size == 2) {
                    /* BSWAP r16 is architecturally undefined; hardware zeroes it. */
                    set_regv(s, r, 0, 2);
                } else {
                    uint32_t v = s->regs[r];
                    s->regs[r] = (v >> 24) | ((v >> 8) & 0xff00u) |
                                 ((v << 8) & 0xff0000u) | (v << 24);
                }
            } else if (b2 == 0x0b) { /* ud2 */
                raise_exception(s, EXC_UD);
            } else if (b2 == 0x08 || b2 == 0x09) { /* invd / wbinvd: nop */
            } else if (b2 == 0x21) { /* mov r32, drN: report 0 */
                uint8_t m = fetch8(s); s->regs[m & 7] = 0;
            } else if (b2 == 0x23) { /* mov drN, r32: ignore */
                (void)fetch8(s);
            } else if (b2 == 0xa0) { push_val(s, s->seg[X86_CPU_SEG_FS].sel, op_size);
            } else if (b2 == 0xa1) { pop_seg(s, X86_CPU_SEG_FS, op_size);
            } else if (b2 == 0xa8) { push_val(s, s->seg[X86_CPU_SEG_GS].sel, op_size);
            } else if (b2 == 0xa9) { pop_seg(s, X86_CPU_SEG_GS, op_size);
            } else if (b2 == 0x0d || (b2 >= 0x18 && b2 <= 0x1f)) { /* hint nops */
                uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
            } else {
                x86_stop(s, "unimplemented opcode 0F %02x", b2);
            }
        } break;

        case 0x84: { /* test r/m8, r8 */
            uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
            (void)alu(s, 4, rm_read(s, &rm, 1), get_reg8(s, reg), 1);
        } break;
        case 0x85: { /* test r/m, r */
            uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
            (void)alu(s, 4, rm_read(s, &rm, op_size), get_regv(s, reg, op_size), op_size);
        } break;
        case 0xa8: /* test al, imm8 */
            (void)alu(s, 4, get_reg8(s, REG_EAX), (uint8_t)fetch8(s), 1);
            break;
        case 0xa9: /* test eAX, imm */
            (void)alu(s, 4, get_regv(s, REG_EAX, op_size), fetch_imm(s, op_size), op_size);
            break;

        case 0x86: { /* xchg r/m8, r8 */
            uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
            uint32_t a = rm_read(s, &rm, 1), b2 = get_reg8(s, reg);
            rm_write(s, &rm, b2, 1); set_reg8(s, reg, a);
        } break;
        case 0x87: { /* xchg r/m, r */
            uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
            uint32_t a = rm_read(s, &rm, op_size), b2 = get_regv(s, reg, op_size);
            rm_write(s, &rm, b2, op_size); set_regv(s, reg, a, op_size);
        } break;
        case 0x91: case 0x92: case 0x93:
        case 0x94: case 0x95: case 0x96: case 0x97: { /* xchg eAX, reg */
            int r = b & 7;
            uint32_t a = get_regv(s, REG_EAX, op_size), c = get_regv(s, r, op_size);
            set_regv(s, REG_EAX, c, op_size); set_regv(s, r, a, op_size);
        } break;

        case 0x8c: { /* mov r/m, Sreg */
            uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
            /* reg 6/7 would index seg[6]/seg[7] = LDT/TR, which are not segment
             * registers: the encoding is #UD, not a silent alias. */
            if (reg > 5) raise_fault(s, EXC_UD);
            rm_write(s, &rm, s->seg[reg].sel, 2);
        } break;
        case 0x8e: { /* mov Sreg, r/m16 */
            uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
            /* CS cannot be loaded this way (#UD); 6/7 are not segment registers. */
            if (reg == X86_CPU_SEG_CS || reg > 5) raise_fault(s, EXC_UD);
            load_seg(s, reg, (uint16_t)rm_read(s, &rm, 2));
            if (reg == X86_CPU_SEG_SS) s->int_shadow = 1;
        } break;

        case 0x98: /* cbw / cwde */
            if (op_size == 2)
                set_regv(s, REG_EAX, (uint16_t)(int16_t)(int8_t)get_reg8(s, REG_EAX), 2);
            else
                s->regs[REG_EAX] = (uint32_t)(int32_t)(int16_t)(s->regs[REG_EAX] & 0xffff);
            break;
        case 0x99: /* cwd / cdq */
            if (op_size == 2)
                set_regv(s, REG_EDX, (s->regs[REG_EAX] & 0x8000) ? 0xffff : 0, 2);
            else
                s->regs[REG_EDX] = (s->regs[REG_EAX] & 0x80000000u) ? 0xffffffffu : 0;
            break;

        case 0x9c: /* pushf */
            push_val(s, s->eflags, op_size);
            break;
        case 0x9d: { /* popf */
            /* IOPL is writable only at CPL 0, and IF only at CPL <= IOPL; a
             * 16-bit popf leaves the high half of EFLAGS alone. */
            uint32_t v = pop_val(s, op_size), mask = 0x00257fd5u;
            int cpl = s->seg[X86_CPU_SEG_CS].sel & 3;
            if (cpl > 0) mask &= ~0x3000u;
            if (cpl > (int)((s->eflags >> 12) & 3)) mask &= ~(uint32_t)EFL_IF;
            if (op_size == 2) mask &= 0xffffu;
            s->eflags = (s->eflags & ~mask) | (v & mask) | 0x2;
        } break;
        case 0x9e: /* sahf: low byte of EFLAGS <- AH (SF ZF AF PF CF) */
            s->eflags = (s->eflags & ~0xffu) | (get_reg8(s, 4) & 0xd5u) | 0x2u;
            break;
        case 0x9f: /* lahf: AH <- low byte of EFLAGS */
            set_reg8(s, 4, (s->eflags & 0xd5u) | 0x02u);
            break;

        case 0xc0: case 0xc1: { /* group2 r/m, imm8 */
            int sz = (b == 0xc0) ? 1 : op_size;
            uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
            uint32_t v = rm_read(s, &rm, sz); int cnt = (uint8_t)fetch8(s);
            rm_write(s, &rm, do_shift(s, reg, v, cnt, sz), sz);
        } break;
        case 0xd0: case 0xd1: { /* group2 r/m, 1 */
            int sz = (b == 0xd0) ? 1 : op_size;
            uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
            rm_write(s, &rm, do_shift(s, reg, rm_read(s, &rm, sz), 1, sz), sz);
        } break;
        case 0xd2: case 0xd3: { /* group2 r/m, CL */
            int sz = (b == 0xd2) ? 1 : op_size;
            uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
            int cnt = get_reg8(s, REG_ECX);
            rm_write(s, &rm, do_shift(s, reg, rm_read(s, &rm, sz), cnt, sz), sz);
        } break;

        case 0xf6: case 0xf7: { /* group3 */
            int sz = (b == 0xf6) ? 1 : op_size;
            uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
            uint32_t a = rm_read(s, &rm, sz);
            switch (reg) {
            case 0: case 1: /* test r/m, imm */
                (void)alu(s, 4, a, fetch_imm(s, sz), sz);
                break;
            case 2: /* not */
                rm_write(s, &rm, ~a, sz);
                break;
            case 3: /* neg */
                rm_write(s, &rm, alu(s, 5, 0, a, sz), sz);
                break;
            case 4: { /* mul (unsigned) */
                if (sz == 1) {
                    uint32_t r = (get_reg8(s, REG_EAX) & 0xff) * (a & 0xff);
                    set_regv(s, REG_EAX, r & 0xffff, 2);
                    s->eflags &= ~(EFL_CF | EFL_OF);
                    if (r & 0xff00) s->eflags |= EFL_CF | EFL_OF;
                } else if (sz == 2) {
                    uint32_t r = (s->regs[REG_EAX] & 0xffff) * (a & 0xffff);
                    set_regv(s, REG_EAX, r & 0xffff, 2);
                    set_regv(s, REG_EDX, (r >> 16) & 0xffff, 2);
                    s->eflags &= ~(EFL_CF | EFL_OF);
                    if (r >> 16) s->eflags |= EFL_CF | EFL_OF;
                } else {
                    uint64_t r = (uint64_t)s->regs[REG_EAX] * a;
                    s->regs[REG_EAX] = (uint32_t)r;
                    s->regs[REG_EDX] = (uint32_t)(r >> 32);
                    s->eflags &= ~(EFL_CF | EFL_OF);
                    if (r >> 32) s->eflags |= EFL_CF | EFL_OF;
                }
            } break;
            case 5: { /* imul (signed, one-operand) */
                if (sz == 1) {
                    int32_t r = (int8_t)get_reg8(s, REG_EAX) * (int8_t)a;
                    set_regv(s, REG_EAX, (uint16_t)r, 2);
                    s->eflags &= ~(EFL_CF | EFL_OF);
                    if (r != (int8_t)r) s->eflags |= EFL_CF | EFL_OF;
                } else if (sz == 2) {
                    int32_t r = (int16_t)s->regs[REG_EAX] * (int16_t)a;
                    set_regv(s, REG_EAX, (uint16_t)r, 2);
                    set_regv(s, REG_EDX, (uint16_t)(r >> 16), 2);
                    s->eflags &= ~(EFL_CF | EFL_OF);
                    if (r != (int16_t)r) s->eflags |= EFL_CF | EFL_OF;
                } else {
                    int64_t r = (int64_t)(int32_t)s->regs[REG_EAX] * (int32_t)a;
                    s->regs[REG_EAX] = (uint32_t)r;
                    s->regs[REG_EDX] = (uint32_t)((uint64_t)r >> 32);
                    s->eflags &= ~(EFL_CF | EFL_OF);
                    if (r != (int32_t)r) s->eflags |= EFL_CF | EFL_OF;
                }
            } break;
            case 6: { /* div (unsigned) */
                if (a == 0) raise_exception(s, EXC_DE);
                if (sz == 1) {
                    uint32_t num = s->regs[REG_EAX] & 0xffff;
                    uint32_t q = num / (a & 0xff), r = num % (a & 0xff);
                    if (q > 0xff) raise_exception(s, EXC_DE);
                    set_reg8(s, REG_EAX, q); set_reg8(s, REG_EAX + 4, r); /* AL, AH */
                } else if (sz == 2) {
                    uint32_t num = ((s->regs[REG_EDX] & 0xffff) << 16) | (s->regs[REG_EAX] & 0xffff);
                    uint32_t q = num / (a & 0xffff), r = num % (a & 0xffff);
                    if (q > 0xffff) raise_exception(s, EXC_DE);
                    set_regv(s, REG_EAX, q, 2); set_regv(s, REG_EDX, r, 2);
                } else {
                    uint64_t num = ((uint64_t)s->regs[REG_EDX] << 32) | s->regs[REG_EAX];
                    uint64_t q = num / a, r = num % a;
                    if (q > 0xffffffffu) raise_exception(s, EXC_DE);
                    s->regs[REG_EAX] = (uint32_t)q; s->regs[REG_EDX] = (uint32_t)r;
                }
            } break;
            case 7: { /* idiv (signed) */
                if (a == 0) raise_exception(s, EXC_DE);
                if (sz == 1) {
                    int32_t num = (int16_t)s->regs[REG_EAX];
                    int32_t q = num / (int8_t)a, r = num % (int8_t)a;
                    if (q != (int8_t)q) raise_exception(s, EXC_DE);
                    set_reg8(s, REG_EAX, (uint8_t)q); set_reg8(s, REG_EAX + 4, (uint8_t)r);
                } else if (sz == 2) {
                    int32_t num = (int32_t)(((s->regs[REG_EDX] & 0xffff) << 16) | (s->regs[REG_EAX] & 0xffff));
                    int32_t q = num / (int16_t)a, r = num % (int16_t)a;
                    if (q != (int16_t)q) raise_exception(s, EXC_DE);
                    set_regv(s, REG_EAX, (uint16_t)q, 2); set_regv(s, REG_EDX, (uint16_t)r, 2);
                } else {
                    int64_t num = (int64_t)(((uint64_t)s->regs[REG_EDX] << 32) | s->regs[REG_EAX]);
                    int64_t q = num / (int32_t)a, r = num % (int32_t)a;
                    if (q != (int32_t)q) raise_exception(s, EXC_DE);
                    s->regs[REG_EAX] = (uint32_t)q; s->regs[REG_EDX] = (uint32_t)r;
                }
            } break;
            }
        } break;

        case 0xfe: { /* group4: inc/dec r/m8 */
            uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
            int cf = s->eflags & EFL_CF;
            uint32_t a = rm_read(s, &rm, 1);
            rm_write(s, &rm, alu(s, (reg == 0) ? 0 : 5, a, 1, 1), 1);
            s->eflags = (s->eflags & ~EFL_CF) | cf;
        } break;
        case 0xff: { /* group5 */
            uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
            switch (reg) {
            case 0: case 1: { /* inc/dec r/m */
                int cf = s->eflags & EFL_CF;
                uint32_t a = rm_read(s, &rm, op_size);
                rm_write(s, &rm, alu(s, (reg == 0) ? 0 : 5, a, 1, op_size), op_size);
                s->eflags = (s->eflags & ~EFL_CF) | cf;
            } break;
            case 2: { /* call near indirect */
                /* Read the target BEFORE pushing the return address: the operand
                 * read may #PF (e.g. demand-paging a jump table), and the push is
                 * a side effect that must not be applied twice on restart. */
                uint32_t target = rm_read(s, &rm, op_size);
                push_val(s, s->eip, op_size);
                s->eip = target;
            } break;
            case 4: /* jmp near indirect */
                s->eip = rm_read(s, &rm, op_size);
                break;
            case 3: case 5: { /* call/jmp far m16:off */
                uint32_t off;
                uint16_t nsel;
                if (rm.is_reg) raise_fault(s, EXC_UD);
                off  = ld_lin(s, rm.addr, op_size);
                nsel = (uint16_t)ld_lin(s, rm.addr + op_size, 2);
                probe_seg(s, nsel);   /* fault on the descriptor before any push */
                if (reg == 3) {
                    push_val(s, s->seg[X86_CPU_SEG_CS].sel, op_size);
                    push_val(s, s->eip, op_size);
                }
                load_seg(s, X86_CPU_SEG_CS, nsel);
                s->eip = off;
            } break;
            case 6: /* push r/m */
                push_val(s, rm_read(s, &rm, op_size), op_size);
                break;
            default:
                raise_fault(s, EXC_UD);
                break;
            }
        } break;

        case 0x69: { /* imul r, r/m, imm */
            uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
            int64_t src = (op_size == 2) ? (int16_t)rm_read(s, &rm, 2) : (int32_t)rm_read(s, &rm, 4);
            int64_t imm = (op_size == 2) ? (int16_t)fetch16(s) : (int32_t)fetch32(s);
            int64_t r = src * imm;
            set_regv(s, reg, (uint32_t)r, op_size);
            s->eflags &= ~(EFL_CF | EFL_OF);
            if ((op_size == 2 && r != (int16_t)r) || (op_size == 4 && r != (int32_t)r))
                s->eflags |= EFL_CF | EFL_OF;
        } break;
        case 0x6b: { /* imul r, r/m, imm8 */
            uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
            int64_t src = (op_size == 2) ? (int16_t)rm_read(s, &rm, 2) : (int32_t)rm_read(s, &rm, 4);
            int64_t imm = (int8_t)fetch8(s);
            int64_t r = src * imm;
            set_regv(s, reg, (uint32_t)r, op_size);
            s->eflags &= ~(EFL_CF | EFL_OF);
            if ((op_size == 2 && r != (int16_t)r) || (op_size == 4 && r != (int32_t)r))
                s->eflags |= EFL_CF | EFL_OF;
        } break;

        case 0xe8: { /* call rel */
            int32_t d = (op_size == 2) ? (int16_t)fetch16(s) : (int32_t)fetch32(s);
            push_val(s, s->eip, op_size);
            jmp_rel(s, d, op_size);
        } break;
        case 0xc3: /* ret near */
            s->eip = pop_val(s, op_size);
            break;
        case 0xc2: { /* ret near, imm16 */
            uint16_t n = fetch16(s);
            s->eip = pop_val(s, op_size);
            s->regs[REG_ESP] += n;
        } break;
        case 0xc8: { /* enter imm16, imm8 */
            /* Restart-safe: every value to push is gathered and the whole target
             * stack range probed before the first store, so a #PF anywhere in the
             * display copy leaves EBP/ESP untouched and the restart is clean. */
            uint16_t alloc = fetch16(s);
            int level = (uint8_t)fetch8(s) & 0x1f;
            uint32_t tmp[32], sb = s->seg[X86_CPU_SEG_SS].base;
            uint32_t ebp = s->regs[REG_EBP], esp = s->regs[REG_ESP], frame;
            int i, n = 0;
            tmp[n++] = get_regv(s, REG_EBP, op_size);
            frame = esp - (uint32_t)op_size;      /* ESP after the first push */
            for (i = 1; i < level; i++) {
                ebp -= (uint32_t)op_size;
                tmp[n++] = ld_lin(s, sb + ebp, op_size);
            }
            if (level > 0)
                tmp[n++] = frame;
            probe_write(s, sb + esp - (uint32_t)(n * op_size), n * op_size);
            for (i = 0; i < n; i++) {
                esp -= (uint32_t)op_size;
                st_lin(s, sb + esp, tmp[i], op_size);
            }
            s->regs[REG_ESP] = esp - alloc;
            set_regv(s, REG_EBP, frame, op_size);
        } break;
        case 0xc9: /* leave */
            /* Restart-safe: setting ESP from EBP is idempotent, and EBP is only
             * overwritten once the stack read has succeeded. */
            s->regs[REG_ESP] = s->regs[REG_EBP];
            s->regs[REG_EBP] = pop_val(s, op_size);
            break;

        case 0xe4: /* in al, imm8 */
            check_iopl(s);
            set_reg8(s, REG_EAX, s->port_read(s->port_opaque, (uint8_t)fetch8(s), 0));
            break;
        case 0xe5: /* in eAX, imm8 */
            check_iopl(s);
            set_regv(s, REG_EAX, s->port_read(s->port_opaque, (uint8_t)fetch8(s),
                                              op_size == 2 ? 1 : 2), op_size);
            break;
        case 0xec: /* in al, dx */
            check_iopl(s);
            set_reg8(s, REG_EAX, s->port_read(s->port_opaque, s->regs[REG_EDX] & 0xffff, 0));
            break;
        case 0xed: /* in eAX, dx */
            check_iopl(s);
            set_regv(s, REG_EAX, s->port_read(s->port_opaque, s->regs[REG_EDX] & 0xffff,
                                              op_size == 2 ? 1 : 2), op_size);
            break;
        case 0xe6: /* out imm8, al */
            check_iopl(s);
            s->port_write(s->port_opaque, (uint8_t)fetch8(s), get_reg8(s, REG_EAX), 0);
            break;
        case 0xe7: /* out imm8, eAX */
            check_iopl(s);
            s->port_write(s->port_opaque, (uint8_t)fetch8(s), get_regv(s, REG_EAX, op_size),
                          op_size == 2 ? 1 : 2);
            break;
        case 0xee: /* out dx, al */
            check_iopl(s);
            s->port_write(s->port_opaque, s->regs[REG_EDX] & 0xffff, get_reg8(s, REG_EAX), 0);
            break;
        case 0xef: /* out dx, eAX */
            check_iopl(s);
            s->port_write(s->port_opaque, s->regs[REG_EDX] & 0xffff, get_regv(s, REG_EAX, op_size),
                          op_size == 2 ? 1 : 2);
            break;

        /* ---- decimal / ASCII adjust (SDM: DAA/DAS/AAA/AAS/AAM/AAD) ---- */
        case 0x27: case 0x2f: { /* daa / das */
            int sub_form = (b == 0x2f);
            uint32_t al = get_reg8(s, REG_EAX), old_al = al;
            int old_cf = (s->eflags & EFL_CF) != 0, cf = 0;
            if ((al & 0xf) > 9 || (s->eflags & EFL_AF)) {
                al = sub_form ? (al - 6) : (al + 6);
                cf = old_cf || (sub_form ? (old_al < 6) : (al > 0xff));
                s->eflags |= EFL_AF;
            } else {
                s->eflags &= ~EFL_AF;
            }
            if (old_al > 0x99 || old_cf) {
                al = sub_form ? (al - 0x60) : (al + 0x60);
                cf = 1;
            }
            al &= 0xff;
            set_reg8(s, REG_EAX, (uint8_t)al);
            s->eflags = (s->eflags & ~(EFL_CF | EFL_OF)) | (cf ? EFL_CF : 0);
            set_szp(s, al, 1);
        } break;
        case 0x37: case 0x3f: { /* aaa / aas */
            int sub_form = (b == 0x3f);
            uint32_t ax = s->regs[REG_EAX] & 0xffff;
            int cf = 0;
            if ((ax & 0xf) > 9 || (s->eflags & EFL_AF)) {
                ax = sub_form ? (ax - 6) : (ax + 6);
                ax = (ax & 0xff) | (((ax >> 8) + (sub_form ? -1 : 1)) << 8);
                cf = 1;
                s->eflags |= EFL_AF;
            } else {
                s->eflags &= ~EFL_AF;
            }
            ax &= 0xff0f;   /* AL keeps only the low nibble */
            set_regv(s, REG_EAX, ax, 2);
            s->eflags = (s->eflags & ~(EFL_CF | EFL_OF)) | (cf ? EFL_CF : 0);
        } break;
        case 0xd4: { /* aam imm8 */
            uint32_t base = (uint8_t)fetch8(s), al = get_reg8(s, REG_EAX);
            if (base == 0) raise_fault(s, EXC_DE);
            set_regv(s, REG_EAX, ((al / base) << 8) | (al % base), 2);
            set_szp(s, al % base, 1);
        } break;
        case 0xd5: { /* aad imm8 */
            uint32_t base = (uint8_t)fetch8(s);
            uint32_t al = (get_reg8(s, REG_EAX) + base * get_reg8(s, REG_EAX + 4)) & 0xff;
            set_regv(s, REG_EAX, al, 2);
            set_szp(s, al, 1);
        } break;

        case 0x62: { /* bound r, m16&16 / m32&32 */
            int32_t idx, lo, hi;
            uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
            if (rm.is_reg) raise_fault(s, EXC_UD);
            if (op_size == 2) {
                idx = (int16_t)get_regv(s, reg, 2);
                lo  = (int16_t)ld_lin(s, rm.addr, 2);
                hi  = (int16_t)ld_lin(s, rm.addr + 2, 2);
            } else {
                idx = (int32_t)s->regs[reg];
                lo  = (int32_t)ld_lin(s, rm.addr, 4);
                hi  = (int32_t)ld_lin(s, rm.addr + 4, 4);
            }
            if (idx < lo || idx > hi)
                raise_fault(s, EXC_BR);
        } break;
        case 0x63: { /* arpl r/m16, r16 */
            uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
            uint32_t d = rm_read(s, &rm, 2), sr = get_regv(s, reg, 2);
            if ((d & 3) < (sr & 3)) {
                rm_write(s, &rm, (d & ~3u) | (sr & 3), 2);
                s->eflags |= EFL_ZF;
            } else {
                s->eflags &= ~EFL_ZF;
            }
        } break;

        case 0xf5: s->eflags ^= EFL_CF; break;          /* cmc */
        case 0xf8: s->eflags &= ~EFL_CF; break;         /* clc */
        case 0xf9: s->eflags |= EFL_CF; break;          /* stc */
        case 0xfa: check_iopl(s); s->eflags &= ~EFL_IF; break;    /* cli */
        case 0xfb: /* sti: also opens a one-instruction interrupt shadow */
            check_iopl(s);
            if (!(s->eflags & EFL_IF)) s->int_shadow = 1;
            s->eflags |= EFL_IF;
            break;
        case 0xfc: s->eflags &= ~EFL_DF; break;         /* cld */
        case 0xfd: s->eflags |= EFL_DF; break;          /* std */

        case 0xa4: string_op(s, 0, 1, seg_override, rep); break;       /* movsb */
        case 0xa5: string_op(s, 0, op_size, seg_override, rep); break; /* movs  */
        case 0xaa: string_op(s, 1, 1, seg_override, rep); break;       /* stosb */
        case 0xab: string_op(s, 1, op_size, seg_override, rep); break; /* stos  */
        case 0xac: string_op(s, 2, 1, seg_override, rep); break;       /* lodsb */
        case 0xad: string_op(s, 2, op_size, seg_override, rep); break; /* lods  */
        case 0xae: string_op(s, 3, 1, seg_override, rep); break;       /* scasb */
        case 0xaf: string_op(s, 3, op_size, seg_override, rep); break; /* scas  */
        case 0xa6: string_op(s, 4, 1, seg_override, rep); break;       /* cmpsb */
        case 0xa7: string_op(s, 4, op_size, seg_override, rep); break; /* cmps  */

        case 0xa0: { /* mov al, [moffs] */
            uint32_t a = seg_base(s, seg_override, X86_CPU_SEG_DS) +
                         fetch_imm(s, s->addr_size);
            set_reg8(s, REG_EAX, ld_lin(s, a, 1));
        } break;
        case 0xa1: { /* mov eAX, [moffs] */
            uint32_t a = seg_base(s, seg_override, X86_CPU_SEG_DS) +
                         fetch_imm(s, s->addr_size);
            set_regv(s, REG_EAX, ld_lin(s, a, op_size), op_size);
        } break;
        case 0xa2: { /* mov [moffs], al */
            uint32_t a = seg_base(s, seg_override, X86_CPU_SEG_DS) +
                         fetch_imm(s, s->addr_size);
            st_lin(s, a, get_reg8(s, REG_EAX), 1);
        } break;
        case 0xa3: { /* mov [moffs], eAX */
            uint32_t a = seg_base(s, seg_override, X86_CPU_SEG_DS) +
                         fetch_imm(s, s->addr_size);
            st_lin(s, a, get_regv(s, REG_EAX, op_size), op_size);
        } break;

        case 0x60: { /* pusha(d) */
            /* Restartable: write all 8 slots relative to a temp SP, commit ESP
             * only after every write succeeds, so a mid-sequence #PF (e.g. the
             * stack growing into a demand-paged page) does not double-push. */
            uint32_t osp = s->regs[REG_ESP], base = s->seg[X86_CPU_SEG_SS].base;
            int z = op_size;
            st_lin(s, base + osp - 1*z, s->regs[REG_EAX], z);
            st_lin(s, base + osp - 2*z, s->regs[REG_ECX], z);
            st_lin(s, base + osp - 3*z, s->regs[REG_EDX], z);
            st_lin(s, base + osp - 4*z, s->regs[REG_EBX], z);
            st_lin(s, base + osp - 5*z, osp,               z);  /* original ESP */
            st_lin(s, base + osp - 6*z, s->regs[REG_EBP], z);
            st_lin(s, base + osp - 7*z, s->regs[REG_ESI], z);
            st_lin(s, base + osp - 8*z, s->regs[REG_EDI], z);
            s->regs[REG_ESP] = osp - 8*z;
        } break;
        case 0x61: { /* popa(d) */
            /* Restartable: read all 8 slots first, then commit registers+ESP. */
            uint32_t osp = s->regs[REG_ESP], base = s->seg[X86_CPU_SEG_SS].base;
            int z = op_size;
            uint32_t edi = ld_lin(s, base + osp + 0*z, z);
            uint32_t esi = ld_lin(s, base + osp + 1*z, z);
            uint32_t ebp = ld_lin(s, base + osp + 2*z, z);
            /* slot 3 (saved ESP) is discarded */
            uint32_t ebx = ld_lin(s, base + osp + 4*z, z);
            uint32_t edx = ld_lin(s, base + osp + 5*z, z);
            uint32_t ecx = ld_lin(s, base + osp + 6*z, z);
            uint32_t eax = ld_lin(s, base + osp + 7*z, z);
            set_regv(s, REG_EDI, edi, z); set_regv(s, REG_ESI, esi, z);
            set_regv(s, REG_EBP, ebp, z); set_regv(s, REG_EBX, ebx, z);
            set_regv(s, REG_EDX, edx, z); set_regv(s, REG_ECX, ecx, z);
            set_regv(s, REG_EAX, eax, z);
            s->regs[REG_ESP] = osp + 8*z;
        } break;

        case 0xea: { /* jmp far ptr16:32 */
            uint32_t off = (op_size == 2) ? fetch16(s) : fetch32(s);
            uint16_t sel = fetch16(s);
            load_seg(s, X86_CPU_SEG_CS, sel); s->eip = off;
        } break;
        case 0x9a: { /* call far ptr16:32 */
            uint32_t off = (op_size == 2) ? fetch16(s) : fetch32(s);
            uint16_t sel = fetch16(s);
            probe_seg(s, sel); /* fault on the descriptor before pushing */
            push_val(s, s->seg[X86_CPU_SEG_CS].sel, op_size);
            push_val(s, s->eip, op_size);
            load_seg(s, X86_CPU_SEG_CS, sel); s->eip = off;
        } break;
        case 0xcb: case 0xca: { /* retf [imm16] */
            /* Read both slots and probe the descriptor before touching ESP, so
             * a #PF anywhere restarts the instruction with the frame intact. */
            uint16_t n = (b == 0xca) ? fetch16(s) : 0;
            uint32_t sb = s->seg[X86_CPU_SEG_SS].base, osp = s->regs[REG_ESP];
            uint32_t neip = ld_lin(s, sb + osp, op_size);
            uint16_t ncs = (uint16_t)ld_lin(s, sb + osp + op_size, op_size);
            probe_seg(s, ncs);
            s->regs[REG_ESP] = osp + 2 * op_size + n;
            load_seg(s, X86_CPU_SEG_CS, ncs); s->eip = neip;
        } break;

        case 0xd8: case 0xd9: case 0xda: case 0xdb:
        case 0xdc: case 0xdd: case 0xde: case 0xdf: /* x87 ESC */
            fpu_exec(s, b, seg_override);
            break;
        case 0x9b: /* fwait */
            if ((s->cr0 & (CR0_MP | CR0_TS)) == (CR0_MP | CR0_TS))
                raise_exception(s, 7);
            break;

        case 0xcc: /* int3 */
            deliver_interrupt(s, 3, 0, 0, 1);
            break;
        case 0xcd: { /* int n */
            int n = (uint8_t)fetch8(s);
#ifdef X86_SYSLOG
            if (n == 0x80 && (s->seg[X86_CPU_SEG_CS].sel & 3) == 3) {
                extern int g_sys_pending; extern uint32_t g_sys_nr;
                fprintf(stderr, "[sys] nr=%u ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x eip=%08x\n",
                        s->regs[REG_EAX], s->regs[REG_EBX], s->regs[REG_ECX],
                        s->regs[REG_EDX], s->regs[REG_ESI], s->regs[REG_EDI], s->cur_eip);
                g_sys_pending = 1; g_sys_nr = s->regs[REG_EAX];
            }
#endif
            deliver_interrupt(s, n, 0, 0, 1);
        } break;
        case 0xce: /* into */
            if (s->eflags & EFL_OF) deliver_interrupt(s, 4, 0, 0, 1);
            break;
        case 0xcf: /* iret */
            do_iret(s);
            break;

        case 0x06: push_val(s, s->seg[X86_CPU_SEG_ES].sel, op_size); break; /* push es */
        case 0x0e: push_val(s, s->seg[X86_CPU_SEG_CS].sel, op_size); break; /* push cs */
        case 0x16: push_val(s, s->seg[X86_CPU_SEG_SS].sel, op_size); break; /* push ss */
        case 0x1e: push_val(s, s->seg[X86_CPU_SEG_DS].sel, op_size); break; /* push ds */
        case 0x07: pop_seg(s, X86_CPU_SEG_ES, op_size); break; /* pop es */
        case 0x17: pop_seg(s, X86_CPU_SEG_SS, op_size); break; /* pop ss */
        case 0x1f: pop_seg(s, X86_CPU_SEG_DS, op_size); break; /* pop ds */
        case 0x8f: { /* pop r/m.  Per Intel SDM, when ESP is the base register of
                        the destination the effective address is computed AFTER
                        ESP is incremented, so pop the value first (e.g. the
                        `pushf; pop [esp]` local_irq_save idiom depends on this). */
            uint32_t osp = s->regs[REG_ESP];
            uint8_t m = fetch8(s); decode_modrm(s, m, seg_override, &reg, &rm);
            uint32_t v;
            /* Per Intel, an ESP-based destination address is formed from the
             * already-incremented ESP; add the delta rather than mutating ESP,
             * so that a #PF on either access restarts the instruction with the
             * value still on the stack (the restarted pop must not skip a
             * slot). ESP is committed only once both accesses have succeeded. */
            if (!rm.is_reg && rm.esp_base) rm.addr += op_size;
            v = ld_lin(s, s->seg[X86_CPU_SEG_SS].base + osp, op_size);
            if (rm.is_reg) {
                s->regs[REG_ESP] = osp + op_size;
                set_regv(s, rm.reg, v, op_size); /* `pop esp` loads from the stack */
            } else {
                st_lin(s, rm.addr, v, op_size);
                s->regs[REG_ESP] = osp + op_size;
            }
        } break;

        case 0xe0: case 0xe1: case 0xe2: { /* loopne/loope/loop */
            int32_t d = (int8_t)fetch8(s);
            int take;
            adr_add(s, REG_ECX, -1);          /* CX with a 0x67 prefix */
            take = (adr_get(s, REG_ECX) != 0);
            if (b == 0xe1) take = take && (s->eflags & EFL_ZF);   /* loope  */
            if (b == 0xe0) take = take && !(s->eflags & EFL_ZF);  /* loopne */
            if (take) jmp_rel(s, d, op_size);
        } break;
        case 0xe3: { /* jcxz / jecxz */
            int32_t d = (int8_t)fetch8(s);
            if (adr_get(s, REG_ECX) == 0) jmp_rel(s, d, op_size);
        } break;

        case 0xf4: /* hlt */
#ifdef X86_PROGRESS
            { static uint64_t h = 0; ++h;
              if (h <= 3 || (h % 2000) == 0)
                fprintf(stderr, "[hlt] #%llu eip=%08x eflags=%08x irq=%d cr0=%08x\n",
                        (unsigned long long)h, s->cur_eip, s->eflags, s->hard_irq, s->cr0);
              if (h == 1) scan_log(s);
#ifdef X86_TRACE
              if (h == 1) x86_trace_dump(s);
#endif
            }
#endif
            /* `sti; hlt` is the idle sequence: an interrupt that became
             * deliverable during sti's one-instruction shadow is recognized as
             * soon as HLT takes effect, so take it here instead of halting --
             * otherwise the guest would idle until the next timer tick. */
            if (s->hard_irq && (s->eflags & EFL_IF) && s->get_hard_intno) {
                int intno = s->get_hard_intno(s->get_hard_intno_opaque);
                if (intno >= 0) {
                    deliver_interrupt(s, intno, 0, 0, 0);
                    break;
                }
            }
            s->halted = TRUE;
            break;

        default:
            x86_stop(s, "unimplemented opcode %02x", b);
            break;
        }
        s->cycle_count++;
#ifdef X86_PROGRESS
        { static int previf = -1; int cur = (s->eflags >> 9) & 1;
          if (cur != previf) { static int n; if (n++ < 80)
              fprintf(stderr, "[IF] %d->%d @eip=%08x op=%02x\n", previf, cur, s->cur_eip, b);
              previf = cur; } }
#endif
    }
}

/* ---- public interface -------------------------------------------------- */

X86CPUState *x86_cpu_init(PhysMemoryMap *mem_map)
{
    X86CPUState *s = mallocz(sizeof(*s));
    s->mem_map = mem_map;
    s->eflags = 0x2;
    s->addr_size = 4;
    fpu_init(s);
    /* Sane flat defaults; the machine overrides via set_reg/set_seg. */
    return s;
}

void x86_cpu_end(X86CPUState *s)
{
    free(s);
}

void x86_cpu_set_irq(X86CPUState *s, BOOL set)
{
    s->hard_irq = set;
    if (set) s->halted = FALSE; /* wake from HLT (delivery is a later increment) */
}

void x86_cpu_set_reg(X86CPUState *s, int reg, uint32_t val)
{
    if (reg < 8) s->regs[reg] = val;
    else if (reg == X86_CPU_REG_EIP) s->eip = val;
    else if (reg == X86_CPU_REG_CR0) s->cr0 = val;
    else if (reg == X86_CPU_REG_CR2) s->cr2 = val;
}

uint32_t x86_cpu_get_reg(X86CPUState *s, int reg)
{
    if (reg < 8) return s->regs[reg];
    if (reg == X86_CPU_REG_EIP) return s->eip;
    if (reg == X86_CPU_REG_CR0) return s->cr0;
    if (reg == X86_CPU_REG_CR2) return s->cr2;
    return 0;
}

void x86_cpu_set_seg(X86CPUState *s, int seg, const X86CPUSeg *sd)
{
    if (seg < 0 || seg >= 10) return;
    s->seg[seg].sel   = sd->sel;
    s->seg[seg].flags = sd->flags;
    s->seg[seg].base  = sd->base;
    s->seg[seg].limit = sd->limit;
}

void x86_cpu_set_get_hard_intno(X86CPUState *s,
                                int (*get_hard_intno)(void *opaque),
                                void *opaque)
{
    s->get_hard_intno = get_hard_intno;
    s->get_hard_intno_opaque = opaque;
}

void x86_cpu_set_get_tsc(X86CPUState *s,
                         uint64_t (*get_tsc)(void *opaque),
                         void *opaque)
{
    s->get_tsc = get_tsc;
    s->get_tsc_opaque = opaque;
}

void x86_cpu_set_port_io(X86CPUState *s,
                         DeviceReadFunc *port_read, DeviceWriteFunc *port_write,
                         void *opaque)
{
    s->port_read = port_read;
    s->port_write = port_write;
    s->port_opaque = opaque;
}

int64_t x86_cpu_get_cycles(X86CPUState *s)
{
    return s->cycle_count;
}

BOOL x86_cpu_get_power_down(X86CPUState *s)
{
    return s->halted;
}

void x86_cpu_flush_tlb_write_range_ram(X86CPUState *s,
                                       uint8_t *ram_ptr, size_t ram_size)
{
    /* No software TLB yet (paging unimplemented); nothing to flush. */
    (void)s; (void)ram_ptr; (void)ram_size;
}
