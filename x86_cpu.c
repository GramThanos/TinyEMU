/*
 * x86/x64 CPU emulator using the Unicorn Engine library
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
 *
 * This file implements x86/x64 CPU emulation without KVM by delegating to the
 * Unicorn Engine (https://www.unicorn-engine.org/), a lightweight CPU emulator
 * framework based on QEMU's TCG JIT.  Unicorn handles 16-bit real mode,
 * 32-bit protected mode, and 64-bit long mode transparently.
 *
 * Memory regions registered with PhysMemoryMap are mapped into Unicorn on
 * first access via the UC_HOOK_MEM_UNMAPPED hook:
 *   - RAM regions  -> uc_mem_map_ptr()  (zero-copy, host pointer shared)
 *   - MMIO regions -> uc_mmio_map()     (read/write callbacks forwarded to
 *                                        the TinyEMU device model)
 *
 * Hardware IRQs are injected by manually pushing an interrupt stack frame
 * when EFLAGS.IF is set, matching the x86 protected-mode / long-mode ABI.
 *
 * Port I/O (IN / OUT instructions) is intercepted via UC_HOOK_INSN and
 * forwarded to the registered port_read / port_write callbacks.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>

#include <unicorn/unicorn.h>

#include "cutils.h"
#include "iomem.h"
#include "x86_cpu.h"

/* ---- EFLAGS bit masks ---- */
#define EFLAGS_CF   (1U <<  0)
#define EFLAGS_PF   (1U <<  2)
#define EFLAGS_AF   (1U <<  4)
#define EFLAGS_ZF   (1U <<  6)
#define EFLAGS_SF   (1U <<  7)
#define EFLAGS_TF   (1U <<  8)
#define EFLAGS_IF   (1U <<  9)
#define EFLAGS_DF   (1U << 10)
#define EFLAGS_OF   (1U << 11)
#define EFLAGS_RF   (1U << 16)
#define EFLAGS_VM   (1U << 17)

#define X86_OPCODE_HLT      0xf4U  /* HLT instruction opcode */
#define X86_OPCODE_2B_0F    0x0fU
#define X86_OPCODE_3B_0F01  0x01U
#define X86_OPCODE_3B_0F31  0x31U /* RDTSC */
#define X86_OPCODE_3B_0F1E  0x1eU
#define X86_OPCODE_3B_0FCA  0xcaU /* CLAC */
#define X86_OPCODE_3B_0FCB  0xcbU /* STAC */
#define X86_OPCODE_3B_0FF9  0xf9U /* RDTSCP */
#define X86_PREFIX_F3       0xf3U
#define X86_ENDBR64_LAST    0xfaU
#define X86_ENDBR32_LAST    0xfbU
/* Unicorn timeout in microseconds per x86_cpu_interp() slice */
#define INTERP_TIMEOUT_US   5000   /* 5 ms */

/*
 * Per-MMIO region helper.  One instance is heap-allocated for each MMIO range
 * that Unicorn maps via uc_mmio_map().  The Unicorn MMIO callbacks receive this
 * as their user_data so they can look up the backing PhysMemoryRange.
 */
typedef struct {
    uint64_t         base_addr; /* physical base of the region */
    PhysMemoryRange *pr;        /* TinyEMU device range */
} MMIOInfo;

/*
 * X86CPUState – the opaque handle returned by x86_cpu_init().
 *
 * Unicorn owns the CPU register file and TCG JIT state.  We keep only the
 * minimal host-side bookkeeping needed to glue Unicorn to the TinyEMU device
 * model.
 */
struct X86CPUState {
    uc_engine       *uc;
    PhysMemoryMap   *mem_map;

    /* ---- device-model callbacks ---- */
    int            (*get_hard_intno)(void *opaque);
    void            *hard_intno_opaque;

    uint64_t       (*get_tsc)(void *opaque);
    void            *tsc_opaque;

    DeviceReadFunc  *port_read;
    DeviceWriteFunc *port_write;
    void            *port_opaque;

    /* ---- runtime state ---- */
    BOOL             power_down;   /* CPU halted (HLT executed) */
    int              irq_level;    /* 1 = IRQ line asserted */
    int64_t          cycles;       /* approximate cycle counter */

    /*
     * Tracking which PhysMemoryRanges have already been handed to Unicorn.
     * We mirror the PhysMemoryMap index so we can detect new registrations
     * between interp calls (devices registered after init).
     *
     * range_mapped[i] is TRUE once phys_mem_range[i] has been mapped into
     * Unicorn, for both RAM and MMIO regions.
     *
     * mmio_info[i] is non-NULL iff phys_mem_range[i] is an MMIO region that
     * has been mapped.  We own these allocations and free them in x86_cpu_end().
     */
    int       mapped_count;                           /* snapshot of n_phys_mem_range at last sync */
    BOOL      range_mapped[PHYS_MEM_RANGE_MAX];       /* TRUE once range is mapped (RAM or MMIO) */
    MMIOInfo *mmio_info[PHYS_MEM_RANGE_MAX];          /* one entry per MMIO range slot */
};

/* ======================================================================
 * Physical-memory helpers (operate on Unicorn's address space)
 * ====================================================================== */

static uint8_t phys_read8(X86CPUState *s, uint64_t addr)
{
    uint8_t v = 0;
    uc_mem_read(s->uc, addr, &v, sizeof v);
    return v;
}

static uint16_t phys_read16(X86CPUState *s, uint64_t addr)
{
    uint16_t v = 0;
    uc_mem_read(s->uc, addr, &v, sizeof v);
    return v;
}

static uint32_t phys_read32(X86CPUState *s, uint64_t addr)
{
    uint32_t v = 0;
    uc_mem_read(s->uc, addr, &v, sizeof v);
    return v;
}

static void phys_write32(X86CPUState *s, uint64_t addr, uint32_t v)
{
    uc_mem_write(s->uc, addr, &v, sizeof v);
}

static void phys_write64(X86CPUState *s, uint64_t addr, uint64_t v)
{
    uc_mem_write(s->uc, addr, &v, sizeof v);
}

/* ======================================================================
 * MMIO dispatch callbacks (called by Unicorn for MMIO regions)
 * ====================================================================== */

static uint64_t mmio_read_cb(uc_engine *uc, uint64_t offset,
                              unsigned size, void *user_data)
{
    MMIOInfo        *info = user_data;
    PhysMemoryRange *pr   = info->pr;
    int              size_log2;

    switch (size) {
    case 1:  size_log2 = 0; break;
    case 2:  size_log2 = 1; break;
    default: size_log2 = 2; break;
    }

    if ((pr->devio_flags >> size_log2) & 1)
        return pr->read_func(pr->opaque, (uint32_t)offset, size_log2);

    /* Synthesise wider access from 8-bit reads if the device supports them */
    if (size_log2 == 1 && (pr->devio_flags & DEVIO_SIZE8)) {
        uint32_t lo = pr->read_func(pr->opaque, (uint32_t)offset,     0) & 0xff;
        uint32_t hi = pr->read_func(pr->opaque, (uint32_t)offset + 1, 0) & 0xff;
        return lo | (hi << 8);
    }

    return ~0ULL; /* bus floating */
}

static void mmio_write_cb(uc_engine *uc, uint64_t offset,
                           unsigned size, uint64_t value, void *user_data)
{
    MMIOInfo        *info = user_data;
    PhysMemoryRange *pr   = info->pr;
    int              size_log2;

    switch (size) {
    case 1:  size_log2 = 0; break;
    case 2:  size_log2 = 1; break;
    default: size_log2 = 2; break;
    }

    if ((pr->devio_flags >> size_log2) & 1) {
        pr->write_func(pr->opaque, (uint32_t)offset, (uint32_t)value, size_log2);
        return;
    }

    if (size_log2 == 1 && (pr->devio_flags & DEVIO_SIZE8)) {
        pr->write_func(pr->opaque, (uint32_t)offset,     (uint32_t)(value & 0xff),       0);
        pr->write_func(pr->opaque, (uint32_t)offset + 1, (uint32_t)((value >> 8) & 0xff), 0);
    }
}

/* ======================================================================
 * Memory-map synchronisation
 *
 * Walk the PhysMemoryMap and register any ranges not yet known to Unicorn.
 * Called at the top of every x86_cpu_interp() to pick up devices added after
 * init (e.g. the VGA framebuffer, IDE controller, etc.).
 * ====================================================================== */

static void sync_mem_map(X86CPUState *s)
{
    PhysMemoryMap *map = s->mem_map;
    int            n   = map->n_phys_mem_range;

    for (int i = s->mapped_count; i < n; i++) {
        PhysMemoryRange *pr = &map->phys_mem_range[i];

        if (pr->size == 0)
            continue; /* disabled region */

        if (pr->is_ram) {
            /* Direct host-pointer mapping (zero-copy) */
            uint32_t prot = UC_PROT_READ | UC_PROT_EXEC;
            if (!(pr->devram_flags & DEVRAM_FLAG_ROM))
                prot |= UC_PROT_WRITE;

            /*
             * uc_mem_map_ptr() requires the host pointer to be page-aligned.
             * For large allocations (typical RAM / BIOS) glibc malloc delegates
             * to mmap and guarantees page alignment.  For small regions we fall
             * back to uc_mem_map() + copy; the trade-off is that writes by the
             * emulated CPU won't propagate back to pr->phys_mem, so this path
             * is only safe for ROM regions.
             */
            uc_err err;
            if (((uintptr_t)pr->phys_mem & (DEVRAM_PAGE_SIZE - 1)) == 0) {
                err = uc_mem_map_ptr(s->uc, pr->addr, pr->size,
                                     prot, pr->phys_mem);
            } else {
                err = uc_mem_map(s->uc, pr->addr, pr->size, prot);
                if (err == UC_ERR_OK)
                    uc_mem_write(s->uc, pr->addr, pr->phys_mem, pr->size);
            }
            if (err == UC_ERR_OK)
                s->range_mapped[i] = TRUE;
        } else {
            /* MMIO region – allocate a persistent MMIOInfo and call mmio_map */
            if (pr->devio_flags & DEVIO_DISABLED)
                continue;

            MMIOInfo *info = calloc(1, sizeof *info);
            if (!info)
                continue;

            info->base_addr = pr->addr;
            info->pr        = pr;

            if (uc_mmio_map(s->uc, pr->addr, pr->size,
                            mmio_read_cb,  info,
                            mmio_write_cb, info) != UC_ERR_OK) {
                free(info);
                info = NULL;
            } else {
                s->range_mapped[i] = TRUE;
            }
            s->mmio_info[i] = info;
        }
    }

    s->mapped_count = n;
}

/* ======================================================================
 * UC_HOOK_MEM_UNMAPPED – on-demand mapping for ranges added after the
 * initial sync_mem_map() (e.g. hotplug memory or late device registration).
 * ====================================================================== */

static bool hook_mem_invalid(uc_engine *uc, uc_mem_type type,
                              uint64_t address, int size,
                              int64_t value, void *user_data)
{
    X86CPUState     *s  = user_data;
    PhysMemoryRange *pr = get_phys_mem_range(s->mem_map, address);

    if (!pr || pr->size == 0)
        return false;

    /* Find the index of this range and check if already mapped */
    int idx = -1;
    for (int i = 0; i < s->mem_map->n_phys_mem_range; i++) {
        if (&s->mem_map->phys_mem_range[i] == pr) { idx = i; break; }
    }
    if (idx < 0)
        return false;

    if (s->range_mapped[idx])
        return true; /* already registered with Unicorn, spurious callback */

    if (pr->is_ram) {
        uint32_t prot = UC_PROT_READ | UC_PROT_EXEC;
        if (!(pr->devram_flags & DEVRAM_FLAG_ROM))
            prot |= UC_PROT_WRITE;

        uc_err err;
        if (((uintptr_t)pr->phys_mem & (DEVRAM_PAGE_SIZE - 1)) == 0) {
            err = uc_mem_map_ptr(uc, pr->addr, pr->size, prot, pr->phys_mem);
        } else {
            err = uc_mem_map(uc, pr->addr, pr->size, prot);
            if (err == UC_ERR_OK)
                uc_mem_write(uc, pr->addr, pr->phys_mem, pr->size);
        }
        if (err == UC_ERR_OK)
            s->range_mapped[idx] = TRUE;
        return err == UC_ERR_OK;
    } else {
        if (pr->devio_flags & DEVIO_DISABLED)
            return false;

        MMIOInfo *info = calloc(1, sizeof *info);
        if (!info)
            return false;

        info->base_addr = pr->addr;
        info->pr        = pr;

        if (uc_mmio_map(uc, pr->addr, pr->size,
                        mmio_read_cb, info,
                        mmio_write_cb, info) != UC_ERR_OK) {
            free(info);
            return false;
        }
        s->mmio_info[idx]    = info;
        s->range_mapped[idx] = TRUE;
        return true;
    }
}

/* ======================================================================
 * Port I/O hooks (IN / OUT instructions)
 * ====================================================================== */

static uint32_t hook_insn_in(uc_engine *uc, uint32_t port,
                              int size, void *user_data)
{
    X86CPUState *s = user_data;
    int size_log2;

    if (!s->port_read)
        return 0xffffffff;

    switch (size) {
    case 1:  size_log2 = 0; break;
    case 2:  size_log2 = 1; break;
    default: size_log2 = 2; break;
    }
    return s->port_read(s->port_opaque, port, size_log2);
}

static void hook_insn_out(uc_engine *uc, uint32_t port,
                           int size, uint32_t value, void *user_data)
{
    X86CPUState *s = user_data;
    int size_log2;

    if (!s->port_write)
        return;

    switch (size) {
    case 1:  size_log2 = 0; break;
    case 2:  size_log2 = 1; break;
    default: size_log2 = 2; break;
    }
    s->port_write(s->port_opaque, port, value, size_log2);
}

/* ======================================================================
 * Invalid instruction fallback
 *
 * Newer Linux kernels may emit CET ENDBR instructions even when CET is not
 * enabled at runtime. If Unicorn does not decode them, we treat ENDBR as NOP.
 * We also provide minimal emulation for RDTSC/RDTSCP and AC-flag controls
 * (CLAC/STAC) to avoid hard failures on older Unicorn builds.
 * ====================================================================== */

static bool hook_insn_invalid(uc_engine *uc, void *user_data)
{
    X86CPUState *s = user_data;
    uint64_t rip = 0;
    uint8_t code[6];
    uint32_t eax, edx, ecx, eflags;
    uint64_t tsc;

    if (uc_reg_read(uc, UC_X86_REG_RIP, &rip) != UC_ERR_OK)
        return false;
    if (uc_mem_read(uc, rip, code, sizeof(code)) != UC_ERR_OK)
        return false;

    /* ENDBR64/ENDBR32: F3 0F 1E FA / F3 0F 1E FB -> architectural NOP */
    if (code[0] == X86_PREFIX_F3 &&
        code[1] == X86_OPCODE_2B_0F &&
        code[2] == X86_OPCODE_3B_0F1E &&
        (code[3] == X86_ENDBR64_LAST || code[3] == X86_ENDBR32_LAST)) {
        rip += 4;
        uc_reg_write(uc, UC_X86_REG_RIP, &rip);
        return true;
    }

    /* RDTSC: 0F 31 */
    if (code[0] == X86_OPCODE_2B_0F &&
        code[1] == X86_OPCODE_3B_0F31) {
        tsc = s->get_tsc ? s->get_tsc(s->tsc_opaque) : (uint64_t)s->cycles;
        eax = (uint32_t)tsc;
        edx = (uint32_t)(tsc >> 32);
        rip += 2;
        uc_reg_write(uc, UC_X86_REG_EAX, &eax);
        uc_reg_write(uc, UC_X86_REG_EDX, &edx);
        uc_reg_write(uc, UC_X86_REG_RIP, &rip);
        return true;
    }

    /* RDTSCP: 0F 01 F9 */
    if (code[0] == X86_OPCODE_2B_0F &&
        code[1] == X86_OPCODE_3B_0F01 &&
        code[2] == X86_OPCODE_3B_0FF9) {
        tsc = s->get_tsc ? s->get_tsc(s->tsc_opaque) : (uint64_t)s->cycles;
        eax = (uint32_t)tsc;
        edx = (uint32_t)(tsc >> 32);
        ecx = 0; /* IA32_TSC_AUX */
        rip += 3;
        uc_reg_write(uc, UC_X86_REG_EAX, &eax);
        uc_reg_write(uc, UC_X86_REG_EDX, &edx);
        uc_reg_write(uc, UC_X86_REG_ECX, &ecx);
        uc_reg_write(uc, UC_X86_REG_RIP, &rip);
        return true;
    }

    /* CLAC/STAC: 0F 01 CA / 0F 01 CB */
    if (code[0] == X86_OPCODE_2B_0F &&
        code[1] == X86_OPCODE_3B_0F01 &&
        (code[2] == X86_OPCODE_3B_0FCA || code[2] == X86_OPCODE_3B_0FCB)) {
        if (uc_reg_read(uc, UC_X86_REG_EFLAGS, &eflags) == UC_ERR_OK) {
            if (code[2] == X86_OPCODE_3B_0FCA)
                eflags &= ~(1U << 18); /* AC */
            else
                eflags |= (1U << 18);  /* AC */
            uc_reg_write(uc, UC_X86_REG_EFLAGS, &eflags);
        }
        rip += 3;
        uc_reg_write(uc, UC_X86_REG_RIP, &rip);
        return true;
    }

    fprintf(stderr,
            "x86_cpu: unsupported instruction after fallback emulation at "
            "RIP=%" PRIx64 " bytes=%02x %02x %02x %02x %02x %02x\n",
            rip, code[0], code[1], code[2], code[3], code[4], code[5]);
    return false;
}

/* ======================================================================
 * Hardware interrupt injection
 *
 * Unicorn has no native IRQ injection API, so we simulate it by directly
 * pushing the x86 interrupt stack frame and redirecting execution to the
 * IDT handler.  Only same-privilege-level delivery (ring 0) is implemented
 * here, which covers the TinyEMU kernel boot scenario.
 * ====================================================================== */

static void inject_irq(X86CPUState *s, int intno)
{
    uint32_t    eflags = 0;
    uc_x86_mmr  idtr, cs_mmr;

    uc_reg_read(s->uc, UC_X86_REG_EFLAGS, &eflags);
    if (!(eflags & EFLAGS_IF))
        return; /* interrupts disabled */

    uc_reg_read(s->uc, UC_X86_REG_IDTR, &idtr);
    uc_reg_read(s->uc, UC_X86_REG_CS,   &cs_mmr);

    /* CS.L bit (bit 13 of the flags word) distinguishes 64-bit vs 32-bit */
    int is_64 = (cs_mmr.flags >> 13) & 1;

    if (is_64) {
        /* 64-bit long-mode: IDT entries are 16 bytes */
        if (idtr.limit < (uint32_t)intno * 16U + 15U)
            return;

        uint64_t gate = idtr.base + (uint64_t)intno * 16;
        uint64_t handler =
            (uint64_t)phys_read16(s, gate + 0)          |
            ((uint64_t)phys_read16(s, gate + 6) << 16)  |
            ((uint64_t)phys_read32(s, gate + 8) << 32);
        uint8_t  attrs = phys_read8(s, gate + 5);

        if (!(attrs & 0x80))
            return; /* gate not present */

        uint64_t rip, rsp, rflags = eflags;
        uc_reg_read(s->uc, UC_X86_REG_RIP, &rip);
        uc_reg_read(s->uc, UC_X86_REG_RSP, &rsp);

        /* Same-privilege interrupt frame: RFLAGS, CS, RIP */
        rsp -= 8; phys_write64(s, rsp, rflags);
        rsp -= 8; phys_write64(s, rsp, (uint64_t)cs_mmr.selector);
        rsp -= 8; phys_write64(s, rsp, rip);

        uc_reg_write(s->uc, UC_X86_REG_RSP, &rsp);
        uc_reg_write(s->uc, UC_X86_REG_RIP, &handler);

        /* Interrupt gate clears IF, TF, RF */
        if ((attrs & 0xf) == 0xe) {
            eflags &= ~(EFLAGS_IF | EFLAGS_TF | EFLAGS_RF);
            uc_reg_write(s->uc, UC_X86_REG_EFLAGS, &eflags);
        }
    } else {
        /* 32-bit protected mode: IDT entries are 8 bytes */
        if (idtr.limit < (uint32_t)intno * 8U + 7U)
            return;

        uint64_t gate = idtr.base + (uint64_t)intno * 8;
        uint32_t handler =
            (uint32_t)phys_read16(s, gate + 0)          |
            ((uint32_t)phys_read16(s, gate + 6) << 16);
        uint8_t  attrs = phys_read8(s, gate + 5);

        if (!(attrs & 0x80))
            return; /* gate not present */

        uint32_t esp = 0;
        uint64_t rip = 0;
        uc_reg_read(s->uc, UC_X86_REG_ESP, &esp);
        uc_reg_read(s->uc, UC_X86_REG_RIP, &rip);
        uint32_t eip = (uint32_t)rip;

        /* Same-privilege interrupt frame: EFLAGS, CS, EIP */
        esp -= 4; phys_write32(s, esp, eflags);
        esp -= 4; phys_write32(s, esp, (uint32_t)cs_mmr.selector);
        esp -= 4; phys_write32(s, esp, eip);

        uint64_t new_esp = esp;
        uint64_t new_eip = handler;
        uc_reg_write(s->uc, UC_X86_REG_ESP, &new_esp);
        uc_reg_write(s->uc, UC_X86_REG_EIP, &new_eip);

        /* Interrupt gate clears IF, TF */
        if ((attrs & 0xf) == 0xe) {
            eflags &= ~(EFLAGS_IF | EFLAGS_TF);
            uc_reg_write(s->uc, UC_X86_REG_EFLAGS, &eflags);
        }
    }
}

/* ======================================================================
 * Register-ID mappings (TinyEMU API index -> Unicorn register ID)
 * ====================================================================== */

/* GPRs 0-7 in TinyEMU order: EAX ECX EDX EBX ESP EBP ESI EDI */
static const int gpr_uc_id[8] = {
    UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
    UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI,
};

/* Segment register indices X86_CPU_SEG_* -> Unicorn register ID */
static const int seg_uc_id[10] = {
    UC_X86_REG_ES,   /* X86_CPU_SEG_ES  = 0 */
    UC_X86_REG_CS,   /* X86_CPU_SEG_CS  = 1 */
    UC_X86_REG_SS,   /* X86_CPU_SEG_SS  = 2 */
    UC_X86_REG_DS,   /* X86_CPU_SEG_DS  = 3 */
    UC_X86_REG_FS,   /* X86_CPU_SEG_FS  = 4 */
    UC_X86_REG_GS,   /* X86_CPU_SEG_GS  = 5 */
    UC_X86_REG_LDTR, /* X86_CPU_SEG_LDT = 6 */
    UC_X86_REG_TR,   /* X86_CPU_SEG_TR  = 7 */
    UC_X86_REG_GDTR, /* X86_CPU_SEG_GDT = 8 */
    UC_X86_REG_IDTR, /* X86_CPU_SEG_IDT = 9 */
};

/* ======================================================================
 * Public API
 * ====================================================================== */

X86CPUState *x86_cpu_init(PhysMemoryMap *mem_map)
{
    X86CPUState *s;
    uc_err       err;
    uc_hook      h;

    s = calloc(1, sizeof *s);
    if (!s)
        return NULL;

    s->mem_map = mem_map;

    /*
     * Open the engine in 64-bit mode so that it can handle both 32-bit
     * compatibility mode (CS.L = 0) and 64-bit long mode (CS.L = 1).
     * Unicorn / QEMU manages the mode transition internally.
     */
    err = uc_open(UC_ARCH_X86, UC_MODE_64, &s->uc);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "x86_cpu_init: uc_open failed: %s\n",
                uc_strerror(err));
        free(s);
        return NULL;
    }

    /* On-demand memory mapping for regions not yet known to Unicorn */
    uc_hook_add(s->uc, &h, UC_HOOK_MEM_UNMAPPED,
                hook_mem_invalid, s, 1, 0);

    /* Fallback decode/emulation path for instructions Unicorn rejects */
    uc_hook_add(s->uc, &h, UC_HOOK_INSN_INVALID,
                hook_insn_invalid, s, 1, 0);

    /* Port I/O intercept */
    uc_hook_add(s->uc, &h, UC_HOOK_INSN,
                hook_insn_in, s, 1, 0, UC_X86_INS_IN);
    uc_hook_add(s->uc, &h, UC_HOOK_INSN,
                hook_insn_out, s, 1, 0, UC_X86_INS_OUT);

    return s;
}

void x86_cpu_end(X86CPUState *s)
{
    if (!s)
        return;

    if (s->uc)
        uc_close(s->uc);

    /* Free per-MMIO-region info structs we allocated */
    for (int i = 0; i < PHYS_MEM_RANGE_MAX; i++) {
        if (s->mmio_info[i]) {
            free(s->mmio_info[i]);
            s->mmio_info[i] = NULL;
        }
    }

    free(s);
}

void x86_cpu_interp(X86CPUState *s, int max_cycles)
{
    if (s->power_down)
        return;

    /* Ensure all currently registered memory regions are visible to Unicorn */
    sync_mem_map(s);

    /* Attempt hardware IRQ delivery if IF is set */
    if (s->irq_level && s->get_hard_intno) {
        int intno = s->get_hard_intno(s->hard_intno_opaque);
        if (intno >= 0)
            inject_irq(s, intno);
    }

    /* Read the current program counter to pass as the start address */
    uint64_t rip = 0;
    uc_reg_read(s->uc, UC_X86_REG_RIP, &rip);

    /*
     * Execute up to max_cycles instructions, or for INTERP_TIMEOUT_US
     * microseconds, whichever comes first.  Passing until=0 means "don't
     * stop at a specific address" (Unicorn v2 semantics).
     */
    uc_err err = uc_emu_start(s->uc, rip, 0,
                               INTERP_TIMEOUT_US, (uint64_t)max_cycles);

    s->cycles += max_cycles;

    if (err == UC_ERR_OK || err == UC_ERR_EXCEPTION) {
        /*
         * Check whether execution stopped because of a HLT instruction.
         * HLT (opcode 0xF4) causes Unicorn to stop emulation with UC_ERR_OK.
         * The PC stays on the HLT, so we detect it by inspecting the byte.
         */
        uint64_t new_rip = 0;
        uc_reg_read(s->uc, UC_X86_REG_RIP, &new_rip);
        uint8_t opcode = 0;
        if (uc_mem_read(s->uc, new_rip, &opcode, 1) == UC_ERR_OK &&
            opcode == X86_OPCODE_HLT) {
            s->power_down = TRUE;
        }
    } else if (err != UC_ERR_FETCH_UNMAPPED &&
               err != UC_ERR_READ_UNMAPPED   &&
               err != UC_ERR_WRITE_UNMAPPED) {
        /*
         * Unmapped-memory errors are expected during the first execution
         * of a region (before the UC_HOOK_MEM_UNMAPPED callback has fired
         * to map it).  All other errors are unexpected.
         */
        fprintf(stderr, "x86_cpu_interp: uc_emu_start error: %s\n",
                uc_strerror(err));
    }
}

void x86_cpu_set_irq(X86CPUState *s, BOOL set)
{
    s->irq_level = set ? 1 : 0;

    /* Wake from HLT when an interrupt arrives */
    if (set && s->power_down)
        s->power_down = FALSE;
}

void x86_cpu_set_reg(X86CPUState *s, int reg, uint32_t val)
{
    uint64_t v64 = val; /* zero-extend to 64 bits */

    switch (reg) {
    case 0 ... 7:
        uc_reg_write(s->uc, gpr_uc_id[reg], &v64);
        break;
    case X86_CPU_REG_EIP:
        uc_reg_write(s->uc, UC_X86_REG_EIP, &v64);
        break;
    case X86_CPU_REG_CR0:
        uc_reg_write(s->uc, UC_X86_REG_CR0, &v64);
        break;
    case X86_CPU_REG_CR2:
        uc_reg_write(s->uc, UC_X86_REG_CR2, &v64);
        break;
    default:
        break;
    }
}

uint32_t x86_cpu_get_reg(X86CPUState *s, int reg)
{
    uint64_t v64 = 0;

    switch (reg) {
    case 0 ... 7:
        uc_reg_read(s->uc, gpr_uc_id[reg], &v64);
        break;
    case X86_CPU_REG_EIP:
        uc_reg_read(s->uc, UC_X86_REG_EIP, &v64);
        break;
    case X86_CPU_REG_CR0:
        uc_reg_read(s->uc, UC_X86_REG_CR0, &v64);
        break;
    case X86_CPU_REG_CR2:
        uc_reg_read(s->uc, UC_X86_REG_CR2, &v64);
        break;
    default:
        break;
    }
    return (uint32_t)v64;
}

void x86_cpu_set_seg(X86CPUState *s, int seg, const X86CPUSeg *sd)
{
    if (seg < 0 || seg >= 10)
        return;

    uc_x86_mmr mmr = { 0 };
    mmr.selector = sd->sel;
    mmr.base     = sd->base;
    mmr.limit    = sd->limit;
    mmr.flags    = sd->flags;

    /*
     * GDTR/IDTR only consume base+limit. Keep selector/flags cleared for
     * these pseudo-segments so callers don't need to initialize unused fields.
     */
    if (seg == X86_CPU_SEG_GDT || seg == X86_CPU_SEG_IDT) {
        mmr.selector = 0;
        mmr.flags = 0;
    }

    uc_reg_write(s->uc, seg_uc_id[seg], &mmr);
}

void x86_cpu_set_get_hard_intno(X86CPUState *s,
                                int (*get_hard_intno)(void *opaque),
                                void *opaque)
{
    s->get_hard_intno   = get_hard_intno;
    s->hard_intno_opaque = opaque;
}

void x86_cpu_set_get_tsc(X86CPUState *s,
                          uint64_t (*get_tsc)(void *opaque),
                          void *opaque)
{
    s->get_tsc   = get_tsc;
    s->tsc_opaque = opaque;
    /* RDTSC is handled internally by Unicorn; this callback is unused
     * but stored for future use with a RDTSC instruction hook. */
}

void x86_cpu_set_port_io(X86CPUState *s,
                          DeviceReadFunc *port_read,
                          DeviceWriteFunc *port_write,
                          void *opaque)
{
    s->port_read   = port_read;
    s->port_write  = port_write;
    s->port_opaque = opaque;
}

int64_t x86_cpu_get_cycles(X86CPUState *s)
{
    return s->cycles;
}

BOOL x86_cpu_get_power_down(X86CPUState *s)
{
    return s->power_down;
}

void x86_cpu_flush_tlb_write_range_ram(X86CPUState *s,
                                        uint8_t *ram_ptr, size_t ram_size)
{
    /*
     * Unicorn manages its own internal TLB.  When TinyEMU marks pages dirty
     * we have nothing extra to flush because we use uc_mem_map_ptr() with
     * the host pointer shared directly – Unicorn sees writes immediately.
     */
    (void)ram_ptr;
    (void)ram_size;
}
