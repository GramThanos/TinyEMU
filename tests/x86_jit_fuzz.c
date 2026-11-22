/*
 * Differential fuzz test for the x86 -> WebAssembly block JIT (x86_jit.c).
 *
 * Generates random programs from the instructions the JIT compiles, mixed
 * with ones it does not (so blocks end mid-stream and the interpreter picks
 * up the deferred-flag record), plus counted loops for the in-block loop
 * path, stores that land on the code page (self-modifying code), and pages
 * outside RAM. Each program runs twice on two identical machines: once
 * interpreted only, once with every block compiled on first sight. The two
 * must end with identical registers, EIP and the whole of RAM, which holds
 * the stack, and so the final flags pushed by the program's trailing pushfd.
 * Every other program then runs again at CODE_ALIAS, a second linear address
 * of the same frame, with the JIT reusing what it compiled at CODE: compiled
 * code is position-independent, so it must agree with the interpreter there
 * too (a return address pushed by a call, say).
 *
 * Emscripten builds only (the JIT emits WebAssembly); run it through
 * tests/x86_jit_test.sh. Usage: node fuzz.js [programs] [seed] [first] [show],
 * where `first` reruns from one program on (a mismatch report names it) and
 * `show` prints each program before running it.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "cutils.h"
#include "iomem.h"
#include "x86_cpu.h"

extern uint32_t x86_jit_threshold;
extern int x86_jit_chain;
extern uint32_t x86_jit_stat_compiled, x86_jit_stat_blocks, x86_jit_stat_failed,
                x86_jit_stat_invalidated;

#define RAM_SIZE  0x10000
#define CODE      0x1000
#define DATA      0x4800    /* EBP; [ebp+d8] stays inside the data page */
#define STACK     0x8000
#define GDT       0x2000
#define IDT       0x2100
#define HANDLER   0x3000    /* #PF: map the fault page, iret */
#define FAULTPG   0x9000    /* not present until first touched */
#define PD        0xa000
#define PT        0xb000
#define PT2       0xc000    /* every other 4 MB: outside RAM */
#define PT3       0xd000    /* the second 4 MB: PT_ALIAS, then outside RAM */
#define PT_ALIAS  0x400000  /* linear alias of PT, for the #PF handler only */
#define CODE_ALIAS 0x401000 /* a second linear address of the CODE frame */
#define TRAMP     0x3100    /* the alias runs start here: the paging prologue,
                               then a jump to the rest of the program at CODE_ALIAS */
#define PROLOGUE  32        /* bytes of gen_program's paging (and SSE) prologue */
#define XMMOUT    0x4c00    /* the epilogue stores xmm0..7 here, for the comparison */
#define XMMIN     0x5000    /* random bytes xmm0..7 start from (movdqu) */

static uint32_t rng;
static uint32_t rnd(void)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}
#define R(n) (rnd() % (n))

typedef struct {
    PhysMemoryMap *map;
    uint8_t *ram;
    X86CPUState *cpu;
} Vm;

static void vm_init(Vm *v)
{
    PhysMemoryRange *pr;
    v->map = phys_mem_map_init();
    pr = register_ram_entry(v->map, 0, RAM_SIZE, 0);
    pr->phys_mem = mallocz(RAM_SIZE);
    v->ram = pr->phys_mem;
    v->cpu = NULL;
}

/* A program that runs off into random bytes can hit in/out or rdtsc: give
 * them deterministic devices, as a real machine always has some. */
static uint32_t port_rd(void *o, uint32_t port, int sl) { (void)o; (void)sl; return port * 0x9e37u; }
static void port_wr(void *o, uint32_t port, uint32_t v, int sl) { (void)o; (void)port; (void)v; (void)sl; }
static uint64_t fixed_tsc(void *o) { (void)o; return 12345; }

static void vm_reset(Vm *v, const uint8_t *image, const uint32_t *regs)
{
    X86CPUSeg cs = { .sel = 0x10, .flags = 0xc09b, .base = 0, .limit = 0xffffffff };
    X86CPUSeg ds = { .sel = 0x18, .flags = 0xc093, .base = 0, .limit = 0xffffffff };
    int i;
    if (v->cpu) x86_cpu_end(v->cpu);
    memcpy(v->ram, image, RAM_SIZE);
    v->cpu = x86_cpu_init(v->map);
    x86_cpu_set_port_io(v->cpu, port_rd, port_wr, NULL);
    x86_cpu_set_get_tsc(v->cpu, fixed_tsc, NULL);
    x86_cpu_set_seg(v->cpu, X86_CPU_SEG_CS, &cs);
    for (i = 0; i < 6; i++) if (i != X86_CPU_SEG_CS) x86_cpu_set_seg(v->cpu, i, &ds);
    {
        /* FS/GS with bases, so fs:/gs: overrides change the address */
        X86CPUSeg fs = ds, gs = ds;
        fs.base = 0x40; gs.base = 0x1000;
        x86_cpu_set_seg(v->cpu, X86_CPU_SEG_FS, &fs);
        x86_cpu_set_seg(v->cpu, X86_CPU_SEG_GS, &gs);
    }
    for (i = 0; i < 8; i++) x86_cpu_set_reg(v->cpu, i, regs[i]);
    x86_cpu_set_reg(v->cpu, X86_CPU_REG_EIP, CODE);
    x86_cpu_set_reg(v->cpu, X86_CPU_REG_CR0, 1);           /* PE; the program sets PG */
    {
        X86CPUSeg g = { .sel = 0, .flags = 0, .base = GDT, .limit = 0x1f };
        X86CPUSeg t = { .sel = 0, .flags = 0, .base = IDT, .limit = 0x7ff };
        x86_cpu_set_seg(v->cpu, X86_CPU_SEG_GDT, &g);
        x86_cpu_set_seg(v->cpu, X86_CPU_SEG_IDT, &t);
    }
}

static void st32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

/* GDT, IDT (#PF gate), identity page tables with FAULTPG not present, and the
 * handler: mov dword [PT + 9*4], FAULTPG|3; add esp, 4; iret */
static void setup_system(uint8_t *ram)
{
    static const uint8_t h[] = { 0xc7, 0x05, 0, 0, 0, 0, 0, 0, 0, 0, 0x83, 0xc4, 0x04, 0xcf };
    int i;
    memset(ram + GDT, 0, 0x900);
    st32(ram + GDT + 0x10, 0x0000ffff); st32(ram + GDT + 0x14, 0x00cf9b00);
    st32(ram + GDT + 0x18, 0x0000ffff); st32(ram + GDT + 0x1c, 0x00cf9300);
    st32(ram + IDT + 14 * 8, (0x10u << 16) | (HANDLER & 0xffff));
    st32(ram + IDT + 14 * 8 + 4, (HANDLER & 0xffff0000u) | 0x8e00);
    memcpy(ram + HANDLER, h, sizeof(h));
    st32(ram + HANDLER + 2, PT_ALIAS + (FAULTPG >> 12) * 4);
    st32(ram + HANDLER + 6, FAULTPG | 3);
    /* Everything maps somewhere, so a wild address reads all-ones (no RAM
     * there) instead of faulting forever; only FAULTPG faults. */
    for (i = 0; i < 1024; i++) {
        st32(ram + PD + i * 4, (i == 0 ? PT : i == 1 ? PT3 : PT2) | 3);
        st32(ram + PT + i * 4, (uint32_t)i * 0x1000 | 3);
        st32(ram + PT2 + i * 4, 0x100000u + (uint32_t)i * 0x1000 | 3);
        st32(ram + PT3 + i * 4, 0x100000u + (uint32_t)i * 0x1000 | 3);
    }
    st32(ram + PT + (FAULTPG >> 12) * 4, FAULTPG);   /* not present */
    /* The page tables themselves are not reachable at any linear address:
     * a stray store into a live PTE without invlpg has no defined result
     * (the TLB may or may not still hold the old entry, and the JIT flushes
     * the write TLB at times the interpreter does not), so a program that
     * rewrites them is not a meaningful comparison. Alias them to 0xf000. */
    for (i = PD >> 12; i <= PT3 >> 12; i++)
        st32(ram + PT + i * 4, 0xf000 | 3);
    /* ...except to the #PF handler, which maps FAULTPG through this one alias
     * of PT (the first page of the second 4 MB, which programs don't aim at) */
    st32(ram + PT3, PT | 3);
    st32(ram + PT3 + 4, CODE | 3);                  /* CODE_ALIAS */
}

static void vm_run(Vm *v)
{
    int n;
    for (n = 0; n < 200 && !x86_cpu_get_power_down(v->cpu); n++)
        x86_cpu_interp(v->cpu, 100000);
}

/* ---- program generator ------------------------------------------------- */

static uint8_t *pc;

static void b(uint32_t x) { *pc++ = (uint8_t)x; }
static void d32(uint32_t x) { b(x); b(x >> 8); b(x >> 16); b(x >> 24); }

/* a register the program may write: not ESP (4) or EBP (5) */
static int no_ecx;   /* generating a loop body: leave the counter alone */
static int wreg(void)
{
    static const int r[6] = { 0, 1, 2, 3, 6, 7 }, nc[5] = { 0, 2, 3, 6, 7 };
    return no_ecx ? nc[R(5)] : r[R(6)];
}
/* an 8-bit register whose 32-bit register is writable: AL CL DL BL AH CH DH BH */
static int wreg8(void) { int r; do r = R(8); while (no_ecx && (r & 3) == 1); return r; }

/* ModR/M + operand: register, or a memory operand based on EBP */
static void modrm(int reg, int is_reg, int rm)
{
    if (is_reg) { b(0xc0 | reg << 3 | rm); return; }
    switch (R(7)) {
    case 6:                                 /* the fault page: the first touch faults */
        b(0x05 | reg << 3); d32(FAULTPG + (R(0x100) & ~3u)); break;
    case 0: case 1: case 2:                 /* [ebp + d8] */
        b(0x45 | reg << 3); b(R(0x7c) & ~3u); break;
    case 3:                                 /* [ebp + d8], unaligned */
        b(0x45 | reg << 3); b(R(0x7c)); break;
    case 4:                                 /* [ebp + reg*s + d8]: often outside RAM */
        b(0x44 | reg << 3); b(R(4) << 6 | (wreg() << 3) | 5); b(R(0x80)); break;
    default:                                /* [disp32]: the code page, or past RAM */
        b(0x05 | reg << 3);
        d32(R(2) ? CODE + 0x800 + (R(0x100) & ~3u) : 0x20000 + R(0x100));
        break;
    }
}

static void gen_insn(void)
{
    static const uint8_t alu[8] = { 0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38 };
    int k = R(42);
    int op = R(8);                  /* all eight, adc/sbb included */

    if (R(8) == 0)                  /* a segment override (or lock) on anything */
        b(R(3) == 0 ? 0xf0 : R(2) ? 0x64 : 0x65);
    switch (k) {
    case 0: case 1: case 2:         /* alu r/m32, r32 (register) */
        b(alu[op] | 1); modrm(R(8), 1, wreg()); break;
    case 3:                         /* alu r/m32, r32 (memory) */
        b(alu[op] | 1); modrm(R(8), 0, 0); break;
    case 4: case 5:                 /* alu r32, r/m32 */
        b(alu[op] | 3); modrm(wreg(), R(2), R(8)); break;
    case 6:                         /* alu r/m8, r8 / r8, r/m8 */
        b(alu[op] | (R(2) << 1)); modrm(wreg8(), R(2), wreg8()); break;
    case 7:                         /* alu eax, imm32 / al, imm8 */
        if (R(2)) { b(alu[op] | 5); d32(rnd()); }
        else { b(alu[op] | 4); b(rnd()); }
        break;
    case 8: case 9:                 /* group1 imm8 / imm32 / byte */
        switch (R(3)) {
        case 0: b(0x83); modrm(op, R(2), wreg()); b(rnd()); break;
        case 1: b(0x81); modrm(op, R(2), wreg()); d32(R(2) ? rnd() : R(4)); break;
        default: b(0x80); modrm(op, R(2), wreg8()); b(rnd()); break;
        }
        break;
    case 10:                        /* test */
        b(0x84 | R(2)); modrm(R(8), R(2), R(8)); break;
    case 11: case 12:               /* inc/dec */
        b((R(2) ? 0x40 : 0x48) | wreg()); break;
    case 13:                        /* push/pop */
        if (R(3)) { b(0x50 | R(8)); b(0x58 | wreg()); }
        else if (R(2)) { b(0x68); d32(rnd()); b(0x58 | wreg()); }
        else { b(0x6a); b(rnd()); b(0x58 | wreg()); }
        break;
    case 14:                        /* mov r, imm */
        if (R(2)) { b(0xb8 | wreg()); d32(R(3) ? rnd() : R(3)); }
        else { b(0xb0 | wreg8()); b(rnd()); }
        break;
    case 15: case 16:               /* mov r/m, r and r, r/m */
        b(0x88 | R(4)); {
            int w = (pc[-1] & 1), toreg = (pc[-1] & 2);
            int is_reg = R(2);
            int rm = toreg ? R(8) : (w ? wreg() : wreg8());
            int reg = toreg ? (w ? wreg() : wreg8()) : R(8);
            modrm(reg, is_reg, rm);
        }
        break;
    case 17:                        /* mov r/m, imm */
        if (R(2)) { b(0xc7); modrm(0, R(2), wreg()); d32(rnd()); }
        else { b(0xc6); modrm(0, R(2), wreg8()); b(rnd()); }
        break;
    case 18:                        /* lea */
        b(0x8d); modrm(wreg(), 0, 0); break;
    case 19:                        /* movzx / movsx */
        b(0x0f); b(0xb6 | (R(2) << 3) | R(2)); modrm(wreg(), R(2), R(4)); break;
    case 20:                        /* shifts and rotates: imm8, 1, CL; byte or dword */
        switch (R(3)) {
        case 0: {                   /* the register must match the width: a dword
                                       wreg8() index could be ESP or EBP */
            int w = R(2);
            b(0xc0 | w); modrm(op, R(2), w ? wreg() : wreg8()); b(R(4) ? R(40) : rnd());
            break;
        }
        case 1: b(0xd0 | R(2)); modrm(op, R(2), wreg()); break;
        default: b(0xd2 | R(2)); modrm(op, R(2), wreg()); break;
        }
        break;
    case 23: {                      /* group3: test imm, not, neg, mul, imul (no div) */
        static const int g3[5] = { 0, 2, 3, 4, 5 };
        int w = R(2), r = g3[R(5)];
        b(0xf6 | w); modrm(r, R(2), w ? wreg() : wreg8());
        if (r == 0) { if (w) d32(rnd()); else b(rnd()); }
        break;
    }
    case 24:                        /* imul r32, r/m32 [, imm] */
        switch (R(3)) {
        case 0: b(0x0f); b(0xaf); modrm(wreg(), R(2), R(8)); break;
        case 1: b(0x69); modrm(wreg(), R(2), R(8)); d32(R(2) ? rnd() : R(100)); break;
        default: b(0x6b); modrm(wreg(), R(2), R(8)); b(rnd()); break;
        }
        break;
    case 25:                        /* cmovcc / setcc */
        if (R(2)) { b(0x0f); b(0x40 | R(16)); modrm(wreg(), R(2), R(8)); }
        else { b(0x0f); b(0x90 | R(16)); modrm(0, R(2), wreg8()); }
        break;
    case 26:                        /* xchg */
        switch (R(3)) {
        case 0: b(0x87); modrm(wreg(), R(2), wreg()); break;
        case 1: b(0x86); modrm(wreg8(), R(2), wreg8()); break;
        default: { int r = wreg(); if (r) b(0x90 | r); else b(0x90); } break;
        }
        break;
    case 27:                        /* cwde, cdq, moffs moves, test acc imm */
        switch (R(5)) {
        case 0: b(0x98); break;
        case 1: if (!no_ecx) b(0x99); else b(0x90); break;
        case 2: b(0xa0 | R(4)); d32(R(3) ? DATA + R(0x100) : FAULTPG + R(0x100)); break;
        case 3: b(0xa8); b(rnd()); break;
        default: b(0xa9); d32(rnd()); break;
        }
        break;
    case 28:                        /* inc/dec r/m, push r/m */
        switch (R(3)) {
        case 0: b(0xff); modrm(R(2), R(2), wreg()); break;
        case 1: b(0xfe); modrm(R(2), R(2), wreg8()); break;
        default: b(0xff); modrm(6, R(2), R(8)); b(0x58 | wreg()); break;
        }
        break;
    case 29:                        /* push ebp; mov ebp, esp; ...; leave: balanced,
                                       so ESP and EBP stay where the program
                                       expects them (a stray ESP could reach
                                       the page-table alias) */
        b(0x55); b(0x89); b(0xe5);
        if (R(2)) { b(0x83); b(0xec); b(4 * R(4)); }    /* sub esp, n */
        b(0xc9);
        break;
    case 30: case 31:               /* left to the interpreter: x86_jit_step() */
        switch (R(14)) {
        case 0: b(0xf8 | R(6)); break;                  /* clc stc cli sti cld std */
        case 1: b(0x9c); b(0x58 | wreg()); break;       /* pushf; pop r */
        case 2: b(0x0f); b(0xa3 | (R(4) << 3)); modrm(R(8), R(2), wreg()); break; /* bt/bts/btr/btc */
        case 3: b(0x0f); b(0xba); modrm(4 + R(4), R(2), wreg()); b(rnd()); break;
        case 4: b(0x0f); b(0xc8 | wreg()); break;       /* bswap */
        case 5: b(0x0f); b(0xbc | R(2)); modrm(wreg(), R(2), R(8)); break;  /* bsf/bsr */
        case 6: b(0x66); b(0xb8 | wreg()); b(rnd()); b(rnd()); break;      /* mov r16, imm16 */
        case 7: b(0x66); b(0x89 | (R(2) << 1)); modrm(wreg(), R(2), wreg()); break;
        case 8: b(0x66); b(0xc1); modrm(op, R(2), wreg()); b(R(20)); break;
        case 9: b(0x9e | R(2)); break;                  /* sahf / lahf */
        case 10: b(0x0f); b(0xc1 - 0x10 * R(2)); modrm(wreg(), R(2), wreg()); break; /* xadd/cmpxchg */
        case 11: b(0x0f); b(0xa4 | (R(2) << 3)); modrm(wreg(), R(2), wreg()); b(R(40)); break; /* shld/shrd imm */
        case 12: b(0x27 | (R(4) << 3)); break;          /* daa das aaa aas */
        default: b(0x0f); b(0xa5 | (R(2) << 3)); modrm(wreg(), R(2), wreg()); break; /* shld/shrd cl */
        }
        break;
    case 36: case 37:               /* 16-bit operand size: compiled natively */
        b(0x66);
        switch (R(10)) {
        case 0: b(alu[op] | 1); modrm(R(8), R(2), wreg()); break;      /* alu r/m16, r16 */
        case 1: b(alu[op] | 3); modrm(wreg(), R(2), R(8)); break;      /* alu r16, r/m16 */
        case 2: b(alu[op] | 5); b(rnd()); b(rnd()); break;             /* alu ax, imm16 */
        case 3: b(0x81); modrm(op, R(2), wreg()); b(rnd()); b(rnd()); break;
        case 4: b(0x83); modrm(op, R(2), wreg()); b(rnd()); break;
        case 5: b(0x85); modrm(R(8), R(2), R(8)); break;               /* test */
        case 6: b(0x89 | (R(2) << 1)); modrm(wreg(), R(2), wreg()); break;  /* mov */
        case 7: b(0xc7); modrm(0, R(2), wreg()); b(rnd()); b(rnd()); break;
        case 8: b(0xb8 | wreg()); b(rnd()); b(rnd()); break;
        default:                                                       /* shifts */
            switch (R(3)) {
            case 0: b(0xc1); modrm(op, R(2), wreg()); b(R(4) ? R(20) : rnd()); break;
            case 1: b(0xd1); modrm(op, R(2), wreg()); break;
            default: b(0xd3); modrm(op, R(2), wreg()); break;
            }
            break;
        }
        break;
    case 38: case 39: case 40: case 41:     /* the SSE2 moves, cld/std */
        switch (R(5)) {
        case 0: b(0x66); b(0x0f); b(0x6e); modrm(R(8), R(2), R(8)); break;  /* movd xmm, r/m32 */
        case 1: b(0x66); b(0x0f); b(0x62); b(0xc0 | R(8) << 3 | R(8)); break; /* punpckldq */
        case 2: b(0x66); b(0x0f); b(0xd6); modrm(R(8), R(2), R(8)); break;  /* movq xmm/m64, xmm */
        case 3: b(0x66); b(0x0f); b(0x7e); b(0xc0 | R(8) << 3 | wreg()); break; /* movd r32, xmm */
        default: b(0xfc | R(2)); break;
        }
        break;
    case 21:                        /* nop */
        b(0x90); break;
    case 22:                        /* call next; pop (exercises call) */
        b(0xe8); d32(0); b(0x58 | wreg()); break;
    default: {                      /* jcc over the next instruction */
        uint8_t *j = pc;
        uint8_t *after;
        int wide = R(4) == 0;
        if (wide) { b(0x0f); b(0x80 | R(16)); d32(0); }
        else { b(0x70 | R(16)); b(0); }
        after = pc;
        gen_insn();
        if (wide) { uint32_t d = (uint32_t)(pc - after); j[2] = d; j[3] = d >> 8; j[4] = d >> 16; j[5] = d >> 24; }
        else j[1] = (uint8_t)(pc - after);
        break;
    }
    }
}

static void gen_program(uint8_t *ram)
{
    int n, i, sse;
    pc = ram + CODE;
    /* paging on: push eax; mov eax, PD; mov cr3, eax; mov eax, cr0;
     * or eax, 0x80000000; mov cr0, eax; pop eax */
    b(0x50); b(0xb8); d32(PD); b(0x0f); b(0x22); b(0xd8);
    b(0x0f); b(0x20); b(0xc0); b(0x0d); d32(0x80000000u); b(0x0f); b(0x22); b(0xc0);
    /* SSE on: mov eax, cr4; or eax, OSFXSR; mov cr4, eax. Left off in one
     * program in eight: its first SSE instruction must #UD on both sides
     * (unhandled here, so it stops), which checks the JIT's sse_check guard. */
    sse = R(8) != 0;  /* (the alias pass copies this prologue: same length either way) */
    b(0x0f); b(0x20); b(0xe0); b(0x0d); d32(sse ? 0x200 : 0); b(0x0f); b(0x22); b(0xe0); b(0x58);
    {   /* xmm0..7 from random bytes: movdqu xmm x, [XMMIN + 16x] */
        int x;
        for (x = 0; x < 8 && sse; x++) { b(0xf3); b(0x0f); b(0x6f); b(0x05 | x << 3); d32(XMMIN + 16 * x); }
    }
    /* random arithmetic flags in, via the interpreter's popf */
    b(0x68); d32(0x2 | (rnd() & 0x8d5)); b(0x9d);
    n = 4 + R(28);
    for (i = 0; i < n; i++) {
        if (R(12) == 0) {               /* counted loop: the in-block loop path */
            uint8_t *top;
            int j, m = 1 + R(4);
            b(0xb9); d32(1 + R(R(2) ? 5 : 40000));   /* mov ecx, n */
            top = pc;
            no_ecx = 1;
            for (j = 0; j < m; j++) {
                uint8_t *save = pc;
                gen_insn();
                /* no jcc inside the body (it would split the block), and keep ECX */
                if (save[0] >= 0x70 && save[0] <= 0x7f) { pc = save; b(0x90); }
                else if (save[0] == 0x0f && save[1] >= 0x80 && save[1] <= 0x8f) { pc = save; b(0x90); }
            }
            no_ecx = 0;
            b(0x49);                    /* dec ecx */
            b(0x75); b((uint8_t)(top - (pc + 1)));      /* jnz top */
        } else {
            gen_insn();
        }
        if (pc - ram > CODE + 0x700) break;
    }
    {   /* the low halves with movq (compiled), then all of each with
         * movdqu [XMMOUT + 16x], xmm x (interpreted), into RAM */
        int x;
        for (x = 0; x < 8; x++) { b(0x66); b(0x0f); b(0xd6); b(0x05 | x << 3); d32(XMMOUT + 0x80 + 8 * x); }
        for (x = 0; x < 8; x++) { b(0xf3); b(0x0f); b(0x7f); b(0x05 | x << 3); d32(XMMOUT + 16 * x); }
    }
    b(0x9c);                            /* pushfd: final flags to the stack */
    b(0xf4);                            /* hlt */
}

/* When two finished runs disagree: run the program again a block at a
 * time on the JIT side, stepping the interpreter the same number of
 * instructions, and report the first block after which the two differ in a
 * register, EFLAGS, EIP or RAM. */
static int same_state(Vm *a, Vm *j)
{
    int r;
    for (r = 0; r <= X86_CPU_REG_EFLAGS; r++) {
        if (r == X86_CPU_REG_CR0 || r == X86_CPU_REG_CR2) continue;
        if (x86_cpu_get_reg(a->cpu, r) != x86_cpu_get_reg(j->cpu, r)) return 0;
    }
    return memcmp(a->ram, j->ram, RAM_SIZE) == 0;
}

static void lockstep(Vm *a, Vm *j, const uint8_t *image, const uint32_t *regs, uint32_t eip)
{
    long steps;
    static const char *names[12] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi",
                                     "edi", "eip", "cr0", "cr2", "eflags" };
    x86_jit_chain = 0;          /* a step is one block */
    x86_jit_threshold = 0xfffffff0u;
    vm_reset(a, image, regs);
    x86_cpu_set_reg(a->cpu, X86_CPU_REG_EIP, eip);
    x86_jit_threshold = 1;
    vm_reset(j, image, regs);
    x86_cpu_set_reg(j->cpu, X86_CPU_REG_EIP, eip);
    for (steps = 0; steps < 2000000 && !x86_cpu_get_power_down(j->cpu); steps++) {
        uint32_t e0 = x86_cpu_get_reg(j->cpu, X86_CPU_REG_EIP);
        int64_t c0 = x86_cpu_get_cycles(j->cpu), d, n;
        x86_jit_threshold = 1;
        x86_cpu_interp(j->cpu, 1);
        d = x86_cpu_get_cycles(j->cpu) - c0;
        x86_jit_threshold = 0xfffffff0u;
        for (n = 0; n < d && !x86_cpu_get_power_down(a->cpu); n++)
            x86_cpu_interp(a->cpu, 1);
        /* a fault takes a step without counting an instruction */
        for (n = 0; n < 2 && x86_cpu_get_reg(a->cpu, X86_CPU_REG_EIP) !=
                             x86_cpu_get_reg(j->cpu, X86_CPU_REG_EIP); n++)
            x86_cpu_interp(a->cpu, 1);
        if (!same_state(a, j)) {
            int r, o;
            printf("  lockstep: first difference after the step at eip=%08x (%lld instructions)\n",
                   e0, (long long)d);
            for (r = 0; r < 12; r++)
                if (r != X86_CPU_REG_CR0 && r != X86_CPU_REG_CR2)
                    printf("    %-6s interp=%08x jit=%08x%s\n", names[r],
                           x86_cpu_get_reg(a->cpu, r), x86_cpu_get_reg(j->cpu, r),
                           x86_cpu_get_reg(a->cpu, r) != x86_cpu_get_reg(j->cpu, r) ? "  <--" : "");
            for (o = 0; o < RAM_SIZE; o++)
                if (a->ram[o] != j->ram[o]) {
                    printf("    first RAM difference at %05x: interp=%02x jit=%02x\n",
                           o, a->ram[o], j->ram[o]);
                    break;
                }
            printf("    code from %08x:", e0);
            for (o = 0; o < 24 && (e0 + o) < RAM_SIZE; o++) printf(" %02x", image[e0 + o]);
            printf("\n");
            return;
        }
    }
    printf("  lockstep: no difference found (the runs may differ only in timing)\n");
}

/* free this program's compiled code, then start the table afresh */
static void forget_code(Vm *j)
{
    x86_cpu_note_ram_write(j->cpu, CODE);
    x86_jit_forget();
}
static int finished_same(Vm *a, Vm *j, uint32_t it, uint32_t seed, const uint8_t *image,
                         const char *what);

int main(int argc, char **argv)
{
    uint32_t iters = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 20000;
    uint32_t seed = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 12345;
    uint32_t first = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 0) : 0;   /* to rerun one */
    int show = argc > 4 && !strcmp(argv[4], "show");   /* print each program */
    static uint8_t image[RAM_SIZE];
    Vm a, j;
    uint32_t it, skipped = 0;
    int i;

    vm_init(&a);
    vm_init(&j);
    for (it = first; it < first + iters; it++) {
        uint32_t regs[8];
        rng = seed + it * 2654435761u;
        if (!rng) rng = 1;
        for (i = 0; i < RAM_SIZE; i++) image[i] = (uint8_t)rnd();
        memset(image + CODE, 0x90, 0x1000);
        setup_system(image);
        gen_program(image);
        memcpy(image + TRAMP, image + CODE, PROLOGUE);
        image[TRAMP + PROLOGUE] = 0xe9;             /* jmp CODE_ALIAS + PROLOGUE */
        st32(image + TRAMP + PROLOGUE + 1, (CODE_ALIAS + PROLOGUE) - (TRAMP + PROLOGUE + 5));
        if (show) {
            int o;
            printf("program %u:", it);
            for (o = CODE; o < CODE + 0x800; o++) {
                printf(" %02x", image[o]);
                if (image[o] == 0xf4 && image[o - 1] == 0x9c) break;
            }
            printf("\n");
            fflush(stdout);
        }
        for (i = 0; i < 8; i++) regs[i] = R(3) ? rnd() : R(0x100);
        regs[4] = STACK; regs[5] = DATA;

        x86_jit_threshold = 0xfffffff0u;        /* interpreter only */
        vm_reset(&a, image, regs);
        vm_run(&a);

        x86_jit_threshold = 1;                  /* compile every block */
        vm_reset(&j, image, regs);
        vm_run(&j);

        /* A loop whose body rewrote ECX may outrun the budget: compare only
         * programs that finished. Their instruction counts must agree too. */
        /* the two count slices differently (a compiled block may overrun one),
         * so a program that ends near the budget can finish on one side only:
         * give the other more time before calling it a difference */
        if (x86_cpu_get_power_down(a.cpu) != x86_cpu_get_power_down(j.cpu)) {
            x86_jit_threshold = 0xfffffff0u;
            vm_run(&a);
            x86_jit_threshold = 1;
            vm_run(&j);
        }
        if (!x86_cpu_get_power_down(a.cpu) || !x86_cpu_get_power_down(j.cpu)) {
            if (x86_cpu_get_power_down(a.cpu) != x86_cpu_get_power_down(j.cpu)) {
                printf("MISMATCH at iteration %u (seed %u): only one run reached hlt\n"
                       "  interp: halted=%d eip=%08x instructions=%lld\n"
                       "  jit:    halted=%d eip=%08x instructions=%lld\n", it, seed,
                       x86_cpu_get_power_down(a.cpu), x86_cpu_get_reg(a.cpu, X86_CPU_REG_EIP),
                       (long long)x86_cpu_get_cycles(a.cpu),
                       x86_cpu_get_power_down(j.cpu), x86_cpu_get_reg(j.cpu, X86_CPU_REG_EIP),
                       (long long)x86_cpu_get_cycles(j.cpu));
                forget_code(&j);
                lockstep(&a, &j, image, regs, CODE);
                return 1;
            }
            skipped++;
            forget_code(&j);
            continue;
        }

        for (i = 0; i <= 8; i++) {
            int r = i < 8 ? i : X86_CPU_REG_EIP;
            if (x86_cpu_get_reg(a.cpu, r) != x86_cpu_get_reg(j.cpu, r)) break;
        }
        if (!finished_same(&a, &j, it, seed, image, "")) {
            forget_code(&j);
            lockstep(&a, &j, image, regs, CODE);
            return 1;
        }

        if (it & 1) {
            /* The alias pass. The first JIT run may have rewritten code:
             * vm_reset copies the image back behind the JIT's back, so first
             * drop the code of every page that run changed. */
            for (i = 0; i < RAM_SIZE; i += 0x1000)
                if (memcmp(j.ram + i, image + i, 0x1000))
                    x86_cpu_note_ram_write(j.cpu, (uint64_t)i);
            x86_jit_threshold = 0xfffffff0u;
            vm_reset(&a, image, regs);
            x86_cpu_set_reg(a.cpu, X86_CPU_REG_EIP, TRAMP);
            vm_run(&a);
            x86_jit_threshold = 1;
            vm_reset(&j, image, regs);
            x86_cpu_set_reg(j.cpu, X86_CPU_REG_EIP, TRAMP);
            vm_run(&j);
            if (x86_cpu_get_power_down(a.cpu) && x86_cpu_get_power_down(j.cpu) &&
                !finished_same(&a, &j, it, seed, image, " (alias pass, at CODE_ALIAS)")) {
                forget_code(&j);
                lockstep(&a, &j, image, regs, TRAMP);
                return 1;
            }
        }
        forget_code(&j);
    }
    printf("JIT FUZZ PASSED: %u programs (%u unfinished, skipped), %u blocks compiled "
           "in %u page functions, %u failed, %u invalidated\n",
           iters, skipped, x86_jit_stat_blocks, x86_jit_stat_compiled,
           x86_jit_stat_failed, x86_jit_stat_invalidated);
    return 0;
}

/* Registers, EIP, RAM and instruction counts of two finished runs; prints
 * the differences when they disagree. */
static int finished_same(Vm *a, Vm *j, uint32_t it, uint32_t seed, const uint8_t *image,
                         const char *what)
{
    int i;
    for (i = 0; i <= 8; i++) {
        int r = i < 8 ? i : X86_CPU_REG_EIP;
        if (x86_cpu_get_reg(a->cpu, r) != x86_cpu_get_reg(j->cpu, r)) break;
    }
    if (i <= 8 || memcmp(a->ram, j->ram, RAM_SIZE) != 0 ||
        x86_cpu_get_cycles(a->cpu) != x86_cpu_get_cycles(j->cpu)) {
        int o;
        printf("MISMATCH at iteration %u (seed %u)%s\n", it, seed, what);
        for (i = 0; i < 8; i++)
            printf("  r%d interp=%08x jit=%08x%s\n", i, x86_cpu_get_reg(a->cpu, i),
                   x86_cpu_get_reg(j->cpu, i),
                   x86_cpu_get_reg(a->cpu, i) != x86_cpu_get_reg(j->cpu, i) ? "  <--" : "");
        printf("  instructions interp=%lld jit=%lld\n",
               (long long)x86_cpu_get_cycles(a->cpu), (long long)x86_cpu_get_cycles(j->cpu));
        printf("  eip interp=%08x jit=%08x\n", x86_cpu_get_reg(a->cpu, X86_CPU_REG_EIP),
               x86_cpu_get_reg(j->cpu, X86_CPU_REG_EIP));
        for (o = 0; o < RAM_SIZE; o++)
            if (a->ram[o] != j->ram[o]) {
                printf("  first RAM difference at %05x: interp=%02x jit=%02x\n",
                       o, a->ram[o], j->ram[o]);
                break;
            }
        printf("  code:");
        for (o = CODE; o < CODE + 0x800; o++) {
            printf(" %02x", image[o]);
            if (image[o] == 0xf4 && image[o - 1] == 0x9c) break;
        }
        printf("\n");
        return 0;
    }
    return 1;
}
