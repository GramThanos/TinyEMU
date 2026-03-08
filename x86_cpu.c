/*
 * x86 CPU emulator - 32-bit protected mode software interpreter
 *
 * Copyright (c) 2011-2017 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#include <setjmp.h>

#include "cutils.h"
#include "iomem.h"
#include "x86_cpu.h"

/* EFLAGS bits */
#define EF_CF    (1U << 0)
#define EF_FIXED (1U << 1)  /* always 1 */
#define EF_PF    (1U << 2)
#define EF_AF    (1U << 4)
#define EF_ZF    (1U << 6)
#define EF_SF    (1U << 7)
#define EF_TF    (1U << 8)
#define EF_IF    (1U << 9)
#define EF_DF    (1U << 10)
#define EF_OF    (1U << 11)
#define EF_NT    (1U << 14)
#define EF_RF    (1U << 16)
#define EF_VM    (1U << 17)
#define EF_AC    (1U << 18)
#define EF_VIF   (1U << 19)
#define EF_VIP   (1U << 20)
#define EF_ID    (1U << 21)

/* CR0 bits */
#define CR0_PE  (1U << 0)   /* protected mode */
#define CR0_WP  (1U << 16)  /* write protect */
#define CR0_PG  (1U << 31)  /* paging */

/* CR4 bits */
#define CR4_PAE (1U << 5)   /* physical address extension */

/* Exception vectors */
#define EXCP_DE  0   /* divide error */
#define EXCP_DB  1   /* debug */
#define EXCP_NMI 2   /* NMI */
#define EXCP_BP  3   /* breakpoint */
#define EXCP_OF  4   /* overflow */
#define EXCP_BR  5   /* BOUND range exceeded */
#define EXCP_UD  6   /* invalid opcode */
#define EXCP_NM  7   /* device not available */
#define EXCP_DF  8   /* double fault */
#define EXCP_TS  10  /* invalid TSS */
#define EXCP_NP  11  /* segment not present */
#define EXCP_SS  12  /* stack fault */
#define EXCP_GP  13  /* general protection */
#define EXCP_PF  14  /* page fault */
#define EXCP_MF  16  /* math fault */
#define EXCP_AC  17  /* alignment check */

/* TLB */
#define TLB_SIZE  256
#define TLB_MASK  (TLB_SIZE - 1)
#define TLB_TAG_INVALID  (~0U)
#define PAGE_SHIFT 12
#define PAGE_SIZE  (1 << PAGE_SHIFT)
#define PAGE_MASK  (PAGE_SIZE - 1)

typedef struct {
    uint32_t vaddr;   /* page-aligned virtual addr tag, TLB_TAG_INVALID = invalid */
    uint8_t *ptr;     /* host pointer to start of page, NULL = device I/O */
    uint32_t paddr;   /* physical page base address */
} TLBEntry;

struct X86CPUState {
    /* General purpose registers: 0=EAX,1=ECX,2=EDX,3=EBX,4=ESP,5=EBP,6=ESI,7=EDI */
    uint32_t regs[8];
    uint32_t eip;
    uint32_t eflags;

    /* Segment registers: ES,CS,SS,DS,FS,GS,LDT,TR,GDT,IDT */
    X86CPUSeg segs[10];

    /* Control registers */
    uint32_t cr0, cr2, cr3, cr4;
    uint32_t dr6, dr7;

    /* MSRs */
    uint32_t sysenter_cs;
    uint32_t sysenter_esp;
    uint32_t sysenter_eip;
    uint64_t msr_efer;
    uint64_t msr_star;
    uint64_t msr_lstar;

    /* Interrupt/exception state */
    int irq_level;            /* current IRQ level */
    BOOL power_down;          /* CPU halted */
    int exception_num;        /* pending exception, -1 = none */
    uint32_t exception_error_code;
    BOOL exception_has_error_code;
    jmp_buf jmp_env;          /* for exception delivery */

    /* Callbacks */
    int   (*get_hard_intno)(void *opaque);
    void   *get_hard_intno_opaque;
    uint64_t (*get_tsc)(void *opaque);
    void   *get_tsc_opaque;
    DeviceReadFunc  *port_read;
    DeviceWriteFunc *port_write;
    void   *port_opaque;

    /* Memory */
    PhysMemoryMap *mem_map;

    /* TLB (separate read and write to detect write-faults) */
    TLBEntry tlb_read[TLB_SIZE];
    TLBEntry tlb_write[TLB_SIZE];

    /* Cycle counter */
    int64_t cycle_count;
};

/* ------------------------------------------------------------------
 * Physical memory access
 * ------------------------------------------------------------------ */

static uint8_t phys_read8(X86CPUState *s, uint32_t addr)
{
    uint8_t *ptr = phys_mem_get_ram_ptr(s->mem_map, addr, FALSE);
    if (ptr) return *ptr;
    PhysMemoryRange *pr = get_phys_mem_range(s->mem_map, addr);
    if (pr && !pr->is_ram) return (uint8_t)pr->read_func(pr->opaque, addr - (uint32_t)pr->addr, 0);
    return 0xFF;
}

static uint16_t phys_read16(X86CPUState *s, uint32_t addr)
{
    uint8_t *ptr = phys_mem_get_ram_ptr(s->mem_map, addr, FALSE);
    if (ptr) return get_le16(ptr);
    return (uint16_t)phys_read8(s, addr) | ((uint16_t)phys_read8(s, addr + 1) << 8);
}

static uint32_t phys_read32(X86CPUState *s, uint32_t addr)
{
    uint8_t *ptr = phys_mem_get_ram_ptr(s->mem_map, addr, FALSE);
    if (ptr) return get_le32(ptr);
    return (uint32_t)phys_read8(s, addr) |
           ((uint32_t)phys_read8(s, addr + 1) << 8) |
           ((uint32_t)phys_read8(s, addr + 2) << 16) |
           ((uint32_t)phys_read8(s, addr + 3) << 24);
}

static void phys_write8(X86CPUState *s, uint32_t addr, uint8_t val)
{
    uint8_t *ptr = phys_mem_get_ram_ptr(s->mem_map, addr, TRUE);
    if (ptr) { *ptr = val; return; }
    PhysMemoryRange *pr = get_phys_mem_range(s->mem_map, addr);
    if (pr && !pr->is_ram) pr->write_func(pr->opaque, addr - (uint32_t)pr->addr, val, 0);
}

static void phys_write16(X86CPUState *s, uint32_t addr, uint16_t val)
{
    uint8_t *ptr = phys_mem_get_ram_ptr(s->mem_map, addr, TRUE);
    if (ptr) { put_le16(ptr, val); return; }
    phys_write8(s, addr, (uint8_t)val);
    phys_write8(s, addr + 1, (uint8_t)(val >> 8));
}

static void phys_write32(X86CPUState *s, uint32_t addr, uint32_t val)
{
    uint8_t *ptr = phys_mem_get_ram_ptr(s->mem_map, addr, TRUE);
    if (ptr) { put_le32(ptr, val); return; }
    phys_write8(s, addr,     (uint8_t)val);
    phys_write8(s, addr + 1, (uint8_t)(val >> 8));
    phys_write8(s, addr + 2, (uint8_t)(val >> 16));
    phys_write8(s, addr + 3, (uint8_t)(val >> 24));
}

/* ------------------------------------------------------------------
 * TLB and page table walking
 * ------------------------------------------------------------------ */

static void tlb_flush_all(X86CPUState *s)
{
    int i;
    for (i = 0; i < TLB_SIZE; i++) {
        s->tlb_read[i].vaddr  = TLB_TAG_INVALID;
        s->tlb_write[i].vaddr = TLB_TAG_INVALID;
    }
}

/* Flush TLB entries that map to host RAM range [ram_ptr, ram_ptr+ram_size) */
static void tlb_flush_for_ram(X86CPUState *s, uint8_t *ram_ptr, size_t ram_size)
{
    int i;
    for (i = 0; i < TLB_SIZE; i++) {
        if (s->tlb_read[i].vaddr != TLB_TAG_INVALID &&
            s->tlb_read[i].ptr >= ram_ptr &&
            s->tlb_read[i].ptr < ram_ptr + ram_size) {
            s->tlb_read[i].vaddr  = TLB_TAG_INVALID;
            s->tlb_write[i].vaddr = TLB_TAG_INVALID;
        }
    }
}

/* Translate virtual address to physical; longjmp on #PF */
static uint32_t virt_to_phys(X86CPUState *s, uint32_t vaddr, BOOL is_write)
{
    uint32_t pde_addr, pte_addr, pde, pte, paddr;
    int tlb_idx;
    TLBEntry *tlb;

    if (!(s->cr0 & CR0_PG))
        return vaddr;

    tlb_idx = (vaddr >> PAGE_SHIFT) & TLB_MASK;
    tlb = is_write ? &s->tlb_write[tlb_idx] : &s->tlb_read[tlb_idx];
    if (tlb->vaddr == (vaddr & ~(uint32_t)PAGE_MASK))
        return tlb->paddr | (vaddr & PAGE_MASK);

    /* Walk 2-level page table (non-PAE 32-bit) */
    pde_addr = (s->cr3 & ~0xFFFU) + (((vaddr >> 22) & 0x3FF) << 2);
    pde = phys_read32(s, pde_addr);
    if (!(pde & 1)) { /* not present */
        s->cr2 = vaddr;
        s->exception_num = EXCP_PF;
        s->exception_error_code = (is_write ? 2 : 0);
        s->exception_has_error_code = TRUE;
        longjmp(s->jmp_env, 1);
    }

    if (pde & (1 << 7)) {
        /* 4 MB large page */
        paddr = (pde & 0xFFC00000U) | (vaddr & 0x3FFFFF);
    } else {
        pte_addr = (pde & ~0xFFFU) + (((vaddr >> 12) & 0x3FF) << 2);
        pte = phys_read32(s, pte_addr);
        if (!(pte & 1)) {
            s->cr2 = vaddr;
            s->exception_num = EXCP_PF;
            s->exception_error_code = (is_write ? 2 : 0);
            s->exception_has_error_code = TRUE;
            longjmp(s->jmp_env, 1);
        }
        if (is_write && !(pte & 2) && (s->cr0 & CR0_WP)) {
            s->cr2 = vaddr;
            s->exception_num = EXCP_PF;
            s->exception_error_code = 3; /* present + write */
            s->exception_has_error_code = TRUE;
            longjmp(s->jmp_env, 1);
        }
        if (!(pde & (1 << 5)))
            phys_write32(s, pde_addr, pde | (1 << 5));
        if (is_write && !(pte & (1 << 6)))
            phys_write32(s, pte_addr, pte | (1 << 6) | (1 << 5));
        else if (!(pte & (1 << 5)))
            phys_write32(s, pte_addr, pte | (1 << 5));
        paddr = (pte & ~0xFFFU) | (vaddr & PAGE_MASK);
    }

    /* Fill TLB */
    {
        uint8_t *hp = phys_mem_get_ram_ptr(s->mem_map, paddr & ~(uint32_t)PAGE_MASK, is_write);
        tlb->vaddr = vaddr & ~(uint32_t)PAGE_MASK;
        tlb->ptr   = hp; /* may be NULL for device I/O pages */
        tlb->paddr = paddr & ~(uint32_t)PAGE_MASK;
    }
    return paddr;
}

/* ------------------------------------------------------------------
 * Virtual memory read/write
 * ------------------------------------------------------------------ */

#define LIN_ADDR(seg_idx, offset) (s->segs[seg_idx].base + (offset))

static uint8_t vmem_read8(X86CPUState *s, uint32_t laddr)
{
    uint32_t paddr = (s->cr0 & CR0_PG) ? virt_to_phys(s, laddr, FALSE) : laddr;
    return phys_read8(s, paddr);
}

static uint16_t vmem_read16(X86CPUState *s, uint32_t laddr)
{
    uint32_t paddr = (s->cr0 & CR0_PG) ? virt_to_phys(s, laddr, FALSE) : laddr;
    return phys_read16(s, paddr);
}

static uint32_t vmem_read32(X86CPUState *s, uint32_t laddr)
{
    uint32_t paddr = (s->cr0 & CR0_PG) ? virt_to_phys(s, laddr, FALSE) : laddr;
    return phys_read32(s, paddr);
}

static void vmem_write8(X86CPUState *s, uint32_t laddr, uint8_t val)
{
    uint32_t paddr = (s->cr0 & CR0_PG) ? virt_to_phys(s, laddr, TRUE) : laddr;
    phys_write8(s, paddr, val);
}

static void vmem_write16(X86CPUState *s, uint32_t laddr, uint16_t val)
{
    uint32_t paddr = (s->cr0 & CR0_PG) ? virt_to_phys(s, laddr, TRUE) : laddr;
    phys_write16(s, paddr, val);
}

static void vmem_write32(X86CPUState *s, uint32_t laddr, uint32_t val)
{
    uint32_t paddr = (s->cr0 & CR0_PG) ? virt_to_phys(s, laddr, TRUE) : laddr;
    phys_write32(s, paddr, val);
}
/* ------------------------------------------------------------------
 * Segment descriptor helpers
 * ------------------------------------------------------------------ */

/* Load a segment descriptor from GDT/LDT into seg */
static void load_seg_desc(X86CPUState *s, int seg_idx, uint16_t sel)
{
    uint32_t dt_base;
    uint32_t lo, hi;
    X86CPUSeg *seg = &s->segs[seg_idx];

    seg->sel = sel;
    if (sel == 0) {
        seg->base  = 0;
        seg->limit = 0;
        seg->flags = 0;
        return;
    }

    /* TI bit: 0=GDT, 1=LDT */
    if (sel & 4)
        dt_base = s->segs[X86_CPU_SEG_LDT].base;
    else
        dt_base = s->segs[X86_CPU_SEG_GDT].base;

    lo = phys_read32(s, dt_base + (sel & ~7));
    hi = phys_read32(s, dt_base + (sel & ~7) + 4);

    seg->base  = ((lo >> 16) & 0xFFFF) | ((hi & 0xFF) << 16) | ((hi >> 24) << 24);
    uint32_t lim = (lo & 0xFFFF) | ((hi & 0x000F0000));
    if (hi & (1U << 23)) lim = (lim << 12) | 0xFFF; /* G=1, 4KB granularity */
    seg->limit = lim;
    seg->flags = ((hi >> 8) & 0xFF) | (((hi >> 20) & 0xF) << 12);
}

/* ------------------------------------------------------------------
 * EFLAGS helpers
 * ------------------------------------------------------------------ */

/* Parity lookup table: parity_tab[n] = EF_PF if popcount(n) is even */
static uint32_t parity_tab[256];

static void init_parity_table(void)
{
    int i, p, v;
    for (i = 0; i < 256; i++) {
        p = 0; v = i;
        while (v) { p ^= (v & 1); v >>= 1; }
        parity_tab[i] = p ? 0 : EF_PF;
    }
}

/* Compute PF, ZF, SF for an 8-bit result */
static uint32_t compute_flags_pzsb8(uint8_t r)
{
    return parity_tab[r] |
           (r == 0 ? EF_ZF : 0) |
           ((r & 0x80) ? EF_SF : 0);
}

/* Compute PF, ZF, SF for a 32-bit result */
static uint32_t compute_flags_pzs32(uint32_t r)
{
    return parity_tab[r & 0xFF] |
           (r == 0 ? EF_ZF : 0) |
           ((r >> 31) ? EF_SF : 0);
}

/* Compute PF, ZF, SF for a 16-bit result */
static uint32_t compute_flags_pzs16(uint16_t r)
{
    return parity_tab[r & 0xFF] |
           (r == 0 ? EF_ZF : 0) |
           ((r >> 15) ? EF_SF : 0);
}

/* UPDATE_FLAGS_PZSO: clear PF,ZF,SF,OF,CF,AF then set from r */
#define FLAGS_MASK_ARITH (EF_CF|EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF)

/* ADD 32-bit: update all ARITH flags */
static void flags_add32(X86CPUState *s, uint32_t a, uint32_t b, uint32_t r)
{
    s->eflags &= ~FLAGS_MASK_ARITH;
    s->eflags |= compute_flags_pzs32(r);
    if (r < a)                             s->eflags |= EF_CF;
    if ((a ^ b ^ r ^ ((a ^ b ^ 0x10) & 0x10)) & 0x10) s->eflags |= EF_AF;
    if ((~(a ^ b) & (a ^ r)) >> 31)        s->eflags |= EF_OF;
}

static void flags_add16(X86CPUState *s, uint16_t a, uint16_t b, uint16_t r)
{
    s->eflags &= ~FLAGS_MASK_ARITH;
    s->eflags |= compute_flags_pzs16(r);
    if (r < a)                             s->eflags |= EF_CF;
    if ((uint32_t)(a ^ b ^ r ^ ((a ^ b ^ 0x10) & 0x10)) & 0x10) s->eflags |= EF_AF;
    if ((uint16_t)(~(a ^ b) & (a ^ r)) >> 15) s->eflags |= EF_OF;
}

static void flags_add8(X86CPUState *s, uint8_t a, uint8_t b, uint8_t r)
{
    s->eflags &= ~FLAGS_MASK_ARITH;
    s->eflags |= compute_flags_pzsb8(r);
    if (r < a)                             s->eflags |= EF_CF;
    if ((a ^ b ^ r ^ ((a ^ b ^ 0x10) & 0x10)) & 0x10) s->eflags |= EF_AF;
    if ((uint8_t)(~(a ^ b) & (a ^ r)) >> 7) s->eflags |= EF_OF;
}

static void flags_sub32(X86CPUState *s, uint32_t a, uint32_t b, uint32_t r)
{
    s->eflags &= ~FLAGS_MASK_ARITH;
    s->eflags |= compute_flags_pzs32(r);
    if (a < b)                             s->eflags |= EF_CF;
    if ((a ^ b ^ r ^ ((a ^ b ^ 0x10) & 0x10)) & 0x10) s->eflags |= EF_AF;
    if (((a ^ b) & (a ^ r)) >> 31)         s->eflags |= EF_OF;
}

static void flags_sub16(X86CPUState *s, uint16_t a, uint16_t b, uint16_t r)
{
    s->eflags &= ~FLAGS_MASK_ARITH;
    s->eflags |= compute_flags_pzs16(r);
    if (a < b)                             s->eflags |= EF_CF;
    if ((uint32_t)(a ^ b ^ r ^ ((a ^ b ^ 0x10) & 0x10)) & 0x10) s->eflags |= EF_AF;
    if ((uint16_t)((a ^ b) & (a ^ r)) >> 15) s->eflags |= EF_OF;
}

static void flags_sub8(X86CPUState *s, uint8_t a, uint8_t b, uint8_t r)
{
    s->eflags &= ~FLAGS_MASK_ARITH;
    s->eflags |= compute_flags_pzsb8(r);
    if (a < b)                             s->eflags |= EF_CF;
    if ((a ^ b ^ r ^ ((a ^ b ^ 0x10) & 0x10)) & 0x10) s->eflags |= EF_AF;
    if ((uint8_t)((a ^ b) & (a ^ r)) >> 7) s->eflags |= EF_OF;
}

static void flags_logic32(X86CPUState *s, uint32_t r)
{
    s->eflags &= ~FLAGS_MASK_ARITH;
    s->eflags |= compute_flags_pzs32(r);
    /* CF=0, OF=0, AF undefined (set to 0) */
}

static void flags_logic16(X86CPUState *s, uint16_t r)
{
    s->eflags &= ~FLAGS_MASK_ARITH;
    s->eflags |= compute_flags_pzs16(r);
}

static void flags_logic8(X86CPUState *s, uint8_t r)
{
    s->eflags &= ~FLAGS_MASK_ARITH;
    s->eflags |= compute_flags_pzsb8(r);
}

/* ADC 32-bit */
static void flags_adc32(X86CPUState *s, uint32_t a, uint32_t b, uint32_t cin, uint32_t r)
{
    (void)cin;
    flags_add32(s, a, b, r - cin); /* close enough for flags */
    /* recompute CF properly: r = a+b+cin, overflow if r < a+(cin!=0?1:0) */
    s->eflags &= ~EF_CF;
    if ((uint64_t)a + (uint64_t)b + cin > 0xFFFFFFFFU) s->eflags |= EF_CF;
    (void)r;
}

/* ------------------------------------------------------------------
 * Exception / interrupt delivery
 * ------------------------------------------------------------------ */

static void x86_do_interrupt(X86CPUState *s, int intno,
                              BOOL has_error_code, uint32_t error_code)
{
    uint32_t idt_base = s->segs[X86_CPU_SEG_IDT].base;
    uint32_t idt_limit = s->segs[X86_CPU_SEG_IDT].limit;
    uint32_t idt_addr, lo, hi, gate_offset;
    uint16_t gate_sel;
    uint8_t gate_type;
    uint32_t esp;

    if ((uint32_t)(intno * 8 + 7) > idt_limit) {
        fprintf(stderr, "x86: IDT too small for vector %d (limit=0x%x)\n",
                intno, idt_limit);
        exit(1);
    }

    idt_addr = idt_base + intno * 8;
    lo = phys_read32(s, idt_addr);
    hi = phys_read32(s, idt_addr + 4);

    gate_offset = (lo & 0xFFFF) | (hi & 0xFFFF0000U);
    gate_sel    = (lo >> 16) & 0xFFFF;
    gate_type   = (hi >> 8) & 0xFF;

    if (!(gate_type & 0x80)) { /* P=0: not present */
        fprintf(stderr, "x86: IDT gate %d not present\n", intno);
        exit(1);
    }

    /* Push EFLAGS, CS, EIP (and error code) using current SS:ESP */
    esp = s->regs[4]; /* ESP */
    esp -= 4; vmem_write32(s, LIN_ADDR(X86_CPU_SEG_SS, esp), s->eflags | EF_FIXED);
    esp -= 4; vmem_write32(s, LIN_ADDR(X86_CPU_SEG_SS, esp), s->segs[X86_CPU_SEG_CS].sel);
    esp -= 4; vmem_write32(s, LIN_ADDR(X86_CPU_SEG_SS, esp), s->eip);
    if (has_error_code) {
        esp -= 4; vmem_write32(s, LIN_ADDR(X86_CPU_SEG_SS, esp), error_code);
    }
    s->regs[4] = esp;

    /* Load CS from GDT using gate_sel */
    load_seg_desc(s, X86_CPU_SEG_CS, gate_sel);
    s->eip = gate_offset;

    /* Interrupt gate clears IF; trap gate preserves IF */
    if ((gate_type & 0xF) == 0xE) /* interrupt gate */
        s->eflags &= ~EF_IF;
    s->eflags &= ~(EF_TF | EF_NT | EF_RF);
}

static void __attribute__((noreturn)) raise_exception(X86CPUState *s, int intno)
{
    s->exception_num = intno;
    s->exception_has_error_code = FALSE;
    longjmp(s->jmp_env, 1);
}

static void __attribute__((noreturn)) __maybe_unused raise_exception_err(X86CPUState *s, int intno, uint32_t err)
{
    s->exception_num = intno;
    s->exception_error_code = err;
    s->exception_has_error_code = TRUE;
    longjmp(s->jmp_env, 1);
}
/* ------------------------------------------------------------------
 * Instruction decode state
 * ------------------------------------------------------------------ */

typedef struct {
    X86CPUState *cpu;
    uint32_t     pc;         /* current decode position (CS linear addr) */
    BOOL         op32;       /* 1 = 32-bit operand size */
    BOOL         addr32;     /* 1 = 32-bit address size */
    int          seg_ovr;    /* segment override index, -1 = none */
    BOOL         rep;        /* REP prefix */
    BOOL         repne;      /* REPNE prefix */
} DecodeState;

/* Fetch instruction byte and advance pc */
static uint8_t fetch_byte(DecodeState *ds)
{
    X86CPUState *s = ds->cpu;
    uint32_t laddr = ds->pc;
    uint32_t paddr = (s->cr0 & CR0_PG) ? virt_to_phys(s, laddr, FALSE) : laddr;
    ds->pc++;
    return phys_read8(s, paddr);
}

static uint16_t fetch_word(DecodeState *ds)
{
    uint8_t lo = fetch_byte(ds);
    uint8_t hi = fetch_byte(ds);
    return (uint16_t)lo | ((uint16_t)hi << 8);
}

static uint32_t fetch_dword(DecodeState *ds)
{
    uint32_t v = (uint32_t)fetch_byte(ds);
    v |= (uint32_t)fetch_byte(ds) << 8;
    v |= (uint32_t)fetch_byte(ds) << 16;
    v |= (uint32_t)fetch_byte(ds) << 24;
    return v;
}

static int32_t fetch_imm8s(DecodeState *ds)
{
    return (int32_t)(int8_t)fetch_byte(ds);
}

/* Fetch 16 or 32-bit immediate based on operand size */
static uint32_t __maybe_unused fetch_immN(DecodeState *ds)
{
    if (ds->op32) return fetch_dword(ds);
    return (uint32_t)fetch_word(ds);
}

/* ------------------------------------------------------------------
 * ModRM / SIB / effective address decoding
 * ------------------------------------------------------------------ */

/* Default data segment based on register (ESP/EBP base -> SS, else DS) */
static int default_seg_for_rm(int rm)
{
    if (rm == 4 || rm == 5) /* ESP or EBP */
        return X86_CPU_SEG_SS;
    return X86_CPU_SEG_DS;
}

/* Decode ModRM, returning:
   *reg_idx   = ModRM.reg field (0-7)
   *rm_reg    = -1 if memory operand, otherwise register index
   *ea        = effective address (only valid if rm_reg == -1)
   *ea_seg    = segment index for memory (only valid if rm_reg == -1) */
static void decode_modrm(DecodeState *ds, int *reg_idx,
                          int *rm_reg, uint32_t *ea, int *ea_seg)
{
    X86CPUState *s = ds->cpu;
    uint8_t modrm = fetch_byte(ds);
    int mod = (modrm >> 6) & 3;
    int reg = (modrm >> 3) & 7;
    int rm  = modrm & 7;
    int seg = (ds->seg_ovr >= 0) ? ds->seg_ovr : X86_CPU_SEG_DS;
    uint32_t eff_addr = 0;

    *reg_idx = reg;

    if (mod == 3) {
        *rm_reg = rm;
        *ea     = 0;
        *ea_seg = seg;
        return;
    }
    *rm_reg = -1;

    if (ds->addr32) {
        /* 32-bit addressing */
        if (rm == 4) {
            /* SIB byte */
            uint8_t sib = fetch_byte(ds);
            int scale = 1 << ((sib >> 6) & 3);
            int idx   = (sib >> 3) & 7;
            int base  = sib & 7;
            if (base == 5 && mod == 0) {
                eff_addr = fetch_dword(ds); /* disp32, no base */
            } else {
                eff_addr = s->regs[base];
                if (base == 4 || base == 5) /* ESP or EBP */
                    seg = (ds->seg_ovr >= 0) ? ds->seg_ovr : X86_CPU_SEG_SS;
            }
            if (idx != 4)
                eff_addr += s->regs[idx] * (uint32_t)scale;
        } else if (rm == 5 && mod == 0) {
            eff_addr = fetch_dword(ds);
            seg = (ds->seg_ovr >= 0) ? ds->seg_ovr : X86_CPU_SEG_DS;
        } else {
            eff_addr = s->regs[rm];
            seg = (ds->seg_ovr >= 0) ? ds->seg_ovr : default_seg_for_rm(rm);
        }
        if (mod == 1)
            eff_addr += (uint32_t)(int32_t)(int8_t)fetch_byte(ds);
        else if (mod == 2)
            eff_addr += fetch_dword(ds);
    } else {
        /* 16-bit addressing */
        int32_t disp = 0;
        switch (rm) {
        case 0: eff_addr = s->regs[3] + s->regs[6]; seg = X86_CPU_SEG_DS; break;
        case 1: eff_addr = s->regs[3] + s->regs[7]; seg = X86_CPU_SEG_DS; break;
        case 2: eff_addr = s->regs[5] + s->regs[6]; seg = X86_CPU_SEG_SS; break;
        case 3: eff_addr = s->regs[5] + s->regs[7]; seg = X86_CPU_SEG_SS; break;
        case 4: eff_addr = s->regs[6];              seg = X86_CPU_SEG_DS; break;
        case 5: eff_addr = s->regs[7];              seg = X86_CPU_SEG_DS; break;
        case 6: if (mod == 0) { eff_addr = fetch_word(ds); seg = X86_CPU_SEG_DS; break; }
                eff_addr = s->regs[5]; seg = X86_CPU_SEG_SS; break;
        case 7: eff_addr = s->regs[0]; seg = X86_CPU_SEG_DS; break;
        default: eff_addr = 0; break;
        }
        if (mod == 1)
            disp = (int32_t)(int8_t)fetch_byte(ds);
        else if (mod == 2)
            disp = (int32_t)(int16_t)fetch_word(ds);
        eff_addr = (eff_addr + (uint32_t)disp) & 0xFFFF;
        if (ds->seg_ovr >= 0) seg = ds->seg_ovr;
    }
    *ea     = eff_addr;
    *ea_seg = seg;
}

/* Compute linear address from ea + segment */
static uint32_t seg_ea(X86CPUState *s, uint32_t ea, int seg)
{
    return s->segs[seg].base + ea;
}

/* ------------------------------------------------------------------
 * 8-bit register access (AH/BH/CH/DH for 4-7)
 * ------------------------------------------------------------------ */

static uint8_t get_reg8(X86CPUState *s, int r)
{
    if (r < 4) return (uint8_t)s->regs[r];
    return (uint8_t)(s->regs[r - 4] >> 8);
}

static void set_reg8(X86CPUState *s, int r, uint8_t v)
{
    if (r < 4) s->regs[r] = (s->regs[r] & 0xFFFFFF00U) | v;
    else       s->regs[r - 4] = (s->regs[r - 4] & 0xFFFF00FFU) | ((uint32_t)v << 8);
}

static uint16_t get_reg16(X86CPUState *s, int r) { return (uint16_t)s->regs[r]; }
static void set_reg16(X86CPUState *s, int r, uint16_t v)
{
    s->regs[r] = (s->regs[r] & 0xFFFF0000U) | v;
}

/* ------------------------------------------------------------------
 * Shift/rotate helpers
 * ------------------------------------------------------------------ */

static uint32_t do_shl32(X86CPUState *s, uint32_t v, int n)
{
    if (n == 0) return v;
    n &= 31;
    uint32_t r = v << n;
    s->eflags &= ~(EF_CF | EF_OF | EF_SF | EF_ZF | EF_PF);
    s->eflags |= compute_flags_pzs32(r);
    if (n >= 1 && (v >> (32 - n)) & 1) s->eflags |= EF_CF;
    if (n == 1) {
        if (((r >> 31) & 1) ^ ((s->eflags & EF_CF) ? 1 : 0))
            s->eflags |= EF_OF;
    }
    return r;
}

static uint32_t do_shr32(X86CPUState *s, uint32_t v, int n)
{
    if (n == 0) return v;
    n &= 31;
    uint32_t r = v >> n;
    s->eflags &= ~(EF_CF | EF_OF | EF_SF | EF_ZF | EF_PF);
    s->eflags |= compute_flags_pzs32(r);
    if ((v >> (n - 1)) & 1) s->eflags |= EF_CF;
    if (n == 1 && (v >> 31)) s->eflags |= EF_OF;
    return r;
}

static uint32_t do_sar32(X86CPUState *s, uint32_t v, int n)
{
    if (n == 0) return v;
    n &= 31;
    uint32_t r = (uint32_t)((int32_t)v >> n);
    s->eflags &= ~(EF_CF | EF_OF | EF_SF | EF_ZF | EF_PF);
    s->eflags |= compute_flags_pzs32(r);
    if ((v >> (n - 1)) & 1) s->eflags |= EF_CF;
    /* OF=0 for n>=1 in SAR */
    return r;
}

static uint8_t do_shl8(X86CPUState *s, uint8_t v, int n)
{
    if (n == 0) return v;
    n &= 31;
    uint8_t r = (n >= 8) ? 0 : (v << n);
    s->eflags &= ~(EF_CF | EF_OF | EF_SF | EF_ZF | EF_PF);
    s->eflags |= compute_flags_pzsb8(r);
    if (n <= 8 && ((v >> (8 - n)) & 1)) s->eflags |= EF_CF;
    if (n == 1) {
        if (((r >> 7) & 1) ^ ((s->eflags & EF_CF) ? 1 : 0))
            s->eflags |= EF_OF;
    }
    return r;
}

static uint8_t do_shr8(X86CPUState *s, uint8_t v, int n)
{
    if (n == 0) return v;
    n &= 31;
    uint8_t r = (n >= 8) ? 0 : (v >> n);
    s->eflags &= ~(EF_CF | EF_OF | EF_SF | EF_ZF | EF_PF);
    s->eflags |= compute_flags_pzsb8(r);
    if (n <= 8 && ((v >> (n - 1)) & 1)) s->eflags |= EF_CF;
    if (n == 1 && (v >> 7)) s->eflags |= EF_OF;
    return r;
}

static uint8_t do_sar8(X86CPUState *s, uint8_t v, int n)
{
    if (n == 0) return v;
    n &= 31;
    uint8_t r = (uint8_t)((int8_t)v >> (n >= 8 ? 7 : n));
    s->eflags &= ~(EF_CF | EF_OF | EF_SF | EF_ZF | EF_PF);
    s->eflags |= compute_flags_pzsb8(r);
    if (n <= 8 && ((v >> (n - 1)) & 1)) s->eflags |= EF_CF;
    return r;
}

static uint16_t do_shl16(X86CPUState *s, uint16_t v, int n)
{
    if (n == 0) return v;
    n &= 31;
    uint16_t r = (n >= 16) ? 0 : (v << n);
    s->eflags &= ~(EF_CF | EF_OF | EF_SF | EF_ZF | EF_PF);
    s->eflags |= compute_flags_pzs16(r);
    if (n <= 16 && ((v >> (16 - n)) & 1)) s->eflags |= EF_CF;
    if (n == 1) {
        if (((r >> 15) & 1) ^ ((s->eflags & EF_CF) ? 1 : 0))
            s->eflags |= EF_OF;
    }
    return r;
}

static uint16_t do_shr16(X86CPUState *s, uint16_t v, int n)
{
    if (n == 0) return v;
    n &= 31;
    uint16_t r = (n >= 16) ? 0 : (v >> n);
    s->eflags &= ~(EF_CF | EF_OF | EF_SF | EF_ZF | EF_PF);
    s->eflags |= compute_flags_pzs16(r);
    if (n <= 16 && ((v >> (n - 1)) & 1)) s->eflags |= EF_CF;
    if (n == 1 && (v >> 15)) s->eflags |= EF_OF;
    return r;
}

static uint16_t do_sar16(X86CPUState *s, uint16_t v, int n)
{
    if (n == 0) return v;
    n &= 31;
    uint16_t r = (uint16_t)((int16_t)v >> (n >= 16 ? 15 : n));
    s->eflags &= ~(EF_CF | EF_OF | EF_SF | EF_ZF | EF_PF);
    s->eflags |= compute_flags_pzs16(r);
    if (n <= 16 && ((v >> (n - 1)) & 1)) s->eflags |= EF_CF;
    return r;
}

static uint32_t do_rol32(X86CPUState *s, uint32_t v, int n)
{
    n &= 31;
    if (!n) return v;
    uint32_t r = (v << n) | (v >> (32 - n));
    s->eflags &= ~(EF_CF | EF_OF);
    if (r & 1) s->eflags |= EF_CF;
    if (n == 1 && (((r >> 31) ^ r) & 1)) s->eflags |= EF_OF;
    return r;
}

static uint32_t do_ror32(X86CPUState *s, uint32_t v, int n)
{
    n &= 31;
    if (!n) return v;
    uint32_t r = (v >> n) | (v << (32 - n));
    s->eflags &= ~(EF_CF | EF_OF);
    if (r >> 31) s->eflags |= EF_CF;
    if (n == 1 && (((r >> 31) ^ (r >> 30)) & 1)) s->eflags |= EF_OF;
    return r;
}

static uint8_t do_rol8(X86CPUState *s, uint8_t v, int n)
{
    n &= 7;
    if (!n) return v;
    uint8_t r = (v << n) | (v >> (8 - n));
    s->eflags &= ~(EF_CF | EF_OF);
    if (r & 1) s->eflags |= EF_CF;
    if (n == 1 && (((r >> 7) ^ r) & 1)) s->eflags |= EF_OF;
    return r;
}

static uint8_t do_ror8(X86CPUState *s, uint8_t v, int n)
{
    n &= 7;
    if (!n) return v;
    uint8_t r = (v >> n) | (v << (8 - n));
    s->eflags &= ~(EF_CF | EF_OF);
    if (r >> 7) s->eflags |= EF_CF;
    if (n == 1 && (((r >> 7) ^ (r >> 6)) & 1)) s->eflags |= EF_OF;
    return r;
}

static uint16_t do_rol16(X86CPUState *s, uint16_t v, int n)
{
    n &= 15;
    if (!n) return v;
    uint16_t r = (uint16_t)((v << n) | (v >> (16 - n)));
    s->eflags &= ~(EF_CF | EF_OF);
    if (r & 1) s->eflags |= EF_CF;
    if (n == 1 && (((r >> 15) ^ r) & 1)) s->eflags |= EF_OF;
    return r;
}

static uint16_t do_ror16(X86CPUState *s, uint16_t v, int n)
{
    n &= 15;
    if (!n) return v;
    uint16_t r = (uint16_t)((v >> n) | (v << (16 - n)));
    s->eflags &= ~(EF_CF | EF_OF);
    if (r >> 15) s->eflags |= EF_CF;
    if (n == 1 && (((r >> 15) ^ (r >> 14)) & 1)) s->eflags |= EF_OF;
    return r;
}
/* ------------------------------------------------------------------
 * ALU group 1 operations (ADD/OR/ADC/SBB/AND/SUB/XOR/CMP)
 * op values: 0=ADD,1=OR,2=ADC,3=SBB,4=AND,5=SUB,6=XOR,7=CMP
 * ------------------------------------------------------------------ */

static uint32_t alu_op32(X86CPUState *s, int op, uint32_t a, uint32_t b)
{
    uint32_t r;
    int cf = (s->eflags & EF_CF) ? 1 : 0;
    switch (op & 7) {
    case 0: r = a + b;        flags_add32(s, a, b, r); break;
    case 1: r = a | b;        flags_logic32(s, r); break;
    case 2: r = a + b + cf;   flags_adc32(s, a, b, cf, r); break;
    case 3: { uint32_t tmp = b + cf; r = a - tmp;
             s->eflags &= ~FLAGS_MASK_ARITH; s->eflags |= compute_flags_pzs32(r);
             if ((uint64_t)a < (uint64_t)b + cf) s->eflags |= EF_CF;
             if (((a ^ tmp) & (a ^ r)) >> 31) s->eflags |= EF_OF; break; }
    case 4: r = a & b;        flags_logic32(s, r); break;
    case 5: r = a - b;        flags_sub32(s, a, b, r); break;
    case 6: r = a ^ b;        flags_logic32(s, r); break;
    case 7: r = a - b;        flags_sub32(s, a, b, r); break; /* CMP: don't store */
    default: r = 0; break;
    }
    return r;
}

static uint16_t alu_op16(X86CPUState *s, int op, uint16_t a, uint16_t b)
{
    uint16_t r;
    int cf = (s->eflags & EF_CF) ? 1 : 0;
    switch (op & 7) {
    case 0: r = a + b;                flags_add16(s, a, b, r); break;
    case 1: r = a | b;                flags_logic16(s, r); break;
    case 2: r = a + b + cf;           flags_add16(s, a, b, r); /* approx */ break;
    case 3: { uint16_t tmp = b + cf; r = a - tmp;
             s->eflags &= ~FLAGS_MASK_ARITH; s->eflags |= compute_flags_pzs16(r);
             if ((uint32_t)a < (uint32_t)b + cf) s->eflags |= EF_CF;
             if ((uint16_t)((a ^ tmp) & (a ^ r)) >> 15) s->eflags |= EF_OF; break; }
    case 4: r = a & b;                flags_logic16(s, r); break;
    case 5: r = a - b;                flags_sub16(s, a, b, r); break;
    case 6: r = a ^ b;                flags_logic16(s, r); break;
    case 7: r = a - b;                flags_sub16(s, a, b, r); break;
    default: r = 0; break;
    }
    return r;
}

static uint8_t alu_op8(X86CPUState *s, int op, uint8_t a, uint8_t b)
{
    uint8_t r;
    int cf = (s->eflags & EF_CF) ? 1 : 0;
    switch (op & 7) {
    case 0: r = a + b;            flags_add8(s, a, b, r); break;
    case 1: r = a | b;            flags_logic8(s, r); break;
    case 2: r = a + b + cf;       flags_add8(s, a, b, r); /* approx */ break;
    case 3: { uint8_t tmp = b + cf; r = a - tmp;
             s->eflags &= ~FLAGS_MASK_ARITH; s->eflags |= compute_flags_pzsb8(r);
             if ((uint32_t)a < (uint32_t)b + cf) s->eflags |= EF_CF;
             if ((uint8_t)((a ^ tmp) & (a ^ r)) >> 7) s->eflags |= EF_OF; break; }
    case 4: r = a & b;            flags_logic8(s, r); break;
    case 5: r = a - b;            flags_sub8(s, a, b, r); break;
    case 6: r = a ^ b;            flags_logic8(s, r); break;
    case 7: r = a - b;            flags_sub8(s, a, b, r); break;
    default: r = 0; break;
    }
    return r;
}

/* ------------------------------------------------------------------
 * Stack operations
 * ------------------------------------------------------------------ */

static void push32(X86CPUState *s, uint32_t val)
{
    s->regs[4] -= 4;
    vmem_write32(s, LIN_ADDR(X86_CPU_SEG_SS, s->regs[4]), val);
}

static uint32_t pop32(X86CPUState *s)
{
    uint32_t val = vmem_read32(s, LIN_ADDR(X86_CPU_SEG_SS, s->regs[4]));
    s->regs[4] += 4;
    return val;
}

static void push16(X86CPUState *s, uint16_t val)
{
    s->regs[4] -= 2;
    vmem_write16(s, LIN_ADDR(X86_CPU_SEG_SS, s->regs[4]), val);
}

static uint16_t pop16(X86CPUState *s)
{
    uint16_t val = vmem_read16(s, LIN_ADDR(X86_CPU_SEG_SS, s->regs[4]));
    s->regs[4] += 2;
    return val;
}

/* Conditional jump test */
static BOOL test_cc(X86CPUState *s, int cc)
{
    int zf = (s->eflags & EF_ZF) != 0;
    int cf = (s->eflags & EF_CF) != 0;
    int sf = (s->eflags & EF_SF) != 0;
    int of = (s->eflags & EF_OF) != 0;
    int pf = (s->eflags & EF_PF) != 0;
    switch (cc & 0xF) {
    case 0x0: return of;           /* O  */
    case 0x1: return !of;          /* NO */
    case 0x2: return cf;           /* B/NAE/C */
    case 0x3: return !cf;          /* NB/AE/NC */
    case 0x4: return zf;           /* E/Z */
    case 0x5: return !zf;          /* NE/NZ */
    case 0x6: return cf | zf;      /* BE/NA */
    case 0x7: return !cf & !zf;    /* NBE/A */
    case 0x8: return sf;           /* S */
    case 0x9: return !sf;          /* NS */
    case 0xA: return pf;           /* P/PE */
    case 0xB: return !pf;          /* NP/PO */
    case 0xC: return sf ^ of;      /* L/NGE */
    case 0xD: return !(sf ^ of);   /* NL/GE */
    case 0xE: return zf | (sf ^ of); /* LE/NG */
    case 0xF: return !zf & !(sf ^ of); /* NLE/G */
    default:  return FALSE;
    }
}

/* ------------------------------------------------------------------
 * I/O port helpers
 * ------------------------------------------------------------------ */

static uint32_t port_in(X86CPUState *s, uint16_t port, int size_log2)
{
    if (s->port_read)
        return s->port_read(s->port_opaque, port, size_log2);
    return (uint32_t)-1;
}

static void port_out(X86CPUState *s, uint16_t port, uint32_t val, int size_log2)
{
    if (s->port_write)
        s->port_write(s->port_opaque, port, val, size_log2);
}

/* ------------------------------------------------------------------
 * CPUID emulation
 * ------------------------------------------------------------------ */

static void do_cpuid(X86CPUState *s)
{
    uint32_t leaf = s->regs[0]; /* EAX */
    uint32_t subleaf = s->regs[1]; /* ECX */
    (void)subleaf;
    switch (leaf) {
    case 0:
        s->regs[0] = 1;      /* max leaf */
        /* "GenuineIntel" */
        s->regs[3] = 0x756e6547; /* EBX "Genu" */
        s->regs[2] = 0x6c65746e; /* ECX "ntel" */
        s->regs[1] = 0x49656e69; /* EDX "ineI" */
        break;
    case 1:
        s->regs[0] = 0x00000623; /* family 6, model 2, stepping 3 */
        s->regs[3] = 0;
        s->regs[2] = 0; /* no extensions */
        /* EDX: FPU, VME, DE, PSE, TSC, MSR, PAE, MCE, CX8, APIC, SEP, MTRR, PGE, MCA, CMOV, PAT, PSE36, CLFSH, DS, ACPI, MMX, FXSR, SSE, SSE2, SS, HTT, TM, PBE */
        s->regs[1] = (1 << 0)  /* FPU */  |
                     (1 << 4)  /* TSC */  |
                     (1 << 5)  /* MSR */  |
                     (1 << 11) /* SEP */  |
                     (1 << 13) /* PGE */  |
                     (1 << 15) /* CMOV */ |
                     (1 << 23) /* MMX */  |
                     (1 << 24) /* FXSR */ ;
        break;
    case 0x80000000:
        s->regs[0] = 0x80000004; /* max extended leaf */
        s->regs[3] = s->regs[2] = s->regs[1] = 0;
        break;
    case 0x80000001:
        s->regs[0] = s->regs[3] = s->regs[2] = s->regs[1] = 0;
        break;
    case 0x80000002: /* processor brand string 1 */
        s->regs[0] = 0x756e694c; /* "Linu" */
        s->regs[3] = 0x4d45547a; /* "zTEM" */
        s->regs[2] = 0x75783638; /* "86xu" */
        s->regs[1] = 0x50555043; /* "CPUP" */
        break;
    case 0x80000003:
        s->regs[0] = s->regs[3] = 0x20202020;
        s->regs[2] = s->regs[1] = 0x20202020;
        break;
    case 0x80000004:
        s->regs[0] = s->regs[3] = 0x20202020;
        s->regs[2] = s->regs[1] = 0x20202020;
        break;
    default:
        s->regs[0] = s->regs[3] = s->regs[2] = s->regs[1] = 0;
        break;
    }
}

/* ------------------------------------------------------------------
 * Two-byte opcode helpers (0x0F prefix)
 * ------------------------------------------------------------------ */

static void exec_0f(DecodeState *ds)
{
    X86CPUState *s = ds->cpu;
    uint8_t op2 = fetch_byte(ds);
    int reg, rm_reg;
    uint32_t ea;
    int ea_seg;

    switch (op2) {
    case 0x01: { /* GROUP 7: SGDT/SIDT/LGDT/LIDT/SMSW/LMSW/INVLPG */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint32_t laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        switch (reg) {
        case 0: { /* SGDT: store GDT to mem */
            if (rm_reg >= 0) raise_exception(s, EXCP_UD);
            vmem_write16(s, laddr, (uint16_t)s->segs[X86_CPU_SEG_GDT].limit);
            vmem_write32(s, laddr + 2, s->segs[X86_CPU_SEG_GDT].base);
            break; }
        case 1: { /* SIDT: store IDT to mem */
            if (rm_reg >= 0) raise_exception(s, EXCP_UD);
            vmem_write16(s, laddr, (uint16_t)s->segs[X86_CPU_SEG_IDT].limit);
            vmem_write32(s, laddr + 2, s->segs[X86_CPU_SEG_IDT].base);
            break; }
        case 2: { /* LGDT: load GDT from mem */
            if (rm_reg >= 0) raise_exception(s, EXCP_UD);
            s->segs[X86_CPU_SEG_GDT].limit = vmem_read16(s, laddr);
            s->segs[X86_CPU_SEG_GDT].base  = vmem_read32(s, laddr + 2);
            break; }
        case 3: { /* LIDT: load IDT from mem */
            if (rm_reg >= 0) raise_exception(s, EXCP_UD);
            s->segs[X86_CPU_SEG_IDT].limit = vmem_read16(s, laddr);
            s->segs[X86_CPU_SEG_IDT].base  = vmem_read32(s, laddr + 2);
            break; }
        case 4: { /* SMSW */
            uint16_t msw = (uint16_t)(s->cr0 & 0xFFFF);
            if (rm_reg >= 0) set_reg16(s, rm_reg, msw);
            else vmem_write16(s, laddr, msw);
            break; }
        case 6: { /* LMSW */
            uint16_t msw;
            if (rm_reg >= 0) msw = get_reg16(s, rm_reg);
            else msw = vmem_read16(s, laddr);
            s->cr0 = (s->cr0 & 0xFFFF0010U) | (msw & 0xF);
            break; }
        case 7: { /* INVLPG */
            if (rm_reg >= 0) raise_exception(s, EXCP_UD);
            tlb_flush_all(s);
            break; }
        default: raise_exception(s, EXCP_UD);
        }
        break; }

    case 0x06: /* CLTS */
        s->cr0 &= ~(1U << 3);
        break;

    case 0x20: { /* MOV r32, CRn */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        if (rm_reg < 0) rm_reg = (int)ea; /* should be reg */
        uint32_t crval;
        switch (reg) {
        case 0: crval = s->cr0; break;
        case 2: crval = s->cr2; break;
        case 3: crval = s->cr3; break;
        case 4: crval = s->cr4; break;
        default: raise_exception(s, EXCP_UD); crval = 0; break;
        }
        s->regs[rm_reg] = crval;
        break; }

    case 0x22: { /* MOV CRn, r32 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        if (rm_reg < 0) rm_reg = (int)ea;
        uint32_t val = s->regs[rm_reg];
        switch (reg) {
        case 0: {
            uint32_t old = s->cr0;
            s->cr0 = val;
            if ((val ^ old) & CR0_PG) tlb_flush_all(s);
            break; }
        case 2: s->cr2 = val; break;
        case 3: s->cr3 = val; tlb_flush_all(s); break;
        case 4: s->cr4 = val; tlb_flush_all(s); break;
        default: raise_exception(s, EXCP_UD); break;
        }
        break; }

    case 0x23: { /* MOV DRn, r32 - ignore */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        break; }

    case 0x30: { /* WRMSR */
        uint32_t msr = s->regs[1]; /* ECX */
        uint64_t val = ((uint64_t)s->regs[2] << 32) | s->regs[0];
        switch (msr) {
        case 0x174: s->sysenter_cs  = (uint32_t)val; break;
        case 0x175: s->sysenter_esp = (uint32_t)val; break;
        case 0x176: s->sysenter_eip = (uint32_t)val; break;
        case 0xC0000080: s->msr_efer = val; break;
        case 0xC0000081: s->msr_star = val; break;
        case 0xC0000082: s->msr_lstar = val; break;
        default: break; /* ignore unknown MSRs */
        }
        break; }

    case 0x31: { /* RDTSC */
        uint64_t tsc = s->get_tsc ? s->get_tsc(s->get_tsc_opaque) : s->cycle_count;
        s->regs[0] = (uint32_t)tsc;         /* EAX */
        s->regs[2] = (uint32_t)(tsc >> 32); /* EDX */
        break; }

    case 0x32: { /* RDMSR */
        uint32_t msr = s->regs[1]; /* ECX */
        uint64_t val = 0;
        switch (msr) {
        case 0x174: val = s->sysenter_cs;  break;
        case 0x175: val = s->sysenter_esp; break;
        case 0x176: val = s->sysenter_eip; break;
        case 0xC0000080: val = s->msr_efer; break;
        case 0xC0000081: val = s->msr_star; break;
        case 0xC0000082: val = s->msr_lstar; break;
        default: break;
        }
        s->regs[0] = (uint32_t)val;         /* EAX */
        s->regs[2] = (uint32_t)(val >> 32); /* EDX */
        break; }

    case 0x34: { /* SYSENTER */
        s->segs[X86_CPU_SEG_CS].sel   = s->sysenter_cs & 0xFFFC;
        s->segs[X86_CPU_SEG_CS].base  = 0;
        s->segs[X86_CPU_SEG_CS].limit = 0xFFFFFFFF;
        s->segs[X86_CPU_SEG_CS].flags = 0xc09b;
        s->segs[X86_CPU_SEG_SS].sel   = (s->sysenter_cs + 8) & 0xFFFF;
        s->segs[X86_CPU_SEG_SS].base  = 0;
        s->segs[X86_CPU_SEG_SS].limit = 0xFFFFFFFF;
        s->segs[X86_CPU_SEG_SS].flags = 0xc093;
        s->regs[4] = s->sysenter_esp;
        s->eip     = s->sysenter_eip;
        s->eflags &= ~(EF_VM | EF_IF | EF_RF);
        break; }

    case 0x35: { /* SYSEXIT */
        s->segs[X86_CPU_SEG_CS].sel   = (s->sysenter_cs + 16) & 0xFFFC;
        s->segs[X86_CPU_SEG_CS].base  = 0;
        s->segs[X86_CPU_SEG_CS].limit = 0xFFFFFFFF;
        s->segs[X86_CPU_SEG_CS].flags = 0xc09b;
        s->segs[X86_CPU_SEG_SS].sel   = (s->sysenter_cs + 24) & 0xFFFF;
        s->segs[X86_CPU_SEG_SS].base  = 0;
        s->segs[X86_CPU_SEG_SS].limit = 0xFFFFFFFF;
        s->segs[X86_CPU_SEG_SS].flags = 0xc093;
        s->regs[4] = s->regs[2]; /* ESP = EDX */
        s->eip     = s->regs[1]; /* EIP = ECX */
        break; }

    /* CMOVcc r32, r/m32 */
    case 0x40: {
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint32_t src = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, seg_ea(s, ea, ea_seg));
        if (test_cc(s, 0x0)) s->regs[reg] = src;
        break; }
    case 0x41: {
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint32_t src = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, seg_ea(s, ea, ea_seg));
        if (test_cc(s, 0x1)) s->regs[reg] = src;
        break; }
    case 0x42: {
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint32_t src = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, seg_ea(s, ea, ea_seg));
        if (test_cc(s, 0x2)) s->regs[reg] = src;
        break; }
    case 0x43: {
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint32_t src = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, seg_ea(s, ea, ea_seg));
        if (test_cc(s, 0x3)) s->regs[reg] = src;
        break; }
    case 0x44: {
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint32_t src = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, seg_ea(s, ea, ea_seg));
        if (test_cc(s, 0x4)) s->regs[reg] = src;
        break; }
    case 0x45: {
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint32_t src = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, seg_ea(s, ea, ea_seg));
        if (test_cc(s, 0x5)) s->regs[reg] = src;
        break; }
    case 0x46: {
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint32_t src = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, seg_ea(s, ea, ea_seg));
        if (test_cc(s, 0x6)) s->regs[reg] = src;
        break; }
    case 0x47: {
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint32_t src = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, seg_ea(s, ea, ea_seg));
        if (test_cc(s, 0x7)) s->regs[reg] = src;
        break; }
    case 0x48: {
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint32_t src = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, seg_ea(s, ea, ea_seg));
        if (test_cc(s, 0x8)) s->regs[reg] = src;
        break; }
    case 0x49: {
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint32_t src = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, seg_ea(s, ea, ea_seg));
        if (test_cc(s, 0x9)) s->regs[reg] = src;
        break; }
    case 0x4A: {
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint32_t src = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, seg_ea(s, ea, ea_seg));
        if (test_cc(s, 0xA)) s->regs[reg] = src;
        break; }
    case 0x4B: {
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint32_t src = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, seg_ea(s, ea, ea_seg));
        if (test_cc(s, 0xB)) s->regs[reg] = src;
        break; }
    case 0x4C: {
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint32_t src = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, seg_ea(s, ea, ea_seg));
        if (test_cc(s, 0xC)) s->regs[reg] = src;
        break; }
    case 0x4D: {
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint32_t src = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, seg_ea(s, ea, ea_seg));
        if (test_cc(s, 0xD)) s->regs[reg] = src;
        break; }
    case 0x4E: {
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint32_t src = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, seg_ea(s, ea, ea_seg));
        if (test_cc(s, 0xE)) s->regs[reg] = src;
        break; }
    case 0x4F: {
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint32_t src = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, seg_ea(s, ea, ea_seg));
        if (test_cc(s, 0xF)) s->regs[reg] = src;
        break; }

    /* Jcc near (32-bit relative) */
    case 0x80: {
        int32_t rel = (int32_t)fetch_dword(ds);
        if (test_cc(s, 0x0)) s->eip = ds->pc + rel - s->segs[X86_CPU_SEG_CS].base;
        break; }
    case 0x81: {
        int32_t rel = (int32_t)fetch_dword(ds);
        if (test_cc(s, 0x1)) s->eip = ds->pc + rel - s->segs[X86_CPU_SEG_CS].base;
        break; }
    case 0x82: {
        int32_t rel = (int32_t)fetch_dword(ds);
        if (test_cc(s, 0x2)) s->eip = ds->pc + rel - s->segs[X86_CPU_SEG_CS].base;
        break; }
    case 0x83: {
        int32_t rel = (int32_t)fetch_dword(ds);
        if (test_cc(s, 0x3)) s->eip = ds->pc + rel - s->segs[X86_CPU_SEG_CS].base;
        break; }
    case 0x84: {
        int32_t rel = (int32_t)fetch_dword(ds);
        if (test_cc(s, 0x4)) s->eip = ds->pc + rel - s->segs[X86_CPU_SEG_CS].base;
        break; }
    case 0x85: {
        int32_t rel = (int32_t)fetch_dword(ds);
        if (test_cc(s, 0x5)) s->eip = ds->pc + rel - s->segs[X86_CPU_SEG_CS].base;
        break; }
    case 0x86: {
        int32_t rel = (int32_t)fetch_dword(ds);
        if (test_cc(s, 0x6)) s->eip = ds->pc + rel - s->segs[X86_CPU_SEG_CS].base;
        break; }
    case 0x87: {
        int32_t rel = (int32_t)fetch_dword(ds);
        if (test_cc(s, 0x7)) s->eip = ds->pc + rel - s->segs[X86_CPU_SEG_CS].base;
        break; }
    case 0x88: {
        int32_t rel = (int32_t)fetch_dword(ds);
        if (test_cc(s, 0x8)) s->eip = ds->pc + rel - s->segs[X86_CPU_SEG_CS].base;
        break; }
    case 0x89: {
        int32_t rel = (int32_t)fetch_dword(ds);
        if (test_cc(s, 0x9)) s->eip = ds->pc + rel - s->segs[X86_CPU_SEG_CS].base;
        break; }
    case 0x8A: {
        int32_t rel = (int32_t)fetch_dword(ds);
        if (test_cc(s, 0xA)) s->eip = ds->pc + rel - s->segs[X86_CPU_SEG_CS].base;
        break; }
    case 0x8B: {
        int32_t rel = (int32_t)fetch_dword(ds);
        if (test_cc(s, 0xB)) s->eip = ds->pc + rel - s->segs[X86_CPU_SEG_CS].base;
        break; }
    case 0x8C: {
        int32_t rel = (int32_t)fetch_dword(ds);
        if (test_cc(s, 0xC)) s->eip = ds->pc + rel - s->segs[X86_CPU_SEG_CS].base;
        break; }
    case 0x8D: {
        int32_t rel = (int32_t)fetch_dword(ds);
        if (test_cc(s, 0xD)) s->eip = ds->pc + rel - s->segs[X86_CPU_SEG_CS].base;
        break; }
    case 0x8E: {
        int32_t rel = (int32_t)fetch_dword(ds);
        if (test_cc(s, 0xE)) s->eip = ds->pc + rel - s->segs[X86_CPU_SEG_CS].base;
        break; }
    case 0x8F: {
        int32_t rel = (int32_t)fetch_dword(ds);
        if (test_cc(s, 0xF)) s->eip = ds->pc + rel - s->segs[X86_CPU_SEG_CS].base;
        break; }

    /* SETcc r/m8 */
    case 0x90: {
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t v = test_cc(s, 0x0) ? 1 : 0;
        if (rm_reg >= 0) set_reg8(s, rm_reg, v);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), v);
        break; }
    case 0x91: {
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t v = test_cc(s, 0x1) ? 1 : 0;
        if (rm_reg >= 0) set_reg8(s, rm_reg, v);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), v);
        break; }
    case 0x92: {
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t v = test_cc(s, 0x2) ? 1 : 0;
        if (rm_reg >= 0) set_reg8(s, rm_reg, v);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), v);
        break; }
    case 0x93: {
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t v = test_cc(s, 0x3) ? 1 : 0;
        if (rm_reg >= 0) set_reg8(s, rm_reg, v);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), v);
        break; }
    case 0x94: {
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t v = test_cc(s, 0x4) ? 1 : 0;
        if (rm_reg >= 0) set_reg8(s, rm_reg, v);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), v);
        break; }
    case 0x95: {
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t v = test_cc(s, 0x5) ? 1 : 0;
        if (rm_reg >= 0) set_reg8(s, rm_reg, v);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), v);
        break; }
    case 0x96: {
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t v = test_cc(s, 0x6) ? 1 : 0;
        if (rm_reg >= 0) set_reg8(s, rm_reg, v);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), v);
        break; }
    case 0x97: {
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t v = test_cc(s, 0x7) ? 1 : 0;
        if (rm_reg >= 0) set_reg8(s, rm_reg, v);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), v);
        break; }
    case 0x98: {
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t v = test_cc(s, 0x8) ? 1 : 0;
        if (rm_reg >= 0) set_reg8(s, rm_reg, v);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), v);
        break; }
    case 0x99: {
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t v = test_cc(s, 0x9) ? 1 : 0;
        if (rm_reg >= 0) set_reg8(s, rm_reg, v);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), v);
        break; }
    case 0x9A: {
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t v = test_cc(s, 0xA) ? 1 : 0;
        if (rm_reg >= 0) set_reg8(s, rm_reg, v);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), v);
        break; }
    case 0x9B: {
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t v = test_cc(s, 0xB) ? 1 : 0;
        if (rm_reg >= 0) set_reg8(s, rm_reg, v);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), v);
        break; }
    case 0x9C: {
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t v = test_cc(s, 0xC) ? 1 : 0;
        if (rm_reg >= 0) set_reg8(s, rm_reg, v);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), v);
        break; }
    case 0x9D: {
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t v = test_cc(s, 0xD) ? 1 : 0;
        if (rm_reg >= 0) set_reg8(s, rm_reg, v);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), v);
        break; }
    case 0x9E: {
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t v = test_cc(s, 0xE) ? 1 : 0;
        if (rm_reg >= 0) set_reg8(s, rm_reg, v);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), v);
        break; }
    case 0x9F: {
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t v = test_cc(s, 0xF) ? 1 : 0;
        if (rm_reg >= 0) set_reg8(s, rm_reg, v);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), v);
        break; }

    case 0xA2: /* CPUID */
        do_cpuid(s);
        break;

    case 0xA3: { /* BT r/m32, r32 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint32_t bit_base; int bit_off;
        if (rm_reg >= 0) { bit_base = s->regs[rm_reg]; bit_off = s->regs[reg] & 31; }
        else { bit_base = vmem_read32(s, seg_ea(s, ea, ea_seg)); bit_off = s->regs[reg] & 31; }
        s->eflags &= ~EF_CF;
        if ((bit_base >> bit_off) & 1) s->eflags |= EF_CF;
        break; }

    case 0xA4: { /* SHLD r/m32, r32, imm8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        int cnt = fetch_byte(ds) & 31;
        uint32_t dst = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, seg_ea(s, ea, ea_seg));
        uint32_t src = s->regs[reg];
        uint32_t r = cnt ? (dst << cnt) | (src >> (32 - cnt)) : dst;
        if (rm_reg >= 0) s->regs[rm_reg] = r; else vmem_write32(s, seg_ea(s, ea, ea_seg), r);
        if (cnt) { flags_logic32(s, r); if (cnt == 1) { if (((r ^ dst) >> 31) & 1) s->eflags |= EF_OF; } }
        break; }

    case 0xA5: { /* SHLD r/m32, r32, CL */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        int cnt = s->regs[1] & 31; /* CL */
        uint32_t dst = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, seg_ea(s, ea, ea_seg));
        uint32_t src = s->regs[reg];
        uint32_t r = cnt ? (dst << cnt) | (src >> (32 - cnt)) : dst;
        if (rm_reg >= 0) s->regs[rm_reg] = r; else vmem_write32(s, seg_ea(s, ea, ea_seg), r);
        if (cnt) { flags_logic32(s, r); }
        break; }

    case 0xAB: { /* BTS r/m32, r32 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint32_t laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        uint32_t v = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, laddr);
        int bit = s->regs[reg] & 31;
        s->eflags &= ~EF_CF;
        if ((v >> bit) & 1) s->eflags |= EF_CF;
        v |= 1U << bit;
        if (rm_reg >= 0) s->regs[rm_reg] = v; else vmem_write32(s, laddr, v);
        break; }

    case 0xAC: { /* SHRD r/m32, r32, imm8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        int cnt = fetch_byte(ds) & 31;
        uint32_t dst = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, seg_ea(s, ea, ea_seg));
        uint32_t src = s->regs[reg];
        uint32_t r = cnt ? (dst >> cnt) | (src << (32 - cnt)) : dst;
        if (rm_reg >= 0) s->regs[rm_reg] = r; else vmem_write32(s, seg_ea(s, ea, ea_seg), r);
        if (cnt) { flags_logic32(s, r); }
        break; }

    case 0xAD: { /* SHRD r/m32, r32, CL */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        int cnt = s->regs[1] & 31;
        uint32_t dst = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, seg_ea(s, ea, ea_seg));
        uint32_t src = s->regs[reg];
        uint32_t r = cnt ? (dst >> cnt) | (src << (32 - cnt)) : dst;
        if (rm_reg >= 0) s->regs[rm_reg] = r; else vmem_write32(s, seg_ea(s, ea, ea_seg), r);
        if (cnt) { flags_logic32(s, r); }
        break; }

    case 0xAF: { /* IMUL r32, r/m32 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint32_t src = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, seg_ea(s, ea, ea_seg));
        int64_t r = (int64_t)(int32_t)s->regs[reg] * (int64_t)(int32_t)src;
        s->regs[reg] = (uint32_t)r;
        s->eflags &= ~(EF_CF | EF_OF);
        if (r != (int64_t)(int32_t)r) s->eflags |= EF_CF | EF_OF;
        break; }

    case 0xB0: { /* CMPXCHG r/m8, r8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t cmp  = get_reg8(s, 0); /* AL */
        uint8_t src  = get_reg8(s, reg);
        uint8_t dst  = (rm_reg >= 0) ? get_reg8(s, rm_reg)
                                     : vmem_read8(s, seg_ea(s, ea, ea_seg));
        flags_sub8(s, cmp, dst, cmp - dst);
        if (cmp == dst) {
            if (rm_reg >= 0) set_reg8(s, rm_reg, src); else vmem_write8(s, seg_ea(s, ea, ea_seg), src);
        } else {
            set_reg8(s, 0, dst);
        }
        break; }

    case 0xB1: { /* CMPXCHG r/m32, r32 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint32_t cmp = s->regs[0]; /* EAX */
        uint32_t src = s->regs[reg];
        uint32_t laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        uint32_t dst = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, laddr);
        flags_sub32(s, cmp, dst, cmp - dst);
        if (cmp == dst) {
            if (rm_reg >= 0) s->regs[rm_reg] = src; else vmem_write32(s, laddr, src);
        } else {
            s->regs[0] = dst;
        }
        break; }

    case 0xB3: { /* BTR r/m32, r32 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint32_t laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        uint32_t v = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, laddr);
        int bit = s->regs[reg] & 31;
        s->eflags &= ~EF_CF;
        if ((v >> bit) & 1) s->eflags |= EF_CF;
        v &= ~(1U << bit);
        if (rm_reg >= 0) s->regs[rm_reg] = v; else vmem_write32(s, laddr, v);
        break; }

    case 0xB6: { /* MOVZX r32, r/m8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t v = (rm_reg >= 0) ? get_reg8(s, rm_reg) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        s->regs[reg] = (uint32_t)v;
        break; }

    case 0xB7: { /* MOVZX r32, r/m16 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint16_t v = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, seg_ea(s, ea, ea_seg));
        s->regs[reg] = (uint32_t)v;
        break; }

    case 0xBA: { /* GROUP 8: BT/BTS/BTR/BTC r/m32, imm8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        int bit = fetch_byte(ds) & 31;
        uint32_t laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        uint32_t v = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, laddr);
        s->eflags &= ~EF_CF;
        if ((v >> bit) & 1) s->eflags |= EF_CF;
        switch (reg) {
        case 4: break; /* BT: just test */
        case 5: v |=  (1U << bit); break; /* BTS */
        case 6: v &= ~(1U << bit); break; /* BTR */
        case 7: v ^=  (1U << bit); break; /* BTC */
        }
        if (reg != 4) {
            if (rm_reg >= 0) s->regs[rm_reg] = v; else vmem_write32(s, laddr, v);
        }
        break; }

    case 0xBB: { /* BTC r/m32, r32 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint32_t laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        uint32_t v = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, laddr);
        int bit = s->regs[reg] & 31;
        s->eflags &= ~EF_CF;
        if ((v >> bit) & 1) s->eflags |= EF_CF;
        v ^= (1U << bit);
        if (rm_reg >= 0) s->regs[rm_reg] = v; else vmem_write32(s, laddr, v);
        break; }

    case 0xBC: { /* BSF r32, r/m32 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint32_t v = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, seg_ea(s, ea, ea_seg));
        s->eflags &= ~EF_ZF;
        if (v == 0) { s->eflags |= EF_ZF; }
        else {
            int i = 0; while (!((v >> i) & 1)) i++;
            s->regs[reg] = i;
        }
        break; }

    case 0xBD: { /* BSR r32, r/m32 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint32_t v = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, seg_ea(s, ea, ea_seg));
        s->eflags &= ~EF_ZF;
        if (v == 0) { s->eflags |= EF_ZF; }
        else {
            int i = 31; while (!((v >> i) & 1)) i--;
            s->regs[reg] = i;
        }
        break; }

    case 0xBE: { /* MOVSX r32, r/m8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t v = (rm_reg >= 0) ? get_reg8(s, rm_reg) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        s->regs[reg] = (uint32_t)(int32_t)(int8_t)v;
        break; }

    case 0xBF: { /* MOVSX r32, r/m16 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint16_t v = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, seg_ea(s, ea, ea_seg));
        s->regs[reg] = (uint32_t)(int32_t)(int16_t)v;
        break; }

    case 0xC0: { /* XADD r/m8, r8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t dst = (rm_reg >= 0) ? get_reg8(s, rm_reg) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        uint8_t src = get_reg8(s, reg);
        uint8_t r = dst + src;
        flags_add8(s, dst, src, r);
        set_reg8(s, reg, dst);
        if (rm_reg >= 0) set_reg8(s, rm_reg, r); else vmem_write8(s, seg_ea(s, ea, ea_seg), r);
        break; }

    case 0xC1: { /* XADD r/m32, r32 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint32_t laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        uint32_t dst = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, laddr);
        uint32_t src = s->regs[reg];
        uint32_t r = dst + src;
        flags_add32(s, dst, src, r);
        s->regs[reg] = dst;
        if (rm_reg >= 0) s->regs[rm_reg] = r; else vmem_write32(s, laddr, r);
        break; }

    case 0xC8: case 0xC9: case 0xCA: case 0xCB: /* BSWAP r32 */
    case 0xCC: case 0xCD: case 0xCE: case 0xCF: {
        int r = op2 & 7;
        uint32_t v = s->regs[r];
        s->regs[r] = ((v & 0xFF) << 24) | (((v >> 8) & 0xFF) << 16) |
                     (((v >> 16) & 0xFF) << 8) | (v >> 24);
        break; }

    default:
        raise_exception(s, EXCP_UD);
        break;
    }
}
/* ------------------------------------------------------------------
 * Main instruction decode and execute loop
 * ------------------------------------------------------------------ */

/* Execute one instruction; ds->pc starts at start of instruction */
static void exec_one(DecodeState *ds)
{
    X86CPUState *s = ds->cpu;
    uint8_t b;
    int reg, rm_reg;
    uint32_t ea, laddr;
    int ea_seg;

    /* Update DS->PC with CS base; advance past prefixes */
    ds->pc = s->segs[X86_CPU_SEG_CS].base + s->eip;
    ds->op32   = (s->segs[X86_CPU_SEG_CS].flags >> 14) & 1; /* CS.D/B bit */
    ds->addr32 = ds->op32;
    ds->seg_ovr = -1;
    ds->rep    = FALSE;
    ds->repne  = FALSE;

    /* Decode prefixes */
prefix_loop:
    b = fetch_byte(ds);
    switch (b) {
    case 0xF3: ds->rep   = TRUE; goto prefix_loop;
    case 0xF2: ds->repne = TRUE; goto prefix_loop;
    case 0xF0: /* LOCK: ignored */  goto prefix_loop;
    case 0x26: ds->seg_ovr = X86_CPU_SEG_ES; goto prefix_loop;
    case 0x2E: ds->seg_ovr = X86_CPU_SEG_CS; goto prefix_loop;
    case 0x36: ds->seg_ovr = X86_CPU_SEG_SS; goto prefix_loop;
    case 0x3E: ds->seg_ovr = X86_CPU_SEG_DS; goto prefix_loop;
    case 0x64: ds->seg_ovr = X86_CPU_SEG_FS; goto prefix_loop;
    case 0x65: ds->seg_ovr = X86_CPU_SEG_GS; goto prefix_loop;
    case 0x66: ds->op32   = !((s->segs[X86_CPU_SEG_CS].flags >> 14) & 1); goto prefix_loop;
    case 0x67: ds->addr32 = !((s->segs[X86_CPU_SEG_CS].flags >> 14) & 1); goto prefix_loop;
    default: break;
    }

    /* b is the opcode */
    switch (b) {

    case 0x00: { /* ADD r/m8, r8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = (rm_reg >= 0) ? get_reg8(s, rm_reg) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        uint8_t r = alu_op8(s, 0, a, get_reg8(s, reg));
        if (rm_reg >= 0) set_reg8(s, rm_reg, r); else vmem_write8(s, seg_ea(s, ea, ea_seg), r);
        break; }
    case 0x01: { /* ADD r/m32, r32 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op32) {
            uint32_t a = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, laddr);
            uint32_t r = alu_op32(s, 0, a, s->regs[reg]);
            if (rm_reg >= 0) s->regs[rm_reg] = r; else vmem_write32(s, laddr, r);
        } else {
            uint16_t a = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            uint16_t r = alu_op16(s, 0, a, get_reg16(s, reg));
            if (rm_reg >= 0) set_reg16(s, rm_reg, r); else vmem_write16(s, laddr, r);
        }
        break; }
    case 0x02: { /* ADD r8, r/m8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = get_reg8(s, reg);
        uint8_t b2 = (rm_reg >= 0) ? get_reg8(s, rm_reg) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        set_reg8(s, reg, alu_op8(s, 0, a, b2));
        break; }
    case 0x03: { /* ADD r32, r/m32 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op32) {
            uint32_t a = s->regs[reg];
            uint32_t b2 = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, laddr);
            s->regs[reg] = alu_op32(s, 0, a, b2);
        } else {
            uint16_t a = get_reg16(s, reg);
            uint16_t b2 = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            set_reg16(s, reg, alu_op16(s, 0, a, b2));
        }
        break; }
    case 0x04: { /* ADD AL, imm8 */
        uint8_t imm = fetch_byte(ds);
        uint8_t r = alu_op8(s, 0, (uint8_t)s->regs[0], imm);
        s->regs[0] = (s->regs[0] & 0xFFFFFF00U) | r;
        break; }
    case 0x05: { /* ADD AX/EAX, imm */
        if (ds->op32) {
            uint32_t imm = fetch_dword(ds);
            uint32_t r = alu_op32(s, 0, s->regs[0], imm);
            s->regs[0] = r;
        } else {
            uint16_t imm = fetch_word(ds);
            uint16_t r = alu_op16(s, 0, (uint16_t)s->regs[0], imm);
            s->regs[0] = (s->regs[0] & 0xFFFF0000U) | r;
        }
        break; }
    case 0x06: { /* PUSH ES */
        if (ds->op32) push32(s, s->segs[0].sel);
        else push16(s, s->segs[0].sel);
        break; }
    case 0x07: { /* POP ES */
        uint16_t sel = ds->op32 ? (uint16_t)pop32(s) : pop16(s);
        load_seg_desc(s, 0, sel);
        break; }
    case 0x08: { /* OR r/m8, r8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = (rm_reg >= 0) ? get_reg8(s, rm_reg) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        uint8_t r = alu_op8(s, 1, a, get_reg8(s, reg));
        if (rm_reg >= 0) set_reg8(s, rm_reg, r); else vmem_write8(s, seg_ea(s, ea, ea_seg), r);
        break; }
    case 0x09: { /* OR r/m32, r32 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op32) {
            uint32_t a = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, laddr);
            uint32_t r = alu_op32(s, 1, a, s->regs[reg]);
            if (rm_reg >= 0) s->regs[rm_reg] = r; else vmem_write32(s, laddr, r);
        } else {
            uint16_t a = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            uint16_t r = alu_op16(s, 1, a, get_reg16(s, reg));
            if (rm_reg >= 0) set_reg16(s, rm_reg, r); else vmem_write16(s, laddr, r);
        }
        break; }
    case 0x0A: { /* OR r8, r/m8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = get_reg8(s, reg);
        uint8_t b2 = (rm_reg >= 0) ? get_reg8(s, rm_reg) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        set_reg8(s, reg, alu_op8(s, 1, a, b2));
        break; }
    case 0x0B: { /* OR r32, r/m32 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op32) {
            uint32_t a = s->regs[reg];
            uint32_t b2 = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, laddr);
            s->regs[reg] = alu_op32(s, 1, a, b2);
        } else {
            uint16_t a = get_reg16(s, reg);
            uint16_t b2 = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            set_reg16(s, reg, alu_op16(s, 1, a, b2));
        }
        break; }
    case 0x0C: { /* OR AL, imm8 */
        uint8_t imm = fetch_byte(ds);
        uint8_t r = alu_op8(s, 1, (uint8_t)s->regs[0], imm);
        s->regs[0] = (s->regs[0] & 0xFFFFFF00U) | r;
        break; }
    case 0x0D: { /* OR AX/EAX, imm */
        if (ds->op32) {
            uint32_t imm = fetch_dword(ds);
            uint32_t r = alu_op32(s, 1, s->regs[0], imm);
            s->regs[0] = r;
        } else {
            uint16_t imm = fetch_word(ds);
            uint16_t r = alu_op16(s, 1, (uint16_t)s->regs[0], imm);
            s->regs[0] = (s->regs[0] & 0xFFFF0000U) | r;
        }
        break; }
    case 0x0E: { /* PUSH CS */
        if (ds->op32) push32(s, s->segs[1].sel);
        else push16(s, s->segs[1].sel);
        break; }
    /* case 0x0F: Two-byte escape - handled at end of switch */
    case 0x10: { /* ADC r/m8, r8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = (rm_reg >= 0) ? get_reg8(s, rm_reg) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        uint8_t r = alu_op8(s, 2, a, get_reg8(s, reg));
        if (rm_reg >= 0) set_reg8(s, rm_reg, r); else vmem_write8(s, seg_ea(s, ea, ea_seg), r);
        break; }
    case 0x11: { /* ADC r/m32, r32 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op32) {
            uint32_t a = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, laddr);
            uint32_t r = alu_op32(s, 2, a, s->regs[reg]);
            if (rm_reg >= 0) s->regs[rm_reg] = r; else vmem_write32(s, laddr, r);
        } else {
            uint16_t a = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            uint16_t r = alu_op16(s, 2, a, get_reg16(s, reg));
            if (rm_reg >= 0) set_reg16(s, rm_reg, r); else vmem_write16(s, laddr, r);
        }
        break; }
    case 0x12: { /* ADC r8, r/m8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = get_reg8(s, reg);
        uint8_t b2 = (rm_reg >= 0) ? get_reg8(s, rm_reg) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        set_reg8(s, reg, alu_op8(s, 2, a, b2));
        break; }
    case 0x13: { /* ADC r32, r/m32 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op32) {
            uint32_t a = s->regs[reg];
            uint32_t b2 = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, laddr);
            s->regs[reg] = alu_op32(s, 2, a, b2);
        } else {
            uint16_t a = get_reg16(s, reg);
            uint16_t b2 = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            set_reg16(s, reg, alu_op16(s, 2, a, b2));
        }
        break; }
    case 0x14: { /* ADC AL, imm8 */
        uint8_t imm = fetch_byte(ds);
        uint8_t r = alu_op8(s, 2, (uint8_t)s->regs[0], imm);
        s->regs[0] = (s->regs[0] & 0xFFFFFF00U) | r;
        break; }
    case 0x15: { /* ADC AX/EAX, imm */
        if (ds->op32) {
            uint32_t imm = fetch_dword(ds);
            uint32_t r = alu_op32(s, 2, s->regs[0], imm);
            s->regs[0] = r;
        } else {
            uint16_t imm = fetch_word(ds);
            uint16_t r = alu_op16(s, 2, (uint16_t)s->regs[0], imm);
            s->regs[0] = (s->regs[0] & 0xFFFF0000U) | r;
        }
        break; }
    case 0x16: { /* PUSH SS */
        if (ds->op32) push32(s, s->segs[2].sel);
        else push16(s, s->segs[2].sel);
        break; }
    case 0x17: { /* POP SS */
        uint16_t sel = ds->op32 ? (uint16_t)pop32(s) : pop16(s);
        load_seg_desc(s, 2, sel);
        break; }
    case 0x18: { /* SBB r/m8, r8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = (rm_reg >= 0) ? get_reg8(s, rm_reg) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        uint8_t r = alu_op8(s, 3, a, get_reg8(s, reg));
        if (rm_reg >= 0) set_reg8(s, rm_reg, r); else vmem_write8(s, seg_ea(s, ea, ea_seg), r);
        break; }
    case 0x19: { /* SBB r/m32, r32 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op32) {
            uint32_t a = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, laddr);
            uint32_t r = alu_op32(s, 3, a, s->regs[reg]);
            if (rm_reg >= 0) s->regs[rm_reg] = r; else vmem_write32(s, laddr, r);
        } else {
            uint16_t a = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            uint16_t r = alu_op16(s, 3, a, get_reg16(s, reg));
            if (rm_reg >= 0) set_reg16(s, rm_reg, r); else vmem_write16(s, laddr, r);
        }
        break; }
    case 0x1A: { /* SBB r8, r/m8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = get_reg8(s, reg);
        uint8_t b2 = (rm_reg >= 0) ? get_reg8(s, rm_reg) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        set_reg8(s, reg, alu_op8(s, 3, a, b2));
        break; }
    case 0x1B: { /* SBB r32, r/m32 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op32) {
            uint32_t a = s->regs[reg];
            uint32_t b2 = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, laddr);
            s->regs[reg] = alu_op32(s, 3, a, b2);
        } else {
            uint16_t a = get_reg16(s, reg);
            uint16_t b2 = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            set_reg16(s, reg, alu_op16(s, 3, a, b2));
        }
        break; }
    case 0x1C: { /* SBB AL, imm8 */
        uint8_t imm = fetch_byte(ds);
        uint8_t r = alu_op8(s, 3, (uint8_t)s->regs[0], imm);
        s->regs[0] = (s->regs[0] & 0xFFFFFF00U) | r;
        break; }
    case 0x1D: { /* SBB AX/EAX, imm */
        if (ds->op32) {
            uint32_t imm = fetch_dword(ds);
            uint32_t r = alu_op32(s, 3, s->regs[0], imm);
            s->regs[0] = r;
        } else {
            uint16_t imm = fetch_word(ds);
            uint16_t r = alu_op16(s, 3, (uint16_t)s->regs[0], imm);
            s->regs[0] = (s->regs[0] & 0xFFFF0000U) | r;
        }
        break; }
    case 0x1E: { /* PUSH DS */
        if (ds->op32) push32(s, s->segs[3].sel);
        else push16(s, s->segs[3].sel);
        break; }
    case 0x1F: { /* POP DS */
        uint16_t sel = ds->op32 ? (uint16_t)pop32(s) : pop16(s);
        load_seg_desc(s, 3, sel);
        break; }
    case 0x20: { /* AND r/m8, r8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = (rm_reg >= 0) ? get_reg8(s, rm_reg) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        uint8_t r = alu_op8(s, 4, a, get_reg8(s, reg));
        if (rm_reg >= 0) set_reg8(s, rm_reg, r); else vmem_write8(s, seg_ea(s, ea, ea_seg), r);
        break; }
    case 0x21: { /* AND r/m32, r32 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op32) {
            uint32_t a = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, laddr);
            uint32_t r = alu_op32(s, 4, a, s->regs[reg]);
            if (rm_reg >= 0) s->regs[rm_reg] = r; else vmem_write32(s, laddr, r);
        } else {
            uint16_t a = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            uint16_t r = alu_op16(s, 4, a, get_reg16(s, reg));
            if (rm_reg >= 0) set_reg16(s, rm_reg, r); else vmem_write16(s, laddr, r);
        }
        break; }
    case 0x22: { /* AND r8, r/m8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = get_reg8(s, reg);
        uint8_t b2 = (rm_reg >= 0) ? get_reg8(s, rm_reg) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        set_reg8(s, reg, alu_op8(s, 4, a, b2));
        break; }
    case 0x23: { /* AND r32, r/m32 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op32) {
            uint32_t a = s->regs[reg];
            uint32_t b2 = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, laddr);
            s->regs[reg] = alu_op32(s, 4, a, b2);
        } else {
            uint16_t a = get_reg16(s, reg);
            uint16_t b2 = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            set_reg16(s, reg, alu_op16(s, 4, a, b2));
        }
        break; }
    case 0x24: { /* AND AL, imm8 */
        uint8_t imm = fetch_byte(ds);
        uint8_t r = alu_op8(s, 4, (uint8_t)s->regs[0], imm);
        s->regs[0] = (s->regs[0] & 0xFFFFFF00U) | r;
        break; }
    case 0x25: { /* AND AX/EAX, imm */
        if (ds->op32) {
            uint32_t imm = fetch_dword(ds);
            uint32_t r = alu_op32(s, 4, s->regs[0], imm);
            s->regs[0] = r;
        } else {
            uint16_t imm = fetch_word(ds);
            uint16_t r = alu_op16(s, 4, (uint16_t)s->regs[0], imm);
            s->regs[0] = (s->regs[0] & 0xFFFF0000U) | r;
        }
        break; }
    case 0x28: { /* SUB r/m8, r8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = (rm_reg >= 0) ? get_reg8(s, rm_reg) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        uint8_t r = alu_op8(s, 5, a, get_reg8(s, reg));
        if (rm_reg >= 0) set_reg8(s, rm_reg, r); else vmem_write8(s, seg_ea(s, ea, ea_seg), r);
        break; }
    case 0x29: { /* SUB r/m32, r32 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op32) {
            uint32_t a = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, laddr);
            uint32_t r = alu_op32(s, 5, a, s->regs[reg]);
            if (rm_reg >= 0) s->regs[rm_reg] = r; else vmem_write32(s, laddr, r);
        } else {
            uint16_t a = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            uint16_t r = alu_op16(s, 5, a, get_reg16(s, reg));
            if (rm_reg >= 0) set_reg16(s, rm_reg, r); else vmem_write16(s, laddr, r);
        }
        break; }
    case 0x2A: { /* SUB r8, r/m8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = get_reg8(s, reg);
        uint8_t b2 = (rm_reg >= 0) ? get_reg8(s, rm_reg) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        set_reg8(s, reg, alu_op8(s, 5, a, b2));
        break; }
    case 0x2B: { /* SUB r32, r/m32 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op32) {
            uint32_t a = s->regs[reg];
            uint32_t b2 = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, laddr);
            s->regs[reg] = alu_op32(s, 5, a, b2);
        } else {
            uint16_t a = get_reg16(s, reg);
            uint16_t b2 = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            set_reg16(s, reg, alu_op16(s, 5, a, b2));
        }
        break; }
    case 0x2C: { /* SUB AL, imm8 */
        uint8_t imm = fetch_byte(ds);
        uint8_t r = alu_op8(s, 5, (uint8_t)s->regs[0], imm);
        s->regs[0] = (s->regs[0] & 0xFFFFFF00U) | r;
        break; }
    case 0x2D: { /* SUB AX/EAX, imm */
        if (ds->op32) {
            uint32_t imm = fetch_dword(ds);
            uint32_t r = alu_op32(s, 5, s->regs[0], imm);
            s->regs[0] = r;
        } else {
            uint16_t imm = fetch_word(ds);
            uint16_t r = alu_op16(s, 5, (uint16_t)s->regs[0], imm);
            s->regs[0] = (s->regs[0] & 0xFFFF0000U) | r;
        }
        break; }
    case 0x30: { /* XOR r/m8, r8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = (rm_reg >= 0) ? get_reg8(s, rm_reg) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        uint8_t r = alu_op8(s, 6, a, get_reg8(s, reg));
        if (rm_reg >= 0) set_reg8(s, rm_reg, r); else vmem_write8(s, seg_ea(s, ea, ea_seg), r);
        break; }
    case 0x31: { /* XOR r/m32, r32 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op32) {
            uint32_t a = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, laddr);
            uint32_t r = alu_op32(s, 6, a, s->regs[reg]);
            if (rm_reg >= 0) s->regs[rm_reg] = r; else vmem_write32(s, laddr, r);
        } else {
            uint16_t a = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            uint16_t r = alu_op16(s, 6, a, get_reg16(s, reg));
            if (rm_reg >= 0) set_reg16(s, rm_reg, r); else vmem_write16(s, laddr, r);
        }
        break; }
    case 0x32: { /* XOR r8, r/m8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = get_reg8(s, reg);
        uint8_t b2 = (rm_reg >= 0) ? get_reg8(s, rm_reg) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        set_reg8(s, reg, alu_op8(s, 6, a, b2));
        break; }
    case 0x33: { /* XOR r32, r/m32 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op32) {
            uint32_t a = s->regs[reg];
            uint32_t b2 = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, laddr);
            s->regs[reg] = alu_op32(s, 6, a, b2);
        } else {
            uint16_t a = get_reg16(s, reg);
            uint16_t b2 = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            set_reg16(s, reg, alu_op16(s, 6, a, b2));
        }
        break; }
    case 0x34: { /* XOR AL, imm8 */
        uint8_t imm = fetch_byte(ds);
        uint8_t r = alu_op8(s, 6, (uint8_t)s->regs[0], imm);
        s->regs[0] = (s->regs[0] & 0xFFFFFF00U) | r;
        break; }
    case 0x35: { /* XOR AX/EAX, imm */
        if (ds->op32) {
            uint32_t imm = fetch_dword(ds);
            uint32_t r = alu_op32(s, 6, s->regs[0], imm);
            s->regs[0] = r;
        } else {
            uint16_t imm = fetch_word(ds);
            uint16_t r = alu_op16(s, 6, (uint16_t)s->regs[0], imm);
            s->regs[0] = (s->regs[0] & 0xFFFF0000U) | r;
        }
        break; }
    case 0x38: { /* CMP r/m8, r8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = (rm_reg >= 0) ? get_reg8(s, rm_reg) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        (void)alu_op8(s, 7, a, get_reg8(s, reg));
        break; }
    case 0x39: { /* CMP r/m32, r32 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op32) {
            uint32_t a = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, laddr);
            (void)alu_op32(s, 7, a, s->regs[reg]);
        } else {
            uint16_t a = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            (void)alu_op16(s, 7, a, get_reg16(s, reg));
        }
        break; }
    case 0x3A: { /* CMP r8, r/m8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = get_reg8(s, reg);
        uint8_t b2 = (rm_reg >= 0) ? get_reg8(s, rm_reg) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        set_reg8(s, reg, alu_op8(s, 7, a, b2));
        break; }
    case 0x3B: { /* CMP r32, r/m32 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op32) {
            uint32_t a = s->regs[reg];
            uint32_t b2 = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, laddr);
            s->regs[reg] = alu_op32(s, 7, a, b2);
        } else {
            uint16_t a = get_reg16(s, reg);
            uint16_t b2 = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            set_reg16(s, reg, alu_op16(s, 7, a, b2));
        }
        break; }
    case 0x3C: { /* CMP AL, imm8 */
        uint8_t imm = fetch_byte(ds);
        (void)alu_op8(s, 7, (uint8_t)s->regs[0], imm);
        break; }
    case 0x3D: { /* CMP AX/EAX, imm */
        if (ds->op32) {
            uint32_t imm = fetch_dword(ds);
            (void)alu_op32(s, 7, s->regs[0], imm);
        } else {
            uint16_t imm = fetch_word(ds);
            (void)alu_op16(s, 7, (uint16_t)s->regs[0], imm);
        }
        break; }

    case 0x40: { /* INC EAX */
        if (ds->op32) { uint32_t v = s->regs[0]+1;
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzs32(v);
            if (v == 0x80000000U) s->eflags |= EF_OF;
            if ((v & 0xF) == 0) s->eflags |= EF_AF;
            s->regs[0] = v; }
        else { uint16_t v = (uint16_t)s->regs[0]+1;
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzs16(v);
            if (v == 0x8000U) s->eflags |= EF_OF;
            if ((v & 0xF) == 0) s->eflags |= EF_AF;
            s->regs[0] = (s->regs[0] & 0xFFFF0000U) | v; }
        break; }
    case 0x41: { /* INC ECX */
        if (ds->op32) { uint32_t v = s->regs[1]+1;
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzs32(v);
            if (v == 0x80000000U) s->eflags |= EF_OF;
            if ((v & 0xF) == 0) s->eflags |= EF_AF;
            s->regs[1] = v; }
        else { uint16_t v = (uint16_t)s->regs[1]+1;
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzs16(v);
            if (v == 0x8000U) s->eflags |= EF_OF;
            if ((v & 0xF) == 0) s->eflags |= EF_AF;
            s->regs[1] = (s->regs[1] & 0xFFFF0000U) | v; }
        break; }
    case 0x42: { /* INC EDX */
        if (ds->op32) { uint32_t v = s->regs[2]+1;
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzs32(v);
            if (v == 0x80000000U) s->eflags |= EF_OF;
            if ((v & 0xF) == 0) s->eflags |= EF_AF;
            s->regs[2] = v; }
        else { uint16_t v = (uint16_t)s->regs[2]+1;
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzs16(v);
            if (v == 0x8000U) s->eflags |= EF_OF;
            if ((v & 0xF) == 0) s->eflags |= EF_AF;
            s->regs[2] = (s->regs[2] & 0xFFFF0000U) | v; }
        break; }
    case 0x43: { /* INC EBX */
        if (ds->op32) { uint32_t v = s->regs[3]+1;
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzs32(v);
            if (v == 0x80000000U) s->eflags |= EF_OF;
            if ((v & 0xF) == 0) s->eflags |= EF_AF;
            s->regs[3] = v; }
        else { uint16_t v = (uint16_t)s->regs[3]+1;
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzs16(v);
            if (v == 0x8000U) s->eflags |= EF_OF;
            if ((v & 0xF) == 0) s->eflags |= EF_AF;
            s->regs[3] = (s->regs[3] & 0xFFFF0000U) | v; }
        break; }
    case 0x44: { /* INC ESP */
        if (ds->op32) { uint32_t v = s->regs[4]+1;
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzs32(v);
            if (v == 0x80000000U) s->eflags |= EF_OF;
            if ((v & 0xF) == 0) s->eflags |= EF_AF;
            s->regs[4] = v; }
        else { uint16_t v = (uint16_t)s->regs[4]+1;
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzs16(v);
            if (v == 0x8000U) s->eflags |= EF_OF;
            if ((v & 0xF) == 0) s->eflags |= EF_AF;
            s->regs[4] = (s->regs[4] & 0xFFFF0000U) | v; }
        break; }
    case 0x45: { /* INC EBP */
        if (ds->op32) { uint32_t v = s->regs[5]+1;
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzs32(v);
            if (v == 0x80000000U) s->eflags |= EF_OF;
            if ((v & 0xF) == 0) s->eflags |= EF_AF;
            s->regs[5] = v; }
        else { uint16_t v = (uint16_t)s->regs[5]+1;
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzs16(v);
            if (v == 0x8000U) s->eflags |= EF_OF;
            if ((v & 0xF) == 0) s->eflags |= EF_AF;
            s->regs[5] = (s->regs[5] & 0xFFFF0000U) | v; }
        break; }
    case 0x46: { /* INC ESI */
        if (ds->op32) { uint32_t v = s->regs[6]+1;
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzs32(v);
            if (v == 0x80000000U) s->eflags |= EF_OF;
            if ((v & 0xF) == 0) s->eflags |= EF_AF;
            s->regs[6] = v; }
        else { uint16_t v = (uint16_t)s->regs[6]+1;
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzs16(v);
            if (v == 0x8000U) s->eflags |= EF_OF;
            if ((v & 0xF) == 0) s->eflags |= EF_AF;
            s->regs[6] = (s->regs[6] & 0xFFFF0000U) | v; }
        break; }
    case 0x47: { /* INC EDI */
        if (ds->op32) { uint32_t v = s->regs[7]+1;
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzs32(v);
            if (v == 0x80000000U) s->eflags |= EF_OF;
            if ((v & 0xF) == 0) s->eflags |= EF_AF;
            s->regs[7] = v; }
        else { uint16_t v = (uint16_t)s->regs[7]+1;
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzs16(v);
            if (v == 0x8000U) s->eflags |= EF_OF;
            if ((v & 0xF) == 0) s->eflags |= EF_AF;
            s->regs[7] = (s->regs[7] & 0xFFFF0000U) | v; }
        break; }
    case 0x48: { /* DEC EAX */
        if (ds->op32) { uint32_t v = s->regs[0]-1;
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzs32(v);
            if (v == 0x7FFFFFFFU) s->eflags |= EF_OF;
            if ((v & 0xF) == 0xF) s->eflags |= EF_AF;
            s->regs[0] = v; }
        else { uint16_t v = (uint16_t)s->regs[0]-1;
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzs16(v);
            if (v == 0x7FFFU) s->eflags |= EF_OF;
            if ((v & 0xF) == 0xF) s->eflags |= EF_AF;
            s->regs[0] = (s->regs[0] & 0xFFFF0000U) | v; }
        break; }
    case 0x49: { /* DEC ECX */
        if (ds->op32) { uint32_t v = s->regs[1]-1;
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzs32(v);
            if (v == 0x7FFFFFFFU) s->eflags |= EF_OF;
            if ((v & 0xF) == 0xF) s->eflags |= EF_AF;
            s->regs[1] = v; }
        else { uint16_t v = (uint16_t)s->regs[1]-1;
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzs16(v);
            if (v == 0x7FFFU) s->eflags |= EF_OF;
            if ((v & 0xF) == 0xF) s->eflags |= EF_AF;
            s->regs[1] = (s->regs[1] & 0xFFFF0000U) | v; }
        break; }
    case 0x4A: { /* DEC EDX */
        if (ds->op32) { uint32_t v = s->regs[2]-1;
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzs32(v);
            if (v == 0x7FFFFFFFU) s->eflags |= EF_OF;
            if ((v & 0xF) == 0xF) s->eflags |= EF_AF;
            s->regs[2] = v; }
        else { uint16_t v = (uint16_t)s->regs[2]-1;
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzs16(v);
            if (v == 0x7FFFU) s->eflags |= EF_OF;
            if ((v & 0xF) == 0xF) s->eflags |= EF_AF;
            s->regs[2] = (s->regs[2] & 0xFFFF0000U) | v; }
        break; }
    case 0x4B: { /* DEC EBX */
        if (ds->op32) { uint32_t v = s->regs[3]-1;
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzs32(v);
            if (v == 0x7FFFFFFFU) s->eflags |= EF_OF;
            if ((v & 0xF) == 0xF) s->eflags |= EF_AF;
            s->regs[3] = v; }
        else { uint16_t v = (uint16_t)s->regs[3]-1;
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzs16(v);
            if (v == 0x7FFFU) s->eflags |= EF_OF;
            if ((v & 0xF) == 0xF) s->eflags |= EF_AF;
            s->regs[3] = (s->regs[3] & 0xFFFF0000U) | v; }
        break; }
    case 0x4C: { /* DEC ESP */
        if (ds->op32) { uint32_t v = s->regs[4]-1;
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzs32(v);
            if (v == 0x7FFFFFFFU) s->eflags |= EF_OF;
            if ((v & 0xF) == 0xF) s->eflags |= EF_AF;
            s->regs[4] = v; }
        else { uint16_t v = (uint16_t)s->regs[4]-1;
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzs16(v);
            if (v == 0x7FFFU) s->eflags |= EF_OF;
            if ((v & 0xF) == 0xF) s->eflags |= EF_AF;
            s->regs[4] = (s->regs[4] & 0xFFFF0000U) | v; }
        break; }
    case 0x4D: { /* DEC EBP */
        if (ds->op32) { uint32_t v = s->regs[5]-1;
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzs32(v);
            if (v == 0x7FFFFFFFU) s->eflags |= EF_OF;
            if ((v & 0xF) == 0xF) s->eflags |= EF_AF;
            s->regs[5] = v; }
        else { uint16_t v = (uint16_t)s->regs[5]-1;
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzs16(v);
            if (v == 0x7FFFU) s->eflags |= EF_OF;
            if ((v & 0xF) == 0xF) s->eflags |= EF_AF;
            s->regs[5] = (s->regs[5] & 0xFFFF0000U) | v; }
        break; }
    case 0x4E: { /* DEC ESI */
        if (ds->op32) { uint32_t v = s->regs[6]-1;
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzs32(v);
            if (v == 0x7FFFFFFFU) s->eflags |= EF_OF;
            if ((v & 0xF) == 0xF) s->eflags |= EF_AF;
            s->regs[6] = v; }
        else { uint16_t v = (uint16_t)s->regs[6]-1;
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzs16(v);
            if (v == 0x7FFFU) s->eflags |= EF_OF;
            if ((v & 0xF) == 0xF) s->eflags |= EF_AF;
            s->regs[6] = (s->regs[6] & 0xFFFF0000U) | v; }
        break; }
    case 0x4F: { /* DEC EDI */
        if (ds->op32) { uint32_t v = s->regs[7]-1;
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzs32(v);
            if (v == 0x7FFFFFFFU) s->eflags |= EF_OF;
            if ((v & 0xF) == 0xF) s->eflags |= EF_AF;
            s->regs[7] = v; }
        else { uint16_t v = (uint16_t)s->regs[7]-1;
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzs16(v);
            if (v == 0x7FFFU) s->eflags |= EF_OF;
            if ((v & 0xF) == 0xF) s->eflags |= EF_AF;
            s->regs[7] = (s->regs[7] & 0xFFFF0000U) | v; }
        break; }

    case 0x50: /* PUSH r32/16 */
    case 0x51: /* PUSH r32/16 */
    case 0x52: /* PUSH r32/16 */
    case 0x53: /* PUSH r32/16 */
    case 0x54: /* PUSH r32/16 */
    case 0x55: /* PUSH r32/16 */
    case 0x56: /* PUSH r32/16 */
    case 0x57: /* PUSH r32/16 */
    {
        int r = b & 7;
        if (ds->op32) push32(s, s->regs[r]);
        else push16(s, (uint16_t)s->regs[r]);
        break; }

    case 0x58: /* POP r32/16 */
    case 0x59: /* POP r32/16 */
    case 0x5A: /* POP r32/16 */
    case 0x5B: /* POP r32/16 */
    case 0x5C: /* POP r32/16 */
    case 0x5D: /* POP r32/16 */
    case 0x5E: /* POP r32/16 */
    case 0x5F: /* POP r32/16 */
    {
        int r = b & 7;
        if (ds->op32) s->regs[r] = pop32(s);
        else s->regs[r] = (s->regs[r] & 0xFFFF0000U) | pop16(s);
        break; }

    case 0x60: { /* PUSHA/PUSHAD */
        uint32_t sp = s->regs[4];
        if (ds->op32) {
            for (int i = 0; i < 8; i++) {
                push32(s, (i == 4) ? sp : s->regs[i]);
            }
        } else {
            for (int i = 0; i < 8; i++) {
                push16(s, (uint16_t)((i == 4) ? sp : s->regs[i]));
            }
        }
        break; }

    case 0x61: { /* POPA/POPAD */
        if (ds->op32) {
            uint32_t tmp[8];
            for (int i = 7; i >= 0; i--) tmp[i] = pop32(s);
            for (int i = 0; i < 8; i++) { if (i != 4) s->regs[i] = tmp[i]; }
        } else {
            uint16_t tmp[8];
            for (int i = 7; i >= 0; i--) tmp[i] = pop16(s);
            for (int i = 0; i < 8; i++) {
                if (i != 4) s->regs[i] = (s->regs[i] & 0xFFFF0000U) | tmp[i];
            }
        }
        break; }

    case 0x62: /* BOUND - ignore */ break;

    case 0x63: { /* ARPL / MOVSXD in 64-bit - in 32-bit just zero-extend */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        /* ARPL: not used in protected mode emulation, skip */
        break; }

    case 0x68: { /* PUSH imm32/16 */
        if (ds->op32) { uint32_t imm = fetch_dword(ds); push32(s, imm); }
        else          { uint16_t imm = fetch_word(ds);  push16(s, imm); }
        break; }

    case 0x69: { /* IMUL r32, r/m32, imm32 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        int32_t src = (rm_reg >= 0) ? (int32_t)s->regs[rm_reg]
                                    : (int32_t)vmem_read32(s, seg_ea(s, ea, ea_seg));
        int32_t imm = (int32_t)fetch_dword(ds);
        int64_t r = (int64_t)src * imm;
        s->regs[reg] = (uint32_t)r;
        s->eflags &= ~(EF_CF | EF_OF);
        if (r != (int64_t)(int32_t)r) s->eflags |= EF_CF | EF_OF;
        break; }

    case 0x6A: { /* PUSH imm8 (sign-extended) */
        int32_t imm = fetch_imm8s(ds);
        if (ds->op32) push32(s, (uint32_t)imm);
        else push16(s, (uint16_t)(int16_t)imm);
        break; }

    case 0x6B: { /* IMUL r32, r/m32, imm8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        int32_t src = (rm_reg >= 0) ? (int32_t)s->regs[rm_reg]
                                    : (int32_t)vmem_read32(s, seg_ea(s, ea, ea_seg));
        int32_t imm = (int32_t)(int8_t)fetch_byte(ds);
        int64_t r = (int64_t)src * imm;
        s->regs[reg] = (uint32_t)r;
        s->eflags &= ~(EF_CF | EF_OF);
        if (r != (int64_t)(int32_t)r) s->eflags |= EF_CF | EF_OF;
        break; }

    case 0x70: { /* Jcc short */
        int32_t rel = fetch_imm8s(ds);
        if (test_cc(s, 0x0)) s->eip = ds->pc + rel - s->segs[X86_CPU_SEG_CS].base;
        break; }
    case 0x71: { /* Jcc short */
        int32_t rel = fetch_imm8s(ds);
        if (test_cc(s, 0x1)) s->eip = ds->pc + rel - s->segs[X86_CPU_SEG_CS].base;
        break; }
    case 0x72: { /* Jcc short */
        int32_t rel = fetch_imm8s(ds);
        if (test_cc(s, 0x2)) s->eip = ds->pc + rel - s->segs[X86_CPU_SEG_CS].base;
        break; }
    case 0x73: { /* Jcc short */
        int32_t rel = fetch_imm8s(ds);
        if (test_cc(s, 0x3)) s->eip = ds->pc + rel - s->segs[X86_CPU_SEG_CS].base;
        break; }
    case 0x74: { /* Jcc short */
        int32_t rel = fetch_imm8s(ds);
        if (test_cc(s, 0x4)) s->eip = ds->pc + rel - s->segs[X86_CPU_SEG_CS].base;
        break; }
    case 0x75: { /* Jcc short */
        int32_t rel = fetch_imm8s(ds);
        if (test_cc(s, 0x5)) s->eip = ds->pc + rel - s->segs[X86_CPU_SEG_CS].base;
        break; }
    case 0x76: { /* Jcc short */
        int32_t rel = fetch_imm8s(ds);
        if (test_cc(s, 0x6)) s->eip = ds->pc + rel - s->segs[X86_CPU_SEG_CS].base;
        break; }
    case 0x77: { /* Jcc short */
        int32_t rel = fetch_imm8s(ds);
        if (test_cc(s, 0x7)) s->eip = ds->pc + rel - s->segs[X86_CPU_SEG_CS].base;
        break; }
    case 0x78: { /* Jcc short */
        int32_t rel = fetch_imm8s(ds);
        if (test_cc(s, 0x8)) s->eip = ds->pc + rel - s->segs[X86_CPU_SEG_CS].base;
        break; }
    case 0x79: { /* Jcc short */
        int32_t rel = fetch_imm8s(ds);
        if (test_cc(s, 0x9)) s->eip = ds->pc + rel - s->segs[X86_CPU_SEG_CS].base;
        break; }
    case 0x7A: { /* Jcc short */
        int32_t rel = fetch_imm8s(ds);
        if (test_cc(s, 0xA)) s->eip = ds->pc + rel - s->segs[X86_CPU_SEG_CS].base;
        break; }
    case 0x7B: { /* Jcc short */
        int32_t rel = fetch_imm8s(ds);
        if (test_cc(s, 0xB)) s->eip = ds->pc + rel - s->segs[X86_CPU_SEG_CS].base;
        break; }
    case 0x7C: { /* Jcc short */
        int32_t rel = fetch_imm8s(ds);
        if (test_cc(s, 0xC)) s->eip = ds->pc + rel - s->segs[X86_CPU_SEG_CS].base;
        break; }
    case 0x7D: { /* Jcc short */
        int32_t rel = fetch_imm8s(ds);
        if (test_cc(s, 0xD)) s->eip = ds->pc + rel - s->segs[X86_CPU_SEG_CS].base;
        break; }
    case 0x7E: { /* Jcc short */
        int32_t rel = fetch_imm8s(ds);
        if (test_cc(s, 0xE)) s->eip = ds->pc + rel - s->segs[X86_CPU_SEG_CS].base;
        break; }
    case 0x7F: { /* Jcc short */
        int32_t rel = fetch_imm8s(ds);
        if (test_cc(s, 0xF)) s->eip = ds->pc + rel - s->segs[X86_CPU_SEG_CS].base;
        break; }

    case 0x80: { /* GROUP 1 r/m8, imm8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        uint8_t a = (rm_reg >= 0) ? get_reg8(s, rm_reg) : vmem_read8(s, laddr);
        uint8_t imm = fetch_byte(ds);
        uint8_t r = alu_op8(s, reg, a, imm);
        if (reg != 7 && rm_reg >= 0) set_reg8(s, rm_reg, r);
        else if (reg != 7) vmem_write8(s, laddr, r);
        break; }

    case 0x81: { /* GROUP 1 r/m32, imm32 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op32) {
            uint32_t a = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, laddr);
            uint32_t imm = fetch_dword(ds);
            uint32_t r = alu_op32(s, reg, a, imm);
            if (reg != 7) { if (rm_reg >= 0) s->regs[rm_reg] = r; else vmem_write32(s, laddr, r); }
        } else {
            uint16_t a = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            uint16_t imm = fetch_word(ds);
            uint16_t r = alu_op16(s, reg, a, imm);
            if (reg != 7) { if (rm_reg >= 0) set_reg16(s, rm_reg, r); else vmem_write16(s, laddr, r); }
        }
        break; }

    case 0x82: { /* GROUP 1 r/m8, imm8 (alias of 0x80) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        uint8_t a = (rm_reg >= 0) ? get_reg8(s, rm_reg) : vmem_read8(s, laddr);
        uint8_t imm = fetch_byte(ds);
        uint8_t r = alu_op8(s, reg, a, imm);
        if (reg != 7) { if (rm_reg >= 0) set_reg8(s, rm_reg, r); else vmem_write8(s, laddr, r); }
        break; }

    case 0x83: { /* GROUP 1 r/m32, imm8 (sign-extended) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op32) {
            uint32_t a = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, laddr);
            uint32_t imm = (uint32_t)(int32_t)(int8_t)fetch_byte(ds);
            uint32_t r = alu_op32(s, reg, a, imm);
            if (reg != 7) { if (rm_reg >= 0) s->regs[rm_reg] = r; else vmem_write32(s, laddr, r); }
        } else {
            uint16_t a = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            uint16_t imm = (uint16_t)(int16_t)(int8_t)fetch_byte(ds);
            uint16_t r = alu_op16(s, reg, a, imm);
            if (reg != 7) { if (rm_reg >= 0) set_reg16(s, rm_reg, r); else vmem_write16(s, laddr, r); }
        }
        break; }

    case 0x84: { /* TEST r/m8, r8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = (rm_reg >= 0) ? get_reg8(s, rm_reg) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        flags_logic8(s, a & get_reg8(s, reg));
        break; }

    case 0x85: { /* TEST r/m32, r32 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op32) {
            uint32_t a = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, laddr);
            flags_logic32(s, a & s->regs[reg]);
        } else {
            uint16_t a = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            flags_logic16(s, a & get_reg16(s, reg));
        }
        break; }

    case 0x86: { /* XCHG r/m8, r8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        uint8_t a = (rm_reg >= 0) ? get_reg8(s, rm_reg) : vmem_read8(s, laddr);
        uint8_t b2 = get_reg8(s, reg);
        set_reg8(s, reg, a);
        if (rm_reg >= 0) set_reg8(s, rm_reg, b2); else vmem_write8(s, laddr, b2);
        break; }

    case 0x87: { /* XCHG r/m32, r32 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op32) {
            uint32_t a = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, laddr);
            uint32_t b2 = s->regs[reg]; s->regs[reg] = a;
            if (rm_reg >= 0) s->regs[rm_reg] = b2; else vmem_write32(s, laddr, b2);
        } else {
            uint16_t a = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            uint16_t b2 = get_reg16(s, reg); set_reg16(s, reg, a);
            if (rm_reg >= 0) set_reg16(s, rm_reg, b2); else vmem_write16(s, laddr, b2);
        }
        break; }

    case 0x88: { /* MOV r/m8, r8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t v = get_reg8(s, reg);
        if (rm_reg >= 0) set_reg8(s, rm_reg, v); else vmem_write8(s, seg_ea(s, ea, ea_seg), v);
        break; }

    case 0x89: { /* MOV r/m32, r32 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op32) { uint32_t v = s->regs[reg];
            if (rm_reg >= 0) s->regs[rm_reg] = v; else vmem_write32(s, laddr, v); }
        else { uint16_t v = get_reg16(s, reg);
            if (rm_reg >= 0) set_reg16(s, rm_reg, v); else vmem_write16(s, laddr, v); }
        break; }

    case 0x8A: { /* MOV r8, r/m8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t v = (rm_reg >= 0) ? get_reg8(s, rm_reg) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        set_reg8(s, reg, v);
        break; }

    case 0x8B: { /* MOV r32, r/m32 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op32) s->regs[reg] = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, laddr);
        else set_reg16(s, reg, (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr));
        break; }

    case 0x8C: { /* MOV r/m16, Sreg */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint16_t v = s->segs[reg & 7].sel;
        if (rm_reg >= 0) set_reg16(s, rm_reg, v);
        else vmem_write16(s, seg_ea(s, ea, ea_seg), v);
        break; }

    case 0x8D: { /* LEA r32, m */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        if (ds->op32) s->regs[reg] = ea;
        else set_reg16(s, reg, (uint16_t)ea);
        break; }

    case 0x8E: { /* MOV Sreg, r/m16 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint16_t sel = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, seg_ea(s, ea, ea_seg));
        int sidx = reg & 7;
        if (sidx < 6) load_seg_desc(s, sidx, sel);
        break; }

    case 0x8F: { /* POP r/m32 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op32) { uint32_t v = pop32(s);
            if (rm_reg >= 0) s->regs[rm_reg] = v; else vmem_write32(s, laddr, v); }
        else { uint16_t v = pop16(s);
            if (rm_reg >= 0) set_reg16(s, rm_reg, v); else vmem_write16(s, laddr, v); }
        break; }

    case 0x90: /* NOP / XCHG EAX,EAX */
        break;

    case 0x91: { /* XCHG r, EAX */
        if (ds->op32) { uint32_t t = s->regs[0]; s->regs[0] = s->regs[1]; s->regs[1] = t; }
        else { uint16_t t = (uint16_t)s->regs[0];
             set_reg16(s, 0, get_reg16(s, 1)); set_reg16(s, 1, t); }
        break; }
    case 0x92: { /* XCHG r, EAX */
        if (ds->op32) { uint32_t t = s->regs[0]; s->regs[0] = s->regs[2]; s->regs[2] = t; }
        else { uint16_t t = (uint16_t)s->regs[0];
             set_reg16(s, 0, get_reg16(s, 2)); set_reg16(s, 2, t); }
        break; }
    case 0x93: { /* XCHG r, EAX */
        if (ds->op32) { uint32_t t = s->regs[0]; s->regs[0] = s->regs[3]; s->regs[3] = t; }
        else { uint16_t t = (uint16_t)s->regs[0];
             set_reg16(s, 0, get_reg16(s, 3)); set_reg16(s, 3, t); }
        break; }
    case 0x94: { /* XCHG r, EAX */
        if (ds->op32) { uint32_t t = s->regs[0]; s->regs[0] = s->regs[4]; s->regs[4] = t; }
        else { uint16_t t = (uint16_t)s->regs[0];
             set_reg16(s, 0, get_reg16(s, 4)); set_reg16(s, 4, t); }
        break; }
    case 0x95: { /* XCHG r, EAX */
        if (ds->op32) { uint32_t t = s->regs[0]; s->regs[0] = s->regs[5]; s->regs[5] = t; }
        else { uint16_t t = (uint16_t)s->regs[0];
             set_reg16(s, 0, get_reg16(s, 5)); set_reg16(s, 5, t); }
        break; }
    case 0x96: { /* XCHG r, EAX */
        if (ds->op32) { uint32_t t = s->regs[0]; s->regs[0] = s->regs[6]; s->regs[6] = t; }
        else { uint16_t t = (uint16_t)s->regs[0];
             set_reg16(s, 0, get_reg16(s, 6)); set_reg16(s, 6, t); }
        break; }
    case 0x97: { /* XCHG r, EAX */
        if (ds->op32) { uint32_t t = s->regs[0]; s->regs[0] = s->regs[7]; s->regs[7] = t; }
        else { uint16_t t = (uint16_t)s->regs[0];
             set_reg16(s, 0, get_reg16(s, 7)); set_reg16(s, 7, t); }
        break; }

    case 0x98: { /* CBW/CWDE */
        if (ds->op32) s->regs[0] = (uint32_t)(int32_t)(int16_t)s->regs[0]; /* CWDE */
        else s->regs[0] = (s->regs[0] & 0xFFFF0000U) | (uint16_t)(int16_t)(int8_t)s->regs[0]; /* CBW */
        break; }

    case 0x99: { /* CWD/CDQ */
        if (ds->op32) s->regs[2] = ((int32_t)s->regs[0] < 0) ? 0xFFFFFFFFU : 0; /* CDQ -> EDX */
        else s->regs[2] = (s->regs[2] & 0xFFFF0000U) | (((int16_t)s->regs[0] < 0) ? 0xFFFF : 0); /* CWD */
        break; }

    case 0x9C: { /* PUSHFD/PUSHF */
        if (ds->op32) push32(s, s->eflags | EF_FIXED);
        else push16(s, (uint16_t)(s->eflags | EF_FIXED));
        break; }

    case 0x9D: { /* POPFD/POPF */
        uint32_t mask = EF_CF|EF_PF|EF_AF|EF_ZF|EF_SF|EF_TF|EF_IF|EF_DF|EF_OF|EF_NT|EF_AC;
        if (ds->op32) {
            uint32_t v = pop32(s);
            s->eflags = (s->eflags & ~mask) | (v & mask) | EF_FIXED;
        } else {
            uint16_t v = pop16(s);
            s->eflags = (s->eflags & ~0xFFFFU) | (v & mask) | EF_FIXED;
        }
        break; }

    case 0x9E: /* SAHF */
        s->eflags = (s->eflags & ~0xFFU) | (s->regs[0] >> 8); /* AH -> flags */
        break;

    case 0x9F: /* LAHF */
        s->regs[0] = (s->regs[0] & 0xFFFFU) | (((s->eflags & 0xFF) | 0x02) << 8);
        break;

    case 0xA0: { /* MOV AL, moffs8 */
        uint32_t addr = ds->addr32 ? fetch_dword(ds) : fetch_word(ds);
        int seg2 = (ds->seg_ovr >= 0) ? ds->seg_ovr : X86_CPU_SEG_DS;
        s->regs[0] = (s->regs[0] & 0xFFFFFF00U) | vmem_read8(s, LIN_ADDR(seg2, addr));
        break; }

    case 0xA1: { /* MOV EAX/AX, moffs32/16 */
        uint32_t addr = ds->addr32 ? fetch_dword(ds) : fetch_word(ds);
        int seg2 = (ds->seg_ovr >= 0) ? ds->seg_ovr : X86_CPU_SEG_DS;
        if (ds->op32) s->regs[0] = vmem_read32(s, LIN_ADDR(seg2, addr));
        else set_reg16(s, 0, vmem_read16(s, LIN_ADDR(seg2, addr)));
        break; }

    case 0xA2: { /* MOV moffs8, AL */
        uint32_t addr = ds->addr32 ? fetch_dword(ds) : fetch_word(ds);
        int seg2 = (ds->seg_ovr >= 0) ? ds->seg_ovr : X86_CPU_SEG_DS;
        vmem_write8(s, LIN_ADDR(seg2, addr), (uint8_t)s->regs[0]);
        break; }

    case 0xA3: { /* MOV moffs32/16, EAX/AX */
        uint32_t addr = ds->addr32 ? fetch_dword(ds) : fetch_word(ds);
        int seg2 = (ds->seg_ovr >= 0) ? ds->seg_ovr : X86_CPU_SEG_DS;
        if (ds->op32) vmem_write32(s, LIN_ADDR(seg2, addr), s->regs[0]);
        else vmem_write16(s, LIN_ADDR(seg2, addr), (uint16_t)s->regs[0]);
        break; }

    case 0xA4: { /* MOVSB */
        int seg_src = (ds->seg_ovr >= 0) ? ds->seg_ovr : X86_CPU_SEG_DS;
        int inc = (s->eflags & EF_DF) ? -1 : 1;
        uint32_t count = ds->rep ? (ds->addr32 ? s->regs[1] : (uint16_t)s->regs[1]) : 1; /* ECX/CX */
        while (count-- > 0) {
            vmem_write8(s, LIN_ADDR(X86_CPU_SEG_ES, s->regs[7]),
                        vmem_read8(s, LIN_ADDR(seg_src, s->regs[6])));
            s->regs[6] = (uint32_t)(s->regs[6] + inc);
            s->regs[7] = (uint32_t)(s->regs[7] + inc);
        }
        if (ds->rep) { if (ds->addr32) s->regs[1] = 0; else set_reg16(s, 1, 0); }
        break; }

    case 0xA5: { /* MOVSD/MOVSW */
        int seg_src = (ds->seg_ovr >= 0) ? ds->seg_ovr : X86_CPU_SEG_DS;
        int inc = (s->eflags & EF_DF) ? (ds->op32 ? -4 : -2) : (ds->op32 ? 4 : 2);
        uint32_t count = ds->rep ? (ds->addr32 ? s->regs[1] : (uint16_t)s->regs[1]) : 1;
        while (count-- > 0) {
            if (ds->op32) {
                vmem_write32(s, LIN_ADDR(X86_CPU_SEG_ES, s->regs[7]),
                             vmem_read32(s, LIN_ADDR(seg_src, s->regs[6])));
            } else {
                vmem_write16(s, LIN_ADDR(X86_CPU_SEG_ES, s->regs[7]),
                             vmem_read16(s, LIN_ADDR(seg_src, s->regs[6])));
            }
            s->regs[6] = (uint32_t)(s->regs[6] + inc);
            s->regs[7] = (uint32_t)(s->regs[7] + inc);
        }
        if (ds->rep) { if (ds->addr32) s->regs[1] = 0; else set_reg16(s, 1, 0); }
        break; }

    case 0xA6: { /* CMPSB */
        int seg_src = (ds->seg_ovr >= 0) ? ds->seg_ovr : X86_CPU_SEG_DS;
        int inc = (s->eflags & EF_DF) ? -1 : 1;
        uint32_t count = ds->rep || ds->repne ? (ds->addr32 ? s->regs[1] : (uint16_t)s->regs[1]) : 1;
        while (count-- > 0) {
            uint8_t a2 = vmem_read8(s, LIN_ADDR(seg_src, s->regs[6]));
            uint8_t b2 = vmem_read8(s, LIN_ADDR(X86_CPU_SEG_ES, s->regs[7]));
            flags_sub8(s, a2, b2, a2 - b2);
            s->regs[6] = (uint32_t)(s->regs[6] + inc);
            s->regs[7] = (uint32_t)(s->regs[7] + inc);
            if (ds->rep   && !(s->eflags & EF_ZF)) break;
            if (ds->repne &&  (s->eflags & EF_ZF)) break;
        }
        if (ds->rep || ds->repne) { if (ds->addr32) s->regs[1] = count+1; else set_reg16(s, 1, (uint16_t)(count+1)); }
        break; }

    case 0xA7: { /* CMPSD */
        int seg_src = (ds->seg_ovr >= 0) ? ds->seg_ovr : X86_CPU_SEG_DS;
        int inc = (s->eflags & EF_DF) ? (ds->op32 ? -4 : -2) : (ds->op32 ? 4 : 2);
        uint32_t count = ds->rep || ds->repne ? (ds->addr32 ? s->regs[1] : (uint16_t)s->regs[1]) : 1;
        while (count-- > 0) {
            if (ds->op32) {
                uint32_t a2 = vmem_read32(s, LIN_ADDR(seg_src, s->regs[6]));
                uint32_t b2 = vmem_read32(s, LIN_ADDR(X86_CPU_SEG_ES, s->regs[7]));
                flags_sub32(s, a2, b2, a2 - b2);
            } else {
                uint16_t a2 = vmem_read16(s, LIN_ADDR(seg_src, s->regs[6]));
                uint16_t b2 = vmem_read16(s, LIN_ADDR(X86_CPU_SEG_ES, s->regs[7]));
                flags_sub16(s, a2, b2, a2 - b2);
            }
            s->regs[6] = (uint32_t)(s->regs[6] + inc);
            s->regs[7] = (uint32_t)(s->regs[7] + inc);
            if (ds->rep   && !(s->eflags & EF_ZF)) break;
            if (ds->repne &&  (s->eflags & EF_ZF)) break;
        }
        if (ds->rep || ds->repne) { if (ds->addr32) s->regs[1] = count+1; else set_reg16(s, 1, (uint16_t)(count+1)); }
        break; }

    case 0xA8: { /* TEST AL, imm8 */
        uint8_t imm = fetch_byte(ds);
        flags_logic8(s, (uint8_t)s->regs[0] & imm);
        break; }

    case 0xA9: { /* TEST EAX/AX, imm */
        if (ds->op32) { uint32_t imm = fetch_dword(ds); flags_logic32(s, s->regs[0] & imm); }
        else { uint16_t imm = fetch_word(ds); flags_logic16(s, (uint16_t)s->regs[0] & imm); }
        break; }

    case 0xAA: { /* STOSB */
        int inc = (s->eflags & EF_DF) ? -1 : 1;
        uint32_t count = ds->rep ? (ds->addr32 ? s->regs[1] : (uint16_t)s->regs[1]) : 1;
        while (count-- > 0) {
            vmem_write8(s, LIN_ADDR(X86_CPU_SEG_ES, s->regs[7]), (uint8_t)s->regs[0]);
            s->regs[7] = (uint32_t)(s->regs[7] + inc);
        }
        if (ds->rep) { if (ds->addr32) s->regs[1] = 0; else set_reg16(s, 1, 0); }
        break; }

    case 0xAB: { /* STOSD/STOSW */
        int inc = (s->eflags & EF_DF) ? (ds->op32 ? -4 : -2) : (ds->op32 ? 4 : 2);
        uint32_t count = ds->rep ? (ds->addr32 ? s->regs[1] : (uint16_t)s->regs[1]) : 1;
        while (count-- > 0) {
            if (ds->op32) vmem_write32(s, LIN_ADDR(X86_CPU_SEG_ES, s->regs[7]), s->regs[0]);
            else vmem_write16(s, LIN_ADDR(X86_CPU_SEG_ES, s->regs[7]), (uint16_t)s->regs[0]);
            s->regs[7] = (uint32_t)(s->regs[7] + inc);
        }
        if (ds->rep) { if (ds->addr32) s->regs[1] = 0; else set_reg16(s, 1, 0); }
        break; }

    case 0xAC: { /* LODSB */
        int seg_src = (ds->seg_ovr >= 0) ? ds->seg_ovr : X86_CPU_SEG_DS;
        int inc = (s->eflags & EF_DF) ? -1 : 1;
        uint32_t count = ds->rep ? (ds->addr32 ? s->regs[1] : (uint16_t)s->regs[1]) : 1;
        while (count-- > 0) {
            s->regs[0] = (s->regs[0] & 0xFFFFFF00U) | vmem_read8(s, LIN_ADDR(seg_src, s->regs[6]));
            s->regs[6] = (uint32_t)(s->regs[6] + inc);
        }
        if (ds->rep) { if (ds->addr32) s->regs[1] = 0; else set_reg16(s, 1, 0); }
        break; }

    case 0xAD: { /* LODSD/LODSW */
        int seg_src = (ds->seg_ovr >= 0) ? ds->seg_ovr : X86_CPU_SEG_DS;
        int inc = (s->eflags & EF_DF) ? (ds->op32 ? -4 : -2) : (ds->op32 ? 4 : 2);
        uint32_t count = ds->rep ? (ds->addr32 ? s->regs[1] : (uint16_t)s->regs[1]) : 1;
        while (count-- > 0) {
            if (ds->op32) s->regs[0] = vmem_read32(s, LIN_ADDR(seg_src, s->regs[6]));
            else set_reg16(s, 0, vmem_read16(s, LIN_ADDR(seg_src, s->regs[6])));
            s->regs[6] = (uint32_t)(s->regs[6] + inc);
        }
        if (ds->rep) { if (ds->addr32) s->regs[1] = 0; else set_reg16(s, 1, 0); }
        break; }

    case 0xAE: { /* SCASB */
        int inc = (s->eflags & EF_DF) ? -1 : 1;
        uint32_t count = ds->rep || ds->repne ? (ds->addr32 ? s->regs[1] : (uint16_t)s->regs[1]) : 1;
        while (count-- > 0) {
            uint8_t v = vmem_read8(s, LIN_ADDR(X86_CPU_SEG_ES, s->regs[7]));
            flags_sub8(s, (uint8_t)s->regs[0], v, (uint8_t)s->regs[0] - v);
            s->regs[7] = (uint32_t)(s->regs[7] + inc);
            if (ds->rep   && !(s->eflags & EF_ZF)) break;
            if (ds->repne &&  (s->eflags & EF_ZF)) break;
        }
        if (ds->rep || ds->repne) { if (ds->addr32) s->regs[1] = count+1; else set_reg16(s, 1, (uint16_t)(count+1)); }
        break; }

    case 0xAF: { /* SCASD/SCASW */
        int inc = (s->eflags & EF_DF) ? (ds->op32 ? -4 : -2) : (ds->op32 ? 4 : 2);
        uint32_t count = ds->rep || ds->repne ? (ds->addr32 ? s->regs[1] : (uint16_t)s->regs[1]) : 1;
        while (count-- > 0) {
            if (ds->op32) {
                uint32_t v = vmem_read32(s, LIN_ADDR(X86_CPU_SEG_ES, s->regs[7]));
                flags_sub32(s, s->regs[0], v, s->regs[0] - v);
            } else {
                uint16_t v = vmem_read16(s, LIN_ADDR(X86_CPU_SEG_ES, s->regs[7]));
                flags_sub16(s, (uint16_t)s->regs[0], v, (uint16_t)s->regs[0] - v);
            }
            s->regs[7] = (uint32_t)(s->regs[7] + inc);
            if (ds->rep   && !(s->eflags & EF_ZF)) break;
            if (ds->repne &&  (s->eflags & EF_ZF)) break;
        }
        if (ds->rep || ds->repne) { if (ds->addr32) s->regs[1] = count+1; else set_reg16(s, 1, (uint16_t)(count+1)); }
        break; }

    case 0xB0: set_reg8(s, 0, fetch_byte(ds)); break; /* MOV r8, imm8 */
    case 0xB1: set_reg8(s, 1, fetch_byte(ds)); break; /* MOV r8, imm8 */
    case 0xB2: set_reg8(s, 2, fetch_byte(ds)); break; /* MOV r8, imm8 */
    case 0xB3: set_reg8(s, 3, fetch_byte(ds)); break; /* MOV r8, imm8 */
    case 0xB4: set_reg8(s, 4, fetch_byte(ds)); break; /* MOV r8, imm8 */
    case 0xB5: set_reg8(s, 5, fetch_byte(ds)); break; /* MOV r8, imm8 */
    case 0xB6: set_reg8(s, 6, fetch_byte(ds)); break; /* MOV r8, imm8 */
    case 0xB7: set_reg8(s, 7, fetch_byte(ds)); break; /* MOV r8, imm8 */

    case 0xB8: { /* MOV r32/16, imm */
        if (ds->op32) s->regs[0] = fetch_dword(ds);
        else set_reg16(s, 0, fetch_word(ds));
        break; }
    case 0xB9: { /* MOV r32/16, imm */
        if (ds->op32) s->regs[1] = fetch_dword(ds);
        else set_reg16(s, 1, fetch_word(ds));
        break; }
    case 0xBA: { /* MOV r32/16, imm */
        if (ds->op32) s->regs[2] = fetch_dword(ds);
        else set_reg16(s, 2, fetch_word(ds));
        break; }
    case 0xBB: { /* MOV r32/16, imm */
        if (ds->op32) s->regs[3] = fetch_dword(ds);
        else set_reg16(s, 3, fetch_word(ds));
        break; }
    case 0xBC: { /* MOV r32/16, imm */
        if (ds->op32) s->regs[4] = fetch_dword(ds);
        else set_reg16(s, 4, fetch_word(ds));
        break; }
    case 0xBD: { /* MOV r32/16, imm */
        if (ds->op32) s->regs[5] = fetch_dword(ds);
        else set_reg16(s, 5, fetch_word(ds));
        break; }
    case 0xBE: { /* MOV r32/16, imm */
        if (ds->op32) s->regs[6] = fetch_dword(ds);
        else set_reg16(s, 6, fetch_word(ds));
        break; }
    case 0xBF: { /* MOV r32/16, imm */
        if (ds->op32) s->regs[7] = fetch_dword(ds);
        else set_reg16(s, 7, fetch_word(ds));
        break; }

    case 0xC0: { /* GROUP 2 r/m8, imm8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        uint8_t v = (rm_reg >= 0) ? get_reg8(s, rm_reg) : vmem_read8(s, laddr);
        int cnt = fetch_byte(ds) & 0x1F;
        uint8_t r;
        switch (reg) {
        case 0: r = do_rol8(s, v, cnt); break; case 1: r = do_ror8(s, v, cnt); break;
        case 2: r = cnt ? (uint8_t)((v << cnt)|(((uint8_t)((s->eflags&EF_CF)?1:0))<<(cnt>0?cnt-1:0))) : v; break; /* RCL approx */
        case 3: r = cnt ? (uint8_t)((v >> cnt)|(((uint8_t)((s->eflags&EF_CF)?1:0))<<(8-cnt))) : v; break; /* RCR approx */
        case 4: case 6: r = do_shl8(s, v, cnt); break;
        case 5: r = do_shr8(s, v, cnt); break;
        case 7: r = do_sar8(s, v, cnt); break;
        default: r = v; break;
        }
        if (rm_reg >= 0) set_reg8(s, rm_reg, r); else vmem_write8(s, laddr, r);
        break; }

    case 0xC1: { /* GROUP 2 r/m32, imm8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        int cnt = fetch_byte(ds) & 0x1F;
        if (ds->op32) {
            uint32_t v = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, laddr);
            uint32_t r;
            switch (reg) {
            case 0: r = do_rol32(s, v, cnt); break; case 1: r = do_ror32(s, v, cnt); break;
            case 4: case 6: r = do_shl32(s, v, cnt); break;
            case 5: r = do_shr32(s, v, cnt); break;
            case 7: r = do_sar32(s, v, cnt); break;
            default: r = v; break;
            }
            if (rm_reg >= 0) s->regs[rm_reg] = r; else vmem_write32(s, laddr, r);
        } else {
            uint16_t v = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            uint16_t r;
            switch (reg) {
            case 0: r = do_rol16(s, v, cnt); break; case 1: r = do_ror16(s, v, cnt); break;
            case 4: case 6: r = do_shl16(s, v, cnt); break;
            case 5: r = do_shr16(s, v, cnt); break;
            case 7: r = do_sar16(s, v, cnt); break;
            default: r = v; break;
            }
            if (rm_reg >= 0) set_reg16(s, rm_reg, r); else vmem_write16(s, laddr, r);
        }
        break; }

    case 0xC2: { /* RET imm16 */
        uint16_t imm = fetch_word(ds);
        if (ds->op32) s->eip = pop32(s);
        else s->eip = (s->eip & 0xFFFF0000U) | pop16(s);
        s->regs[4] += imm;
        return; /* EIP already updated */
    }

    case 0xC3: { /* RET near */
        if (ds->op32) s->eip = pop32(s);
        else s->eip = (s->eip & 0xFFFF0000U) | pop16(s);
        return;
    }

    case 0xC4: { /* LES */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = seg_ea(s, ea, ea_seg);
        uint32_t off = ds->op32 ? vmem_read32(s, laddr) : vmem_read16(s, laddr);
        uint16_t sel = vmem_read16(s, laddr + (ds->op32 ? 4 : 2));
        if (ds->op32) s->regs[reg] = off; else set_reg16(s, reg, (uint16_t)off);
        load_seg_desc(s, X86_CPU_SEG_ES, sel);
        break; }

    case 0xC5: { /* LDS */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = seg_ea(s, ea, ea_seg);
        uint32_t off = ds->op32 ? vmem_read32(s, laddr) : vmem_read16(s, laddr);
        uint16_t sel = vmem_read16(s, laddr + (ds->op32 ? 4 : 2));
        if (ds->op32) s->regs[reg] = off; else set_reg16(s, reg, (uint16_t)off);
        load_seg_desc(s, X86_CPU_SEG_DS, sel);
        break; }

    case 0xC6: { /* MOV r/m8, imm8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t imm = fetch_byte(ds);
        if (rm_reg >= 0) set_reg8(s, rm_reg, imm); else vmem_write8(s, seg_ea(s, ea, ea_seg), imm);
        break; }

    case 0xC7: { /* MOV r/m32, imm32 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op32) { uint32_t imm = fetch_dword(ds);
            if (rm_reg >= 0) s->regs[rm_reg] = imm; else vmem_write32(s, laddr, imm); }
        else { uint16_t imm = fetch_word(ds);
            if (rm_reg >= 0) set_reg16(s, rm_reg, imm); else vmem_write16(s, laddr, imm); }
        break; }

    case 0xC8: { /* ENTER imm16, imm8 */
        uint16_t frame_size = fetch_word(ds);
        uint8_t level = fetch_byte(ds) & 31;
        (void)level; /* simplified: only level 0 */
        if (ds->op32) {
            push32(s, s->regs[5]); /* push EBP */
            s->regs[5] = s->regs[4]; /* EBP = ESP */
            s->regs[4] -= frame_size;
        } else {
            push16(s, (uint16_t)s->regs[5]);
            s->regs[5] = (s->regs[5] & 0xFFFF0000U) | (s->regs[4] & 0xFFFF);
            s->regs[4] = (s->regs[4] & 0xFFFF0000U) | ((s->regs[4] - frame_size) & 0xFFFF);
        }
        break; }

    case 0xC9: { /* LEAVE */
        s->regs[4] = s->regs[5]; /* ESP = EBP */
        if (ds->op32) s->regs[5] = pop32(s);
        else s->regs[5] = (s->regs[5] & 0xFFFF0000U) | pop16(s);
        break; }

    case 0xCA: { /* RET far imm16 */
        uint16_t imm = fetch_word(ds);
        uint32_t eip2 = ds->op32 ? pop32(s) : (uint32_t)pop16(s);
        uint16_t sel  = (uint16_t)pop32(s);
        load_seg_desc(s, X86_CPU_SEG_CS, sel);
        s->eip = eip2;
        s->regs[4] += imm;
        return;
    }

    case 0xCB: { /* RET far */
        uint32_t eip2 = ds->op32 ? pop32(s) : (uint32_t)pop16(s);
        uint16_t sel  = (uint16_t)pop32(s);
        load_seg_desc(s, X86_CPU_SEG_CS, sel);
        s->eip = eip2;
        return;
    }

    case 0xCC: /* INT3 */
        x86_do_interrupt(s, 3, FALSE, 0);
        return;

    case 0xCD: { /* INT n */
        uint8_t intno = fetch_byte(ds);
        s->eip = ds->pc - s->segs[X86_CPU_SEG_CS].base; /* advance EIP past INT */
        x86_do_interrupt(s, intno, FALSE, 0);
        return; /* EIP now points to handler, updated in do_interrupt */
    }

    case 0xCE: /* INTO */
        if (s->eflags & EF_OF)
            x86_do_interrupt(s, 4, FALSE, 0);
        break;

    case 0xCF: { /* IRETD/IRET */
        uint32_t eip2    = ds->op32 ? pop32(s) : (uint32_t)pop16(s);
        uint16_t cs_sel  = (uint16_t)(ds->op32 ? pop32(s) : pop16(s));
        uint32_t eflags2 = ds->op32 ? pop32(s) : (uint32_t)pop16(s);
        load_seg_desc(s, X86_CPU_SEG_CS, cs_sel);
        s->eip = eip2;
        uint32_t mask = EF_CF|EF_PF|EF_AF|EF_ZF|EF_SF|EF_TF|EF_IF|EF_DF|EF_OF|EF_NT|EF_AC;
        s->eflags = (s->eflags & ~mask) | (eflags2 & mask) | EF_FIXED;
        return;
    }

    case 0xD0: { /* GROUP 2 r/m8, 1 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        int cnt = 1;
        uint8_t v = (rm_reg >= 0) ? get_reg8(s, rm_reg) : vmem_read8(s, laddr);
        uint8_t r;
        switch (reg) {
        case 0: r = do_rol8(s, v, cnt); break; case 1: r = do_ror8(s, v, cnt); break;
        case 4: case 6: r = do_shl8(s, v, cnt); break;
        case 5: r = do_shr8(s, v, cnt); break;
        case 7: r = do_sar8(s, v, cnt); break;
        default: r = v; break;
        }
        if (rm_reg >= 0) set_reg8(s, rm_reg, r); else vmem_write8(s, laddr, r);
        break; }
    case 0xD1: { /* GROUP 2 r/m32, 1 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        int cnt = 1;
        if (ds->op32) {
            uint32_t v = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, laddr);
            uint32_t r;
            switch (reg) {
            case 0: r = do_rol32(s, v, cnt); break; case 1: r = do_ror32(s, v, cnt); break;
            case 4: case 6: r = do_shl32(s, v, cnt); break;
            case 5: r = do_shr32(s, v, cnt); break;
            case 7: r = do_sar32(s, v, cnt); break;
            default: r = v; break;
            }
            if (rm_reg >= 0) s->regs[rm_reg] = r; else vmem_write32(s, laddr, r);
        } else {
            uint16_t v = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            uint16_t r;
            switch (reg) {
            case 0: r = do_rol16(s, v, cnt); break; case 1: r = do_ror16(s, v, cnt); break;
            case 4: case 6: r = do_shl16(s, v, cnt); break;
            case 5: r = do_shr16(s, v, cnt); break;
            case 7: r = do_sar16(s, v, cnt); break;
            default: r = v; break;
            }
            if (rm_reg >= 0) set_reg16(s, rm_reg, r); else vmem_write16(s, laddr, r);
        }
        break; }
    case 0xD2: { /* GROUP 2 r/m8, CL */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        int cnt = (s->regs[1] & 0x1F);
        uint8_t v = (rm_reg >= 0) ? get_reg8(s, rm_reg) : vmem_read8(s, laddr);
        uint8_t r;
        switch (reg) {
        case 0: r = do_rol8(s, v, cnt); break; case 1: r = do_ror8(s, v, cnt); break;
        case 4: case 6: r = do_shl8(s, v, cnt); break;
        case 5: r = do_shr8(s, v, cnt); break;
        case 7: r = do_sar8(s, v, cnt); break;
        default: r = v; break;
        }
        if (rm_reg >= 0) set_reg8(s, rm_reg, r); else vmem_write8(s, laddr, r);
        break; }
    case 0xD3: { /* GROUP 2 r/m32, CL */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        int cnt = (s->regs[1] & 0x1F);
        if (ds->op32) {
            uint32_t v = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, laddr);
            uint32_t r;
            switch (reg) {
            case 0: r = do_rol32(s, v, cnt); break; case 1: r = do_ror32(s, v, cnt); break;
            case 4: case 6: r = do_shl32(s, v, cnt); break;
            case 5: r = do_shr32(s, v, cnt); break;
            case 7: r = do_sar32(s, v, cnt); break;
            default: r = v; break;
            }
            if (rm_reg >= 0) s->regs[rm_reg] = r; else vmem_write32(s, laddr, r);
        } else {
            uint16_t v = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            uint16_t r;
            switch (reg) {
            case 0: r = do_rol16(s, v, cnt); break; case 1: r = do_ror16(s, v, cnt); break;
            case 4: case 6: r = do_shl16(s, v, cnt); break;
            case 5: r = do_shr16(s, v, cnt); break;
            case 7: r = do_sar16(s, v, cnt); break;
            default: r = v; break;
            }
            if (rm_reg >= 0) set_reg16(s, rm_reg, r); else vmem_write16(s, laddr, r);
        }
        break; }

    case 0xD7: { /* XLAT */
        int seg2 = (ds->seg_ovr >= 0) ? ds->seg_ovr : X86_CPU_SEG_DS;
        uint32_t addr = s->regs[3] + (uint8_t)s->regs[0]; /* EBX + AL */
        s->regs[0] = (s->regs[0] & 0xFFFFFF00U) | vmem_read8(s, LIN_ADDR(seg2, addr));
        break; }

    case 0xE0: { /* LOOPNE/LOOPNZ */
        int32_t rel = fetch_imm8s(ds);
        uint32_t cnt = ds->addr32 ? --s->regs[1] : (uint32_t)(uint16_t)(s->regs[1] = (s->regs[1] & 0xFFFF0000U) | (uint16_t)(s->regs[1] - 1));
        if (cnt != 0 && !(s->eflags & EF_ZF)) s->eip = ds->pc + rel - s->segs[X86_CPU_SEG_CS].base;
        break; }

    case 0xE1: { /* LOOPE/LOOPZ */
        int32_t rel = fetch_imm8s(ds);
        uint32_t cnt = ds->addr32 ? --s->regs[1] : (uint32_t)(uint16_t)(s->regs[1] = (s->regs[1] & 0xFFFF0000U) | (uint16_t)(s->regs[1] - 1));
        if (cnt != 0 && (s->eflags & EF_ZF)) s->eip = ds->pc + rel - s->segs[X86_CPU_SEG_CS].base;
        break; }

    case 0xE2: { /* LOOP */
        int32_t rel = fetch_imm8s(ds);
        uint32_t cnt = ds->addr32 ? --s->regs[1] : (uint32_t)(uint16_t)(s->regs[1] = (s->regs[1] & 0xFFFF0000U) | (uint16_t)(s->regs[1] - 1));
        if (cnt != 0) s->eip = ds->pc + rel - s->segs[X86_CPU_SEG_CS].base;
        break; }

    case 0xE3: { /* JCXZ/JECXZ */
        int32_t rel = fetch_imm8s(ds);
        uint32_t cnt = ds->addr32 ? s->regs[1] : (uint16_t)s->regs[1];
        if (cnt == 0) s->eip = ds->pc + rel - s->segs[X86_CPU_SEG_CS].base;
        break; }

    case 0xE4: { /* IN AL, imm8 */
        uint8_t port = fetch_byte(ds);
        s->regs[0] = (s->regs[0] & 0xFFFFFF00U) | (port_in(s, port, 0) & 0xFF);
        break; }

    case 0xE5: { /* IN EAX/AX, imm8 */
        uint8_t port = fetch_byte(ds);
        if (ds->op32) s->regs[0] = port_in(s, port, 2);
        else set_reg16(s, 0, (uint16_t)port_in(s, port, 1));
        break; }

    case 0xE6: { /* OUT imm8, AL */
        uint8_t port = fetch_byte(ds);
        port_out(s, port, s->regs[0] & 0xFF, 0);
        break; }

    case 0xE7: { /* OUT imm8, EAX/AX */
        uint8_t port = fetch_byte(ds);
        if (ds->op32) port_out(s, port, s->regs[0], 2);
        else port_out(s, port, s->regs[0] & 0xFFFF, 1);
        break; }

    case 0xE8: { /* CALL near relative */
        int32_t rel = ds->op32 ? (int32_t)fetch_dword(ds) : (int32_t)(int16_t)fetch_word(ds);
        uint32_t target = ds->pc + (uint32_t)rel - s->segs[X86_CPU_SEG_CS].base;
        uint32_t ret_eip = ds->pc - s->segs[X86_CPU_SEG_CS].base;
        if (ds->op32) push32(s, ret_eip); else push16(s, (uint16_t)ret_eip);
        s->eip = target;
        return;
    }

    case 0xE9: { /* JMP near relative */
        int32_t rel = ds->op32 ? (int32_t)fetch_dword(ds) : (int32_t)(int16_t)fetch_word(ds);
        s->eip = ds->pc + (uint32_t)rel - s->segs[X86_CPU_SEG_CS].base;
        return;
    }

    case 0xEA: { /* JMP far absolute */
        uint32_t off = ds->op32 ? fetch_dword(ds) : (uint32_t)fetch_word(ds);
        uint16_t sel = fetch_word(ds);
        load_seg_desc(s, X86_CPU_SEG_CS, sel);
        s->eip = off;
        return;
    }

    case 0xEB: { /* JMP short */
        int32_t rel = fetch_imm8s(ds);
        s->eip = ds->pc + (uint32_t)rel - s->segs[X86_CPU_SEG_CS].base;
        return;
    }

    case 0xEC: { /* IN AL, DX */
        s->regs[0] = (s->regs[0] & 0xFFFFFF00U) | (port_in(s, (uint16_t)s->regs[2], 0) & 0xFF);
        break; }

    case 0xED: { /* IN EAX/AX, DX */
        if (ds->op32) s->regs[0] = port_in(s, (uint16_t)s->regs[2], 2);
        else set_reg16(s, 0, (uint16_t)port_in(s, (uint16_t)s->regs[2], 1));
        break; }

    case 0xEE: { /* OUT DX, AL */
        port_out(s, (uint16_t)s->regs[2], s->regs[0] & 0xFF, 0);
        break; }

    case 0xEF: { /* OUT DX, EAX/AX */
        if (ds->op32) port_out(s, (uint16_t)s->regs[2], s->regs[0], 2);
        else port_out(s, (uint16_t)s->regs[2], s->regs[0] & 0xFFFF, 1);
        break; }

    case 0xF4: /* HLT */
        s->power_down = TRUE;
        break;

    case 0xF5: /* CMC */
        s->eflags ^= EF_CF;
        break;

    case 0xF6: { /* GROUP 3 r/m8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        uint8_t v = (rm_reg >= 0) ? get_reg8(s, rm_reg) : vmem_read8(s, laddr);
        switch (reg) {
        case 0: case 1: { /* TEST */
            uint8_t imm = fetch_byte(ds);
            flags_logic8(s, v & imm); break; }
        case 2: { /* NOT */
            uint8_t r = ~v;
            if (rm_reg >= 0) set_reg8(s, rm_reg, r); else vmem_write8(s, laddr, r); break; }
        case 3: { /* NEG */
            uint8_t r = 0 - v;
            flags_sub8(s, 0, v, r);
            if (rm_reg >= 0) set_reg8(s, rm_reg, r); else vmem_write8(s, laddr, r); break; }
        case 4: { /* MUL AL, r/m8 -> AX */
            uint16_t r = (uint16_t)(uint8_t)s->regs[0] * v;
            s->regs[0] = (s->regs[0] & 0xFFFF0000U) | r;
            s->eflags &= ~(EF_CF|EF_OF);
            if (r >> 8) s->eflags |= EF_CF|EF_OF; break; }
        case 5: { /* IMUL AL, r/m8 -> AX */
            int16_t r = (int16_t)(int8_t)s->regs[0] * (int8_t)v;
            s->regs[0] = (s->regs[0] & 0xFFFF0000U) | (uint16_t)r;
            s->eflags &= ~(EF_CF|EF_OF);
            if (r != (int16_t)(int8_t)r) s->eflags |= EF_CF|EF_OF; break; }
        case 6: { /* DIV AX / r/m8 -> AL, AH */
            uint16_t ax = (uint16_t)s->regs[0];
            if (v == 0) raise_exception(s, EXCP_DE);
            uint8_t q = ax / v; uint8_t rem = ax % v;
            s->regs[0] = (s->regs[0] & 0xFFFF0000U) | q | ((uint32_t)rem << 8); break; }
        case 7: { /* IDIV AX / r/m8 -> AL, AH */
            int16_t ax = (int16_t)s->regs[0];
            if (v == 0) raise_exception(s, EXCP_DE);
            int8_t q = ax / (int8_t)v; int8_t rem = ax % (int8_t)v;
            s->regs[0] = (s->regs[0] & 0xFFFF0000U) | (uint8_t)q | ((uint32_t)(uint8_t)rem << 8); break; }
        }
        break; }

    case 0xF7: { /* GROUP 3 r/m32 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op32) {
            uint32_t v = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, laddr);
            switch (reg) {
            case 0: case 1: { uint32_t imm = fetch_dword(ds); flags_logic32(s, v & imm); break; }
            case 2: { uint32_t r = ~v;
                if (rm_reg >= 0) s->regs[rm_reg] = r; else vmem_write32(s, laddr, r); break; }
            case 3: { uint32_t r = 0 - v; flags_sub32(s, 0, v, r);
                if (rm_reg >= 0) s->regs[rm_reg] = r; else vmem_write32(s, laddr, r); break; }
            case 4: { /* MUL EDX:EAX = EAX * r/m32 */
                uint64_t r = (uint64_t)s->regs[0] * v;
                s->regs[0] = (uint32_t)r; s->regs[2] = (uint32_t)(r >> 32);
                s->eflags &= ~(EF_CF|EF_OF);
                if (s->regs[2]) s->eflags |= EF_CF|EF_OF; break; }
            case 5: { /* IMUL */
                int64_t r = (int64_t)(int32_t)s->regs[0] * (int32_t)v;
                s->regs[0] = (uint32_t)r; s->regs[2] = (uint32_t)(r >> 32);
                s->eflags &= ~(EF_CF|EF_OF);
                if (r != (int64_t)(int32_t)r) s->eflags |= EF_CF|EF_OF; break; }
            case 6: { /* DIV EDX:EAX / r/m32 */
                uint64_t dvd = ((uint64_t)s->regs[2] << 32) | s->regs[0];
                if (v == 0) raise_exception(s, EXCP_DE);
                s->regs[0] = (uint32_t)(dvd / v); s->regs[2] = (uint32_t)(dvd % v); break; }
            case 7: { /* IDIV */
                int64_t dvd = (int64_t)(((uint64_t)s->regs[2] << 32) | s->regs[0]);
                if (v == 0) raise_exception(s, EXCP_DE);
                s->regs[0] = (uint32_t)(dvd / (int32_t)v);
                s->regs[2] = (uint32_t)(dvd % (int32_t)v); break; }
            }
        } else {
            uint16_t v = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            switch (reg) {
            case 0: case 1: { uint16_t imm = fetch_word(ds); flags_logic16(s, v & imm); break; }
            case 2: { uint16_t r = ~v;
                if (rm_reg >= 0) set_reg16(s, rm_reg, r); else vmem_write16(s, laddr, r); break; }
            case 3: { uint16_t r = 0 - v; flags_sub16(s, 0, v, r);
                if (rm_reg >= 0) set_reg16(s, rm_reg, r); else vmem_write16(s, laddr, r); break; }
            case 4: { uint32_t r = (uint32_t)(uint16_t)s->regs[0] * v;
                s->regs[0] = (s->regs[0] & 0xFFFF0000U) | (r & 0xFFFF);
                s->regs[2] = (s->regs[2] & 0xFFFF0000U) | (r >> 16); break; }
            case 5: { int32_t r = (int16_t)s->regs[0] * (int16_t)v;
                s->regs[0] = (s->regs[0] & 0xFFFF0000U) | (uint16_t)r;
                s->regs[2] = (s->regs[2] & 0xFFFF0000U) | (uint16_t)((uint32_t)r >> 16); break; }
            case 6: { uint32_t dvd = ((uint32_t)(uint16_t)s->regs[2] << 16) | (uint16_t)s->regs[0];
                if (v == 0) raise_exception(s, EXCP_DE);
                s->regs[0] = (s->regs[0] & 0xFFFF0000U) | (uint16_t)(dvd / v);
                s->regs[2] = (s->regs[2] & 0xFFFF0000U) | (uint16_t)(dvd % v); break; }
            default: break;
            }
        }
        break; }

    case 0xF8: s->eflags &= ~EF_CF; break; /* CLC */
    case 0xF9: s->eflags |=  EF_CF; break; /* STC */
    case 0xFA: s->eflags &= ~EF_IF; break; /* CLI */
    case 0xFB: s->eflags |=  EF_IF; break; /* STI */
    case 0xFC: s->eflags &= ~EF_DF; break; /* CLD */
    case 0xFD: s->eflags |=  EF_DF; break; /* STD */

    case 0xFE: { /* GROUP 4 r/m8: INC/DEC */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        uint8_t v = (rm_reg >= 0) ? get_reg8(s, rm_reg) : vmem_read8(s, laddr);
        uint8_t r;
        if (reg == 0) { /* INC */
            r = v + 1;
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzsb8(r);
            if (v == 0x7F) s->eflags |= EF_OF;
        } else { /* DEC */
            r = v - 1;
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzsb8(r);
            if (v == 0x80) s->eflags |= EF_OF;
        }
        if (rm_reg >= 0) set_reg8(s, rm_reg, r); else vmem_write8(s, laddr, r);
        break; }

    case 0xFF: { /* GROUP 5 r/m32: INC/DEC/CALL/JMP/PUSH */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        switch (reg) {
        case 0: { /* INC r/m32 */
            if (ds->op32) {
                uint32_t v = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, laddr);
                uint32_t r = v + 1;
                s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
                s->eflags |= compute_flags_pzs32(r);
                if (v == 0x7FFFFFFFU) s->eflags |= EF_OF;
                if (rm_reg >= 0) s->regs[rm_reg] = r; else vmem_write32(s, laddr, r);
            } else {
                uint16_t v = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
                uint16_t r = v + 1;
                s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
                s->eflags |= compute_flags_pzs16(r);
                if (v == 0x7FFFU) s->eflags |= EF_OF;
                if (rm_reg >= 0) set_reg16(s, rm_reg, r); else vmem_write16(s, laddr, r);
            }
            break; }
        case 1: { /* DEC r/m32 */
            if (ds->op32) {
                uint32_t v = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, laddr);
                uint32_t r = v - 1;
                s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
                s->eflags |= compute_flags_pzs32(r);
                if (v == 0x80000000U) s->eflags |= EF_OF;
                if (rm_reg >= 0) s->regs[rm_reg] = r; else vmem_write32(s, laddr, r);
            } else {
                uint16_t v = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
                uint16_t r = v - 1;
                s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
                s->eflags |= compute_flags_pzs16(r);
                if (v == 0x8000U) s->eflags |= EF_OF;
                if (rm_reg >= 0) set_reg16(s, rm_reg, r); else vmem_write16(s, laddr, r);
            }
            break; }
        case 2: { /* CALL near r/m32 */
            uint32_t target = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, laddr);
            uint32_t ret_eip = ds->pc - s->segs[X86_CPU_SEG_CS].base;
            if (ds->op32) push32(s, ret_eip); else push16(s, (uint16_t)ret_eip);
            s->eip = target;
            return; }
        case 3: { /* CALL far */
            if (rm_reg >= 0) raise_exception(s, EXCP_UD);
            uint32_t off = ds->op32 ? vmem_read32(s, laddr) : vmem_read16(s, laddr);
            uint16_t sel = vmem_read16(s, laddr + (ds->op32 ? 4 : 2));
            uint32_t ret_eip = ds->pc - s->segs[X86_CPU_SEG_CS].base;
            if (ds->op32) { push32(s, s->segs[X86_CPU_SEG_CS].sel); push32(s, ret_eip); }
            else { push16(s, s->segs[X86_CPU_SEG_CS].sel); push16(s, (uint16_t)ret_eip); }
            load_seg_desc(s, X86_CPU_SEG_CS, sel);
            s->eip = off;
            return; }
        case 4: { /* JMP near r/m32 */
            uint32_t target = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, laddr);
            s->eip = target;
            return; }
        case 5: { /* JMP far */
            if (rm_reg >= 0) raise_exception(s, EXCP_UD);
            uint32_t off = ds->op32 ? vmem_read32(s, laddr) : vmem_read16(s, laddr);
            uint16_t sel = vmem_read16(s, laddr + (ds->op32 ? 4 : 2));
            load_seg_desc(s, X86_CPU_SEG_CS, sel);
            s->eip = off;
            return; }
        case 6: { /* PUSH r/m32 */
            if (ds->op32) {
                uint32_t v = (rm_reg >= 0) ? s->regs[rm_reg] : vmem_read32(s, laddr);
                push32(s, v);
            } else {
                uint16_t v = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
                push16(s, v);
            }
            break; }
        default: raise_exception(s, EXCP_UD); break;
        }
        break; }

    case 0x0F: /* Two-byte opcodes */
        exec_0f(ds);
        return; /* EIP updated by exec_0f */

    default:
        raise_exception(s, EXCP_UD);
        break;
    } /* end switch(b) */

    /* Update EIP past decoded instruction */
    s->eip = ds->pc - s->segs[X86_CPU_SEG_CS].base;
}
/* ------------------------------------------------------------------
 * Main interp loop and public interface functions
 * ------------------------------------------------------------------ */

void x86_cpu_interp(X86CPUState *s, int max_cycles)
{
    int cycles = 0;
    DecodeState ds_s, *ds = &ds_s;
    ds->cpu = s;

    /* Exception delivery point */
    if (setjmp(s->jmp_env) != 0) {
        /* Deliver the pending exception */
        int intno = s->exception_num;
        s->exception_num = -1;
        x86_do_interrupt(s, intno,
                         s->exception_has_error_code,
                         s->exception_error_code);
        /* continue execution from exception handler */
    }

    while (cycles < max_cycles && !s->power_down) {
        /* Check for pending hardware interrupt */
        if (s->irq_level && (s->eflags & EF_IF) && s->get_hard_intno) {
            int intno = s->get_hard_intno(s->get_hard_intno_opaque);
            if (intno >= 0) {
                s->eip = s->eip; /* ensure EIP is consistent */
                x86_do_interrupt(s, intno, FALSE, 0);
                continue;
            }
        }

        exec_one(ds);
        cycles++;
        s->cycle_count++;
    }
}

/* ------------------------------------------------------------------
 * Public API functions
 * ------------------------------------------------------------------ */

X86CPUState *x86_cpu_init(PhysMemoryMap *mem_map)
{
    X86CPUState *s;
    int i;

    init_parity_table();

    s = mallocz(sizeof(*s));
    s->mem_map = mem_map;

    /* Initialize TLB to invalid state */
    for (i = 0; i < TLB_SIZE; i++) {
        s->tlb_read[i].vaddr  = TLB_TAG_INVALID;
        s->tlb_write[i].vaddr = TLB_TAG_INVALID;
    }

    /* Default EFLAGS: bit 1 always set */
    s->eflags = EF_FIXED;

    /* Default CR0: protected mode off initially */
    s->cr0 = 0;

    s->exception_num = -1;
    s->power_down = FALSE;
    s->irq_level  = 0;

    return s;
}

void x86_cpu_end(X86CPUState *s)
{
    free(s);
}

void x86_cpu_set_irq(X86CPUState *s, BOOL set)
{
    s->irq_level = set ? 1 : 0;
    if (set)
        s->power_down = FALSE; /* wake from HLT */
}

void x86_cpu_set_reg(X86CPUState *s, int reg, uint32_t val)
{
    switch (reg) {
    case 0: case 1: case 2: case 3:
    case 4: case 5: case 6: case 7:
        s->regs[reg] = val;
        break;
    case X86_CPU_REG_EIP:
        s->eip = val;
        break;
    case X86_CPU_REG_CR0: {
        uint32_t old = s->cr0;
        s->cr0 = val;
        if ((val ^ old) & CR0_PG) tlb_flush_all(s);
        break;
    }
    case X86_CPU_REG_CR2:
        s->cr2 = val;
        break;
    default:
        break;
    }
}

uint32_t x86_cpu_get_reg(X86CPUState *s, int reg)
{
    switch (reg) {
    case 0: case 1: case 2: case 3:
    case 4: case 5: case 6: case 7:
        return s->regs[reg];
    case X86_CPU_REG_EIP:
        return s->eip;
    case X86_CPU_REG_CR0:
        return s->cr0;
    case X86_CPU_REG_CR2:
        return s->cr2;
    default:
        return 0;
    }
}

void x86_cpu_set_seg(X86CPUState *s, int seg, const X86CPUSeg *sd)
{
    if (seg >= 0 && seg < 10) {
        s->segs[seg] = *sd;
    }
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
    s->port_read   = port_read;
    s->port_write  = port_write;
    s->port_opaque = opaque;
}

int64_t x86_cpu_get_cycles(X86CPUState *s)
{
    return s->cycle_count;
}

BOOL x86_cpu_get_power_down(X86CPUState *s)
{
    return s->power_down;
}

void x86_cpu_flush_tlb_write_range_ram(X86CPUState *s,
                                       uint8_t *ram_ptr, size_t ram_size)
{
    tlb_flush_for_ram(s, ram_ptr, ram_size);
}
