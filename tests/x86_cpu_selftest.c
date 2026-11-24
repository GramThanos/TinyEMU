/*
 * Self-test / regression suite for the x86 software interpreter.
 *
 * Each case hand-assembles a 32-bit program, runs it under the interpreter, and
 * checks the resulting register file (and physical memory, for the paging case).
 * Grow this alongside the opcode set.
 *
 * Build & run:  make x86-selftest
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "cutils.h"
#include "iomem.h"
#include "x86_cpu.h"

static uint8_t *g_ram;

static PhysMemoryMap *mk_map(uint64_t size)
{
    PhysMemoryMap *map = phys_mem_map_init();
    PhysMemoryRange *pr = register_ram_entry(map, 0, size, 0);
    pr->phys_mem = mallocz(size);
    g_ram = pr->phys_mem;
    return map;
}

static X86CPUState *mk_cpu(PhysMemoryMap *map, uint32_t esp)
{
    X86CPUState *s = x86_cpu_init(map);
    X86CPUSeg cs = { .sel = 0x10, .flags = 0xc09b, .base = 0, .limit = 0xffffffff };
    X86CPUSeg ds = { .sel = 0x18, .flags = 0xc093, .base = 0, .limit = 0xffffffff };
    int i;
    x86_cpu_set_seg(s, X86_CPU_SEG_CS, &cs);
    for (i = 0; i < 6; i++) if (i != X86_CPU_SEG_CS) x86_cpu_set_seg(s, i, &ds);
    x86_cpu_set_reg(s, X86_CPU_REG_EIP, 0);
    x86_cpu_set_reg(s, 4 /*ESP*/, esp);
    return s;
}

static int g_fail;
#define CHECK(cpu, r, want) do {                                            \
        uint32_t _g = x86_cpu_get_reg(cpu, r);                              \
        if (_g != (uint32_t)(want)) {                                       \
            printf("    *** FAIL: reg[%d]=0x%x want 0x%x\n", (r), _g,       \
                   (uint32_t)(want)); g_fail = 1;                           \
        }                                                                   \
    } while (0)

/* ----------------------------------------------------------------------- */

static void test_basic(void)
{
    static const uint8_t p[] = {
        0xB8,0x05,0,0,0,  0xB9,0x03,0,0,0,  0x01,0xC8,  0x89,0xC2,
        0x83,0xEA,0x01,   0x52, 0x5B,       0x31,0xF6,  0x46,
        0xBF,0x01,0,0,0,  0x83,0xFF,0x01,   0x74,0x05,
        0xBF,0x99,0,0,0,  0xF4
    };
    PhysMemoryMap *map = mk_map(0x10000);
    X86CPUState *s = mk_cpu(map, 0x8000);
    memcpy(g_ram, p, sizeof(p));
    x86_cpu_interp(s, 1000);
    printf("  basic: eax/edx/ebx/esi/edi\n");
    CHECK(s,0,8); CHECK(s,1,3); CHECK(s,2,7); CHECK(s,3,7);
    CHECK(s,6,1); CHECK(s,7,1);
    x86_cpu_end(s);
}

static void test_arith(void)
{
    static const uint8_t p[] = {
        0xB8,0x34,0x12,0,0,   /* mov eax,0x1234 */
        0xC1,0xE0,0x04,       /* shl eax,4      */
        0xC1,0xE8,0x08,       /* shr eax,8  ->0x123 */
        0xB9,0x07,0,0,0,      /* mov ecx,7      */
        0x6B,0xC9,0x06,       /* imul ecx,ecx,6 ->42 */
        0xB8,0x80,0,0,0,      /* mov eax,0x80   */
        0x0F,0xBE,0xE8,       /* movsx ebp,al ->0xFFFFFF80 */
        0xB8,0x64,0,0,0,      /* mov eax,100    */
        0x31,0xD2,            /* xor edx,edx    */
        0xBB,0x07,0,0,0,      /* mov ebx,7      */
        0xF7,0xF3,            /* div ebx -> eax=14 edx=2 */
        0x83,0xF8,0x0E,       /* cmp eax,14     */
        0x0F,0x94,0xC3,       /* sete bl -> ebx=1 */
        0xF4
    };
    PhysMemoryMap *map = mk_map(0x10000);
    X86CPUState *s = mk_cpu(map, 0x8000);
    memcpy(g_ram, p, sizeof(p));
    x86_cpu_interp(s, 1000);
    printf("  arith: shl/shr/imul/movsx/div/setcc\n");
    CHECK(s,0,14); CHECK(s,1,42); CHECK(s,2,2); CHECK(s,3,1);
    CHECK(s,5,0xFFFFFF80);
    x86_cpu_end(s);
}

static void test_callret(void)
{
    static const uint8_t p[] = {
        0xB8,0x05,0,0,0,      /* 0:  mov eax,5      */
        0xE8,0x04,0,0,0,      /* 5:  call +4 (->14) */
        0x89,0xC1,            /* 10: mov ecx,eax    */
        0xEB,0x04,            /* 12: jmp +4 (->18)  */
        0x83,0xC0,0x0A,       /* 14: add eax,10     */
        0xC3,                 /* 17: ret            */
        0xF4                  /* 18: hlt            */
    };
    PhysMemoryMap *map = mk_map(0x10000);
    X86CPUState *s = mk_cpu(map, 0x8000);
    memcpy(g_ram, p, sizeof(p));
    x86_cpu_interp(s, 1000);
    printf("  call/ret + jmp\n");
    CHECK(s,0,15); CHECK(s,1,15);
    x86_cpu_end(s);
}

static void test_paging(void)
{
    /* Guest enables paging itself (mov cr3 / mov cr0), identity-maps VA 0..4MB
     * via a page table, and maps VA 0x40000000 as a 4MB page to phys 0x400000.
     * A paged store to 0x40001000 must then land at phys 0x00401000. */
    static const uint8_t p[] = {
        0xBB,0x00,0x10,0x00,0x00,      /* mov ebx,0x1000        */
        0x0F,0x22,0xDB,                /* mov cr3,ebx           */
        0xBA,0x01,0x00,0x00,0x80,      /* mov edx,0x80000001    */
        0x0F,0x22,0xC2,                /* mov cr0,edx (PG|PE)   */
        0xB8,0xBE,0xBA,0xFE,0xCA,      /* mov eax,0xCAFEBABE    */
        0x89,0x05,0x00,0x10,0x00,0x40, /* mov [0x40001000],eax  */
        0xF4                           /* hlt                   */
    };
    PhysMemoryMap *map = mk_map(0x800000); /* 8 MB */
    X86CPUState *s = mk_cpu(map, 0x8000);
    memcpy(g_ram, p, sizeof(p));

    uint32_t *pd = (uint32_t *)(g_ram + 0x1000); /* page directory @ phys 0x1000 */
    uint32_t *pt = (uint32_t *)(g_ram + 0x2000); /* page table     @ phys 0x2000 */
    pd[0]   = 0x2000 | 0x3;            /* PDE[0]   -> PT (present, rw)   */
    pd[256] = 0x00400000 | 0x83;       /* PDE[256] -> 4MB page @0x400000 */
    pt[0]   = 0x0000 | 0x3;            /* identity-map code page 0       */

    x86_cpu_interp(s, 1000);

    uint32_t got;
    memcpy(&got, g_ram + 0x401000, 4);
    printf("  paging: store via VA 0x40001000 -> phys 0x401000 = 0x%08x\n", got);
    if (got != 0xCAFEBABE) {
        printf("    *** FAIL: paged store landed wrong (0x%08x)\n", got);
        g_fail = 1;
    }
    if (x86_cpu_get_reg(s, X86_CPU_REG_CR0) != 0x80000001u) {
        printf("    *** FAIL: CR0 not set to paging-enabled\n");
        g_fail = 1;
    }
    x86_cpu_end(s);
}

static void test_string(void)
{
    static const uint8_t p[] = {
        0xBE,0x00,0x20,0x00,0x00,   /* mov esi,0x2000     */
        0xBF,0x00,0x30,0x00,0x00,   /* mov edi,0x3000     */
        0xB9,0x05,0x00,0x00,0x00,   /* mov ecx,5          */
        0xFC,                       /* cld                */
        0xF3,0xA4,                  /* rep movsb          */
        0xBF,0x00,0x31,0x00,0x00,   /* mov edi,0x3100     */
        0xB9,0x04,0x00,0x00,0x00,   /* mov ecx,4          */
        0xB0,0xAA,                  /* mov al,0xAA        */
        0xF3,0xAA,                  /* rep stosb          */
        0xBF,0x00,0x30,0x00,0x00,   /* mov edi,0x3000     */
        0xB9,0x05,0x00,0x00,0x00,   /* mov ecx,5          */
        0xB0,0x6C,                  /* mov al,'l'         */
        0xF2,0xAE,                  /* repne scasb        */
        0xF4                        /* hlt                */
    };
    PhysMemoryMap *map = mk_map(0x10000);
    X86CPUState *s = mk_cpu(map, 0x8000);
    memcpy(g_ram, p, sizeof(p));
    memcpy(g_ram + 0x2000, "Hello", 5);
    x86_cpu_interp(s, 100000);
    printf("  string: rep movsb/stosb, repne scasb\n");
    if (memcmp(g_ram + 0x3000, "Hello", 5) != 0) {
        printf("    *** FAIL: rep movsb did not copy\n"); g_fail = 1;
    }
    for (int i = 0; i < 4; i++)
        if (g_ram[0x3100 + i] != 0xAA) { printf("    *** FAIL: rep stosb\n"); g_fail = 1; break; }
    CHECK(s, 7, 0x3003); /* edi past first 'l' */
    CHECK(s, 1, 2);      /* ecx remaining */
    x86_cpu_end(s);
}

static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = v >> (8*i); }

static void test_interrupt(void)
{
    /* Ring-0 int/iret through a real GDT+IDT. `int 0x40` vectors to a handler
     * that sets EAX and IRETs; execution then resumes after the INT. */
    static const uint8_t prog[] = {   /* @ 0x0000 */
        0xCD,0x40,                 /* int 0x40      */
        0xBB,0x11,0x11,0x00,0x00,  /* mov ebx,0x1111 */
        0xF4                       /* hlt           */
    };
    static const uint8_t handler[] = { /* @ 0x0100 */
        0xB8,0xCD,0xAB,0x00,0x00,  /* mov eax,0xABCD */
        0xCF                       /* iret          */
    };
    PhysMemoryMap *map = mk_map(0x10000);
    X86CPUState *s = x86_cpu_init(map);
    memcpy(g_ram, prog, sizeof(prog));
    memcpy(g_ram + 0x100, handler, sizeof(handler));

    /* GDT @ 0x1000: [1]=code32 flat, [2]=data32 flat */
    put64(g_ram + 0x1000 + 0x08, 0x00CF9B000000FFFFull);
    put64(g_ram + 0x1000 + 0x10, 0x00CF93000000FFFFull);
    /* IDT @ 0x1800: vector 0x40 -> interrupt gate, sel 0x08, offset 0x100 */
    put64(g_ram + 0x1800 + 0x40*8, 0x00008E0000080100ull);

    X86CPUSeg gdt = { .base = 0x1000, .limit = 0x17 };
    X86CPUSeg idt = { .base = 0x1800, .limit = 0x40*8 + 7 };
    X86CPUSeg cs  = { .sel = 0x08, .flags = 0xc09b, .base = 0, .limit = 0xffffffff };
    X86CPUSeg ds  = { .sel = 0x10, .flags = 0xc093, .base = 0, .limit = 0xffffffff };
    x86_cpu_set_seg(s, X86_CPU_SEG_GDT, &gdt);
    x86_cpu_set_seg(s, X86_CPU_SEG_IDT, &idt);
    x86_cpu_set_seg(s, X86_CPU_SEG_CS, &cs);
    x86_cpu_set_seg(s, X86_CPU_SEG_DS, &ds);
    x86_cpu_set_seg(s, X86_CPU_SEG_ES, &ds);
    x86_cpu_set_seg(s, X86_CPU_SEG_SS, &ds);
    x86_cpu_set_reg(s, X86_CPU_REG_CR0, 0x1); /* PE */
    x86_cpu_set_reg(s, X86_CPU_REG_EIP, 0);
    x86_cpu_set_reg(s, 4 /*ESP*/, 0x8000);

    x86_cpu_interp(s, 1000);
    printf("  interrupt: int 0x40 -> handler -> iret\n");
    CHECK(s, 0, 0xABCD); /* set by handler */
    CHECK(s, 3, 0x1111); /* set after iret  */
    x86_cpu_end(s);
}

static void test_flags(void)
{
    static const uint8_t p[] = {
        0xB8,0xFF,0xFF,0xFF,0xFF,  /* mov eax,0xffffffff */
        0xBB,0x00,0x00,0x00,0x00,  /* mov ebx,0          */
        0x83,0xC0,0x01,            /* add eax,1  (eax=0, CF=1) */
        0x83,0xD3,0x00,            /* adc ebx,0  (ebx=1)       */
        0xB9,0x07,0x00,0x00,0x00,  /* mov ecx,7          */
        0x83,0xE8,0x01,            /* sub eax,1  (eax=0xffffffff, CF=1) */
        0x19,0xC9,                 /* sbb ecx,ecx (ecx=0xffffffff)     */
        0xBA,0x01,0x00,0x00,0x80,  /* mov edx,0x80000001 */
        0xD1,0xC2,                 /* rol edx,1 (edx=3)  */
        0xBE,0x78,0x56,0x34,0x12,  /* mov esi,0x12345678 */
        0xBF,0xF0,0xDE,0xBC,0x9A,  /* mov edi,0x9abcdef0 */
        0x0F,0xA4,0xFE,0x08,       /* shld esi,edi,8 (esi=0x3456789a) */
        0xBD,0x00,0x00,0x00,0x00,  /* mov ebp,0          */
        0x39,0xF6,                 /* cmp esi,esi (ZF=1) */
        0xB8,0xAA,0xAA,0x00,0x00,  /* mov eax,0xAAAA     */
        0x0F,0x44,0xE8,            /* cmovz ebp,eax (ebp=0xAAAA) */
        0xF4
    };
    PhysMemoryMap *map = mk_map(0x10000);
    X86CPUState *s = mk_cpu(map, 0x8000);
    memcpy(g_ram, p, sizeof(p));
    x86_cpu_interp(s, 1000);
    printf("  flags: adc/sbb/rol/shld/cmovz\n");
    CHECK(s, 3, 1);            /* ebx: adc  */
    CHECK(s, 1, 0xffffffff);   /* ecx: sbb self */
    CHECK(s, 2, 3);            /* edx: rol  */
    CHECK(s, 6, 0x3456789a);   /* esi: shld */
    CHECK(s, 5, 0xAAAA);       /* ebp: cmovz */
    x86_cpu_end(s);
}

static void test_pop_esp(void)
{
    /* `pop [esp]` (part of the pushf;pop [esp] local_irq_save idiom): per Intel,
     * when ESP is the base register the destination address is computed AFTER ESP
     * is incremented, so the popped value must land at the *new* top of stack. */
    static const uint8_t p[] = {
        0xBC,0x00,0x80,0x00,0x00,   /* mov esp,0x8000                   */
        0x68,0x11,0x11,0x11,0x11,   /* push 0x11111111 (esp=0x7ffc)     */
        0x8F,0x04,0x24,             /* pop dword [esp] -> writes [0x8000]*/
        0xF4                        /* hlt                              */
    };
    PhysMemoryMap *map = mk_map(0x10000);
    X86CPUState *s = mk_cpu(map, 0x8000);
    memcpy(g_ram, p, sizeof(p));
    x86_cpu_interp(s, 1000);
    printf("  pop [esp]: post-increment destination address\n");
    uint32_t got;
    memcpy(&got, g_ram + 0x8000, 4);
    if (got != 0x11111111u) {
        printf("    *** FAIL: pop [esp] wrote 0x%08x to [0x8000], want 0x11111111\n", got);
        g_fail = 1;
    }
    CHECK(s, 4 /*ESP*/, 0x8000);
    x86_cpu_end(s);
}

/* A read where no RAM or device sits returns all-ones at the access width,
 * zero-extended: movzx of an unmapped word is 0x0000ffff, not 0xffffffff
 * (phys_read used to return 0xffffffff at every size). */
static void test_unmapped_read_width(void)
{
    static const uint8_t p[] = {
        0x0F,0xB7,0x1D,0x00,0x00,0x10,0x00,     /* movzx ebx, word [0x100000] */
        0x0F,0xB6,0x0D,0x00,0x00,0x10,0x00,     /* movzx ecx, byte [0x100000] */
        0x8B,0x15,0x00,0x00,0x10,0x00,          /* mov edx, [0x100000]        */
        0xF4
    };
    PhysMemoryMap *map = mk_map(0x10000);       /* 64 KB: 1 MB is unmapped */
    X86CPUState *s = mk_cpu(map, 0x8000);
    memcpy(g_ram, p, sizeof(p));
    x86_cpu_interp(s, 1000);
    printf("  unmapped read: movzx word/byte zero-extend all-ones\n");
    CHECK(s, 3, 0x0000ffff);
    CHECK(s, 1, 0x000000ff);
    CHECK(s, 2, 0xffffffff);
    x86_cpu_end(s);
}

static void test_bswap(void)
{
    static const uint8_t p[] = {
        0xB8,0x78,0x56,0x34,0x12,   /* mov eax,0x12345678 */
        0x0F,0xC8,                  /* bswap eax          */
        0xBB,0xEF,0xBE,0xAD,0xDE,   /* mov ebx,0xDEADBEEF */
        0x0F,0xCB,                  /* bswap ebx          */
        0xF4                        /* hlt                */
    };
    PhysMemoryMap *map = mk_map(0x10000);
    X86CPUState *s = mk_cpu(map, 0x8000);
    memcpy(g_ram, p, sizeof(p));
    x86_cpu_interp(s, 1000);
    printf("  bswap: byte-reverse eax/ebx\n");
    CHECK(s, 0, 0x78563412);
    CHECK(s, 3, 0xEFBEADDE);
    x86_cpu_end(s);
}

/* mov r/m, Sreg (8C): a 32-bit register destination is zero-extended, a 16-bit
 * one keeps the upper half, and a memory destination is a 16-bit store whatever
 * the operand size. glibc's i386 pthread_create depends on the first. */
static void test_mov_from_sreg(void)
{
    static const uint8_t p[] = {
        0xB8,0xEF,0xBE,0xAD,0xDE,         /* mov eax,0xdeadbeef     */
        0x8C,0xD8,                        /* mov eax,ds             */
        0xBB,0xEF,0xBE,0xAD,0xDE,         /* mov ebx,0xdeadbeef     */
        0x66,0x8C,0xDB,                   /* mov bx,ds              */
        0xC7,0x05,0x00,0x30,0,0,          /* mov dword [0x3000],    */
              0xEF,0xBE,0xAD,0xDE,        /*     0xdeadbeef         */
        0x8C,0x1D,0x00,0x30,0,0,          /* mov [0x3000],ds        */
        0x8B,0x0D,0x00,0x30,0,0,          /* mov ecx,[0x3000]       */
        0xF4                              /* hlt                    */
    };
    PhysMemoryMap *map = mk_map(0x10000);
    X86CPUState *s = mk_cpu(map, 0x8000);
    memcpy(g_ram, p, sizeof(p));
    x86_cpu_interp(s, 1000);
    printf("  mov r/m, Sreg: zero-extend r32, keep r16 high, 16-bit store\n");
    CHECK(s, 0 /*EAX*/, 0x00000018);
    CHECK(s, 3 /*EBX*/, 0xDEAD0018);
    CHECK(s, 1 /*ECX*/, 0xDEAD0018);
    x86_cpu_end(s);
}

static void test_call_indirect_pf_restart(void)
{
    /* Regression for the "non-restartable instruction" bug: an indirect
     * `call [mem]` whose operand read demand-faults (#PF) must NOT push the
     * return address before the faulting read. If it does, the kernel handles
     * the fault and restarts the instruction, and the push is applied twice --
     * leaving ESP 4 bytes too low and corrupting every later stack offset.
     * (This is exactly what crashed busybox/Alpine userspace: argc/argv shifted
     * by 4 -> deref of argc(=1).)
     *
     * Setup: paging on; the jump-table page (VA 0x5000) starts NOT-present. A
     * #PF handler marks it present and IRETs, so the call restarts and must push
     * the return address exactly once (ESP: 0x8000 -> 0x7ffc, not 0x7ff8). */
    static const uint8_t prog[] = {         /* @VA 0x0000 */
        0xBB,0x00,0x00,0x10,0x00,           /* mov ebx,0x100000          */
        0x0F,0x22,0xDB,                     /* mov cr3,ebx               */
        0xBA,0x01,0x00,0x00,0x80,           /* mov edx,0x80000001        */
        0x0F,0x22,0xC2,                     /* mov cr0,edx (enable PG)   */
        0xBC,0x00,0x80,0x00,0x00,           /* mov esp,0x8000            */
        0xFF,0x15,0x00,0x50,0x00,0x00,      /* call dword [0x5000]       */
        0xF4                                /* hlt  (retaddr = 0x001B)   */
    };
    static const uint8_t handler[] = {      /* @VA 0x0100  (#PF handler) */
        0xA1,0x14,0x10,0x10,0x00,           /* mov eax,[0x101014] (PTE)  */
        0x83,0xC8,0x01,                     /* or  eax,1  (present)      */
        0xA3,0x14,0x10,0x10,0x00,           /* mov [0x101014],eax        */
        0x58,                               /* pop eax  (drop err code)  */
        0xCF                                /* iret                      */
    };
    static const uint8_t target[] = { 0xF4 }; /* @VA 0x0200: hlt         */

    PhysMemoryMap *map = mk_map(0x200000);  /* 2 MB */
    X86CPUState *s = x86_cpu_init(map);
    memcpy(g_ram + 0x000, prog, sizeof(prog));
    memcpy(g_ram + 0x100, handler, sizeof(handler));
    memcpy(g_ram + 0x200, target, sizeof(target));
    put64(g_ram + 0x5000, 0x00000200);      /* jump-table[0] -> 0x200    */

    /* GDT @0x1000: [1]=code32 flat, [2]=data32 flat */
    put64(g_ram + 0x1000 + 0x08, 0x00CF9B000000FFFFull);
    put64(g_ram + 0x1000 + 0x10, 0x00CF93000000FFFFull);
    /* IDT @0x1800: vector 14 (#PF) -> interrupt gate sel 0x08 offset 0x100 */
    put64(g_ram + 0x1800 + 14*8, 0x00008E0000080100ull);

    /* Page directory @0x100000, page table @0x101000; PD[0] -> PT. */
    { uint32_t *pd = (uint32_t *)(g_ram + 0x100000);
      uint32_t *pt = (uint32_t *)(g_ram + 0x101000);
      pd[0]   = 0x101000 | 3;
      pt[0]   = 0x00000 | 3;    /* code + handler + target        */
      pt[1]   = 0x01000 | 3;    /* GDT/IDT                        */
      pt[5]   = 0x05000 | 2;    /* jump table: PRESENT BIT CLEAR   */
      pt[7]   = 0x07000 | 3;    /* stack grows down from 0x8000 into page 7 */
      pt[8]   = 0x08000 | 3;    /* stack top page                 */
      pt[257] = 0x101000 | 3;   /* map the PT page so handler can edit it */
    }

    X86CPUSeg gdt = { .base = 0x1000, .limit = 0x17 };
    X86CPUSeg idt = { .base = 0x1800, .limit = 14*8 + 7 };
    X86CPUSeg cs  = { .sel = 0x08, .flags = 0xc09b, .base = 0, .limit = 0xffffffff };
    X86CPUSeg ds  = { .sel = 0x10, .flags = 0xc093, .base = 0, .limit = 0xffffffff };
    x86_cpu_set_seg(s, X86_CPU_SEG_GDT, &gdt);
    x86_cpu_set_seg(s, X86_CPU_SEG_IDT, &idt);
    x86_cpu_set_seg(s, X86_CPU_SEG_CS, &cs);
    x86_cpu_set_seg(s, X86_CPU_SEG_DS, &ds);
    x86_cpu_set_seg(s, X86_CPU_SEG_ES, &ds);
    x86_cpu_set_seg(s, X86_CPU_SEG_SS, &ds);
    x86_cpu_set_reg(s, X86_CPU_REG_CR0, 0x1); /* PE (guest enables PG itself) */
    x86_cpu_set_reg(s, X86_CPU_REG_EIP, 0);

    x86_cpu_interp(s, 100000);
    printf("  call [mem] restart: #PF on operand read must not double-push\n");
    /* Landed at the target stub (0x200) and pushed the return addr once. */
    CHECK(s, X86_CPU_REG_EIP, 0x201); /* one past the target hlt */
    CHECK(s, 4 /*ESP*/, 0x7ffc);      /* single push (double push => 0x7ff8) */
    uint32_t ret;
    memcpy(&ret, g_ram + 0x7ffc, 4);
    if (ret != 0x0000001Bu) {
        printf("    *** FAIL: return addr on stack = 0x%08x, want 0x0000001B\n", ret);
        g_fail = 1;
    }
    x86_cpu_end(s);
}

static void test_page_cross(void)
{
    /* A load/store that straddles a page boundary must translate each page
     * separately.  Physical pages backing consecutive linear pages are
     * unrelated, so translating only the first byte and then touching `size`
     * contiguous *physical* bytes silently corrupts the wrong page -- the class
     * of bug that shows up as random userspace corruption.
     *
     * VA 0x6000 -> phys 0x6000 and VA 0x7000 -> phys 0x20000 (deliberately not
     * adjacent).  A dword store at VA 0x6ffe must split 2 + 2 bytes across
     * phys 0x6ffe and phys 0x20000, and read back identically. */
    static const uint8_t p[] = {
        0xBB,0x00,0x00,0x10,0x00,       /* mov ebx,0x100000       */
        0x0F,0x22,0xDB,                 /* mov cr3,ebx            */
        0xBA,0x01,0x00,0x00,0x80,       /* mov edx,0x80000001     */
        0x0F,0x22,0xC2,                 /* mov cr0,edx (PG|PE)    */
        0xB8,0xBE,0xBA,0xFE,0xCA,       /* mov eax,0xCAFEBABE     */
        0xA3,0xFE,0x6F,0x00,0x00,       /* mov [0x6ffe],eax       */
        0x8B,0x35,0xFE,0x6F,0x00,0x00,  /* mov esi,[0x6ffe]       */
        0xF4                            /* hlt                    */
    };
    PhysMemoryMap *map = mk_map(0x200000);
    X86CPUState *s = mk_cpu(map, 0x8000);
    memcpy(g_ram, p, sizeof(p));
    { uint32_t *pd = (uint32_t *)(g_ram + 0x100000);
      uint32_t *pt = (uint32_t *)(g_ram + 0x101000);
      pd[0] = 0x101000 | 3;
      pt[0] = 0x00000 | 3;      /* code                              */
      pt[6] = 0x06000 | 3;      /* first half of the split access    */
      pt[7] = 0x20000 | 3;      /* second half: a far-away phys page */
    }
    x86_cpu_interp(s, 1000);
    printf("  page cross: dword access straddling two pages splits per page\n");
    CHECK(s, 6 /*ESI*/, 0xCAFEBABE);
    if (g_ram[0x6ffe] != 0xBE || g_ram[0x6fff] != 0xBA) {
        printf("    *** FAIL: low half at phys 0x6ffe = %02x %02x, want be ba\n",
               g_ram[0x6ffe], g_ram[0x6fff]); g_fail = 1;
    }
    if (g_ram[0x20000] != 0xFE || g_ram[0x20001] != 0xCA) {
        printf("    *** FAIL: high half at phys 0x20000 = %02x %02x, want fe ca\n",
               g_ram[0x20000], g_ram[0x20001]); g_fail = 1;
    }
    if (g_ram[0x7000] != 0 || g_ram[0x7001] != 0) {
        printf("    *** FAIL: wrote through to phys 0x7000 (unsplit access)\n");
        g_fail = 1;
    }
    x86_cpu_end(s);
}

/* Shared scaffolding for the write-fault restart tests below: paging on with
 * CR0.WP, a #PF handler at VA 0x100 that adds the write bit to PTE[5] and
 * returns, so a store to VA 0x5000 faults exactly once and is then restarted. */
static X86CPUState *mk_wpfault_cpu(PhysMemoryMap *map, const uint8_t *prog,
                                   size_t proglen)
{
    static const uint8_t handler[] = {  /* @VA 0x0100 (#PF) */
        0xA1,0x14,0x10,0x10,0x00,       /* mov eax,[0x101014] (PTE for page 5) */
        0x83,0xC8,0x02,                 /* or  eax,2  (writable)               */
        0xA3,0x14,0x10,0x10,0x00,       /* mov [0x101014],eax                  */
        0x58,                           /* pop eax  (drop error code)          */
        0xCF                            /* iret                                */
    };
    X86CPUState *s = x86_cpu_init(map);
    memcpy(g_ram + 0x000, prog, proglen);
    memcpy(g_ram + 0x100, handler, sizeof(handler));
    put64(g_ram + 0x1000 + 0x08, 0x00CF9B000000FFFFull); /* GDT[1] code32 flat */
    put64(g_ram + 0x1000 + 0x10, 0x00CF93000000FFFFull); /* GDT[2] data32 flat */
    put64(g_ram + 0x1800 + 14*8, 0x00008E0000080100ull); /* IDT[14] -> 0x100   */
    { uint32_t *pd = (uint32_t *)(g_ram + 0x100000);
      uint32_t *pt = (uint32_t *)(g_ram + 0x101000);
      pd[0]   = 0x101000 | 3;
      pt[0]   = 0x00000 | 3;    /* code + handler                        */
      pt[1]   = 0x01000 | 3;    /* GDT/IDT                               */
      pt[5]   = 0x05000 | 1;    /* target page: PRESENT, READ-ONLY       */
      pt[7]   = 0x07000 | 3;    /* stack                                 */
      pt[8]   = 0x08000 | 3;
      pt[257] = 0x101000 | 3;   /* PT itself, so the handler can edit it */
    }
    { X86CPUSeg gdt = { .base = 0x1000, .limit = 0x17 };
      X86CPUSeg idt = { .base = 0x1800, .limit = 14*8 + 7 };
      X86CPUSeg cs  = { .sel = 0x08, .flags = 0xc09b, .base = 0, .limit = 0xffffffff };
      X86CPUSeg ds  = { .sel = 0x10, .flags = 0xc093, .base = 0, .limit = 0xffffffff };
      x86_cpu_set_seg(s, X86_CPU_SEG_GDT, &gdt);
      x86_cpu_set_seg(s, X86_CPU_SEG_IDT, &idt);
      x86_cpu_set_seg(s, X86_CPU_SEG_CS, &cs);
      x86_cpu_set_seg(s, X86_CPU_SEG_DS, &ds);
      x86_cpu_set_seg(s, X86_CPU_SEG_ES, &ds);
      x86_cpu_set_seg(s, X86_CPU_SEG_SS, &ds); }
    x86_cpu_set_reg(s, X86_CPU_REG_CR0, 0x1); /* PE; the guest enables PG|WP */
    x86_cpu_set_reg(s, X86_CPU_REG_EIP, 0);
    return s;
}

static void test_xadd_pf_restart(void)
{
    /* `xadd [mem],reg` must store before updating the source register.  The
     * store can #PF on a page that is readable but not yet writable (a COW or
     * zero page), and the restarted instruction would then add the destination
     * to itself: mem = 2*old instead of old + reg. */
    static const uint8_t prog[] = {
        0xBB,0x00,0x00,0x10,0x00,       /* mov ebx,0x100000            */
        0x0F,0x22,0xDB,                 /* mov cr3,ebx                 */
        0xBA,0x01,0x00,0x01,0x80,       /* mov edx,0x80010001 PG|WP|PE */
        0x0F,0x22,0xC2,                 /* mov cr0,edx                 */
        0xBC,0x00,0x80,0x00,0x00,       /* mov esp,0x8000              */
        0xBB,0x05,0x00,0x00,0x00,       /* mov ebx,5                   */
        0x0F,0xC1,0x1D,0x00,0x50,0x00,0x00, /* xadd [0x5000],ebx       */
        0xF4                            /* hlt                         */
    };
    PhysMemoryMap *map = mk_map(0x200000);
    X86CPUState *s = mk_wpfault_cpu(map, prog, sizeof(prog));
    { uint32_t v = 0x100; memcpy(g_ram + 0x5000, &v, 4); }
    x86_cpu_interp(s, 100000);
    printf("  xadd restart: #PF on the store must not double-add\n");
    CHECK(s, 3 /*EBX*/, 0x100);         /* old destination value */
    { uint32_t got; memcpy(&got, g_ram + 0x5000, 4);
      if (got != 0x105) {
          printf("    *** FAIL: [0x5000]=0x%x want 0x105%s\n", got,
                 got == 0x200 ? " (destination added to itself)" : "");
          g_fail = 1;
      } }
    x86_cpu_end(s);
}

static void test_pop_mem_pf_restart(void)
{
    /* `pop [mem]` must commit ESP only after the store succeeds; otherwise a
     * #PF on the destination restarts the instruction with ESP already
     * advanced, popping a second slot and shifting the whole stack frame. */
    static const uint8_t prog[] = {
        0xBB,0x00,0x00,0x10,0x00,       /* mov ebx,0x100000       */
        0x0F,0x22,0xDB,                 /* mov cr3,ebx            */
        0xBA,0x01,0x00,0x01,0x80,       /* mov edx,0x80010001     */
        0x0F,0x22,0xC2,                 /* mov cr0,edx            */
        0xBC,0x00,0x80,0x00,0x00,       /* mov esp,0x8000         */
        0x68,0x34,0x12,0x00,0x00,       /* push 0x1234            */
        0x8F,0x05,0x00,0x50,0x00,0x00,  /* pop dword [0x5000]     */
        0xF4                            /* hlt                    */
    };
    PhysMemoryMap *map = mk_map(0x200000);
    X86CPUState *s = mk_wpfault_cpu(map, prog, sizeof(prog));
    x86_cpu_interp(s, 100000);
    printf("  pop [mem] restart: #PF on the store must not double-pop\n");
    CHECK(s, 4 /*ESP*/, 0x8000);        /* exactly one slot popped */
    { uint32_t got; memcpy(&got, g_ram + 0x5000, 4);
      if (got != 0x1234) {
          printf("    *** FAIL: [0x5000]=0x%x want 0x1234\n", got); g_fail = 1;
      } }
    x86_cpu_end(s);
}

static void test_bt_imm_modulo(void)
{
    /* The immediate form of bt/bts/btr/btc takes the bit offset modulo the
     * operand size, even for a memory bit base: `bts dword [m],33` sets bit 1
     * of that dword and must not write the neighbouring dword. */
    static const uint8_t p[] = {
        0xC7,0x05,0x00,0x50,0x00,0x00,0,0,0,0,   /* mov dword [0x5000],0  */
        0xC7,0x05,0x04,0x50,0x00,0x00,0,0,0,0,   /* mov dword [0x5004],0  */
        0x0F,0xBA,0x2D,0x00,0x50,0x00,0x00,0x21, /* bts dword [0x5000],33 */
        0xA1,0x00,0x50,0x00,0x00,                /* mov eax,[0x5000]      */
        0x8B,0x1D,0x04,0x50,0x00,0x00,           /* mov ebx,[0x5004]      */
        0xF4
    };
    PhysMemoryMap *map = mk_map(0x10000);
    X86CPUState *s = mk_cpu(map, 0x8000);
    memcpy(g_ram, p, sizeof(p));
    x86_cpu_interp(s, 1000);
    printf("  bts imm: bit offset taken modulo the operand size\n");
    CHECK(s, 0 /*EAX*/, 2);   /* bit 33 mod 32 == bit 1 */
    CHECK(s, 3 /*EBX*/, 0);   /* neighbouring dword untouched */
    x86_cpu_end(s);
}


/* ----------------------------------------------------------------------- */
/* Protected-mode scaffolding shared by the privilege tests below: paging off,
 * GDT @0x1000 ([1] code32 DPL0, [2] data32 DPL0, [3] code32 DPL3, [4] data32
 * DPL3, [5] TSS), IDT @0x1800 (256 vectors), TSS @0x2000 with ESP0/SS0 so an
 * exception taken at CPL3 can switch to the ring-0 stack. */
static X86CPUState *mk_prot_cpu(PhysMemoryMap *map)
{
    X86CPUState *s = x86_cpu_init(map);
    put64(g_ram + 0x1000 + 0x08, 0x00CF9B000000FFFFull);  /* 0x08 code32 DPL0 */
    put64(g_ram + 0x1000 + 0x10, 0x00CF93000000FFFFull);  /* 0x10 data32 DPL0 */
    put64(g_ram + 0x1000 + 0x18, 0x00CFFB000000FFFFull);  /* 0x18 code32 DPL3 */
    put64(g_ram + 0x1000 + 0x20, 0x00CFF3000000FFFFull);  /* 0x20 data32 DPL3 */
    put64(g_ram + 0x1000 + 0x28, 0x0000890020000067ull);  /* 0x28 TSS @0x2000  */
    { uint32_t esp0 = 0x9000, ss0 = 0x10;
      memcpy(g_ram + 0x2000 + 4, &esp0, 4);
      memcpy(g_ram + 0x2000 + 8, &ss0, 4); }
    { X86CPUSeg gdt = { .base = 0x1000, .limit = 0x2f };
      X86CPUSeg idt = { .base = 0x1800, .limit = 256*8 - 1 };
      X86CPUSeg cs  = { .sel = 0x08, .flags = 0xc09b, .base = 0, .limit = 0xffffffff };
      X86CPUSeg ds  = { .sel = 0x10, .flags = 0xc093, .base = 0, .limit = 0xffffffff };
      X86CPUSeg tr  = { .sel = 0x28, .flags = 0x0089, .base = 0x2000, .limit = 0x67 };
      x86_cpu_set_seg(s, X86_CPU_SEG_GDT, &gdt);
      x86_cpu_set_seg(s, X86_CPU_SEG_IDT, &idt);
      x86_cpu_set_seg(s, X86_CPU_SEG_TR, &tr);
      x86_cpu_set_seg(s, X86_CPU_SEG_CS, &cs);
      x86_cpu_set_seg(s, X86_CPU_SEG_DS, &ds);
      x86_cpu_set_seg(s, X86_CPU_SEG_ES, &ds);
      x86_cpu_set_seg(s, X86_CPU_SEG_SS, &ds); }
    x86_cpu_set_reg(s, X86_CPU_REG_CR0, 0x1);   /* PE, paging off */
    x86_cpu_set_reg(s, X86_CPU_REG_EIP, 0);
    x86_cpu_set_reg(s, 4 /*ESP*/, 0x9000);
    return s;
}

/* 32-bit interrupt gate through CS 0x08 at the given descriptor privilege. */
static void set_gate(int vec, uint32_t off, int dpl)
{
    uint32_t hi = (off & 0xffff0000u) | 0x8e00u | ((uint32_t)dpl << 13);
    uint32_t lo = (0x0008u << 16) | (off & 0xffffu);
    put64(g_ram + 0x1800 + vec * 8, ((uint64_t)hi << 32) | lo);
}

/* Ring-0 trampoline @0x0000: IRET into CPL3 code at 0x0500 with ESP3 = 0x8000. */
static const uint8_t to_ring3[] = {
    0x68,0x23,0x00,0x00,0x00,   /* push 0x23   (SS3)     */
    0x68,0x00,0x80,0x00,0x00,   /* push 0x8000 (ESP3)    */
    0x68,0x02,0x00,0x00,0x00,   /* push 0x0002 (EFLAGS)  */
    0x68,0x1B,0x00,0x00,0x00,   /* push 0x1b   (CS3)     */
    0x68,0x00,0x05,0x00,0x00,   /* push 0x0500 (EIP3)    */
    0xCF                        /* iret                  */
};
/* #GP handler @0x0300: record the error code in EBX and a marker in EAX. */
static const uint8_t gp_handler[] = {
    0x5B,                       /* pop ebx  (error code) */
    0xB8,0xEE,0x00,0x00,0x00,   /* mov eax,0xEE          */
    0xF4                        /* hlt                   */
};

static X86CPUState *mk_ring3_cpu(PhysMemoryMap *map, const uint8_t *r3, size_t n)
{
    X86CPUState *s = mk_prot_cpu(map);
    memcpy(g_ram + 0x000, to_ring3, sizeof(to_ring3));
    memcpy(g_ram + 0x300, gp_handler, sizeof(gp_handler));
    memcpy(g_ram + 0x500, r3, n);
    set_gate(13, 0x300, 0);     /* #GP: gate DPL 0 -- exceptions ignore it */
    return s;
}

static void test_int_gate_dpl(void)
{
    /* A *software* interrupt must honour the gate's DPL: `int 0x40` at CPL3
     * through a DPL-0 gate is #GP(vector*8+2), not a free ride into ring 0.
     * Without the check any userspace program could fake #PF/#GP/any vector. */
    static const uint8_t r3[] = { 0xCD,0x40, 0xF4 };        /* int 0x40; hlt */
    static const uint8_t h40[] = { 0xB8,0xBB,0x00,0x00,0x00, 0xF4 }; /* mov eax,0xBB; hlt */
    printf("  int n: gate DPL is checked for software interrupts\n");
    {   /* gate DPL 0, invoked from CPL 3 -> #GP(0x40*8+2) */
        PhysMemoryMap *map = mk_map(0x10000);
        X86CPUState *s = mk_ring3_cpu(map, r3, sizeof(r3));
        memcpy(g_ram + 0x400, h40, sizeof(h40));
        set_gate(0x40, 0x400, 0);
        x86_cpu_interp(s, 100000);
        CHECK(s, 0 /*EAX*/, 0xEE);      /* the #GP handler ran ... */
        CHECK(s, 3 /*EBX*/, 0x40*8+2);  /* ... with the right error code */
        x86_cpu_end(s);
    }
    {   /* same gate at DPL 3: the interrupt is allowed through */
        PhysMemoryMap *map = mk_map(0x10000);
        X86CPUState *s = mk_ring3_cpu(map, r3, sizeof(r3));
        memcpy(g_ram + 0x400, h40, sizeof(h40));
        set_gate(0x40, 0x400, 3);
        x86_cpu_interp(s, 100000);
        CHECK(s, 0 /*EAX*/, 0xBB);
        x86_cpu_end(s);
    }
}

static void test_iopl(void)
{
    /* cli/sti and the port ops are IOPL-sensitive: #GP(0) at CPL > IOPL. */
    static const uint8_t r3[] = { 0xFA, 0xF4 };             /* cli; hlt */
    PhysMemoryMap *map = mk_map(0x10000);
    X86CPUState *s = mk_ring3_cpu(map, r3, sizeof(r3));
    x86_cpu_interp(s, 100000);
    printf("  iopl: cli at CPL3 with IOPL=0 raises #GP(0)\n");
    CHECK(s, 0 /*EAX*/, 0xEE);
    CHECK(s, 3 /*EBX*/, 0);
    x86_cpu_end(s);
}

static void test_popf_cpl3(void)
{
    /* popf at CPL3 may not raise IOPL (CPL>0) nor set IF (CPL>IOPL); the other
     * flags still load.  Here only CF survives from the popped value. */
    static const uint8_t r3[] = {
        0x68,0x03,0x32,0x00,0x00,   /* push 0x3203 (IOPL=3, IF, CF) */
        0x9D,                       /* popf                          */
        0x9C, 0x58,                 /* pushf; pop eax                */
        0xF4
    };
    PhysMemoryMap *map = mk_map(0x10000);
    X86CPUState *s = mk_ring3_cpu(map, r3, sizeof(r3));
    x86_cpu_interp(s, 100000);
    printf("  popf: CPL3 cannot change IOPL or IF\n");
    CHECK(s, 0 /*EAX*/, 0x3);       /* 0x2 (reserved) | CF; no IF, no IOPL */
    x86_cpu_end(s);
}

static void test_far_indirect(void)
{
    /* FF /5 (jmp far m16:32) and FF /3 (call far m16:32) used to stop the CPU. */
    static const uint8_t p[] = {
        0xC7,0x05,0x00,0x40,0x00,0x00,0x00,0x06,0x00,0x00, /* mov [0x4000],0x600 */
        0x66,0xC7,0x05,0x04,0x40,0x00,0x00,0x08,0x00,      /* mov word [0x4004],8 */
        0xC7,0x05,0x08,0x40,0x00,0x00,0x00,0x07,0x00,0x00, /* mov [0x4008],0x700 */
        0x66,0xC7,0x05,0x0C,0x40,0x00,0x00,0x08,0x00,      /* mov word [0x400c],8 */
        0xFF,0x1D,0x08,0x40,0x00,0x00,   /* call far [0x4008] -> 0x700         */
        0xFF,0x2D,0x00,0x40,0x00,0x00,   /* jmp  far [0x4000] -> 0x600         */
        0xBB,0xAD,0xDE,0x00,0x00,        /* mov ebx,0xDEAD (must be skipped)   */
        0xF4
    };
    static const uint8_t at600[] = { 0xB8,0x0D,0x60,0x00,0x00, 0xF4 }; /* mov eax,0x600D; hlt */
    static const uint8_t at700[] = { 0xB9,0x0D,0x70,0x00,0x00, 0xCB }; /* mov ecx,0x700D; retf */
    PhysMemoryMap *map = mk_map(0x10000);
    X86CPUState *s = mk_prot_cpu(map);
    memcpy(g_ram, p, sizeof(p));
    memcpy(g_ram + 0x600, at600, sizeof(at600));
    memcpy(g_ram + 0x700, at700, sizeof(at700));
    x86_cpu_interp(s, 100000);
    printf("  far indirect: FF /3 call far and FF /5 jmp far through memory\n");
    CHECK(s, 1 /*ECX*/, 0x700D);
    CHECK(s, 0 /*EAX*/, 0x600D);
    CHECK(s, 3 /*EBX*/, 0);
    x86_cpu_end(s);
}

static void test_addr16(void)
{
    /* The 0x67 address-size prefix selects 16-bit addressing: BX+SI style
     * ModR/M, offsets that wrap at 64K, and CX/SI/DI for the string ops. */
    static const uint8_t p[] = {
        0x66,0xBB,0x00,0x01,             /* mov bx,0x100                      */
        0x66,0xBE,0x10,0x00,             /* mov si,0x10                       */
        0xC7,0x05,0x10,0x01,0x00,0x00, 0x78,0x56,0x34,0x12, /* [0x110]=0x12345678 */
        0x67,0x8B,0x00,                  /* mov eax,[bx+si]  (addr16)         */
        0x66,0xBB,0x00,0xF0,             /* mov bx,0xf000                     */
        0x66,0xBE,0x00,0x50,             /* mov si,0x5000                     */
        0xC7,0x05,0x00,0x40,0x01,0x00, 0x99,0x00,0x00,0x00, /* [0x14000]=0x99    */
        0xC7,0x05,0x00,0x40,0x00,0x00, 0x55,0x00,0x00,0x00, /* [0x4000]=0x55     */
        0x67,0x8B,0x18,                  /* mov ebx,[bx+si]: 0x14000 wraps to  */
                                         /* 0x4000, it is not a 32-bit address */
        0xBE,0x00,0x20,0x00,0x00,        /* mov esi,0x2000                    */
        0xBF,0x00,0x30,0x00,0x00,        /* mov edi,0x3000                    */
        0xB9,0x05,0x00,0x01,0x00,        /* mov ecx,0x00010005                */
        0xFC,                            /* cld                               */
        0x67,0xF3,0xA4,                  /* rep movsb (addr16: CX = 5)        */
        0xF4
    };
    PhysMemoryMap *map = mk_map(0x20000);
    X86CPUState *s = mk_cpu(map, 0x8000);
    memcpy(g_ram, p, sizeof(p));
    memcpy(g_ram + 0x2000, "Hello", 5);
    x86_cpu_interp(s, 100000);
    printf("  addr16: 0x67 ModR/M, 64K wrap, and CX-counted string ops\n");
    CHECK(s, 0 /*EAX*/, 0x12345678);
    CHECK(s, 3 /*EBX*/, 0x55);          /* 0xf000+0x5000 wrapped to 0x4000 */
    CHECK(s, 1 /*ECX*/, 0x00010000);    /* only CX was decremented */
    if (memcmp(g_ram + 0x3000, "Hello", 5) != 0) {
        printf("    *** FAIL: addr16 rep movsb did not copy\n"); g_fail = 1;
    }
    x86_cpu_end(s);
}

static void test_lea_ss_base(void)
{
    /* `lea` yields the segment *offset*.  [ebp+n] is formed against SS, so
     * subtracting the DS base (the old implementation) is wrong whenever the
     * bases differ -- harmless under Linux's flat model, wrong in general. */
    static const uint8_t p[] = {
        0xBD,0x04,0x00,0x00,0x00,        /* mov ebp,4        */
        0x8D,0x45,0x08,                  /* lea eax,[ebp+8]  */
        0x8D,0x0D,0x30,0x00,0x00,0x00,   /* lea ecx,[0x30]   */
        0xF4
    };
    PhysMemoryMap *map = mk_map(0x10000);
    X86CPUState *s = mk_cpu(map, 0x8000);
    X86CPUSeg ss = { .sel = 0x18, .flags = 0xc093, .base = 0x1000, .limit = 0xffffffff };
    memcpy(g_ram, p, sizeof(p));
    x86_cpu_set_seg(s, X86_CPU_SEG_SS, &ss);   /* deliberately not flat */
    x86_cpu_interp(s, 1000);
    printf("  lea: returns the offset, not offset + a mismatched segment base\n");
    CHECK(s, 0 /*EAX*/, 12);
    CHECK(s, 1 /*ECX*/, 0x30);
    x86_cpu_end(s);
}

static void test_shld_of(void)
{
    /* shld/shrd set OF for a 1-bit shift: OF = the destination's sign changed. */
    static const uint8_t p[] = {
        0xB8,0x00,0x00,0x00,0x40,        /* mov eax,0x40000000    */
        0xBB,0x00,0x00,0x00,0x80,        /* mov ebx,0x80000000    */
        0x0F,0xA4,0xD8,0x01,             /* shld eax,ebx,1        */
        0x9C,0x5A,                       /* pushf; pop edx        */
        0xB9,0x00,0x00,0x00,0x10,        /* mov ecx,0x10000000    */
        0x0F,0xA4,0xD9,0x01,             /* shld ecx,ebx,1        */
        0x9C,0x5E,                       /* pushf; pop esi        */
        0xF4
    };
    PhysMemoryMap *map = mk_map(0x10000);
    X86CPUState *s = mk_cpu(map, 0x8000);
    memcpy(g_ram, p, sizeof(p));
    x86_cpu_interp(s, 1000);
    printf("  shld: OF reflects a sign change on a 1-bit shift\n");
    CHECK(s, 0 /*EAX*/, 0x80000001);
    if (!(x86_cpu_get_reg(s, 2 /*EDX*/) & 0x800)) {
        printf("    *** FAIL: OF clear after a sign-changing shld\n"); g_fail = 1;
    }
    CHECK(s, 1 /*ECX*/, 0x20000001);
    if (x86_cpu_get_reg(s, 6 /*ESI*/) & 0x800) {
        printf("    *** FAIL: OF set after a shld that kept the sign\n"); g_fail = 1;
    }
    x86_cpu_end(s);
}

static void test_bcd(void)
{
    /* daa/das/aaa/aam/aad used to stop the CPU outright. */
    static const uint8_t p[] = {
        0xB0,0x15, 0x04,0x27, 0x27,      /* mov al,15h; add al,27h; daa -> 42h */
        0x88,0xC3,                       /* mov bl,al                          */
        0xB0,0x32, 0x2C,0x17, 0x2F,      /* mov al,32h; sub al,17h; das -> 15h */
        0x88,0xC1,                       /* mov cl,al                          */
        0x66,0xB8,0x06,0x01, 0x04,0x05, 0x37, /* mov ax,0106h; add al,5; aaa   */
        0x66,0x89,0xC2,                  /* mov dx,ax    -> 0201h              */
        0xB0,0x1D, 0xD4,0x0A,            /* mov al,29; aam    -> ax = 0209h    */
        0x66,0x89,0xC5,                  /* mov bp,ax                          */
        0x66,0xB8,0x05,0x02, 0xD5,0x0A,  /* mov ax,0205h; aad -> ax = 0019h    */
        0x66,0x89,0xC7,                  /* mov di,ax                          */
        0xF4
    };
    PhysMemoryMap *map = mk_map(0x10000);
    X86CPUState *s = mk_cpu(map, 0x8000);
    memcpy(g_ram, p, sizeof(p));
    x86_cpu_interp(s, 1000);
    printf("  bcd: daa/das/aaa/aam/aad\n");
    CHECK(s, 3 /*EBX*/, 0x42);
    CHECK(s, 1 /*ECX*/, 0x15);
    CHECK(s, 2 /*EDX*/, 0x0201);
    CHECK(s, 5 /*EBP*/, 0x0209);
    CHECK(s, 7 /*EDI*/, 0x0019);
    x86_cpu_end(s);
}

static void test_bound_arpl(void)
{
    /* bound (0x62) in range must not fault; out of range raises #BR (vector 5).
     * arpl (0x63) raises the RPL of the destination selector and sets ZF. */
    static const uint8_t p[] = {
        0xC7,0x05,0x00,0x40,0x00,0x00, 0x0A,0x00,0x00,0x00, /* [0x4000]=10 */
        0xC7,0x05,0x04,0x40,0x00,0x00, 0x14,0x00,0x00,0x00, /* [0x4004]=20 */
        0xB8,0x0F,0x00,0x00,0x00,        /* mov eax,15                     */
        0x62,0x05,0x00,0x40,0x00,0x00,   /* bound eax,[0x4000]  (in range) */
        0x66,0xC7,0x05,0x10,0x40,0x00,0x00, 0x08,0x00, /* mov word [0x4010],8 */
        0x66,0xBB,0x03,0x00,             /* mov bx,3                       */
        0x63,0x1D,0x10,0x40,0x00,0x00,   /* arpl [0x4010],bx               */
        0x0F,0x94,0xC1,                  /* setz cl                        */
        0x66,0x8B,0x15,0x10,0x40,0x00,0x00, /* mov dx,[0x4010]             */
        0xB8,0x63,0x00,0x00,0x00,        /* mov eax,99  (out of range)     */
        0x62,0x05,0x00,0x40,0x00,0x00,   /* bound eax,[0x4000]  -> #BR     */
        0xBD,0xAD,0xDE,0x00,0x00,        /* mov ebp,0xDEAD (must be skipped)*/
        0xF4
    };
    static const uint8_t br[] = { 0xBE,0xB2,0x00,0x00,0x00, 0xF4 }; /* mov esi,0xB2; hlt */
    PhysMemoryMap *map = mk_map(0x10000);
    X86CPUState *s = mk_prot_cpu(map);
    memcpy(g_ram, p, sizeof(p));
    memcpy(g_ram + 0x300, br, sizeof(br));
    set_gate(5, 0x300, 0);
    x86_cpu_interp(s, 100000);
    printf("  bound/arpl: range check raises #BR, arpl raises RPL and sets ZF\n");
    CHECK(s, 1 /*ECX*/, 1);        /* ZF: the RPL was raised */
    CHECK(s, 2 /*EDX*/, 0x0b);     /* selector 0x08 -> RPL 3 */
    CHECK(s, 6 /*ESI*/, 0xB2);     /* the #BR handler ran    */
    CHECK(s, 5 /*EBP*/, 0);        /* the faulting bound did not fall through */
    x86_cpu_end(s);
}

static void test_enter_level(void)
{
    /* `enter n, level` builds the display: old EBP, then level-1 copied frame
     * pointers, then the new frame pointer.  The whole sequence is gathered and
     * probed before the first store so a #PF cannot half-apply it. */
    static const uint8_t p[] = {
        0xC7,0x05,0xFC,0x6F,0x00,0x00, 0x01,0x00,0xAA,0xAA, /* [0x6ffc]=0xAAAA0001 */
        0xBD,0x00,0x70,0x00,0x00,        /* mov ebp,0x7000  */
        0xBC,0x00,0x78,0x00,0x00,        /* mov esp,0x7800  */
        0xC8,0x08,0x00,0x02,             /* enter 8,2       */
        0xF4
    };
    PhysMemoryMap *map = mk_map(0x10000);
    X86CPUState *s = mk_cpu(map, 0x8000);
    memcpy(g_ram, p, sizeof(p));
    x86_cpu_interp(s, 1000);
    printf("  enter: nested display and the 8-byte frame allocation\n");
    CHECK(s, 5 /*EBP*/, 0x77fc);
    CHECK(s, 4 /*ESP*/, 0x77ec);
    { uint32_t a, b2, d;
      memcpy(&a, g_ram + 0x77fc, 4); memcpy(&b2, g_ram + 0x77f8, 4);
      memcpy(&d, g_ram + 0x77f4, 4);
      if (a != 0x7000 || b2 != 0xAAAA0001u || d != 0x77fc) {
          printf("    *** FAIL: display = %08x %08x %08x, want 00007000 aaaa0001 000077fc\n",
                 a, b2, d);
          g_fail = 1;
      } }
    x86_cpu_end(s);
}

static void test_enter_pf_restart(void)
{
    /* `enter n, level` must not mutate EBP/ESP while it walks the display: the
     * copy reads can #PF (a demand-paged outer frame), and the restarted
     * instruction would then copy from the wrong frame onto a stack that had
     * already been pushed once.  Page 6 (the display source) starts not
     * present; the handler maps it and the instruction restarts. */
    static const uint8_t prog[] = {
        0xBB,0x00,0x00,0x10,0x00,       /* mov ebx,0x100000            */
        0x0F,0x22,0xDB,                 /* mov cr3,ebx                 */
        0xBA,0x01,0x00,0x01,0x80,       /* mov edx,0x80010001 PG|WP|PE */
        0x0F,0x22,0xC2,                 /* mov cr0,edx                 */
        0xBC,0x00,0x80,0x00,0x00,       /* mov esp,0x8000              */
        0xBD,0x00,0x70,0x00,0x00,       /* mov ebp,0x7000              */
        0xC8,0x00,0x00,0x03,            /* enter 0,3                   */
        0xF4                            /* hlt                         */
    };
    /* Replaces the shared handler: map page 6 present+writable, then IRET. */
    static const uint8_t handler[] = {
        0xC7,0x05,0x18,0x10,0x10,0x00, 0x03,0x60,0x00,0x00, /* [PTE 6]=0x6003 */
        0x58,                           /* pop eax (error code)        */
        0xCF                            /* iret                        */
    };
    PhysMemoryMap *map = mk_map(0x200000);
    X86CPUState *s = mk_wpfault_cpu(map, prog, sizeof(prog));
    uint32_t v1 = 0x11110001, v2 = 0x22220002;
    memcpy(g_ram + 0x100, handler, sizeof(handler));
    memcpy(g_ram + 0x6ffc, &v1, 4);     /* [EBP-4] */
    memcpy(g_ram + 0x6ff8, &v2, 4);     /* [EBP-8] */
    x86_cpu_interp(s, 100000);
    printf("  enter restart: #PF walking the display must not half-apply\n");
    CHECK(s, 5 /*EBP*/, 0x7ffc);
    CHECK(s, 4 /*ESP*/, 0x7ff0);
    { uint32_t a, b2, d, e;
      memcpy(&a,  g_ram + 0x7ffc, 4); memcpy(&b2, g_ram + 0x7ff8, 4);
      memcpy(&d,  g_ram + 0x7ff4, 4); memcpy(&e,  g_ram + 0x7ff0, 4);
      if (a != 0x7000 || b2 != v1 || d != v2 || e != 0x7ffc) {
          printf("    *** FAIL: frame = %08x %08x %08x %08x,"
                 " want 00007000 11110001 22220002 00007ffc\n", a, b2, d, e);
          g_fail = 1;
      } }
    x86_cpu_end(s);
}

/* ----------------------------------------------------------------------- */
/* Software-TLB invalidation and tagging.
 *
 * The interpreter caches linear->host translations, so a page the guest
 * remaps must not keep resolving to the old frame, and a translation cached
 * for the supervisor must not be reachable from CPL 3. Each of these would be
 * a silent memory corruption / privilege escape rather than a visible fault.
 *
 * Common map: PD @0xA000, identity PT @0xB000 (VA 0..64K), PT2 @0xC000 mapping
 * VA 0x40000000 to phys 0x6000. */
static void tlb_setup_pt(uint32_t identity_leaf_flags)
{
    uint32_t *pd  = (uint32_t *)(g_ram + 0xA000);
    uint32_t *pt  = (uint32_t *)(g_ram + 0xB000);
    uint32_t *pt2 = (uint32_t *)(g_ram + 0xC000);
    int i;

    memset(pd, 0, 0x1000);
    memset(pt, 0, 0x1000);
    memset(pt2, 0, 0x1000);
    pd[0]   = 0xB000 | 0x7;         /* VA 0..4MB     -> identity PT */
    pd[256] = 0xC000 | 0x7;         /* VA 0x40000000 -> PT2         */
    for (i = 0; i < 16; i++)
        pt[i] = (uint32_t)(i << 12) | identity_leaf_flags;
    pt2[0] = 0x6000 | 0x3;          /* VA 0x40000000 -> phys 0x6000 */
}

/* mov ebx,0xA000 / mov cr3,ebx / mov edx,0x80000001 / mov cr0,edx */
#define TLB_ENABLE_PAGING \
    0xBB,0x00,0xA0,0x00,0x00,  0x0F,0x22,0xDB, \
    0xBA,0x01,0x00,0x00,0x80,  0x0F,0x22,0xC2

/* Store through VA 0x40000000 (filling the write TLB), repoint PT2[0] at phys
 * 0x7000 through the identity map, invalidate, then store and load again. With
 * a stale entry the second store goes back to phys 0x6000 and the load returns
 * the old value. `flush` is the invalidating instruction under test. */
static void tlb_remap_case(const char *name, const uint8_t *flush, size_t nflush)
{
    static const uint8_t head[] = {
        TLB_ENABLE_PAGING,
        /* mov dword [0x40000000],0xAAAAAAAA */
        0xC7,0x05,0x00,0x00,0x00,0x40, 0xAA,0xAA,0xAA,0xAA,
        /* mov dword [0xC000],0x7003  -- PT2[0] now points at phys 0x7000 */
        0xC7,0x05,0x00,0xC0,0x00,0x00, 0x03,0x70,0x00,0x00,
    };
    static const uint8_t tail[] = {
        /* mov dword [0x40000000],0xBBBBBBBB */
        0xC7,0x05,0x00,0x00,0x00,0x40, 0xBB,0xBB,0xBB,0xBB,
        0xA1,0x00,0x00,0x00,0x40,       /* mov eax,[0x40000000] */
        0xF4                            /* hlt                  */
    };
    uint8_t prog[128];
    size_t n = 0;
    PhysMemoryMap *map;
    X86CPUState *s;
    uint32_t a, b;

    memcpy(prog + n, head, sizeof(head));   n += sizeof(head);
    memcpy(prog + n, flush, nflush);        n += nflush;
    memcpy(prog + n, tail, sizeof(tail));   n += sizeof(tail);

    map = mk_map(0x10000);
    s = mk_cpu(map, 0x9000);
    tlb_setup_pt(0x3);
    memcpy(g_ram, prog, n);
    x86_cpu_interp(s, 2000);

    memcpy(&a, g_ram + 0x6000, 4);
    memcpy(&b, g_ram + 0x7000, 4);
    printf("  tlb %s: a remapped page must drop its cached translation\n", name);
    if (a != 0xAAAAAAAAu || b != 0xBBBBBBBBu) {
        printf("    *** FAIL: phys 0x6000=0x%08x (want 0xAAAAAAAA), "
               "phys 0x7000=0x%08x (want 0xBBBBBBBB)\n", a, b);
        g_fail = 1;
    }
    CHECK(s, 0 /*EAX*/, 0xBBBBBBBBu);   /* the read TLB must be dropped too */
    x86_cpu_end(s);
}

static void test_tlb_invlpg(void)
{
    /* invlpg [0x40000000] -- 0F 01 /7, mod=00 rm=101 (disp32). */
    static const uint8_t flush[] = { 0x0F,0x01,0x3D,0x00,0x00,0x00,0x40 };
    tlb_remap_case("invlpg", flush, sizeof(flush));
}

static void test_tlb_cr3_reload(void)
{
    /* A CR3 write is the architectural "I changed the page tables" signal and
     * must flush even when the value is unchanged (EBX still holds 0xA000). */
    static const uint8_t flush[] = { 0x0F,0x22,0xDB };   /* mov cr3,ebx */
    tlb_remap_case("cr3 reload", flush, sizeof(flush));
}

static void test_tlb_cpl(void)
{
    /* A supervisor-only page (U/S=0) touched at CPL 0 caches a translation.
     * The same linear address read from CPL 3 must still #PF: TLB entries are
     * tagged by MMU index, not shared across privilege levels. Reusing the
     * ring-0 entry here would be a straight privilege escape. */
    static const uint8_t ring0[] = {
        TLB_ENABLE_PAGING,
        0x8B,0x0D,0x00,0x60,0x00,0x00,  /* mov ecx,[0x6000] -- fills read TLB  */
        0x89,0x0D,0x00,0x60,0x00,0x00,  /* mov [0x6000],ecx -- fills write TLB */
        /* fall through into the IRET-to-ring-3 trampoline appended below */
    };
    static const uint8_t pf_handler[] = {
        0xB8,0xF0,0x00,0x00,0x00,       /* mov eax,0xF0 */
        0xF4                            /* hlt          */
    };
    static const uint8_t ring3[] = {
        0x8B,0x1D,0x00,0x60,0x00,0x00,  /* mov ebx,[0x6000] -- must #PF */
        0xB8,0x11,0x00,0x00,0x00,       /* mov eax,0x11 (reached only on escape) */
        0xF4                            /* hlt */
    };
    PhysMemoryMap *map = mk_map(0x10000);
    X86CPUState *s;
    uint32_t seed = 0x5A5A5A5Au;

    s = mk_prot_cpu(map);
    memcpy(g_ram + 0x000, ring0, sizeof(ring0));
    memcpy(g_ram + 0x000 + sizeof(ring0), to_ring3, sizeof(to_ring3));
    memcpy(g_ram + 0x300, pf_handler, sizeof(pf_handler));
    memcpy(g_ram + 0x500, ring3, sizeof(ring3));
    memcpy(g_ram + 0x6000, &seed, 4);
    set_gate(14, 0x300, 0);             /* #PF */

    /* Identity pages are user-accessible so ring-3 code/stack work, except the
     * page under test, which stays supervisor-only. */
    tlb_setup_pt(0x7);
    ((uint32_t *)(g_ram + 0xB000))[6] = 0x6000 | 0x3;   /* page 6: P|RW, U=0 */

    x86_cpu_interp(s, 3000);

    printf("  tlb cpl: a supervisor translation must not be reusable at CPL3\n");
    CHECK(s, 1 /*ECX*/, 0x5A5A5A5Au);   /* the ring-0 access did happen */
    CHECK(s, 0 /*EAX*/, 0xF0);          /* and ring 3 faulted rather than read it */
    x86_cpu_end(s);
}

/* ----------------------------------------------------------------------- */
/* `rep movs`/`rep stos` bulk path.
 *
 * string_bulk() moves whole runs with memcpy/memset once the TLB holds the
 * pages. These cases pin the three ways that shortcut differs from the
 * element-at-a-time semantics it replaces: overlapping operands, runs that
 * span a page boundary onto a different frame, and runs long enough to hit the
 * interruptibility budget and restart. */

static void test_rep_overlap(void)
{
    /* `rep movsb` with EDI = ESI+1 propagates one byte across the range -- the
     * classic fill idiom. memcpy() over overlapping ranges would leave the tail
     * zeroed, so the bulk path must detect the overlap and stay scalar. */
    static const uint8_t p[] = {
        0xBE,0x00,0x30,0x00,0x00,   /* mov esi,0x3000 */
        0xBF,0x01,0x30,0x00,0x00,   /* mov edi,0x3001 */
        0xB9,0x08,0x00,0x00,0x00,   /* mov ecx,8      */
        0xFC,                       /* cld            */
        0xF3,0xA4,                  /* rep movsb      */
        0xF4
    };
    PhysMemoryMap *map = mk_map(0x10000);
    X86CPUState *s = mk_cpu(map, 0x9000);
    int i, bad = -1;

    memcpy(g_ram, p, sizeof(p));
    memset(g_ram + 0x3000, 0, 16);
    g_ram[0x3000] = 0xAB;
    x86_cpu_interp(s, 2000);

    for (i = 0; i <= 8; i++)
        if (g_ram[0x3000 + i] != 0xAB) { bad = i; break; }
    printf("  rep overlap: movsb with EDI=ESI+1 propagates byte-by-byte\n");
    if (bad >= 0) {
        printf("    *** FAIL: byte %d at 0x%04x = 0x%02x, want 0xAB\n",
               bad, 0x3000 + bad, g_ram[0x3000 + bad]);
        g_fail = 1;
    }
    if (g_ram[0x3009] != 0) {
        printf("    *** FAIL: wrote past the requested count\n");
        g_fail = 1;
    }
    x86_cpu_end(s);
}

static void test_rep_page_cross(void)
{
    /* `rep movsd` whose destination run crosses a page boundary onto an
     * unrelated frame: VA 0x40000000 -> phys 0x6000, VA 0x40001000 -> phys
     * 0x8000. A bulk copy that ignored the page edge would run off 0x6000 into
     * 0x7000 and corrupt the wrong frame. */
    static const uint8_t p[] = {
        TLB_ENABLE_PAGING,
        0xBE,0x00,0x30,0x00,0x00,   /* mov esi,0x3000     */
        0xBF,0xF0,0x0F,0x00,0x40,   /* mov edi,0x40000ff0 */
        0xB9,0x08,0x00,0x00,0x00,   /* mov ecx,8          */
        0xFC,                       /* cld                */
        0xF3,0xA5,                  /* rep movsd          */
        0xF4
    };
    PhysMemoryMap *map = mk_map(0x10000);
    X86CPUState *s = mk_cpu(map, 0x9000);
    uint32_t src[8], got;
    int i;

    memcpy(g_ram, p, sizeof(p));
    tlb_setup_pt(0x3);
    ((uint32_t *)(g_ram + 0xC000))[1] = 0x8000 | 0x3;   /* VA 0x40001000 -> phys 0x8000 */
    for (i = 0; i < 8; i++) src[i] = 0xA0000000u + (uint32_t)i;
    memcpy(g_ram + 0x3000, src, sizeof(src));
    memset(g_ram + 0x7000, 0, 0x20);
    x86_cpu_interp(s, 3000);

    printf("  rep page cross: movsd run split at the page edge, per frame\n");
    for (i = 0; i < 4; i++) {           /* first four land at phys 0x6ff0 */
        memcpy(&got, g_ram + 0x6ff0 + i * 4, 4);
        if (got != src[i]) {
            printf("    *** FAIL: phys 0x%04x = 0x%08x, want 0x%08x\n",
                   0x6ff0 + i * 4, got, src[i]);
            g_fail = 1;
        }
    }
    for (i = 4; i < 8; i++) {           /* rest continue at phys 0x8000 */
        memcpy(&got, g_ram + 0x8000 + (i - 4) * 4, 4);
        if (got != src[i]) {
            printf("    *** FAIL: phys 0x%04x = 0x%08x, want 0x%08x\n",
                   0x8000 + (i - 4) * 4, got, src[i]);
            g_fail = 1;
        }
    }
    memcpy(&got, g_ram + 0x7000, 4);    /* the frame that is *not* mapped here */
    if (got != 0) {
        printf("    *** FAIL: ran past the page edge into phys 0x7000 (0x%08x)\n", got);
        g_fail = 1;
    }
    x86_cpu_end(s);
}

static void test_rep_stos_pages(void)
{
    /* `rep stosd` of 1200 dwords from VA 0x40000000: 1024 fill phys 0x6000 and
     * the remaining 176 must continue at phys 0x8000, not run off the frame. */
    static const uint8_t p[] = {
        TLB_ENABLE_PAGING,
        0xB8,0xEF,0xBE,0xAD,0xDE,   /* mov eax,0xDEADBEEF */
        0xBF,0x00,0x00,0x00,0x40,   /* mov edi,0x40000000 */
        0xB9,0xB0,0x04,0x00,0x00,   /* mov ecx,1200       */
        0xFC,                       /* cld                */
        0xF3,0xAB,                  /* rep stosd          */
        0xF4
    };
    PhysMemoryMap *map = mk_map(0x10000);
    X86CPUState *s = mk_cpu(map, 0x9000);
    uint32_t got;

    memcpy(g_ram, p, sizeof(p));
    tlb_setup_pt(0x3);
    ((uint32_t *)(g_ram + 0xC000))[1] = 0x8000 | 0x3;
    memset(g_ram + 0x7000, 0, 0x100);
    x86_cpu_interp(s, 3000);

    printf("  rep stos pages: 1200 dwords spill onto the next mapped frame\n");
    memcpy(&got, g_ram + 0x6000, 4);
    if (got != 0xDEADBEEFu) { printf("    *** FAIL: first frame not filled\n"); g_fail = 1; }
    memcpy(&got, g_ram + 0x6ffc, 4);
    if (got != 0xDEADBEEFu) { printf("    *** FAIL: first frame not filled to its end\n"); g_fail = 1; }
    memcpy(&got, g_ram + 0x8000 + 175 * 4, 4);
    if (got != 0xDEADBEEFu) { printf("    *** FAIL: spill onto phys 0x8000 short\n"); g_fail = 1; }
    memcpy(&got, g_ram + 0x8000 + 176 * 4, 4);
    if (got != 0) { printf("    *** FAIL: wrote past ECX elements\n"); g_fail = 1; }
    memcpy(&got, g_ram + 0x7000, 4);
    if (got != 0) { printf("    *** FAIL: ran into the unmapped frame 0x7000\n"); g_fail = 1; }
    CHECK(s, 1 /*ECX*/, 0);
    x86_cpu_end(s);
}

static void test_rep_budget_restart(void)
{
    /* A `rep` longer than the interruptibility budget (65536 elements) returns
     * to the interpreter loop and restarts. ECX/EDI carry the progress, so the
     * observable result must be identical to running it in one go. */
    static const uint8_t p[] = {
        0xB0,0x5A,                  /* mov al,0x5A     */
        0xBF,0x00,0x00,0x01,0x00,   /* mov edi,0x10000 */
        0xB9,0xA0,0x86,0x01,0x00,   /* mov ecx,100000  */
        0xFC,                       /* cld             */
        0xF3,0xAA,                  /* rep stosb       */
        0xF4
    };
    PhysMemoryMap *map = mk_map(0x40000);
    X86CPUState *s = mk_cpu(map, 0x9000);

    memcpy(g_ram, p, sizeof(p));
    x86_cpu_interp(s, 5000);

    printf("  rep budget: a 100000-element rep restarts and still completes\n");
    if (g_ram[0x10000] != 0x5A || g_ram[0x10000 + 99999] != 0x5A) {
        printf("    *** FAIL: fill incomplete (first=0x%02x last=0x%02x)\n",
               g_ram[0x10000], g_ram[0x10000 + 99999]);
        g_fail = 1;
    }
    if (g_ram[0x10000 + 100000] != 0) {
        printf("    *** FAIL: wrote one element too many\n");
        g_fail = 1;
    }
    CHECK(s, 1 /*ECX*/, 0);
    CHECK(s, 7 /*EDI*/, 0x10000 + 100000);
    x86_cpu_end(s);
}

static void test_rep_backward(void)
{
    /* DF=1 must stay on the scalar path: the bulk copy only handles the
     * forward direction. */
    static const uint8_t p[] = {
        0xBE,0x0F,0x30,0x00,0x00,   /* mov esi,0x300f */
        0xBF,0x0F,0x40,0x00,0x00,   /* mov edi,0x400f */
        0xB9,0x10,0x00,0x00,0x00,   /* mov ecx,16     */
        0xFD,                       /* std            */
        0xF3,0xA4,                  /* rep movsb      */
        0xFC,                       /* cld            */
        0xF4
    };
    PhysMemoryMap *map = mk_map(0x10000);
    X86CPUState *s = mk_cpu(map, 0x9000);
    int i;

    memcpy(g_ram, p, sizeof(p));
    for (i = 0; i < 16; i++) g_ram[0x3000 + i] = (uint8_t)(0x40 + i);
    memset(g_ram + 0x4000, 0, 16);
    x86_cpu_interp(s, 2000);

    printf("  rep backward: DF=1 movsb stays element-at-a-time\n");
    if (memcmp(g_ram + 0x3000, g_ram + 0x4000, 16) != 0) {
        printf("    *** FAIL: backward copy did not reproduce the source\n");
        g_fail = 1;
    }
    x86_cpu_end(s);
}

/* ----------------------------------------------------------------------- *
 * Deferred ("lazy") arithmetic flags.
 *
 * alu() no longer produces flag bits; it records the operands and the six
 * flags are reconstructed on demand (see the comment in x86_cpu_priv.h). That
 * makes flags a *stateful* property, with two failure modes the older,
 * eager-flag tests above cannot see:
 *
 *   1. a derivation that disagrees with the architectural definition, and
 *   2. an eager producer (shift, BCD, sahf, stc, ...) whose bits are later
 *      overwritten by a record that was still pending when it ran.
 *
 * (1) is covered differentially: ref_alu_flags() below is a second,
 * independent implementation written straight from the SDM definitions, and
 * every op x size x operand-pair x carry-in combination is compared against
 * what the interpreter actually leaves in EFLAGS. (2) gets explicit ordering
 * cases at the end.
 * ----------------------------------------------------------------------- */

#define F_CF 0x001u
#define F_PF 0x004u
#define F_AF 0x010u
#define F_ZF 0x040u
#define F_SF 0x080u
#define F_OF 0x800u
#define F_ARITH (F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF)

/* Run a hand-assembled program and return the EAX it halts with. */
static uint32_t run_prog(const uint8_t *code, size_t n)
{
    PhysMemoryMap *map = mk_map(0x10000);
    X86CPUState *s = mk_cpu(map, 0x8000);
    uint32_t r;
    memcpy(g_ram, code, n);
    x86_cpu_interp(s, 1000);
    r = x86_cpu_get_reg(s, 0);
    x86_cpu_end(s);
    return r;
}

static int ref_parity(uint8_t v)
{
    int i, n = 0;
    for (i = 0; i < 8; i++) n += (v >> i) & 1;
    return !(n & 1);
}

/* Independent reference: ALU ops 0..7 = add,or,adc,sbb,and,sub,xor,cmp.
 * AF after a logic op is architecturally undefined; this core defines it as
 * cleared, and the reference pins that choice. */
static uint32_t ref_alu_flags(int op, uint32_t a, uint32_t b, int size, int cin)
{
    uint64_t mask = (size == 1) ? 0xffu : (size == 2) ? 0xffffu : 0xffffffffu;
    uint32_t sbit = (size == 1) ? 0x80u : (size == 2) ? 0x8000u : 0x80000000u;
    uint32_t f = 0, r;
    int is_logic = (op == 1 || op == 4 || op == 6);
    int is_sub = (op == 3 || op == 5 || op == 7);

    a &= (uint32_t)mask;
    b &= (uint32_t)mask;
    if (op != 2 && op != 3) cin = 0;

    switch (op) {
    case 0: case 2: r = (uint32_t)(((uint64_t)a + b + cin) & mask); break;
    case 3: case 5: case 7: r = (uint32_t)(((uint64_t)a - b - cin) & mask); break;
    case 1: r = a | b; break;
    case 4: r = a & b; break;
    default: r = a ^ b; break;
    }

    if (!is_logic) {
        if (is_sub) {
            if ((uint64_t)a < (uint64_t)b + cin) f |= F_CF;
            if ((a ^ b) & (a ^ r) & sbit) f |= F_OF;
        } else {
            if ((uint64_t)a + b + cin > mask) f |= F_CF;
            if ((~(a ^ b)) & (a ^ r) & sbit) f |= F_OF;
        }
        if ((a ^ b ^ r) & 0x10) f |= F_AF;
    }
    if (r == 0) f |= F_ZF;
    if (r & sbit) f |= F_SF;
    if (ref_parity((uint8_t)r)) f |= F_PF;
    return f;
}

static void test_lazy_flags(void)
{
    /* Corner cases: zero, one, the signed edges, all-ones, nibble carries and
     * a value that makes parity flip. */
    static const uint32_t vals[] = {
        0, 1, 2, 0x0f, 0x7f, 0x80, 0xff, 0x7fff, 0x8000,
        0x7fffffffu, 0x80000000u, 0xffffffffu, 0x12345678u
    };
    static const int sizes[] = { 1, 2, 4 };
    int op, si, ai, bi, cin, checked = 0;

    for (op = 0; op < 8; op++) {
        for (si = 0; si < 3; si++) {
            int size = sizes[si];
            for (ai = 0; ai < (int)countof(vals); ai++) {
                for (bi = 0; bi < (int)countof(vals); bi++) {
                    for (cin = 0; cin < 2; cin++) {
                        uint32_t a = vals[ai], b = vals[bi];
                        uint8_t prog[32];
                        size_t k = 0;
                        uint32_t got, want;

                        /* Only adc/sbb consume the incoming carry. */
                        if (cin && op != 2 && op != 3)
                            continue;

                        if (size == 1) {
                            prog[k++] = 0xB0; prog[k++] = (uint8_t)a;   /* mov al,a  */
                            prog[k++] = 0xB3; prog[k++] = (uint8_t)b;   /* mov bl,b  */
                        } else if (size == 2) {
                            prog[k++] = 0x66; prog[k++] = 0xB8;         /* mov ax,a  */
                            prog[k++] = (uint8_t)a; prog[k++] = (uint8_t)(a >> 8);
                            prog[k++] = 0x66; prog[k++] = 0xBB;         /* mov bx,b  */
                            prog[k++] = (uint8_t)b; prog[k++] = (uint8_t)(b >> 8);
                        } else {
                            prog[k++] = 0xB8;                           /* mov eax,a */
                            memcpy(prog + k, &a, 4); k += 4;
                            prog[k++] = 0xBB;                           /* mov ebx,b */
                            memcpy(prog + k, &b, 4); k += 4;
                        }
                        prog[k++] = cin ? 0xF9 : 0xF8;    /* stc / clc */
                        /* <op> r/m,r with modrm 0xD8 = "op (e)a(x), (e)b(x)" */
                        if (size == 2) prog[k++] = 0x66;
                        prog[k++] = (uint8_t)(op * 8 + (size == 1 ? 0 : 1));
                        prog[k++] = 0xD8;
                        prog[k++] = 0x9C;                 /* pushfd    */
                        prog[k++] = 0x58;                 /* pop eax   */
                        prog[k++] = 0xF4;                 /* hlt       */

                        got  = run_prog(prog, k) & F_ARITH;
                        want = ref_alu_flags(op, a, b, size, cin);
                        checked++;
                        if (got != want) {
                            printf("    *** FAIL: op%d size%d a=0x%x b=0x%x cin=%d: "
                                   "flags 0x%03x want 0x%03x\n",
                                   op, size, a, b, cin, got, want);
                            g_fail = 1;
                            return;   /* one report is enough to debug from */
                        }
                    }
                }
            }
        }
    }
    printf("  lazy flags: %d alu op/size/operand/carry combinations vs reference\n",
           checked);
}

static void test_lazy_flags_ordering(void)
{
    /* inc/dec must leave CF alone while still deriving the other five from
     * the result. Both carry-in polarities, so a hardwired 0 or 1 fails. */
    static const uint8_t inc_cf1[] = {
        0xF9,                         /* stc              */
        0xB8,0xFF,0xFF,0xFF,0xFF,     /* mov eax,-1       */
        0x40,                         /* inc eax  -> 0    */
        0x9C, 0x58, 0xF4              /* pushfd; pop eax  */
    };
    static const uint8_t inc_cf0[] = {
        0xF8,                         /* clc              */
        0xB8,0xFF,0xFF,0xFF,0xFF,
        0x40,
        0x9C, 0x58, 0xF4
    };
    /* An eager producer running with a record still pending: the `cmp` leaves
     * ZF=1 deferred, then `shl ebx,1` must win with ZF=0. Without the sync in
     * do_shift() the stale record would resurrect ZF=1. */
    static const uint8_t shift_after_cmp[] = {
        0xB8,0x01,0x00,0x00,0x00,     /* mov eax,1        */
        0xBB,0x01,0x00,0x00,0x00,     /* mov ebx,1        */
        0x39,0xD8,                    /* cmp eax,ebx (ZF=1 pending) */
        0xD1,0xE3,                    /* shl ebx,1 -> 2, ZF=0       */
        0x9C, 0x58, 0xF4
    };
    /* sahf rewrites only the low byte, so OF (bit 11) must survive the
     * materialization of the pending `add` that set it. */
    static const uint8_t sahf_keeps_of[] = {
        0xB8,0xFF,0xFF,0xFF,0x7F,     /* mov eax,0x7fffffff */
        0x83,0xC0,0x01,               /* add eax,1  -> OF=1 */
        0xB4,0x00,                    /* mov ah,0           */
        0x9E,                         /* sahf               */
        0x9C, 0x58, 0xF4
    };
    /* stc touches CF only; the other five must come from the pending sub. */
    static const uint8_t stc_keeps_zf[] = {
        0xB8,0x05,0x00,0x00,0x00,     /* mov eax,5          */
        0x83,0xE8,0x05,               /* sub eax,5 -> ZF=1 pending */
        0xF9,                         /* stc                */
        0x9C, 0x58, 0xF4
    };
    static const struct {
        const uint8_t *p; size_t n; uint32_t mask, want; const char *what;
    } c[] = {
        { inc_cf1, sizeof(inc_cf1), F_CF | F_ZF, F_CF | F_ZF, "inc preserves CF=1" },
        { inc_cf0, sizeof(inc_cf0), F_CF | F_ZF, F_ZF,        "inc preserves CF=0" },
        { shift_after_cmp, sizeof(shift_after_cmp), F_ZF, 0,  "shl overrides a pending cmp" },
        { sahf_keeps_of, sizeof(sahf_keeps_of), F_OF, F_OF,   "sahf preserves OF" },
        { stc_keeps_zf, sizeof(stc_keeps_zf), F_CF | F_ZF, F_CF | F_ZF, "stc preserves ZF" },
    };
    size_t i;

    printf("  lazy flags ordering: eager producers vs a pending record\n");
    for (i = 0; i < countof(c); i++) {
        uint32_t got = run_prog(c[i].p, c[i].n) & c[i].mask;
        if (got != c[i].want) {
            printf("    *** FAIL: %s: flags 0x%03x want 0x%03x\n",
                   c[i].what, got, c[i].want);
            g_fail = 1;
        }
    }
}

/* Run `prog` at 0x0000 in protected mode with a #DE (vector 0) gate that jumps
 * to a stub at 0x0200 setting EBX=0xDEAD. Returns the final CPU so the caller can
 * tell whether #DE was delivered (EBX==0xDEAD) or the idiv fell through
 * (EBX==0x2222) — and, crucially, that a MIN/-1 idiv does not crash the host with
 * SIGFPE. */
static X86CPUState *run_de(const uint8_t *prog, size_t n)
{
    static const uint8_t de_handler[] = {
        0xBB,0xAD,0xDE,0x00,0x00,   /* mov ebx,0xDEAD */
        0xF4                        /* hlt            */
    };
    PhysMemoryMap *map = mk_map(0x10000);
    X86CPUState *s = x86_cpu_init(map);
    X86CPUSeg gdt = { .base = 0x1000, .limit = 0x17 };
    X86CPUSeg idt = { .base = 0x1800, .limit = 0x7ff };
    X86CPUSeg cs  = { .sel = 0x08, .flags = 0xc09b, .base = 0, .limit = 0xffffffff };
    X86CPUSeg ds  = { .sel = 0x10, .flags = 0xc093, .base = 0, .limit = 0xffffffff };
    memcpy(g_ram, prog, n);
    memcpy(g_ram + 0x200, de_handler, sizeof(de_handler));
    put64(g_ram + 0x1000 + 0x08, 0x00CF9B000000FFFFull); /* code32 flat */
    put64(g_ram + 0x1000 + 0x10, 0x00CF93000000FFFFull); /* data32 flat */
    put64(g_ram + 0x1800 + 0*8,  0x00008E0000080200ull); /* #DE -> 0x200 */
    x86_cpu_set_seg(s, X86_CPU_SEG_GDT, &gdt);
    x86_cpu_set_seg(s, X86_CPU_SEG_IDT, &idt);
    x86_cpu_set_seg(s, X86_CPU_SEG_CS, &cs);
    x86_cpu_set_seg(s, X86_CPU_SEG_DS, &ds);
    x86_cpu_set_seg(s, X86_CPU_SEG_ES, &ds);
    x86_cpu_set_seg(s, X86_CPU_SEG_SS, &ds);
    x86_cpu_set_reg(s, X86_CPU_REG_CR0, 0x1); /* PE */
    x86_cpu_set_reg(s, X86_CPU_REG_EIP, 0);
    x86_cpu_set_reg(s, 4 /*ESP*/, 0x8000);
    x86_cpu_interp(s, 1000);
    return s;
}

static void test_idiv_overflow(void)
{
    /* 32-bit: EDX:EAX = 0x8000000000000000 (INT64_MIN) / -1. The raw C division
     * is UB (host SIGFPE) and the quotient 2^31 does not fit EAX -> #DE. */
    static const uint8_t idiv32_min[] = {
        0xB8,0x00,0x00,0x00,0x00,   /* mov eax,0          */
        0xBA,0x00,0x00,0x00,0x80,   /* mov edx,0x80000000 */
        0xB9,0xFF,0xFF,0xFF,0xFF,   /* mov ecx,-1         */
        0xF7,0xF9,                  /* idiv ecx           */
        0xBB,0x22,0x22,0x00,0x00,   /* mov ebx,0x2222     */
        0xF4                        /* hlt                */
    };
    /* 16-bit: DX:AX = 0x80000000 (INT32_MIN) / -1 -> #DE. */
    static const uint8_t idiv16_min[] = {
        0xB8,0x00,0x00,0x00,0x00,   /* mov eax,0      (ax=0)      */
        0xBA,0x00,0x80,0x00,0x00,   /* mov edx,0x8000 (dx=0x8000) */
        0xB9,0xFF,0xFF,0x00,0x00,   /* mov ecx,0xffff (cx=-1)     */
        0x66,0xF7,0xF9,             /* idiv cx        */
        0xBB,0x22,0x22,0x00,0x00,   /* mov ebx,0x2222 */
        0xF4                        /* hlt            */
    };
    /* Sanity: -100 / 7 must NOT fault -> EAX=-14, EDX=-2, EBX reaches 0x2222. */
    static const uint8_t idiv32_ok[] = {
        0xB8,0x9C,0xFF,0xFF,0xFF,   /* mov eax,-100 */
        0xBA,0xFF,0xFF,0xFF,0xFF,   /* mov edx,-1   */
        0xB9,0x07,0x00,0x00,0x00,   /* mov ecx,7    */
        0xF7,0xF9,                  /* idiv ecx     */
        0xBB,0x22,0x22,0x00,0x00,   /* mov ebx,0x2222 */
        0xF4                        /* hlt          */
    };
    X86CPUState *s;

    printf("  idiv overflow: MIN/-1 raises #DE instead of crashing the host\n");
    s = run_de(idiv32_min, sizeof(idiv32_min));
    CHECK(s, 3, 0xDEAD); /* ebx: #DE delivered, not the fall-through mov */
    x86_cpu_end(s);
    s = run_de(idiv16_min, sizeof(idiv16_min));
    CHECK(s, 3, 0xDEAD);
    x86_cpu_end(s);
    s = run_de(idiv32_ok, sizeof(idiv32_ok));
    CHECK(s, 3, 0x2222);       /* no fault */
    CHECK(s, 0, 0xFFFFFFF2);   /* eax = -14 */
    CHECK(s, 2, 0xFFFFFFFE);   /* edx = -2  */
    x86_cpu_end(s);
}

static void test_rol_ror_cf(void)
{
    /* rol/ror by a nonzero multiple of the operand width leaves the value
     * unchanged, but CF is still defined (= LSB for rol, MSB for ror) because
     * the 5-bit count is nonzero. The old code early-returned and left CF stale.
     * Each program ends pushf/pop eax/hlt so run_prog returns EFLAGS. */
    static const uint8_t rol8_cf1[]  = { 0xB0,0x81, 0xC0,0xC0,0x08, 0x9C,0x58,0xF4 }; /* rol al,8  LSB=1 */
    static const uint8_t rol8_cf0[]  = { 0xB0,0x80, 0xC0,0xC0,0x08, 0x9C,0x58,0xF4 }; /* rol al,8  LSB=0 */
    static const uint8_t ror8_cf1[]  = { 0xB0,0x81, 0xC0,0xC8,0x08, 0x9C,0x58,0xF4 }; /* ror al,8  MSB=1 */
    static const uint8_t ror8_cf0[]  = { 0xB0,0x7F, 0xC0,0xC8,0x08, 0x9C,0x58,0xF4 }; /* ror al,8  MSB=0 */
    static const uint8_t rol16_cf1[] = { 0x66,0xB8,0x01,0x00, 0x66,0xC1,0xC0,0x10, 0x9C,0x58,0xF4 }; /* rol ax,16 */
    /* value untouched: rol al,8 must not perturb the rest of EAX. */
    static const uint8_t val_ok[]    = { 0xB8,0x78,0x56,0x34,0x12, 0xC0,0xC0,0x08, 0xF4 };
    static const struct {
        const uint8_t *p; size_t n; uint32_t mask, want; const char *what;
    } c[] = {
        { rol8_cf1,  sizeof(rol8_cf1),  F_CF, F_CF, "rol al,8 -> CF=LSB(1)"  },
        { rol8_cf0,  sizeof(rol8_cf0),  F_CF, 0,    "rol al,8 -> CF=LSB(0)"  },
        { ror8_cf1,  sizeof(ror8_cf1),  F_CF, F_CF, "ror al,8 -> CF=MSB(1)"  },
        { ror8_cf0,  sizeof(ror8_cf0),  F_CF, 0,    "ror al,8 -> CF=MSB(0)"  },
        { rol16_cf1, sizeof(rol16_cf1), F_CF, F_CF, "rol ax,16 -> CF=LSB(1)" },
    };
    size_t i;
    uint32_t v;

    printf("  rol/ror multiple-of-width: CF still updated, value unchanged\n");
    for (i = 0; i < countof(c); i++) {
        uint32_t got = run_prog(c[i].p, c[i].n) & c[i].mask;
        if (got != c[i].want) {
            printf("    *** FAIL: %s: flags 0x%x\n", c[i].what, got); g_fail = 1;
        }
    }
    v = run_prog(val_ok, sizeof(val_ok));
    if (v != 0x12345678u) {
        printf("    *** FAIL: rol al,8 changed the value: 0x%x\n", v); g_fail = 1;
    }
}

/* Run `verr`/`verw <sel>` at CPL0 through a GDT of assorted descriptor types and
 * return ZF. The 5.15 kernel issues `verw` for its MDS store-buffer flush; before
 * the fix these opcodes hit x86_stop and wedged the boot. */
static int run_verify(uint16_t sel, int is_verw)
{
    uint8_t prog[] = {
        0x66,0xB8,(uint8_t)sel,(uint8_t)(sel>>8),  /* mov ax, sel      */
        0x0F,0x00,(uint8_t)(is_verw?0xE8:0xE0),    /* verw/verr ax     */
        0x9C,0x58,0xF4                             /* pushf; pop eax; hlt */
    };
    PhysMemoryMap *map = mk_map(0x10000);
    X86CPUState *s = x86_cpu_init(map);
    X86CPUSeg gdt = { .base = 0x1000, .limit = 0xff };
    X86CPUSeg cs  = { .sel = 0x08, .flags = 0xc09b, .base = 0, .limit = 0xffffffff };
    X86CPUSeg ds  = { .sel = 0x10, .flags = 0xc093, .base = 0, .limit = 0xffffffff };
    uint32_t r;
    memcpy(g_ram, prog, sizeof(prog));
    put64(g_ram + 0x1000 + 0x08, 0x00CF9B000000FFFFull); /* readable code    */
    put64(g_ram + 0x1000 + 0x10, 0x00CF93000000FFFFull); /* writable data    */
    put64(g_ram + 0x1000 + 0x18, 0x00CF91000000FFFFull); /* read-only data   */
    put64(g_ram + 0x1000 + 0x20, 0x00CF99000000FFFFull); /* execute-only code*/
    put64(g_ram + 0x1000 + 0x28, 0x00CF89000000FFFFull); /* system (TSS)     */
    x86_cpu_set_seg(s, X86_CPU_SEG_GDT, &gdt);
    x86_cpu_set_seg(s, X86_CPU_SEG_CS, &cs);
    x86_cpu_set_seg(s, X86_CPU_SEG_DS, &ds);
    x86_cpu_set_seg(s, X86_CPU_SEG_ES, &ds);
    x86_cpu_set_seg(s, X86_CPU_SEG_SS, &ds);
    x86_cpu_set_reg(s, X86_CPU_REG_CR0, 0x1); /* PE */
    x86_cpu_set_reg(s, X86_CPU_REG_EIP, 0);
    x86_cpu_set_reg(s, 4 /*ESP*/, 0x8000);
    x86_cpu_interp(s, 100);
    r = x86_cpu_get_reg(s, 0) & F_ZF;
    x86_cpu_end(s);
    return r ? 1 : 0;
}

static void test_pae_ad(void)
{
    /* A PAE 3-level walk must set Accessed on the PDE and PTE, and Dirty on the
     * PTE after a write. Previously the PAE path set no A/D bits at all -- the
     * 5.15 guest boots on PAE, so this is exercised on every access there. */
    static const uint8_t p[] = {
        0xB8,0x20,0x00,0x00,0x00,       /* mov eax,0x20   (CR4.PAE)   */
        0x0F,0x22,0xE0,                 /* mov cr4,eax                */
        0xB8,0x00,0x10,0x00,0x00,       /* mov eax,0x1000 (PDPT base) */
        0x0F,0x22,0xD8,                 /* mov cr3,eax                */
        0xB8,0x01,0x00,0x00,0x80,       /* mov eax,0x80000001         */
        0x0F,0x22,0xC0,                 /* mov cr0,eax    (PG|PE)     */
        0x8B,0x1D,0x00,0x10,0x00,0x00,  /* mov ebx,[0x1000]  (read)   */
        0xC7,0x05,0x00,0x10,0x00,0x00,0x0D,0xD0,0xFE,0xCA, /* mov [0x1000],0xCAFED00D */
        0xF4                            /* hlt                        */
    };
    PhysMemoryMap *map = mk_map(0x10000);
    X86CPUState *s = mk_cpu(map, 0x8000);
    uint32_t pte1, pde0, data;
    memcpy(g_ram, p, sizeof(p));
    put64(g_ram + 0x1000, 0x2001);   /* PDPTE[0] -> PD @0x2000, present         */
    put64(g_ram + 0x2000, 0x3003);   /* PDE[0]   -> PT @0x3000, present|rw      */
    put64(g_ram + 0x3000, 0x0003);   /* PTE[0]   -> phys 0 (code), present|rw   */
    put64(g_ram + 0x3008, 0x4003);   /* PTE[1]   -> phys 0x4000 (data page)     */
    x86_cpu_interp(s, 1000);
    memcpy(&pte1, g_ram + 0x3008, 4);
    memcpy(&pde0, g_ram + 0x2000, 4);
    memcpy(&data, g_ram + 0x4000, 4);
    printf("  pae A/D: PTE=%08x PDE=%08x data=%08x\n", pte1, pde0, data);
    if ((pte1 & 0x60) != 0x60) { /* PG_A|PG_D */
        printf("    *** FAIL: PTE missing A|D (0x%08x)\n", pte1); g_fail = 1; }
    if (!(pde0 & 0x20)) {        /* PG_A */
        printf("    *** FAIL: PDE missing A (0x%08x)\n", pde0); g_fail = 1; }
    if (data != 0xCAFED00Du) {
        printf("    *** FAIL: paged write landed wrong (0x%08x)\n", data); g_fail = 1; }
    x86_cpu_end(s);
}

static void test_verrw(void)
{
    static const struct { uint16_t sel; int verw, want; const char *what; } c[] = {
        { 0x10, 1, 1, "verw writable data -> ZF=1"    },
        { 0x10, 0, 1, "verr writable data -> ZF=1"    },
        { 0x18, 1, 0, "verw read-only data -> ZF=0"   },
        { 0x18, 0, 1, "verr read-only data -> ZF=1"   },
        { 0x20, 0, 0, "verr execute-only code -> ZF=0"},
        { 0x08, 0, 1, "verr readable code -> ZF=1"    },
        { 0x08, 1, 0, "verw code -> ZF=0"             },
        { 0x28, 1, 0, "verw system segment -> ZF=0"   },
        { 0x0000, 0, 0, "verr null selector -> ZF=0"  },
    };
    size_t i;
    printf("  verr/verw: 0F 00 /4,/5 set ZF by segment accessibility\n");
    for (i = 0; i < countof(c); i++) {
        int got = run_verify(c[i].sel, c[i].verw);
        if (got != c[i].want) {
            printf("    *** FAIL: %s: got ZF=%d\n", c[i].what, got); g_fail = 1;
        }
    }
}

/* LFS/LGS (0F B4/B5) load a far pointer: the offset goes to a GP register and
 * the selector to the segment register. Verified end to end by reading through
 * the freshly-loaded segment base. LES/LDS/LSS share load_far_ptr(). */
static void test_far_ptr_load(void)
{
    static const uint8_t p[] = {
        0x0F,0xB4,0x1D,0x00,0x20,0x00,0x00,   /* lfs ebx, [0x2000]        */
        0x64,0x8B,0x05,0x00,0x00,0x00,0x00,   /* mov eax, fs:[0]          */
        0x0F,0xB5,0x0D,0x08,0x20,0x00,0x00,   /* lgs ecx, [0x2008]        */
        0x65,0x8B,0x15,0x00,0x00,0x00,0x00,   /* mov edx, gs:[0]          */
        0xF4
    };
    PhysMemoryMap *map = mk_map(0x10000);
    X86CPUState *s = x86_cpu_init(map);
    X86CPUSeg gdt = { .base = 0x1000, .limit = 0xff };
    X86CPUSeg cs  = { .sel = 0x08, .flags = 0xc09b, .base = 0, .limit = 0xffffffff };
    X86CPUSeg ds  = { .sel = 0x10, .flags = 0xc093, .base = 0, .limit = 0xffffffff };
    memcpy(g_ram, p, sizeof(p));
    /* GDT[3]=0x18: data seg, base 0x3000; GDT[4]=0x20: data seg, base 0x4000 */
    put64(g_ram + 0x1000 + 0x08, 0x00CF9B000000FFFFull);
    put64(g_ram + 0x1000 + 0x10, 0x00CF93000000FFFFull);
    put64(g_ram + 0x1000 + 0x18, 0x00409300300000FFull); /* base=0x3000 lim=0xff */
    put64(g_ram + 0x1000 + 0x20, 0x00409300400000FFull); /* base=0x4000 lim=0xff */
    put64(g_ram + 0x2000, 0x0000001811223344ull);        /* off=0x11223344 sel=0x18 */
    put64(g_ram + 0x2008, 0x0000002055667788ull);        /* off=0x55667788 sel=0x20 */
    put64(g_ram + 0x3000, 0x00000000DEADBEEFull);        /* fs:[0] marker */
    put64(g_ram + 0x4000, 0x00000000FEEDFACEull);        /* gs:[0] marker */
    x86_cpu_set_seg(s, X86_CPU_SEG_GDT, &gdt);
    x86_cpu_set_seg(s, X86_CPU_SEG_CS, &cs);
    x86_cpu_set_seg(s, X86_CPU_SEG_DS, &ds);
    x86_cpu_set_seg(s, X86_CPU_SEG_ES, &ds);
    x86_cpu_set_seg(s, X86_CPU_SEG_SS, &ds);
    x86_cpu_set_reg(s, X86_CPU_REG_CR0, 0x1); /* PE */
    x86_cpu_set_reg(s, X86_CPU_REG_EIP, 0);
    x86_cpu_set_reg(s, 4 /*ESP*/, 0x8000);
    x86_cpu_interp(s, 200);
    printf("  far ptr: lfs/lgs load offset+segment\n");
    CHECK(s, 3, 0x11223344);   /* ebx = lfs offset       */
    CHECK(s, 0, 0xDEADBEEF);   /* eax = *(fs:0), base 0x3000 */
    CHECK(s, 1, 0x55667788);   /* ecx = lgs offset       */
    CHECK(s, 2, 0xFEEDFACE);   /* edx = *(gs:0), base 0x4000 */
    x86_cpu_end(s);
}

/* XLAT (D7): AL <- [(e)BX + AL] in the data segment. */
static void test_xlat(void)
{
    static const uint8_t p[] = {
        0xB8,0x00,0x00,0x00,0x00,   /* mov eax,0        */
        0xBB,0x00,0x20,0x00,0x00,   /* mov ebx,0x2000   */
        0xB0,0x05,                  /* mov al,5         */
        0xD7,                       /* xlat             */
        0xF4
    };
    PhysMemoryMap *map = mk_map(0x10000);
    X86CPUState *s = mk_cpu(map, 0x8000);
    memcpy(g_ram, p, sizeof(p));
    g_ram[0x2005] = 0x77;
    x86_cpu_interp(s, 100);
    printf("  xlat: AL <- [EBX+AL]\n");
    CHECK(s, 0, 0x77);
    x86_cpu_end(s);
}

/* LAR/LSL (0F 02/03): load access rights / segment limit and set ZF; a bad
 * selector leaves the destination unchanged and clears ZF. */
static void test_lar_lsl(void)
{
    static const uint8_t p[] = {
        0xBB,0x10,0x00,0x00,0x00,   /* mov ebx,0x10      */
        0x0F,0x02,0xC3,             /* lar eax,ebx       */
        0x0F,0x03,0xCB,             /* lsl ecx,ebx       */
        0xBA,0x34,0x12,0x00,0x00,   /* mov edx,0x1234    */
        0xBB,0x34,0x12,0x00,0x00,   /* mov ebx,0x1234 (bad sel) */
        0x0F,0x02,0xD3,             /* lar edx,ebx -> fail, edx kept */
        0xF4
    };
    PhysMemoryMap *map = mk_map(0x10000);
    X86CPUState *s = x86_cpu_init(map);
    X86CPUSeg gdt = { .base = 0x1000, .limit = 0xff };
    X86CPUSeg cs  = { .sel = 0x08, .flags = 0xc09b, .base = 0, .limit = 0xffffffff };
    X86CPUSeg ds  = { .sel = 0x10, .flags = 0xc093, .base = 0, .limit = 0xffffffff };
    memcpy(g_ram, p, sizeof(p));
    put64(g_ram + 0x1000 + 0x08, 0x00CF9B000000FFFFull);
    put64(g_ram + 0x1000 + 0x10, 0x00CF93000000FFFFull); /* flat data, G=1 */
    x86_cpu_set_seg(s, X86_CPU_SEG_GDT, &gdt);
    x86_cpu_set_seg(s, X86_CPU_SEG_CS, &cs);
    x86_cpu_set_seg(s, X86_CPU_SEG_DS, &ds);
    x86_cpu_set_seg(s, X86_CPU_SEG_ES, &ds);
    x86_cpu_set_seg(s, X86_CPU_SEG_SS, &ds);
    x86_cpu_set_reg(s, X86_CPU_REG_CR0, 0x1); /* PE */
    x86_cpu_set_reg(s, X86_CPU_REG_EIP, 0);
    x86_cpu_set_reg(s, 4 /*ESP*/, 0x8000);
    x86_cpu_interp(s, 100);
    printf("  lar/lsl: access-rights + scaled limit\n");
    CHECK(s, 0, 0x00CF9300);   /* lar: AR byte of a flat data descriptor */
    CHECK(s, 1, 0xFFFFFFFF);   /* lsl: G-scaled limit                    */
    CHECK(s, 2, 0x1234);       /* lar on bad selector left edx unchanged */
    x86_cpu_end(s);
}

/* x87 transcendentals added to the D9 register map: f2xm1, fsincos, fpatan.
 * Each result is stored back with fistp and checked as an integer. */
static void test_x87_transcendental(void)
{
    static const uint8_t p[] = {
        0xD9,0xE8,                          /* fld1                  */
        0xD9,0xF0,                          /* f2xm1  -> 2^1-1 = 1   */
        0xDB,0x1D,0x00,0x20,0x00,0x00,      /* fistp [0x2000] -> 1   */
        0xD9,0xEE,                          /* fldz                  */
        0xD9,0xFB,                          /* fsincos(0): st0=cos=1, st1=sin=0 */
        0xDB,0x1D,0x04,0x20,0x00,0x00,      /* fistp [0x2004] -> 1 (cos) */
        0xDB,0x1D,0x08,0x20,0x00,0x00,      /* fistp [0x2008] -> 0 (sin) */
        0xD9,0xEE,                          /* fldz  -> st0=0        */
        0xD9,0xE8,                          /* fld1  -> st0=1, st1=0 */
        0xD9,0xF3,                          /* fpatan: atan2(0,1)=0  */
        0xDB,0x1D,0x0C,0x20,0x00,0x00,      /* fistp [0x200C] -> 0   */
        0xF4
    };
    PhysMemoryMap *map = mk_map(0x10000);
    X86CPUState *s = mk_cpu(map, 0x8000);
    int32_t r[4];
    memcpy(g_ram, p, sizeof(p));
    x86_cpu_interp(s, 200);
    memcpy(&r[0], g_ram + 0x2000, 4);
    memcpy(&r[1], g_ram + 0x2004, 4);
    memcpy(&r[2], g_ram + 0x2008, 4);
    memcpy(&r[3], g_ram + 0x200C, 4);
    printf("  x87 transc: f2xm1=%d cos0=%d sin0=%d atan2(0,1)=%d\n",
           r[0], r[1], r[2], r[3]);
    if (r[0] != 1) { printf("    *** FAIL: f2xm1(1)!=1\n"); g_fail = 1; }
    if (r[1] != 1) { printf("    *** FAIL: cos(0)!=1\n");   g_fail = 1; }
    if (r[2] != 0) { printf("    *** FAIL: sin(0)!=0\n");   g_fail = 1; }
    if (r[3] != 0) { printf("    *** FAIL: atan2(0,1)!=0\n"); g_fail = 1; }
    x86_cpu_end(s);
}

/* 80-bit extended precision + control-word RC/PC fields.
 *
 *  - retention: an integer > 2^53 round-trips fild m64 -> fistp m64 exactly.
 *    A 53-bit (double) significand would drop the low bit; a 64-bit extended
 *    significand keeps it.
 *  - RC: frndint(0.5) is 0 under round-to-nearest-even but 1 under round-up,
 *    proving the CW rounding-control field reaches the host rounding mode.
 *  - PC: 1/3 computed with PC=24 (single) zeroes the low 40 bits of the stored
 *    m80 significand; with PC=64 (extended) they are non-zero. */
static uint64_t x87_run_u64(const uint8_t *prog, int plen,
                            const void *data, int dlen, int dat_off, int out_off)
{
    PhysMemoryMap *map = mk_map(0x10000);
    X86CPUState *s = mk_cpu(map, 0x8000);
    uint64_t out;
    memcpy(g_ram, prog, plen);
    if (data) memcpy(g_ram + dat_off, data, dlen);
    x86_cpu_interp(s, 200);
    memcpy(&out, g_ram + out_off, 8);
    x86_cpu_end(s);
    return out;
}

static void test_x87_extended(void)
{
    /* --- retention: fild qword [0x2000]; fistp qword [0x2008] --- */
    static const uint8_t p_ret[] = {
        0xDF,0x2D,0x00,0x20,0x00,0x00,   /* fild  qword [0x2000] */
        0xDF,0x3D,0x08,0x20,0x00,0x00,   /* fistp qword [0x2008] */
        0xF4
    };
    uint64_t vin = 0x0020000000000001ULL;   /* 2^53 + 1 */
    uint64_t vout = x87_run_u64(p_ret, sizeof(p_ret), &vin, 8, 0x2000, 0x2008);
    printf("  x87 80-bit: fild/fistp 2^53+1 -> %#llx\n", (unsigned long long)vout);
    if (vout != vin) { printf("    *** FAIL: lost the low bit (double-precision significand)\n"); g_fail = 1; }

    /* --- RC: build 0.5 (=1/2), fldcw, frndint, fistp dword --- */
    /*  D9 E8 fld1 ; DB 05 [0x2000] fild dword(=2) ; DE F9 fdivp st1,st0 (=0.5)
     *  D9 2D [0x2004] fldcw ; D9 FC frndint ; DB 1D [0x2008] fistp dword    */
    static const uint8_t p_rc[] = {
        0xD9,0xE8,
        0xDB,0x05,0x00,0x20,0x00,0x00,
        0xDE,0xF9,
        0xD9,0x2D,0x04,0x20,0x00,0x00,
        0xD9,0xFC,
        0xDB,0x1D,0x08,0x20,0x00,0x00,
        0xF4
    };
    struct { int32_t two; uint16_t cw; } up   = { 2, 0x0B7F };  /* RC=10 round up */
    struct { int32_t two; uint16_t cw; } near = { 2, 0x037F };  /* RC=00 nearest  */
    int32_t r_up   = (int32_t)x87_run_u64(p_rc, sizeof(p_rc), &up,   6, 0x2000, 0x2008);
    int32_t r_near = (int32_t)x87_run_u64(p_rc, sizeof(p_rc), &near, 6, 0x2000, 0x2008);
    printf("  x87 RC: frndint(0.5) up=%d nearest=%d\n", r_up, r_near);
    if (r_up != 1)   { printf("    *** FAIL: round-up frndint(0.5)!=1\n");   g_fail = 1; }
    if (r_near != 0) { printf("    *** FAIL: nearest frndint(0.5)!=0\n");    g_fail = 1; }

    /* --- PC: 1/3 with PC=24 vs PC=64, inspect stored m80 significand --- */
    /*  D9 2D [0x2004] fldcw ; D9 E8 fld1 ; DB 05 [0x2000] fild(=3)
     *  DE F9 fdivp (=1/3) ; DB 3D [0x2008] fstp tbyte [0x2008]              */
    static const uint8_t p_pc[] = {
        0xD9,0x2D,0x04,0x20,0x00,0x00,
        0xD9,0xE8,
        0xDB,0x05,0x00,0x20,0x00,0x00,
        0xDE,0xF9,
        0xDB,0x3D,0x08,0x20,0x00,0x00,
        0xF4
    };
    struct { int32_t three; uint16_t cw; } pc24 = { 3, 0x007F };  /* PC=00 single */
    struct { int32_t three; uint16_t cw; } pc64 = { 3, 0x037F };  /* PC=11 extended */
    uint64_t m24 = x87_run_u64(p_pc, sizeof(p_pc), &pc24, 6, 0x2000, 0x2008);
    uint64_t m64 = x87_run_u64(p_pc, sizeof(p_pc), &pc64, 6, 0x2000, 0x2008);
    uint64_t lowmask = ((uint64_t)1 << 40) - 1;   /* bits below the 24th significand bit */
    printf("  x87 PC: 1/3 significand pc24=%#llx pc64=%#llx\n",
           (unsigned long long)m24, (unsigned long long)m64);
    if ((m24 & lowmask) != 0) { printf("    *** FAIL: PC=24 left low significand bits set\n"); g_fail = 1; }
    if ((m64 & lowmask) == 0) { printf("    *** FAIL: PC=64 dropped low significand bits\n");  g_fail = 1; }
}

/* SSE/SSE2. Regression for the python3 crash on Alpine 3.20: libpython's
 * startup runs `movd %eax,%xmm0` (66 0F 6E C0). With no SSE the 0F fallback
 * x86_stop()ed, resumed on the next IRQ with EIP *inside* the instruction,
 * and ran the ModRM byte C0 as a shift through a garbage pointer. Each step
 * below feeds the next, so a mis-sized instruction or a wrong lane shows up in
 * the final registers: movd/pshufd/paddd/movdqu, the scalar-double path
 * gcc -mfpmath=sse emits (cvtsi2sd, mulsd m64, cvttsd2si, cvtsd2si's
 * round-half-even, ucomisd flags), and an fxsave/fxrstor round trip of XMM1. */
static void test_sse(void)
{
    static const uint8_t p[] = {
        0xB8,0x00,0x02,0x00,0x00,               /* mov eax,0x200 (OSFXSR)  */
        0x0F,0x22,0xE0,                         /* mov cr4,eax             */
        0xB8,0x78,0x56,0x34,0x12,               /* mov eax,0x12345678      */
        0x66,0x0F,0x6E,0xC0,                    /* movd xmm0,eax           */
        0x66,0x0F,0x70,0xC8,0x00,               /* pshufd xmm1,xmm0,0      */
        0x66,0x0F,0xFE,0xC9,                    /* paddd xmm1,xmm1         */
        0xF3,0x0F,0x7F,0x0D,0x00,0x30,0,0,      /* movdqu [0x3000],xmm1    */
        0x66,0x0F,0x7E,0xC9,                    /* movd ecx,xmm1           */
        0xB8,0x07,0x00,0x00,0x00,               /* mov eax,7               */
        0xF2,0x0F,0x2A,0xD0,                    /* cvtsi2sd xmm2,eax       */
        0xF2,0x0F,0x59,0x15,0x00,0x31,0,0,      /* mulsd xmm2,[0x3100]     */
        0xF2,0x0F,0x2C,0xD2,                    /* cvttsd2si edx,xmm2      */
        0xF2,0x0F,0x2D,0xF2,                    /* cvtsd2si esi,xmm2       */
        0xBB,0x00,0x00,0x00,0x00,               /* mov ebx,0               */
        0x66,0x0F,0x2E,0x15,0x00,0x31,0,0,      /* ucomisd xmm2,[0x3100]   */
        0x0F,0x97,0xC3,                         /* seta bl                 */
        0x0F,0xAE,0x05,0x00,0x40,0,0,           /* fxsave [0x4000]         */
        0x66,0x0F,0xEF,0xC9,                    /* pxor xmm1,xmm1          */
        0x0F,0xAE,0x0D,0x00,0x40,0,0,           /* fxrstor [0x4000]        */
        0x66,0x0F,0x7E,0xCF,                    /* movd edi,xmm1           */
        0xF4                                    /* hlt                     */
    };
    static const uint8_t onehalf[8] = { 0,0,0,0,0,0,0xF8,0x3F }; /* 1.5 */
    PhysMemoryMap *map = mk_map(0x10000);
    X86CPUState *s = mk_cpu(map, 0x8000);
    uint32_t v;
    int i, bad = 0;
    memcpy(g_ram, p, sizeof(p));
    memcpy(g_ram + 0x3100, onehalf, 8);
    x86_cpu_interp(s, 1000);
    printf("  sse: movd/pshufd/paddd, cvtsi2sd*1.5 -> cvt(t)sd2si, ucomisd, fxsave\n");
    CHECK(s, 1, 0x2468ACF0);   /* movd ecx: (x+x) in lane 0               */
    CHECK(s, 2, 10);           /* cvttsd2si(10.5) truncates               */
    CHECK(s, 6, 10);           /* cvtsd2si(10.5) rounds half to even      */
    CHECK(s, 3, 1);            /* ucomisd 10.5 vs 1.5: CF=ZF=0 -> seta    */
    CHECK(s, 7, 0x2468ACF0);   /* fxrstor brought XMM1 back after pxor    */
    CHECK(s, X86_CPU_REG_EIP, sizeof(p)); /* every length decoded right   */
    for (i = 0; i < 4; i++) {  /* movdqu stored all four lanes            */
        memcpy(&v, g_ram + 0x3000 + 4 * i, 4);
        if (v != 0x2468ACF0) bad = 1;
    }
    memcpy(&v, g_ram + 0x4000 + 24, 4);   /* fxsave image: MXCSR default  */
    if (v != 0x1f80) bad = 1;
    memcpy(&v, g_ram + 0x4000 + 160 + 16, 4); /* XMM1 slot               */
    if (v != 0x2468ACF0) bad = 1;
    if (bad) { printf("    *** FAIL: movdqu / fxsave memory image\n"); g_fail = 1; }
    x86_cpu_end(s);
}

/* NX/XD enforcement: with EFER.NXE and CR4.PAE, fetching from a page whose XD
 * bit is set raises #PF with the present+instruction-fetch error code, even
 * though the page is present and readable. Two 2 MB PAE identity mappings alias
 * the same physical frame -- one executable, one XD -- and a jump into the XD
 * alias must fault into the #PF handler rather than executing the byte there. */
static void test_nx_exec(void)
{
    static const uint8_t prog[] = {          /* @phys 0 (VA 0, identity) */
        0xB9,0x80,0x00,0x00,0xC0,   /* mov ecx,0xC0000080 (EFER)   */
        0xB8,0x00,0x08,0x00,0x00,   /* mov eax,0x800 (NXE)         */
        0x31,0xD2,                  /* xor edx,edx                 */
        0x0F,0x30,                  /* wrmsr                       */
        0xB8,0x20,0x00,0x00,0x00,   /* mov eax,0x20 (CR4.PAE)      */
        0x0F,0x22,0xE0,             /* mov cr4,eax                 */
        0xB8,0x00,0x00,0x01,0x00,   /* mov eax,0x10000 (PDPT)      */
        0x0F,0x22,0xD8,             /* mov cr3,eax                 */
        0xB8,0x01,0x00,0x00,0x80,   /* mov eax,0x80000001 (PG|PE)  */
        0x0F,0x22,0xC0,             /* mov cr0,eax                 */
        0xB8,0x00,0x04,0x20,0x00,   /* mov eax,0x200400 (XD alias) */
        0xFF,0xE0                   /* jmp eax  -> NX #PF          */
    };
    static const uint8_t handler[] = {       /* @phys 0x600 (#PF handler) */
        0x0F,0x20,0xD0,             /* mov eax,cr2                 */
        0x59,                       /* pop ecx  (error code)       */
        0xBB,0x0D,0x60,0x00,0x00,   /* mov ebx,0x600D              */
        0xF4                        /* hlt                         */
    };
    static const uint8_t target[] = { 0xF4 };/* @phys 0x400: reached only if NX fails */

    PhysMemoryMap *map = mk_map(0x200000);   /* 2 MB, identity-mapped */
    X86CPUState *s = x86_cpu_init(map);
    memcpy(g_ram + 0x000, prog, sizeof(prog));
    memcpy(g_ram + 0x400, target, sizeof(target));
    memcpy(g_ram + 0x600, handler, sizeof(handler));

    /* GDT @0x1000, IDT @0x1800: vector 14 (#PF) -> gate sel 0x08 offset 0x600 */
    put64(g_ram + 0x1000 + 0x08, 0x00CF9B000000FFFFull);
    put64(g_ram + 0x1000 + 0x10, 0x00CF93000000FFFFull);
    put64(g_ram + 0x1800 + 14*8, 0x00008E0000080600ull);

    /* PAE tables: PDPT@0x10000 -> PD@0x11000. PDE[0] identity 2 MB (executable),
     * PDE[1] identity 2 MB alias of the SAME frame with XD (bit 63) set. */
    put64(g_ram + 0x10000, 0x0000000000011001ull); /* PDPTE[0] present -> PD    */
    put64(g_ram + 0x11000, 0x0000000000000083ull); /* PDE[0]  P|RW|PS, X ok     */
    put64(g_ram + 0x11008, 0x8000000000000083ull); /* PDE[1]  P|RW|PS + XD       */

    X86CPUSeg gdt = { .base = 0x1000, .limit = 0x17 };
    X86CPUSeg idt = { .base = 0x1800, .limit = 14*8 + 7 };
    X86CPUSeg cs  = { .sel = 0x08, .flags = 0xc09b, .base = 0, .limit = 0xffffffff };
    X86CPUSeg ds  = { .sel = 0x10, .flags = 0xc093, .base = 0, .limit = 0xffffffff };
    x86_cpu_set_seg(s, X86_CPU_SEG_GDT, &gdt);
    x86_cpu_set_seg(s, X86_CPU_SEG_IDT, &idt);
    x86_cpu_set_seg(s, X86_CPU_SEG_CS, &cs);
    x86_cpu_set_seg(s, X86_CPU_SEG_DS, &ds);
    x86_cpu_set_seg(s, X86_CPU_SEG_ES, &ds);
    x86_cpu_set_seg(s, X86_CPU_SEG_SS, &ds);
    x86_cpu_set_reg(s, X86_CPU_REG_CR0, 0x1); /* PE; guest enables PG itself */
    x86_cpu_set_reg(s, X86_CPU_REG_EIP, 0);
    x86_cpu_set_reg(s, 4 /*ESP*/, 0x8000);

    x86_cpu_interp(s, 1000);
    printf("  nx: fetch from an XD PAE page raises #PF (present + I/D)\n");
    CHECK(s, 0, 0x200400);   /* cr2 = the faulting fetch address */
    CHECK(s, 1, 0x11);       /* error code: present(1) | instr-fetch(0x10) */
    CHECK(s, 3, 0x600D);     /* handler ran (rather than the aliased hlt) */
    x86_cpu_end(s);
}

int main(void)
{
    test_basic();
    test_arith();
    test_callret();
    test_string();
    test_interrupt();
    test_flags();
    test_paging();
    test_pop_esp();
    test_bswap();
    test_mov_from_sreg();
    test_unmapped_read_width();
    test_call_indirect_pf_restart();
    test_page_cross();
    test_xadd_pf_restart();
    test_pop_mem_pf_restart();
    test_bt_imm_modulo();
    test_int_gate_dpl();
    test_iopl();
    test_popf_cpl3();
    test_far_indirect();
    test_addr16();
    test_lea_ss_base();
    test_shld_of();
    test_bcd();
    test_bound_arpl();
    test_idiv_overflow();
    test_rol_ror_cf();
    test_verrw();
    test_pae_ad();
    test_enter_level();
    test_enter_pf_restart();
    test_tlb_invlpg();
    test_tlb_cr3_reload();
    test_tlb_cpl();
    test_rep_overlap();
    test_rep_page_cross();
    test_rep_stos_pages();
    test_rep_budget_restart();
    test_rep_backward();
    test_lazy_flags();
    test_lazy_flags_ordering();
    test_far_ptr_load();
    test_xlat();
    test_lar_lsl();
    test_x87_transcendental();
    test_x87_extended();
    test_nx_exec();
    test_sse();
    printf("%s\n", g_fail ? "SELFTEST FAILED" : "SELFTEST PASSED");
    return g_fail;
}
