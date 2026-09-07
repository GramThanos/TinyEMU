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

/* Hidden segment descriptor cache (matches the fields the machine sets). */
typedef struct {
    uint16_t sel;
    uint16_t flags;   /* access-rights/flags word as passed by the machine */
    uint32_t base;
    uint32_t limit;
} X86Seg;

struct X86CPUState {
    /* Architectural state */
    uint32_t regs[X86_NB_REGS];
    uint32_t eip;
    uint32_t eflags;
    uint32_t cr0, cr2, cr3, cr4;
    X86Seg   seg[10];   /* ES,CS,SS,DS,FS,GS,LDT,TR,GDT,IDT (see x86_cpu.h) */

    /* SYSENTER/SYSEXIT fast-syscall MSRs (0x174/0x175/0x176). musl/glibc use the
     * sysenter path via the kernel vDSO, so these must be honored. */
    uint32_t sysenter_cs;
    uint32_t sysenter_esp;
    uint32_t sysenter_eip;

    /* x87 FPU (register stack modelled with doubles) */
    double   fpst[8];        /* physical registers; ST(i) = fpst[(fpu_top+i)&7] */
    int      fpu_top;        /* 0-7 */
    uint16_t fpu_cw;         /* control word  */
    uint16_t fpu_sw;         /* status word (condition codes; TOP kept separate) */
    uint8_t  fpu_tag[8];     /* per physical reg: 0=valid, 3=empty */

    /* Execution bookkeeping */
    int64_t  cycle_count;
    BOOL     halted;
    int      hard_irq;      /* level of the INTR pin from the PIC */
    int      addr_size;     /* address size of the instruction being decoded: 2 or 4 */
    int      int_shadow;    /* inhibit maskable IRQs for one instruction (mov ss/sti) */

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
};

#endif /* X86_CPU_PRIV_H */
