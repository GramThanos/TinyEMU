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
    test_enter_level();
    test_enter_pf_restart();
    printf("%s\n", g_fail ? "SELFTEST FAILED" : "SELFTEST PASSED");
    return g_fail;
}
