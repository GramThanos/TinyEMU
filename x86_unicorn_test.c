#include <stdio.h>
#include <string.h>

#include "cutils.h"
#include "iomem.h"
#include "x86_cpu.h"

static uint8_t ram[0x3000] __attribute__((aligned(4096)));

static int run_nop_progress_test(void)
{
    PhysMemoryMap *map;
    X86CPUState *cpu;
    uint32_t start = 0x1000;
    uint32_t eip;

    memset(ram, 0, sizeof(ram));
    memset(ram + start, 0x90, 64); /* NOP sled */

    map = phys_mem_map_init();
    if (!map)
        return -1;
    if (!cpu_register_ram(map, 0, sizeof(ram), 0)) {
        fprintf(stderr, "nop_progress: cpu_register_ram failed\n");
        return -1;
    }

    cpu = x86_cpu_init(map);
    if (!cpu) {
        fprintf(stderr, "nop_progress: x86_cpu_init failed\n");
        return -1;
    }

    x86_cpu_set_reg(cpu, X86_CPU_REG_EIP, start);
    x86_cpu_interp(cpu, 8);
    eip = x86_cpu_get_reg(cpu, X86_CPU_REG_EIP);

    x86_cpu_end(cpu);
    phys_mem_map_end(map);

    if (eip <= start) {
        fprintf(stderr, "nop_progress: EIP did not advance (start=%u, eip=%u)\n",
                start, eip);
        return -1;
    }
    return 0;
}

static int run_fallback_instruction_progress_test(void)
{
    PhysMemoryMap *map;
    X86CPUState *cpu;
    uint32_t start = 0x1200;
    uint32_t eip;

    /*
     * ENDBR32       (F3 0F 1E FB)
     * RDTSC         (0F 31)
     * CLAC          (0F 01 CA)
     * STAC          (0F 01 CB)
     * RDTSCP        (0F 01 F9)
     * NOP           (90)
     */
    const uint8_t code[] = {
        0xF3, 0x0F, 0x1E, 0xFB,
        0x0F, 0x31,
        0x0F, 0x01, 0xCA,
        0x0F, 0x01, 0xCB,
        0x0F, 0x01, 0xF9,
        0x90
    };

    memset(ram, 0, sizeof(ram));
    memcpy(ram + start, code, sizeof(code));

    map = phys_mem_map_init();
    if (!map)
        return -1;
    if (!cpu_register_ram(map, 0, sizeof(ram), 0)) {
        fprintf(stderr, "fallback_progress: cpu_register_ram failed\n");
        return -1;
    }

    cpu = x86_cpu_init(map);
    if (!cpu) {
        fprintf(stderr, "fallback_progress: x86_cpu_init failed\n");
        return -1;
    }

    x86_cpu_set_reg(cpu, X86_CPU_REG_EIP, start);
    x86_cpu_interp(cpu, 16);
    eip = x86_cpu_get_reg(cpu, X86_CPU_REG_EIP);

    x86_cpu_end(cpu);
    phys_mem_map_end(map);

    if (eip < start + 15) {
        fprintf(stderr, "fallback_progress: EIP too small (eip=%u)\n", eip);
        return -1;
    }
    return 0;
}

int main(void)
{
    if (run_nop_progress_test() != 0)
        return 1;
    if (run_fallback_instruction_progress_test() != 0)
        return 1;
    printf("x86_unicorn_test: ok\n");
    return 0;
}
