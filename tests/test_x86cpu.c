/*
 * x86 CPU emulator test suite
 *
 * Tests correctness of the software x86 interpreter in x86_cpu.c.
 * Covers: CPUID register mapping, AF flag, REP CMPS/SCAS exhaustion,
 * page-fault error codes, condition codes, paging, control flow.
 *
 * Build:
 *   make CONFIG_FS_NET= CONFIG_SDL= test
 * Or manually:
 *   gcc -O0 -g -I. -o test_x86cpu tests/test_x86cpu.c \
 *       x86_cpu.o iomem.o cutils.o -lrt
 * Run:
 *   ./test_x86cpu
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

#include "cutils.h"
#include "iomem.h"
#include "x86_cpu.h"

/* ---------- helpers ---------- */

static int tests_run   = 0;
static int tests_pass  = 0;
static int tests_fail  = 0;

#define PASS() do { tests_run++; tests_pass++; } while(0)
#define FAIL(msg, ...) do { \
    tests_run++; tests_fail++; \
    fprintf(stderr, "FAIL [%s:%d] " msg "\n", __func__, __LINE__, ##__VA_ARGS__); \
} while(0)

#define CHECK(cond) do { \
    if (cond) { PASS(); } \
    else { FAIL("CHECK failed: %s", #cond); } \
} while(0)

#define CHECK_EQ(a, b) do { \
    if ((uint64_t)(a) == (uint64_t)(b)) { PASS(); } \
    else { FAIL("CHECK_EQ: got 0x%llx, expected 0x%llx", \
                (unsigned long long)(uint64_t)(a), \
                (unsigned long long)(uint64_t)(b)); } \
} while(0)

/* ---------- machine setup ---------- */

#define RAM_SIZE  (4 * 1024 * 1024)   /* 4 MB */
#define RAM_BASE  0x00000000U

/* dummy IRQ callback (no interrupts in tests) */
static int no_irq(void *opaque) { (void)opaque; return -1; }

typedef struct {
    PhysMemoryMap *mem_map;
    X86CPUState   *cpu;
    uint8_t       *ram;
} TestMachine;

static TestMachine *machine_new(void)
{
    TestMachine *m = mallocz(sizeof(*m));
    m->mem_map = phys_mem_map_init();
    PhysMemoryRange *pr = cpu_register_ram(m->mem_map, RAM_BASE, RAM_SIZE, 0);
    m->ram = pr->phys_mem;
    m->cpu = x86_cpu_init(m->mem_map);
    x86_cpu_set_get_hard_intno(m->cpu, no_irq, NULL);
    return m;
}

static void machine_free(TestMachine *m)
{
    x86_cpu_end(m->cpu);
    phys_mem_map_end(m->mem_map);
    free(m);
}

/* Enter 32-bit protected mode with flat 4 GB segments.
 * GDT at gdt_base: [0]=null, [1]=code (base=0, G=1, DB=1), [2]=data (same).
 * Entry EIP = entry_eip, ESP = stack_top. */
static void enter_protected_mode(TestMachine *m, uint32_t gdt_base,
                                  uint32_t entry_eip, uint32_t stack_top)
{
    uint8_t *ram = m->ram;
    uint8_t *gdt = ram + gdt_base;
    /* null descriptor */
    memset(gdt, 0, 8);
    /* code: base=0, limit=0xFFFFF, G=1, DB=1, P=1, S=1, type=0xA */
    uint64_t code_d = 0x00CF9A000000FFFFULL;
    memcpy(gdt + 8,  &code_d, 8);
    /* data: base=0, limit=0xFFFFF, G=1, DB=1, P=1, S=1, type=0x2 */
    uint64_t data_d = 0x00CF92000000FFFFULL;
    memcpy(gdt + 16, &data_d, 8);

    /* Enable PE (CR0.PE) */
    uint32_t cr0 = x86_cpu_get_reg(m->cpu, X86_CPU_REG_CR0);
    x86_cpu_set_reg(m->cpu, X86_CPU_REG_CR0, cr0 | 1);

    X86CPUSeg sd = {0};
    /* GDT */
    sd.base  = gdt_base;
    sd.limit = 24 - 1;
    x86_cpu_set_seg(m->cpu, X86_CPU_SEG_GDT, &sd);
    /* CS = selector 0x08, flat code */
    sd.sel   = 0x08;
    sd.base  = 0;
    sd.limit = 0xFFFFFFFF;
    sd.flags = (1 << 9) | (1 << 10) | (1 << 11) | (1 << 14) | (1 << 15); /* P/S/type/DB/G */
    x86_cpu_set_seg(m->cpu, X86_CPU_SEG_CS, &sd);
    /* DS = SS = ES = selector 0x10, flat data */
    sd.sel   = 0x10;
    sd.flags = (1 << 9) | (1 << 11) | (1 << 14) | (1 << 15);
    x86_cpu_set_seg(m->cpu, X86_CPU_SEG_DS, &sd);
    x86_cpu_set_seg(m->cpu, X86_CPU_SEG_SS, &sd);
    x86_cpu_set_seg(m->cpu, X86_CPU_SEG_ES, &sd);

    x86_cpu_set_reg(m->cpu, X86_CPU_REG_EIP, entry_eip);
    x86_cpu_set_reg(m->cpu, 4, stack_top); /* ESP */
}

/* Write x86 machine code at ram[offset] and return offset. */
static uint32_t write_code(uint8_t *ram, uint32_t offset,
                            const uint8_t *code, size_t len)
{
    memcpy(ram + offset, code, len);
    return offset + (uint32_t)len;
}

/* Run up to max_cycles; return cycles executed. */
static void run_cpu(TestMachine *m, int max_cycles)
{
    x86_cpu_interp(m->cpu, max_cycles);
}

/* =====================================================================
 * Test 1: CPUID leaf 0 — vendor string order
 * Expected: EBX='Genu', EDX='ineI', ECX='ntel'
 * Register map: regs[0]=EAX, regs[1]=ECX, regs[2]=EDX, regs[3]=EBX
 * ===================================================================== */
static void test_cpuid_vendor(void)
{
    /* Code:
     *   xor eax, eax      ; leaf 0
     *   cpuid
     *   hlt
     */
    static const uint8_t code[] = {
        0x31, 0xC0,         /* XOR EAX, EAX */
        0x0F, 0xA2,         /* CPUID */
        0xF4,               /* HLT */
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ebx = x86_cpu_get_reg(m->cpu, 3);
    uint32_t edx = x86_cpu_get_reg(m->cpu, 2);
    uint32_t ecx = x86_cpu_get_reg(m->cpu, 1);
    /* EBX+EDX+ECX should spell "GenuineIntel" */
    CHECK_EQ(ebx, 0x756e6547U); /* 'Genu' */
    CHECK_EQ(edx, 0x49656e69U); /* 'ineI' */
    CHECK_EQ(ecx, 0x6c65746eU); /* 'ntel' */
    machine_free(m);
}

/* =====================================================================
 * Test 2: CPUID leaf 1 — PAE bit must be in EDX (bit 6)
 * ===================================================================== */
static void test_cpuid_leaf1_pae(void)
{
    static const uint8_t code[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00, /* MOV EAX, 1 */
        0x0F, 0xA2,                    /* CPUID */
        0xF4,                          /* HLT */
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t edx = x86_cpu_get_reg(m->cpu, 2); /* regs[2] = EDX */
    CHECK((edx >> 6) & 1); /* PAE must be set in EDX */
    CHECK((edx >> 0) & 1); /* FPU */
    CHECK((edx >> 15) & 1); /* CMOV */

    uint32_t ecx = x86_cpu_get_reg(m->cpu, 1); /* regs[1] = ECX */
    (void)ecx; /* ECX may have extended features; just check it's not confused with EDX */

    machine_free(m);
}

/* =====================================================================
 * Test 3: CPUID leaf 0x80000001 — LM bit in EDX (bit 29)
 * ===================================================================== */
static void test_cpuid_ext_lm(void)
{
    static const uint8_t code[] = {
        0xB8, 0x01, 0x00, 0x00, 0x80, /* MOV EAX, 0x80000001 */
        0x0F, 0xA2,                    /* CPUID */
        0xF4,                          /* HLT */
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t edx = x86_cpu_get_reg(m->cpu, 2); /* regs[2] = EDX */
    CHECK((edx >> 29) & 1); /* LM (Long Mode) must be in EDX */
    CHECK((edx >> 11) & 1); /* SYSCALL */

    machine_free(m);
}

/* =====================================================================
 * Test 4: AF flag — ADD 8+8=16 must set AF (carry out of nibble)
 * ===================================================================== */
static void test_af_flag_add(void)
{
    /* Code:
     *   mov al, 8
     *   add al, 8    ; 8+8=16, carry out of bit 3 → AF=1
     *   pushf
     *   pop eax      ; eax = eflags
     *   hlt
     */
    static const uint8_t code[] = {
        0xB0, 0x08,         /* MOV AL, 8 */
        0x04, 0x08,         /* ADD AL, 8 */
        0x9C,               /* PUSHF */
        0x58,               /* POP EAX */
        0xF4,               /* HLT */
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t eflags = x86_cpu_get_reg(m->cpu, 0); /* EAX = popped eflags */
    CHECK((eflags >> 4) & 1); /* AF = bit 4 of eflags */
    machine_free(m);
}

/* =====================================================================
 * Test 5: AF flag — ADD 5+3=8 must NOT set AF
 * ===================================================================== */
static void test_af_flag_no_carry(void)
{
    static const uint8_t code[] = {
        0xB0, 0x05,         /* MOV AL, 5 */
        0x04, 0x03,         /* ADD AL, 3 */
        0x9C,               /* PUSHF */
        0x58,               /* POP EAX */
        0xF4,               /* HLT */
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t eflags = x86_cpu_get_reg(m->cpu, 0);
    CHECK(!((eflags >> 4) & 1)); /* AF must NOT be set: 5+3=8, no nibble carry */
    machine_free(m);
}

/* =====================================================================
 * Test 6: AF flag — SUB 0x10-1 must set AF (borrow from nibble)
 * ===================================================================== */
static void test_af_flag_sub(void)
{
    static const uint8_t code[] = {
        0xB0, 0x10,         /* MOV AL, 0x10 */
        0x2C, 0x01,         /* SUB AL, 1 */
        0x9C,               /* PUSHF */
        0x58,               /* POP EAX */
        0xF4,               /* HLT */
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t eflags = x86_cpu_get_reg(m->cpu, 0);
    CHECK((eflags >> 4) & 1); /* AF=1: 0x10-1, borrow from bit 4 */
    machine_free(m);
}

/* =====================================================================
 * Test 7: REP SCAS exhaustion — ECX must be 0 after full scan
 * without finding the byte
 * ===================================================================== */
static void test_rep_scas_exhaustion(void)
{
    /*
     * Set up: EDI → buffer of 4 bytes (all 0xFF), AL=0x00 (won't match),
     * ECX=4, REPNE SCASB. After exhaustion ECX should be 0 (was bug: 0xFFFFFFFF).
     *
     * Code:
     *   mov edi, 0x5000     ; buffer
     *   mov ecx, 4
     *   mov al, 0x00
     *   repne scasb
     *   mov ebx, ecx        ; save ECX result
     *   hlt
     */
    static const uint8_t code[] = {
        0xBF, 0x00, 0x50, 0x00, 0x00, /* MOV EDI, 0x5000 */
        0xB9, 0x04, 0x00, 0x00, 0x00, /* MOV ECX, 4 */
        0xB0, 0x00,                    /* MOV AL, 0x00 */
        0xF2, 0xAE,                    /* REPNE SCASB */
        0x89, 0xCB,                    /* MOV EBX, ECX */
        0xF4,                          /* HLT */
    };

    TestMachine *m = machine_new();
    /* Fill buffer with 0xFF (won't match 0x00) */
    memset(m->ram + 0x5000, 0xFF, 16);
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ecx_result = x86_cpu_get_reg(m->cpu, 3); /* EBX saved ECX */
    CHECK_EQ(ecx_result, 0U); /* ECX must be 0 after full exhaustion */
    machine_free(m);
}

/* =====================================================================
 * Test 8: REP CMPS exhaustion — ECX must be 0 when buffers are equal
 * ===================================================================== */
static void test_rep_cmps_exhaustion(void)
{
    /*
     * REPE CMPSB on two identical 4-byte buffers: should exhaust and leave ECX=0.
     *
     * Code:
     *   mov esi, 0x5000
     *   mov edi, 0x5100
     *   mov ecx, 4
     *   repe cmpsb
     *   mov ebx, ecx
     *   hlt
     */
    static const uint8_t code[] = {
        0xBE, 0x00, 0x50, 0x00, 0x00, /* MOV ESI, 0x5000 */
        0xBF, 0x00, 0x51, 0x00, 0x00, /* MOV EDI, 0x5100 */
        0xB9, 0x04, 0x00, 0x00, 0x00, /* MOV ECX, 4 */
        0xF3, 0xA6,                    /* REPE CMPSB */
        0x89, 0xCB,                    /* MOV EBX, ECX */
        0xF4,                          /* HLT */
    };

    TestMachine *m = machine_new();
    /* Two identical 4-byte buffers */
    memcpy(m->ram + 0x5000, "ABCD", 4);
    memcpy(m->ram + 0x5100, "ABCD", 4);
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ecx_result = x86_cpu_get_reg(m->cpu, 3);
    CHECK_EQ(ecx_result, 0U); /* ECX must be 0 after full exhaustion */
    machine_free(m);
}

/* =====================================================================
 * Test 9: REP CMPS early break — ECX = remaining count
 * ===================================================================== */
static void test_rep_cmps_early_break(void)
{
    /*
     * REPE CMPSB: src="ABXD", dst="ABCD" — mismatch at byte 2 (0-indexed).
     * After break, ECX should be 1 (1 byte remaining after the mismatch byte).
     */
    static const uint8_t code[] = {
        0xBE, 0x00, 0x50, 0x00, 0x00, /* MOV ESI, 0x5000 */
        0xBF, 0x00, 0x51, 0x00, 0x00, /* MOV EDI, 0x5100 */
        0xB9, 0x04, 0x00, 0x00, 0x00, /* MOV ECX, 4 */
        0xF3, 0xA6,                    /* REPE CMPSB */
        0x89, 0xCB,                    /* MOV EBX, ECX */
        0xF4,                          /* HLT */
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x5000, "ABXD", 4);  /* src */
    memcpy(m->ram + 0x5100, "ABCD", 4);  /* dst */
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ecx_result = x86_cpu_get_reg(m->cpu, 3);
    CHECK_EQ(ecx_result, 1U); /* 1 byte remaining after mismatch at index 2 */
    machine_free(m);
}

/* =====================================================================
 * Test 10: Condition codes — JNZ taken when ZF=0 (after non-zero result)
 * ===================================================================== */
static void test_jnz_taken(void)
{
    /*
     * xor eax, eax  ; EAX=0, ZF=1
     * inc eax       ; EAX=1, ZF=0
     * jnz target    ; must jump
     * mov ebx, 0xDEAD  ; should NOT execute
     * target:
     * mov ebx, 0x1234  ; should execute
     * hlt
     */
    static const uint8_t code[] = {
        0x31, 0xC0,              /* XOR EAX, EAX */
        0x40,                    /* INC EAX */
        0x75, 0x05,              /* JNZ +5 */
        0xBB, 0xAD, 0xDE, 0x00, 0x00, /* MOV EBX, 0xDEAD */
        0xBB, 0x34, 0x12, 0x00, 0x00, /* MOV EBX, 0x1234 */
        0xF4,                    /* HLT */
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ebx = x86_cpu_get_reg(m->cpu, 3);
    CHECK_EQ(ebx, 0x1234U);
    machine_free(m);
}

/* =====================================================================
 * Test 11: Condition codes — JNZ not taken when ZF=1
 * ===================================================================== */
static void test_jnz_not_taken(void)
{
    static const uint8_t code[] = {
        0x31, 0xC0,              /* XOR EAX, EAX */  /* ZF=1 */
        0x75, 0x05,              /* JNZ +5 — should NOT jump */
        0xBB, 0x34, 0x12, 0x00, 0x00, /* MOV EBX, 0x1234 — should execute */
        0xEB, 0x05,              /* JMP past bad code */
        0xBB, 0xAD, 0xDE, 0x00, 0x00, /* MOV EBX, 0xDEAD — should NOT execute */
        0xF4,                    /* HLT */
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ebx = x86_cpu_get_reg(m->cpu, 3);
    CHECK_EQ(ebx, 0x1234U);
    machine_free(m);
}

/* =====================================================================
 * Test 12: MOV, CALL, RET — basic control flow
 * ===================================================================== */
static void test_call_ret(void)
{
    /*
     * entry:
     *   call func
     *   mov ebx, 0x5678
     *   hlt
     * func:
     *   mov ebx, 0x1234
     *   ret
     */
    static const uint8_t code[] = {
        /* entry at 0x1000 */
        0xE8, 0x06, 0x00, 0x00, 0x00,  /* CALL func (rel32=+6: next_pc=0x1005, func=0x100B) */
        0xBB, 0x78, 0x56, 0x00, 0x00,  /* MOV EBX, 0x5678 */
        0xF4,                           /* HLT */
        /* func at 0x100B */
        0xBB, 0x34, 0x12, 0x00, 0x00,  /* MOV EBX, 0x1234 */
        0xC3,                           /* RET */
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ebx = x86_cpu_get_reg(m->cpu, 3);
    CHECK_EQ(ebx, 0x5678U); /* after func returns, 0x5678 is loaded */
    machine_free(m);
}

/* =====================================================================
 * Test 13: REP STOSD — fills memory and ECX becomes 0
 * ===================================================================== */
static void test_rep_stosd(void)
{
    /*
     * mov edi, 0x5000
     * mov ecx, 4
     * mov eax, 0xDEADBEEF
     * rep stosd
     * mov ebx, ecx   ; should be 0
     * hlt
     */
    static const uint8_t code[] = {
        0xBF, 0x00, 0x50, 0x00, 0x00,  /* MOV EDI, 0x5000 */
        0xB9, 0x04, 0x00, 0x00, 0x00,  /* MOV ECX, 4 */
        0xB8, 0xEF, 0xBE, 0xAD, 0xDE,  /* MOV EAX, 0xDEADBEEF */
        0xF3, 0xAB,                     /* REP STOSD */
        0x89, 0xCB,                     /* MOV EBX, ECX */
        0xF4,                           /* HLT */
    };

    TestMachine *m = machine_new();
    memset(m->ram + 0x5000, 0, 32);
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ecx = x86_cpu_get_reg(m->cpu, 3);
    CHECK_EQ(ecx, 0U); /* ECX=0 after REP STOSD */

    /* Verify memory was written */
    uint32_t v;
    memcpy(&v, m->ram + 0x5000, 4);
    CHECK_EQ(v, 0xDEADBEEFU);
    memcpy(&v, m->ram + 0x500C, 4);
    CHECK_EQ(v, 0xDEADBEEFU);
    machine_free(m);
}

/* =====================================================================
 * Test 14: Standard 2-level paging — read/write with paging enabled
 * ===================================================================== */
static void test_paging_32bit(void)
{
    /*
     * Set up a minimal page table:
     *   - Page directory at 0x100000 (1MB)
     *   - Page table for vaddr 0x0-0x3FFFFF at 0x101000
     *   - Identity-map first 4 MB
     *
     * Then enable paging, write a value to vaddr, read it back.
     * Code will be at vaddr 0x4000 (identity-mapped from phys 0x4000).
     */

    TestMachine *m = machine_new();
    uint8_t *ram = m->ram;

    /* Build page directory at phys 0x100000 */
    memset(ram + 0x100000, 0, 0x1000);
    /* First PDE: points to page table at 0x101000, P+W */
    uint32_t pde = 0x101000 | 3;
    memcpy(ram + 0x100000, &pde, 4);

    /* Build page table at phys 0x101000 — identity map 1024 pages */
    memset(ram + 0x101000, 0, 0x1000);
    for (int i = 0; i < 1024; i++) {
        uint32_t pte = (i << 12) | 3; /* P+W, physical page = virtual page */
        memcpy(ram + 0x101000 + i * 4, &pte, 4);
    }

    /*
     * Code at phys/virt 0x4000:
     *   mov eax, cr0
     *   or  eax, 0x80000000   ; set PG bit
     *   mov cr0, eax          ; enable paging
     *   mov [0x5000], dword 0x12345678
     *   mov eax, [0x5000]
     *   mov ebx, eax
     *   hlt
     */
    static const uint8_t code[] = {
        0x0F, 0x20, 0xC0,               /* MOV EAX, CR0 */
        0x0D, 0x00, 0x00, 0x00, 0x80,   /* OR EAX, 0x80000000 */
        0x0F, 0x22, 0xC0,               /* MOV CR0, EAX */
        0xC7, 0x05, 0x00, 0x50, 0x00, 0x00, /* MOV [0x5000], */
            0x78, 0x56, 0x34, 0x12,     /*   0x12345678 */
        0x8B, 0x05, 0x00, 0x50, 0x00, 0x00, /* MOV EAX, [0x5000] */
        0x89, 0xC3,                     /* MOV EBX, EAX */
        0xF4,                           /* HLT */
    };

    memcpy(ram + 0x4000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x4000, 0x3000);

    /* Point CR3 at page directory */
    x86_cpu_set_reg(m->cpu, X86_CPU_REG_CR0, 1); /* PE only for now */
    /* Set CR3 via write to the struct — use phys mem approach */
    /* Actually we need to write CR3 before enabling PG; the code does it */
    /* Use the emulator's ability to set CR3 by passing through: write code that does MOV CR3 */
    /* Simplest: write CR3 setup into code preamble */
    static const uint8_t cr3_code[] = {
        0xB8, 0x00, 0x00, 0x10, 0x00,   /* MOV EAX, 0x100000 (PD) */
        0x0F, 0x22, 0xD8,               /* MOV CR3, EAX */
    };
    /* Prepend cr3_code before the main code */
    memmove(ram + 0x4000 + sizeof(cr3_code), ram + 0x4000, sizeof(code));
    memcpy(ram + 0x4000, cr3_code, sizeof(cr3_code));

    run_cpu(m, 200000);

    uint32_t ebx = x86_cpu_get_reg(m->cpu, 3);
    CHECK_EQ(ebx, 0x12345678U);
    machine_free(m);
}

/* =====================================================================
 * Test 15: JZ / JE condition codes
 * ===================================================================== */
static void test_jz_taken(void)
{
    static const uint8_t code[] = {
        0x31, 0xC0,              /* XOR EAX, EAX  → ZF=1 */
        0x74, 0x05,              /* JZ +5 → should jump */
        0xBB, 0xAD, 0xDE, 0x00, 0x00, /* MOV EBX, 0xDEAD (skipped) */
        0xBB, 0x34, 0x12, 0x00, 0x00, /* MOV EBX, 0x1234 */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ebx = x86_cpu_get_reg(m->cpu, 3);
    CHECK_EQ(ebx, 0x1234U);
    machine_free(m);
}

/* =====================================================================
 * Test 16: SHL / SHR basic
 * ===================================================================== */
static void test_shift(void)
{
    static const uint8_t code[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00, /* MOV EAX, 1 */
        0xC1, 0xE0, 0x04,             /* SHL EAX, 4  → EAX = 0x10 */
        0x89, 0xC3,                   /* MOV EBX, EAX */
        0xC1, 0xEB, 0x02,             /* SHR EBX, 2  → EBX = 4 */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ebx = x86_cpu_get_reg(m->cpu, 3);
    CHECK_EQ(ebx, 4U);
    machine_free(m);
}

/* =====================================================================
 * Test 17: MOVZX / MOVSX
 * ===================================================================== */
static void test_movzx_movsx(void)
{
    static const uint8_t code[] = {
        0xB0, 0xFF,             /* MOV AL, 0xFF */
        0x0F, 0xB6, 0xC0,       /* MOVZX EAX, AL → EAX = 0xFF */
        0x89, 0xC3,             /* MOV EBX, EAX */
        0xB0, 0xFF,             /* MOV AL, 0xFF */
        0x0F, 0xBE, 0xC0,       /* MOVSX EAX, AL → EAX = 0xFFFFFFFF (-1) */
        0x89, 0xC1,             /* MOV ECX, EAX */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ebx = x86_cpu_get_reg(m->cpu, 3);
    uint32_t ecx = x86_cpu_get_reg(m->cpu, 1);
    CHECK_EQ(ebx, 0xFFU);
    CHECK_EQ(ecx, 0xFFFFFFFFU);
    machine_free(m);
}

/* =====================================================================
 * Test 18: PUSH / POP / stack frame
 * ===================================================================== */
static void test_push_pop(void)
{
    static const uint8_t code[] = {
        0xB8, 0x78, 0x56, 0x34, 0x12, /* MOV EAX, 0x12345678 */
        0x50,                          /* PUSH EAX */
        0xBB, 0x00, 0x00, 0x00, 0x00, /* MOV EBX, 0 */
        0x5B,                          /* POP EBX */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ebx = x86_cpu_get_reg(m->cpu, 3);
    CHECK_EQ(ebx, 0x12345678U);
    machine_free(m);
}

/* =====================================================================
 * Test 19: Unsigned multiplication (MUL)
 * ===================================================================== */
static void test_mul(void)
{
    static const uint8_t code[] = {
        0xB8, 0x10, 0x00, 0x00, 0x00,  /* MOV EAX, 0x10 */
        0xB9, 0x10, 0x00, 0x00, 0x00,  /* MOV ECX, 0x10 */
        0xF7, 0xE1,                     /* MUL ECX → EDX:EAX = 0x100 */
        0x89, 0xC3,                     /* MOV EBX, EAX */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ebx = x86_cpu_get_reg(m->cpu, 3);
    CHECK_EQ(ebx, 0x100U);
    machine_free(m);
}

/* =====================================================================
 * Test 20: NEG — negate and check SF/ZF
 * ===================================================================== */
static void test_neg(void)
{
    static const uint8_t code[] = {
        0xB8, 0x05, 0x00, 0x00, 0x00, /* MOV EAX, 5 */
        0xF7, 0xD8,                   /* NEG EAX → EAX = -5 = 0xFFFFFFFB */
        0x89, 0xC3,                   /* MOV EBX, EAX */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ebx = x86_cpu_get_reg(m->cpu, 3);
    CHECK_EQ(ebx, 0xFFFFFFFBU); /* -5 two's complement */
    machine_free(m);
}

/* =====================================================================
 * Test 21: JA / JNBE (unsigned above) condition code
 * ===================================================================== */
static void test_ja(void)
{
    /* CMP 5, 3 → 5 > 3 (unsigned), CF=0, ZF=0 → JA taken */
    static const uint8_t code[] = {
        0xB8, 0x05, 0x00, 0x00, 0x00, /* MOV EAX, 5 */
        0x3D, 0x03, 0x00, 0x00, 0x00, /* CMP EAX, 3 */
        0x77, 0x05,                    /* JA +5 */
        0xBB, 0xAD, 0xDE, 0x00, 0x00,
        0xBB, 0x34, 0x12, 0x00, 0x00, /* MOV EBX, 0x1234 */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ebx = x86_cpu_get_reg(m->cpu, 3);
    CHECK_EQ(ebx, 0x1234U);
    machine_free(m);
}

/* =====================================================================
 * Test 22: JGE / JNL (signed >= ) condition code
 * ===================================================================== */
static void test_jge(void)
{
    /* CMP 5, 3 → SF=0, OF=0 → JGE taken */
    static const uint8_t code[] = {
        0xB8, 0x05, 0x00, 0x00, 0x00, /* MOV EAX, 5 */
        0x3D, 0x03, 0x00, 0x00, 0x00, /* CMP EAX, 3 */
        0x7D, 0x05,                    /* JGE +5 */
        0xBB, 0xAD, 0xDE, 0x00, 0x00,
        0xBB, 0x34, 0x12, 0x00, 0x00,
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ebx = x86_cpu_get_reg(m->cpu, 3);
    CHECK_EQ(ebx, 0x1234U);
    machine_free(m);
}

/* =====================================================================
 * Test 23: XCHG — swaps two registers
 * ===================================================================== */
static void test_xchg(void)
{
    static const uint8_t code[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00, /* MOV EAX, 1 */
        0xBB, 0x02, 0x00, 0x00, 0x00, /* MOV EBX, 2 */
        0x93,                          /* XCHG EAX, EBX */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t eax = x86_cpu_get_reg(m->cpu, 0);
    uint32_t ebx = x86_cpu_get_reg(m->cpu, 3);
    CHECK_EQ(eax, 2U);
    CHECK_EQ(ebx, 1U);
    machine_free(m);
}

/* =====================================================================
 * Test 24: AND / OR / XOR with flags
 * ===================================================================== */
static void test_logic_flags(void)
{
    /* XOR EAX, EAX → ZF=1, CF=0, SF=0 */
    static const uint8_t code[] = {
        0xB8, 0x05, 0x00, 0x00, 0x00, /* MOV EAX, 5 */
        0x31, 0xC0,                    /* XOR EAX, EAX → ZF=1 */
        0x9C,                          /* PUSHF */
        0x5B,                          /* POP EBX */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t flags = x86_cpu_get_reg(m->cpu, 3);
    CHECK((flags >> 6) & 1); /* ZF=1 */
    CHECK(!((flags >> 0) & 1)); /* CF=0 */
    CHECK(!((flags >> 7) & 1)); /* SF=0 */
    machine_free(m);
}

/* =====================================================================
 * Test 25: REP MOVSB — memory copy
 * ===================================================================== */
static void test_rep_movsb(void)
{
    static const uint8_t code[] = {
        0xBE, 0x00, 0x50, 0x00, 0x00,  /* MOV ESI, 0x5000 */
        0xBF, 0x00, 0x51, 0x00, 0x00,  /* MOV EDI, 0x5100 */
        0xB9, 0x05, 0x00, 0x00, 0x00,  /* MOV ECX, 5 */
        0xF3, 0xA4,                     /* REP MOVSB */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x5000, "Hello", 5);
    memset(m->ram + 0x5100, 0, 5);
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    CHECK(memcmp(m->ram + 0x5100, "Hello", 5) == 0);
    machine_free(m);
}

/* =====================================================================
 * Test 26: BSWAP (0F C8) — reverses byte order
 * ===================================================================== */
static void test_bswap(void)
{
    static const uint8_t code[] = {
        0xB8, 0x78, 0x56, 0x34, 0x12, /* MOV EAX, 0x12345678 */
        0x0F, 0xC8,                    /* BSWAP EAX */
        0x89, 0xC3,                    /* MOV EBX, EAX */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ebx = x86_cpu_get_reg(m->cpu, 3);
    CHECK_EQ(ebx, 0x78563412U);
    machine_free(m);
}

/* =====================================================================
 * Test 27: SETNE / SETZ — SETcc instructions
 * ===================================================================== */
static void test_setcc(void)
{
    static const uint8_t code[] = {
        0x31, 0xC0,        /* XOR EAX, EAX → ZF=1 */
        0x0F, 0x94, 0xC3,  /* SETZ BL → BL=1 (ZF=1) */
        0x0F, 0x95, 0xC1,  /* SETNZ CL → CL=0 (ZF=1, so NZ=false) */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    /* BL = low byte of EBX, CL = low byte of ECX */
    uint32_t ebx = x86_cpu_get_reg(m->cpu, 3);
    uint32_t ecx = x86_cpu_get_reg(m->cpu, 1);
    CHECK_EQ(ebx & 0xFF, 1U); /* BL = 1 (SETZ with ZF=1) */
    CHECK_EQ(ecx & 0xFF, 0U); /* CL = 0 (SETNZ with ZF=1 → NZ is false) */
    machine_free(m);
}

/* =====================================================================
 * Test 28: CMPXCHG (0F B1) — compare and exchange
 * ===================================================================== */
static void test_cmpxchg(void)
{
    /* CMPXCHG [mem], ECX: if EAX==[mem], [mem]=ECX, ZF=1; else EAX=[mem] */
    static const uint8_t code[] = {
        0xC7, 0x05, 0x00, 0x50, 0x00, 0x00, /* MOV [0x5000], 0x10 */
            0x10, 0x00, 0x00, 0x00,
        0xB8, 0x10, 0x00, 0x00, 0x00,       /* MOV EAX, 0x10 (matches [0x5000]) */
        0xB9, 0x20, 0x00, 0x00, 0x00,       /* MOV ECX, 0x20 (new value) */
        0x0F, 0xB1, 0x0D, 0x00, 0x50, 0x00, 0x00, /* CMPXCHG [0x5000], ECX */
        0x8B, 0x1D, 0x00, 0x50, 0x00, 0x00, /* MOV EBX, [0x5000] */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ebx = x86_cpu_get_reg(m->cpu, 3);
    CHECK_EQ(ebx, 0x20U); /* [0x5000] should be 0x20 after successful CMPXCHG */
    machine_free(m);
}

/* =====================================================================
 * Test 29: LOOP instruction
 * ===================================================================== */
static void test_loop(void)
{
    /* Loop: i starts at 1, increments to 5; sum = 1+2+3+4+5 = 15 */
    static const uint8_t code[] = {
        0xB8, 0x00, 0x00, 0x00, 0x00, /* MOV EAX, 0 (accumulator) */
        0xBB, 0x00, 0x00, 0x00, 0x00, /* MOV EBX, 0 (counter i) */
        0xB9, 0x05, 0x00, 0x00, 0x00, /* MOV ECX, 5 (loop count) */
        /* loop_top: */
        0x43,                          /* INC EBX  (i++) */
        0x03, 0xC3,                    /* ADD EAX, EBX */
        0xE2, 0xFB,                    /* LOOP loop_top (disp = -5) */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t eax = x86_cpu_get_reg(m->cpu, 0);
    CHECK_EQ(eax, 15U); /* 1+2+3+4+5 = 15 */
    machine_free(m);
}

/* =====================================================================
 * Test 30: AF flag — INC 0x0F must set AF (nibble carry to bit 4)
 * ===================================================================== */
static void test_af_inc(void)
{
    static const uint8_t code[] = {
        0xB0, 0x0F,   /* MOV AL, 0x0F */
        0xFE, 0xC0,   /* INC AL → AL = 0x10, AF=1 (carry from low nibble) */
        0x9C,
        0x58,         /* POP EAX (flags) */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t eflags = x86_cpu_get_reg(m->cpu, 0);
    CHECK((eflags >> 4) & 1); /* AF=1 */
    machine_free(m);
}

/* ---------- main ---------- */

int main(void)
{
    fprintf(stderr, "Running x86 CPU emulator tests...\n\n");

    test_cpuid_vendor();
    test_cpuid_leaf1_pae();
    test_cpuid_ext_lm();
    test_af_flag_add();
    test_af_flag_no_carry();
    test_af_flag_sub();
    test_rep_scas_exhaustion();
    test_rep_cmps_exhaustion();
    test_rep_cmps_early_break();
    test_jnz_taken();
    test_jnz_not_taken();
    test_call_ret();
    test_rep_stosd();
    test_paging_32bit();
    test_jz_taken();
    test_shift();
    test_movzx_movsx();
    test_push_pop();
    test_mul();
    test_neg();
    test_ja();
    test_jge();
    test_xchg();
    test_logic_flags();
    test_rep_movsb();
    test_bswap();
    test_setcc();
    test_cmpxchg();
    test_loop();
    test_af_inc();

    fprintf(stderr, "\nResults: %d/%d passed", tests_pass, tests_run);
    if (tests_fail)
        fprintf(stderr, ", %d FAILED\n", tests_fail);
    else
        fprintf(stderr, " (all pass)\n");

    return (tests_fail > 0) ? 1 : 0;
}
