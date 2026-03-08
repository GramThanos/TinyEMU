/*
 * x86/x86-64 CPU emulator - 32-bit protected mode and 64-bit long mode
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

/* Branch-prediction hints */
#ifndef likely
# define likely(x)   __builtin_expect(!!(x), 1)
# define unlikely(x) __builtin_expect(!!(x), 0)
#endif

/* Force / prevent inlining */
#ifdef __GNUC__
# define ALWAYS_INLINE __attribute__((always_inline)) static inline
# define NOINLINE      __attribute__((noinline))
#else
# define ALWAYS_INLINE static inline
# define NOINLINE
#endif

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

/* EFER MSR bits */
#define EFER_SCE (1ULL << 0)  /* system call extensions */
#define EFER_LME (1ULL << 8)  /* long mode enable */
#define EFER_LMA (1ULL << 10) /* long mode active */
#define EFER_NXE (1ULL << 11) /* no-execute enable */

/* Exception vectors */
#define EXCP_DE  0
#define EXCP_DB  1
#define EXCP_NMI 2
#define EXCP_BP  3
#define EXCP_OF  4
#define EXCP_BR  5
#define EXCP_UD  6
#define EXCP_NM  7
#define EXCP_DF  8
#define EXCP_TS  10
#define EXCP_NP  11
#define EXCP_SS  12
#define EXCP_GP  13
#define EXCP_PF  14
#define EXCP_MF  16
#define EXCP_AC  17

/* Maximum number of nested exceptions before treating it as a triple fault */
#define MAX_EXCEPTION_DEPTH 3

/* TLB */
#define TLB_SIZE  512
#define TLB_MASK  (TLB_SIZE - 1)
#define TLB_TAG_INVALID  (~0ULL)
#define PAGE_SHIFT 12
#define PAGE_SIZE  (1 << PAGE_SHIFT)
#define PAGE_MASK  (PAGE_SIZE - 1)

typedef struct {
    uint64_t vaddr;   /* page-aligned virtual addr, TLB_TAG_INVALID = invalid */
    uint8_t *ptr;     /* host pointer (NULL = device I/O or not cached) */
    uint64_t paddr;   /* physical page base address */
} TLBEntry;

struct X86CPUState {
    /*
     * General-purpose registers:
     *   32-bit: 0=EAX,1=ECX,2=EDX,3=EBX,4=ESP,5=EBP,6=ESI,7=EDI
     *   64-bit: same plus 8=R8 .. 15=R15
     */
    uint64_t regs[16];
    uint64_t rip;     /* instruction pointer (32-bit mode uses lower 32 bits) */
    uint32_t eflags;

    /* Segment registers: ES,CS,SS,DS,FS,GS,LDT,TR,GDT,IDT */
    X86CPUSeg segs[10];

    /* Control registers */
    uint32_t cr0, cr2;
    uint64_t cr3;     /* 64-bit for long mode page tables */
    uint32_t cr4;
    uint32_t dr6, dr7;

    /* MSRs */
    uint32_t sysenter_cs;
    uint64_t sysenter_esp;
    uint64_t sysenter_eip;
    uint64_t msr_efer;
    uint64_t msr_star;
    uint64_t msr_lstar;
    uint64_t msr_cstar;
    uint32_t msr_syscall_mask;
    uint64_t msr_gs_base;    /* GSBASE (kernel GS) */
    uint64_t msr_kernel_gs_base; /* KernelGSBase (used by SWAPGS) */
    uint64_t msr_fs_base;    /* FSBASE */

    /* Interrupt/exception state */
    int irq_level;
    BOOL power_down;
    int exception_num;       /* -1 = none */
    uint32_t exception_error_code;
    BOOL exception_has_error_code;
    jmp_buf jmp_env;

    /* Callbacks */
    int      (*get_hard_intno)(void *opaque);
    void     *get_hard_intno_opaque;
    uint64_t (*get_tsc)(void *opaque);
    void     *get_tsc_opaque;
    DeviceReadFunc  *port_read;
    DeviceWriteFunc *port_write;
    void     *port_opaque;

    /* Memory */
    PhysMemoryMap *mem_map;

    /* TLB (separate read and write to detect write-faults) */
    TLBEntry tlb_read[TLB_SIZE];
    TLBEntry tlb_write[TLB_SIZE];

    /* Cycle counter */
    int64_t cycle_count;
};

/* Convenience: test long mode active */
#define is_long_mode(s)  (!!((s)->msr_efer & EFER_LMA))
/* ------------------------------------------------------------------
 * Physical memory access
 * ------------------------------------------------------------------ */

static uint8_t phys_read8(X86CPUState *s, uint64_t addr)
{
    uint8_t *ptr = phys_mem_get_ram_ptr(s->mem_map, addr, FALSE);
    if (ptr) return *ptr;
    PhysMemoryRange *pr = get_phys_mem_range(s->mem_map, addr);
    if (pr && !pr->is_ram) return (uint8_t)pr->read_func(pr->opaque, (uint32_t)(addr - pr->addr), 0);
    return 0xFF;
}

static uint16_t phys_read16(X86CPUState *s, uint64_t addr)
{
    uint8_t *ptr = phys_mem_get_ram_ptr(s->mem_map, addr, FALSE);
    if (ptr) return get_le16(ptr);
    return (uint16_t)phys_read8(s, addr) | ((uint16_t)phys_read8(s, addr + 1) << 8);
}

static uint32_t phys_read32(X86CPUState *s, uint64_t addr)
{
    uint8_t *ptr = phys_mem_get_ram_ptr(s->mem_map, addr, FALSE);
    if (ptr) return get_le32(ptr);
    return (uint32_t)phys_read8(s, addr) |
           ((uint32_t)phys_read8(s, addr + 1) << 8) |
           ((uint32_t)phys_read8(s, addr + 2) << 16) |
           ((uint32_t)phys_read8(s, addr + 3) << 24);
}

static uint64_t phys_read64(X86CPUState *s, uint64_t addr)
{
    return (uint64_t)phys_read32(s, addr) | ((uint64_t)phys_read32(s, addr + 4) << 32);
}

static void phys_write8(X86CPUState *s, uint64_t addr, uint8_t val)
{
    uint8_t *ptr = phys_mem_get_ram_ptr(s->mem_map, addr, TRUE);
    if (ptr) { *ptr = val; return; }
    PhysMemoryRange *pr = get_phys_mem_range(s->mem_map, addr);
    if (pr && !pr->is_ram) pr->write_func(pr->opaque, (uint32_t)(addr - pr->addr), val, 0);
}

static void phys_write16(X86CPUState *s, uint64_t addr, uint16_t val)
{
    uint8_t *ptr = phys_mem_get_ram_ptr(s->mem_map, addr, TRUE);
    if (ptr) { put_le16(ptr, val); return; }
    phys_write8(s, addr, (uint8_t)val);
    phys_write8(s, addr + 1, (uint8_t)(val >> 8));
}

static void phys_write32(X86CPUState *s, uint64_t addr, uint32_t val)
{
    uint8_t *ptr = phys_mem_get_ram_ptr(s->mem_map, addr, TRUE);
    if (ptr) { put_le32(ptr, val); return; }
    phys_write8(s, addr,     (uint8_t)val);
    phys_write8(s, addr + 1, (uint8_t)(val >> 8));
    phys_write8(s, addr + 2, (uint8_t)(val >> 16));
    phys_write8(s, addr + 3, (uint8_t)(val >> 24));
}

static void phys_write64(X86CPUState *s, uint64_t addr, uint64_t val)
{
    phys_write32(s, addr,     (uint32_t)val);
    phys_write32(s, addr + 4, (uint32_t)(val >> 32));
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

/* Raise page fault via longjmp */
static void __attribute__((noreturn))
raise_page_fault(X86CPUState *s, uint64_t vaddr, BOOL is_write)
{
    s->cr2 = (uint32_t)vaddr; /* CR2 stores faulting address (lower 32 for compat) */
    s->exception_num = EXCP_PF;
    s->exception_error_code = (is_write ? 2 : 0);
    s->exception_has_error_code = TRUE;
    longjmp(s->jmp_env, 1);
}

/* 32-bit (non-PAE) 2-level page table walk */
static uint64_t walk_32bit_paging(X86CPUState *s, uint32_t vaddr, BOOL is_write)
{
    uint32_t pde_addr, pte_addr, pde, pte;

    pde_addr = (s->cr3 & ~0xFFFULL) + (((vaddr >> 22) & 0x3FF) << 2);
    pde = phys_read32(s, pde_addr);
    if (!(pde & 1)) raise_page_fault(s, vaddr, is_write);

    if (pde & (1 << 7)) {
        /* 4 MB page */
        return (uint64_t)(pde & 0xFFC00000U) | (vaddr & 0x3FFFFFU);
    }

    pte_addr = (pde & ~0xFFFU) + (((vaddr >> 12) & 0x3FF) << 2);
    pte = phys_read32(s, pte_addr);
    if (!(pte & 1)) raise_page_fault(s, vaddr, is_write);
    if (is_write && !(pte & 2) && (s->cr0 & CR0_WP)) {
        s->exception_error_code = 3;
        raise_page_fault(s, vaddr, is_write);
    }
    if (!(pde & (1 << 5))) phys_write32(s, pde_addr, pde | (1 << 5));
    if (is_write && !(pte & (1 << 6)))
        phys_write32(s, pte_addr, pte | (1 << 6) | (1 << 5));
    else if (!(pte & (1 << 5)))
        phys_write32(s, pte_addr, pte | (1 << 5));
    return (uint64_t)(pte & ~0xFFFU) | (vaddr & PAGE_MASK);
}

/* 4-level (PML4) page table walk for 64-bit long mode */
static uint64_t walk_64bit_paging(X86CPUState *s, uint64_t vaddr, BOOL is_write)
{
    uint64_t pml4e_addr, pdpte_addr, pde_addr, pte_addr;
    uint64_t pml4e, pdpte, pde, pte;

    /* PML4 index: bits 47:39 */
    pml4e_addr = (s->cr3 & ~0xFFFULL) + (((vaddr >> 39) & 0x1FF) << 3);
    pml4e = phys_read64(s, pml4e_addr);
    if (!(pml4e & 1)) raise_page_fault(s, vaddr, is_write);

    /* PDPT index: bits 38:30 */
    pdpte_addr = (pml4e & ~0xFFFULL & ~(1ULL << 63)) + (((vaddr >> 30) & 0x1FF) << 3);
    pdpte = phys_read64(s, pdpte_addr);
    if (!(pdpte & 1)) raise_page_fault(s, vaddr, is_write);

    /* 1 GB page? (PDPTE.PS=1) */
    if (pdpte & (1ULL << 7)) {
        return (pdpte & ~0x3FFFFFFFULL & ~(1ULL << 63)) | (vaddr & 0x3FFFFFFFU);
    }

    /* PD index: bits 29:21 */
    pde_addr = (pdpte & ~0xFFFULL & ~(1ULL << 63)) + (((vaddr >> 21) & 0x1FF) << 3);
    pde = phys_read64(s, pde_addr);
    if (!(pde & 1)) raise_page_fault(s, vaddr, is_write);

    /* 2 MB page? (PDE.PS=1) */
    if (pde & (1ULL << 7)) {
        return (pde & ~0x1FFFFFULL & ~(1ULL << 63)) | (vaddr & 0x1FFFFFU);
    }

    /* PT index: bits 20:12 */
    pte_addr = (pde & ~0xFFFULL & ~(1ULL << 63)) + (((vaddr >> 12) & 0x1FF) << 3);
    pte = phys_read64(s, pte_addr);
    if (!(pte & 1)) raise_page_fault(s, vaddr, is_write);
    if (is_write && !(pte & 2) && (s->cr0 & CR0_WP)) {
        s->exception_error_code = 3;
        raise_page_fault(s, vaddr, is_write);
    }
    /* Set accessed/dirty bits */
    if (!(pml4e & (1 << 5))) phys_write64(s, pml4e_addr, pml4e | (1 << 5));
    if (!(pdpte & (1 << 5))) phys_write64(s, pdpte_addr, pdpte | (1 << 5));
    if (!(pde & (1 << 5))) phys_write64(s, pde_addr, pde | (1 << 5));
    if (is_write && !(pte & (1 << 6)))
        phys_write64(s, pte_addr, pte | (1 << 6) | (1 << 5));
    else if (!(pte & (1 << 5)))
        phys_write64(s, pte_addr, pte | (1 << 5));
    return (pte & ~0xFFFULL & ~(1ULL << 63)) | (vaddr & PAGE_MASK);
}

/* Translate virtual → physical address; update TLB */
static uint64_t virt_to_phys(X86CPUState *s, uint64_t vaddr, BOOL is_write)
{
    uint64_t paddr;
    TLBEntry *tlb;

    if (!(s->cr0 & CR0_PG))
        return vaddr;

    tlb = is_write
        ? &s->tlb_write[(vaddr >> PAGE_SHIFT) & TLB_MASK]
        : &s->tlb_read [(vaddr >> PAGE_SHIFT) & TLB_MASK];
    if (tlb->vaddr == (vaddr & ~(uint64_t)PAGE_MASK))
        return tlb->paddr | (vaddr & PAGE_MASK);

    /* Walk page tables based on mode */
    if (is_long_mode(s))
        paddr = walk_64bit_paging(s, vaddr, is_write);
    else
        paddr = walk_32bit_paging(s, (uint32_t)vaddr, is_write);

    /* Fill TLB */
    {
        uint8_t *hp = phys_mem_get_ram_ptr(s->mem_map, paddr & ~(uint64_t)PAGE_MASK, is_write);
        tlb->vaddr = vaddr & ~(uint64_t)PAGE_MASK;
        tlb->ptr   = hp;
        tlb->paddr = paddr & ~(uint64_t)PAGE_MASK;
    }
    return paddr;
}

/* ------------------------------------------------------------------
 * Virtual memory read/write — hot paths inline TLB host-pointer lookup,
 * eliminating the second phys_mem_get_ram_ptr call on every RAM access.
 * ------------------------------------------------------------------ */

#define LIN_ADDR(seg_idx, offset) ((uint64_t)s->segs[seg_idx].base + (uint64_t)(offset))

/* Helper: resolve a virtual address to a physical address, using TLB.
 * Returns the physical address; also fills *tlb_ptr_out with the TLB's
 * cached host pointer (NULL for I/O-mapped pages). */
ALWAYS_INLINE uint64_t vmem_paddr_r(X86CPUState *s, uint64_t laddr,
                                     uint8_t **tlb_ptr_out)
{
    if (likely(s->cr0 & CR0_PG)) {
        TLBEntry *tlb = &s->tlb_read[(laddr >> PAGE_SHIFT) & TLB_MASK];
        if (likely(tlb->vaddr == (laddr & ~(uint64_t)PAGE_MASK))) {
            *tlb_ptr_out = tlb->ptr;
            return tlb->paddr | (laddr & PAGE_MASK);
        }
        /* TLB miss: walk page tables; virt_to_phys fills tlb->ptr in place. */
        uint64_t pa = virt_to_phys(s, laddr, FALSE);
        *tlb_ptr_out = tlb->ptr;
        return pa;
    }
    *tlb_ptr_out = NULL;
    return laddr;
}

ALWAYS_INLINE uint64_t vmem_paddr_w(X86CPUState *s, uint64_t laddr,
                                     uint8_t **tlb_ptr_out)
{
    if (likely(s->cr0 & CR0_PG)) {
        TLBEntry *tlb = &s->tlb_write[(laddr >> PAGE_SHIFT) & TLB_MASK];
        if (likely(tlb->vaddr == (laddr & ~(uint64_t)PAGE_MASK))) {
            *tlb_ptr_out = tlb->ptr;
            return tlb->paddr | (laddr & PAGE_MASK);
        }
        /* TLB miss: walk page tables; virt_to_phys fills tlb->ptr in place. */
        uint64_t pa = virt_to_phys(s, laddr, TRUE);
        *tlb_ptr_out = tlb->ptr;
        return pa;
    }
    *tlb_ptr_out = NULL;
    return laddr;
}

ALWAYS_INLINE uint8_t vmem_read8(X86CPUState *s, uint64_t laddr)
{
    uint8_t *hp;
    uint64_t pa = vmem_paddr_r(s, laddr, &hp);
    if (likely(hp))
        return hp[pa & PAGE_MASK];
    return phys_read8(s, pa);
}

ALWAYS_INLINE uint16_t vmem_read16(X86CPUState *s, uint64_t laddr)
{
    uint8_t *hp;
    uint64_t pa = vmem_paddr_r(s, laddr, &hp);
    if (likely(hp) && likely((pa & PAGE_MASK) <= PAGE_SIZE - 2))
        return get_le16(hp + (pa & PAGE_MASK));
    return phys_read16(s, pa);
}

ALWAYS_INLINE uint32_t vmem_read32(X86CPUState *s, uint64_t laddr)
{
    uint8_t *hp;
    uint64_t pa = vmem_paddr_r(s, laddr, &hp);
    if (likely(hp) && likely((pa & PAGE_MASK) <= PAGE_SIZE - 4))
        return get_le32(hp + (pa & PAGE_MASK));
    return phys_read32(s, pa);
}

ALWAYS_INLINE uint64_t vmem_read64(X86CPUState *s, uint64_t laddr)
{
    uint8_t *hp;
    uint64_t pa = vmem_paddr_r(s, laddr, &hp);
    if (likely(hp) && likely((pa & PAGE_MASK) <= PAGE_SIZE - 8))
        return get_le64(hp + (pa & PAGE_MASK));
    return phys_read64(s, pa);
}

ALWAYS_INLINE void vmem_write8(X86CPUState *s, uint64_t laddr, uint8_t val)
{
    uint8_t *hp;
    uint64_t pa = vmem_paddr_w(s, laddr, &hp);
    if (likely(hp)) {
        hp[pa & PAGE_MASK] = val;
        return;
    }
    phys_write8(s, pa, val);
}

ALWAYS_INLINE void vmem_write16(X86CPUState *s, uint64_t laddr, uint16_t val)
{
    uint8_t *hp;
    uint64_t pa = vmem_paddr_w(s, laddr, &hp);
    if (likely(hp) && likely((pa & PAGE_MASK) <= PAGE_SIZE - 2)) {
        put_le16(hp + (pa & PAGE_MASK), val);
        return;
    }
    phys_write16(s, pa, val);
}

ALWAYS_INLINE void vmem_write32(X86CPUState *s, uint64_t laddr, uint32_t val)
{
    uint8_t *hp;
    uint64_t pa = vmem_paddr_w(s, laddr, &hp);
    if (likely(hp) && likely((pa & PAGE_MASK) <= PAGE_SIZE - 4)) {
        put_le32(hp + (pa & PAGE_MASK), val);
        return;
    }
    phys_write32(s, pa, val);
}

ALWAYS_INLINE void vmem_write64(X86CPUState *s, uint64_t laddr, uint64_t val)
{
    uint8_t *hp;
    uint64_t pa = vmem_paddr_w(s, laddr, &hp);
    if (likely(hp) && likely((pa & PAGE_MASK) <= PAGE_SIZE - 8)) {
        put_le64(hp + (pa & PAGE_MASK), val);
        return;
    }
    phys_write64(s, pa, val);
}
/* ------------------------------------------------------------------
 * Segment descriptor helpers
 * ------------------------------------------------------------------ */

static void load_seg_desc(X86CPUState *s, int seg_idx, uint16_t sel)
{
    uint64_t dt_base;
    uint32_t lo, hi;
    X86CPUSeg *seg = &s->segs[seg_idx];

    seg->sel = sel;
    if (sel == 0) {
        seg->base  = 0;
        seg->limit = 0;
        seg->flags = 0;
        return;
    }

    if (sel & 4)
        dt_base = s->segs[X86_CPU_SEG_LDT].base;
    else
        dt_base = s->segs[X86_CPU_SEG_GDT].base;

    lo = phys_read32(s, dt_base + (sel & ~7));
    hi = phys_read32(s, dt_base + (sel & ~7) + 4);

    seg->base  = ((lo >> 16) & 0xFFFF) | ((hi & 0xFF) << 16) | ((hi >> 24) << 24);
    uint32_t lim = (lo & 0xFFFF) | (hi & 0x000F0000U);
    if (hi & (1U << 23)) lim = (lim << 12) | 0xFFF;
    seg->limit = lim;
    seg->flags = ((hi >> 8) & 0xFF) | (((hi >> 20) & 0xF) << 12);
}

/* ------------------------------------------------------------------
 * EFLAGS helpers
 * ------------------------------------------------------------------ */

/* Pre-computed parity table: EF_PF (=4) when popcount is even, 0 when odd. */
static const uint32_t parity_tab[256] = {
    4,0,0,4,0,4,4,0,0,4,4,0,4,0,0,4,  /* 0x00-0x0F */
    0,4,4,0,4,0,0,4,4,0,0,4,0,4,4,0,  /* 0x10-0x1F */
    0,4,4,0,4,0,0,4,4,0,0,4,0,4,4,0,  /* 0x20-0x2F */
    4,0,0,4,0,4,4,0,0,4,4,0,4,0,0,4,  /* 0x30-0x3F */
    0,4,4,0,4,0,0,4,4,0,0,4,0,4,4,0,  /* 0x40-0x4F */
    4,0,0,4,0,4,4,0,0,4,4,0,4,0,0,4,  /* 0x50-0x5F */
    4,0,0,4,0,4,4,0,0,4,4,0,4,0,0,4,  /* 0x60-0x6F */
    0,4,4,0,4,0,0,4,4,0,0,4,0,4,4,0,  /* 0x70-0x7F */
    0,4,4,0,4,0,0,4,4,0,0,4,0,4,4,0,  /* 0x80-0x8F */
    4,0,0,4,0,4,4,0,0,4,4,0,4,0,0,4,  /* 0x90-0x9F */
    4,0,0,4,0,4,4,0,0,4,4,0,4,0,0,4,  /* 0xA0-0xAF */
    0,4,4,0,4,0,0,4,4,0,0,4,0,4,4,0,  /* 0xB0-0xBF */
    4,0,0,4,0,4,4,0,0,4,4,0,4,0,0,4,  /* 0xC0-0xCF */
    0,4,4,0,4,0,0,4,4,0,0,4,0,4,4,0,  /* 0xD0-0xDF */
    0,4,4,0,4,0,0,4,4,0,0,4,0,4,4,0,  /* 0xE0-0xEF */
    4,0,0,4,0,4,4,0,0,4,4,0,4,0,0,4,  /* 0xF0-0xFF */
};

static uint32_t compute_flags_pzsb8(uint8_t r)
{
    return parity_tab[r] | (r == 0 ? EF_ZF : 0) | ((r & 0x80) ? EF_SF : 0);
}

static uint32_t compute_flags_pzs16(uint16_t r)
{
    return parity_tab[r & 0xFF] | (r == 0 ? EF_ZF : 0) | ((r >> 15) ? EF_SF : 0);
}

static uint32_t compute_flags_pzs32(uint32_t r)
{
    return parity_tab[r & 0xFF] | (r == 0 ? EF_ZF : 0) | ((r >> 31) ? EF_SF : 0);
}

static uint32_t compute_flags_pzs64(uint64_t r)
{
    return parity_tab[r & 0xFF] | (r == 0 ? EF_ZF : 0) | ((r >> 63) ? EF_SF : 0);
}

#define FLAGS_MASK_ARITH (EF_CF|EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF)

static void flags_add8(X86CPUState *s, uint8_t a, uint8_t b, uint8_t r)
{
    s->eflags &= ~FLAGS_MASK_ARITH;
    s->eflags |= compute_flags_pzsb8(r);
    if (r < a)                              s->eflags |= EF_CF;
    if ((a ^ b ^ r ^ ((a ^ b ^ 0x10) & 0x10)) & 0x10) s->eflags |= EF_AF;
    if ((uint8_t)(~(a ^ b) & (a ^ r)) >> 7) s->eflags |= EF_OF;
}

static void flags_add16(X86CPUState *s, uint16_t a, uint16_t b, uint16_t r)
{
    s->eflags &= ~FLAGS_MASK_ARITH;
    s->eflags |= compute_flags_pzs16(r);
    if (r < a)                              s->eflags |= EF_CF;
    if ((uint32_t)(a ^ b ^ r ^ ((a ^ b ^ 0x10) & 0x10)) & 0x10) s->eflags |= EF_AF;
    if ((uint16_t)(~(a ^ b) & (a ^ r)) >> 15) s->eflags |= EF_OF;
}

static void flags_add32(X86CPUState *s, uint32_t a, uint32_t b, uint32_t r)
{
    s->eflags &= ~FLAGS_MASK_ARITH;
    s->eflags |= compute_flags_pzs32(r);
    if (r < a)                              s->eflags |= EF_CF;
    if ((a ^ b ^ r ^ ((a ^ b ^ 0x10) & 0x10)) & 0x10) s->eflags |= EF_AF;
    if ((~(a ^ b) & (a ^ r)) >> 31)         s->eflags |= EF_OF;
}

static void flags_add64(X86CPUState *s, uint64_t a, uint64_t b, uint64_t r)
{
    s->eflags &= ~FLAGS_MASK_ARITH;
    s->eflags |= compute_flags_pzs64(r);
    if (r < a)                              s->eflags |= EF_CF;
    if ((a ^ b ^ r ^ ((a ^ b ^ 0x10) & 0x10)) & 0x10) s->eflags |= EF_AF;
    if ((~(a ^ b) & (a ^ r)) >> 63)         s->eflags |= EF_OF;
}

static void flags_sub8(X86CPUState *s, uint8_t a, uint8_t b, uint8_t r)
{
    s->eflags &= ~FLAGS_MASK_ARITH;
    s->eflags |= compute_flags_pzsb8(r);
    if (a < b)                              s->eflags |= EF_CF;
    if ((a ^ b ^ r ^ ((a ^ b ^ 0x10) & 0x10)) & 0x10) s->eflags |= EF_AF;
    if ((uint8_t)((a ^ b) & (a ^ r)) >> 7) s->eflags |= EF_OF;
}

static void flags_sub16(X86CPUState *s, uint16_t a, uint16_t b, uint16_t r)
{
    s->eflags &= ~FLAGS_MASK_ARITH;
    s->eflags |= compute_flags_pzs16(r);
    if (a < b)                              s->eflags |= EF_CF;
    if ((uint32_t)(a ^ b ^ r ^ ((a ^ b ^ 0x10) & 0x10)) & 0x10) s->eflags |= EF_AF;
    if ((uint16_t)((a ^ b) & (a ^ r)) >> 15) s->eflags |= EF_OF;
}

static void flags_sub32(X86CPUState *s, uint32_t a, uint32_t b, uint32_t r)
{
    s->eflags &= ~FLAGS_MASK_ARITH;
    s->eflags |= compute_flags_pzs32(r);
    if (a < b)                              s->eflags |= EF_CF;
    if ((a ^ b ^ r ^ ((a ^ b ^ 0x10) & 0x10)) & 0x10) s->eflags |= EF_AF;
    if (((a ^ b) & (a ^ r)) >> 31)          s->eflags |= EF_OF;
}

static void flags_sub64(X86CPUState *s, uint64_t a, uint64_t b, uint64_t r)
{
    s->eflags &= ~FLAGS_MASK_ARITH;
    s->eflags |= compute_flags_pzs64(r);
    if (a < b)                              s->eflags |= EF_CF;
    if ((a ^ b ^ r ^ ((a ^ b ^ 0x10) & 0x10)) & 0x10) s->eflags |= EF_AF;
    if (((a ^ b) & (a ^ r)) >> 63)          s->eflags |= EF_OF;
}

static void flags_logic8(X86CPUState *s, uint8_t r)
{
    s->eflags &= ~FLAGS_MASK_ARITH;
    s->eflags |= compute_flags_pzsb8(r);
}

static void flags_logic16(X86CPUState *s, uint16_t r)
{
    s->eflags &= ~FLAGS_MASK_ARITH;
    s->eflags |= compute_flags_pzs16(r);
}

static void flags_logic32(X86CPUState *s, uint32_t r)
{
    s->eflags &= ~FLAGS_MASK_ARITH;
    s->eflags |= compute_flags_pzs32(r);
}

static void flags_logic64(X86CPUState *s, uint64_t r)
{
    s->eflags &= ~FLAGS_MASK_ARITH;
    s->eflags |= compute_flags_pzs64(r);
}

static void flags_adc32(X86CPUState *s, uint32_t a, uint32_t b, uint32_t cin, uint32_t r)
{
    (void)r; (void)cin;
    s->eflags &= ~FLAGS_MASK_ARITH;
    s->eflags |= compute_flags_pzs32(a + b + cin);
    if ((uint64_t)a + (uint64_t)b + cin > 0xFFFFFFFFU) s->eflags |= EF_CF;
    if ((~(a ^ b) & (a ^ (a + b + cin))) >> 31) s->eflags |= EF_OF;
}

static void flags_adc64(X86CPUState *s, uint64_t a, uint64_t b, uint64_t cin, uint64_t r)
{
    (void)r;
    s->eflags &= ~FLAGS_MASK_ARITH;
    s->eflags |= compute_flags_pzs64(a + b + cin);
    /* CF: use __uint128_t for overflow check */
    if ((__uint128_t)a + (__uint128_t)b + cin > UINT64_MAX) s->eflags |= EF_CF;
    if ((~(a ^ b) & (a ^ (a + b + cin))) >> 63) s->eflags |= EF_OF;
}

/* ------------------------------------------------------------------
 * Exception / interrupt delivery
 * ------------------------------------------------------------------ */

/* Forward declaration: raise_exception_err is defined below */
static void __attribute__((noreturn))
raise_exception_err(X86CPUState *s, int intno, uint32_t err);

static void x86_do_interrupt(X86CPUState *s, int intno,
                              BOOL has_error_code, uint32_t error_code)
{
    uint64_t idt_base = s->segs[X86_CPU_SEG_IDT].base;
    uint32_t idt_limit = s->segs[X86_CPU_SEG_IDT].limit;
    uint64_t idt_addr;
    uint8_t gate_type;

    if (is_long_mode(s)) {
        /* 64-bit IDT gate: 16 bytes per entry */
        uint64_t lo, hi;
        uint64_t gate_offset;
        uint16_t gate_sel;
        uint64_t rsp;

        /*
         * If the IDT is too small for this vector, raise #GP.
         * This happens legitimately before the kernel installs the IDT.
         * Error code: (vector << 3) | IDT=1, EXT=0.
         */
        if ((uint32_t)(intno * 16 + 15) > idt_limit)
            raise_exception_err(s, EXCP_GP, (intno << 3) | 2);

        idt_addr = idt_base + intno * 16;
        lo = phys_read64(s, idt_addr);
        hi = phys_read64(s, idt_addr + 8);

        gate_offset = (lo & 0xFFFFULL) |
                      ((lo >> 32) & 0xFFFF0000ULL) |
                      (hi << 32);
        gate_sel    = (uint16_t)(lo >> 16);
        gate_type   = (uint8_t)(lo >> 40);

        /*
         * Gate-not-present (P=0): raise #NP with the gate selector.
         * Error code: (selector_index << 3) | IDT=1, EXT=0.
         */
        if (!(gate_type & 0x80))
            raise_exception_err(s, EXCP_NP, (gate_sel & ~7) | 2);

        rsp = s->regs[4]; /* RSP */
        /* Push SS:RSP (at CPL3→0 no actual stack switch here - simplified) */
        rsp -= 8; vmem_write64(s, LIN_ADDR(X86_CPU_SEG_SS, rsp),
                                (uint64_t)s->segs[X86_CPU_SEG_SS].sel);
        rsp -= 8; vmem_write64(s, LIN_ADDR(X86_CPU_SEG_SS, rsp), s->regs[4]);
        rsp -= 8; vmem_write64(s, LIN_ADDR(X86_CPU_SEG_SS, rsp),
                                (uint64_t)(s->eflags | EF_FIXED));
        rsp -= 8; vmem_write64(s, LIN_ADDR(X86_CPU_SEG_SS, rsp),
                                (uint64_t)s->segs[X86_CPU_SEG_CS].sel);
        rsp -= 8; vmem_write64(s, LIN_ADDR(X86_CPU_SEG_SS, rsp), s->rip);
        if (has_error_code) {
            rsp -= 8; vmem_write64(s, LIN_ADDR(X86_CPU_SEG_SS, rsp),
                                    (uint64_t)error_code);
        }
        s->regs[4] = rsp;

        load_seg_desc(s, X86_CPU_SEG_CS, gate_sel);
        s->rip = gate_offset;
        if ((gate_type & 0xF) == 0xE) /* interrupt gate */
            s->eflags &= ~EF_IF;
        s->eflags &= ~(EF_TF | EF_NT | EF_RF);
    } else {
        /* 32-bit IDT gate: 8 bytes per entry */
        uint32_t lo, hi;
        uint32_t gate_offset;
        uint16_t gate_sel;
        uint32_t esp;

        /*
         * If the IDT is too small for this vector, raise #GP.
         * Error code: (vector << 3) | IDT=1, EXT=0.
         */
        if ((uint32_t)(intno * 8 + 7) > idt_limit)
            raise_exception_err(s, EXCP_GP, (intno << 3) | 2);

        idt_addr = idt_base + intno * 8;
        lo = phys_read32(s, idt_addr);
        hi = phys_read32(s, idt_addr + 4);

        gate_offset = (lo & 0xFFFF) | (hi & 0xFFFF0000U);
        gate_sel    = (uint16_t)(lo >> 16);
        gate_type   = (uint8_t)(hi >> 8);

        /*
         * Gate-not-present (P=0): raise #NP.
         * Error code: (selector_index << 3) | IDT=1, EXT=0.
         */
        if (!(gate_type & 0x80))
            raise_exception_err(s, EXCP_NP, (gate_sel & ~7) | 2);

        esp = (uint32_t)s->regs[4];
        esp -= 4; vmem_write32(s, LIN_ADDR(X86_CPU_SEG_SS, esp), s->eflags | EF_FIXED);
        esp -= 4; vmem_write32(s, LIN_ADDR(X86_CPU_SEG_SS, esp), s->segs[X86_CPU_SEG_CS].sel);
        esp -= 4; vmem_write32(s, LIN_ADDR(X86_CPU_SEG_SS, esp), (uint32_t)s->rip);
        if (has_error_code) {
            esp -= 4; vmem_write32(s, LIN_ADDR(X86_CPU_SEG_SS, esp), error_code);
        }
        s->regs[4] = (s->regs[4] & 0xFFFFFFFF00000000ULL) | esp;

        load_seg_desc(s, X86_CPU_SEG_CS, gate_sel);
        s->rip = gate_offset;
        if ((gate_type & 0xF) == 0xE)
            s->eflags &= ~EF_IF;
        s->eflags &= ~(EF_TF | EF_NT | EF_RF);
    }
}

static void __attribute__((noreturn)) raise_exception(X86CPUState *s, int intno)
{
    s->exception_num = intno;
    s->exception_has_error_code = FALSE;
    longjmp(s->jmp_env, 1);
}

static void __attribute__((noreturn))
raise_exception_err(X86CPUState *s, int intno, uint32_t err)
{
    s->exception_num = intno;
    s->exception_error_code = err;
    s->exception_has_error_code = TRUE;
    longjmp(s->jmp_env, 1);
}
/* ------------------------------------------------------------------
 * Instruction decode state with REX prefix support
 * ------------------------------------------------------------------ */

typedef struct {
    X86CPUState *cpu;
    uint64_t     pc;       /* current decode position (linear/virtual addr) */
    BOOL         op32;     /* operand size: 1=32-bit, 0=16-bit */
    BOOL         op64;     /* REX.W: 1=64-bit operand */
    BOOL         addr32;   /* address size: 1=32-bit, 0=16-bit (always 64 in long mode) */
    BOOL         addr64;   /* 1=64-bit address (long mode) */
    int          seg_ovr;  /* segment override, -1=none */
    BOOL         rep;
    BOOL         repne;
    int          rex_r;    /* REX.R: extends ModRM.reg */
    int          rex_x;    /* REX.X: extends SIB.index */
    int          rex_b;    /* REX.B: extends ModRM.rm / SIB.base / opcode reg */
    BOOL         has_rex;  /* REX prefix was seen */

    /*
     * Instruction-fetch cache: avoids a TLB lookup for every byte of an
     * instruction.  Invalidated at the start of each instruction dispatch
     * (see do_interp).  fetch_page == ~0ULL means invalid.
     */
    uint64_t     fetch_page;   /* vaddr & ~PAGE_MASK of the cached code page */
    uint64_t     fetch_paddr;  /* physical page base address */
    uint8_t     *fetch_ptr;    /* host pointer to fetch_paddr (NULL = I/O region) */
} DecodeState;

/* Slow path: cross-page or cold fetch — fills the fetch cache then returns byte. */
static NOINLINE uint8_t fetch_byte_slow(DecodeState *ds, uint64_t vaddr)
{
    X86CPUState *s = ds->cpu;
    uint64_t paddr;
    if (likely(s->cr0 & CR0_PG))
        paddr = virt_to_phys(s, vaddr, FALSE);
    else
        paddr = vaddr;

    uint64_t phys_page = paddr & ~(uint64_t)PAGE_MASK;
    uint8_t *hp = phys_mem_get_ram_ptr(s->mem_map, phys_page, FALSE);
    ds->fetch_page  = vaddr & ~(uint64_t)PAGE_MASK;
    ds->fetch_paddr = phys_page;
    ds->fetch_ptr   = hp;
    if (likely(hp))
        return hp[paddr & PAGE_MASK];
    /* I/O-mapped code page (extremely unusual) */
    return phys_read8(s, paddr);
}

/* Hot path: return the byte at ds->pc and advance it by one. */
ALWAYS_INLINE uint8_t fetch_byte(DecodeState *ds)
{
    uint64_t vaddr = ds->pc++;
    uint64_t page  = vaddr & ~(uint64_t)PAGE_MASK;
    if (likely(page == ds->fetch_page) && likely(ds->fetch_ptr))
        return ds->fetch_ptr[vaddr & PAGE_MASK];
    return fetch_byte_slow(ds, vaddr);
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

static uint64_t fetch_qword(DecodeState *ds)
{
    uint64_t lo = fetch_dword(ds);
    uint64_t hi = fetch_dword(ds);
    return lo | (hi << 32);
}

static int32_t fetch_imm8s(DecodeState *ds)
{
    return (int32_t)(int8_t)fetch_byte(ds);
}

/* ------------------------------------------------------------------
 * ModRM / SIB / effective address decoding
 * ------------------------------------------------------------------ */

static int default_seg_for_rm(int rm)
{
    if (rm == 4 || rm == 5) return X86_CPU_SEG_SS;
    return X86_CPU_SEG_DS;
}

/*
 * Decode ModRM byte, returning:
 *   *reg_idx  = ModRM.reg + rex_r extension
 *   *rm_reg   = register index if mod==3, else -1 (memory)
 *   *ea       = effective address (valid when rm_reg == -1)
 *   *ea_seg   = segment for memory access
 */
static void decode_modrm(DecodeState *ds, int *reg_idx,
                          int *rm_reg, uint64_t *ea, int *ea_seg)
{
    X86CPUState *s = ds->cpu;
    uint8_t modrm = fetch_byte(ds);
    int mod = (modrm >> 6) & 3;
    int reg = ((modrm >> 3) & 7) | (ds->rex_r << 3);
    int rm  = (modrm & 7) | (ds->rex_b << 3);
    int seg = (ds->seg_ovr >= 0) ? ds->seg_ovr : X86_CPU_SEG_DS;
    uint64_t eff_addr = 0;

    *reg_idx = reg;

    if (mod == 3) {
        *rm_reg = rm;
        *ea     = 0;
        *ea_seg = seg;
        return;
    }
    *rm_reg = -1;

    if (ds->addr64) {
        /* 64-bit addressing */
        if ((rm & 7) == 4) {
            /* SIB byte */
            uint8_t sib   = fetch_byte(ds);
            int scale = 1 << ((sib >> 6) & 3);
            int idx   = ((sib >> 3) & 7) | (ds->rex_x << 3);
            int base  = (sib & 7)        | (ds->rex_b << 3);
            if ((base & 7) == 5 && mod == 0) {
                eff_addr = (uint64_t)(int64_t)(int32_t)fetch_dword(ds);
            } else {
                eff_addr = s->regs[base];
                if ((base & 7) == 4 || (base & 7) == 5)
                    seg = (ds->seg_ovr >= 0) ? ds->seg_ovr : X86_CPU_SEG_SS;
            }
            if (idx != 4)
                eff_addr += s->regs[idx] * (uint64_t)scale;
        } else if ((rm & 7) == 5 && mod == 0) {
            /* RIP-relative addressing */
            int32_t disp = (int32_t)fetch_dword(ds);
            eff_addr = ds->pc + (int64_t)disp;
            seg = (ds->seg_ovr >= 0) ? ds->seg_ovr : X86_CPU_SEG_DS;
        } else {
            eff_addr = s->regs[rm];
            seg = (ds->seg_ovr >= 0) ? ds->seg_ovr : default_seg_for_rm(rm & 7);
        }
        if (mod == 1)
            eff_addr += (int64_t)(int8_t)fetch_byte(ds);
        else if (mod == 2)
            eff_addr += (int64_t)(int32_t)fetch_dword(ds);
    } else if (ds->addr32) {
        /* 32-bit addressing */
        if (rm == 4) {
            uint8_t sib = fetch_byte(ds);
            int scale = 1 << ((sib >> 6) & 3);
            int idx   = (sib >> 3) & 7;
            int base  = sib & 7;
            if (base == 5 && mod == 0) {
                eff_addr = (uint64_t)(uint32_t)fetch_dword(ds);
            } else {
                eff_addr = (uint32_t)s->regs[base];
                if (base == 4 || base == 5)
                    seg = (ds->seg_ovr >= 0) ? ds->seg_ovr : X86_CPU_SEG_SS;
            }
            if (idx != 4)
                eff_addr += (uint32_t)s->regs[idx] * (uint32_t)scale;
        } else if (rm == 5 && mod == 0) {
            eff_addr = (uint64_t)(uint32_t)fetch_dword(ds);
            seg = (ds->seg_ovr >= 0) ? ds->seg_ovr : X86_CPU_SEG_DS;
        } else {
            eff_addr = (uint32_t)s->regs[rm];
            seg = (ds->seg_ovr >= 0) ? ds->seg_ovr : default_seg_for_rm(rm);
        }
        if (mod == 1)
            eff_addr = (uint32_t)(eff_addr + (uint32_t)(int32_t)(int8_t)fetch_byte(ds));
        else if (mod == 2)
            eff_addr = (uint32_t)(eff_addr + fetch_dword(ds));
    } else {
        /* 16-bit addressing */
        int32_t disp = 0;
        switch (rm) {
        case 0: eff_addr = s->regs[3] + s->regs[6]; seg = X86_CPU_SEG_DS; break;
        case 1: eff_addr = s->regs[3] + s->regs[7]; seg = X86_CPU_SEG_DS; break;
        case 2: eff_addr = s->regs[5] + s->regs[6]; seg = X86_CPU_SEG_SS; break;
        case 3: eff_addr = s->regs[5] + s->regs[7]; seg = X86_CPU_SEG_SS; break;
        case 4: eff_addr = s->regs[6];               seg = X86_CPU_SEG_DS; break;
        case 5: eff_addr = s->regs[7];               seg = X86_CPU_SEG_DS; break;
        case 6: if (mod == 0) { eff_addr = fetch_word(ds); seg = X86_CPU_SEG_DS; break; }
                eff_addr = s->regs[5]; seg = X86_CPU_SEG_SS; break;
        case 7: eff_addr = s->regs[0]; seg = X86_CPU_SEG_DS; break;
        default: break;
        }
        if (mod == 1) disp = (int32_t)(int8_t)fetch_byte(ds);
        else if (mod == 2) disp = (int32_t)(int16_t)fetch_word(ds);
        eff_addr = (eff_addr + (uint64_t)(uint32_t)disp) & 0xFFFF;
        if (ds->seg_ovr >= 0) seg = ds->seg_ovr;
    }
    *ea     = eff_addr;
    *ea_seg = seg;
}

static uint64_t seg_ea(X86CPUState *s, uint64_t ea, int seg)
{
    return (uint64_t)s->segs[seg].base + ea;
}

/* ------------------------------------------------------------------
 * Register helpers (8/16/32/64-bit)
 * ------------------------------------------------------------------ */

/* 8-bit: AH/BH/CH/DH for regs 4-7 WITHOUT REX; with REX uses SPL/BPL/SIL/DIL */
static uint8_t get_reg8(X86CPUState *s, int r, BOOL has_rex)
{
    if (!has_rex && r >= 4 && r <= 7)
        return (uint8_t)(s->regs[r - 4] >> 8); /* AH/CH/DH/BH */
    return (uint8_t)s->regs[r];
}

static void set_reg8(X86CPUState *s, int r, uint8_t v, BOOL has_rex)
{
    if (!has_rex && r >= 4 && r <= 7)
        s->regs[r - 4] = (s->regs[r - 4] & ~0xFF00ULL) | ((uint64_t)v << 8);
    else
        s->regs[r] = (s->regs[r] & ~0xFFULL) | v;
}

static uint16_t get_reg16(X86CPUState *s, int r) { return (uint16_t)s->regs[r]; }
static void set_reg16(X86CPUState *s, int r, uint16_t v)
{
    /* Writing 16-bit register does NOT zero-extend to 32/64 bits */
    s->regs[r] = (s->regs[r] & ~0xFFFFULL) | v;
}

static uint32_t get_reg32(X86CPUState *s, int r) { return (uint32_t)s->regs[r]; }
static void set_reg32(X86CPUState *s, int r, uint32_t v)
{
    /* Writing 32-bit register ZERO-EXTENDS to 64 bits (x86-64 ABI) */
    s->regs[r] = (uint64_t)v;
}

static uint64_t get_reg64(X86CPUState *s, int r) { return s->regs[r]; }
static void set_reg64(X86CPUState *s, int r, uint64_t v) { s->regs[r] = v; }
/* ------------------------------------------------------------------
 * Shift / rotate helpers
 * ------------------------------------------------------------------ */

static uint8_t do_shift8(X86CPUState *s, int op, uint8_t a, int cnt)
{
    uint32_t cf = (s->eflags & EF_CF) ? 1 : 0;
    int c = cnt & 31;
    if (c == 0) return a;
    uint8_t r;
    s->eflags &= ~(EF_CF | EF_OF);
    switch (op) {
    case 4: case 6: /* SHL/SAL */
        r = (uint8_t)(((uint8_t)a) << c);
        if (((uint8_t)(a >> (8-c))) & 1) s->eflags |= EF_CF;
        if (c == 1 && !!((r >> 7) & 1) != !!(s->eflags & EF_CF)) s->eflags |= EF_OF;
        break;
    case 5: /* SHR */
        r = (uint8_t)(a >> c);
        if ((a >> (c-1)) & 1) s->eflags |= EF_CF;
        if (c == 1 && (a >> 7) & 1) s->eflags |= EF_OF;
        break;
    case 7: /* SAR */
        r = (uint8_t)((uint8_t)((int8_t)a) >> c);
        if (((uint64_t)a >> (c-1)) & 1) s->eflags |= EF_CF;
        break;
    case 0: /* ROL */
        r = (uint8_t)((a << c) | (a >> (8-c)));
        if (r & 1) s->eflags |= EF_CF;
        if (c == 1 && !!((r >> 7) & 1) != !!(s->eflags & EF_CF)) s->eflags |= EF_OF;
        break;
    case 1: /* ROR */
        r = (uint8_t)((a >> c) | (a << (8-c)));
        if ((r >> 7) & 1) s->eflags |= EF_CF;
        if (c == 1 && !!(r >> 7 & 1) != !!((r >> 6) & 1)) s->eflags |= EF_OF;
        break;
    case 2: /* RCL */
        { int fc = cf; r = (uint8_t)((a << c) | (fc << (c-1)));
          for (int i = c; i < 8+1; i++) r |= (uint8_t)((a >> (8+1-i-1)) << (8+1-i-1)); }
        /* simplified: approximate RCL/RCR for small counts */
        r = (uint8_t)(((uint64_t)a << c) | (cf << (c-1)) | (a >> (8+1-c)));
        if (((uint8_t)(a >> (8-c))) & 1) s->eflags |= EF_CF;
        break;
    case 3: /* RCR */
        r = (uint8_t)((a >> c) | ((uint64_t)cf << (8-c)) | (a << (8+1-c)));
        if ((a >> (c-1)) & 1) s->eflags |= EF_CF;
        break;
    default: r = a; break;
    }
    s->eflags &= ~(EF_PF|EF_ZF|EF_SF);
    s->eflags |= compute_flags_pzsb8(r);
    return r;
}

static uint16_t do_shift16(X86CPUState *s, int op, uint16_t a, int cnt)
{
    uint32_t cf = (s->eflags & EF_CF) ? 1 : 0;
    int c = cnt & 31;
    if (c == 0) return a;
    uint16_t r;
    s->eflags &= ~(EF_CF | EF_OF);
    switch (op) {
    case 4: case 6: /* SHL/SAL */
        r = (uint16_t)(((uint16_t)a) << c);
        if (((uint16_t)(a >> (16-c))) & 1) s->eflags |= EF_CF;
        if (c == 1 && !!((r >> 15) & 1) != !!(s->eflags & EF_CF)) s->eflags |= EF_OF;
        break;
    case 5: /* SHR */
        r = (uint16_t)(a >> c);
        if ((a >> (c-1)) & 1) s->eflags |= EF_CF;
        if (c == 1 && (a >> 15) & 1) s->eflags |= EF_OF;
        break;
    case 7: /* SAR */
        r = (uint16_t)((uint16_t)((int16_t)a) >> c);
        if (((uint64_t)a >> (c-1)) & 1) s->eflags |= EF_CF;
        break;
    case 0: /* ROL */
        r = (uint16_t)((a << c) | (a >> (16-c)));
        if (r & 1) s->eflags |= EF_CF;
        if (c == 1 && !!((r >> 15) & 1) != !!(s->eflags & EF_CF)) s->eflags |= EF_OF;
        break;
    case 1: /* ROR */
        r = (uint16_t)((a >> c) | (a << (16-c)));
        if ((r >> 15) & 1) s->eflags |= EF_CF;
        if (c == 1 && !!(r >> 15 & 1) != !!((r >> 14) & 1)) s->eflags |= EF_OF;
        break;
    case 2: /* RCL */
        { int fc = cf; r = (uint16_t)((a << c) | (fc << (c-1)));
          for (int i = c; i < 16+1; i++) r |= (uint16_t)((a >> (16+1-i-1)) << (16+1-i-1)); }
        /* simplified: approximate RCL/RCR for small counts */
        r = (uint16_t)(((uint64_t)a << c) | (cf << (c-1)) | (a >> (16+1-c)));
        if (((uint16_t)(a >> (16-c))) & 1) s->eflags |= EF_CF;
        break;
    case 3: /* RCR */
        r = (uint16_t)((a >> c) | ((uint64_t)cf << (16-c)) | (a << (16+1-c)));
        if ((a >> (c-1)) & 1) s->eflags |= EF_CF;
        break;
    default: r = a; break;
    }
    s->eflags &= ~(EF_PF|EF_ZF|EF_SF);
    s->eflags |= compute_flags_pzs16(r);
    return r;
}

static uint32_t do_shift32(X86CPUState *s, int op, uint32_t a, int cnt)
{
    uint32_t cf = (s->eflags & EF_CF) ? 1 : 0;
    int c = cnt & 31;
    if (c == 0) return a;
    uint32_t r;
    s->eflags &= ~(EF_CF | EF_OF);
    switch (op) {
    case 4: case 6: /* SHL/SAL */
        r = (uint32_t)(((uint32_t)a) << c);
        if (((uint32_t)(a >> (32-c))) & 1) s->eflags |= EF_CF;
        if (c == 1 && !!((r >> 31) & 1) != !!(s->eflags & EF_CF)) s->eflags |= EF_OF;
        break;
    case 5: /* SHR */
        r = (uint32_t)(a >> c);
        if ((a >> (c-1)) & 1) s->eflags |= EF_CF;
        if (c == 1 && (a >> 31) & 1) s->eflags |= EF_OF;
        break;
    case 7: /* SAR */
        r = (uint32_t)((uint32_t)((int32_t)a) >> c);
        if (((uint64_t)a >> (c-1)) & 1) s->eflags |= EF_CF;
        break;
    case 0: /* ROL */
        r = (uint32_t)((a << c) | (a >> (32-c)));
        if (r & 1) s->eflags |= EF_CF;
        if (c == 1 && !!((r >> 31) & 1) != !!(s->eflags & EF_CF)) s->eflags |= EF_OF;
        break;
    case 1: /* ROR */
        r = (uint32_t)((a >> c) | (a << (32-c)));
        if ((r >> 31) & 1) s->eflags |= EF_CF;
        if (c == 1 && !!(r >> 31 & 1) != !!((r >> 30) & 1)) s->eflags |= EF_OF;
        break;
    case 2: /* RCL */
        { int fc = cf; r = (uint32_t)((a << c) | (fc << (c-1)));
          for (int i = c; i < 32+1; i++) r |= (uint32_t)((a >> (32+1-i-1)) << (32+1-i-1)); }
        /* simplified: approximate RCL/RCR for small counts */
        r = (uint32_t)(((uint64_t)a << c) | (cf << (c-1)) | (a >> (32+1-c)));
        if (((uint32_t)(a >> (32-c))) & 1) s->eflags |= EF_CF;
        break;
    case 3: /* RCR */
        r = (uint32_t)((a >> c) | ((uint64_t)cf << (32-c)) | (a << (32+1-c)));
        if ((a >> (c-1)) & 1) s->eflags |= EF_CF;
        break;
    default: r = a; break;
    }
    s->eflags &= ~(EF_PF|EF_ZF|EF_SF);
    s->eflags |= compute_flags_pzs32(r);
    return r;
}

static uint64_t do_shift64(X86CPUState *s, int op, uint64_t a, int cnt)
{
    uint32_t cf = (s->eflags & EF_CF) ? 1 : 0;
    int c = cnt & 63;
    if (c == 0) return a;
    uint64_t r;
    s->eflags &= ~(EF_CF | EF_OF);
    switch (op) {
    case 4: case 6: /* SHL/SAL */
        r = (uint64_t)(((uint64_t)a) << c);
        if (((uint64_t)(a >> (64-c))) & 1) s->eflags |= EF_CF;
        if (c == 1 && !!((r >> 63) & 1) != !!(s->eflags & EF_CF)) s->eflags |= EF_OF;
        break;
    case 5: /* SHR */
        r = (uint64_t)(a >> c);
        if ((a >> (c-1)) & 1) s->eflags |= EF_CF;
        if (c == 1 && (a >> 63) & 1) s->eflags |= EF_OF;
        break;
    case 7: /* SAR */
        r = (uint64_t)((uint64_t)((int64_t)a) >> c);
        if (((uint64_t)a >> (c-1)) & 1) s->eflags |= EF_CF;
        break;
    case 0: /* ROL */
        r = (uint64_t)((a << c) | (a >> (64-c)));
        if (r & 1) s->eflags |= EF_CF;
        if (c == 1 && !!((r >> 63) & 1) != !!(s->eflags & EF_CF)) s->eflags |= EF_OF;
        break;
    case 1: /* ROR */
        r = (uint64_t)((a >> c) | (a << (64-c)));
        if ((r >> 63) & 1) s->eflags |= EF_CF;
        if (c == 1 && !!(r >> 63 & 1) != !!((r >> 62) & 1)) s->eflags |= EF_OF;
        break;
    case 2: /* RCL */
        { int fc = cf; r = (uint64_t)((a << c) | (fc << (c-1)));
          for (int i = c; i < 64+1; i++) r |= (uint64_t)((a >> (64+1-i-1)) << (64+1-i-1)); }
        /* simplified: approximate RCL/RCR for small counts */
        r = (uint64_t)(((uint64_t)a << c) | (cf << (c-1)) | (a >> (64+1-c)));
        if (((uint64_t)(a >> (64-c))) & 1) s->eflags |= EF_CF;
        break;
    case 3: /* RCR */
        r = (uint64_t)((a >> c) | ((uint64_t)cf << (64-c)) | (a << (64+1-c)));
        if ((a >> (c-1)) & 1) s->eflags |= EF_CF;
        break;
    default: r = a; break;
    }
    s->eflags &= ~(EF_PF|EF_ZF|EF_SF);
    s->eflags |= compute_flags_pzs64(r);
    return r;
}
/* ------------------------------------------------------------------
 * ALU group-1 operations (ADD/OR/ADC/SBB/AND/SUB/XOR/CMP)
 * Returns result (callers must NOT store for CMP, op==7)
 * ------------------------------------------------------------------ */

static uint8_t alu_op8(X86CPUState *s, int op, uint8_t a, uint8_t b)
{
    uint8_t r;
    uint32_t cf = (s->eflags & EF_CF) ? 1 : 0;
    switch(op) {
    case 0: r = a + b;       flags_add8(s, a, b, r);   break;
    case 1: r = a | b;       flags_logic8(s, r);        break;
    case 2: { uint32_t rr = (uint32_t)a + b + cf; r = (uint8_t)rr; flags_add8(s, a, b+cf, r); } break;
    case 3: { uint8_t tmp = b + (uint8_t)cf; r = a - tmp; flags_sub8(s, a, tmp, r); } break;
    case 4: r = a & b;       flags_logic8(s, r);        break;
    case 5: r = a - b;       flags_sub8(s, a, b, r);    break;
    case 6: r = a ^ b;       flags_logic8(s, r);        break;
    case 7: r = a - b;       flags_sub8(s, a, b, r);    break; /* CMP: result discarded */
    default: r = a; break;
    }
    return r;
}

static uint16_t alu_op16(X86CPUState *s, int op, uint16_t a, uint16_t b)
{
    uint16_t r;
    uint32_t cf = (s->eflags & EF_CF) ? 1 : 0;
    switch(op) {
    case 0: r = a + b;       flags_add16(s, a, b, r);   break;
    case 1: r = a | b;       flags_logic16(s, r);        break;
    case 2: { uint32_t rr = (uint32_t)a + b + cf; r = (uint16_t)rr; flags_add16(s, a, b+cf, r); } break;
    case 3: { uint16_t tmp = b + (uint16_t)cf; r = a - tmp; flags_sub16(s, a, tmp, r); } break;
    case 4: r = a & b;       flags_logic16(s, r);        break;
    case 5: r = a - b;       flags_sub16(s, a, b, r);    break;
    case 6: r = a ^ b;       flags_logic16(s, r);        break;
    case 7: r = a - b;       flags_sub16(s, a, b, r);    break; /* CMP: result discarded */
    default: r = a; break;
    }
    return r;
}

static uint32_t alu_op32(X86CPUState *s, int op, uint32_t a, uint32_t b)
{
    uint32_t r;
    uint32_t cf = (s->eflags & EF_CF) ? 1 : 0;
    switch(op) {
    case 0: r = a + b;       flags_add32(s, a, b, r);   break;
    case 1: r = a | b;       flags_logic32(s, r);        break;
    case 2: r = a + b + (uint32_t)cf; flags_adc32(s, a, b, cf, r); break;
    case 3: { uint32_t tmp = b + (uint32_t)cf; r = a - tmp; flags_sub32(s, a, tmp, r); } break;
    case 4: r = a & b;       flags_logic32(s, r);        break;
    case 5: r = a - b;       flags_sub32(s, a, b, r);    break;
    case 6: r = a ^ b;       flags_logic32(s, r);        break;
    case 7: r = a - b;       flags_sub32(s, a, b, r);    break; /* CMP: result discarded */
    default: r = a; break;
    }
    return r;
}

static uint64_t alu_op64(X86CPUState *s, int op, uint64_t a, uint64_t b)
{
    uint64_t r;
    uint32_t cf = (s->eflags & EF_CF) ? 1 : 0;
    switch(op) {
    case 0: r = a + b;       flags_add64(s, a, b, r);   break;
    case 1: r = a | b;       flags_logic64(s, r);        break;
    case 2: r = a + b + (uint64_t)cf; flags_adc64(s, a, b, cf, r); break;
    case 3: { uint64_t tmp = b + (uint64_t)cf; r = a - tmp; flags_sub64(s, a, tmp, r); } break;
    case 4: r = a & b;       flags_logic64(s, r);        break;
    case 5: r = a - b;       flags_sub64(s, a, b, r);    break;
    case 6: r = a ^ b;       flags_logic64(s, r);        break;
    case 7: r = a - b;       flags_sub64(s, a, b, r);    break; /* CMP: result discarded */
    default: r = a; break;
    }
    return r;
}

/* ------------------------------------------------------------------
 * Stack operations (32/64-bit)
 * ------------------------------------------------------------------ */

static void push16(X86CPUState *s, uint16_t v)
{
    uint32_t esp = (uint32_t)s->regs[4] - 2;
    vmem_write16(s, LIN_ADDR(X86_CPU_SEG_SS, esp), v);
    s->regs[4] = (s->regs[4] & 0xFFFFFFFF00000000ULL) | esp;
}

static uint16_t pop16(X86CPUState *s)
{
    uint32_t esp = (uint32_t)s->regs[4];
    uint16_t v = vmem_read16(s, LIN_ADDR(X86_CPU_SEG_SS, esp));
    s->regs[4] = (s->regs[4] & 0xFFFFFFFF00000000ULL) | (uint32_t)(esp + 2);
    return v;
}

static void push32(X86CPUState *s, uint32_t v)
{
    uint32_t esp = (uint32_t)s->regs[4] - 4;
    vmem_write32(s, LIN_ADDR(X86_CPU_SEG_SS, esp), v);
    s->regs[4] = (s->regs[4] & 0xFFFFFFFF00000000ULL) | esp;
}

static uint32_t pop32(X86CPUState *s)
{
    uint32_t esp = (uint32_t)s->regs[4];
    uint32_t v = vmem_read32(s, LIN_ADDR(X86_CPU_SEG_SS, esp));
    s->regs[4] = (s->regs[4] & 0xFFFFFFFF00000000ULL) | (uint32_t)(esp + 4);
    return v;
}

static void push64(X86CPUState *s, uint64_t v)
{
    uint64_t rsp = s->regs[4] - 8;
    vmem_write64(s, s->segs[X86_CPU_SEG_SS].base + rsp, v);
    s->regs[4] = rsp;
}

static uint64_t pop64(X86CPUState *s)
{
    uint64_t rsp = s->regs[4];
    uint64_t v = vmem_read64(s, s->segs[X86_CPU_SEG_SS].base + rsp);
    s->regs[4] = rsp + 8;
    return v;
}

/* ------------------------------------------------------------------
 * Conditional test
 * ------------------------------------------------------------------ */

static int test_cc(X86CPUState *s, int cc)
{
    uint32_t ef = s->eflags;
    switch (cc & 0xE) {
    case 0x0: return !!(ef & EF_OF);                           /* O/NO */
    case 0x2: return !!(ef & EF_CF);                           /* B/NB */
    case 0x4: return !!(ef & EF_ZF);                           /* E/NE */
    case 0x6: return !!(ef & (EF_CF|EF_ZF));                   /* BE/A */
    case 0x8: return !!(ef & EF_SF);                           /* S/NS */
    case 0xA: return !!(ef & EF_PF);                           /* P/NP */
    case 0xC: return !!((ef & EF_SF) ^ ((ef & EF_OF) >> 4));  /* L/GE (SF!=OF) */
    case 0xE: return !!(ef & EF_ZF) ||                         /* LE/G */
                     !!((ef & EF_SF) ^ ((ef & EF_OF) >> 4));
    default:  return 0;
    }
}

/* ------------------------------------------------------------------
 * I/O port helpers
 * ------------------------------------------------------------------ */

static uint32_t port_read(X86CPUState *s, uint32_t port, int sz)
{
    if (s->port_read) return s->port_read(s->port_opaque, port, sz);
    return 0xFFFFFFFFU;
}

static void port_write(X86CPUState *s, uint32_t port, uint32_t val, int sz)
{
    if (s->port_write) s->port_write(s->port_opaque, port, val, sz);
}

/* ------------------------------------------------------------------
 * CPUID
 * ------------------------------------------------------------------ */

static void do_cpuid(X86CPUState *s)
{
    uint32_t eax = (uint32_t)s->regs[0];
    uint32_t ecx = (uint32_t)s->regs[1];
    (void)ecx;
    switch (eax) {
    case 0:
        s->regs[0] = 7;           /* max basic leaf */
        s->regs[3] = 0x756e6547; /* 'Genu' in EBX */
        s->regs[2] = 0x6c65746e; /* 'ntel' in ECX */
        s->regs[1] = 0x49656e69; /* 'ineI' in EDX */
        break;
    case 1:
        s->regs[0] = 0x00000663; /* family 6, model 6, stepping 3 */
        s->regs[3] = 0;
        s->regs[2] = 0; /* ECX: no SSE4/AVX here */
        /* EDX: FPU PSE TSC MSR PAE CX8 APIC SEP CMOV */
        s->regs[1] = (1<<0)|(1<<3)|(1<<4)|(1<<5)|(1<<6)|(1<<8)|(1<<9)|(1<<11)|(1<<15);
        break;
    case 0x80000000:
        s->regs[0] = 0x80000004;
        s->regs[3] = s->regs[2] = s->regs[1] = 0;
        break;
    case 0x80000001:
        s->regs[0] = 0;
        s->regs[1] = 0;
        s->regs[2] = 0;
        /* EDX: LM (64-bit) | NX | SYSCALL */
        s->regs[3] = (1<<29) | (1<<20) | (1<<11);
        break;
    case 0x80000002:
        /* 'TinyEMU x86-64 CP' */
        s->regs[0] = 0x6e696954; s->regs[3] = 0x4d455965;
        s->regs[2] = 0x36387855; s->regs[1] = 0x50202d34;
        break;
    case 0x80000003:
        s->regs[0] = 0x786f7255; s->regs[3] = 0x73736563;
        s->regs[2] = 0x00000072; s->regs[1] = 0;
        break;
    case 0x80000004:
        s->regs[0] = s->regs[1] = s->regs[2] = s->regs[3] = 0;
        break;
    default:
        s->regs[0] = s->regs[1] = s->regs[2] = s->regs[3] = 0;
        break;
    }
}
/* ------------------------------------------------------------------
 * Two-byte opcodes (0F prefix)
 * ------------------------------------------------------------------ */

static void exec_0f(DecodeState *ds)
{
    X86CPUState *s = ds->cpu;
    uint8_t op2 = fetch_byte(ds);
    int reg, rm_reg;
    uint64_t ea;
    int ea_seg;

    switch (op2) {
    case 0x01: { /* GROUP 7: SGDT/SIDT/LGDT/LIDT/SMSW/LMSW/INVLPG */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint64_t laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        BOOL lm = is_long_mode(s);
        switch (reg) {
        case 0: { /* SGDT */
            if (rm_reg >= 0) raise_exception(s, EXCP_UD);
            vmem_write16(s, laddr, (uint16_t)s->segs[X86_CPU_SEG_GDT].limit);
            if (lm) vmem_write64(s, laddr + 2, s->segs[X86_CPU_SEG_GDT].base);
            else    vmem_write32(s, laddr + 2, (uint32_t)s->segs[X86_CPU_SEG_GDT].base);
            break; }
        case 1: { /* SIDT */
            if (rm_reg >= 0) raise_exception(s, EXCP_UD);
            vmem_write16(s, laddr, (uint16_t)s->segs[X86_CPU_SEG_IDT].limit);
            if (lm) vmem_write64(s, laddr + 2, s->segs[X86_CPU_SEG_IDT].base);
            else    vmem_write32(s, laddr + 2, (uint32_t)s->segs[X86_CPU_SEG_IDT].base);
            break; }
        case 2: { /* LGDT */
            if (rm_reg >= 0) raise_exception(s, EXCP_UD);
            s->segs[X86_CPU_SEG_GDT].limit = vmem_read16(s, laddr);
            if (lm) s->segs[X86_CPU_SEG_GDT].base = vmem_read64(s, laddr + 2);
            else    s->segs[X86_CPU_SEG_GDT].base = vmem_read32(s, laddr + 2);
            break; }
        case 3: { /* LIDT */
            if (rm_reg >= 0) raise_exception(s, EXCP_UD);
            s->segs[X86_CPU_SEG_IDT].limit = vmem_read16(s, laddr);
            if (lm) s->segs[X86_CPU_SEG_IDT].base = vmem_read64(s, laddr + 2);
            else    s->segs[X86_CPU_SEG_IDT].base = vmem_read32(s, laddr + 2);
            break; }
        case 4: { /* SMSW */
            uint16_t msw = (uint16_t)(s->cr0 & 0xFFFF);
            if (rm_reg >= 0) set_reg16(s, rm_reg, msw);
            else vmem_write16(s, laddr, msw);
            break; }
        case 6: { /* LMSW */
            uint16_t msw = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            s->cr0 = (s->cr0 & 0xFFFF0010U) | (msw & 0xF);
            break; }
        case 7: { /* INVLPG */
            if (rm_reg >= 0) raise_exception(s, EXCP_UD);
            tlb_flush_all(s);
            break; }
        default: raise_exception(s, EXCP_UD);
        }
        break; }

    case 0x05: { /* SYSCALL (64-bit) */
        if (!(s->msr_efer & EFER_SCE)) raise_exception(s, EXCP_UD);
        /* Save return state: RCX=RIP, R11=RFLAGS */
        s->regs[1] = ds->pc; /* RCX = next RIP (after SYSCALL) */
        s->regs[11] = s->eflags;
        /* Load CS/SS from STAR MSR */
        uint16_t cs_sel = (uint16_t)(s->msr_star >> 32) & 0xFFFC;
        uint16_t ss_sel = (uint16_t)((s->msr_star >> 32) + 8) & 0xFFFF;
        s->segs[X86_CPU_SEG_CS].sel   = cs_sel;
        s->segs[X86_CPU_SEG_CS].base  = 0;
        s->segs[X86_CPU_SEG_CS].limit = 0xFFFFFFFF;
        s->segs[X86_CPU_SEG_CS].flags = 0xa09b; /* 64-bit code: L=1,G=1,P=1,DPL=0,S=1,type=0xb */
        s->segs[X86_CPU_SEG_SS].sel   = ss_sel;
        s->segs[X86_CPU_SEG_SS].base  = 0;
        s->segs[X86_CPU_SEG_SS].limit = 0xFFFFFFFF;
        s->segs[X86_CPU_SEG_SS].flags = 0xc093;
        /* RFLAGS mask */
        s->eflags &= ~(s->msr_syscall_mask | EF_RF);
        s->eflags &= ~EF_IF;
        /* Jump to LSTAR */
        s->rip = s->msr_lstar;
        break; }

    case 0x07: { /* SYSRET (64-bit) */
        if (!(s->msr_efer & EFER_SCE)) raise_exception(s, EXCP_UD);
        /* Restore RFLAGS from R11, RIP from RCX */
        s->eflags = (uint32_t)s->regs[11] | EF_FIXED;
        s->rip = s->regs[1]; /* RCX */
        /* Restore CS/SS (ring 3) from STAR[63:48] */
        if (ds->op64) {
            uint16_t cs_s = (uint16_t)((s->msr_star >> 48) | 3);
            s->segs[X86_CPU_SEG_CS].sel   = cs_s;
            s->segs[X86_CPU_SEG_CS].base  = 0;
            s->segs[X86_CPU_SEG_CS].limit = 0xFFFFFFFF;
            s->segs[X86_CPU_SEG_CS].flags = 0xa0fb; /* 64-bit user code */
            s->segs[X86_CPU_SEG_SS].sel   = (uint16_t)((s->msr_star >> 48) + 8) | 3;
            s->segs[X86_CPU_SEG_SS].base  = 0;
            s->segs[X86_CPU_SEG_SS].limit = 0xFFFFFFFF;
            s->segs[X86_CPU_SEG_SS].flags = 0xc0f3;
        } else {
            uint16_t cs_s = (uint16_t)((s->msr_star >> 48) - 16) | 3;
            s->segs[X86_CPU_SEG_CS].sel   = cs_s;
            s->segs[X86_CPU_SEG_CS].flags = 0xc0fb;
            s->segs[X86_CPU_SEG_SS].sel   = (uint16_t)((s->msr_star >> 48) - 8) | 3;
            s->segs[X86_CPU_SEG_SS].flags = 0xc0f3;
        }
        break; }

    case 0x06: /* CLTS */
        s->cr0 &= ~(1U << 3);
        break;

    case 0x20: { /* MOV r, CRn */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        if (rm_reg < 0) rm_reg = reg; /* rm field is the register in this encoding */
        /* The ModRM byte has rm in the r/m field. Reparse: reg=crn, rm=gpr */
        /* Already decoded: reg=crn index, rm_reg=gpr (or we must re-read) */
        /* For MOV CRn, the hardware encoding is ModRM with mod=3 always */
        uint64_t crval;
        switch (reg) {
        case 0: crval = s->cr0; break;
        case 2: crval = s->cr2; break;
        case 3: crval = s->cr3; break;
        case 4: crval = s->cr4; break;
        default: raise_exception(s, EXCP_UD); crval = 0; break;
        }
        if (ds->op64 || is_long_mode(s)) set_reg64(s, rm_reg, crval);
        else set_reg32(s, rm_reg, (uint32_t)crval);
        break; }

    case 0x22: { /* MOV CRn, r */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        if (rm_reg < 0) rm_reg = reg;
        uint64_t val = (ds->op64 || is_long_mode(s)) ? get_reg64(s, rm_reg) : get_reg32(s, rm_reg);
        switch (reg) {
        case 0: {
            uint32_t old = s->cr0;
            s->cr0 = (uint32_t)val;
            /* Long mode activation: PG being set with EFER.LME=1 and CR4.PAE=1 */
            if ((s->cr0 & CR0_PG) && !(old & CR0_PG) &&
                (s->msr_efer & EFER_LME) && (s->cr4 & CR4_PAE)) {
                s->msr_efer |= EFER_LMA;
            } else if (!(s->cr0 & CR0_PG) && (old & CR0_PG)) {
                s->msr_efer &= ~EFER_LMA;
            }
            if ((val ^ old) & CR0_PG) tlb_flush_all(s);
            break; }
        case 2: s->cr2 = (uint32_t)val; break;
        case 3: s->cr3 = val; tlb_flush_all(s); break;
        case 4:
            s->cr4 = (uint32_t)val;
            tlb_flush_all(s);
            break;
        default: raise_exception(s, EXCP_UD); break;
        }
        break; }

    case 0x23: { /* MOV DRn, r - ignore */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        (void)reg; (void)rm_reg;
        break; }

    case 0x30: { /* WRMSR */
        uint32_t msr = (uint32_t)s->regs[1]; /* ECX */
        uint64_t val = ((uint64_t)(uint32_t)s->regs[2] << 32) | (uint32_t)s->regs[0];
        switch (msr) {
        case 0x174: s->sysenter_cs  = (uint32_t)val; break;
        case 0x175: s->sysenter_esp = val; break;
        case 0x176: s->sysenter_eip = val; break;
        case 0xC0000080: {
            uint64_t old = s->msr_efer;
            s->msr_efer = val;
            /* If LME was just cleared, also clear LMA */
            if (!(val & EFER_LME) && (old & EFER_LME))
                s->msr_efer &= ~EFER_LMA;
            break; }
        case 0xC0000081: s->msr_star = val; break;
        case 0xC0000082: s->msr_lstar = val; break;
        case 0xC0000083: s->msr_cstar = val; break;
        case 0xC0000084: s->msr_syscall_mask = (uint32_t)val; break;
        case 0xC0000100: s->msr_fs_base = val;
            s->segs[X86_CPU_SEG_FS].base = val; break;
        case 0xC0000101: s->msr_gs_base = val;
            s->segs[X86_CPU_SEG_GS].base = val; break;
        case 0xC0000102: s->msr_kernel_gs_base = val; break;
        default: break;
        }
        break; }

    case 0x31: { /* RDTSC */
        uint64_t tsc = s->get_tsc ? s->get_tsc(s->get_tsc_opaque) : (uint64_t)s->cycle_count;
        set_reg32(s, 0, (uint32_t)tsc);
        set_reg32(s, 2, (uint32_t)(tsc >> 32));
        break; }

    case 0x32: { /* RDMSR */
        uint32_t msr = (uint32_t)s->regs[1];
        uint64_t val = 0;
        switch (msr) {
        case 0x174: val = s->sysenter_cs; break;
        case 0x175: val = s->sysenter_esp; break;
        case 0x176: val = s->sysenter_eip; break;
        case 0xC0000080: val = s->msr_efer; break;
        case 0xC0000081: val = s->msr_star; break;
        case 0xC0000082: val = s->msr_lstar; break;
        case 0xC0000083: val = s->msr_cstar; break;
        case 0xC0000084: val = s->msr_syscall_mask; break;
        case 0xC0000100: val = s->msr_fs_base; break;
        case 0xC0000101: val = s->msr_gs_base; break;
        case 0xC0000102: val = s->msr_kernel_gs_base; break;
        default: break;
        }
        set_reg32(s, 0, (uint32_t)val);
        set_reg32(s, 2, (uint32_t)(val >> 32));
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
        s->rip     = s->sysenter_eip;
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
        s->rip     = s->regs[1]; /* EIP = ECX */
        break; }

    case 0x40: { /* CMOVcc r, r/m (0x0) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        if (test_cc(s, 0x0)) {
            if (ds->op64) {
                uint64_t src = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, seg_ea(s, ea, ea_seg));
                set_reg64(s, reg, src);
            } else if (ds->op32) {
                uint32_t src = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, seg_ea(s, ea, ea_seg));
                set_reg32(s, reg, src);
            } else {
                uint16_t src = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, seg_ea(s, ea, ea_seg));
                set_reg16(s, reg, src);
            }
        }
        break; }
    case 0x41: { /* CMOVcc r, r/m (0x1) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        if (test_cc(s, 0x1)) {
            if (ds->op64) {
                uint64_t src = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, seg_ea(s, ea, ea_seg));
                set_reg64(s, reg, src);
            } else if (ds->op32) {
                uint32_t src = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, seg_ea(s, ea, ea_seg));
                set_reg32(s, reg, src);
            } else {
                uint16_t src = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, seg_ea(s, ea, ea_seg));
                set_reg16(s, reg, src);
            }
        }
        break; }
    case 0x42: { /* CMOVcc r, r/m (0x2) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        if (test_cc(s, 0x2)) {
            if (ds->op64) {
                uint64_t src = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, seg_ea(s, ea, ea_seg));
                set_reg64(s, reg, src);
            } else if (ds->op32) {
                uint32_t src = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, seg_ea(s, ea, ea_seg));
                set_reg32(s, reg, src);
            } else {
                uint16_t src = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, seg_ea(s, ea, ea_seg));
                set_reg16(s, reg, src);
            }
        }
        break; }
    case 0x43: { /* CMOVcc r, r/m (0x3) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        if (test_cc(s, 0x3)) {
            if (ds->op64) {
                uint64_t src = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, seg_ea(s, ea, ea_seg));
                set_reg64(s, reg, src);
            } else if (ds->op32) {
                uint32_t src = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, seg_ea(s, ea, ea_seg));
                set_reg32(s, reg, src);
            } else {
                uint16_t src = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, seg_ea(s, ea, ea_seg));
                set_reg16(s, reg, src);
            }
        }
        break; }
    case 0x44: { /* CMOVcc r, r/m (0x4) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        if (test_cc(s, 0x4)) {
            if (ds->op64) {
                uint64_t src = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, seg_ea(s, ea, ea_seg));
                set_reg64(s, reg, src);
            } else if (ds->op32) {
                uint32_t src = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, seg_ea(s, ea, ea_seg));
                set_reg32(s, reg, src);
            } else {
                uint16_t src = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, seg_ea(s, ea, ea_seg));
                set_reg16(s, reg, src);
            }
        }
        break; }
    case 0x45: { /* CMOVcc r, r/m (0x5) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        if (test_cc(s, 0x5)) {
            if (ds->op64) {
                uint64_t src = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, seg_ea(s, ea, ea_seg));
                set_reg64(s, reg, src);
            } else if (ds->op32) {
                uint32_t src = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, seg_ea(s, ea, ea_seg));
                set_reg32(s, reg, src);
            } else {
                uint16_t src = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, seg_ea(s, ea, ea_seg));
                set_reg16(s, reg, src);
            }
        }
        break; }
    case 0x46: { /* CMOVcc r, r/m (0x6) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        if (test_cc(s, 0x6)) {
            if (ds->op64) {
                uint64_t src = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, seg_ea(s, ea, ea_seg));
                set_reg64(s, reg, src);
            } else if (ds->op32) {
                uint32_t src = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, seg_ea(s, ea, ea_seg));
                set_reg32(s, reg, src);
            } else {
                uint16_t src = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, seg_ea(s, ea, ea_seg));
                set_reg16(s, reg, src);
            }
        }
        break; }
    case 0x47: { /* CMOVcc r, r/m (0x7) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        if (test_cc(s, 0x7)) {
            if (ds->op64) {
                uint64_t src = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, seg_ea(s, ea, ea_seg));
                set_reg64(s, reg, src);
            } else if (ds->op32) {
                uint32_t src = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, seg_ea(s, ea, ea_seg));
                set_reg32(s, reg, src);
            } else {
                uint16_t src = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, seg_ea(s, ea, ea_seg));
                set_reg16(s, reg, src);
            }
        }
        break; }
    case 0x48: { /* CMOVcc r, r/m (0x8) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        if (test_cc(s, 0x8)) {
            if (ds->op64) {
                uint64_t src = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, seg_ea(s, ea, ea_seg));
                set_reg64(s, reg, src);
            } else if (ds->op32) {
                uint32_t src = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, seg_ea(s, ea, ea_seg));
                set_reg32(s, reg, src);
            } else {
                uint16_t src = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, seg_ea(s, ea, ea_seg));
                set_reg16(s, reg, src);
            }
        }
        break; }
    case 0x49: { /* CMOVcc r, r/m (0x9) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        if (test_cc(s, 0x9)) {
            if (ds->op64) {
                uint64_t src = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, seg_ea(s, ea, ea_seg));
                set_reg64(s, reg, src);
            } else if (ds->op32) {
                uint32_t src = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, seg_ea(s, ea, ea_seg));
                set_reg32(s, reg, src);
            } else {
                uint16_t src = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, seg_ea(s, ea, ea_seg));
                set_reg16(s, reg, src);
            }
        }
        break; }
    case 0x4A: { /* CMOVcc r, r/m (0xa) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        if (test_cc(s, 0xA)) {
            if (ds->op64) {
                uint64_t src = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, seg_ea(s, ea, ea_seg));
                set_reg64(s, reg, src);
            } else if (ds->op32) {
                uint32_t src = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, seg_ea(s, ea, ea_seg));
                set_reg32(s, reg, src);
            } else {
                uint16_t src = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, seg_ea(s, ea, ea_seg));
                set_reg16(s, reg, src);
            }
        }
        break; }
    case 0x4B: { /* CMOVcc r, r/m (0xb) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        if (test_cc(s, 0xB)) {
            if (ds->op64) {
                uint64_t src = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, seg_ea(s, ea, ea_seg));
                set_reg64(s, reg, src);
            } else if (ds->op32) {
                uint32_t src = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, seg_ea(s, ea, ea_seg));
                set_reg32(s, reg, src);
            } else {
                uint16_t src = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, seg_ea(s, ea, ea_seg));
                set_reg16(s, reg, src);
            }
        }
        break; }
    case 0x4C: { /* CMOVcc r, r/m (0xc) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        if (test_cc(s, 0xC)) {
            if (ds->op64) {
                uint64_t src = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, seg_ea(s, ea, ea_seg));
                set_reg64(s, reg, src);
            } else if (ds->op32) {
                uint32_t src = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, seg_ea(s, ea, ea_seg));
                set_reg32(s, reg, src);
            } else {
                uint16_t src = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, seg_ea(s, ea, ea_seg));
                set_reg16(s, reg, src);
            }
        }
        break; }
    case 0x4D: { /* CMOVcc r, r/m (0xd) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        if (test_cc(s, 0xD)) {
            if (ds->op64) {
                uint64_t src = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, seg_ea(s, ea, ea_seg));
                set_reg64(s, reg, src);
            } else if (ds->op32) {
                uint32_t src = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, seg_ea(s, ea, ea_seg));
                set_reg32(s, reg, src);
            } else {
                uint16_t src = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, seg_ea(s, ea, ea_seg));
                set_reg16(s, reg, src);
            }
        }
        break; }
    case 0x4E: { /* CMOVcc r, r/m (0xe) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        if (test_cc(s, 0xE)) {
            if (ds->op64) {
                uint64_t src = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, seg_ea(s, ea, ea_seg));
                set_reg64(s, reg, src);
            } else if (ds->op32) {
                uint32_t src = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, seg_ea(s, ea, ea_seg));
                set_reg32(s, reg, src);
            } else {
                uint16_t src = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, seg_ea(s, ea, ea_seg));
                set_reg16(s, reg, src);
            }
        }
        break; }
    case 0x4F: { /* CMOVcc r, r/m (0xf) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        if (test_cc(s, 0xF)) {
            if (ds->op64) {
                uint64_t src = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, seg_ea(s, ea, ea_seg));
                set_reg64(s, reg, src);
            } else if (ds->op32) {
                uint32_t src = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, seg_ea(s, ea, ea_seg));
                set_reg32(s, reg, src);
            } else {
                uint16_t src = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, seg_ea(s, ea, ea_seg));
                set_reg16(s, reg, src);
            }
        }
        break; }
    case 0x80: { /* Jcc near (0x0) */
        int32_t rel = (int32_t)fetch_dword(ds);
        if (test_cc(s, 0x0)) {
            if (is_long_mode(s))
                s->rip = ds->pc + (int64_t)rel;
            else
                s->rip = (uint32_t)(ds->pc + rel - s->segs[X86_CPU_SEG_CS].base);
        }
        break; }
    case 0x81: { /* Jcc near (0x1) */
        int32_t rel = (int32_t)fetch_dword(ds);
        if (test_cc(s, 0x1)) {
            if (is_long_mode(s))
                s->rip = ds->pc + (int64_t)rel;
            else
                s->rip = (uint32_t)(ds->pc + rel - s->segs[X86_CPU_SEG_CS].base);
        }
        break; }
    case 0x82: { /* Jcc near (0x2) */
        int32_t rel = (int32_t)fetch_dword(ds);
        if (test_cc(s, 0x2)) {
            if (is_long_mode(s))
                s->rip = ds->pc + (int64_t)rel;
            else
                s->rip = (uint32_t)(ds->pc + rel - s->segs[X86_CPU_SEG_CS].base);
        }
        break; }
    case 0x83: { /* Jcc near (0x3) */
        int32_t rel = (int32_t)fetch_dword(ds);
        if (test_cc(s, 0x3)) {
            if (is_long_mode(s))
                s->rip = ds->pc + (int64_t)rel;
            else
                s->rip = (uint32_t)(ds->pc + rel - s->segs[X86_CPU_SEG_CS].base);
        }
        break; }
    case 0x84: { /* Jcc near (0x4) */
        int32_t rel = (int32_t)fetch_dword(ds);
        if (test_cc(s, 0x4)) {
            if (is_long_mode(s))
                s->rip = ds->pc + (int64_t)rel;
            else
                s->rip = (uint32_t)(ds->pc + rel - s->segs[X86_CPU_SEG_CS].base);
        }
        break; }
    case 0x85: { /* Jcc near (0x5) */
        int32_t rel = (int32_t)fetch_dword(ds);
        if (test_cc(s, 0x5)) {
            if (is_long_mode(s))
                s->rip = ds->pc + (int64_t)rel;
            else
                s->rip = (uint32_t)(ds->pc + rel - s->segs[X86_CPU_SEG_CS].base);
        }
        break; }
    case 0x86: { /* Jcc near (0x6) */
        int32_t rel = (int32_t)fetch_dword(ds);
        if (test_cc(s, 0x6)) {
            if (is_long_mode(s))
                s->rip = ds->pc + (int64_t)rel;
            else
                s->rip = (uint32_t)(ds->pc + rel - s->segs[X86_CPU_SEG_CS].base);
        }
        break; }
    case 0x87: { /* Jcc near (0x7) */
        int32_t rel = (int32_t)fetch_dword(ds);
        if (test_cc(s, 0x7)) {
            if (is_long_mode(s))
                s->rip = ds->pc + (int64_t)rel;
            else
                s->rip = (uint32_t)(ds->pc + rel - s->segs[X86_CPU_SEG_CS].base);
        }
        break; }
    case 0x88: { /* Jcc near (0x8) */
        int32_t rel = (int32_t)fetch_dword(ds);
        if (test_cc(s, 0x8)) {
            if (is_long_mode(s))
                s->rip = ds->pc + (int64_t)rel;
            else
                s->rip = (uint32_t)(ds->pc + rel - s->segs[X86_CPU_SEG_CS].base);
        }
        break; }
    case 0x89: { /* Jcc near (0x9) */
        int32_t rel = (int32_t)fetch_dword(ds);
        if (test_cc(s, 0x9)) {
            if (is_long_mode(s))
                s->rip = ds->pc + (int64_t)rel;
            else
                s->rip = (uint32_t)(ds->pc + rel - s->segs[X86_CPU_SEG_CS].base);
        }
        break; }
    case 0x8A: { /* Jcc near (0xa) */
        int32_t rel = (int32_t)fetch_dword(ds);
        if (test_cc(s, 0xA)) {
            if (is_long_mode(s))
                s->rip = ds->pc + (int64_t)rel;
            else
                s->rip = (uint32_t)(ds->pc + rel - s->segs[X86_CPU_SEG_CS].base);
        }
        break; }
    case 0x8B: { /* Jcc near (0xb) */
        int32_t rel = (int32_t)fetch_dword(ds);
        if (test_cc(s, 0xB)) {
            if (is_long_mode(s))
                s->rip = ds->pc + (int64_t)rel;
            else
                s->rip = (uint32_t)(ds->pc + rel - s->segs[X86_CPU_SEG_CS].base);
        }
        break; }
    case 0x8C: { /* Jcc near (0xc) */
        int32_t rel = (int32_t)fetch_dword(ds);
        if (test_cc(s, 0xC)) {
            if (is_long_mode(s))
                s->rip = ds->pc + (int64_t)rel;
            else
                s->rip = (uint32_t)(ds->pc + rel - s->segs[X86_CPU_SEG_CS].base);
        }
        break; }
    case 0x8D: { /* Jcc near (0xd) */
        int32_t rel = (int32_t)fetch_dword(ds);
        if (test_cc(s, 0xD)) {
            if (is_long_mode(s))
                s->rip = ds->pc + (int64_t)rel;
            else
                s->rip = (uint32_t)(ds->pc + rel - s->segs[X86_CPU_SEG_CS].base);
        }
        break; }
    case 0x8E: { /* Jcc near (0xe) */
        int32_t rel = (int32_t)fetch_dword(ds);
        if (test_cc(s, 0xE)) {
            if (is_long_mode(s))
                s->rip = ds->pc + (int64_t)rel;
            else
                s->rip = (uint32_t)(ds->pc + rel - s->segs[X86_CPU_SEG_CS].base);
        }
        break; }
    case 0x8F: { /* Jcc near (0xf) */
        int32_t rel = (int32_t)fetch_dword(ds);
        if (test_cc(s, 0xF)) {
            if (is_long_mode(s))
                s->rip = ds->pc + (int64_t)rel;
            else
                s->rip = (uint32_t)(ds->pc + rel - s->segs[X86_CPU_SEG_CS].base);
        }
        break; }
    case 0x90: { /* SETcc (0x0) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t setv = test_cc(s, 0x0) ? 1 : 0;
        if (rm_reg >= 0) set_reg8(s, rm_reg, setv, ds->has_rex);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), setv);
        break; }
    case 0x91: { /* SETcc (0x1) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t setv = test_cc(s, 0x1) ? 1 : 0;
        if (rm_reg >= 0) set_reg8(s, rm_reg, setv, ds->has_rex);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), setv);
        break; }
    case 0x92: { /* SETcc (0x2) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t setv = test_cc(s, 0x2) ? 1 : 0;
        if (rm_reg >= 0) set_reg8(s, rm_reg, setv, ds->has_rex);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), setv);
        break; }
    case 0x93: { /* SETcc (0x3) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t setv = test_cc(s, 0x3) ? 1 : 0;
        if (rm_reg >= 0) set_reg8(s, rm_reg, setv, ds->has_rex);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), setv);
        break; }
    case 0x94: { /* SETcc (0x4) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t setv = test_cc(s, 0x4) ? 1 : 0;
        if (rm_reg >= 0) set_reg8(s, rm_reg, setv, ds->has_rex);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), setv);
        break; }
    case 0x95: { /* SETcc (0x5) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t setv = test_cc(s, 0x5) ? 1 : 0;
        if (rm_reg >= 0) set_reg8(s, rm_reg, setv, ds->has_rex);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), setv);
        break; }
    case 0x96: { /* SETcc (0x6) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t setv = test_cc(s, 0x6) ? 1 : 0;
        if (rm_reg >= 0) set_reg8(s, rm_reg, setv, ds->has_rex);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), setv);
        break; }
    case 0x97: { /* SETcc (0x7) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t setv = test_cc(s, 0x7) ? 1 : 0;
        if (rm_reg >= 0) set_reg8(s, rm_reg, setv, ds->has_rex);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), setv);
        break; }
    case 0x98: { /* SETcc (0x8) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t setv = test_cc(s, 0x8) ? 1 : 0;
        if (rm_reg >= 0) set_reg8(s, rm_reg, setv, ds->has_rex);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), setv);
        break; }
    case 0x99: { /* SETcc (0x9) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t setv = test_cc(s, 0x9) ? 1 : 0;
        if (rm_reg >= 0) set_reg8(s, rm_reg, setv, ds->has_rex);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), setv);
        break; }
    case 0x9A: { /* SETcc (0xa) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t setv = test_cc(s, 0xA) ? 1 : 0;
        if (rm_reg >= 0) set_reg8(s, rm_reg, setv, ds->has_rex);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), setv);
        break; }
    case 0x9B: { /* SETcc (0xb) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t setv = test_cc(s, 0xB) ? 1 : 0;
        if (rm_reg >= 0) set_reg8(s, rm_reg, setv, ds->has_rex);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), setv);
        break; }
    case 0x9C: { /* SETcc (0xc) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t setv = test_cc(s, 0xC) ? 1 : 0;
        if (rm_reg >= 0) set_reg8(s, rm_reg, setv, ds->has_rex);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), setv);
        break; }
    case 0x9D: { /* SETcc (0xd) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t setv = test_cc(s, 0xD) ? 1 : 0;
        if (rm_reg >= 0) set_reg8(s, rm_reg, setv, ds->has_rex);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), setv);
        break; }
    case 0x9E: { /* SETcc (0xe) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t setv = test_cc(s, 0xE) ? 1 : 0;
        if (rm_reg >= 0) set_reg8(s, rm_reg, setv, ds->has_rex);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), setv);
        break; }
    case 0x9F: { /* SETcc (0xf) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t setv = test_cc(s, 0xF) ? 1 : 0;
        if (rm_reg >= 0) set_reg8(s, rm_reg, setv, ds->has_rex);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), setv);
        break; }
    case 0xA0: push32(s, s->segs[X86_CPU_SEG_FS].sel); break; /* PUSH FS */
    case 0xA1: s->segs[X86_CPU_SEG_FS].sel = pop32(s) & 0xFFFF; break; /* POP FS */
    case 0xA2: /* CPUID */
        do_cpuid(s);
        break;

    case 0xA3: { /* BT r/m, r */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint32_t bit;
        if (ds->op64) {
            uint64_t base;
            bit = (uint32_t)s->regs[reg] & 63;
            if (rm_reg >= 0) base = s->regs[rm_reg];
            else base = vmem_read64(s, seg_ea(s, ea, ea_seg));
            if ((base >> bit) & 1) s->eflags |= EF_CF; else s->eflags &= ~EF_CF;
        } else {
            uint32_t base;
            bit = (uint32_t)s->regs[reg] & 31;
            if (rm_reg >= 0) base = get_reg32(s, rm_reg);
            else base = vmem_read32(s, seg_ea(s, ea, ea_seg));
            if ((base >> bit) & 1) s->eflags |= EF_CF; else s->eflags &= ~EF_CF;
        }
        break; }

    case 0xA4: { /* SHLD r/m, r, imm8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t cnt = fetch_byte(ds) & (ds->op64 ? 63 : 31);
        if (ds->op64) {
            uint64_t dst = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, seg_ea(s, ea, ea_seg));
            uint64_t src = get_reg64(s, reg);
            uint64_t r = (cnt == 0) ? dst : (dst << cnt) | (src >> (64 - cnt));
            flags_logic64(s, r);
            if (rm_reg >= 0) set_reg64(s, rm_reg, r); else vmem_write64(s, seg_ea(s, ea, ea_seg), r);
        } else {
            uint32_t dst = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, seg_ea(s, ea, ea_seg));
            uint32_t src = get_reg32(s, reg);
            uint32_t r = (cnt == 0) ? dst : (dst << cnt) | (src >> (32 - cnt));
            flags_logic32(s, r);
            if (rm_reg >= 0) set_reg32(s, rm_reg, r); else vmem_write32(s, seg_ea(s, ea, ea_seg), r);
        }
        break; }

    case 0xA5: { /* SHLD r/m, r, CL */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t cnt = (uint8_t)s->regs[1] & (ds->op64 ? 63 : 31);
        if (ds->op64) {
            uint64_t dst = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, seg_ea(s, ea, ea_seg));
            uint64_t src = get_reg64(s, reg);
            uint64_t r = (cnt == 0) ? dst : (dst << cnt) | (src >> (64 - cnt));
            flags_logic64(s, r);
            if (rm_reg >= 0) set_reg64(s, rm_reg, r); else vmem_write64(s, seg_ea(s, ea, ea_seg), r);
        } else {
            uint32_t dst = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, seg_ea(s, ea, ea_seg));
            uint32_t src = get_reg32(s, reg);
            uint32_t r = (cnt == 0) ? dst : (dst << cnt) | (src >> (32 - cnt));
            flags_logic32(s, r);
            if (rm_reg >= 0) set_reg32(s, rm_reg, r); else vmem_write32(s, seg_ea(s, ea, ea_seg), r);
        }
        break; }

    case 0xA8: push32(s, s->segs[X86_CPU_SEG_GS].sel); break; /* PUSH GS */
    case 0xA9: s->segs[X86_CPU_SEG_GS].sel = pop32(s) & 0xFFFF; break; /* POP GS */

    case 0xAB: { /* BTS r/m, r */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint32_t bit = (uint32_t)s->regs[reg] & 31;
        uint32_t base = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, seg_ea(s, ea, ea_seg));
        if ((base >> bit) & 1) s->eflags |= EF_CF; else s->eflags &= ~EF_CF;
        base |= (1U << bit);
        if (rm_reg >= 0) set_reg32(s, rm_reg, base); else vmem_write32(s, seg_ea(s, ea, ea_seg), base);
        break; }

    case 0xAC: { /* SHRD r/m, r, imm8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t cnt = fetch_byte(ds) & (ds->op64 ? 63 : 31);
        if (ds->op64) {
            uint64_t dst = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, seg_ea(s, ea, ea_seg));
            uint64_t src = get_reg64(s, reg);
            uint64_t r = (cnt == 0) ? dst : (dst >> cnt) | (src << (64 - cnt));
            flags_logic64(s, r);
            if (rm_reg >= 0) set_reg64(s, rm_reg, r); else vmem_write64(s, seg_ea(s, ea, ea_seg), r);
        } else {
            uint32_t dst = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, seg_ea(s, ea, ea_seg));
            uint32_t src = get_reg32(s, reg);
            uint32_t r = (cnt == 0) ? dst : (dst >> cnt) | (src << (32 - cnt));
            flags_logic32(s, r);
            if (rm_reg >= 0) set_reg32(s, rm_reg, r); else vmem_write32(s, seg_ea(s, ea, ea_seg), r);
        }
        break; }

    case 0xAD: { /* SHRD r/m, r, CL */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t cnt = (uint8_t)s->regs[1] & (ds->op64 ? 63 : 31);
        if (ds->op64) {
            uint64_t dst = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, seg_ea(s, ea, ea_seg));
            uint64_t src = get_reg64(s, reg);
            uint64_t r = (cnt == 0) ? dst : (dst >> cnt) | (src << (64 - cnt));
            flags_logic64(s, r);
            if (rm_reg >= 0) set_reg64(s, rm_reg, r); else vmem_write64(s, seg_ea(s, ea, ea_seg), r);
        } else {
            uint32_t dst = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, seg_ea(s, ea, ea_seg));
            uint32_t src = get_reg32(s, reg);
            uint32_t r = (cnt == 0) ? dst : (dst >> cnt) | (src << (32 - cnt));
            flags_logic32(s, r);
            if (rm_reg >= 0) set_reg32(s, rm_reg, r); else vmem_write32(s, seg_ea(s, ea, ea_seg), r);
        }
        break; }

    case 0xAF: { /* IMUL r, r/m */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        if (ds->op64) {
            int64_t a = (int64_t)get_reg64(s, reg);
            int64_t b = (rm_reg >= 0) ? (int64_t)get_reg64(s, rm_reg) : (int64_t)vmem_read64(s, seg_ea(s, ea, ea_seg));
            int64_t r = a * b;
            set_reg64(s, reg, (uint64_t)r);
            __int128_t full = (__int128_t)a * (__int128_t)b;
            if (full != (int64_t)r) s->eflags |= EF_CF|EF_OF; else s->eflags &= ~(EF_CF|EF_OF);
        } else {
            int32_t a = (int32_t)get_reg32(s, reg);
            int32_t b = (rm_reg >= 0) ? (int32_t)get_reg32(s, rm_reg) : (int32_t)vmem_read32(s, seg_ea(s, ea, ea_seg));
            int64_t r = (int64_t)a * b;
            set_reg32(s, reg, (uint32_t)r);
            if (r != (int64_t)(int32_t)r) s->eflags |= EF_CF|EF_OF; else s->eflags &= ~(EF_CF|EF_OF);
        }
        break; }

    case 0xB0: { /* CMPXCHG r/m8, r8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t dst = (rm_reg >= 0) ? get_reg8(s, rm_reg, ds->has_rex) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        uint8_t acc = get_reg8(s, 0, ds->has_rex);
        (void)alu_op8(s, 7, acc, dst); /* sets flags */
        if (acc == dst) {
            if (rm_reg >= 0) set_reg8(s, rm_reg, get_reg8(s, reg, ds->has_rex), ds->has_rex);
            else vmem_write8(s, seg_ea(s, ea, ea_seg), get_reg8(s, reg, ds->has_rex));
        } else {
            set_reg8(s, 0, dst, ds->has_rex);
        }
        break; }

    case 0xB1: { /* CMPXCHG r/m, r */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        if (ds->op64) {
            uint64_t dst = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, seg_ea(s, ea, ea_seg));
            uint64_t acc = get_reg64(s, 0);
            (void)alu_op64(s, 7, acc, dst);
            if (acc == dst) {
                if (rm_reg >= 0) set_reg64(s, rm_reg, get_reg64(s, reg));
                else vmem_write64(s, seg_ea(s, ea, ea_seg), get_reg64(s, reg));
            } else set_reg64(s, 0, dst);
        } else if (ds->op32) {
            uint32_t dst = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, seg_ea(s, ea, ea_seg));
            uint32_t acc = get_reg32(s, 0);
            (void)alu_op32(s, 7, acc, dst);
            if (acc == dst) {
                if (rm_reg >= 0) set_reg32(s, rm_reg, get_reg32(s, reg));
                else vmem_write32(s, seg_ea(s, ea, ea_seg), get_reg32(s, reg));
            } else set_reg32(s, 0, dst);
        } else {
            uint16_t dst = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, seg_ea(s, ea, ea_seg));
            uint16_t acc = get_reg16(s, 0);
            (void)alu_op16(s, 7, acc, dst);
            if (acc == dst) {
                if (rm_reg >= 0) set_reg16(s, rm_reg, get_reg16(s, reg));
                else vmem_write16(s, seg_ea(s, ea, ea_seg), get_reg16(s, reg));
            } else set_reg16(s, 0, dst);
        }
        break; }

    case 0xB3: { /* BTR r/m, r */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint32_t bit = (uint32_t)s->regs[reg] & 31;
        uint32_t base = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, seg_ea(s, ea, ea_seg));
        if ((base >> bit) & 1) s->eflags |= EF_CF; else s->eflags &= ~EF_CF;
        base &= ~(1U << bit);
        if (rm_reg >= 0) set_reg32(s, rm_reg, base); else vmem_write32(s, seg_ea(s, ea, ea_seg), base);
        break; }

    case 0xB6: { /* MOVZX r, r/m8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t v = (rm_reg >= 0) ? get_reg8(s, rm_reg, ds->has_rex) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        if (ds->op64) set_reg64(s, reg, v);
        else if (ds->op32) set_reg32(s, reg, v);
        else set_reg16(s, reg, v);
        break; }

    case 0xB7: { /* MOVZX r, r/m16 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint16_t v = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, seg_ea(s, ea, ea_seg));
        if (ds->op64) set_reg64(s, reg, v);
        else set_reg32(s, reg, v);
        break; }

    case 0xB8: { /* POPCNT / BSF fallthrough - BSF r, r/m */
        /* Treat as BSF */
        /* FALLTHROUGH to 0xBC */
    }
    case 0xBC: { /* BSF r, r/m */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        if (ds->op64) {
            uint64_t src = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, seg_ea(s, ea, ea_seg));
            if (src == 0) { s->eflags |= EF_ZF; break; }
            s->eflags &= ~EF_ZF;
            int i; for (i = 0; i < 64 && !((src >> i) & 1); i++);
            set_reg64(s, reg, (uint64_t)i);
        } else {
            uint32_t src = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, seg_ea(s, ea, ea_seg));
            if (src == 0) { s->eflags |= EF_ZF; break; }
            s->eflags &= ~EF_ZF;
            int i; for (i = 0; i < 32 && !((src >> i) & 1); i++);
            set_reg32(s, reg, (uint32_t)i);
        }
        break; }

    case 0xBD: { /* BSR r, r/m */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        if (ds->op64) {
            uint64_t src = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, seg_ea(s, ea, ea_seg));
            if (src == 0) { s->eflags |= EF_ZF; break; }
            s->eflags &= ~EF_ZF;
            int i; for (i = 63; i >= 0 && !((src >> i) & 1); i--);
            set_reg64(s, reg, (uint64_t)i);
        } else {
            uint32_t src = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, seg_ea(s, ea, ea_seg));
            if (src == 0) { s->eflags |= EF_ZF; break; }
            s->eflags &= ~EF_ZF;
            int i; for (i = 31; i >= 0 && !((src >> i) & 1); i--);
            set_reg32(s, reg, (uint32_t)i);
        }
        break; }

    case 0xBE: { /* MOVSX r, r/m8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        int8_t v = (rm_reg >= 0) ? (int8_t)get_reg8(s, rm_reg, ds->has_rex) : (int8_t)vmem_read8(s, seg_ea(s, ea, ea_seg));
        if (ds->op64) set_reg64(s, reg, (uint64_t)(int64_t)v);
        else if (ds->op32) set_reg32(s, reg, (uint32_t)(int32_t)v);
        else set_reg16(s, reg, (uint16_t)(int16_t)v);
        break; }

    case 0xBF: { /* MOVSX r, r/m16 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        int16_t v = (rm_reg >= 0) ? (int16_t)get_reg16(s, rm_reg) : (int16_t)vmem_read16(s, seg_ea(s, ea, ea_seg));
        if (ds->op64) set_reg64(s, reg, (uint64_t)(int64_t)v);
        else set_reg32(s, reg, (uint32_t)(int32_t)v);
        break; }

    case 0xBA: { /* GROUP 8: BT/BTS/BTR/BTC r/m, imm8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t imm8 = fetch_byte(ds);
        if (ds->op64) {
            uint64_t base = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, seg_ea(s, ea, ea_seg));
            uint32_t bit = imm8 & 63;
            uint64_t oldv = base;
            if ((base >> bit) & 1) s->eflags |= EF_CF; else s->eflags &= ~EF_CF;
            switch (reg) {
            case 5: base |= (1ULL << bit); break;
            case 6: base &= ~(1ULL << bit); break;
            case 7: base ^= (1ULL << bit); break;
            default: break; /* BT only reads */
            }
            if (base != oldv) {
                if (rm_reg >= 0) set_reg64(s, rm_reg, base);
                else vmem_write64(s, seg_ea(s, ea, ea_seg), base);
            }
        } else {
            uint32_t base = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, seg_ea(s, ea, ea_seg));
            uint32_t bit = imm8 & 31;
            uint32_t oldv = base;
            if ((base >> bit) & 1) s->eflags |= EF_CF; else s->eflags &= ~EF_CF;
            switch (reg) {
            case 5: base |= (1U << bit); break;
            case 6: base &= ~(1U << bit); break;
            case 7: base ^= (1U << bit); break;
            default: break;
            }
            if (base != oldv) {
                if (rm_reg >= 0) set_reg32(s, rm_reg, base);
                else vmem_write32(s, seg_ea(s, ea, ea_seg), base);
            }
        }
        break; }

    case 0xBB: { /* BTC r/m, r */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint32_t bit = (uint32_t)s->regs[reg] & 31;
        uint32_t base = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, seg_ea(s, ea, ea_seg));
        if ((base >> bit) & 1) s->eflags |= EF_CF; else s->eflags &= ~EF_CF;
        base ^= (1U << bit);
        if (rm_reg >= 0) set_reg32(s, rm_reg, base); else vmem_write32(s, seg_ea(s, ea, ea_seg), base);
        break; }

    case 0xC0: { /* XADD r/m8, r8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = (rm_reg >= 0) ? get_reg8(s, rm_reg, ds->has_rex) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        uint8_t b = get_reg8(s, reg, ds->has_rex);
        uint8_t r = alu_op8(s, 0, a, b);
        set_reg8(s, reg, a, ds->has_rex);
        if (rm_reg >= 0) set_reg8(s, rm_reg, r, ds->has_rex);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), r);
        break; }

    case 0xC1: { /* XADD r/m, r */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        if (ds->op64) {
            uint64_t a = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, seg_ea(s, ea, ea_seg));
            uint64_t b = get_reg64(s, reg);
            uint64_t r = alu_op64(s, 0, a, b);
            set_reg64(s, reg, a);
            if (rm_reg >= 0) set_reg64(s, rm_reg, r); else vmem_write64(s, seg_ea(s, ea, ea_seg), r);
        } else if (ds->op32) {
            uint32_t a = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, seg_ea(s, ea, ea_seg));
            uint32_t b = get_reg32(s, reg);
            uint32_t r = alu_op32(s, 0, a, b);
            set_reg32(s, reg, a);
            if (rm_reg >= 0) set_reg32(s, rm_reg, r); else vmem_write32(s, seg_ea(s, ea, ea_seg), r);
        } else {
            uint16_t a = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, seg_ea(s, ea, ea_seg));
            uint16_t b = get_reg16(s, reg);
            uint16_t r = alu_op16(s, 0, a, b);
            set_reg16(s, reg, a);
            if (rm_reg >= 0) set_reg16(s, rm_reg, r); else vmem_write16(s, seg_ea(s, ea, ea_seg), r);
        }
        break; }

    case 0xC8: { /* BSWAP r+ REX.B */
        int bswap_r = 0 + ds->rex_b;
        if (ds->op64) {
            uint64_t v = get_reg64(s, bswap_r);
            v = ((v & 0xFF) << 56) | (((v>>8)&0xFF)<<48) | (((v>>16)&0xFF)<<40) |
                (((v>>24)&0xFF)<<32) | (((v>>32)&0xFF)<<24) | (((v>>40)&0xFF)<<16) |
                (((v>>48)&0xFF)<<8) | ((v>>56)&0xFF);
            set_reg64(s, bswap_r, v);
        } else {
            uint32_t v = get_reg32(s, bswap_r);
            v = ((v&0xFF)<<24)|(((v>>8)&0xFF)<<16)|(((v>>16)&0xFF)<<8)|((v>>24)&0xFF);
            set_reg32(s, bswap_r, v);
        }
        break; }
    case 0xC9: { /* BSWAP r */
        int bswap_r = 1 + ds->rex_b;
        if (ds->op64) {
            uint64_t v = get_reg64(s, bswap_r);
            v = ((v & 0xFF) << 56) | (((v>>8)&0xFF)<<48) | (((v>>16)&0xFF)<<40) |
                (((v>>24)&0xFF)<<32) | (((v>>32)&0xFF)<<24) | (((v>>40)&0xFF)<<16) |
                (((v>>48)&0xFF)<<8) | ((v>>56)&0xFF);
            set_reg64(s, bswap_r, v);
        } else {
            uint32_t v = get_reg32(s, bswap_r);
            v = ((v&0xFF)<<24)|(((v>>8)&0xFF)<<16)|(((v>>16)&0xFF)<<8)|((v>>24)&0xFF);
            set_reg32(s, bswap_r, v);
        }
        break; }
    case 0xCA: { /* BSWAP r */
        int bswap_r = 2 + ds->rex_b;
        if (ds->op64) {
            uint64_t v = get_reg64(s, bswap_r);
            v = ((v & 0xFF) << 56) | (((v>>8)&0xFF)<<48) | (((v>>16)&0xFF)<<40) |
                (((v>>24)&0xFF)<<32) | (((v>>32)&0xFF)<<24) | (((v>>40)&0xFF)<<16) |
                (((v>>48)&0xFF)<<8) | ((v>>56)&0xFF);
            set_reg64(s, bswap_r, v);
        } else {
            uint32_t v = get_reg32(s, bswap_r);
            v = ((v&0xFF)<<24)|(((v>>8)&0xFF)<<16)|(((v>>16)&0xFF)<<8)|((v>>24)&0xFF);
            set_reg32(s, bswap_r, v);
        }
        break; }
    case 0xCB: { /* BSWAP r */
        int bswap_r = 3 + ds->rex_b;
        if (ds->op64) {
            uint64_t v = get_reg64(s, bswap_r);
            v = ((v & 0xFF) << 56) | (((v>>8)&0xFF)<<48) | (((v>>16)&0xFF)<<40) |
                (((v>>24)&0xFF)<<32) | (((v>>32)&0xFF)<<24) | (((v>>40)&0xFF)<<16) |
                (((v>>48)&0xFF)<<8) | ((v>>56)&0xFF);
            set_reg64(s, bswap_r, v);
        } else {
            uint32_t v = get_reg32(s, bswap_r);
            v = ((v&0xFF)<<24)|(((v>>8)&0xFF)<<16)|(((v>>16)&0xFF)<<8)|((v>>24)&0xFF);
            set_reg32(s, bswap_r, v);
        }
        break; }
    case 0xCC: { /* BSWAP r */
        int bswap_r = 4 + ds->rex_b;
        if (ds->op64) {
            uint64_t v = get_reg64(s, bswap_r);
            v = ((v & 0xFF) << 56) | (((v>>8)&0xFF)<<48) | (((v>>16)&0xFF)<<40) |
                (((v>>24)&0xFF)<<32) | (((v>>32)&0xFF)<<24) | (((v>>40)&0xFF)<<16) |
                (((v>>48)&0xFF)<<8) | ((v>>56)&0xFF);
            set_reg64(s, bswap_r, v);
        } else {
            uint32_t v = get_reg32(s, bswap_r);
            v = ((v&0xFF)<<24)|(((v>>8)&0xFF)<<16)|(((v>>16)&0xFF)<<8)|((v>>24)&0xFF);
            set_reg32(s, bswap_r, v);
        }
        break; }
    case 0xCD: { /* BSWAP r */
        int bswap_r = 5 + ds->rex_b;
        if (ds->op64) {
            uint64_t v = get_reg64(s, bswap_r);
            v = ((v & 0xFF) << 56) | (((v>>8)&0xFF)<<48) | (((v>>16)&0xFF)<<40) |
                (((v>>24)&0xFF)<<32) | (((v>>32)&0xFF)<<24) | (((v>>40)&0xFF)<<16) |
                (((v>>48)&0xFF)<<8) | ((v>>56)&0xFF);
            set_reg64(s, bswap_r, v);
        } else {
            uint32_t v = get_reg32(s, bswap_r);
            v = ((v&0xFF)<<24)|(((v>>8)&0xFF)<<16)|(((v>>16)&0xFF)<<8)|((v>>24)&0xFF);
            set_reg32(s, bswap_r, v);
        }
        break; }
    case 0xCE: { /* BSWAP r */
        int bswap_r = 6 + ds->rex_b;
        if (ds->op64) {
            uint64_t v = get_reg64(s, bswap_r);
            v = ((v & 0xFF) << 56) | (((v>>8)&0xFF)<<48) | (((v>>16)&0xFF)<<40) |
                (((v>>24)&0xFF)<<32) | (((v>>32)&0xFF)<<24) | (((v>>40)&0xFF)<<16) |
                (((v>>48)&0xFF)<<8) | ((v>>56)&0xFF);
            set_reg64(s, bswap_r, v);
        } else {
            uint32_t v = get_reg32(s, bswap_r);
            v = ((v&0xFF)<<24)|(((v>>8)&0xFF)<<16)|(((v>>16)&0xFF)<<8)|((v>>24)&0xFF);
            set_reg32(s, bswap_r, v);
        }
        break; }
    case 0xCF: { /* BSWAP r */
        int bswap_r = 7 + ds->rex_b;
        if (ds->op64) {
            uint64_t v = get_reg64(s, bswap_r);
            v = ((v & 0xFF) << 56) | (((v>>8)&0xFF)<<48) | (((v>>16)&0xFF)<<40) |
                (((v>>24)&0xFF)<<32) | (((v>>32)&0xFF)<<24) | (((v>>40)&0xFF)<<16) |
                (((v>>48)&0xFF)<<8) | ((v>>56)&0xFF);
            set_reg64(s, bswap_r, v);
        } else {
            uint32_t v = get_reg32(s, bswap_r);
            v = ((v&0xFF)<<24)|(((v>>8)&0xFF)<<16)|(((v>>16)&0xFF)<<8)|((v>>24)&0xFF);
            set_reg32(s, bswap_r, v);
        }
        break; }

    default:
        fprintf(stderr, "x86: unhandled 0F opcode 0x%02X at RIP=%llx\n",
                op2, (unsigned long long)s->rip);
        raise_exception(s, EXCP_UD);
    }
}
/* ------------------------------------------------------------------
 * exec_one - one-byte opcode dispatch (32/64-bit aware)
 * ------------------------------------------------------------------ */

static void exec_one(DecodeState *ds)
{
    X86CPUState *s = ds->cpu;
    uint8_t op;
    int reg, rm_reg;
    uint64_t ea, laddr;
    int ea_seg;

    /* Default operand/address sizes from current CPU mode */
    if (is_long_mode(s)) {
        /* In 64-bit mode, CS.L=1 means 64-bit default for most things
         * but operand default is 32-bit; addr default is 64-bit */
        ds->op32  = TRUE;
        ds->op64  = FALSE;
        ds->addr32 = FALSE;
        ds->addr64 = TRUE;
    } else {
        /* In 32-bit protected mode: op32 from CS.D bit */
        ds->op32  = (s->segs[X86_CPU_SEG_CS].flags >> 14) & 1;
        ds->op64  = FALSE;
        ds->addr32 = ds->op32;
        ds->addr64 = FALSE;
    }
    ds->seg_ovr = -1;
    ds->rep = ds->repne = FALSE;
    ds->rex_r = ds->rex_x = ds->rex_b = 0;
    ds->has_rex = FALSE;

prefix_loop:
    op = fetch_byte(ds);

    /* Handle REX prefix in 64-bit mode (0x40-0x4F) */
    if (is_long_mode(s) && (op & 0xF0) == 0x40) {
        ds->has_rex = TRUE;
        if (op & 8) ds->op64 = TRUE;  /* REX.W */
        ds->rex_r = (op >> 2) & 1;    /* REX.R */
        ds->rex_x = (op >> 1) & 1;    /* REX.X */
        ds->rex_b = (op >> 0) & 1;    /* REX.B */
        goto prefix_loop;
    }

    switch (op) {

    case 0x26: ds->seg_ovr = X86_CPU_SEG_ES; goto prefix_loop;
    case 0x2E: ds->seg_ovr = X86_CPU_SEG_CS; goto prefix_loop;
    case 0x36: ds->seg_ovr = X86_CPU_SEG_SS; goto prefix_loop;
    case 0x3E: ds->seg_ovr = X86_CPU_SEG_DS; goto prefix_loop;
    case 0x64: ds->seg_ovr = X86_CPU_SEG_FS; goto prefix_loop;
    case 0x65: ds->seg_ovr = X86_CPU_SEG_GS; goto prefix_loop;
    case 0x66: /* operand size override */
        if (is_long_mode(s)) { if (!ds->op64) ds->op32 = !ds->op32; }
        else ds->op32 = !ds->op32;
        goto prefix_loop;
    case 0x67: /* address size override */
        if (is_long_mode(s)) { ds->addr64 = FALSE; ds->addr32 = TRUE; }
        else { ds->addr32 = !ds->addr32; }
        goto prefix_loop;
    case 0xF0: /* LOCK prefix - ignore */
        goto prefix_loop;
    case 0xF2: ds->repne = TRUE; ds->rep = FALSE; goto prefix_loop;
    case 0xF3: ds->rep   = TRUE; ds->repne = FALSE; goto prefix_loop;

    case 0x00: { /* ADD r/m8, r8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = (rm_reg >= 0) ? get_reg8(s, rm_reg, ds->has_rex) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        uint8_t b = get_reg8(s, reg, ds->has_rex);
        uint8_t r = alu_op8(s, 0, a, b);
        if (rm_reg >= 0) set_reg8(s, rm_reg, r, ds->has_rex);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), r);
        break; }
    case 0x01: { /* ADD r/m, r */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op64) {
            uint64_t a = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, laddr);
            uint64_t b = get_reg64(s, reg);
            uint64_t r = alu_op64(s, 0, a, b);
            if (rm_reg >= 0) set_reg64(s, rm_reg, r); else vmem_write64(s, laddr, r);
        } else if (ds->op32) {
            uint32_t a = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, laddr);
            uint32_t b = get_reg32(s, reg);
            uint32_t r = alu_op32(s, 0, a, b);
            if (rm_reg >= 0) set_reg32(s, rm_reg, r); else vmem_write32(s, laddr, r);
        } else {
            uint16_t a = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            uint16_t b = get_reg16(s, reg);
            uint16_t r = alu_op16(s, 0, a, b);
            if (rm_reg >= 0) set_reg16(s, rm_reg, r); else vmem_write16(s, laddr, r);
        }
        break; }
    case 0x02: { /* ADD r8, r/m8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = get_reg8(s, reg, ds->has_rex);
        uint8_t b = (rm_reg >= 0) ? get_reg8(s, rm_reg, ds->has_rex) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        uint8_t r = alu_op8(s, 0, a, b);
        set_reg8(s, reg, r, ds->has_rex);
        break; }
    case 0x03: { /* ADD r, r/m */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op64) {
            uint64_t a = get_reg64(s, reg);
            uint64_t b = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, laddr);
            uint64_t r = alu_op64(s, 0, a, b);
            set_reg64(s, reg, r);
        } else if (ds->op32) {
            uint32_t a = get_reg32(s, reg);
            uint32_t b = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, laddr);
            uint32_t r = alu_op32(s, 0, a, b);
            set_reg32(s, reg, r);
        } else {
            uint16_t a = get_reg16(s, reg);
            uint16_t b = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            uint16_t r = alu_op16(s, 0, a, b);
            set_reg16(s, reg, r);
        }
        break; }
    case 0x04: { /* ADD AL, imm8 */
        uint8_t imm = fetch_byte(ds);
        uint8_t r = alu_op8(s, 0, (uint8_t)s->regs[0], imm);
        s->regs[0] = (s->regs[0] & ~0xFFULL) | r;
        break; }
    case 0x05: { /* ADD AX/EAX/RAX, imm */
        if (ds->op64) {
            uint32_t imm32 = fetch_dword(ds);
            uint64_t imm = (uint64_t)(int64_t)(int32_t)imm32;
            uint64_t r = alu_op64(s, 0, s->regs[0], imm);
            s->regs[0] = r;
        } else if (ds->op32) {
            uint32_t imm = fetch_dword(ds);
            uint32_t r = alu_op32(s, 0, (uint32_t)s->regs[0], imm);
            set_reg32(s, 0, r);
        } else {
            uint16_t imm = fetch_word(ds);
            uint16_t r = alu_op16(s, 0, (uint16_t)s->regs[0], imm);
            set_reg16(s, 0, r);
        }
        break; }

    case 0x08: { /* OR r/m8, r8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = (rm_reg >= 0) ? get_reg8(s, rm_reg, ds->has_rex) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        uint8_t b = get_reg8(s, reg, ds->has_rex);
        uint8_t r = alu_op8(s, 1, a, b);
        if (rm_reg >= 0) set_reg8(s, rm_reg, r, ds->has_rex);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), r);
        break; }
    case 0x09: { /* OR r/m, r */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op64) {
            uint64_t a = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, laddr);
            uint64_t b = get_reg64(s, reg);
            uint64_t r = alu_op64(s, 1, a, b);
            if (rm_reg >= 0) set_reg64(s, rm_reg, r); else vmem_write64(s, laddr, r);
        } else if (ds->op32) {
            uint32_t a = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, laddr);
            uint32_t b = get_reg32(s, reg);
            uint32_t r = alu_op32(s, 1, a, b);
            if (rm_reg >= 0) set_reg32(s, rm_reg, r); else vmem_write32(s, laddr, r);
        } else {
            uint16_t a = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            uint16_t b = get_reg16(s, reg);
            uint16_t r = alu_op16(s, 1, a, b);
            if (rm_reg >= 0) set_reg16(s, rm_reg, r); else vmem_write16(s, laddr, r);
        }
        break; }
    case 0x0A: { /* OR r8, r/m8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = get_reg8(s, reg, ds->has_rex);
        uint8_t b = (rm_reg >= 0) ? get_reg8(s, rm_reg, ds->has_rex) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        uint8_t r = alu_op8(s, 1, a, b);
        set_reg8(s, reg, r, ds->has_rex);
        break; }
    case 0x0B: { /* OR r, r/m */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op64) {
            uint64_t a = get_reg64(s, reg);
            uint64_t b = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, laddr);
            uint64_t r = alu_op64(s, 1, a, b);
            set_reg64(s, reg, r);
        } else if (ds->op32) {
            uint32_t a = get_reg32(s, reg);
            uint32_t b = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, laddr);
            uint32_t r = alu_op32(s, 1, a, b);
            set_reg32(s, reg, r);
        } else {
            uint16_t a = get_reg16(s, reg);
            uint16_t b = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            uint16_t r = alu_op16(s, 1, a, b);
            set_reg16(s, reg, r);
        }
        break; }
    case 0x0C: { /* OR AL, imm8 */
        uint8_t imm = fetch_byte(ds);
        uint8_t r = alu_op8(s, 1, (uint8_t)s->regs[0], imm);
        s->regs[0] = (s->regs[0] & ~0xFFULL) | r;
        break; }
    case 0x0D: { /* OR AX/EAX/RAX, imm */
        if (ds->op64) {
            uint32_t imm32 = fetch_dword(ds);
            uint64_t imm = (uint64_t)(int64_t)(int32_t)imm32;
            uint64_t r = alu_op64(s, 1, s->regs[0], imm);
            s->regs[0] = r;
        } else if (ds->op32) {
            uint32_t imm = fetch_dword(ds);
            uint32_t r = alu_op32(s, 1, (uint32_t)s->regs[0], imm);
            set_reg32(s, 0, r);
        } else {
            uint16_t imm = fetch_word(ds);
            uint16_t r = alu_op16(s, 1, (uint16_t)s->regs[0], imm);
            set_reg16(s, 0, r);
        }
        break; }

    case 0x10: { /* ADC r/m8, r8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = (rm_reg >= 0) ? get_reg8(s, rm_reg, ds->has_rex) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        uint8_t b = get_reg8(s, reg, ds->has_rex);
        uint8_t r = alu_op8(s, 2, a, b);
        if (rm_reg >= 0) set_reg8(s, rm_reg, r, ds->has_rex);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), r);
        break; }
    case 0x11: { /* ADC r/m, r */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op64) {
            uint64_t a = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, laddr);
            uint64_t b = get_reg64(s, reg);
            uint64_t r = alu_op64(s, 2, a, b);
            if (rm_reg >= 0) set_reg64(s, rm_reg, r); else vmem_write64(s, laddr, r);
        } else if (ds->op32) {
            uint32_t a = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, laddr);
            uint32_t b = get_reg32(s, reg);
            uint32_t r = alu_op32(s, 2, a, b);
            if (rm_reg >= 0) set_reg32(s, rm_reg, r); else vmem_write32(s, laddr, r);
        } else {
            uint16_t a = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            uint16_t b = get_reg16(s, reg);
            uint16_t r = alu_op16(s, 2, a, b);
            if (rm_reg >= 0) set_reg16(s, rm_reg, r); else vmem_write16(s, laddr, r);
        }
        break; }
    case 0x12: { /* ADC r8, r/m8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = get_reg8(s, reg, ds->has_rex);
        uint8_t b = (rm_reg >= 0) ? get_reg8(s, rm_reg, ds->has_rex) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        uint8_t r = alu_op8(s, 2, a, b);
        set_reg8(s, reg, r, ds->has_rex);
        break; }
    case 0x13: { /* ADC r, r/m */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op64) {
            uint64_t a = get_reg64(s, reg);
            uint64_t b = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, laddr);
            uint64_t r = alu_op64(s, 2, a, b);
            set_reg64(s, reg, r);
        } else if (ds->op32) {
            uint32_t a = get_reg32(s, reg);
            uint32_t b = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, laddr);
            uint32_t r = alu_op32(s, 2, a, b);
            set_reg32(s, reg, r);
        } else {
            uint16_t a = get_reg16(s, reg);
            uint16_t b = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            uint16_t r = alu_op16(s, 2, a, b);
            set_reg16(s, reg, r);
        }
        break; }
    case 0x14: { /* ADC AL, imm8 */
        uint8_t imm = fetch_byte(ds);
        uint8_t r = alu_op8(s, 2, (uint8_t)s->regs[0], imm);
        s->regs[0] = (s->regs[0] & ~0xFFULL) | r;
        break; }
    case 0x15: { /* ADC AX/EAX/RAX, imm */
        if (ds->op64) {
            uint32_t imm32 = fetch_dword(ds);
            uint64_t imm = (uint64_t)(int64_t)(int32_t)imm32;
            uint64_t r = alu_op64(s, 2, s->regs[0], imm);
            s->regs[0] = r;
        } else if (ds->op32) {
            uint32_t imm = fetch_dword(ds);
            uint32_t r = alu_op32(s, 2, (uint32_t)s->regs[0], imm);
            set_reg32(s, 0, r);
        } else {
            uint16_t imm = fetch_word(ds);
            uint16_t r = alu_op16(s, 2, (uint16_t)s->regs[0], imm);
            set_reg16(s, 0, r);
        }
        break; }

    case 0x18: { /* SBB r/m8, r8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = (rm_reg >= 0) ? get_reg8(s, rm_reg, ds->has_rex) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        uint8_t b = get_reg8(s, reg, ds->has_rex);
        uint8_t r = alu_op8(s, 3, a, b);
        if (rm_reg >= 0) set_reg8(s, rm_reg, r, ds->has_rex);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), r);
        break; }
    case 0x19: { /* SBB r/m, r */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op64) {
            uint64_t a = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, laddr);
            uint64_t b = get_reg64(s, reg);
            uint64_t r = alu_op64(s, 3, a, b);
            if (rm_reg >= 0) set_reg64(s, rm_reg, r); else vmem_write64(s, laddr, r);
        } else if (ds->op32) {
            uint32_t a = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, laddr);
            uint32_t b = get_reg32(s, reg);
            uint32_t r = alu_op32(s, 3, a, b);
            if (rm_reg >= 0) set_reg32(s, rm_reg, r); else vmem_write32(s, laddr, r);
        } else {
            uint16_t a = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            uint16_t b = get_reg16(s, reg);
            uint16_t r = alu_op16(s, 3, a, b);
            if (rm_reg >= 0) set_reg16(s, rm_reg, r); else vmem_write16(s, laddr, r);
        }
        break; }
    case 0x1A: { /* SBB r8, r/m8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = get_reg8(s, reg, ds->has_rex);
        uint8_t b = (rm_reg >= 0) ? get_reg8(s, rm_reg, ds->has_rex) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        uint8_t r = alu_op8(s, 3, a, b);
        set_reg8(s, reg, r, ds->has_rex);
        break; }
    case 0x1B: { /* SBB r, r/m */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op64) {
            uint64_t a = get_reg64(s, reg);
            uint64_t b = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, laddr);
            uint64_t r = alu_op64(s, 3, a, b);
            set_reg64(s, reg, r);
        } else if (ds->op32) {
            uint32_t a = get_reg32(s, reg);
            uint32_t b = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, laddr);
            uint32_t r = alu_op32(s, 3, a, b);
            set_reg32(s, reg, r);
        } else {
            uint16_t a = get_reg16(s, reg);
            uint16_t b = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            uint16_t r = alu_op16(s, 3, a, b);
            set_reg16(s, reg, r);
        }
        break; }
    case 0x1C: { /* SBB AL, imm8 */
        uint8_t imm = fetch_byte(ds);
        uint8_t r = alu_op8(s, 3, (uint8_t)s->regs[0], imm);
        s->regs[0] = (s->regs[0] & ~0xFFULL) | r;
        break; }
    case 0x1D: { /* SBB AX/EAX/RAX, imm */
        if (ds->op64) {
            uint32_t imm32 = fetch_dword(ds);
            uint64_t imm = (uint64_t)(int64_t)(int32_t)imm32;
            uint64_t r = alu_op64(s, 3, s->regs[0], imm);
            s->regs[0] = r;
        } else if (ds->op32) {
            uint32_t imm = fetch_dword(ds);
            uint32_t r = alu_op32(s, 3, (uint32_t)s->regs[0], imm);
            set_reg32(s, 0, r);
        } else {
            uint16_t imm = fetch_word(ds);
            uint16_t r = alu_op16(s, 3, (uint16_t)s->regs[0], imm);
            set_reg16(s, 0, r);
        }
        break; }

    case 0x20: { /* AND r/m8, r8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = (rm_reg >= 0) ? get_reg8(s, rm_reg, ds->has_rex) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        uint8_t b = get_reg8(s, reg, ds->has_rex);
        uint8_t r = alu_op8(s, 4, a, b);
        if (rm_reg >= 0) set_reg8(s, rm_reg, r, ds->has_rex);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), r);
        break; }
    case 0x21: { /* AND r/m, r */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op64) {
            uint64_t a = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, laddr);
            uint64_t b = get_reg64(s, reg);
            uint64_t r = alu_op64(s, 4, a, b);
            if (rm_reg >= 0) set_reg64(s, rm_reg, r); else vmem_write64(s, laddr, r);
        } else if (ds->op32) {
            uint32_t a = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, laddr);
            uint32_t b = get_reg32(s, reg);
            uint32_t r = alu_op32(s, 4, a, b);
            if (rm_reg >= 0) set_reg32(s, rm_reg, r); else vmem_write32(s, laddr, r);
        } else {
            uint16_t a = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            uint16_t b = get_reg16(s, reg);
            uint16_t r = alu_op16(s, 4, a, b);
            if (rm_reg >= 0) set_reg16(s, rm_reg, r); else vmem_write16(s, laddr, r);
        }
        break; }
    case 0x22: { /* AND r8, r/m8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = get_reg8(s, reg, ds->has_rex);
        uint8_t b = (rm_reg >= 0) ? get_reg8(s, rm_reg, ds->has_rex) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        uint8_t r = alu_op8(s, 4, a, b);
        set_reg8(s, reg, r, ds->has_rex);
        break; }
    case 0x23: { /* AND r, r/m */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op64) {
            uint64_t a = get_reg64(s, reg);
            uint64_t b = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, laddr);
            uint64_t r = alu_op64(s, 4, a, b);
            set_reg64(s, reg, r);
        } else if (ds->op32) {
            uint32_t a = get_reg32(s, reg);
            uint32_t b = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, laddr);
            uint32_t r = alu_op32(s, 4, a, b);
            set_reg32(s, reg, r);
        } else {
            uint16_t a = get_reg16(s, reg);
            uint16_t b = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            uint16_t r = alu_op16(s, 4, a, b);
            set_reg16(s, reg, r);
        }
        break; }
    case 0x24: { /* AND AL, imm8 */
        uint8_t imm = fetch_byte(ds);
        uint8_t r = alu_op8(s, 4, (uint8_t)s->regs[0], imm);
        s->regs[0] = (s->regs[0] & ~0xFFULL) | r;
        break; }
    case 0x25: { /* AND AX/EAX/RAX, imm */
        if (ds->op64) {
            uint32_t imm32 = fetch_dword(ds);
            uint64_t imm = (uint64_t)(int64_t)(int32_t)imm32;
            uint64_t r = alu_op64(s, 4, s->regs[0], imm);
            s->regs[0] = r;
        } else if (ds->op32) {
            uint32_t imm = fetch_dword(ds);
            uint32_t r = alu_op32(s, 4, (uint32_t)s->regs[0], imm);
            set_reg32(s, 0, r);
        } else {
            uint16_t imm = fetch_word(ds);
            uint16_t r = alu_op16(s, 4, (uint16_t)s->regs[0], imm);
            set_reg16(s, 0, r);
        }
        break; }

    case 0x28: { /* SUB r/m8, r8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = (rm_reg >= 0) ? get_reg8(s, rm_reg, ds->has_rex) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        uint8_t b = get_reg8(s, reg, ds->has_rex);
        uint8_t r = alu_op8(s, 5, a, b);
        if (rm_reg >= 0) set_reg8(s, rm_reg, r, ds->has_rex);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), r);
        break; }
    case 0x29: { /* SUB r/m, r */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op64) {
            uint64_t a = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, laddr);
            uint64_t b = get_reg64(s, reg);
            uint64_t r = alu_op64(s, 5, a, b);
            if (rm_reg >= 0) set_reg64(s, rm_reg, r); else vmem_write64(s, laddr, r);
        } else if (ds->op32) {
            uint32_t a = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, laddr);
            uint32_t b = get_reg32(s, reg);
            uint32_t r = alu_op32(s, 5, a, b);
            if (rm_reg >= 0) set_reg32(s, rm_reg, r); else vmem_write32(s, laddr, r);
        } else {
            uint16_t a = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            uint16_t b = get_reg16(s, reg);
            uint16_t r = alu_op16(s, 5, a, b);
            if (rm_reg >= 0) set_reg16(s, rm_reg, r); else vmem_write16(s, laddr, r);
        }
        break; }
    case 0x2A: { /* SUB r8, r/m8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = get_reg8(s, reg, ds->has_rex);
        uint8_t b = (rm_reg >= 0) ? get_reg8(s, rm_reg, ds->has_rex) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        uint8_t r = alu_op8(s, 5, a, b);
        set_reg8(s, reg, r, ds->has_rex);
        break; }
    case 0x2B: { /* SUB r, r/m */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op64) {
            uint64_t a = get_reg64(s, reg);
            uint64_t b = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, laddr);
            uint64_t r = alu_op64(s, 5, a, b);
            set_reg64(s, reg, r);
        } else if (ds->op32) {
            uint32_t a = get_reg32(s, reg);
            uint32_t b = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, laddr);
            uint32_t r = alu_op32(s, 5, a, b);
            set_reg32(s, reg, r);
        } else {
            uint16_t a = get_reg16(s, reg);
            uint16_t b = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            uint16_t r = alu_op16(s, 5, a, b);
            set_reg16(s, reg, r);
        }
        break; }
    case 0x2C: { /* SUB AL, imm8 */
        uint8_t imm = fetch_byte(ds);
        uint8_t r = alu_op8(s, 5, (uint8_t)s->regs[0], imm);
        s->regs[0] = (s->regs[0] & ~0xFFULL) | r;
        break; }
    case 0x2D: { /* SUB AX/EAX/RAX, imm */
        if (ds->op64) {
            uint32_t imm32 = fetch_dword(ds);
            uint64_t imm = (uint64_t)(int64_t)(int32_t)imm32;
            uint64_t r = alu_op64(s, 5, s->regs[0], imm);
            s->regs[0] = r;
        } else if (ds->op32) {
            uint32_t imm = fetch_dword(ds);
            uint32_t r = alu_op32(s, 5, (uint32_t)s->regs[0], imm);
            set_reg32(s, 0, r);
        } else {
            uint16_t imm = fetch_word(ds);
            uint16_t r = alu_op16(s, 5, (uint16_t)s->regs[0], imm);
            set_reg16(s, 0, r);
        }
        break; }

    case 0x30: { /* XOR r/m8, r8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = (rm_reg >= 0) ? get_reg8(s, rm_reg, ds->has_rex) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        uint8_t b = get_reg8(s, reg, ds->has_rex);
        uint8_t r = alu_op8(s, 6, a, b);
        if (rm_reg >= 0) set_reg8(s, rm_reg, r, ds->has_rex);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), r);
        break; }
    case 0x31: { /* XOR r/m, r */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op64) {
            uint64_t a = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, laddr);
            uint64_t b = get_reg64(s, reg);
            uint64_t r = alu_op64(s, 6, a, b);
            if (rm_reg >= 0) set_reg64(s, rm_reg, r); else vmem_write64(s, laddr, r);
        } else if (ds->op32) {
            uint32_t a = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, laddr);
            uint32_t b = get_reg32(s, reg);
            uint32_t r = alu_op32(s, 6, a, b);
            if (rm_reg >= 0) set_reg32(s, rm_reg, r); else vmem_write32(s, laddr, r);
        } else {
            uint16_t a = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            uint16_t b = get_reg16(s, reg);
            uint16_t r = alu_op16(s, 6, a, b);
            if (rm_reg >= 0) set_reg16(s, rm_reg, r); else vmem_write16(s, laddr, r);
        }
        break; }
    case 0x32: { /* XOR r8, r/m8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = get_reg8(s, reg, ds->has_rex);
        uint8_t b = (rm_reg >= 0) ? get_reg8(s, rm_reg, ds->has_rex) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        uint8_t r = alu_op8(s, 6, a, b);
        set_reg8(s, reg, r, ds->has_rex);
        break; }
    case 0x33: { /* XOR r, r/m */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op64) {
            uint64_t a = get_reg64(s, reg);
            uint64_t b = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, laddr);
            uint64_t r = alu_op64(s, 6, a, b);
            set_reg64(s, reg, r);
        } else if (ds->op32) {
            uint32_t a = get_reg32(s, reg);
            uint32_t b = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, laddr);
            uint32_t r = alu_op32(s, 6, a, b);
            set_reg32(s, reg, r);
        } else {
            uint16_t a = get_reg16(s, reg);
            uint16_t b = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            uint16_t r = alu_op16(s, 6, a, b);
            set_reg16(s, reg, r);
        }
        break; }
    case 0x34: { /* XOR AL, imm8 */
        uint8_t imm = fetch_byte(ds);
        uint8_t r = alu_op8(s, 6, (uint8_t)s->regs[0], imm);
        s->regs[0] = (s->regs[0] & ~0xFFULL) | r;
        break; }
    case 0x35: { /* XOR AX/EAX/RAX, imm */
        if (ds->op64) {
            uint32_t imm32 = fetch_dword(ds);
            uint64_t imm = (uint64_t)(int64_t)(int32_t)imm32;
            uint64_t r = alu_op64(s, 6, s->regs[0], imm);
            s->regs[0] = r;
        } else if (ds->op32) {
            uint32_t imm = fetch_dword(ds);
            uint32_t r = alu_op32(s, 6, (uint32_t)s->regs[0], imm);
            set_reg32(s, 0, r);
        } else {
            uint16_t imm = fetch_word(ds);
            uint16_t r = alu_op16(s, 6, (uint16_t)s->regs[0], imm);
            set_reg16(s, 0, r);
        }
        break; }

    case 0x38: { /* CMP r/m8, r8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = (rm_reg >= 0) ? get_reg8(s, rm_reg, ds->has_rex) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        uint8_t b = get_reg8(s, reg, ds->has_rex);
        uint8_t r = alu_op8(s, 7, a, b);
        (void)r; /* CMP: flags only */
        break; }
    case 0x39: { /* CMP r/m, r */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op64) {
            uint64_t a = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, laddr);
            uint64_t b = get_reg64(s, reg);
            uint64_t r = alu_op64(s, 7, a, b);
            (void)r;
        } else if (ds->op32) {
            uint32_t a = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, laddr);
            uint32_t b = get_reg32(s, reg);
            uint32_t r = alu_op32(s, 7, a, b);
            (void)r;
        } else {
            uint16_t a = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            uint16_t b = get_reg16(s, reg);
            uint16_t r = alu_op16(s, 7, a, b);
            (void)r;
        }
        break; }
    case 0x3A: { /* CMP r8, r/m8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = get_reg8(s, reg, ds->has_rex);
        uint8_t b = (rm_reg >= 0) ? get_reg8(s, rm_reg, ds->has_rex) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        uint8_t r = alu_op8(s, 7, a, b);
        (void)r;
        break; }
    case 0x3B: { /* CMP r, r/m */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op64) {
            uint64_t a = get_reg64(s, reg);
            uint64_t b = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, laddr);
            uint64_t r = alu_op64(s, 7, a, b);
            (void)r;
        } else if (ds->op32) {
            uint32_t a = get_reg32(s, reg);
            uint32_t b = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, laddr);
            uint32_t r = alu_op32(s, 7, a, b);
            (void)r;
        } else {
            uint16_t a = get_reg16(s, reg);
            uint16_t b = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            uint16_t r = alu_op16(s, 7, a, b);
            (void)r;
        }
        break; }
    case 0x3C: { /* CMP AL, imm8 */
        uint8_t imm = fetch_byte(ds);
        uint8_t r = alu_op8(s, 7, (uint8_t)s->regs[0], imm);
        (void)r;
        break; }
    case 0x3D: { /* CMP AX/EAX/RAX, imm */
        if (ds->op64) {
            uint32_t imm32 = fetch_dword(ds);
            uint64_t imm = (uint64_t)(int64_t)(int32_t)imm32;
            uint64_t r = alu_op64(s, 7, s->regs[0], imm);
            (void)r;
        } else if (ds->op32) {
            uint32_t imm = fetch_dword(ds);
            uint32_t r = alu_op32(s, 7, (uint32_t)s->regs[0], imm);
            (void)r;
        } else {
            uint16_t imm = fetch_word(ds);
            uint16_t r = alu_op16(s, 7, (uint16_t)s->regs[0], imm);
            (void)r;
        }
        break; }
    case 0x06: { /* PUSH ES */ push32(s, s->segs[X86_CPU_SEG_ES].sel); break; }
    case 0x07: { /* POP ES */ s->segs[X86_CPU_SEG_ES].sel = (uint16_t)pop32(s); break; }
    case 0x0E: { /* PUSH CS */ push32(s, s->segs[X86_CPU_SEG_CS].sel); break; }
    /* case 0x0F: Two-byte escape handled below */
    case 0x16: { /* PUSH SS */ push32(s, s->segs[X86_CPU_SEG_SS].sel); break; }
    case 0x17: { /* POP SS */ s->segs[X86_CPU_SEG_SS].sel = (uint16_t)pop32(s); break; }
    case 0x1E: { /* PUSH DS */ push32(s, s->segs[X86_CPU_SEG_DS].sel); break; }
    case 0x1F: { /* POP DS */ s->segs[X86_CPU_SEG_DS].sel = (uint16_t)pop32(s); break; }

    case 0x27: case 0x2F: case 0x37: case 0x3F: /* BCD - not supported */
        raise_exception(s, EXCP_UD); break;

    case 0x40: case 0x41: case 0x42: case 0x43:
    case 0x44: case 0x45: case 0x46: case 0x47: { /* INC r32 */
        int r = op & 7;
        if (ds->op32) {
            uint32_t v = get_reg32(s, r);
            uint32_t r2 = v + 1;
            set_reg32(s, r, r2);
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzs32(r2);
            if (r2 == 0x80000000U) s->eflags |= EF_OF;
            if ((r2 & 0xF) == 0) s->eflags |= EF_AF;
        } else {
            uint16_t v = get_reg16(s, r);
            uint16_t r2 = v + 1;
            set_reg16(s, r, r2);
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzs16(r2);
            if (r2 == 0x8000U) s->eflags |= EF_OF;
            if ((r2 & 0xF) == 0) s->eflags |= EF_AF;
        }
        break; }
    case 0x48: case 0x49: case 0x4A: case 0x4B:
    case 0x4C: case 0x4D: case 0x4E: case 0x4F: { /* DEC r32 */
        int r = op & 7;
        if (ds->op32) {
            uint32_t v = get_reg32(s, r);
            uint32_t r2 = v - 1;
            set_reg32(s, r, r2);
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzs32(r2);
            if (v == 0x80000000U) s->eflags |= EF_OF;
            if ((r2 & 0xF) == 0xF) s->eflags |= EF_AF;
        } else {
            uint16_t v = get_reg16(s, r);
            uint16_t r2 = v - 1;
            set_reg16(s, r, r2);
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzs16(r2);
            if (v == 0x8000U) s->eflags |= EF_OF;
            if ((r2 & 0xF) == 0xF) s->eflags |= EF_AF;
        }
        break; }

    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x57: { /* PUSH r */
        int r = (op & 7) + ds->rex_b;
        if (is_long_mode(s)) push64(s, get_reg64(s, r));
        else if (ds->op32) push32(s, get_reg32(s, r));
        else push16(s, get_reg16(s, r));
        break; }
    case 0x58: case 0x59: case 0x5A: case 0x5B:
    case 0x5C: case 0x5D: case 0x5E: case 0x5F: { /* POP r */
        int r = (op & 7) + ds->rex_b;
        if (is_long_mode(s)) set_reg64(s, r, pop64(s));
        else if (ds->op32) set_reg32(s, r, pop32(s));
        else set_reg16(s, r, pop16(s));
        break; }

    case 0x60: { /* PUSHA / PUSHAD */
        if (ds->op32) {
            uint32_t esp0 = (uint32_t)s->regs[4];
            push32(s, (uint32_t)s->regs[0]); push32(s, (uint32_t)s->regs[1]);
            push32(s, (uint32_t)s->regs[2]); push32(s, (uint32_t)s->regs[3]);
            push32(s, esp0);
            push32(s, (uint32_t)s->regs[5]); push32(s, (uint32_t)s->regs[6]);
            push32(s, (uint32_t)s->regs[7]);
        } else {
            uint16_t sp0 = (uint16_t)s->regs[4];
            push16(s, (uint16_t)s->regs[0]); push16(s, (uint16_t)s->regs[1]);
            push16(s, (uint16_t)s->regs[2]); push16(s, (uint16_t)s->regs[3]);
            push16(s, sp0);
            push16(s, (uint16_t)s->regs[5]); push16(s, (uint16_t)s->regs[6]);
            push16(s, (uint16_t)s->regs[7]);
        }
        break; }
    case 0x61: { /* POPA / POPAD */
        if (ds->op32) {
            set_reg32(s, 7, pop32(s)); set_reg32(s, 6, pop32(s));
            set_reg32(s, 5, pop32(s)); (void)pop32(s); /* skip ESP */
            set_reg32(s, 3, pop32(s)); set_reg32(s, 2, pop32(s));
            set_reg32(s, 1, pop32(s)); set_reg32(s, 0, pop32(s));
        } else {
            set_reg16(s, 7, pop16(s)); set_reg16(s, 6, pop16(s));
            set_reg16(s, 5, pop16(s)); (void)pop16(s);
            set_reg16(s, 3, pop16(s)); set_reg16(s, 2, pop16(s));
            set_reg16(s, 1, pop16(s)); set_reg16(s, 0, pop16(s));
        }
        break; }

    case 0x62: /* BOUND - not supported */ raise_exception(s, EXCP_UD); break;

    case 0x63: { /* ARPL (32-bit) / MOVSXD r, r/m (64-bit) */
        if (is_long_mode(s)) {
            /* MOVSXD r64, r/m32 */
            decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
            int32_t src = (rm_reg >= 0) ? (int32_t)get_reg32(s, rm_reg) : (int32_t)vmem_read32(s, seg_ea(s, ea, ea_seg));
            set_reg64(s, reg, (uint64_t)(int64_t)src);
        } else {
            /* ARPL - skip */
            decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        }
        break; }

    case 0x68: { /* PUSH imm16/32 */
        if (is_long_mode(s)) {
            int32_t imm = (int32_t)fetch_dword(ds);
            push64(s, (uint64_t)(int64_t)imm);
        } else if (ds->op32) { push32(s, fetch_dword(ds)); }
        else { push16(s, fetch_word(ds)); }
        break; }
    case 0x69: { /* IMUL r, r/m, imm */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op64) {
            uint64_t a = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, laddr);
            int64_t imm = (int64_t)(int32_t)fetch_dword(ds);
            int64_t r = (int64_t)a * imm;
            set_reg64(s, reg, (uint64_t)r);
            __int128_t full = (__int128_t)(int64_t)a * (__int128_t)imm;
            if (full != (int64_t)r) s->eflags |= EF_CF|EF_OF; else s->eflags &= ~(EF_CF|EF_OF);
        } else if (ds->op32) {
            uint32_t a = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, laddr);
            int32_t imm = (int32_t)fetch_dword(ds);
            int64_t r = (int64_t)(int32_t)a * imm;
            set_reg32(s, reg, (uint32_t)r);
            if (r != (int64_t)(int32_t)r) s->eflags |= EF_CF|EF_OF; else s->eflags &= ~(EF_CF|EF_OF);
        } else {
            uint16_t a = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            int16_t imm = (int16_t)fetch_word(ds);
            int32_t r = (int32_t)(int16_t)a * imm;
            set_reg16(s, reg, (uint16_t)r);
            if (r != (int32_t)(int16_t)r) s->eflags |= EF_CF|EF_OF; else s->eflags &= ~(EF_CF|EF_OF);
        }
        break; }
    case 0x6A: { /* PUSH imm8 */
        int8_t imm = (int8_t)fetch_byte(ds);
        if (is_long_mode(s)) push64(s, (uint64_t)(int64_t)imm);
        else if (ds->op32) push32(s, (uint32_t)(int32_t)imm);
        else push16(s, (uint16_t)(int16_t)imm);
        break; }
    case 0x6B: { /* IMUL r, r/m, imm8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op64) {
            uint64_t a = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, laddr);
            int64_t imm = (int64_t)(int8_t)fetch_byte(ds);
            int64_t r = (int64_t)a * imm;
            set_reg64(s, reg, (uint64_t)r);
        } else if (ds->op32) {
            uint32_t a = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, laddr);
            int32_t imm = (int32_t)(int8_t)fetch_byte(ds);
            int64_t r = (int64_t)(int32_t)a * imm;
            set_reg32(s, reg, (uint32_t)r);
        } else {
            uint16_t a = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            int32_t r = (int32_t)(int16_t)a * (int32_t)(int8_t)fetch_byte(ds);
            set_reg16(s, reg, (uint16_t)r);
        }
        break; }

    case 0x70: { /* J0 short */
        int8_t rel = (int8_t)fetch_byte(ds);
        if (test_cc(s, 0x0)) {
            if (is_long_mode(s))
                s->rip = ds->pc + (int64_t)rel;
            else
                s->rip = (uint32_t)(ds->pc + rel - s->segs[X86_CPU_SEG_CS].base);
        }
        break; }
    case 0x71: { /* J1 short */
        int8_t rel = (int8_t)fetch_byte(ds);
        if (test_cc(s, 0x1)) {
            if (is_long_mode(s))
                s->rip = ds->pc + (int64_t)rel;
            else
                s->rip = (uint32_t)(ds->pc + rel - s->segs[X86_CPU_SEG_CS].base);
        }
        break; }
    case 0x72: { /* J2 short */
        int8_t rel = (int8_t)fetch_byte(ds);
        if (test_cc(s, 0x2)) {
            if (is_long_mode(s))
                s->rip = ds->pc + (int64_t)rel;
            else
                s->rip = (uint32_t)(ds->pc + rel - s->segs[X86_CPU_SEG_CS].base);
        }
        break; }
    case 0x73: { /* J3 short */
        int8_t rel = (int8_t)fetch_byte(ds);
        if (test_cc(s, 0x3)) {
            if (is_long_mode(s))
                s->rip = ds->pc + (int64_t)rel;
            else
                s->rip = (uint32_t)(ds->pc + rel - s->segs[X86_CPU_SEG_CS].base);
        }
        break; }
    case 0x74: { /* J4 short */
        int8_t rel = (int8_t)fetch_byte(ds);
        if (test_cc(s, 0x4)) {
            if (is_long_mode(s))
                s->rip = ds->pc + (int64_t)rel;
            else
                s->rip = (uint32_t)(ds->pc + rel - s->segs[X86_CPU_SEG_CS].base);
        }
        break; }
    case 0x75: { /* J5 short */
        int8_t rel = (int8_t)fetch_byte(ds);
        if (test_cc(s, 0x5)) {
            if (is_long_mode(s))
                s->rip = ds->pc + (int64_t)rel;
            else
                s->rip = (uint32_t)(ds->pc + rel - s->segs[X86_CPU_SEG_CS].base);
        }
        break; }
    case 0x76: { /* J6 short */
        int8_t rel = (int8_t)fetch_byte(ds);
        if (test_cc(s, 0x6)) {
            if (is_long_mode(s))
                s->rip = ds->pc + (int64_t)rel;
            else
                s->rip = (uint32_t)(ds->pc + rel - s->segs[X86_CPU_SEG_CS].base);
        }
        break; }
    case 0x77: { /* J7 short */
        int8_t rel = (int8_t)fetch_byte(ds);
        if (test_cc(s, 0x7)) {
            if (is_long_mode(s))
                s->rip = ds->pc + (int64_t)rel;
            else
                s->rip = (uint32_t)(ds->pc + rel - s->segs[X86_CPU_SEG_CS].base);
        }
        break; }
    case 0x78: { /* J8 short */
        int8_t rel = (int8_t)fetch_byte(ds);
        if (test_cc(s, 0x8)) {
            if (is_long_mode(s))
                s->rip = ds->pc + (int64_t)rel;
            else
                s->rip = (uint32_t)(ds->pc + rel - s->segs[X86_CPU_SEG_CS].base);
        }
        break; }
    case 0x79: { /* J9 short */
        int8_t rel = (int8_t)fetch_byte(ds);
        if (test_cc(s, 0x9)) {
            if (is_long_mode(s))
                s->rip = ds->pc + (int64_t)rel;
            else
                s->rip = (uint32_t)(ds->pc + rel - s->segs[X86_CPU_SEG_CS].base);
        }
        break; }
    case 0x7A: { /* J10 short */
        int8_t rel = (int8_t)fetch_byte(ds);
        if (test_cc(s, 0xA)) {
            if (is_long_mode(s))
                s->rip = ds->pc + (int64_t)rel;
            else
                s->rip = (uint32_t)(ds->pc + rel - s->segs[X86_CPU_SEG_CS].base);
        }
        break; }
    case 0x7B: { /* J11 short */
        int8_t rel = (int8_t)fetch_byte(ds);
        if (test_cc(s, 0xB)) {
            if (is_long_mode(s))
                s->rip = ds->pc + (int64_t)rel;
            else
                s->rip = (uint32_t)(ds->pc + rel - s->segs[X86_CPU_SEG_CS].base);
        }
        break; }
    case 0x7C: { /* J12 short */
        int8_t rel = (int8_t)fetch_byte(ds);
        if (test_cc(s, 0xC)) {
            if (is_long_mode(s))
                s->rip = ds->pc + (int64_t)rel;
            else
                s->rip = (uint32_t)(ds->pc + rel - s->segs[X86_CPU_SEG_CS].base);
        }
        break; }
    case 0x7D: { /* J13 short */
        int8_t rel = (int8_t)fetch_byte(ds);
        if (test_cc(s, 0xD)) {
            if (is_long_mode(s))
                s->rip = ds->pc + (int64_t)rel;
            else
                s->rip = (uint32_t)(ds->pc + rel - s->segs[X86_CPU_SEG_CS].base);
        }
        break; }
    case 0x7E: { /* J14 short */
        int8_t rel = (int8_t)fetch_byte(ds);
        if (test_cc(s, 0xE)) {
            if (is_long_mode(s))
                s->rip = ds->pc + (int64_t)rel;
            else
                s->rip = (uint32_t)(ds->pc + rel - s->segs[X86_CPU_SEG_CS].base);
        }
        break; }
    case 0x7F: { /* J15 short */
        int8_t rel = (int8_t)fetch_byte(ds);
        if (test_cc(s, 0xF)) {
            if (is_long_mode(s))
                s->rip = ds->pc + (int64_t)rel;
            else
                s->rip = (uint32_t)(ds->pc + rel - s->segs[X86_CPU_SEG_CS].base);
        }
        break; }

    case 0x80: { /* GROUP 1 r/m8, imm8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = (rm_reg >= 0) ? get_reg8(s, rm_reg, ds->has_rex) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        uint8_t b = fetch_byte(ds);
        uint8_t r = alu_op8(s, reg & 7, a, b);
        if ((reg & 7) != 7) {
            if (rm_reg >= 0) set_reg8(s, rm_reg, r, ds->has_rex);
            else vmem_write8(s, seg_ea(s, ea, ea_seg), r);
        }
        break; }
    case 0x81: { /* GROUP 1 r/m, imm */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op64) {
            uint64_t a = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, laddr);
            uint64_t b = (uint64_t)(int64_t)(int32_t)fetch_dword(ds);
            uint64_t r = alu_op64(s, reg & 7, a, b);
            if ((reg & 7) != 7) { if (rm_reg >= 0) set_reg64(s, rm_reg, r); else vmem_write64(s, laddr, r); }
        } else if (ds->op32) {
            uint32_t a = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, laddr);
            uint32_t b = fetch_dword(ds);
            uint32_t r = alu_op32(s, reg & 7, a, b);
            if ((reg & 7) != 7) { if (rm_reg >= 0) set_reg32(s, rm_reg, r); else vmem_write32(s, laddr, r); }
        } else {
            uint16_t a = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            uint16_t b = fetch_word(ds);
            uint16_t r = alu_op16(s, reg & 7, a, b);
            if ((reg & 7) != 7) { if (rm_reg >= 0) set_reg16(s, rm_reg, r); else vmem_write16(s, laddr, r); }
        }
        break; }
    case 0x82: { /* GROUP 1 r/m, imm */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op64) {
            uint64_t a = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, laddr);
            uint64_t b = (uint64_t)(int64_t)(int8_t)fetch_byte(ds);
            uint64_t r = alu_op64(s, reg & 7, a, b);
            if ((reg & 7) != 7) { if (rm_reg >= 0) set_reg64(s, rm_reg, r); else vmem_write64(s, laddr, r); }
        } else if (ds->op32) {
            uint32_t a = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, laddr);
            uint32_t b = (uint32_t)(int32_t)(int8_t)fetch_byte(ds);
            uint32_t r = alu_op32(s, reg & 7, a, b);
            if ((reg & 7) != 7) { if (rm_reg >= 0) set_reg32(s, rm_reg, r); else vmem_write32(s, laddr, r); }
        } else {
            uint16_t a = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            uint16_t b = (uint16_t)(int16_t)(int8_t)fetch_byte(ds);
            uint16_t r = alu_op16(s, reg & 7, a, b);
            if ((reg & 7) != 7) { if (rm_reg >= 0) set_reg16(s, rm_reg, r); else vmem_write16(s, laddr, r); }
        }
        break; }
    case 0x83: { /* GROUP 1 r/m, imm */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op64) {
            uint64_t a = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, laddr);
            uint64_t b = (uint64_t)(int64_t)(int8_t)fetch_byte(ds);
            uint64_t r = alu_op64(s, reg & 7, a, b);
            if ((reg & 7) != 7) { if (rm_reg >= 0) set_reg64(s, rm_reg, r); else vmem_write64(s, laddr, r); }
        } else if (ds->op32) {
            uint32_t a = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, laddr);
            uint32_t b = (uint32_t)(int32_t)(int8_t)fetch_byte(ds);
            uint32_t r = alu_op32(s, reg & 7, a, b);
            if ((reg & 7) != 7) { if (rm_reg >= 0) set_reg32(s, rm_reg, r); else vmem_write32(s, laddr, r); }
        } else {
            uint16_t a = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            uint16_t b = (uint16_t)(int16_t)(int8_t)fetch_byte(ds);
            uint16_t r = alu_op16(s, reg & 7, a, b);
            if ((reg & 7) != 7) { if (rm_reg >= 0) set_reg16(s, rm_reg, r); else vmem_write16(s, laddr, r); }
        }
        break; }

    case 0x84: { /* TEST r/m8, r8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = (rm_reg >= 0) ? get_reg8(s, rm_reg, ds->has_rex) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        flags_logic8(s, a & get_reg8(s, reg, ds->has_rex));
        break; }
    case 0x85: { /* TEST r/m, r */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op64) {
            flags_logic64(s, ((rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, laddr)) & get_reg64(s, reg));
        } else if (ds->op32) {
            flags_logic32(s, ((rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, laddr)) & get_reg32(s, reg));
        } else {
            flags_logic16(s, ((rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr)) & get_reg16(s, reg));
        }
        break; }

    case 0x86: { /* XCHG r/m8, r8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = (rm_reg >= 0) ? get_reg8(s, rm_reg, ds->has_rex) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        uint8_t b = get_reg8(s, reg, ds->has_rex);
        set_reg8(s, reg, a, ds->has_rex);
        if (rm_reg >= 0) set_reg8(s, rm_reg, b, ds->has_rex);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), b);
        break; }
    case 0x87: { /* XCHG r/m, r */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op64) {
            uint64_t a = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, laddr);
            uint64_t b = get_reg64(s, reg);
            set_reg64(s, reg, a);
            if (rm_reg >= 0) set_reg64(s, rm_reg, b); else vmem_write64(s, laddr, b);
        } else if (ds->op32) {
            uint32_t a = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, laddr);
            uint32_t b = get_reg32(s, reg);
            set_reg32(s, reg, a);
            if (rm_reg >= 0) set_reg32(s, rm_reg, b); else vmem_write32(s, laddr, b);
        } else {
            uint16_t a = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            uint16_t b = get_reg16(s, reg);
            set_reg16(s, reg, a);
            if (rm_reg >= 0) set_reg16(s, rm_reg, b); else vmem_write16(s, laddr, b);
        }
        break; }

    case 0x88: { /* MOV r/m8, r8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t v = get_reg8(s, reg, ds->has_rex);
        if (rm_reg >= 0) set_reg8(s, rm_reg, v, ds->has_rex);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), v);
        break; }
    case 0x89: { /* MOV r/m, r */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op64) {
            uint64_t v = get_reg64(s, reg);
            if (rm_reg >= 0) set_reg64(s, rm_reg, v); else vmem_write64(s, laddr, v);
        } else if (ds->op32) {
            uint32_t v = get_reg32(s, reg);
            if (rm_reg >= 0) set_reg32(s, rm_reg, v); else vmem_write32(s, laddr, v);
        } else {
            uint16_t v = get_reg16(s, reg);
            if (rm_reg >= 0) set_reg16(s, rm_reg, v); else vmem_write16(s, laddr, v);
        }
        break; }
    case 0x8A: { /* MOV r8, r/m8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t v = (rm_reg >= 0) ? get_reg8(s, rm_reg, ds->has_rex) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        set_reg8(s, reg, v, ds->has_rex);
        break; }
    case 0x8B: { /* MOV r, r/m */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op64) {
            uint64_t v = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, laddr);
            set_reg64(s, reg, v);
        } else if (ds->op32) {
            uint32_t v = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, laddr);
            set_reg32(s, reg, v);
        } else {
            uint16_t v = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            set_reg16(s, reg, v);
        }
        break; }

    case 0x8C: { /* MOV r/m16, Sreg */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint16_t v = (uint16_t)s->segs[reg & 7].sel;
        if (rm_reg >= 0) set_reg16(s, rm_reg, v);
        else vmem_write16(s, seg_ea(s, ea, ea_seg), v);
        break; }
    case 0x8D: { /* LEA r, m */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        if (ds->op64) set_reg64(s, reg, ea);
        else if (ds->op32) set_reg32(s, reg, (uint32_t)ea);
        else set_reg16(s, reg, (uint16_t)ea);
        break; }
    case 0x8E: { /* MOV Sreg, r/m16 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint16_t v = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, seg_ea(s, ea, ea_seg));
        load_seg_desc(s, reg & 7, v);
        break; }
    case 0x8F: { /* POP r/m */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        if (is_long_mode(s)) {
            uint64_t v = pop64(s);
            if (rm_reg >= 0) set_reg64(s, rm_reg, v); else vmem_write64(s, seg_ea(s, ea, ea_seg), v);
        } else if (ds->op32) {
            uint32_t v = pop32(s);
            if (rm_reg >= 0) set_reg32(s, rm_reg, v); else vmem_write32(s, seg_ea(s, ea, ea_seg), v);
        } else {
            uint16_t v = pop16(s);
            if (rm_reg >= 0) set_reg16(s, rm_reg, v); else vmem_write16(s, seg_ea(s, ea, ea_seg), v);
        }
        break; }

    case 0x90: /* NOP / XCHG AX, AX */ break;
    case 0x91: { /* XCHG rAX, r1 */
        int xr = 1 + ds->rex_b;
        if (ds->op64) { uint64_t t = s->regs[0]; s->regs[0] = s->regs[xr]; s->regs[xr] = t; }
        else if (ds->op32) { uint32_t t = (uint32_t)s->regs[0]; set_reg32(s, 0, (uint32_t)s->regs[xr]); set_reg32(s, xr, t); }
        else { uint16_t t = (uint16_t)s->regs[0]; set_reg16(s, 0, (uint16_t)s->regs[xr]); set_reg16(s, xr, t); }
        break; }
    case 0x92: { /* XCHG rAX, r2 */
        int xr = 2 + ds->rex_b;
        if (ds->op64) { uint64_t t = s->regs[0]; s->regs[0] = s->regs[xr]; s->regs[xr] = t; }
        else if (ds->op32) { uint32_t t = (uint32_t)s->regs[0]; set_reg32(s, 0, (uint32_t)s->regs[xr]); set_reg32(s, xr, t); }
        else { uint16_t t = (uint16_t)s->regs[0]; set_reg16(s, 0, (uint16_t)s->regs[xr]); set_reg16(s, xr, t); }
        break; }
    case 0x93: { /* XCHG rAX, r3 */
        int xr = 3 + ds->rex_b;
        if (ds->op64) { uint64_t t = s->regs[0]; s->regs[0] = s->regs[xr]; s->regs[xr] = t; }
        else if (ds->op32) { uint32_t t = (uint32_t)s->regs[0]; set_reg32(s, 0, (uint32_t)s->regs[xr]); set_reg32(s, xr, t); }
        else { uint16_t t = (uint16_t)s->regs[0]; set_reg16(s, 0, (uint16_t)s->regs[xr]); set_reg16(s, xr, t); }
        break; }
    case 0x94: { /* XCHG rAX, r4 */
        int xr = 4 + ds->rex_b;
        if (ds->op64) { uint64_t t = s->regs[0]; s->regs[0] = s->regs[xr]; s->regs[xr] = t; }
        else if (ds->op32) { uint32_t t = (uint32_t)s->regs[0]; set_reg32(s, 0, (uint32_t)s->regs[xr]); set_reg32(s, xr, t); }
        else { uint16_t t = (uint16_t)s->regs[0]; set_reg16(s, 0, (uint16_t)s->regs[xr]); set_reg16(s, xr, t); }
        break; }
    case 0x95: { /* XCHG rAX, r5 */
        int xr = 5 + ds->rex_b;
        if (ds->op64) { uint64_t t = s->regs[0]; s->regs[0] = s->regs[xr]; s->regs[xr] = t; }
        else if (ds->op32) { uint32_t t = (uint32_t)s->regs[0]; set_reg32(s, 0, (uint32_t)s->regs[xr]); set_reg32(s, xr, t); }
        else { uint16_t t = (uint16_t)s->regs[0]; set_reg16(s, 0, (uint16_t)s->regs[xr]); set_reg16(s, xr, t); }
        break; }
    case 0x96: { /* XCHG rAX, r6 */
        int xr = 6 + ds->rex_b;
        if (ds->op64) { uint64_t t = s->regs[0]; s->regs[0] = s->regs[xr]; s->regs[xr] = t; }
        else if (ds->op32) { uint32_t t = (uint32_t)s->regs[0]; set_reg32(s, 0, (uint32_t)s->regs[xr]); set_reg32(s, xr, t); }
        else { uint16_t t = (uint16_t)s->regs[0]; set_reg16(s, 0, (uint16_t)s->regs[xr]); set_reg16(s, xr, t); }
        break; }
    case 0x97: { /* XCHG rAX, r7 */
        int xr = 7 + ds->rex_b;
        if (ds->op64) { uint64_t t = s->regs[0]; s->regs[0] = s->regs[xr]; s->regs[xr] = t; }
        else if (ds->op32) { uint32_t t = (uint32_t)s->regs[0]; set_reg32(s, 0, (uint32_t)s->regs[xr]); set_reg32(s, xr, t); }
        else { uint16_t t = (uint16_t)s->regs[0]; set_reg16(s, 0, (uint16_t)s->regs[xr]); set_reg16(s, xr, t); }
        break; }

    case 0x98: { /* CBW/CWDE/CDQE */
        if (ds->op64) set_reg64(s, 0, (uint64_t)(int64_t)(int32_t)get_reg32(s, 0));
        else if (ds->op32) set_reg32(s, 0, (uint32_t)(int32_t)(int16_t)get_reg16(s, 0));
        else set_reg16(s, 0, (uint16_t)(int8_t)(uint8_t)s->regs[0]);
        break; }
    case 0x99: { /* CWD/CDQ/CQO */
        if (ds->op64) s->regs[2] = ((int64_t)s->regs[0] < 0) ? UINT64_MAX : 0;
        else if (ds->op32) set_reg32(s, 2, ((int32_t)s->regs[0] < 0) ? 0xFFFFFFFF : 0);
        else set_reg16(s, 2, ((int16_t)s->regs[0] < 0) ? 0xFFFF : 0);
        break; }

    case 0x9A: { /* CALL far: not supported in 64-bit */
        uint16_t off, sel;
        if (ds->op32) { uint32_t o = fetch_dword(ds); sel = fetch_word(ds); off = (uint16_t)o; }
        else { off = fetch_word(ds); sel = fetch_word(ds); }
        (void)sel; (void)off;
        raise_exception(s, EXCP_GP);
        break; }

    case 0x9B: /* WAIT/FWAIT - ignore */ break;

    case 0x9C: { /* PUSHF/PUSHFQ */
        if (is_long_mode(s)) push64(s, (uint64_t)(s->eflags | EF_FIXED));
        else if (ds->op32) push32(s, s->eflags | EF_FIXED);
        else push16(s, (uint16_t)(s->eflags | EF_FIXED));
        break; }
    case 0x9D: { /* POPF/POPFQ */
        uint32_t v;
        if (is_long_mode(s)) v = (uint32_t)pop64(s);
        else if (ds->op32) v = pop32(s);
        else v = pop16(s);
        s->eflags = (v & 0x3F7FD5U) | EF_FIXED;
        break; }

    case 0x9E: /* SAHF */ s->eflags = (s->eflags & ~0xD5U) | ((uint8_t)(s->regs[0] >> 8) & 0xD5U); break;
    case 0x9F: /* LAHF */ s->regs[0] = (s->regs[0] & ~0xFF00ULL) | ((uint64_t)((s->eflags & 0xD5U) | 0x02U) << 8); break;

    case 0xA0: { /* MOV AL, moffs8 */
        uint64_t moffs = ds->addr64 ? fetch_qword(ds) : ds->addr32 ? fetch_dword(ds) : fetch_word(ds);
        uint8_t v = vmem_read8(s, seg_ea(s, moffs, ds->seg_ovr >= 0 ? ds->seg_ovr : X86_CPU_SEG_DS));
        s->regs[0] = (s->regs[0] & ~0xFFULL) | v;
        break; }
    case 0xA1: { /* MOV rAX, moffs */
        uint64_t moffs = ds->addr64 ? fetch_qword(ds) : ds->addr32 ? fetch_dword(ds) : fetch_word(ds);
        uint64_t lin = seg_ea(s, moffs, ds->seg_ovr >= 0 ? ds->seg_ovr : X86_CPU_SEG_DS);
        if (ds->op64) s->regs[0] = vmem_read64(s, lin);
        else if (ds->op32) set_reg32(s, 0, vmem_read32(s, lin));
        else set_reg16(s, 0, vmem_read16(s, lin));
        break; }
    case 0xA2: { /* MOV moffs8, AL */
        uint64_t moffs = ds->addr64 ? fetch_qword(ds) : ds->addr32 ? fetch_dword(ds) : fetch_word(ds);
        vmem_write8(s, seg_ea(s, moffs, ds->seg_ovr >= 0 ? ds->seg_ovr : X86_CPU_SEG_DS), (uint8_t)s->regs[0]);
        break; }
    case 0xA3: { /* MOV moffs, rAX */
        uint64_t moffs = ds->addr64 ? fetch_qword(ds) : ds->addr32 ? fetch_dword(ds) : fetch_word(ds);
        uint64_t lin = seg_ea(s, moffs, ds->seg_ovr >= 0 ? ds->seg_ovr : X86_CPU_SEG_DS);
        if (ds->op64) vmem_write64(s, lin, s->regs[0]);
        else if (ds->op32) vmem_write32(s, lin, (uint32_t)s->regs[0]);
        else vmem_write16(s, lin, (uint16_t)s->regs[0]);
        break; }

    case 0xA4: { /* MOVS m8, m8 (with REP) */
        uint64_t cnt = ds->rep ? (ds->op64 ? s->regs[1] : ds->op32 ? (uint32_t)s->regs[1] : (uint16_t)s->regs[1]) : 1;
        int di_inc = (s->eflags & EF_DF) ? -1 : 1;
        while (cnt--) {
            uint8_t v = vmem_read8(s, seg_ea(s, ds->addr64 ? s->regs[6] : (uint32_t)s->regs[6], ds->seg_ovr >= 0 ? ds->seg_ovr : X86_CPU_SEG_DS));
            vmem_write8(s, seg_ea(s, ds->addr64 ? s->regs[7] : (uint32_t)s->regs[7], X86_CPU_SEG_ES), v);
            s->regs[6] += di_inc; s->regs[7] += di_inc;
            if (!ds->addr64) { s->regs[6] &= 0xFFFFFFFF; s->regs[7] &= 0xFFFFFFFF; }
        }
        if (ds->rep) { if (ds->op64) s->regs[1] = 0; else if (ds->op32) set_reg32(s, 1, 0); else set_reg16(s, 1, 0); }
        break; }
    case 0xA5: { /* MOVS m, m */
        int sz = ds->op64 ? 8 : ds->op32 ? 4 : 2;
        uint64_t cnt = ds->rep ? (ds->op64 ? s->regs[1] : ds->op32 ? (uint32_t)s->regs[1] : (uint16_t)s->regs[1]) : 1;
        int di_inc = ((s->eflags & EF_DF) ? -sz : sz);
        while (cnt--) {
            uint64_t src_lin = seg_ea(s, ds->addr64 ? s->regs[6] : (uint32_t)s->regs[6], ds->seg_ovr >= 0 ? ds->seg_ovr : X86_CPU_SEG_DS);
            uint64_t dst_lin = seg_ea(s, ds->addr64 ? s->regs[7] : (uint32_t)s->regs[7], X86_CPU_SEG_ES);
            if (sz == 8) vmem_write64(s, dst_lin, vmem_read64(s, src_lin));
            else if (sz == 4) vmem_write32(s, dst_lin, vmem_read32(s, src_lin));
            else vmem_write16(s, dst_lin, vmem_read16(s, src_lin));
            s->regs[6] += di_inc; s->regs[7] += di_inc;
            if (!ds->addr64) { s->regs[6] &= 0xFFFFFFFF; s->regs[7] &= 0xFFFFFFFF; }
        }
        if (ds->rep) { if (ds->op64) s->regs[1] = 0; else if (ds->op32) set_reg32(s, 1, 0); else set_reg16(s, 1, 0); }
        break; }
    case 0xA6: { /* CMPS m8, m8 */
        uint64_t cnt = ds->rep || ds->repne ? (ds->op32 ? (uint32_t)s->regs[1] : (uint16_t)s->regs[1]) : 1;
        int di_inc = (s->eflags & EF_DF) ? -1 : 1;
        while (cnt--) {
            uint8_t a = vmem_read8(s, seg_ea(s, ds->addr32 ? (uint32_t)s->regs[6] : (uint16_t)s->regs[6], ds->seg_ovr >= 0 ? ds->seg_ovr : X86_CPU_SEG_DS));
            uint8_t b = vmem_read8(s, seg_ea(s, ds->addr32 ? (uint32_t)s->regs[7] : (uint16_t)s->regs[7], X86_CPU_SEG_ES));
            (void)alu_op8(s, 7, a, b);
            s->regs[6] += di_inc; s->regs[7] += di_inc;
            if (!ds->addr64) { s->regs[6] &= 0xFFFFFFFF; s->regs[7] &= 0xFFFFFFFF; }
            if (ds->rep && !(s->eflags & EF_ZF)) break;
            if (ds->repne && (s->eflags & EF_ZF)) break;
        }
        if (ds->rep || ds->repne) { if (ds->op32) set_reg32(s, 1, (uint32_t)cnt); else set_reg16(s, 1, (uint16_t)cnt); }
        break; }
    case 0xA7: { /* CMPS m, m */
        int sz = ds->op64 ? 8 : ds->op32 ? 4 : 2;
        uint64_t cnt = ds->rep || ds->repne ? (ds->op32 ? (uint32_t)s->regs[1] : (uint16_t)s->regs[1]) : 1;
        int di_inc = (s->eflags & EF_DF) ? -sz : sz;
        while (cnt--) {
            uint64_t src_lin = seg_ea(s, ds->addr64 ? s->regs[6] : (uint32_t)s->regs[6], ds->seg_ovr >= 0 ? ds->seg_ovr : X86_CPU_SEG_DS);
            uint64_t dst_lin = seg_ea(s, ds->addr64 ? s->regs[7] : (uint32_t)s->regs[7], X86_CPU_SEG_ES);
            if (sz == 8) { uint64_t a = vmem_read64(s, src_lin), b = vmem_read64(s, dst_lin); (void)alu_op64(s, 7, a, b); }
            else if (sz == 4) { uint32_t a = vmem_read32(s, src_lin), b = vmem_read32(s, dst_lin); (void)alu_op32(s, 7, a, b); }
            else { uint16_t a = vmem_read16(s, src_lin), b = vmem_read16(s, dst_lin); (void)alu_op16(s, 7, a, b); }
            s->regs[6] += di_inc; s->regs[7] += di_inc;
            if (!ds->addr64) { s->regs[6] &= 0xFFFFFFFF; s->regs[7] &= 0xFFFFFFFF; }
            if (ds->rep && !(s->eflags & EF_ZF)) break;
            if (ds->repne && (s->eflags & EF_ZF)) break;
        }
        if (ds->rep || ds->repne) { if (ds->op32) set_reg32(s, 1, (uint32_t)cnt); else set_reg16(s, 1, (uint16_t)cnt); }
        break; }
    case 0xA8: { /* TEST AL, imm8 */
        uint8_t imm = fetch_byte(ds);
        flags_logic8(s, (uint8_t)s->regs[0] & imm);
        break; }
    case 0xA9: { /* TEST rAX, imm */
        if (ds->op64) { uint64_t imm = (uint64_t)(int64_t)(int32_t)fetch_dword(ds); flags_logic64(s, s->regs[0] & imm); }
        else if (ds->op32) { uint32_t imm = fetch_dword(ds); flags_logic32(s, (uint32_t)s->regs[0] & imm); }
        else { uint16_t imm = fetch_word(ds); flags_logic16(s, (uint16_t)s->regs[0] & imm); }
        break; }
    case 0xAA: { /* STOS m8 */
        uint64_t cnt = ds->rep ? (ds->addr64 ? s->regs[1] : ds->addr32 ? (uint32_t)s->regs[1] : (uint16_t)s->regs[1]) : 1;
        int di_inc = (s->eflags & EF_DF) ? -1 : 1;
        while (cnt--) {
            vmem_write8(s, seg_ea(s, ds->addr64 ? s->regs[7] : (uint32_t)s->regs[7], X86_CPU_SEG_ES), (uint8_t)s->regs[0]);
            s->regs[7] += di_inc;
            if (!ds->addr64) s->regs[7] &= 0xFFFFFFFF;
        }
        if (ds->rep) { if (ds->addr64) s->regs[1] = 0; else if (ds->addr32) set_reg32(s, 1, 0); else set_reg16(s, 1, 0); }
        break; }
    case 0xAB: { /* STOS m */
        int sz = ds->op64 ? 8 : ds->op32 ? 4 : 2;
        uint64_t cnt = ds->rep ? (ds->addr64 ? s->regs[1] : ds->addr32 ? (uint32_t)s->regs[1] : (uint16_t)s->regs[1]) : 1;
        int di_inc = (s->eflags & EF_DF) ? -sz : sz;
        while (cnt--) {
            uint64_t lin = seg_ea(s, ds->addr64 ? s->regs[7] : (uint32_t)s->regs[7], X86_CPU_SEG_ES);
            if (sz == 8) vmem_write64(s, lin, s->regs[0]);
            else if (sz == 4) vmem_write32(s, lin, (uint32_t)s->regs[0]);
            else vmem_write16(s, lin, (uint16_t)s->regs[0]);
            s->regs[7] += di_inc;
            if (!ds->addr64) s->regs[7] &= 0xFFFFFFFF;
        }
        if (ds->rep) { if (ds->addr64) s->regs[1] = 0; else if (ds->addr32) set_reg32(s, 1, 0); else set_reg16(s, 1, 0); }
        break; }
    case 0xAC: { /* LODS AL, m8 */
        s->regs[0] = (s->regs[0] & ~0xFFULL) | vmem_read8(s, seg_ea(s, ds->addr64 ? s->regs[6] : (uint32_t)s->regs[6], ds->seg_ovr >= 0 ? ds->seg_ovr : X86_CPU_SEG_DS));
        s->regs[6] += (s->eflags & EF_DF) ? -1 : 1;
        if (!ds->addr64) s->regs[6] &= 0xFFFFFFFF;
        break; }
    case 0xAD: { /* LODS m */
        int sz = ds->op64 ? 8 : ds->op32 ? 4 : 2;
        uint64_t lin = seg_ea(s, ds->addr64 ? s->regs[6] : (uint32_t)s->regs[6], ds->seg_ovr >= 0 ? ds->seg_ovr : X86_CPU_SEG_DS);
        if (sz == 8) s->regs[0] = vmem_read64(s, lin);
        else if (sz == 4) set_reg32(s, 0, vmem_read32(s, lin));
        else set_reg16(s, 0, vmem_read16(s, lin));
        s->regs[6] += (s->eflags & EF_DF) ? -sz : sz;
        if (!ds->addr64) s->regs[6] &= 0xFFFFFFFF;
        break; }
    case 0xAE: { /* SCAS AL */
        uint64_t cnt = ds->rep || ds->repne ? (ds->addr64 ? s->regs[1] : ds->addr32 ? (uint32_t)s->regs[1] : (uint16_t)s->regs[1]) : 1;
        int di_inc = (s->eflags & EF_DF) ? -1 : 1;
        while (cnt--) {
            uint8_t v = vmem_read8(s, seg_ea(s, ds->addr64 ? s->regs[7] : (uint32_t)s->regs[7], X86_CPU_SEG_ES));
            (void)alu_op8(s, 7, (uint8_t)s->regs[0], v);
            s->regs[7] += di_inc;
            if (!ds->addr64) s->regs[7] &= 0xFFFFFFFF;
            if (ds->rep && !(s->eflags & EF_ZF)) break;
            if (ds->repne && (s->eflags & EF_ZF)) break;
        }
        if (ds->rep || ds->repne) { if (ds->addr64) s->regs[1] = cnt; else if (ds->addr32) set_reg32(s, 1, (uint32_t)cnt); else set_reg16(s, 1, (uint16_t)cnt); }
        break; }
    case 0xAF: { /* SCAS m */
        int sz = ds->op64 ? 8 : ds->op32 ? 4 : 2;
        uint64_t cnt = ds->rep || ds->repne ? (ds->addr64 ? s->regs[1] : ds->addr32 ? (uint32_t)s->regs[1] : (uint16_t)s->regs[1]) : 1;
        int di_inc = (s->eflags & EF_DF) ? -sz : sz;
        while (cnt--) {
            uint64_t lin = seg_ea(s, ds->addr64 ? s->regs[7] : (uint32_t)s->regs[7], X86_CPU_SEG_ES);
            if (sz == 8) { (void)alu_op64(s, 7, s->regs[0], vmem_read64(s, lin)); }
            else if (sz == 4) { (void)alu_op32(s, 7, (uint32_t)s->regs[0], vmem_read32(s, lin)); }
            else { (void)alu_op16(s, 7, (uint16_t)s->regs[0], vmem_read16(s, lin)); }
            s->regs[7] += di_inc;
            if (!ds->addr64) s->regs[7] &= 0xFFFFFFFF;
            if (ds->rep && !(s->eflags & EF_ZF)) break;
            if (ds->repne && (s->eflags & EF_ZF)) break;
        }
        if (ds->rep || ds->repne) { if (ds->addr64) s->regs[1] = cnt; else if (ds->addr32) set_reg32(s, 1, (uint32_t)cnt); else set_reg16(s, 1, (uint16_t)cnt); }
        break; }

    case 0xB0: case 0xB1: case 0xB2: case 0xB3:
    case 0xB4: case 0xB5: case 0xB6: case 0xB7: { /* MOV r8, imm8 */
        int r = op & 7;
        set_reg8(s, r + (ds->has_rex ? ds->rex_b * 8 : 0), fetch_byte(ds), ds->has_rex);
        break; }
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF: { /* MOV r, imm */
        int r = (op & 7) + ds->rex_b;
        if (ds->op64) set_reg64(s, r, fetch_qword(ds));
        else if (ds->op32) set_reg32(s, r, fetch_dword(ds));
        else set_reg16(s, r, fetch_word(ds));
        break; }

    case 0xC0: { /* GROUP 2 r/m8, imm8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t cnt = fetch_byte(ds);
        uint8_t a = (rm_reg >= 0) ? get_reg8(s, rm_reg, ds->has_rex) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        uint8_t r = do_shift8(s, reg & 7, a, cnt);
        if (rm_reg >= 0) set_reg8(s, rm_reg, r, ds->has_rex);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), r);
        break; }
    case 0xC1: { /* GROUP 2 r/m, imm8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t cnt = fetch_byte(ds);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op64) {
            uint64_t a = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, laddr);
            uint64_t r = do_shift64(s, reg & 7, a, cnt);
            if (rm_reg >= 0) set_reg64(s, rm_reg, r); else vmem_write64(s, laddr, r);
        } else if (ds->op32) {
            uint32_t a = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, laddr);
            uint32_t r = do_shift32(s, reg & 7, a, cnt);
            if (rm_reg >= 0) set_reg32(s, rm_reg, r); else vmem_write32(s, laddr, r);
        } else {
            uint16_t a = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            uint16_t r = do_shift16(s, reg & 7, a, cnt);
            if (rm_reg >= 0) set_reg16(s, rm_reg, r); else vmem_write16(s, laddr, r);
        }
        break; }

    case 0xC2: { /* RET near imm16 */
        uint16_t imm = fetch_word(ds);
        if (is_long_mode(s)) { s->rip = pop64(s); s->regs[4] += imm; }
        else if (ds->op32) { s->rip = pop32(s); s->regs[4] = (s->regs[4] & 0xFFFFFFFF00000000ULL) | ((uint32_t)s->regs[4] + imm); }
        else { s->rip = (uint32_t)pop16(s); s->regs[4] = (s->regs[4] & 0xFFFFFFFF00000000ULL) | ((uint32_t)(uint16_t)s->regs[4] + imm); }
        break; }
    case 0xC3: { /* RET near */
        if (is_long_mode(s)) s->rip = pop64(s);
        else if (ds->op32) s->rip = pop32(s);
        else s->rip = (uint32_t)pop16(s);
        break; }
    case 0xC4: { /* LES - not in 64-bit; use for VEX prefix placeholder */
        if (is_long_mode(s)) raise_exception(s, EXCP_UD);
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        if (rm_reg >= 0) raise_exception(s, EXCP_UD);
        uint64_t lin = seg_ea(s, ea, ea_seg);
        set_reg32(s, reg, vmem_read32(s, lin));
        s->segs[X86_CPU_SEG_ES].sel = vmem_read16(s, lin + 4);
        break; }
    case 0xC5: { /* LDS */
        if (is_long_mode(s)) raise_exception(s, EXCP_UD);
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        if (rm_reg >= 0) raise_exception(s, EXCP_UD);
        uint64_t lin = seg_ea(s, ea, ea_seg);
        set_reg32(s, reg, vmem_read32(s, lin));
        s->segs[X86_CPU_SEG_DS].sel = vmem_read16(s, lin + 4);
        break; }
    case 0xC6: { /* MOV r/m8, imm8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t imm = fetch_byte(ds);
        if (rm_reg >= 0) set_reg8(s, rm_reg, imm, ds->has_rex);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), imm);
        break; }
    case 0xC7: { /* MOV r/m, imm */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op64) {
            uint64_t imm = (uint64_t)(int64_t)(int32_t)fetch_dword(ds);
            if (rm_reg >= 0) set_reg64(s, rm_reg, imm); else vmem_write64(s, laddr, imm);
        } else if (ds->op32) {
            uint32_t imm = fetch_dword(ds);
            if (rm_reg >= 0) set_reg32(s, rm_reg, imm); else vmem_write32(s, laddr, imm);
        } else {
            uint16_t imm = fetch_word(ds);
            if (rm_reg >= 0) set_reg16(s, rm_reg, imm); else vmem_write16(s, laddr, imm);
        }
        break; }

    case 0xC8: { /* ENTER imm16, imm8 */
        uint16_t frame_size = fetch_word(ds);
        uint8_t nesting = fetch_byte(ds);
        (void)nesting;
        if (is_long_mode(s)) {
            push64(s, s->regs[5]);
            uint64_t frame_ptr = s->regs[4];
            s->regs[5] = frame_ptr;
            s->regs[4] -= frame_size;
        } else if (ds->op32) {
            push32(s, (uint32_t)s->regs[5]);
            uint32_t frame_ptr = (uint32_t)s->regs[4];
            set_reg32(s, 5, frame_ptr);
            set_reg32(s, 4, (uint32_t)s->regs[4] - frame_size);
        } else {
            push16(s, (uint16_t)s->regs[5]);
            uint16_t frame_ptr = (uint16_t)s->regs[4];
            set_reg16(s, 5, frame_ptr);
            set_reg16(s, 4, (uint16_t)s->regs[4] - frame_size);
        }
        break; }
    case 0xC9: { /* LEAVE */
        if (is_long_mode(s)) { s->regs[4] = s->regs[5]; s->regs[5] = pop64(s); }
        else if (ds->op32) { set_reg32(s, 4, (uint32_t)s->regs[5]); set_reg32(s, 5, pop32(s)); }
        else { set_reg16(s, 4, get_reg16(s, 5)); set_reg16(s, 5, pop16(s)); }
        break; }

    case 0xCA: { /* RETF imm16 */
        uint16_t imm = fetch_word(ds);
        uint32_t ret_ip, ret_cs;
        ret_ip = pop32(s); ret_cs = pop32(s);
        s->segs[X86_CPU_SEG_CS].sel = (uint16_t)ret_cs;
        s->rip = ret_ip;
        s->regs[4] = (s->regs[4] & 0xFFFFFFFF00000000ULL) | ((uint32_t)s->regs[4] + imm);
        break; }
    case 0xCB: { /* RETF */
        uint32_t ret_ip, ret_cs;
        ret_ip = pop32(s); ret_cs = pop32(s);
        s->segs[X86_CPU_SEG_CS].sel = (uint16_t)ret_cs;
        s->rip = ret_ip;
        break; }

    case 0xCC: /* INT 3 */ x86_do_interrupt(s, 3, FALSE, 0); break;
    case 0xCD: { /* INT imm8 */
        uint8_t n = fetch_byte(ds);
        x86_do_interrupt(s, n, FALSE, 0);
        break; }
    case 0xCE: /* INTO */ if (s->eflags & EF_OF) x86_do_interrupt(s, 4, FALSE, 0); break;
    case 0xCF: { /* IRET/IRETD/IRETQ */
        if (is_long_mode(s)) {
            uint64_t new_rip = pop64(s);
            uint64_t new_cs  = pop64(s);
            uint64_t new_fl  = pop64(s);
            uint64_t new_rsp = pop64(s);
            uint64_t new_ss  = pop64(s);
            (void)new_ss;
            s->rip = new_rip;
            s->segs[X86_CPU_SEG_CS].sel = (uint16_t)new_cs;
            s->eflags = (uint32_t)new_fl | EF_FIXED;
            s->regs[4] = new_rsp;
        } else if (ds->op32) {
            uint32_t new_ip = pop32(s);
            uint32_t new_cs = pop32(s);
            uint32_t new_fl = pop32(s);
            s->rip = new_ip;
            s->segs[X86_CPU_SEG_CS].sel = (uint16_t)new_cs;
            s->eflags = (new_fl & 0x3F7FD5U) | EF_FIXED;
        } else {
            uint16_t new_ip = pop16(s);
            uint16_t new_cs = pop16(s);
            uint16_t new_fl = pop16(s);
            s->rip = new_ip;
            s->segs[X86_CPU_SEG_CS].sel = new_cs;
            s->eflags = (s->eflags & ~0xFFFFU) | new_fl;
        }
        break; }

    case 0xD0: { /* GROUP 2 r/m8, 1 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = (rm_reg >= 0) ? get_reg8(s, rm_reg, ds->has_rex) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        uint8_t r = do_shift8(s, reg & 7, a, 1);
        if (rm_reg >= 0) set_reg8(s, rm_reg, r, ds->has_rex);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), r);
        break; }
    case 0xD1: { /* GROUP 2 r/m, 1 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op64) {
            uint64_t a = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, laddr);
            uint64_t r = do_shift64(s, reg & 7, a, 1);
            if (rm_reg >= 0) set_reg64(s, rm_reg, r); else vmem_write64(s, laddr, r);
        } else if (ds->op32) {
            uint32_t a = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, laddr);
            uint32_t r = do_shift32(s, reg & 7, a, 1);
            if (rm_reg >= 0) set_reg32(s, rm_reg, r); else vmem_write32(s, laddr, r);
        } else {
            uint16_t a = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            uint16_t r = do_shift16(s, reg & 7, a, 1);
            if (rm_reg >= 0) set_reg16(s, rm_reg, r); else vmem_write16(s, laddr, r);
        }
        break; }
    case 0xD2: { /* GROUP 2 r/m8, CL */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = (rm_reg >= 0) ? get_reg8(s, rm_reg, ds->has_rex) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        uint8_t r = do_shift8(s, reg & 7, a, (uint8_t)s->regs[1]);
        if (rm_reg >= 0) set_reg8(s, rm_reg, r, ds->has_rex);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), r);
        break; }
    case 0xD3: { /* GROUP 2 r/m, CL */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t cnt = (uint8_t)s->regs[1];
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op64) {
            uint64_t a = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, laddr);
            uint64_t r = do_shift64(s, reg & 7, a, cnt);
            if (rm_reg >= 0) set_reg64(s, rm_reg, r); else vmem_write64(s, laddr, r);
        } else if (ds->op32) {
            uint32_t a = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, laddr);
            uint32_t r = do_shift32(s, reg & 7, a, cnt);
            if (rm_reg >= 0) set_reg32(s, rm_reg, r); else vmem_write32(s, laddr, r);
        } else {
            uint16_t a = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            uint16_t r = do_shift16(s, reg & 7, a, cnt);
            if (rm_reg >= 0) set_reg16(s, rm_reg, r); else vmem_write16(s, laddr, r);
        }
        break; }

    case 0xD4: /* AAM */ raise_exception(s, EXCP_UD); break;
    case 0xD5: /* AAD */ raise_exception(s, EXCP_UD); break;

    case 0xD8: { /* ESC 0 (FPU) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        (void)reg; (void)rm_reg;
        break; }
    case 0xD9: { /* ESC 1 (FPU) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        (void)reg; (void)rm_reg;
        break; }
    case 0xDA: { /* ESC 2 (FPU) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        (void)reg; (void)rm_reg;
        break; }
    case 0xDB: { /* ESC 3 (FPU) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        (void)reg; (void)rm_reg;
        break; }
    case 0xDC: { /* ESC 4 (FPU) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        (void)reg; (void)rm_reg;
        break; }
    case 0xDD: { /* ESC 5 (FPU) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        (void)reg; (void)rm_reg;
        break; }
    case 0xDE: { /* ESC 6 (FPU) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        (void)reg; (void)rm_reg;
        break; }
    case 0xDF: { /* ESC 7 (FPU) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        (void)reg; (void)rm_reg;
        break; }

    case 0xE0: case 0xE1: case 0xE2: case 0xE3: { /* LOOP/LOOPZ/LOOPNZ/JCXZ */
        int8_t rel = (int8_t)fetch_byte(ds);
        BOOL taken = FALSE;
        if (op == 0xE3) {
            /* JCXZ/JECXZ/JRCXZ */
            uint64_t cx = ds->addr64 ? s->regs[1] : ds->addr32 ? (uint32_t)s->regs[1] : (uint16_t)s->regs[1];
            taken = (cx == 0);
        } else {
            /* Decrement CX/ECX/RCX */
            if (ds->addr64) { s->regs[1]--; uint64_t cx = s->regs[1]; taken = (cx != 0); }
            else if (ds->addr32) { uint32_t cx = (uint32_t)s->regs[1] - 1; set_reg32(s, 1, cx); taken = (cx != 0); }
            else { uint16_t cx = (uint16_t)s->regs[1] - 1; set_reg16(s, 1, cx); taken = (cx != 0); }
            if (op == 0xE0 && taken) taken = !(s->eflags & EF_ZF);
            if (op == 0xE1 && taken) taken =  (s->eflags & EF_ZF);
        }
        if (taken) {
            if (is_long_mode(s)) s->rip = ds->pc + (int64_t)rel;
            else s->rip = (uint32_t)(ds->pc + rel - s->segs[X86_CPU_SEG_CS].base);
        }
        break; }

    case 0xE4: { /* IN AL, imm8 */
        uint8_t port = fetch_byte(ds);
        s->regs[0] = (s->regs[0] & ~0xFFULL) | (uint8_t)port_read(s, port, 0);
        break; }
    case 0xE5: { /* IN AX/EAX, imm8 */
        uint8_t port = fetch_byte(ds);
        if (ds->op32) set_reg32(s, 0, port_read(s, port, 2));
        else set_reg16(s, 0, (uint16_t)port_read(s, port, 1));
        break; }
    case 0xE6: { /* OUT imm8, AL */
        uint8_t port = fetch_byte(ds);
        port_write(s, port, (uint8_t)s->regs[0], 0);
        break; }
    case 0xE7: { /* OUT imm8, AX/EAX */
        uint8_t port = fetch_byte(ds);
        if (ds->op32) port_write(s, port, (uint32_t)s->regs[0], 2);
        else port_write(s, port, (uint16_t)s->regs[0], 1);
        break; }

    case 0xE8: { /* CALL rel near */
        int32_t rel = (int32_t)fetch_dword(ds);
        if (is_long_mode(s)) {
            push64(s, ds->pc);
            s->rip = ds->pc + (int64_t)rel;
        } else if (ds->op32) {
            push32(s, (uint32_t)ds->pc - (uint32_t)s->segs[X86_CPU_SEG_CS].base);
            s->rip = (uint32_t)(ds->pc + rel - s->segs[X86_CPU_SEG_CS].base);
        } else {
            push16(s, (uint16_t)((uint32_t)ds->pc - (uint32_t)s->segs[X86_CPU_SEG_CS].base));
            s->rip = (uint32_t)(uint16_t)(ds->pc + rel - s->segs[X86_CPU_SEG_CS].base);
        }
        break; }
    case 0xE9: { /* JMP rel near */
        int32_t rel = (int32_t)fetch_dword(ds);
        if (is_long_mode(s)) s->rip = ds->pc + (int64_t)rel;
        else s->rip = (uint32_t)(ds->pc + rel - s->segs[X86_CPU_SEG_CS].base);
        break; }
    case 0xEA: { /* JMP far (not in 64-bit) */
        uint32_t off;
        uint16_t sel;
        if (ds->op32) { off = fetch_dword(ds); sel = fetch_word(ds); }
        else { off = fetch_word(ds); sel = fetch_word(ds); }
        load_seg_desc(s, X86_CPU_SEG_CS, sel);
        s->rip = off;
        break; }
    case 0xEB: { /* JMP rel8 */
        int8_t rel = (int8_t)fetch_byte(ds);
        if (is_long_mode(s)) s->rip = ds->pc + (int64_t)rel;
        else s->rip = (uint32_t)(ds->pc + rel - s->segs[X86_CPU_SEG_CS].base);
        break; }

    case 0xEC: { /* IN AL, DX */
        s->regs[0] = (s->regs[0] & ~0xFFULL) | (uint8_t)port_read(s, (uint16_t)s->regs[2], 0);
        break; }
    case 0xED: { /* IN AX/EAX, DX */
        uint16_t port = (uint16_t)s->regs[2];
        if (ds->op32) set_reg32(s, 0, port_read(s, port, 2));
        else set_reg16(s, 0, (uint16_t)port_read(s, port, 1));
        break; }
    case 0xEE: /* OUT DX, AL */ port_write(s, (uint16_t)s->regs[2], (uint8_t)s->regs[0], 0); break;
    case 0xEF: { /* OUT DX, AX/EAX */
        uint16_t port = (uint16_t)s->regs[2];
        if (ds->op32) port_write(s, port, (uint32_t)s->regs[0], 2);
        else port_write(s, port, (uint16_t)s->regs[0], 1);
        break; }

    case 0x0F:
        exec_0f(ds);
        return;

    case 0xF4: /* HLT */ s->power_down = TRUE; break;
    case 0xF5: /* CMC */ s->eflags ^= EF_CF; break;
    case 0xF8: /* CLC */ s->eflags &= ~EF_CF; break;
    case 0xF9: /* STC */ s->eflags |=  EF_CF; break;
    case 0xFA: /* CLI */ s->eflags &= ~EF_IF; break;
    case 0xFB: /* STI */ s->eflags |=  EF_IF; break;
    case 0xFC: /* CLD */ s->eflags &= ~EF_DF; break;
    case 0xFD: /* STD */ s->eflags |=  EF_DF; break;

    case 0xF6: { /* GROUP 3 r/m8 */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = (rm_reg >= 0) ? get_reg8(s, rm_reg, ds->has_rex) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        switch (reg & 7) {
        case 0: case 1: /* TEST r/m8, imm8 */
            flags_logic8(s, a & fetch_byte(ds)); break;
        case 2: /* NOT */
            a = ~a;
            if (rm_reg >= 0) set_reg8(s, rm_reg, a, ds->has_rex);
            else vmem_write8(s, seg_ea(s, ea, ea_seg), a);
            break;
        case 3: /* NEG */
            { uint8_t r = (uint8_t)(0 - a); flags_sub8(s, 0, a, r);
              if (rm_reg >= 0) set_reg8(s, rm_reg, r, ds->has_rex);
              else vmem_write8(s, seg_ea(s, ea, ea_seg), r); }
            break;
        case 4: /* MUL AX = AL * r/m8 */
            { uint16_t r = (uint16_t)(uint8_t)s->regs[0] * a;
              set_reg16(s, 0, r);
              if (r >> 8) s->eflags |= EF_CF|EF_OF; else s->eflags &= ~(EF_CF|EF_OF); }
            break;
        case 5: /* IMUL AX = AL * r/m8 */
            { int16_t r = (int16_t)(int8_t)(uint8_t)s->regs[0] * (int8_t)a;
              set_reg16(s, 0, (uint16_t)r);
              if (r != (int16_t)(int8_t)r) s->eflags |= EF_CF|EF_OF; else s->eflags &= ~(EF_CF|EF_OF); }
            break;
        case 6: /* DIV */
            { uint16_t dividend = (uint16_t)s->regs[0];
              if (a == 0) raise_exception(s, EXCP_DE);
              set_reg8(s, 0, (uint8_t)(dividend / a), TRUE);
              s->regs[0] = (s->regs[0] & ~0xFF00ULL) | ((uint64_t)(dividend % a) << 8); }
            break;
        case 7: /* IDIV */
            { int16_t dividend = (int16_t)(uint16_t)s->regs[0];
              if (a == 0) raise_exception(s, EXCP_DE);
              int8_t q = (int8_t)(dividend / (int8_t)a);
              int8_t r = (int8_t)(dividend % (int8_t)a);
              set_reg8(s, 0, (uint8_t)q, TRUE); /* AL */
              s->regs[0] = (s->regs[0] & ~0xFF00ULL) | ((uint64_t)(uint8_t)r << 8); /* AH */ }
            break;
        }
        break; }

    case 0xF7: { /* GROUP 3 r/m */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        if (ds->op64) {
            uint64_t a = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, laddr);
            switch (reg & 7) {
            case 0: case 1: { uint64_t imm = (uint64_t)(int64_t)(int32_t)fetch_dword(ds); flags_logic64(s, a & imm); break; }
            case 2: a = ~a; if (rm_reg >= 0) set_reg64(s, rm_reg, a); else vmem_write64(s, laddr, a); break;
            case 3: { uint64_t r = (uint64_t)(0 - a); flags_sub64(s, 0, a, r); if (rm_reg >= 0) set_reg64(s, rm_reg, r); else vmem_write64(s, laddr, r); break; }
            case 4: { /* MUL RDX:RAX = RAX * r/m64 */
                __uint128_t r = (__uint128_t)s->regs[0] * a;
                s->regs[0] = (uint64_t)r;
                s->regs[2] = (uint64_t)(r >> 64);
                if (s->regs[2]) s->eflags |= EF_CF|EF_OF; else s->eflags &= ~(EF_CF|EF_OF);
                break; }
            case 5: { /* IMUL */
                __int128_t r = (__int128_t)(int64_t)s->regs[0] * (int64_t)a;
                s->regs[0] = (uint64_t)r;
                s->regs[2] = (uint64_t)((uint64_t)(r >> 64));
                if (s->regs[2] != ((s->regs[0] >> 63) ? UINT64_MAX : 0)) s->eflags |= EF_CF|EF_OF; else s->eflags &= ~(EF_CF|EF_OF);
                break; }
            case 6: { /* DIV RDX:RAX / r/m64 */
                if (a == 0) raise_exception(s, EXCP_DE);
                __uint128_t dividend = ((__uint128_t)s->regs[2] << 64) | s->regs[0];
                s->regs[0] = (uint64_t)(dividend / a);
                s->regs[2] = (uint64_t)(dividend % a);
                break; }
            case 7: { /* IDIV */
                if (a == 0) raise_exception(s, EXCP_DE);
                __int128_t dividend = ((__int128_t)(int64_t)s->regs[2] << 64) | s->regs[0];
                s->regs[0] = (uint64_t)(int64_t)(dividend / (int64_t)a);
                s->regs[2] = (uint64_t)(int64_t)(dividend % (int64_t)a);
                break; }
            }
        } else if (ds->op32) {
            uint32_t a = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, laddr);
            switch (reg & 7) {
            case 0: case 1: { uint32_t imm = fetch_dword(ds); flags_logic32(s, a & imm); break; }
            case 2: a = ~a; if (rm_reg >= 0) set_reg32(s, rm_reg, a); else vmem_write32(s, laddr, a); break;
            case 3: { uint32_t r = (uint32_t)(0 - a); flags_sub32(s, 0, a, r); if (rm_reg >= 0) set_reg32(s, rm_reg, r); else vmem_write32(s, laddr, r); break; }
            case 4: { uint64_t r = (uint64_t)(uint32_t)s->regs[0] * a; set_reg32(s, 0, (uint32_t)r); set_reg32(s, 2, (uint32_t)(r >> 32)); if (s->regs[2]) s->eflags |= EF_CF|EF_OF; else s->eflags &= ~(EF_CF|EF_OF); break; }
            case 5: { int64_t r = (int64_t)(int32_t)s->regs[0] * (int32_t)a; set_reg32(s, 0, (uint32_t)r); set_reg32(s, 2, (uint32_t)(r >> 32)); if (r != (int64_t)(int32_t)(uint32_t)s->regs[0]) s->eflags |= EF_CF|EF_OF; else s->eflags &= ~(EF_CF|EF_OF); break; }
            case 6: { if (a == 0) raise_exception(s, EXCP_DE); uint64_t d = ((uint64_t)(uint32_t)s->regs[2] << 32) | (uint32_t)s->regs[0]; set_reg32(s, 0, (uint32_t)(d / a)); set_reg32(s, 2, (uint32_t)(d % a)); break; }
            case 7: { if (a == 0) raise_exception(s, EXCP_DE); int64_t d = ((int64_t)(int32_t)s->regs[2] << 32) | (uint32_t)s->regs[0]; set_reg32(s, 0, (uint32_t)(d / (int32_t)a)); set_reg32(s, 2, (uint32_t)(d % (int32_t)a)); break; }
            }
        } else {
            uint16_t a = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            switch (reg & 7) {
            case 0: case 1: { uint16_t imm = fetch_word(ds); flags_logic16(s, a & imm); break; }
            case 2: a = ~a; if (rm_reg >= 0) set_reg16(s, rm_reg, a); else vmem_write16(s, laddr, a); break;
            case 3: { uint16_t r = (uint16_t)(0 - a); flags_sub16(s, 0, a, r); if (rm_reg >= 0) set_reg16(s, rm_reg, r); else vmem_write16(s, laddr, r); break; }
            case 4: { uint32_t r = (uint32_t)(uint16_t)s->regs[0] * a; set_reg16(s, 0, (uint16_t)r); set_reg16(s, 2, (uint16_t)(r >> 16)); if (r >> 16) s->eflags |= EF_CF|EF_OF; else s->eflags &= ~(EF_CF|EF_OF); break; }
            case 5: { int32_t r = (int32_t)(int16_t)s->regs[0] * (int16_t)a; set_reg16(s, 0, (uint16_t)r); set_reg16(s, 2, (uint16_t)(r >> 16)); if (r != (int32_t)(int16_t)r) s->eflags |= EF_CF|EF_OF; else s->eflags &= ~(EF_CF|EF_OF); break; }
            case 6: { if (a == 0) raise_exception(s, EXCP_DE); uint32_t d = ((uint32_t)(uint16_t)s->regs[2] << 16) | (uint16_t)s->regs[0]; set_reg16(s, 0, (uint16_t)(d / a)); set_reg16(s, 2, (uint16_t)(d % a)); break; }
            case 7: { if (a == 0) raise_exception(s, EXCP_DE); int32_t d = ((int32_t)(int16_t)s->regs[2] << 16) | (uint16_t)s->regs[0]; set_reg16(s, 0, (uint16_t)(d / (int16_t)a)); set_reg16(s, 2, (uint16_t)(d % (int16_t)a)); break; }
            }
        }
        break; }

    case 0xFE: { /* GROUP 4 r/m8 (INC/DEC) */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        uint8_t a = (rm_reg >= 0) ? get_reg8(s, rm_reg, ds->has_rex) : vmem_read8(s, seg_ea(s, ea, ea_seg));
        uint8_t r;
        if ((reg & 7) == 0) {
            r = a + 1;
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzsb8(r);
            if (r == 0x80) s->eflags |= EF_OF;
            if ((r & 0xF) == 0) s->eflags |= EF_AF;
        } else {
            r = a - 1;
            s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
            s->eflags |= compute_flags_pzsb8(r);
            if (a == 0x80) s->eflags |= EF_OF;
            if ((r & 0xF) == 0xF) s->eflags |= EF_AF;
        }
        if (rm_reg >= 0) set_reg8(s, rm_reg, r, ds->has_rex);
        else vmem_write8(s, seg_ea(s, ea, ea_seg), r);
        break; }

    case 0xFF: { /* GROUP 5 r/m */
        decode_modrm(ds, &reg, &rm_reg, &ea, &ea_seg);
        laddr = (rm_reg < 0) ? seg_ea(s, ea, ea_seg) : 0;
        switch (reg & 7) {
        case 0: { /* INC r/m */
            if (ds->op64) {
                uint64_t a = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, laddr);
                uint64_t r = a + 1;
                s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
                s->eflags |= compute_flags_pzs64(r);
                if (r == (1ULL << 63)) s->eflags |= EF_OF;
                if ((r & 0xF) == 0) s->eflags |= EF_AF;
                if (rm_reg >= 0) set_reg64(s, rm_reg, r); else vmem_write64(s, laddr, r);
            } else if (ds->op32) {
                uint32_t a = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, laddr);
                uint32_t r = a + 1;
                s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
                s->eflags |= compute_flags_pzs32(r);
                if (r == 0x80000000U) s->eflags |= EF_OF;
                if ((r & 0xF) == 0) s->eflags |= EF_AF;
                if (rm_reg >= 0) set_reg32(s, rm_reg, r); else vmem_write32(s, laddr, r);
            } else {
                uint16_t a = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
                uint16_t r = a + 1;
                s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
                s->eflags |= compute_flags_pzs16(r);
                if (r == 0x8000U) s->eflags |= EF_OF;
                if ((r & 0xF) == 0) s->eflags |= EF_AF;
                if (rm_reg >= 0) set_reg16(s, rm_reg, r); else vmem_write16(s, laddr, r);
            }
            break; }
        case 1: { /* DEC r/m */
            if (ds->op64) {
                uint64_t a = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, laddr);
                uint64_t r = a - 1;
                s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
                s->eflags |= compute_flags_pzs64(r);
                if (a == (1ULL << 63)) s->eflags |= EF_OF;
                if ((r & 0xF) == 0xF) s->eflags |= EF_AF;
                if (rm_reg >= 0) set_reg64(s, rm_reg, r); else vmem_write64(s, laddr, r);
            } else if (ds->op32) {
                uint32_t a = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, laddr);
                uint32_t r = a - 1;
                s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
                s->eflags |= compute_flags_pzs32(r);
                if (a == 0x80000000U) s->eflags |= EF_OF;
                if ((r & 0xF) == 0xF) s->eflags |= EF_AF;
                if (rm_reg >= 0) set_reg32(s, rm_reg, r); else vmem_write32(s, laddr, r);
            } else {
                uint16_t a = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
                uint16_t r = a - 1;
                s->eflags &= ~(EF_PF|EF_AF|EF_ZF|EF_SF|EF_OF);
                s->eflags |= compute_flags_pzs16(r);
                if (a == 0x8000U) s->eflags |= EF_OF;
                if ((r & 0xF) == 0xF) s->eflags |= EF_AF;
                if (rm_reg >= 0) set_reg16(s, rm_reg, r); else vmem_write16(s, laddr, r);
            }
            break; }
        case 2: { /* CALL near r/m */
            uint64_t target;
            if (is_long_mode(s)) {
                target = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, laddr);
                push64(s, ds->pc);
                s->rip = target;
            } else if (ds->op32) {
                target = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, laddr);
                push32(s, (uint32_t)ds->pc - (uint32_t)s->segs[X86_CPU_SEG_CS].base);
                s->rip = (uint32_t)target;
            } else {
                target = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
                push16(s, (uint16_t)((uint32_t)ds->pc - (uint32_t)s->segs[X86_CPU_SEG_CS].base));
                s->rip = (uint32_t)(uint16_t)target;
            }
            break; }
        case 3: { /* CALL far */
            uint32_t off = vmem_read32(s, laddr);
            uint16_t sel = vmem_read16(s, laddr + 4);
            push32(s, s->segs[X86_CPU_SEG_CS].sel);
            push32(s, (uint32_t)ds->pc - (uint32_t)s->segs[X86_CPU_SEG_CS].base);
            load_seg_desc(s, X86_CPU_SEG_CS, sel);
            s->rip = off;
            break; }
        case 4: { /* JMP near r/m */
            uint64_t target;
            if (is_long_mode(s)) target = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, laddr);
            else if (ds->op32) target = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, laddr);
            else target = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
            if (is_long_mode(s)) s->rip = target;
            else s->rip = (uint32_t)target;
            break; }
        case 5: { /* JMP far */
            uint32_t off = vmem_read32(s, laddr);
            uint16_t sel = vmem_read16(s, laddr + 4);
            load_seg_desc(s, X86_CPU_SEG_CS, sel);
            s->rip = off;
            break; }
        case 6: { /* PUSH r/m */
            if (is_long_mode(s)) {
                uint64_t v = (rm_reg >= 0) ? get_reg64(s, rm_reg) : vmem_read64(s, laddr);
                push64(s, v);
            } else if (ds->op32) {
                uint32_t v = (rm_reg >= 0) ? get_reg32(s, rm_reg) : vmem_read32(s, laddr);
                push32(s, v);
            } else {
                uint16_t v = (rm_reg >= 0) ? get_reg16(s, rm_reg) : vmem_read16(s, laddr);
                push16(s, v);
            }
            break; }
        default: raise_exception(s, EXCP_UD); break;
        }
        break; }

    default:
        fprintf(stderr, "x86: unhandled opcode 0x%02X at RIP=%llx\n",
                op, (unsigned long long)s->rip);
        raise_exception(s, EXCP_UD);
    }

    /* Update RIP after successful decode (for non-jump instructions) */
    if (is_long_mode(s))
        s->rip = ds->pc;
    else
        s->rip = (uint32_t)(ds->pc - s->segs[X86_CPU_SEG_CS].base);
}/* ------------------------------------------------------------------
 * Interp loop and public API
 * ------------------------------------------------------------------ */

void x86_cpu_flush_tlb_write_range_ram(X86CPUState *s, uint8_t *ram_ptr, size_t ram_size)
{
    tlb_flush_for_ram(s, ram_ptr, ram_size);
}

static void do_interp(X86CPUState *s, int max_cycles)
{
    DecodeState ds;
    ds.cpu        = s;
    ds.fetch_page = ~0ULL;   /* invalid — force refill on first fetch */
    ds.fetch_ptr  = NULL;

    /*
     * setjmp called ONCE per do_interp invocation rather than once per
     * instruction.  On a longjmp (exception), we handle the exception
     * below and fall through to resume the main loop.
     *
     * exc_depth is volatile so its value survives the longjmp stack
     * restoration; it guards against triple-fault infinite loops.
     */
    volatile int exc_depth = 0;
    if (setjmp(s->jmp_env) != 0) {
        if (++exc_depth > MAX_EXCEPTION_DEPTH) {
            /* Triple fault: halt the CPU */
            s->power_down = TRUE;
            return;
        }
        if (s->exception_num >= 0) {
            int n = s->exception_num;
            s->exception_num = -1;
            x86_do_interrupt(s, n,
                             s->exception_has_error_code,
                             s->exception_error_code);
            s->exception_has_error_code = FALSE;
        }
        s->cycle_count++;
        exc_depth = 0;
        /* Fall through to the main loop below */
    }

    while (s->cycle_count < (int64_t)max_cycles) {
        /* Check for halted state */
        if (unlikely(s->power_down)) {
            if (s->irq_level && (s->eflags & EF_IF)) {
                s->power_down = FALSE;
            } else {
                s->cycle_count++;
                continue;
            }
        }

        /* Pending hardware interrupt */
        if (unlikely(s->irq_level && (s->eflags & EF_IF) && s->get_hard_intno)) {
            int intno = s->get_hard_intno(s->get_hard_intno_opaque);
            if (intno >= 0)
                x86_do_interrupt(s, intno, FALSE, 0);
        }

        /* Set up decode PC from CPU mode */
        if (is_long_mode(s))
            ds.pc = s->rip;
        else
            ds.pc = (uint64_t)(uint32_t)(s->segs[X86_CPU_SEG_CS].base + (uint32_t)s->rip);

        /*
         * Invalidate the fetch cache at every instruction boundary.
         * This ensures coherency after any instruction that modifies
         * paging (MOV CR3, INVLPG, etc.) within the previous exec_one.
         */
        ds.fetch_page = ~0ULL;

        exec_one(&ds);
        s->cycle_count++;
    }
}

void x86_cpu_interp(X86CPUState *s, int max_cycles)
{
    if (!(s->cr0 & CR0_PE)) {
        /* Real mode - not fully supported; treat as already in protected mode */
        fprintf(stderr, "x86: real mode not supported\n");
        
    }
    do_interp(s, max_cycles);
    
}

X86CPUState *x86_cpu_init(PhysMemoryMap *mem_map)
{
    X86CPUState *s;
    s = mallocz(sizeof(*s));
    s->mem_map = mem_map;
    s->eflags = EF_FIXED;
    s->exception_num = -1;
    tlb_flush_all(s);
    return s;
}

void x86_cpu_end(X86CPUState *s)
{
    free(s);
}

uint32_t x86_cpu_get_reg(X86CPUState *s, int reg)
{
    switch (reg) {
    case X86_CPU_REG_EIP:  return (uint32_t)s->rip;
    case X86_CPU_REG_CR0:  return s->cr0;
    default:
        if (reg >= 0 && reg < 16) return (uint32_t)s->regs[reg];
        return 0;
    }
}

void x86_cpu_set_reg(X86CPUState *s, int reg, uint32_t val)
{
    switch (reg) {
    case X86_CPU_REG_EIP:  s->rip = val; break;
    case X86_CPU_REG_CR0:  s->cr0 = val; break;
    default:
        if (reg >= 0 && reg < 16) s->regs[reg] = val; /* zero-extend */
        break;
    }
}

void x86_cpu_set_seg(X86CPUState *s, int seg_idx, const X86CPUSeg *sd)
{
    X86CPUSeg *seg = &s->segs[seg_idx];
    seg->sel   = sd->sel;
    seg->base  = sd->base;
    seg->limit = sd->limit;
    seg->flags = sd->flags;
}

void x86_cpu_set_mmap(X86CPUState *s, PhysMemoryMap *mem_map)
{
    s->mem_map = mem_map;
    tlb_flush_all(s);
}

void x86_cpu_set_irq(X86CPUState *s, BOOL set)
{
    s->irq_level = set ? 1 : 0;
}

void x86_cpu_set_get_hard_intno(X86CPUState *s,
                                 int (*get_hard_intno)(void *opaque),
                                 void *opaque)
{
    s->get_hard_intno = get_hard_intno;
    s->get_hard_intno_opaque = opaque;
}

void x86_cpu_set_get_tsc(X86CPUState *s,
                      uint64_t (*get_tsc)(void *opaque), void *opaque)
{
    s->get_tsc = get_tsc;
    s->get_tsc_opaque = opaque;
}

void x86_cpu_set_port_io(X86CPUState *s,
                          DeviceReadFunc *read_func,
                          DeviceWriteFunc *write_func,
                          void *opaque)
{
    s->port_read   = read_func;
    s->port_write  = write_func;
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

