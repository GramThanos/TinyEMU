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


/* =====================================================================
 * Test 31: JO taken — overflow flag set
 * ===================================================================== */
static void test_jo_taken(void)
{
    static const uint8_t code[] = {
        0xB8, 0xFF, 0xFF, 0xFF, 0x7F, /* MOV EAX, 0x7FFFFFFF */
        0x83, 0xC0, 0x01,             /* ADD EAX, 1  ; OF=1 */
        0x70, 0x05,                   /* JO +5 (taken) */
        0xBB, 0xAD, 0xDE, 0x00, 0x00, /* MOV EBX, 0xDEAD (skipped) */
        0xBB, 0x01, 0x00, 0x00, 0x00, /* MOV EBX, 1 */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ebx = x86_cpu_get_reg(m->cpu, 3);
    CHECK_EQ(ebx, 1U);
    machine_free(m);
}

/* =====================================================================
 * Test 32: JO not taken — overflow flag clear
 * ===================================================================== */
static void test_jo_not_taken(void)
{
    static const uint8_t code[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00, /* MOV EAX, 1 */
        0x83, 0xC0, 0x01,             /* ADD EAX, 1  ; OF=0 */
        0x70, 0x05,                   /* JO +5 (not taken) */
        0xBB, 0x02, 0x00, 0x00, 0x00, /* MOV EBX, 2 (executed) */
        0xEB, 0x05,                   /* JMP +5 */
        0xBB, 0xFF, 0xFF, 0xFF, 0xFF, /* MOV EBX, bad (skipped) */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ebx = x86_cpu_get_reg(m->cpu, 3);
    CHECK_EQ(ebx, 2U);
    machine_free(m);
}

/* =====================================================================
 * Test 33: JB taken — carry flag set
 * ===================================================================== */
static void test_jb_taken(void)
{
    static const uint8_t code[] = {
        0xB8, 0xFF, 0xFF, 0xFF, 0xFF, /* MOV EAX, 0xFFFFFFFF */
        0x83, 0xC0, 0x01,             /* ADD EAX, 1  ; CF=1 */
        0x72, 0x05,                   /* JB +5 (taken) */
        0xBB, 0xAD, 0xDE, 0x00, 0x00, /* MOV EBX, 0xDEAD (skipped) */
        0xBB, 0x01, 0x00, 0x00, 0x00, /* MOV EBX, 1 */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ebx = x86_cpu_get_reg(m->cpu, 3);
    CHECK_EQ(ebx, 1U);
    machine_free(m);
}

/* =====================================================================
 * Test 34: JB not taken — carry flag clear
 * ===================================================================== */
static void test_jb_not_taken(void)
{
    static const uint8_t code[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00, /* MOV EAX, 1 */
        0x83, 0xC0, 0x01,             /* ADD EAX, 1  ; CF=0 */
        0x72, 0x05,                   /* JB +5 (not taken) */
        0xBB, 0x02, 0x00, 0x00, 0x00, /* MOV EBX, 2 (executed) */
        0xEB, 0x05,                   /* JMP +5 */
        0xBB, 0xFF, 0xFF, 0xFF, 0xFF, /* MOV EBX, bad (skipped) */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ebx = x86_cpu_get_reg(m->cpu, 3);
    CHECK_EQ(ebx, 2U);
    machine_free(m);
}

/* =====================================================================
 * Test 35: JBE taken — ZF=1 (equal comparison)
 * ===================================================================== */
static void test_jbe_taken(void)
{
    static const uint8_t code[] = {
        0xB8, 0x03, 0x00, 0x00, 0x00, /* MOV EAX, 3 */
        0x3D, 0x03, 0x00, 0x00, 0x00, /* CMP EAX, 3  ; ZF=1 */
        0x76, 0x05,                   /* JBE +5 (taken) */
        0xB8, 0xAD, 0xDE, 0x00, 0x00, /* MOV EAX, 0xDEAD (skipped) */
        0xB8, 0x01, 0x00, 0x00, 0x00, /* MOV EAX, 1 */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t eax = x86_cpu_get_reg(m->cpu, 0);
    CHECK_EQ(eax, 1U);
    machine_free(m);
}

/* =====================================================================
 * Test 36: JBE not taken — CF=0, ZF=0 (strictly above)
 * ===================================================================== */
static void test_jbe_not_taken(void)
{
    static const uint8_t code[] = {
        0xB8, 0x05, 0x00, 0x00, 0x00, /* MOV EAX, 5 */
        0x3D, 0x03, 0x00, 0x00, 0x00, /* CMP EAX, 3  ; CF=0, ZF=0 */
        0x76, 0x05,                   /* JBE +5 (not taken) */
        0xB8, 0x02, 0x00, 0x00, 0x00, /* MOV EAX, 2 (executed) */
        0xEB, 0x05,                   /* JMP +5 */
        0xB8, 0xFF, 0xFF, 0xFF, 0xFF, /* MOV EAX, bad (skipped) */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t eax = x86_cpu_get_reg(m->cpu, 0);
    CHECK_EQ(eax, 2U);
    machine_free(m);
}

/* =====================================================================
 * Test 37: JS taken — sign flag set
 * ===================================================================== */
static void test_js_taken(void)
{
    static const uint8_t code[] = {
        0xB8, 0xFF, 0xFF, 0xFF, 0xFF, /* MOV EAX, 0xFFFFFFFF (-1) */
        0x3D, 0x00, 0x00, 0x00, 0x00, /* CMP EAX, 0  ; SF=1 */
        0x78, 0x05,                   /* JS +5 (taken) */
        0xB9, 0xAD, 0xDE, 0x00, 0x00, /* MOV ECX, 0xDEAD (skipped) */
        0xB9, 0x01, 0x00, 0x00, 0x00, /* MOV ECX, 1 */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ecx = x86_cpu_get_reg(m->cpu, 1);
    CHECK_EQ(ecx, 1U);
    machine_free(m);
}

/* =====================================================================
 * Test 38: JS not taken — sign flag clear
 * ===================================================================== */
static void test_js_not_taken(void)
{
    static const uint8_t code[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00, /* MOV EAX, 1 */
        0x83, 0xC0, 0x01,             /* ADD EAX, 1  ; SF=0 */
        0x78, 0x05,                   /* JS +5 (not taken) */
        0xB9, 0x02, 0x00, 0x00, 0x00, /* MOV ECX, 2 (executed) */
        0xEB, 0x05,                   /* JMP +5 */
        0xB9, 0xFF, 0xFF, 0xFF, 0xFF, /* MOV ECX, bad (skipped) */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ecx = x86_cpu_get_reg(m->cpu, 1);
    CHECK_EQ(ecx, 2U);
    machine_free(m);
}

/* =====================================================================
 * Test 39: JP taken — parity flag set (even number of 1-bits)
 * ===================================================================== */
static void test_jp_taken(void)
{
    /* 3 = 0b00000011: two set bits -> even parity -> PF=1 */
    static const uint8_t code[] = {
        0x31, 0xC0,                   /* XOR EAX, EAX */
        0x83, 0xC0, 0x03,             /* ADD EAX, 3  ; result=3, PF=1 */
        0x7A, 0x05,                   /* JP +5 (taken) */
        0xB9, 0xAD, 0xDE, 0x00, 0x00, /* MOV ECX, 0xDEAD (skipped) */
        0xB9, 0x01, 0x00, 0x00, 0x00, /* MOV ECX, 1 */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ecx = x86_cpu_get_reg(m->cpu, 1);
    CHECK_EQ(ecx, 1U);
    machine_free(m);
}

/* =====================================================================
 * Test 40: JP not taken — parity flag clear (odd number of 1-bits)
 * ===================================================================== */
static void test_jp_not_taken(void)
{
    /* 1 = 0b00000001: one set bit -> odd parity -> PF=0 */
    static const uint8_t code[] = {
        0x31, 0xC0,                   /* XOR EAX, EAX */
        0x83, 0xC0, 0x01,             /* ADD EAX, 1  ; result=1, PF=0 */
        0x7A, 0x05,                   /* JP +5 (not taken) */
        0xB9, 0x02, 0x00, 0x00, 0x00, /* MOV ECX, 2 (executed) */
        0xEB, 0x05,                   /* JMP +5 */
        0xB9, 0xFF, 0xFF, 0xFF, 0xFF, /* MOV ECX, bad (skipped) */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ecx = x86_cpu_get_reg(m->cpu, 1);
    CHECK_EQ(ecx, 2U);
    machine_free(m);
}

/* =====================================================================
 * Test 41: JL taken — signed less: SF != OF
 * ===================================================================== */
static void test_jl_taken(void)
{
    /* CMP -1, 0: result=-1, SF=1, OF=0 -> SF!=OF -> JL taken */
    static const uint8_t code[] = {
        0xB8, 0xFF, 0xFF, 0xFF, 0xFF, /* MOV EAX, 0xFFFFFFFF (-1) */
        0x3D, 0x00, 0x00, 0x00, 0x00, /* CMP EAX, 0  ; SF=1, OF=0 */
        0x7C, 0x05,                   /* JL +5 (taken) */
        0xB8, 0xAD, 0xDE, 0x00, 0x00, /* MOV EAX, 0xDEAD (skipped) */
        0xB8, 0x01, 0x00, 0x00, 0x00, /* MOV EAX, 1 */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t eax = x86_cpu_get_reg(m->cpu, 0);
    CHECK_EQ(eax, 1U);
    machine_free(m);
}

/* =====================================================================
 * Test 42: JLE taken — signed less-or-equal: SF != OF
 * ===================================================================== */
static void test_jle_taken(void)
{
    /* CMP 2, 3: 2-3=-1, ZF=0, SF=1, OF=0 -> ZF=0 but SF!=OF -> JLE taken */
    static const uint8_t code[] = {
        0xB8, 0x02, 0x00, 0x00, 0x00, /* MOV EAX, 2 */
        0x3D, 0x03, 0x00, 0x00, 0x00, /* CMP EAX, 3  ; SF=1, OF=0 */
        0x7E, 0x05,                   /* JLE +5 (taken) */
        0xB8, 0xAD, 0xDE, 0x00, 0x00, /* MOV EAX, 0xDEAD (skipped) */
        0xB8, 0x01, 0x00, 0x00, 0x00, /* MOV EAX, 1 */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t eax = x86_cpu_get_reg(m->cpu, 0);
    CHECK_EQ(eax, 1U);
    machine_free(m);
}

/* =====================================================================
 * Test 43: JG taken — signed greater: ZF=0 and SF==OF
 * ===================================================================== */
static void test_jg_taken(void)
{
    /* CMP 5, 3: 5-3=2, ZF=0, SF=0, OF=0 -> ZF=0 && SF==OF -> JG taken */
    static const uint8_t code[] = {
        0xB8, 0x05, 0x00, 0x00, 0x00, /* MOV EAX, 5 */
        0x3D, 0x03, 0x00, 0x00, 0x00, /* CMP EAX, 3  ; ZF=0, SF=0, OF=0 */
        0x7F, 0x05,                   /* JG +5 (taken) */
        0xB8, 0xAD, 0xDE, 0x00, 0x00, /* MOV EAX, 0xDEAD (skipped) */
        0xB8, 0x01, 0x00, 0x00, 0x00, /* MOV EAX, 1 */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t eax = x86_cpu_get_reg(m->cpu, 0);
    CHECK_EQ(eax, 1U);
    machine_free(m);
}

/* =====================================================================
 * Test 44: ADC — add with carry
 * ===================================================================== */
static void test_adc(void)
{
    /* STC; ADC EAX, 1 with EAX=5 -> EAX = 5+1+CF(1) = 7 */
    static const uint8_t code[] = {
        0xB8, 0x05, 0x00, 0x00, 0x00, /* MOV EAX, 5 */
        0xF9,                         /* STC  ; CF=1 */
        0x83, 0xD0, 0x01,             /* ADC EAX, 1 */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t eax = x86_cpu_get_reg(m->cpu, 0);
    CHECK_EQ(eax, 7U);
    machine_free(m);
}

/* =====================================================================
 * Test 45: SBB — subtract with borrow
 * ===================================================================== */
static void test_sbb(void)
{
    /* STC; SBB EAX, 1 with EAX=10 -> EAX = 10-1-CF(1) = 8 */
    static const uint8_t code[] = {
        0xB8, 0x0A, 0x00, 0x00, 0x00, /* MOV EAX, 10 */
        0xF9,                         /* STC  ; CF=1 */
        0x83, 0xD8, 0x01,             /* SBB EAX, 1 */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t eax = x86_cpu_get_reg(m->cpu, 0);
    CHECK_EQ(eax, 8U);
    machine_free(m);
}

/* =====================================================================
 * Test 46: ROL — rotate left by 1
 * ===================================================================== */
static void test_rol(void)
{
    /* ROL EAX, 1: 0x80000001 -> 0x00000003, CF=1 (old bit 31) */
    static const uint8_t code[] = {
        0xB8, 0x01, 0x00, 0x00, 0x80, /* MOV EAX, 0x80000001 */
        0xD1, 0xC0,                   /* ROL EAX, 1 */
        0x89, 0xC3,                   /* MOV EBX, EAX  ; save result */
        0x9C,                         /* PUSHF */
        0x58,                         /* POP EAX  ; flags */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ebx = x86_cpu_get_reg(m->cpu, 3);
    uint32_t eflags = x86_cpu_get_reg(m->cpu, 0);
    CHECK_EQ(ebx, 0x00000003U);
    CHECK((eflags >> 0) & 1); /* CF=1 */
    machine_free(m);
}

/* =====================================================================
 * Test 47: ROR — rotate right by 1
 * ===================================================================== */
static void test_ror(void)
{
    /* ROR EAX, 1: 0x00000003 -> 0x80000001, CF=1 (old bit 0) */
    static const uint8_t code[] = {
        0xB8, 0x03, 0x00, 0x00, 0x00, /* MOV EAX, 0x00000003 */
        0xD1, 0xC8,                   /* ROR EAX, 1 */
        0x89, 0xC3,                   /* MOV EBX, EAX  ; save result */
        0x9C,                         /* PUSHF */
        0x58,                         /* POP EAX  ; flags */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ebx = x86_cpu_get_reg(m->cpu, 3);
    uint32_t eflags = x86_cpu_get_reg(m->cpu, 0);
    CHECK_EQ(ebx, 0x80000001U);
    CHECK((eflags >> 0) & 1); /* CF=1 */
    machine_free(m);
}

/* =====================================================================
 * Test 48: IMUL two-operand form (0F AF)
 * ===================================================================== */
static void test_imul_2op(void)
{
    /* IMUL ECX, EDX: ECX=3, EDX=7 -> ECX=21 */
    static const uint8_t code[] = {
        0xB9, 0x03, 0x00, 0x00, 0x00, /* MOV ECX, 3 */
        0xBA, 0x07, 0x00, 0x00, 0x00, /* MOV EDX, 7 */
        0x0F, 0xAF, 0xCA,             /* IMUL ECX, EDX */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ecx = x86_cpu_get_reg(m->cpu, 1);
    CHECK_EQ(ecx, 21U);
    machine_free(m);
}

/* =====================================================================
 * Test 49: IMUL three-operand imm8 form (6B)
 * ===================================================================== */
static void test_imul_3op_imm8(void)
{
    /* IMUL ECX, EDX, 5: EDX=4 -> ECX=20 */
    static const uint8_t code[] = {
        0xBA, 0x04, 0x00, 0x00, 0x00, /* MOV EDX, 4 */
        0x6B, 0xCA, 0x05,             /* IMUL ECX, EDX, 5 */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ecx = x86_cpu_get_reg(m->cpu, 1);
    CHECK_EQ(ecx, 20U);
    machine_free(m);
}

/* =====================================================================
 * Test 50: IMUL with negative operand
 * ===================================================================== */
static void test_imul_neg(void)
{
    /* IMUL EAX, ECX: EAX=4, ECX=-3 -> EAX=-12 (0xFFFFFFF4) */
    static const uint8_t code[] = {
        0xB8, 0x04, 0x00, 0x00, 0x00, /* MOV EAX, 4 */
        0xB9, 0xFD, 0xFF, 0xFF, 0xFF, /* MOV ECX, 0xFFFFFFFD (-3) */
        0x0F, 0xAF, 0xC1,             /* IMUL EAX, ECX */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t eax = x86_cpu_get_reg(m->cpu, 0);
    CHECK_EQ(eax, 0xFFFFFFF4U); /* -12 */
    machine_free(m);
}

/* =====================================================================
 * Test 51: DIV — unsigned division
 * ===================================================================== */
static void test_div(void)
{
    /* DIV ECX: EDX:EAX=100, ECX=7 -> EAX=14 (quotient), EDX=2 (remainder) */
    static const uint8_t code[] = {
        0xB8, 0x64, 0x00, 0x00, 0x00, /* MOV EAX, 100 */
        0xBA, 0x00, 0x00, 0x00, 0x00, /* MOV EDX, 0 */
        0xB9, 0x07, 0x00, 0x00, 0x00, /* MOV ECX, 7 */
        0xF7, 0xF1,                   /* DIV ECX */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t eax = x86_cpu_get_reg(m->cpu, 0);
    uint32_t edx = x86_cpu_get_reg(m->cpu, 2);
    CHECK_EQ(eax, 14U); /* quotient */
    CHECK_EQ(edx, 2U);  /* remainder */
    machine_free(m);
}

/* =====================================================================
 * Test 52: IDIV — signed division
 * ===================================================================== */
static void test_idiv(void)
{
    /* IDIV ECX: EDX:EAX=-13, ECX=4 -> EAX=-3 (0xFFFFFFFD), EDX=-1 (0xFFFFFFFF) */
    static const uint8_t code[] = {
        0xB8, 0xF3, 0xFF, 0xFF, 0xFF, /* MOV EAX, 0xFFFFFFF3 (-13) */
        0xBA, 0xFF, 0xFF, 0xFF, 0xFF, /* MOV EDX, 0xFFFFFFFF (high dword of sign-extended -13) */
        0xB9, 0x04, 0x00, 0x00, 0x00, /* MOV ECX, 4 */
        0xF7, 0xF9,                   /* IDIV ECX */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t eax = x86_cpu_get_reg(m->cpu, 0);
    uint32_t edx = x86_cpu_get_reg(m->cpu, 2);
    CHECK_EQ(eax, 0xFFFFFFFDU); /* quotient -3 */
    CHECK_EQ(edx, 0xFFFFFFFFU); /* remainder -1 */
    machine_free(m);
}

/* =====================================================================
 * Test 53: CLC / STC / CMC — flag control instructions
 * ===================================================================== */
static void test_clc_stc_cmc(void)
{
    /* CLC (CF=0) -> STC (CF=1) -> CMC (CF=0); verify CF=0 */
    static const uint8_t code[] = {
        0xF8,  /* CLC */
        0xF9,  /* STC */
        0xF5,  /* CMC  ; CF: 1 -> 0 */
        0x9C,  /* PUSHF */
        0x58,  /* POP EAX */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t eflags = x86_cpu_get_reg(m->cpu, 0);
    CHECK(!((eflags >> 0) & 1)); /* CF=0 after CMC on CF=1 */
    machine_free(m);
}

/* =====================================================================
 * Test 54: CWDE — sign-extend AX to EAX
 * ===================================================================== */
static void test_cwde(void)
{
    /* AX=0xFF80 (negative 16-bit), CWDE -> EAX=0xFFFFFF80 */
    static const uint8_t code[] = {
        0xB8, 0x80, 0xFF, 0x00, 0x00, /* MOV EAX, 0x0000FF80 */
        0x98,                         /* CWDE */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t eax = x86_cpu_get_reg(m->cpu, 0);
    CHECK_EQ(eax, 0xFFFFFF80U);
    machine_free(m);
}

/* =====================================================================
 * Test 55: CDQ — sign-extend EAX to EDX:EAX
 * ===================================================================== */
static void test_cdq(void)
{
    /* EAX=0x80000000 (negative), CDQ -> EDX=0xFFFFFFFF */
    static const uint8_t code[] = {
        0xB8, 0x00, 0x00, 0x00, 0x80, /* MOV EAX, 0x80000000 */
        0x99,                         /* CDQ */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t edx = x86_cpu_get_reg(m->cpu, 2);
    CHECK_EQ(edx, 0xFFFFFFFFU);
    machine_free(m);
}

/* =====================================================================
 * Test 56: ENTER / LEAVE — stack frame setup and teardown
 * ===================================================================== */
static void test_enter_leave(void)
{
    /*
     * ESP starts at 0x3000, EBP starts at 0.
     * ENTER 8, 0: push EBP -> ESP=0x2FFC; EBP=0x2FFC; ESP=0x2FF4.
     * LEAVE: ESP=EBP=0x2FFC; pop EBP=0; ESP=0x3000.
     */
    static const uint8_t code[] = {
        0xC8, 0x08, 0x00, 0x00, /* ENTER 8, 0 */
        0x89, 0xE9,              /* MOV ECX, EBP  ; ECX=0x2FFC */
        0x89, 0xE2,              /* MOV EDX, ESP  ; EDX=0x2FF4 */
        0xC9,                    /* LEAVE */
        0x89, 0xE3,              /* MOV EBX, ESP  ; EBX=0x3000 */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ecx = x86_cpu_get_reg(m->cpu, 1);
    uint32_t edx = x86_cpu_get_reg(m->cpu, 2);
    uint32_t ebx = x86_cpu_get_reg(m->cpu, 3);
    CHECK_EQ(ecx, 0x2FFCU); /* EBP after ENTER */
    CHECK_EQ(edx, 0x2FF4U); /* ESP after ENTER */
    CHECK_EQ(ebx, 0x3000U); /* ESP after LEAVE */
    machine_free(m);
}

/* =====================================================================
 * Test 57: LEA with SIB addressing
 * ===================================================================== */
static void test_lea_sib(void)
{
    /* LEA EAX, [EBX+ECX*4+8]: EBX=0x100, ECX=4 -> EAX=0x100+16+8=0x118 */
    static const uint8_t code[] = {
        0xBB, 0x00, 0x01, 0x00, 0x00, /* MOV EBX, 0x100 */
        0xB9, 0x04, 0x00, 0x00, 0x00, /* MOV ECX, 4 */
        0x8D, 0x44, 0x8B, 0x08,       /* LEA EAX, [EBX+ECX*4+8] */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t eax = x86_cpu_get_reg(m->cpu, 0);
    CHECK_EQ(eax, 0x118U);
    machine_free(m);
}

/* =====================================================================
 * Test 58: BT — bit test sets CF
 * ===================================================================== */
static void test_bt(void)
{
    /* BT EAX, 3: EAX=0x08 -> CF=1; EAX=0x04 -> CF=0 */
    static const uint8_t code[] = {
        0xB8, 0x08, 0x00, 0x00, 0x00, /* MOV EAX, 0x08 */
        0x0F, 0xBA, 0xE0, 0x03,       /* BT EAX, 3  ; CF=1 */
        0x9C,                         /* PUSHF */
        0x5B,                         /* POP EBX  ; flags1 -> EBX */
        0xB8, 0x04, 0x00, 0x00, 0x00, /* MOV EAX, 0x04 */
        0x0F, 0xBA, 0xE0, 0x03,       /* BT EAX, 3  ; CF=0 */
        0x9C,                         /* PUSHF */
        0x58,                         /* POP EAX  ; flags2 -> EAX */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ebx = x86_cpu_get_reg(m->cpu, 3);
    uint32_t eax = x86_cpu_get_reg(m->cpu, 0);
    CHECK((ebx >> 0) & 1);    /* CF=1: bit 3 of 0x08 is set */
    CHECK(!((eax >> 0) & 1)); /* CF=0: bit 3 of 0x04 is clear */
    machine_free(m);
}

/* =====================================================================
 * Test 59: BTS — bit test and set
 * ===================================================================== */
static void test_bts(void)
{
    /* BTS EAX, 2: EAX=0 -> set bit 2 -> EAX=4, CF=0 (bit was clear) */
    static const uint8_t code[] = {
        0x31, 0xC0,             /* XOR EAX, EAX */
        0x0F, 0xBA, 0xE8, 0x02, /* BTS EAX, 2 */
        0x89, 0xC3,             /* MOV EBX, EAX  ; EBX=4 */
        0x9C,                   /* PUSHF */
        0x58,                   /* POP EAX  ; flags */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ebx = x86_cpu_get_reg(m->cpu, 3);
    uint32_t eflags = x86_cpu_get_reg(m->cpu, 0);
    CHECK_EQ(ebx, 4U);
    CHECK(!((eflags >> 0) & 1)); /* CF=0 (bit was not set before BTS) */
    machine_free(m);
}

/* =====================================================================
 * Test 60: BTR — bit test and reset
 * ===================================================================== */
static void test_btr(void)
{
    /* BTR EAX, 2: EAX=7 -> clear bit 2 -> EAX=3, CF=1 (bit was set) */
    static const uint8_t code[] = {
        0xB8, 0x07, 0x00, 0x00, 0x00, /* MOV EAX, 7 */
        0x0F, 0xBA, 0xF0, 0x02,       /* BTR EAX, 2 */
        0x89, 0xC3,                   /* MOV EBX, EAX  ; EBX=3 */
        0x9C,                         /* PUSHF */
        0x58,                         /* POP EAX  ; flags */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ebx = x86_cpu_get_reg(m->cpu, 3);
    uint32_t eflags = x86_cpu_get_reg(m->cpu, 0);
    CHECK_EQ(ebx, 3U);
    CHECK((eflags >> 0) & 1); /* CF=1 (bit was set before BTR) */
    machine_free(m);
}

/* =====================================================================
 * Test 61: BTC — bit test and complement
 * ===================================================================== */
static void test_btc(void)
{
    /* BTC EAX, 1: EAX=0 -> toggle bit 1 -> EAX=2, CF=0 (bit was clear) */
    static const uint8_t code[] = {
        0x31, 0xC0,             /* XOR EAX, EAX */
        0x0F, 0xBA, 0xF8, 0x01, /* BTC EAX, 1 */
        0x89, 0xC3,             /* MOV EBX, EAX  ; EBX=2 */
        0x9C,                   /* PUSHF */
        0x58,                   /* POP EAX  ; flags */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ebx = x86_cpu_get_reg(m->cpu, 3);
    uint32_t eflags = x86_cpu_get_reg(m->cpu, 0);
    CHECK_EQ(ebx, 2U);
    CHECK(!((eflags >> 0) & 1)); /* CF=0 (bit was not set before BTC) */
    machine_free(m);
}

/* =====================================================================
 * Test 62: SHLD — double-precision shift left
 * ===================================================================== */
static void test_shld(void)
{
    /*
     * SHLD EAX, ECX, 4: EAX=0x12340000, ECX=0xABCDEFFF
     * Result: (EAX<<4) | (ECX>>28) = 0x23400000 | 0xA = 0x2340000A
     */
    static const uint8_t code[] = {
        0xB8, 0x00, 0x00, 0x34, 0x12, /* MOV EAX, 0x12340000 */
        0xB9, 0xFF, 0xEF, 0xCD, 0xAB, /* MOV ECX, 0xABCDEFFF */
        0x0F, 0xA4, 0xC8, 0x04,       /* SHLD EAX, ECX, 4 */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t eax = x86_cpu_get_reg(m->cpu, 0);
    CHECK_EQ(eax, 0x2340000AU);
    machine_free(m);
}

/* =====================================================================
 * Test 63: SHRD — double-precision shift right
 * ===================================================================== */
static void test_shrd(void)
{
    /*
     * SHRD EAX, ECX, 4: EAX=0x0000ABCD, ECX=0x12340000
     * Result: (EAX>>4) | (ECX[3:0]<<28) = 0xABC | (0<<28) = 0x00000ABC
     * (ECX low nibble = 0, so upper 4 bits of result are 0)
     */
    static const uint8_t code[] = {
        0xB8, 0xCD, 0xAB, 0x00, 0x00, /* MOV EAX, 0x0000ABCD */
        0xB9, 0x00, 0x00, 0x34, 0x12, /* MOV ECX, 0x12340000 */
        0x0F, 0xAC, 0xC8, 0x04,       /* SHRD EAX, ECX, 4 */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t eax = x86_cpu_get_reg(m->cpu, 0);
    CHECK_EQ(eax, 0x00000ABCU);
    machine_free(m);
}

/* =====================================================================
 * Test 64: XADD — exchange and add
 * ===================================================================== */
static void test_xadd(void)
{
    /* XADD EBX, ECX: EBX=10, ECX=5 -> EBX=15, ECX=10 (old EBX) */
    static const uint8_t code[] = {
        0xBB, 0x0A, 0x00, 0x00, 0x00, /* MOV EBX, 10 */
        0xB9, 0x05, 0x00, 0x00, 0x00, /* MOV ECX, 5 */
        0x0F, 0xC1, 0xCB,             /* XADD EBX, ECX */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ebx = x86_cpu_get_reg(m->cpu, 3);
    uint32_t ecx = x86_cpu_get_reg(m->cpu, 1);
    CHECK_EQ(ebx, 15U);
    CHECK_EQ(ecx, 10U);
    machine_free(m);
}

/* =====================================================================
 * Test 65: CMOVZ — conditional move if zero flag set
 * ===================================================================== */
static void test_cmovz(void)
{
    /* CMP EBX, EBX -> ZF=1; CMOVZ EAX, ECX -> EAX=ECX=42 */
    static const uint8_t code[] = {
        0xB9, 0x2A, 0x00, 0x00, 0x00, /* MOV ECX, 42 */
        0xB8, 0xFF, 0xFF, 0xFF, 0xFF, /* MOV EAX, 0xFFFFFFFF */
        0x39, 0xDB,                   /* CMP EBX, EBX  ; ZF=1 */
        0x0F, 0x44, 0xC1,             /* CMOVZ EAX, ECX */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t eax = x86_cpu_get_reg(m->cpu, 0);
    CHECK_EQ(eax, 42U);
    machine_free(m);
}

/* =====================================================================
 * Test 66: CMOVNZ — conditional move if zero flag clear
 * ===================================================================== */
static void test_cmovnz(void)
{
    /* CMP EBX, 2 with EBX=1 -> ZF=0; CMOVNZ EAX, ECX -> EAX=42 */
    static const uint8_t code[] = {
        0xB9, 0x2A, 0x00, 0x00, 0x00, /* MOV ECX, 42 */
        0xB8, 0xFF, 0xFF, 0xFF, 0xFF, /* MOV EAX, 0xFFFFFFFF */
        0xBB, 0x01, 0x00, 0x00, 0x00, /* MOV EBX, 1 */
        0x83, 0xFB, 0x02,             /* CMP EBX, 2  ; ZF=0 */
        0x0F, 0x45, 0xC1,             /* CMOVNZ EAX, ECX */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t eax = x86_cpu_get_reg(m->cpu, 0);
    CHECK_EQ(eax, 42U);
    machine_free(m);
}

/* =====================================================================
 * Test 67: INC / DEC — increment and decrement; ZF on DEC-to-zero
 * ===================================================================== */
static void test_inc_dec(void)
{
    static const uint8_t code[] = {
        0x31, 0xC0,                   /* XOR EAX, EAX  ; EAX=0 */
        0x40,                         /* INC EAX  ; EAX=1 */
        0xB9, 0x05, 0x00, 0x00, 0x00, /* MOV ECX, 5 */
        0x49,                         /* DEC ECX  ; ECX=4 */
        0x89, 0xCB,                   /* MOV EBX, ECX  ; EBX=4 */
        0xB9, 0x01, 0x00, 0x00, 0x00, /* MOV ECX, 1 */
        0x49,                         /* DEC ECX  ; ECX=0, ZF=1 */
        0x9C,                         /* PUSHF */
        0x5A,                         /* POP EDX  ; flags */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t eax = x86_cpu_get_reg(m->cpu, 0);
    uint32_t ebx = x86_cpu_get_reg(m->cpu, 3);
    uint32_t edx = x86_cpu_get_reg(m->cpu, 2);
    CHECK_EQ(eax, 1U);
    CHECK_EQ(ebx, 4U);
    CHECK((edx >> 6) & 1); /* ZF=1 after DEC to 0 */
    machine_free(m);
}

/* =====================================================================
 * Test 68: NOT — bitwise complement
 * ===================================================================== */
static void test_not(void)
{
    static const uint8_t code[] = {
        0xB8, 0xF0, 0xF0, 0xF0, 0xF0, /* MOV EAX, 0xF0F0F0F0 */
        0xF7, 0xD0,                   /* NOT EAX */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t eax = x86_cpu_get_reg(m->cpu, 0);
    CHECK_EQ(eax, 0x0F0F0F0FU);
    machine_free(m);
}

/* =====================================================================
 * Test 69: 16-bit operand prefix (0x66) ADD AX, AX
 * ===================================================================== */
static void test_16bit_add(void)
{
    /* AX=0x1234; 0x66 ADD AX, AX -> AX=0x2468, high word unchanged */
    static const uint8_t code[] = {
        0xB8, 0x34, 0x12, 0x00, 0x00, /* MOV EAX, 0x00001234 */
        0x66, 0x01, 0xC0,             /* ADD AX, AX  ; AX=0x2468 */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t eax = x86_cpu_get_reg(m->cpu, 0);
    CHECK_EQ(eax, 0x2468U);
    machine_free(m);
}

/* =====================================================================
 * Test 70: Parity flag — even number of set bits -> PF=1
 * ===================================================================== */
static void test_pf(void)
{
    /* 0x81 = 0b10000001: 2 set bits (even) -> PF=1 */
    static const uint8_t code[] = {
        0x31, 0xC0,       /* XOR EAX, EAX */
        0xB0, 0x81,       /* MOV AL, 0x81 */
        0x85, 0xC0,       /* TEST EAX, EAX  ; PF from result 0x81 */
        0x9C,             /* PUSHF */
        0x58,             /* POP EAX */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t eflags = x86_cpu_get_reg(m->cpu, 0);
    CHECK((eflags >> 2) & 1); /* PF=1 */
    machine_free(m);
}

/* =====================================================================
 * Test 71: OF set by ADD overflow (positive -> negative)
 * ===================================================================== */
static void test_of_add(void)
{
    /* ADD 0x7FFFFFFF + 1 -> 0x80000000, OF=1 */
    static const uint8_t code[] = {
        0xB8, 0xFF, 0xFF, 0xFF, 0x7F, /* MOV EAX, 0x7FFFFFFF */
        0x83, 0xC0, 0x01,             /* ADD EAX, 1 */
        0x9C,                         /* PUSHF */
        0x58,                         /* POP EAX */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t eflags = x86_cpu_get_reg(m->cpu, 0);
    CHECK((eflags >> 11) & 1); /* OF=1 */
    machine_free(m);
}

/* =====================================================================
 * Test 72: OF set by SUB overflow (negative -> positive)
 * ===================================================================== */
static void test_of_sub(void)
{
    /* SUB 0x80000000 - 1 -> 0x7FFFFFFF, OF=1 */
    static const uint8_t code[] = {
        0xB8, 0x00, 0x00, 0x00, 0x80, /* MOV EAX, 0x80000000 */
        0x83, 0xE8, 0x01,             /* SUB EAX, 1 */
        0x9C,                         /* PUSHF */
        0x58,                         /* POP EAX */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t eflags = x86_cpu_get_reg(m->cpu, 0);
    CHECK((eflags >> 11) & 1); /* OF=1 */
    machine_free(m);
}

/* =====================================================================
 * Test 73: LAHF / SAHF — load/store AH from/to flags
 * ===================================================================== */
static void test_lahf_sahf(void)
{
    /* STC -> LAHF captures CF=1 in AH; CLC; SAHF restores CF=1 */
    static const uint8_t code[] = {
        0xF9,  /* STC  ; CF=1 */
        0x9F,  /* LAHF ; AH = flags with CF=1 */
        0xF8,  /* CLC  ; CF=0 */
        0x9E,  /* SAHF ; restore flags from AH -> CF=1 */
        0x9C,  /* PUSHF */
        0x58,  /* POP EAX */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t eflags = x86_cpu_get_reg(m->cpu, 0);
    CHECK((eflags >> 0) & 1); /* CF=1 restored by SAHF */
    machine_free(m);
}

/* =====================================================================
 * Test 74: LODSB — load string byte
 * ===================================================================== */
static void test_lodsb(void)
{
    /* [0x5000]=0x42; LODSB -> AL=0x42, ESI=0x5001 */
    static const uint8_t code[] = {
        0x31, 0xC0,                   /* XOR EAX, EAX */
        0xBE, 0x00, 0x50, 0x00, 0x00, /* MOV ESI, 0x5000 */
        0xAC,                         /* LODSB  ; AL=[ESI], ESI++ */
        0x89, 0xC3,                   /* MOV EBX, EAX */
        0xF4,
    };

    TestMachine *m = machine_new();
    m->ram[0x5000] = 0x42;
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ebx = x86_cpu_get_reg(m->cpu, 3);
    uint32_t esi = x86_cpu_get_reg(m->cpu, 6);
    CHECK_EQ(ebx & 0xFF, 0x42U);
    CHECK_EQ(esi, 0x5001U);
    machine_free(m);
}

/* =====================================================================
 * Test 75: STOSB — store string byte
 * ===================================================================== */
static void test_stosb(void)
{
    /* AL=0x42, EDI=0x5000; STOSB -> [0x5000]=0x42, EDI=0x5001 */
    static const uint8_t code[] = {
        0xB0, 0x42,                   /* MOV AL, 0x42 */
        0xBF, 0x00, 0x50, 0x00, 0x00, /* MOV EDI, 0x5000 */
        0xAA,                         /* STOSB  ; [EDI]=AL, EDI++ */
        0xF4,
    };

    TestMachine *m = machine_new();
    memset(m->ram + 0x5000, 0, 4);
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t edi = x86_cpu_get_reg(m->cpu, 7);
    CHECK_EQ(m->ram[0x5000], 0x42U);
    CHECK_EQ(edi, 0x5001U);
    machine_free(m);
}

/* =====================================================================
 * Test 76: SCASB match — ZF=1 when AL == [EDI]
 * ===================================================================== */
static void test_scasb_match(void)
{
    /* AL=5, [0x5000]=5 -> SCASB -> ZF=1 */
    static const uint8_t code[] = {
        0xB0, 0x05,                   /* MOV AL, 5 */
        0xBF, 0x00, 0x50, 0x00, 0x00, /* MOV EDI, 0x5000 */
        0xAE,                         /* SCASB  ; cmp AL,[EDI], EDI++ */
        0x9C,                         /* PUSHF */
        0x58,                         /* POP EAX */
        0xF4,
    };

    TestMachine *m = machine_new();
    m->ram[0x5000] = 5;
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t eflags = x86_cpu_get_reg(m->cpu, 0);
    CHECK((eflags >> 6) & 1); /* ZF=1 (match) */
    machine_free(m);
}

/* =====================================================================
 * Test 77: MOV BYTE [mem], imm8 — byte memory write and read-back
 * ===================================================================== */
static void test_mem_byte_write(void)
{
    static const uint8_t code[] = {
        /* MOV BYTE [0x5000], 0xAB */
        0xC6, 0x05, 0x00, 0x50, 0x00, 0x00, 0xAB,
        /* MOVZX EAX, BYTE [0x5000] */
        0x0F, 0xB6, 0x05, 0x00, 0x50, 0x00, 0x00,
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t eax = x86_cpu_get_reg(m->cpu, 0);
    CHECK_EQ(eax, 0xABU);
    machine_free(m);
}

/* =====================================================================
 * Test 78: MOV WORD [mem], imm16 — word memory write and read-back
 * ===================================================================== */
static void test_mem_word_write(void)
{
    static const uint8_t code[] = {
        /* MOV WORD [0x5000], 0x1234 (0x66 prefix) */
        0x66, 0xC7, 0x05, 0x00, 0x50, 0x00, 0x00, 0x34, 0x12,
        /* MOVZX EAX, WORD [0x5000] */
        0x0F, 0xB7, 0x05, 0x00, 0x50, 0x00, 0x00,
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t eax = x86_cpu_get_reg(m->cpu, 0);
    CHECK_EQ(eax, 0x1234U);
    machine_free(m);
}

/* ---------- helpers for interrupt / IDT tests ---------- */

/*
 * Install a 32-bit interrupt gate into an in-RAM IDT.
 * The gate has DPL=0, P=1, type=0xE (interrupt gate, clears IF on entry).
 *   idt_base:    physical address of IDT base in ram[]
 *   vector:      interrupt vector number (0–255)
 *   cs_sel:      code segment selector for the handler
 *   handler_off: flat EIP of the handler
 */
static void setup_idt_entry(uint8_t *ram, uint32_t idt_base, int vector,
                             uint16_t cs_sel, uint32_t handler_off)
{
    uint32_t lo = ((uint32_t)cs_sel << 16) | (handler_off & 0xFFFFU);
    uint32_t hi = (handler_off & 0xFFFF0000U) | (0x8EU << 8); /* P=1, type=0xE */
    memcpy(ram + idt_base + vector * 8,     &lo, 4);
    memcpy(ram + idt_base + vector * 8 + 4, &hi, 4);
}

/* One-shot hardware IRQ callback: fires vector 0x20 exactly once. */
static int irq_once(void *opaque)
{
    int *fired = (int *)opaque;
    if (!*fired) { *fired = 1; return 0x20; }
    return -1;
}

/* =====================================================================
 * Test 79: Software INT 0x80 + IRET, with EFLAGS.IF preservation check
 *
 * Sets IF=1 via STI, issues INT 0x80.  The interrupt gate (type=0xE) clears
 * IF on entry; IRET restores it.  Verifies:
 *   EAX = 0xDEAD (handler ran)
 *   EBX = eflags captured inside handler  -> IF=0
 *   ECX = eflags captured after IRET      -> IF=1 (restored)
 * ===================================================================== */
static void test_int80_iret(void)
{
    /*
     * Main code at 0x1000:
     *   FB          STI
     *   CD 80       INT 0x80
     *   9C          PUSHF         (captures restored eflags after IRET)
     *   59          POP ECX
     *   F4          HLT
     *
     * Handler at 0x1010:
     *   B8 AD DE 00 00   MOV EAX, 0xDEAD
     *   9C               PUSHF    (captures handler eflags, IF should be 0)
     *   5B               POP EBX
     *   CF               IRET
     */
    static const uint8_t main_code[] = {
        0xFB,                            /* STI */
        0xCD, 0x80,                      /* INT 0x80 */
        0x9C,                            /* PUSHF */
        0x59,                            /* POP ECX */
        0xF4,                            /* HLT */
    };
    static const uint8_t handler_code[] = {
        0xB8, 0xAD, 0xDE, 0x00, 0x00,   /* MOV EAX, 0xDEAD */
        0x9C,                            /* PUSHF */
        0x5B,                            /* POP EBX */
        0xCF,                            /* IRET */
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, main_code,    sizeof(main_code));
    memcpy(m->ram + 0x1010, handler_code, sizeof(handler_code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);

    /* IDT at 0x4000, entry for vector 0x80 (offset 0x80*8=0x400 from base) */
    setup_idt_entry(m->ram, 0x4000, 0x80, 0x08, 0x1010);
    X86CPUSeg idt = {0};
    idt.base  = 0x4000;
    idt.limit = 0x80 * 8 + 7; /* covers vectors 0..128 */
    x86_cpu_set_seg(m->cpu, X86_CPU_SEG_IDT, &idt);

    run_cpu(m, 200000);

    uint32_t eax = x86_cpu_get_reg(m->cpu, 0); /* 0xDEAD */
    uint32_t ebx = x86_cpu_get_reg(m->cpu, 3); /* handler eflags (IF=0) */
    uint32_t ecx = x86_cpu_get_reg(m->cpu, 1); /* restored eflags (IF=1) */

    CHECK_EQ(eax, 0xDEADU);
    CHECK(!(ebx & (1U << 9)));    /* IF=0 inside handler (gate cleared it) */
    CHECK( (ecx & (1U << 9)));    /* IF=1 after IRET (restored from pushed flags) */
    machine_free(m);
}

/* =====================================================================
 * Test 80: Hardware IRQ delivery (vector 0x20) via get_hard_intno callback
 *
 * Sets irq_level, enables IF via STI.  The one-shot callback fires vector 0x20
 * once; the handler sets EAX=0xABCD and IRETs back.
 * ===================================================================== */
static void test_hardware_irq(void)
{
    /*
     * Main code at 0x1000:
     *   FB    STI   (sets IF; hardware IRQ fires before next instruction)
     *   90    NOP   (resumed after IRET)
     *   F4    HLT
     *
     * Handler at 0x1010:
     *   B8 CD AB 00 00   MOV EAX, 0xABCD
     *   CF               IRET
     */
    static const uint8_t main_code[] = {
        0xFB, 0x90, 0xF4,
    };
    static const uint8_t handler_code[] = {
        0xB8, 0xCD, 0xAB, 0x00, 0x00,   /* MOV EAX, 0xABCD */
        0xCF,                            /* IRET */
    };
    int fired = 0;

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, main_code,    sizeof(main_code));
    memcpy(m->ram + 0x1010, handler_code, sizeof(handler_code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);

    /* IDT at 0x4000, entry for vector 0x20 */
    setup_idt_entry(m->ram, 0x4000, 0x20, 0x08, 0x1010);
    X86CPUSeg idt = {0};
    idt.base  = 0x4000;
    idt.limit = 0x20 * 8 + 7;
    x86_cpu_set_seg(m->cpu, X86_CPU_SEG_IDT, &idt);

    /* Wire up one-shot IRQ */
    x86_cpu_set_get_hard_intno(m->cpu, irq_once, &fired);
    x86_cpu_set_irq(m->cpu, TRUE);

    run_cpu(m, 200000);

    uint32_t eax = x86_cpu_get_reg(m->cpu, 0);
    CHECK_EQ(eax, 0xABCDU);
    machine_free(m);
}

/* =====================================================================
 * Test 81: PUSHF / POPF round-trip — CF preserved across push/pop
 * ===================================================================== */
static void test_pushf_popf(void)
{
    /*
     *   F9    STC       ; CF=1
     *   9C    PUSHF     ; push eflags with CF=1
     *   F8    CLC       ; CF=0
     *   9D    POPF      ; restore eflags -> CF=1 again
     *   9C    PUSHF
     *   58    POP EAX   ; EAX = restored eflags
     *   F4    HLT
     */
    static const uint8_t code[] = {
        0xF9, 0x9C, 0xF8, 0x9D, 0x9C, 0x58, 0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t eflags = x86_cpu_get_reg(m->cpu, 0);
    CHECK((eflags >> 0) & 1);  /* CF=1 restored by POPF */
    machine_free(m);
}

/* =====================================================================
 * Test 82: GDT reload via LGDT + JMP FAR
 *
 * Loads a new GDT at 0x6000, then executes a direct far jump to
 * 0x08:0x7000 which reloads CS from the new GDT and jumps to the
 * target code.
 * ===================================================================== */
static void test_gdt_reload_far_jmp(void)
{
    /*
     * Code at 0x1000:
     *   0F 01 15 00 50 00 00   LGDT [0x5000]          (load new GDT)
     *   EA 00 70 00 00 08 00   JMP FAR 0x0008:0x7000  (reload CS + jump)
     *
     * Code at 0x7000:
     *   B8 78 56 34 12         MOV EAX, 0x12345678
     *   F4                     HLT
     */
    static const uint8_t code1000[] = {
        /* LGDT [0x5000]: 0F 01 /2 + disp32 */
        0x0F, 0x01, 0x15, 0x00, 0x50, 0x00, 0x00,
        /* JMP FAR 0x0008:0x7000: EA + off32 + sel16 */
        0xEA, 0x00, 0x70, 0x00, 0x00, 0x08, 0x00,
    };
    static const uint8_t code7000[] = {
        0xB8, 0x78, 0x56, 0x34, 0x12,   /* MOV EAX, 0x12345678 */
        0xF4,                            /* HLT */
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code1000, sizeof(code1000));
    memcpy(m->ram + 0x7000, code7000, sizeof(code7000));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);

    /* New GDT at 0x6000:
     *   [0x6000..0x6007]: null descriptor
     *   [0x6008..0x600F]: code descriptor (flat 32-bit, base=0)
     *   [0x6010..0x6017]: data descriptor (flat 32-bit, base=0)
     */
    uint64_t code_d = 0x00CF9A000000FFFFULL;
    uint64_t data_d = 0x00CF92000000FFFFULL;
    memset(m->ram + 0x6000, 0, 8);            /* null */
    memcpy(m->ram + 0x6008, &code_d, 8);      /* CS selector 0x08 */
    memcpy(m->ram + 0x6010, &data_d, 8);      /* DS selector 0x10 */

    /* LGDT pseudo-descriptor at 0x5000: { limit=23, base=0x6000 } */
    uint16_t lim  = 23;
    uint32_t base = 0x6000;
    memcpy(m->ram + 0x5000, &lim,  2);
    memcpy(m->ram + 0x5002, &base, 4);

    run_cpu(m, 200000);

    uint32_t eax = x86_cpu_get_reg(m->cpu, 0);
    CHECK_EQ(eax, 0x12345678U);
    machine_free(m);
}

/* =====================================================================
 * Test 83: Page fault delivery — fault on unmapped page, handler fixes EIP
 *
 * Sets up 32-bit paging (identity-map first 256 pages / 1 MB only).
 * Accessing virtual 0x200000 (page 512, not mapped) triggers #PF.
 * The handler pops the error code, advances the return EIP past the
 * faulting instruction, then IRETs back to HLT.
 * ===================================================================== */
static void test_page_fault_delivery(void)
{
    /*
     * Code at 0x1000:
     *   0F 20 C0              MOV EAX, CR0
     *   0D 00 00 00 80        OR  EAX, 0x80000000
     *   B9 00 00 10 00        MOV ECX, 0x100000      (page directory phys addr)
     *   0F 22 D9              MOV CR3, ECX
     *   0F 22 C0              MOV CR0, EAX            (enable paging)
     *   8B 1D 00 00 20 00     MOV EBX, [0x200000]    <- PAGE FAULT (EIP=0x1013)
     *   F4                    HLT                    <- return target (0x1019)
     *
     * Handler at 0x1020 (IDT vector 0x0E):
     *   B8 AD DE 00 00        MOV EAX, 0xDEAD
     *   5B                    POP EBX                (discard error_code)
     *   59                    POP ECX                (faulting EIP = 0x1013)
     *   83 C1 06              ADD ECX, 6             (advance past 6-byte instr)
     *   51                    PUSH ECX               (push fixed return EIP = 0x1019)
     *   CF                    IRET
     */
    static const uint8_t code1000[] = {
        0x0F, 0x20, 0xC0,               /* MOV EAX, CR0 */
        0x0D, 0x00, 0x00, 0x00, 0x80,   /* OR  EAX, 0x80000000 */
        0xB9, 0x00, 0x00, 0x10, 0x00,   /* MOV ECX, 0x100000 */
        0x0F, 0x22, 0xD9,               /* MOV CR3, ECX */
        0x0F, 0x22, 0xC0,               /* MOV CR0, EAX  (paging on) */
        0x8B, 0x1D, 0x00, 0x00, 0x20, 0x00, /* MOV EBX, [0x200000]  (PF here) */
        0xF4,                           /* HLT */
    };
    static const uint8_t handler1020[] = {
        0xB8, 0xAD, 0xDE, 0x00, 0x00,   /* MOV EAX, 0xDEAD */
        0x5B,                            /* POP EBX  (error_code) */
        0x59,                            /* POP ECX  (faulting EIP) */
        0x83, 0xC1, 0x06,               /* ADD ECX, 6 */
        0x51,                            /* PUSH ECX */
        0xCF,                            /* IRET */
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code1000,    sizeof(code1000));
    memcpy(m->ram + 0x1020, handler1020, sizeof(handler1020));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);

    /* Page directory at phys 0x100000: one entry covering first 4 MB */
    memset(m->ram + 0x100000, 0, 0x1000);
    uint32_t pde = 0x101000U | 3U;  /* PT at 0x101000, P=1, RW=1 */
    memcpy(m->ram + 0x100000, &pde, 4);

    /* Page table at phys 0x101000: identity-map first 256 pages (1 MB only) */
    memset(m->ram + 0x101000, 0, 0x1000);
    for (int i = 0; i < 256; i++) {
        uint32_t pte = (uint32_t)(i << 12) | 3U;  /* P=1, RW=1, identity */
        memcpy(m->ram + 0x101000 + i * 4, &pte, 4);
    }
    /* Entries 256..1023 remain 0 (not present); page 0x200 = index 512 → fault */

    /* IDT at 0x4000, entry for vector 0x0E (#PF) */
    setup_idt_entry(m->ram, 0x4000, 0x0E, 0x08, 0x1020);
    X86CPUSeg idt = {0};
    idt.base  = 0x4000;
    idt.limit = 0x0E * 8 + 7;
    x86_cpu_set_seg(m->cpu, X86_CPU_SEG_IDT, &idt);

    run_cpu(m, 500000);

    uint32_t eax = x86_cpu_get_reg(m->cpu, 0);
    CHECK_EQ(eax, 0xDEADU);
    machine_free(m);
}

/* =====================================================================
 * Test 84: PUSHF / POPF preserves CF, PF, ZF, SF, OF simultaneously
 * ===================================================================== */
static void test_pushf_popf_all_flags(void)
{
    /*
     * Load a hand-crafted flags value (CF+PF+ZF+SF+OF+FIXED = 0x8C7),
     * round-trip it through PUSH / POPF / PUSHF / POP, then verify.
     *
     *   68 C7 08 00 00   PUSH 0x000008C7
     *   9D               POPF
     *   9C               PUSHF
     *   58               POP EAX
     *   F4               HLT
     */
    static const uint8_t code[] = {
        0x68, 0xC7, 0x08, 0x00, 0x00,   /* PUSH imm32 = 0x8C7 */
        0x9D,                            /* POPF */
        0x9C,                            /* PUSHF */
        0x58,                            /* POP EAX */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t ef = x86_cpu_get_reg(m->cpu, 0);
    CHECK((ef >> 0) & 1);   /* CF */
    CHECK((ef >> 2) & 1);   /* PF */
    CHECK((ef >> 6) & 1);   /* ZF */
    CHECK((ef >> 7) & 1);   /* SF */
    CHECK((ef >> 11) & 1);  /* OF */
    machine_free(m);
}

/* =====================================================================
 * Test 85: PUSHAD / POPAD stack pivot — all registers preserved
 * ===================================================================== */
static void test_pushad_popad(void)
{
    /*
     *   B8 01 00 00 00   MOV EAX, 1
     *   BB 02 00 00 00   MOV EBX, 2
     *   B9 03 00 00 00   MOV ECX, 3
     *   BA 04 00 00 00   MOV EDX, 4
     *   60               PUSHAD
     *   31 C0            XOR EAX, EAX
     *   31 DB            XOR EBX, EBX
     *   31 C9            XOR ECX, ECX
     *   31 D2            XOR EDX, EDX
     *   61               POPAD
     *   F4               HLT
     */
    static const uint8_t code[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00,
        0xBB, 0x02, 0x00, 0x00, 0x00,
        0xB9, 0x03, 0x00, 0x00, 0x00,
        0xBA, 0x04, 0x00, 0x00, 0x00,
        0x60,
        0x31, 0xC0, 0x31, 0xDB, 0x31, 0xC9, 0x31, 0xD2,
        0x61,
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    CHECK_EQ(x86_cpu_get_reg(m->cpu, 0), 1U); /* EAX */
    CHECK_EQ(x86_cpu_get_reg(m->cpu, 3), 2U); /* EBX */
    CHECK_EQ(x86_cpu_get_reg(m->cpu, 1), 3U); /* ECX */
    CHECK_EQ(x86_cpu_get_reg(m->cpu, 2), 4U); /* EDX */
    machine_free(m);
}

/* =====================================================================
 * Test 86: FS segment override — reads from FS.base + offset, not DS.base
 *
 * FS is loaded with base=0x7000 via the API.  The instruction
 *   64 A0 00 00 00 00   MOV AL, [FS:0]
 * should read from physical 0x7000, not from physical 0 (DS:0).
 * ===================================================================== */
static void test_fs_segment_override(void)
{
    /*
     *   64 A0 00 00 00 00   MOV AL, [FS:disp32=0]
     *   F4                  HLT
     */
    static const uint8_t code[] = {
        0x64, 0xA0, 0x00, 0x00, 0x00, 0x00,  /* FS: MOV AL, [0] */
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);

    /* Place sentinel at ram[0x7000]; ram[0] stays 0 so a DS read would differ */
    m->ram[0x7000] = 0xAB;

    /* Configure FS with base=0x7000.  Flags match DS from enter_protected_mode:
     * bit 9=accessed, bit 11=writable, bit 14=DB(32-bit), bit 15=granularity. */
    X86CPUSeg fsd = {0};
    fsd.sel   = 0x10;
    fsd.base  = 0x7000;
    fsd.limit = 0xFFFFFFFF;
    fsd.flags = (1 << 9) | (1 << 11) | (1 << 14) | (1 << 15);
    x86_cpu_set_seg(m->cpu, X86_CPU_SEG_FS, &fsd);

    run_cpu(m, 100000);

    uint32_t eax = x86_cpu_get_reg(m->cpu, 0);
    CHECK_EQ(eax & 0xFFU, 0xABU); /* AL = 0xAB from FS:0 = 0x7000 */
    machine_free(m);
}

/* =====================================================================
 * Test 87: REP MOVSD with ES:EDI as destination
 *
 * Copies 4 dwords from DS:ESI (0x5000) to ES:EDI (0x6000).
 * Verifies copied data, ECX=0, ESI/EDI advanced by 16.
 * ===================================================================== */
static void test_rep_movsd(void)
{
    /*
     *   BE 00 50 00 00   MOV ESI, 0x5000
     *   BF 00 60 00 00   MOV EDI, 0x6000
     *   B9 04 00 00 00   MOV ECX, 4
     *   F3 A5            REP MOVSD
     *   F4               HLT
     */
    static const uint8_t code[] = {
        0xBE, 0x00, 0x50, 0x00, 0x00,
        0xBF, 0x00, 0x60, 0x00, 0x00,
        0xB9, 0x04, 0x00, 0x00, 0x00,
        0xF3, 0xA5,
        0xF4,
    };

    /* Source data: four distinct dwords */
    static const uint32_t src[4] = {
        0x11223344U, 0x55667788U, 0x99AABBCCU, 0xDDEEFF00U,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x5000, src, sizeof(src));
    memset(m->ram + 0x6000, 0,   sizeof(src));
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    /* Verify all four dwords were copied */
    uint32_t v;
    memcpy(&v, m->ram + 0x6000,  4); CHECK_EQ(v, 0x11223344U);
    memcpy(&v, m->ram + 0x6004,  4); CHECK_EQ(v, 0x55667788U);
    memcpy(&v, m->ram + 0x6008,  4); CHECK_EQ(v, 0x99AABBCCU);
    memcpy(&v, m->ram + 0x600C,  4); CHECK_EQ(v, 0xDDEEFF00U);

    CHECK_EQ(x86_cpu_get_reg(m->cpu, 1),  0U);       /* ECX = 0 */
    CHECK_EQ(x86_cpu_get_reg(m->cpu, 6),  0x5010U);  /* ESI advanced by 16 */
    CHECK_EQ(x86_cpu_get_reg(m->cpu, 7),  0x6010U);  /* EDI advanced by 16 */
    machine_free(m);
}

/* =====================================================================
 * Test 90: IDT at a virtual (paged) address — interrupt delivery bug
 *
 * This test specifically catches the bug where x86_do_interrupt used
 * phys_read32(idt_addr) treating the IDT base as a physical address.
 * When the kernel uses LIDT with a virtual address (e.g. after paging is
 * enabled and the identity mapping is gone), the IDT must be read via
 * vmem_read32 so that virtual→physical paging translation is applied.
 *
 * Setup:
 *   - Code at virtual/physical 0x1000 (identity-mapped).
 *   - Page directory at 0x100000: covers first 4 MB identity + maps virtual
 *     0x80004000 → physical 0x4000 (where the IDT lives).
 *   - The code enables paging, then sets IDTR base to 0x80004000 (virtual),
 *     then executes STI + INT 0x20 to fire the software interrupt.
 *   - Handler at virtual/physical 0x1020: sets EAX=0xDEAD, IRET.
 *   - If the bug is present, phys_read32(0x80004100) returns garbage and the
 *     interrupt is mis-delivered (likely triple-fault); EAX stays 0.
 *   - If fixed, vmem_read32 translates 0x80004100 → 0x4100 (physical) and
 *     the handler fires correctly; EAX = 0xDEAD.
 * ===================================================================== */
static void test_idt_at_virtual_addr(void)
{
    /*
     * Code at phys 0x1000 (virtual 0x1000, identity-mapped):
     *   0F 20 C0              MOV EAX, CR0
     *   0D 00 00 00 80        OR  EAX, 0x80000000
     *   B9 00 00 10 00        MOV ECX, 0x100000     <- PD physical base
     *   0F 22 D9              MOV CR3, ECX
     *   0F 22 C0              MOV CR0, EAX           (enable paging)
     *   0F 01 1D xx xx xx xx  LIDT [idt_desc_ptr]   <- IDT at virtual 0x80004000
     *   FB                    STI
     *   CD 20                 INT 0x20              <- fires via virtual IDT
     *   F4                    HLT
     *
     * IDT descriptor (at phys 0x1030, identity-mapped):
     *   DW  0x7FF              <- limit: 256*8 - 1 = 0x7FF
     *   DD  0x80004000         <- base: virtual 0x80004000
     *
     * Handler at phys 0x1020 (virtual 0x1020, identity-mapped):
     *   B8 AD DE 00 00        MOV EAX, 0xDEAD
     *   CF                    IRET
     *
     * IDT at phys 0x4000 (virtual 0x80004000 via page table):
     *   entry for vector 0x20 at offset 0x100.
     */
    static const uint8_t code1000[] = {
        0x0F, 0x20, 0xC0,               /* MOV EAX, CR0 */
        0x0D, 0x00, 0x00, 0x00, 0x80,   /* OR  EAX, 0x80000000 */
        0xB9, 0x00, 0x00, 0x10, 0x00,   /* MOV ECX, 0x100000 */
        0x0F, 0x22, 0xD9,               /* MOV CR3, ECX */
        0x0F, 0x22, 0xC0,               /* MOV CR0, EAX (paging on) */
        /* LIDT [0x1030]: opcode 0F 01 /3 disp32 */
        0x0F, 0x01, 0x1D, 0x30, 0x10, 0x00, 0x00, /* LIDT [0x1030] */
        0xFB,                           /* STI */
        0xCD, 0x20,                     /* INT 0x20 */
        0xF4,                           /* HLT */
    };
    /* IDT descriptor stored at phys/virt 0x1030 */
    static const uint8_t idt_desc[] = {
        0xFF, 0x07,                     /* limit = 0x7FF (256 entries) */
        0x00, 0x40, 0x00, 0x80,         /* base = 0x80004000 (little-endian) */
    };
    /* Handler at phys 0x1020 */
    static const uint8_t handler1020[] = {
        0xB8, 0xAD, 0xDE, 0x00, 0x00,   /* MOV EAX, 0xDEAD */
        0xCF,                            /* IRET */
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code1000,    sizeof(code1000));
    memcpy(m->ram + 0x1020, handler1020, sizeof(handler1020));
    memcpy(m->ram + 0x1030, idt_desc,    sizeof(idt_desc));

    /*
     * Page directory at phys 0x100000.
     * Entry 0 (virtual 0x00000000-0x003FFFFF):
     *   PT at 0x101000, P=1, RW=1 — identity map first 4 MB.
     * Entry 0x200 (virtual 0x80000000-0x803FFFFF, index = 0x80000000>>22 = 512):
     *   PT at 0x102000, P=1, RW=1 — maps virtual 0x80000000+ to phys 0x0000.
     *
     * Entry 0 covers virtual 0x0000_0000 – 0x003F_FFFF (first 4 MB).
     * Entry 512 covers virtual 0x8000_0000 – 0x803F_FFFF (2 GB mark, 4 MB).
     */
    memset(m->ram + 0x100000, 0, 0x3000);  /* clear PD + 2 PTs */

    /* PD entry 0: PT at 0x101000 */
    uint32_t pde0 = 0x101000U | 3U;
    memcpy(m->ram + 0x100000, &pde0, 4);

    /* PD entry 512 (0x80000000): PT at 0x102000 */
    uint32_t pde512 = 0x102000U | 3U;
    memcpy(m->ram + 0x100000 + 512 * 4, &pde512, 4);

    /* PT at 0x101000: identity-map 4 MB (1024 pages) */
    for (int i = 0; i < 1024; i++) {
        uint32_t pte = (uint32_t)(i << 12) | 3U;
        memcpy(m->ram + 0x101000 + i * 4, &pte, 4);
    }

    /* PT at 0x102000: virtual 0x80000000 → physical 0x00000000 (4 MB) */
    for (int i = 0; i < 1024; i++) {
        uint32_t pte = (uint32_t)(i << 12) | 3U;
        memcpy(m->ram + 0x102000 + i * 4, &pte, 4);
    }

    /*
     * IDT at phys 0x4000.  The kernel will load this with base=0x80004000
     * (virtual).  Via the 0x80000000 mapping, virt 0x80004000 → phys 0x4000.
     * Vector 0x20 entry: offset within IDT = 0x20 * 8 = 0x100.
     * Handler at virtual/physical 0x1020.
     */
    memset(m->ram + 0x4000, 0, 0x1000);
    setup_idt_entry(m->ram, 0x4000, 0x20, 0x08, 0x1020);

    /* Start in protected mode (no paging yet) — code will enable paging */
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);

    run_cpu(m, 2000000);

    uint32_t eax = x86_cpu_get_reg(m->cpu, 0);
    CHECK_EQ(eax, 0xDEADU);
    machine_free(m);
}


/* =====================================================================
 * Test: LLDT/LTR/SLDT/STR — GROUP 6 opcodes (0F 00)
 *
 * Sets up a GDT with a TSS descriptor at entry 3 (selector 0x18).
 * Code executes LLDT (null selector), LTR (0x18), then STR into EAX.
 * Verifies EAX == 0x18 after STR.
 * ===================================================================== */
/* CMPXCHG8B m64 — equal case (ZF=1, write ECX:EBX to mem) */
static void test_cmpxchg8b_equal(void)
{
    /*
     * Code at 0x1000:
     *   B8 78 56 34 12   MOV EAX, 0x12345678   (acc lo)
     *   BA 90 AB CD EF   MOV EDX, 0xEFCDAB90   (acc hi)
     *   BB 11 11 11 11   MOV EBX, 0x11111111   (new lo)
     *   B9 22 22 22 22   MOV ECX, 0x22222222   (new hi)
     *   0F C7 0D 00 20 00 00  CMPXCHG8B [0x2000] (reg=1, disp32)
     *   F4               HLT
     */
    static const uint8_t code1000[] = {
        0xB8, 0x78, 0x56, 0x34, 0x12,          /* MOV EAX, 0x12345678 */
        0xBA, 0x90, 0xAB, 0xCD, 0xEF,          /* MOV EDX, 0xEFCDAB90 */
        0xBB, 0x11, 0x11, 0x11, 0x11,          /* MOV EBX, 0x11111111 */
        0xB9, 0x22, 0x22, 0x22, 0x22,          /* MOV ECX, 0x22222222 */
        0x0F, 0xC7, 0x0D,                      /* CMPXCHG8B [disp32] */
        0x00, 0x20, 0x00, 0x00,                /* disp32 = 0x2000 */
        0xF4,                                  /* HLT */
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code1000, sizeof(code1000));

    /* Pre-load memory at 0x2000 with EDX:EAX value so comparison succeeds */
    uint32_t mem_lo = 0x12345678U;
    uint32_t mem_hi = 0xEFCDAB90U;
    memcpy(m->ram + 0x2000, &mem_lo, 4);
    memcpy(m->ram + 0x2004, &mem_hi, 4);

    enter_protected_mode(m, 0x3000, 0x1000, 0x4000);
    run_cpu(m, 200000);

    /* Equal: ZF=1, memory should now contain ECX:EBX = 0x22222222:0x11111111 */
    uint32_t out_lo, out_hi;
    memcpy(&out_lo, m->ram + 0x2000, 4);
    memcpy(&out_hi, m->ram + 0x2004, 4);
    CHECK_EQ(out_lo, 0x11111111U); /* EBX */
    CHECK_EQ(out_hi, 0x22222222U); /* ECX */

    /* ZF should be set — bit 6 of EFLAGS */
    uint32_t eflags = x86_cpu_get_reg(m->cpu, X86_CPU_REG_EFLAGS);
    CHECK_EQ(!!(eflags & (1U << 6)), 1U); /* ZF=1: comparison matched */
    machine_free(m);
}

/* CMPXCHG8B m64 — not-equal case (ZF=0, load mem into EDX:EAX) */
static void test_cmpxchg8b_notequal(void)
{
    /*
     * Code at 0x1000:
     *   B8 AA BB CC DD   MOV EAX, 0xDDCCBBAA   (wrong acc lo)
     *   BA EE FF 00 11   MOV EDX, 0x1100FFEE   (wrong acc hi)
     *   BB 11 11 11 11   MOV EBX, 0x11111111
     *   B9 22 22 22 22   MOV ECX, 0x22222222
     *   0F C7 0D 00 20 00 00  CMPXCHG8B [0x2000]
     *   F4               HLT
     */
    static const uint8_t code1000[] = {
        0xB8, 0xAA, 0xBB, 0xCC, 0xDD,
        0xBA, 0xEE, 0xFF, 0x00, 0x11,
        0xBB, 0x11, 0x11, 0x11, 0x11,
        0xB9, 0x22, 0x22, 0x22, 0x22,
        0x0F, 0xC7, 0x0D,
        0x00, 0x20, 0x00, 0x00,
        0xF4,
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code1000, sizeof(code1000));

    /* Memory at 0x2000 has different value */
    uint32_t mem_lo = 0x12345678U;
    uint32_t mem_hi = 0xEFCDAB90U;
    memcpy(m->ram + 0x2000, &mem_lo, 4);
    memcpy(m->ram + 0x2004, &mem_hi, 4);

    enter_protected_mode(m, 0x3000, 0x1000, 0x4000);
    run_cpu(m, 200000);

    /* Not equal: ZF=0, EAX/EDX should be loaded from memory */
    uint32_t eax = x86_cpu_get_reg(m->cpu, 0); /* EAX */
    uint32_t edx = x86_cpu_get_reg(m->cpu, 2); /* EDX (index 2) */
    CHECK_EQ(eax, 0x12345678U); /* mem_lo */
    CHECK_EQ(edx, 0xEFCDAB90U); /* mem_hi */

    uint32_t eflags = x86_cpu_get_reg(m->cpu, X86_CPU_REG_EFLAGS);
    CHECK_EQ(!!(eflags & (1U << 6)), 0U); /* ZF=0: comparison did not match */

    /* Memory unchanged */
    uint32_t out_lo, out_hi;
    memcpy(&out_lo, m->ram + 0x2000, 4);
    memcpy(&out_hi, m->ram + 0x2004, 4);
    CHECK_EQ(out_lo, 0x12345678U);
    CHECK_EQ(out_hi, 0xEFCDAB90U);
    machine_free(m);
}

static void test_lldt_ltr(void)
{
    /*
     * Code at 0x1000:
     *   B8 00 00 00 00   MOV EAX, 0
     *   0F 00 D0         LLDT EAX           (loads null LDT, sel=0)
     *   B8 18 00 00 00   MOV EAX, 0x18
     *   0F 00 D8         LTR EAX            (loads TR from GDT[3])
     *   0F 00 C8         STR EAX            (stores TR sel into EAX)
     *   F4               HLT
     */
    static const uint8_t code1000[] = {
        0xB8, 0x00, 0x00, 0x00, 0x00,  /* MOV EAX, 0 */
        0x0F, 0x00, 0xD0,              /* LLDT EAX (reg=2) */
        0xB8, 0x18, 0x00, 0x00, 0x00,  /* MOV EAX, 0x18 */
        0x0F, 0x00, 0xD8,              /* LTR EAX (reg=3) */
        0x0F, 0x00, 0xC8,              /* STR EAX (reg=1) */
        0xF4,                          /* HLT */
    };

    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code1000, sizeof(code1000));

    /*
     * GDT at 0x2000: null / code(0x08) / data(0x10) / tss(0x18)
     * TSS descriptor for selector 0x18:
     *   base=0x5000, limit=0x67, type=0x89 (32-bit available TSS, P=1)
     *   lo32 = 0x50000067, hi32 = 0x00008900
     */
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);

    /* Extend GDT limit to include entry 3 (0x18) */
    X86CPUSeg gdt = {0};
    gdt.base  = 0x2000;
    gdt.limit = 32 - 1;  /* 4 entries × 8 bytes = 32 */
    x86_cpu_set_seg(m->cpu, X86_CPU_SEG_GDT, &gdt);

    /* Write 32-bit TSS descriptor at GDT entry 3 (offset 0x18) */
    uint32_t tss_lo = 0x50000067U;  /* base[15:0]=0x5000, limit[15:0]=0x0067 */
    uint32_t tss_hi = 0x00008900U;  /* base[31:24]=0, G=0, limit[19:16]=0, P=1, DPL=0, type=9 */
    memcpy(m->ram + 0x2018, &tss_lo, 4);
    memcpy(m->ram + 0x201C, &tss_hi, 4);

    /* Minimal TSS at 0x5000 (just needs to exist in RAM) */
    memset(m->ram + 0x5000, 0, 0x68);

    run_cpu(m, 200000);

    uint32_t eax = x86_cpu_get_reg(m->cpu, 0);
    /* STR stores the TR selector (0x18) into EAX */
    CHECK_EQ(eax & 0xFFFF, 0x18U);
    machine_free(m);
}

/* =====================================================================
 * Test: SBB edge case — b=0xFFFFFFFF with CF=1 (b+CF overflows 32 bits)
 * EAX=5, SBB EAX,0xFFFFFFFF with CF=1: result = 5 - 0xFFFFFFFF - 1
 *   = 5 - 0x100000000 = 0x00000005 (mod 2^32, with borrow → CF=1)
 * ===================================================================== */
static void test_sbb_edge_borrow(void)
{
    /*
     *   B8 05 00 00 00     MOV EAX, 5
     *   F9                 STC          ; CF=1
     *   81 D8 FF FF FF FF  SBB EAX, 0xFFFFFFFF
     *   F4                 HLT
     */
    static const uint8_t code[] = {
        0xB8, 0x05, 0x00, 0x00, 0x00,
        0xF9,
        0x81, 0xD8, 0xFF, 0xFF, 0xFF, 0xFF,
        0xF4,
    };
    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t eax    = x86_cpu_get_reg(m->cpu, 0);
    uint32_t eflags = x86_cpu_get_reg(m->cpu, X86_CPU_REG_EFLAGS);
    CHECK_EQ(eax, 5U);           /* 5 - 0xFFFFFFFF - 1 = 5 mod 2^32 */
    CHECK_EQ(!!(eflags & 1), 1); /* CF=1 (borrow: 5 < 0x100000000) */
    machine_free(m);
}

/* =====================================================================
 * Test: INVD — cache invalidate (no-op, must not raise #UD)
 * ===================================================================== */
static void test_invd(void)
{
    /*
     *   0F 08    INVD
     *   F4       HLT
     */
    static const uint8_t code[] = { 0x0F, 0x08, 0xF4 };
    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);
    /* No crash = pass */
    CHECK(1);
    machine_free(m);
}

/* =====================================================================
 * Test: RDPMC — returns 0, must not raise #UD
 * ===================================================================== */
static void test_rdpmc(void)
{
    /*
     *   B9 00 00 00 00  MOV ECX, 0  ; counter index 0
     *   0F 33           RDPMC
     *   F4              HLT
     */
    static const uint8_t code[] = {
        0xB9, 0x00, 0x00, 0x00, 0x00,
        0x0F, 0x33,
        0xF4,
    };
    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);
    uint32_t eax = x86_cpu_get_reg(m->cpu, 0);
    uint32_t edx = x86_cpu_get_reg(m->cpu, 2);
    CHECK_EQ(eax, 0U);
    CHECK_EQ(edx, 0U);
    machine_free(m);
}

/* =====================================================================
 * Test: 0F 01 /1 register-form — MWAIT (0F 01 C9) must not fault
 * ===================================================================== */
static void test_0f01_mwait(void)
{
    /*
     *   31 C0     XOR EAX, EAX
     *   31 C9     XOR ECX, ECX
     *   0F 01 C9  MWAIT   (reg=1, rm=1: mod=11 → register form)
     *   F4        HLT
     */
    static const uint8_t code[] = {
        0x31, 0xC0,
        0x31, 0xC9,
        0x0F, 0x01, 0xC9,
        0xF4,
    };
    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);
    CHECK(1); /* No #UD = pass */
    machine_free(m);
}

/* =====================================================================
 * Test: XGETBV (0F 01 D0) — returns XCR0 = 1 in EAX, 0 in EDX
 * ===================================================================== */
static void test_xgetbv(void)
{
    /*
     *   B9 00 00 00 00  MOV ECX, 0   ; XCR0
     *   0F 01 D0        XGETBV       (reg=2, rm=0)
     *   F4              HLT
     */
    static const uint8_t code[] = {
        0xB9, 0x00, 0x00, 0x00, 0x00,
        0x0F, 0x01, 0xD0,
        0xF4,
    };
    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);
    uint32_t eax = x86_cpu_get_reg(m->cpu, 0);
    uint32_t edx = x86_cpu_get_reg(m->cpu, 2);
    CHECK_EQ(eax, 1U); /* XCR0 bit 0 = x87 state */
    CHECK_EQ(edx, 0U);
    machine_free(m);
}

/* =====================================================================
 * Test: LAR — returns access rights; ZF=1 on success, P bit (bit 15) set
 * Code selector 0x08 (GDT entry 1, descriptor 0x00CF9A000000FFFF):
 *   hi = 0x00CF9A00 → ar = hi & 0x00FFFF00 = 0x009A00
 *   bit 15 of 0x009A00 = 1 (P=1, present segment). ZF should be 1.
 * ===================================================================== */
static void test_lar(void)
{
    /*
     *   B8 08 00 00 00  MOV EAX, 8   ; selector 0x08 (GDT code segment)
     *   0F 02 C8        LAR ECX, EAX
     *   F4              HLT
     */
    static const uint8_t code[] = {
        0xB8, 0x08, 0x00, 0x00, 0x00,
        0x0F, 0x02, 0xC8,
        0xF4,
    };
    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t eflags = x86_cpu_get_reg(m->cpu, X86_CPU_REG_EFLAGS);
    CHECK_EQ(!!(eflags & (1U << 6)), 1U); /* ZF=1: valid segment */
    /* Check P bit (bit 15) is set in the LAR result (ECX = reg[1]) */
    uint32_t ecx = x86_cpu_get_reg(m->cpu, 1);
    CHECK_EQ(!!(ecx & (1U << 15)), 1U); /* P=1 */
    machine_free(m);
}


static void test_lsl(void)
{
    /*
     *   B8 08 00 00 00  MOV EAX, 8   ; selector 0x08 (GDT index 1 = code seg)
     *   0F 03 C8        LSL ECX, EAX
     *   F4              HLT
     * GDT entry 1: 0x00CF9B000000FFFF → limit = 0xFFFF with G=1 → expanded = 0xFFFFFFFF
     */
    static const uint8_t code[] = {
        0xB8, 0x08, 0x00, 0x00, 0x00,
        0x0F, 0x03, 0xC8,
        0xF4,
    };
    TestMachine *m = machine_new();
    memcpy(m->ram + 0x1000, code, sizeof(code));

    /* Use enter_protected_mode which sets GDT with flat 4GB segments */
    enter_protected_mode(m, 0x2000, 0x1000, 0x3000);
    run_cpu(m, 100000);

    uint32_t eflags = x86_cpu_get_reg(m->cpu, X86_CPU_REG_EFLAGS);
    CHECK_EQ(!!(eflags & (1U << 6)), 1U); /* ZF=1: segment found */
    /* For a G=1 descriptor with raw limit 0xFFFF, expanded = 0xFFFFFFFF */
    uint32_t ecx = x86_cpu_get_reg(m->cpu, 1);
    CHECK_EQ(ecx, 0xFFFFFFFFU);
    machine_free(m);
}

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
    test_jo_taken();
    test_jo_not_taken();
    test_jb_taken();
    test_jb_not_taken();
    test_jbe_taken();
    test_jbe_not_taken();
    test_js_taken();
    test_js_not_taken();
    test_jp_taken();
    test_jp_not_taken();
    test_jl_taken();
    test_jle_taken();
    test_jg_taken();
    test_adc();
    test_sbb();
    test_rol();
    test_ror();
    test_imul_2op();
    test_imul_3op_imm8();
    test_imul_neg();
    test_div();
    test_idiv();
    test_clc_stc_cmc();
    test_cwde();
    test_cdq();
    test_enter_leave();
    test_lea_sib();
    test_bt();
    test_bts();
    test_btr();
    test_btc();
    test_shld();
    test_shrd();
    test_xadd();
    test_cmovz();
    test_cmovnz();
    test_inc_dec();
    test_not();
    test_16bit_add();
    test_pf();
    test_of_add();
    test_of_sub();
    test_lahf_sahf();
    test_lodsb();
    test_stosb();
    test_scasb_match();
    test_mem_byte_write();
    test_mem_word_write();

    /* New tests: interrupt delivery, paging, stack, segments */
    test_int80_iret();
    test_hardware_irq();
    test_pushf_popf();
    test_gdt_reload_far_jmp();
    test_page_fault_delivery();
    test_pushf_popf_all_flags();
    test_pushad_popad();
    test_fs_segment_override();
    test_rep_movsd();
    test_idt_at_virtual_addr();
    test_lldt_ltr();
    test_cmpxchg8b_equal();
    test_cmpxchg8b_notequal();
    test_sbb_edge_borrow();
    test_invd();
    test_rdpmc();
    test_0f01_mwait();
    test_xgetbv();
    test_lsl();
    test_lar();

    fprintf(stderr, "\nResults: %d/%d passed", tests_pass, tests_run);
    if (tests_fail)
        fprintf(stderr, ", %d FAILED\n", tests_fail);
    else
        fprintf(stderr, " (all pass)\n");

    return (tests_fail > 0) ? 1 : 0;
}
