/*
 * Deterministic, network-free microbenchmark for the x86 interpreter hot path.
 *
 * Assembles a tight loop of the dominant scalar mix (reg-reg add, inc, a
 * memory load + store, dec, backward jcc) and times a single interp run.
 * A proxy for the awk integer/branch workload used for boot benchmarking,
 * but reproducible and independent of the flaky remote 9p rootfs.
 *
 *   make -f ... (compiled ad hoc against x86_cpu.o iomem.o cutils.o)
 *   ./microbench [iterations]
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
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

int main(int argc, char **argv)
{
    uint32_t N = (argc > 1) ? (uint32_t)strtoul(argv[1], NULL, 0) : 150000000u;
    PhysMemoryMap *map = mk_map(0x10000);
    X86CPUState *s = mk_cpu(map, 0x8000);
    uint8_t *p = g_ram;
    int o = 0;

    /* setup */
    p[o++]=0xBD; *(uint32_t*)(p+o)=0x2000; o+=4;   /* mov ebp,0x2000 */
    p[o++]=0xBE; *(uint32_t*)(p+o)=0;      o+=4;    /* mov esi,0      */
    p[o++]=0xBF; *(uint32_t*)(p+o)=0;      o+=4;    /* mov edi,0      */
    p[o++]=0xB9; *(uint32_t*)(p+o)=N;      o+=4;    /* mov ecx,N      */

    int loop = o;                                   /* loop:          */
    p[o++]=0x01; p[o++]=0xFE;                       /* add esi,edi    */
    p[o++]=0x47;                                    /* inc edi        */
    p[o++]=0x8B; p[o++]=0x45; p[o++]=0x00;          /* mov eax,[ebp]  */
    p[o++]=0x01; p[o++]=0xF8;                       /* add eax,edi    */
    p[o++]=0x89; p[o++]=0x45; p[o++]=0x00;          /* mov [ebp],eax  */
    p[o++]=0x49;                                    /* dec ecx        */
    p[o++]=0x75;                                    /* jnz rel8       */
    int disp = loop - (o + 1);                      /* target - next  */
    p[o++]=(uint8_t)disp;
    p[o++]=0xF4;                                    /* hlt            */

    int body = 7;                                   /* instrs per iter */
    uint64_t expect = (uint64_t)N * body;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    /* one call; hlt stops it well before the 2e9 cycle cap */
    x86_cpu_interp(s, 2000000000);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
    uint32_t esi = x86_cpu_get_reg(s, 6);
    uint32_t ecx = x86_cpu_get_reg(s, 1);
    printf("N=%u body=%d  ~%llu instrs  esi=%u ecx=%u  %.3f s  %.1f Minstr/s\n",
           N, body, (unsigned long long)expect, esi, ecx, secs,
           expect / secs / 1e6);
    x86_cpu_end(s);
    return 0;
}
