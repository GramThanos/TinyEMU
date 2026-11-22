/*
 * x86 -> WebAssembly block JIT (browser / Emscripten builds only)
 *
 * MIT license; see LICENSE. Original code, written for this project: not
 * derived from cpux86.js (JSLinux) or any other emulator.
 *
 * The interpreter (x86_cpu.c) stays the reference and the fallback. When it
 * reaches a control-transfer target (the point where it would rebuild its
 * fetch window), it asks x86_jit_run() first. A target seen often enough has
 * its code page compiled: every known block of the page (a straight run of
 * instructions up to a branch or an instruction this compiler doesn't
 * handle), from the hot entry points and whatever they branch to inside the
 * page, becomes one WebAssembly function. It runs those instructions
 * directly against the X86CPUState in WASM memory, dispatches between the
 * page's blocks with a br_table, so branches inside the page never leave
 * WASM, and returns how many instructions it ran. Anything it does not
 * handle ends a block, and the interpreter runs that one instruction and
 * comes back (jit_poke), so coverage only ever affects speed.
 *
 * Correctness rules the generated code follows -- the same as the
 * interpreter's, see AGENTS.md:
 *  - Restartable faults. Loads and stores check the software TLB inline and
 *    call back into C (x86_jit_ld / x86_jit_st) on a miss; those may raise a
 *    #PF, which longjmps (a Wasm exception) straight through the block's frame
 *    to the setjmp landing in x86_cpu_interp. Before such a call the block
 *    stores the instruction's EIP in cur_eip, and every instruction does its
 *    faulting reads, and the store, before committing anything else.
 *  - Deferred flags. Flag producers write the same cc_* record alu() and
 *    alu_incdec() write, so the interpreter can pick up mid-stream. Conditions
 *    are derived inline when the producer is known at compile time, and by
 *    calling test_cc() otherwise.
 *  - Self-modifying code and DMA. A physical page holding compiled code is
 *    marked in x86_jit_code_bits. The write TLB never caches a marked page, so
 *    every CPU write to it reaches phys_write(), and device DMA reaches
 *    x86_jit_note_write() through the memory map; either drops the page's
 *    blocks. A block whose store dropped code exits after that instruction.
 *
 * Compiled modules are JS objects held in the Emscripten function table, so
 * they are not part of a WASM-memory snapshot: vm_restore() calls
 * x86_jit_forget(), which drops the block table without freeing (the table
 * indices it holds belong to the JS instance that saved the snapshot).
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>

#include "cutils.h"
#include "iomem.h"
#include "x86_cpu.h"
#include "x86_cpu_priv.h"

#ifdef X86_JIT

#include <emscripten.h>

/* Provided by wasm/x86jit.js. */
extern int  x86_jit_js_init(void);
/* nonzero when the browser's WebAssembly has tail calls (return_call_indirect) */
extern int  x86_jit_js_tailcalls(void);
/* Compile a module of n block functions; their table indices go to idx[]
 * (0 = failed). sync: before returning. Otherwise later, followed by a call
 * to x86_jit_installed(slot, epoch). Returns 0 if it could not even start. */
extern int  x86_jit_js_compile(const uint8_t *code, int len, int n, uint32_t *idx,
                               int slot, uint32_t epoch, int sync);
extern void x86_jit_js_free(int idx);

/* ---- tuning ------------------------------------------------------------ */

/* Code is compiled a page at a time: one WASM function per guest code page
 * holds every block of it that is known (hot entry points, and what they
 * branch to inside the page), so branches within the page stay in WASM.
 * Pages live in a 4-way set-associative table; hotness is counted apart, in
 * a tagless array, so cold targets never evict compiled code (an earlier
 * direct-mapped table that held the counters had ~90% of a boot's compiles
 * be recompiles of evicted blocks). */
#define PG_SET_BITS     10
#define PG_SETS         (1 << PG_SET_BITS)
#define PG_WAYS         4
#define PG_TAB_SIZE     (PG_SETS * PG_WAYS)
#define PG_MAX_BLOCKS   256     /* blocks in one page function */
#define PG_MAX_WANT     32      /* hot offsets waiting for the next compile */
#define PG_MAX_BAD      16      /* offsets known not to start a block */
#define PG_MAX_INSNS    3000    /* instructions in one page function (each once:
                                   a block ends where the next one starts) */
#define PG_MAX_COMPILES 32      /* compiles of one page before it stops growing */
#define JIT_HOT_BITS    16
#define JIT_PAGE_HASH   4096
#define JIT_MAX_INSNS   64      /* instructions in one block */
#define JIT_LOOP_LIMIT  20000   /* instructions one call may run */
#define JIT_RECOMPILE_HOT 4     /* x threshold: a new entry recompiling a page */
#define JIT_MAX_INVAL   4       /* invalidations before a page is given up */

/* Executions of a branch target before its page is compiled. Build the
 * selftest with -DX86_JIT_THRESHOLD=1 to send every block through. */
#ifndef X86_JIT_THRESHOLD
#define X86_JIT_THRESHOLD 40
#endif
uint32_t x86_jit_threshold = X86_JIT_THRESHOLD;

/* Statistics, for benchmarks. */
uint32_t x86_jit_stat_compiled;     /* page functions installed */
uint32_t x86_jit_stat_blocks;       /* blocks in them */
uint32_t x86_jit_stat_failed, x86_jit_stat_invalidated, x86_jit_stat_evicted;

typedef struct {
    uint32_t lpage;     /* a linear base the page was seen at (statistics only:
                           code is keyed by frame, and runs at any address) */
    uint32_t hbase;     /* host address of its first byte: identifies the frame */
    uint32_t fn;        /* function-table index; 0 = none (now) */
    uint32_t ppage;     /* physical page, while linked for invalidation */
    uint32_t pnext;     /* next linked page in the bucket, index+1 */
    uint32_t gen;       /* bumped whenever the code is dropped: a batch result
                           for an older generation is discarded */
    uint8_t  mi;        /* MMU index the code was compiled for */
    uint8_t  used, linked;
    uint8_t  pending;   /* queued or compiling */
    uint8_t  bad;       /* invalidated too often: left to the interpreter */
    uint8_t  inval, compiles;
    uint8_t  nwant, nbad;
    uint16_t nblocks;
    uint16_t blocks[PG_MAX_BLOCKS];     /* offsets the function has, sorted */
    uint16_t want[PG_MAX_WANT];         /* hot offsets for the next compile */
    uint16_t badofs[PG_MAX_BAD];        /* offsets that cannot start a block */
} JitPage;

static JitPage  jit_tab[PG_TAB_SIZE];
static uint8_t  jit_rr[PG_SETS];                /* round-robin victim per set */
static uint16_t jit_hot[1 << JIT_HOT_BITS];     /* entries seen, by target hash */
static uint32_t jit_page_head[JIT_PAGE_HASH];   /* index+1 into jit_tab */
uint32_t x86_jit_code_bits[1u << 15];           /* one bit per 4 KB phys page */
static int jit_state;                           /* 0 untried, 1 on, 2 off */

/* ---- WebAssembly encoding ---------------------------------------------- */

typedef struct {
    uint8_t *p;
    int n, cap;
} Buf;

static void b_byte(Buf *b, int v)
{
    if (b->n >= b->cap) {
        b->cap = b->cap ? b->cap * 2 : 1024;
        b->p = realloc(b->p, b->cap);
    }
    b->p[b->n++] = (uint8_t)v;
}
static void b_uleb(Buf *b, uint32_t v)
{
    do {
        uint8_t c = v & 0x7f;
        v >>= 7;
        if (v) c |= 0x80;
        b_byte(b, c);
    } while (v);
}
static void b_sleb(Buf *b, int32_t v)
{
    for (;;) {
        uint8_t c = v & 0x7f;
        v >>= 7;   /* arithmetic on every compiler this builds with */
        if ((v == 0 && !(c & 0x40)) || (v == -1 && (c & 0x40))) {
            b_byte(b, c);
            return;
        }
        b_byte(b, c | 0x80);
    }
}
static void b_bytes(Buf *b, const void *p, int n)
{
    int i;
    for (i = 0; i < n; i++) b_byte(b, ((const uint8_t *)p)[i]);
}
static void b_name(Buf *b, const char *s)
{
    int n = (int)strlen(s);
    b_uleb(b, n);
    b_bytes(b, s, n);
}

/* opcodes */
enum {
    W_UNREACHABLE = 0x00, W_BLOCK = 0x02, W_LOOP = 0x03, W_IF = 0x04,
    W_ELSE = 0x05, W_END = 0x0b, W_BR = 0x0c, W_BR_IF = 0x0d, W_BR_TABLE = 0x0e,
    W_RETURN = 0x0f,
    W_CALL = 0x10, W_DROP = 0x1a,
    W_LGET = 0x20, W_LSET = 0x21, W_LTEE = 0x22,
    W_I32_LOAD = 0x28, W_I64_LOAD = 0x29, W_I32_LOAD8_U = 0x2d,
    W_I32_LOAD16_U = 0x2f, W_I32_STORE = 0x36, W_I64_STORE = 0x37,
    W_I32_STORE8 = 0x3a, W_I32_STORE16 = 0x3b,
    W_I32_CONST = 0x41,
    W_I32_EQZ = 0x45, W_I32_EQ = 0x46, W_I32_NE = 0x47, W_I32_LT_S = 0x48,
    W_I32_LT_U = 0x49, W_I32_GT_S = 0x4a, W_I32_GT_U = 0x4b,
    W_I32_LE_S = 0x4c, W_I32_LE_U = 0x4d,
    W_I32_ADD = 0x6a, W_I32_SUB = 0x6b, W_I32_AND = 0x71, W_I32_OR = 0x72,
    W_I32_XOR = 0x73, W_I32_SHL = 0x74, W_I32_SHR_S = 0x75, W_I32_SHR_U = 0x76,
    W_I64_ADD = 0x7c, W_I64_EXTEND_I32_U = 0xad, W_I64_CONST = 0x42,
};
#define BT_VOID 0x40
#define BT_I32  0x7f

/* imported functions, in import order */
enum { F_LD = 0, F_ST, F_TESTCC, F_GETCF, F_SHIFT, F_MULDIV, F_IMUL, F_STEP, F_NEXT, F_BLOCK };
#define W_RETURN_CALL_INDIRECT 0x13

/* params: 0 the X86CPUState pointer, 1 the block to start at; then locals.
 * FA/FB/FR hold the operands and result of the last flag producer in the
 * block, which e_cond/e_cf read: never use them as scratch. */
enum { L_S = 0, L_B, L_LA, L_E, L_T1, L_T2, L_FA, L_FB, L_FR, L_CNT, L_V, L_AB,
       L_PB,    /* linear base of the page this call runs it at (see gen_page) */
       L_COUNT };
#define N_PARAMS 2

/* ---- decoded instructions ---------------------------------------------- */

typedef struct {
    int is_reg, rm;             /* register operand */
    int base, index, scale;     /* memory operand; -1 = absent */
    int32_t disp;
    int seg;
} Mo;

enum {
    K_ALU,      /* op (0..7, adc/sbb included), size, form */
    K_INCDEC,   /* inc/dec reg32; dec flag in op */
    K_INCDEC_RM,/* inc/dec r/m (fe/ff /0 /1); dec flag in op */
    K_PUSH, K_PUSHI, K_POP, K_PUSH_RM,
    K_MOV_RM_R, K_MOV_R_RM, K_MOV_RM_I, K_MOV_R_I,
    K_LEA, K_MOVZX, K_MOVSX, K_NOP,
    K_SHIFT,    /* group2: op = reg field; form 0 count imm, 1 count CL */
    K_NOT, K_NEG,
    K_MULDIV,   /* group3 /4../7, through x86_jit_muldiv */
    K_IMUL,     /* r32 = rm32 * (form 0: r32, form 1: imm) */
    K_CMOV, K_SETCC, K_XCHG, K_CWDE, K_CDQ, K_LEAVE,
    K_JCC, K_JMP, K_CALL, K_RET, K_JMP_IND, K_CALL_IND,
    K_STEP,     /* interpreted by x86_jit_step(); term if it always jumps */
    K_CLD,      /* cld (op 0) / std (op 1) */
    K_MOVD_X,   /* movd xmm(reg), r/m32 */
    K_PUNPCKLDQ,/* punpckldq xmm(reg), xmm(rm) */
    K_MOVQ_ST,  /* movq xmm(rm)/m64, xmm(reg) */
};

/* ALU operand forms */
enum { AF_RM_R, AF_R_RM, AF_ACC_I, AF_RM_I, AF_TEST, AF_TEST_I };

typedef struct {
    uint32_t eip, next;
    int kind, op, size, form, reg, cc;
    int msize;          /* movzx/movsx source size */
    uint32_t imm, target;
    Mo m;
    int term;
} Insn;

typedef struct {
    const uint8_t *p;
    int n, avail;
    int bad;
} Rd;

static uint8_t rd8(Rd *r)
{
    if (r->n >= r->avail) { r->bad = 1; return 0; }
    return r->p[r->n++];
}
static uint32_t rd16(Rd *r)
{
    uint32_t v = rd8(r);
    v |= (uint32_t)rd8(r) << 8;
    return v;
}
static uint32_t rd32(Rd *r)
{
    uint32_t v = rd8(r);
    v |= (uint32_t)rd8(r) << 8;
    v |= (uint32_t)rd8(r) << 16;
    v |= (uint32_t)rd8(r) << 24;
    return v;
}

/* 32-bit ModR/M, with the interpreter's default-segment rules (decode_modrm). */
static int rd_modrm(Rd *r, Mo *m, int *reg)
{
    uint8_t b = rd8(r);
    int mod = b >> 6, rmf = b & 7;
    *reg = (b >> 3) & 7;
    m->is_reg = 0; m->rm = 0;
    m->base = -1; m->index = -1; m->scale = 0; m->disp = 0;
    m->seg = X86_CPU_SEG_DS;
    if (mod == 3) {
        m->is_reg = 1;
        m->rm = rmf;
        return !r->bad;
    }
    if (rmf == 4) {
        uint8_t sib = rd8(r);
        int idx = (sib >> 3) & 7, bs = sib & 7;
        m->scale = sib >> 6;
        if (idx != 4) m->index = idx;
        if (bs == 5 && mod == 0) {
            m->disp = (int32_t)rd32(r);
        } else {
            m->base = bs;
            if (bs == 4 || bs == 5) m->seg = X86_CPU_SEG_SS;
        }
    } else if (rmf == 5 && mod == 0) {
        m->disp = (int32_t)rd32(r);
    } else {
        m->base = rmf;
        if (rmf == 5) m->seg = X86_CPU_SEG_SS;
    }
    if (mod == 1) m->disp += (int8_t)rd8(r);
    else if (mod == 2) m->disp += (int32_t)rd32(r);
    return !r->bad;
}

/* register operand `reg` as a Mo, for the accumulator/xchg short forms */
static void mo_reg(Mo *m, int reg)
{
    memset(m, 0, sizeof(*m));
    m->is_reg = 1;
    m->rm = reg;
}

/* Decode one instruction the compiler translates. Returns its length, or 0
 * (unsupported, or not entirely in the page; decode() then tries a K_STEP).
 * Prefixes: segment overrides apply to the memory operand, as in the
 * interpreter, and `lock` is ignored, as the interpreter does (one CPU);
 * 66 (16-bit operands) only for the mov/ALU/test/shift forms osz_ok()
 * lists, and three SSE2 moves; 67/F2/F3 are not handled. */
static int osz_ok(uint8_t b)
{
    if (b < 0x40 && (b & 0xc0) == 0 && ((b & 7) == 1 || (b & 7) == 3 || (b & 7) == 5))
        return 1;       /* the ALU family's word forms */
    return b == 0x81 || b == 0x83 || b == 0x85 || b == 0x89 || b == 0x8b || b == 0xa9 ||
           (b >= 0xb8 && b <= 0xbf) || b == 0xc7 || b == 0xc1 || b == 0xd1 || b == 0xd3;
}

static int decode_native(const uint8_t *p, int avail, uint32_t eip, Insn *in)
{
    static const int8_t seg_pfx[8] = {
        X86_CPU_SEG_ES, X86_CPU_SEG_CS, X86_CPU_SEG_SS, X86_CPU_SEG_DS,
    };
    Rd r = { p, 0, avail, 0 };
    int seg = -1, osz = 0;
    uint8_t b;

    memset(in, 0, sizeof(*in));
    in->eip = eip;

    for (;;) {
        b = rd8(&r);
        if (b == 0x26 || b == 0x2e || b == 0x36 || b == 0x3e) seg = seg_pfx[(b >> 3) & 3];
        else if (b == 0x64) seg = X86_CPU_SEG_FS;
        else if (b == 0x65) seg = X86_CPU_SEG_GS;
        else if (b == 0xf0) { /* lock */ }
        else if (b == 0x66) osz = 1;
        else break;
        if (r.n > 4) return 0;
    }
    if (osz && b != 0x0f && !osz_ok(b))
        return 0;
    /* the operand size of the "full size" forms; immediates of that size */
#define SZ4 (osz ? 2 : 4)
#define RDI (osz ? rd16(&r) : rd32(&r))
    in->size = SZ4;

    if (b < 0x40 && (b & 7) < 6 && (b & 0xc0) == 0) {
        int op = b >> 3, form = b & 7;
        in->kind = K_ALU; in->op = op;
        in->size = (form & 1) ? SZ4 : 1;
        switch (form) {
        case 0: case 1: in->form = AF_RM_R; if (!rd_modrm(&r, &in->m, &in->reg)) return 0; break;
        case 2: case 3: in->form = AF_R_RM; if (!rd_modrm(&r, &in->m, &in->reg)) return 0; break;
        case 4: in->form = AF_ACC_I; in->imm = rd8(&r); break;
        default: in->form = AF_ACC_I; in->imm = RDI; break;
        }
    } else if (b >= 0x40 && b <= 0x4f) {
        in->kind = K_INCDEC; in->op = (b >> 3) & 1; in->reg = b & 7;
    } else if (b >= 0x50 && b <= 0x57) {
        in->kind = K_PUSH; in->reg = b & 7;
    } else if (b >= 0x58 && b <= 0x5f) {
        in->kind = K_POP; in->reg = b & 7;
    } else if (b == 0x68) {
        in->kind = K_PUSHI; in->imm = rd32(&r);
    } else if (b == 0x6a) {
        in->kind = K_PUSHI; in->imm = (uint32_t)(int32_t)(int8_t)rd8(&r);
    } else if (b == 0x69 || b == 0x6b) {
        if (!rd_modrm(&r, &in->m, &in->reg)) return 0;
        in->kind = K_IMUL; in->form = 1;
        in->imm = (b == 0x69) ? rd32(&r) : (uint32_t)(int32_t)(int8_t)rd8(&r);
    } else if (b >= 0x70 && b <= 0x7f) {
        int32_t d = (int8_t)rd8(&r);
        in->kind = K_JCC; in->cc = b & 15; in->term = 1;
        in->target = eip + r.n + d;
    } else if (b == 0x80 || b == 0x81 || b == 0x83) {
        if (!rd_modrm(&r, &in->m, &in->reg)) return 0;
        in->kind = K_ALU; in->op = in->reg; in->form = AF_RM_I;
        if (b == 0x80) { in->size = 1; in->imm = rd8(&r); }
        else if (b == 0x81) in->imm = RDI;
        else in->imm = (uint32_t)(int32_t)(int8_t)rd8(&r);
    } else if (b == 0x84 || b == 0x85) {
        if (!rd_modrm(&r, &in->m, &in->reg)) return 0;
        in->kind = K_ALU; in->op = 4; in->form = AF_TEST;
        in->size = (b == 0x84) ? 1 : SZ4;
    } else if (b == 0x86 || b == 0x87) {
        if (!rd_modrm(&r, &in->m, &in->reg)) return 0;
        in->kind = K_XCHG; in->size = (b == 0x86) ? 1 : 4;
    } else if (b >= 0x88 && b <= 0x8b) {
        if (!rd_modrm(&r, &in->m, &in->reg)) return 0;
        in->kind = (b & 2) ? K_MOV_R_RM : K_MOV_RM_R;
        in->size = (b & 1) ? SZ4 : 1;
    } else if (b == 0x8d) {
        if (!rd_modrm(&r, &in->m, &in->reg) || in->m.is_reg) return 0;
        in->kind = K_LEA;
    } else if (b == 0x90) {
        in->kind = K_NOP;
    } else if (b >= 0x91 && b <= 0x97) {
        in->kind = K_XCHG; mo_reg(&in->m, b & 7); in->reg = 0;
    } else if (b == 0x98) {
        in->kind = K_CWDE;
    } else if (b == 0x99) {
        in->kind = K_CDQ;
    } else if (b >= 0xa0 && b <= 0xa3) {
        /* mov al/eax <-> [moffs32]: a ModR/M-less [disp32] */
        memset(&in->m, 0, sizeof(in->m));
        in->m.base = in->m.index = -1;
        in->m.disp = (int32_t)rd32(&r);
        in->m.seg = X86_CPU_SEG_DS;
        in->reg = 0;
        in->size = (b & 1) ? 4 : 1;
        in->kind = (b & 2) ? K_MOV_RM_R : K_MOV_R_RM;
    } else if (b == 0xa8 || b == 0xa9) {
        in->kind = K_ALU; in->op = 4; in->form = AF_TEST_I; mo_reg(&in->m, 0);
        in->size = (b == 0xa8) ? 1 : SZ4;
        in->imm = (b == 0xa8) ? rd8(&r) : RDI;
    } else if (b >= 0xb0 && b <= 0xb7) {
        in->kind = K_MOV_R_I; in->size = 1; in->reg = b & 7; in->imm = rd8(&r);
    } else if (b >= 0xb8 && b <= 0xbf) {
        in->kind = K_MOV_R_I; in->reg = b & 7; in->imm = RDI;
    } else if (b == 0xc0 || b == 0xc1 || (b >= 0xd0 && b <= 0xd3)) {
        if (!rd_modrm(&r, &in->m, &in->reg)) return 0;
        in->kind = K_SHIFT; in->op = in->reg;
        in->size = (b & 1) ? SZ4 : 1;
        if (b <= 0xc1) in->imm = rd8(&r);           /* count imm8 */
        else if (b <= 0xd1) in->imm = 1;            /* count 1 */
        else in->form = 1;                          /* count CL */
    } else if (b == 0xc6 || b == 0xc7) {
        if (!rd_modrm(&r, &in->m, &in->reg) || in->reg != 0) return 0;
        in->kind = K_MOV_RM_I;
        if (b == 0xc6) { in->size = 1; in->imm = rd8(&r); }
        else in->imm = RDI;
    } else if (b == 0xc3) {
        in->kind = K_RET; in->term = 1;
    } else if (b == 0xc9) {
        in->kind = K_LEAVE;
    } else if (b == 0xfc || b == 0xfd) {
        in->kind = K_CLD; in->op = b & 1;
    } else if (b == 0xe8) {
        int32_t d = (int32_t)rd32(&r);
        in->kind = K_CALL; in->term = 1; in->target = eip + r.n + d;
    } else if (b == 0xe9) {
        int32_t d = (int32_t)rd32(&r);
        in->kind = K_JMP; in->term = 1; in->target = eip + r.n + d;
    } else if (b == 0xeb) {
        int32_t d = (int8_t)rd8(&r);
        in->kind = K_JMP; in->term = 1; in->target = eip + r.n + d;
    } else if (b == 0xf6 || b == 0xf7) {
        if (!rd_modrm(&r, &in->m, &in->reg)) return 0;
        in->size = (b == 0xf6) ? 1 : 4;
        switch (in->reg) {
        case 0: case 1:
            in->kind = K_ALU; in->op = 4; in->form = AF_TEST_I;
            in->imm = (b == 0xf6) ? rd8(&r) : rd32(&r);
            break;
        case 2: in->kind = K_NOT; break;
        case 3: in->kind = K_NEG; break;
        default: in->kind = K_MULDIV; in->op = in->reg; break;
        }
    } else if (b == 0xfe || b == 0xff) {
        if (!rd_modrm(&r, &in->m, &in->reg)) return 0;
        in->size = (b == 0xfe) ? 1 : 4;
        switch (in->reg) {
        case 0: case 1: in->kind = K_INCDEC_RM; in->op = in->reg; break;
        case 2: if (b == 0xfe) return 0; in->kind = K_CALL_IND; in->term = 1; break;
        case 4: if (b == 0xfe) return 0; in->kind = K_JMP_IND; in->term = 1; break;
        case 6: if (b == 0xfe) return 0; in->kind = K_PUSH_RM; break;
        default: return 0;
        }
    } else if (b == 0x0f && osz) {
        /* the three SSE2 moves gcc's i686 code uses for 64-bit values */
        uint8_t b2 = rd8(&r);
        if (b2 == 0x6e) in->kind = K_MOVD_X;
        else if (b2 == 0x62) in->kind = K_PUNPCKLDQ;
        else if (b2 == 0xd6) in->kind = K_MOVQ_ST;
        else return 0;
        if (!rd_modrm(&r, &in->m, &in->reg)) return 0;
        if (in->kind == K_PUNPCKLDQ && !in->m.is_reg)
            return 0;               /* m128: alignment-checked; left to the interpreter */
    } else if (b == 0x0f) {
        uint8_t b2 = rd8(&r);
        if (b2 >= 0x80 && b2 <= 0x8f) {
            int32_t d = (int32_t)rd32(&r);
            in->kind = K_JCC; in->cc = b2 & 15; in->term = 1;
            in->target = eip + r.n + d;
        } else if (b2 == 0xb6 || b2 == 0xb7 || b2 == 0xbe || b2 == 0xbf) {
            if (!rd_modrm(&r, &in->m, &in->reg)) return 0;
            in->kind = (b2 & 8) ? K_MOVSX : K_MOVZX;
            in->msize = (b2 & 1) ? 2 : 1;
        } else if (b2 >= 0x40 && b2 <= 0x4f) {
            if (!rd_modrm(&r, &in->m, &in->reg)) return 0;
            in->kind = K_CMOV; in->cc = b2 & 15;
        } else if (b2 >= 0x90 && b2 <= 0x9f) {
            if (!rd_modrm(&r, &in->m, &in->reg)) return 0;
            in->kind = K_SETCC; in->cc = b2 & 15; in->size = 1;
        } else if (b2 == 0xaf) {
            if (!rd_modrm(&r, &in->m, &in->reg)) return 0;
            in->kind = K_IMUL; in->form = 0;
        } else {
            return 0;
        }
    } else {
        return 0;
    }
#undef SZ4
#undef RDI
    if (r.bad) return 0;
    if (seg >= 0 && !in->m.is_reg)
        in->m.seg = seg;
    in->next = eip + r.n;
    return r.n;
}

/* ---- instructions the compiler leaves to the interpreter ----------------
 *
 * Any other instruction whose length is known becomes a K_STEP: the block
 * calls x86_jit_step() to interpret just that one and goes on after it (or
 * leaves, if it jumped or changed what the block assumes). The block no
 * longer has to end there and come back through a lookup that finds no
 * block at the next instruction, interpreting on to the next branch. */

/* Skip a ModR/M byte with its SIB and displacement at p[*n]. 0: truncated. */
static int len_modrm(const uint8_t *p, int avail, int *n, int asz, int *reg)
{
    int m, mod, rm;
    if (*n >= avail) return 0;
    m = p[(*n)++];
    mod = m >> 6; rm = m & 7;
    if (reg) *reg = (m >> 3) & 7;
    if (mod == 3) return 1;
    if (asz == 4) {
        if (rm == 4) {
            if (*n >= avail) return 0;
            if (mod == 0 && (p[*n] & 7) == 5) *n += 4;
            (*n)++;
        } else if (mod == 0 && rm == 5) {
            *n += 4;
        }
        if (mod == 1) *n += 1; else if (mod == 2) *n += 4;
    } else {
        if (mod == 0 && rm == 6) *n += 2;
        else if (mod == 1) *n += 1;
        else if (mod == 2) *n += 2;
    }
    return 1;
}

/* Length of the instruction at p (0: unknown, or not within `avail`); *term:
 * it always transfers control, so a block ends with it. */
static int insn_len(const uint8_t *p, int avail, int *term)
{
    int n = 0, osz = 4, asz = 4, b, reg = 0, mod_rm = 0, imm = 0;

    *term = 0;
    for (;;) {
        if (n >= avail || n > 4) return 0;
        b = p[n++];
        if (b == 0x66) osz = 2;
        else if (b == 0x67) asz = 2;
        else if (b == 0x26 || b == 0x2e || b == 0x36 || b == 0x3e || b == 0x64 ||
                 b == 0x65 || b == 0xf0 || b == 0xf2 || b == 0xf3) { }
        else break;
    }
    if (b == 0x0f) {
        if (n >= avail) return 0;
        b = p[n++];
        if (b <= 0x03 || b == 0x0d || (b >= 0x10 && b <= 0x1f) || (b >= 0x28 && b <= 0x2f) ||
            (b >= 0x40 && b <= 0x6f) || (b >= 0x74 && b <= 0x76) || (b >= 0x7c && b <= 0x7f) ||
            (b >= 0x90 && b <= 0x9f) || b == 0xa3 || b == 0xa5 || b == 0xab || b == 0xad ||
            (b >= 0xae && b <= 0xb8) || (b >= 0xbb && b <= 0xc1) || b == 0xc3 || b == 0xc7 ||
            (b >= 0xd0 && b <= 0xfe)) {
            mod_rm = 1;
        } else if (b >= 0x20 && b <= 0x23) {
            /* mov to/from CR/DR: always a register operand */
            if (n >= avail || (p[n] >> 6) != 3) return 0;
            mod_rm = 1;
        } else if ((b >= 0x70 && b <= 0x73) || b == 0xa4 || b == 0xac || b == 0xba ||
                   b == 0xc2 || (b >= 0xc4 && b <= 0xc6)) {
            mod_rm = 1; imm = 1;
        } else if (b == 0x38) {
            if (n >= avail) return 0;
            n++; mod_rm = 1;
        } else if (b == 0x3a) {
            if (n >= avail) return 0;
            n++; mod_rm = 1; imm = 1;
        } else if (b >= 0x80 && b <= 0x8f) {
            imm = osz;                          /* jcc rel16/32: may fall through */
        } else if (b == 0x05 || b == 0x07 || b == 0x34 || b == 0x35) {
            *term = 1;                          /* syscall/sysret/sysenter/sysexit */
        } else if (b == 0x06 || b == 0x08 || b == 0x09 || b == 0x0b || (b >= 0x30 && b <= 0x33) ||
                   b == 0x77 || b == 0xa0 || b == 0xa1 || b == 0xa2 || b == 0xa8 || b == 0xa9 ||
                   (b >= 0xc8 && b <= 0xcf)) {
            /* no operands */
        } else {
            return 0;
        }
    } else if (b < 0x40) {
        switch (b & 7) {
        case 0: case 1: case 2: case 3: mod_rm = 1; break;
        case 4: imm = 1; break;
        case 5: imm = osz; break;
        default: break;                         /* push/pop seg, daa, das, aaa, aas */
        }
    } else if (b <= 0x61 || (b >= 0x6c && b <= 0x6f) || (b >= 0x90 && b <= 0x99) ||
               (b >= 0x9b && b <= 0x9f) || (b >= 0xa4 && b <= 0xa7) || (b >= 0xaa && b <= 0xaf) ||
               b == 0xc9 || b == 0xd6 || b == 0xd7 || (b >= 0xec && b <= 0xef) || b == 0xf5 ||
               (b >= 0xf8 && b <= 0xfd)) {
        /* no operands */
    } else if (b == 0x62 || b == 0x63 || (b >= 0x84 && b <= 0x8f) || b == 0xc4 || b == 0xc5 ||
               (b >= 0xd0 && b <= 0xd3) || (b >= 0xd8 && b <= 0xdf) || b == 0xfe) {
        mod_rm = 1;
    } else if (b == 0x68 || b == 0xa9 || (b >= 0xb8 && b <= 0xbf)) {
        imm = osz;
    } else if (b == 0x6a || (b >= 0x70 && b <= 0x7f) || b == 0xa8 || (b >= 0xb0 && b <= 0xb7) ||
               b == 0xd4 || b == 0xd5 || (b >= 0xe0 && b <= 0xe7)) {
        imm = 1;                                /* jcc/loop/jcxz may fall through */
    } else if (b == 0x69 || b == 0x81 || b == 0xc7) {
        mod_rm = 1; imm = osz;
    } else if (b == 0x6b || b == 0x80 || b == 0x82 || b == 0x83 || b == 0xc0 || b == 0xc1 || b == 0xc6) {
        mod_rm = 1; imm = 1;
    } else if (b >= 0xa0 && b <= 0xa3) {
        imm = asz;                              /* moffs */
    } else if (b == 0xc8) {
        imm = 3;                                /* enter imm16, imm8 */
    } else if (b == 0xc2 || b == 0xca) {
        imm = 2; *term = 1;
    } else if (b == 0xc3 || b == 0xcb || b == 0xcc || b == 0xce || b == 0xcf || b == 0xf1 || b == 0xf4) {
        *term = 1;                              /* ret, retf, int3, into, iret, int1, hlt */
    } else if (b == 0xcd) {
        imm = 1; *term = 1;
    } else if (b == 0xe8 || b == 0xe9) {
        imm = osz; *term = 1;
    } else if (b == 0xeb) {
        imm = 1; *term = 1;
    } else if (b == 0x9a || b == 0xea) {
        imm = osz + 2; *term = 1;
    } else if (b == 0xf6 || b == 0xf7) {
        if (!len_modrm(p, avail, &n, asz, &reg)) return 0;
        if (reg < 2) imm = (b == 0xf6) ? 1 : osz;
    } else if (b == 0xff) {
        if (!len_modrm(p, avail, &n, asz, &reg)) return 0;
        if (reg == 7) return 0;
        if (reg >= 2 && reg <= 5) *term = 1;    /* call/jmp, near and far */
    } else {
        return 0;
    }
    if (mod_rm && !len_modrm(p, avail, &n, asz, NULL))
        return 0;
    n += imm;
    return n <= avail ? n : 0;
}

/* x86_jit_step() can be turned off, for A/B runs and bisecting: then a block
 * ends before such an instruction, as it used to, and pokes the JIT. */
int x86_jit_steps = 1;

/* One instruction for a block: translated, or else a K_STEP. 0: the block
 * ends before it. */
static int decode(const uint8_t *p, int avail, uint32_t eip, Insn *in)
{
    int len = decode_native(p, avail, eip, in), term;
    if (len || !x86_jit_steps)
        return len;
    len = insn_len(p, avail, &term);
    if (!len)
        return 0;
    memset(in, 0, sizeof(*in));
    in->kind = K_STEP;
    in->eip = eip;
    in->next = eip + (uint32_t)len;
    in->term = term;
    return len;
}

/* ---- code generation --------------------------------------------------- */

/* One function per code page: every compiled block of the page, behind a
 * br_table on the block index, all inside one `loop`. A branch to another
 * block of the page is `local.set b; br loop`, so it never leaves WASM. */
typedef struct {
    Buf c;              /* function body */
    int mi;
    uint32_t page;      /* the code page's base as compiled: always 0, since
                           code is position-independent (L_PB at run time) */
    const uint16_t *starts;   /* page offsets of the blocks, sorted */
    int nstarts;
    int depth;          /* br depth from the current point to the dispatch loop */
    int ldepth;         /* br depth to the current block's own loop */
    int self;           /* index of the block being emitted */
    int cc_known;       /* CC_OP_* of the last flag producer in this block, or 0 */
    int cc_size;
    int k;              /* instructions before the one being emitted */
    int tail;           /* emitting a block's last instruction: a goto to the
                           next block may just fall through into it */
} Jc;

/* index of the block starting at linear `la` in this function, or -1 */
static int jc_block(const Jc *c, uint32_t la)
{
    int lo = 0, hi = c->nstarts - 1;
    uint32_t off;
    if ((la & ~0xfffu) != c->page)
        return -1;
    off = la & 0xfff;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (c->starts[mid] == off) return mid;
        if (c->starts[mid] < off) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

#define OFF(f) ((uint32_t)offsetof(X86CPUState, f))
#define OFF_REG(r) (OFF(regs) + 4u * (uint32_t)(r))
#define OFF_XMM(r) (OFF(xmm) + 16u * (uint32_t)(r))
#define OFF_SEGBASE(sg) (OFF(seg) + (uint32_t)(sg) * (uint32_t)sizeof(X86Seg) + \
                         (uint32_t)offsetof(X86Seg, base))
#define OFF_TLBR(mi) (OFF(tlb_read) + (uint32_t)(mi) * X86_TLB_SIZE * (uint32_t)sizeof(X86TLBEntry))
#define OFF_TLBW(mi) (OFF(tlb_write) + (uint32_t)(mi) * X86_TLB_SIZE * (uint32_t)sizeof(X86TLBEntry))

/* The generated code writes cc_op/cc_size/cc_cin as one 32-bit word. */
_Static_assert(offsetof(X86CPUState, cc_size) == offsetof(X86CPUState, cc_op) + 1, "cc layout");
_Static_assert(offsetof(X86CPUState, cc_cin) == offsetof(X86CPUState, cc_op) + 2, "cc layout");
_Static_assert(offsetof(X86CPUState, seg) >= offsetof(X86CPUState, cc_op) + 4, "cc layout");
_Static_assert((offsetof(X86CPUState, cc_op) & 3) == 0, "cc layout");
_Static_assert(sizeof(X86TLBEntry) == 8 && offsetof(X86TLBEntry, mem_addend) == 4, "tlb layout");
_Static_assert(sizeof(uintptr_t) == 4, "wasm32 only");

#define OP(x) b_byte(&c->c, (x))
#define OP_C(cc, x) b_byte(&(cc)->c, (x))

static void e_i32(Jc *c, uint32_t v) { OP(W_I32_CONST); b_sleb(&c->c, (int32_t)v); }
static void e_get(Jc *c, int l) { OP(W_LGET); b_uleb(&c->c, l); }
static void e_set(Jc *c, int l) { OP(W_LSET); b_uleb(&c->c, l); }
static void e_tee(Jc *c, int l) { OP(W_LTEE); b_uleb(&c->c, l); }
static void e_mem(Jc *c, int op, int align, uint32_t off)
{
    OP(op); b_uleb(&c->c, align); b_uleb(&c->c, off);
}
static void e_call(Jc *c, int f) { OP(W_CALL); b_uleb(&c->c, f); }

/* push a state field */
static void e_ld_s(Jc *c, uint32_t off) { e_get(c, L_S); e_mem(c, W_I32_LOAD, 2, off); }
/* state field = local */
static void e_st_s(Jc *c, uint32_t off, int l)
{
    e_get(c, L_S); e_get(c, l); e_mem(c, W_I32_STORE, 2, off);
}
static void e_st_s_const(Jc *c, uint32_t off, uint32_t v)
{
    e_get(c, L_S); e_i32(c, v); e_mem(c, W_I32_STORE, 2, off);
}

/* The linear address of page-relative `rel` onto the stack. A page's code
 * is compiled as if the page sat at linear 0: every EIP it stores, and
 * every return address it pushes, is the page's linear base at run time
 * (L_PB) plus a constant. So one function serves a frame wherever it is
 * mapped -- shared libraries sit at a different address in every process
 * (ASLR), and keying code by linear page compiled libc again for each. */
static void e_eip(Jc *c, uint32_t rel)
{
    e_get(c, L_PB);
    if (rel) { e_i32(c, rel); OP(W_I32_ADD); }
}
/* state field = the linear address of page-relative `rel` */
static void e_st_s_eip(Jc *c, uint32_t off, uint32_t rel)
{
    e_get(c, L_S); e_eip(c, rel); e_mem(c, W_I32_STORE, 2, off);
}

static void e_getreg(Jc *c, int r) { e_ld_s(c, OFF_REG(r)); }
static void e_setreg(Jc *c, int r, int l) { e_st_s(c, OFF_REG(r), l); }

/* 8-bit register (AL..BH) value onto the stack */
static void e_getreg8(Jc *c, int r)
{
    e_getreg(c, r & 3);
    if (r & 4) { e_i32(c, 8); OP(W_I32_SHR_U); }
    e_i32(c, 0xff); OP(W_I32_AND);
}
/* 8-bit register = low byte of local l */
static void e_setreg8(Jc *c, int r, int l)
{
    int sh = (r & 4) ? 8 : 0;
    e_get(c, L_S);
    e_getreg(c, r & 3);
    e_i32(c, ~(0xffu << sh)); OP(W_I32_AND);
    e_get(c, l); e_i32(c, 0xff); OP(W_I32_AND);
    if (sh) { e_i32(c, sh); OP(W_I32_SHL); }
    OP(W_I32_OR);
    e_mem(c, W_I32_STORE, 2, OFF_REG(r & 3));
}
/* 16-bit register = low half of local l */
static void e_setreg16(Jc *c, int r, int l)
{
    e_get(c, L_S);
    e_getreg(c, r); e_i32(c, 0xffff0000u); OP(W_I32_AND);
    e_get(c, l); e_i32(c, 0xffff); OP(W_I32_AND);
    OP(W_I32_OR);
    e_mem(c, W_I32_STORE, 2, OFF_REG(r));
}
static void e_getreg_sz(Jc *c, int r, int size)
{
    if (size == 1) e_getreg8(c, r);
    else if (size == 2) { e_getreg(c, r); e_i32(c, 0xffff); OP(W_I32_AND); }
    else e_getreg(c, r);
}
static void e_setreg_sz(Jc *c, int r, int l, int size)
{
    if (size == 1) e_setreg8(c, r, l);
    else if (size == 2) e_setreg16(c, r, l);
    else e_setreg(c, r, l);
}

/* Before a helper that may fault: record the instructions completed so far
 * (cnt + k), for the fault landing to count. */
static void e_pending(Jc *c, int k)
{
    e_get(c, L_S);
    e_get(c, L_CNT); e_i32(c, k); OP(W_I32_ADD);
    e_mem(c, W_I32_STORE, 2, OFF(jit_pending));
}

/* Leave the block: cycle_count += cnt + k; return cnt + k. EIP must be set. */
/* With tail calls, an exit asks x86_jit_next() for the compiled block at
 * the new EIP and jumps straight into it (return_call_indirect), so a run
 * goes from page to page without returning to C. 0: return to run_chain.
 * Opt-in (Module.x86jitTailcalls, the agent page's ?jittail=1): measured
 * in Chrome 156 it made boot, the shell loop and Python slower than
 * run_chain's C loop -- the indirect calls go between module instances
 * either way, and every exit carries the extra code. */
static int jit_tail;

static void e_exit_tail(Jc *c, int k)
{
    e_st_s_const(c, OFF(jit_pending), 0);
    e_get(c, L_S);
    e_get(c, L_S); e_mem(c, W_I64_LOAD, 3, OFF(cycle_count));
    e_get(c, L_CNT); e_i32(c, k); OP(W_I32_ADD);
    OP(W_I64_EXTEND_I32_U); OP(W_I64_ADD);
    e_mem(c, W_I64_STORE, 3, OFF(cycle_count));
    if (jit_tail) {
        /* L_V = table index << 8 | block, or 0 */
        e_get(c, L_S); e_get(c, L_CNT); e_i32(c, k); OP(W_I32_ADD);
        e_call(c, F_NEXT); e_tee(c, L_V);
        OP(W_IF); OP(BT_VOID);
          e_get(c, L_S);
          e_get(c, L_V); e_i32(c, 0xff); OP(W_I32_AND);
          e_get(c, L_V); e_i32(c, 8); OP(W_I32_SHR_U);
          OP(W_RETURN_CALL_INDIRECT); b_uleb(&c->c, 2); b_uleb(&c->c, 0);
        OP(W_END);
    }
    e_get(c, L_CNT); e_i32(c, k); OP(W_I32_ADD);
    OP(W_RETURN);
}
static void e_exit(Jc *c, uint32_t eip, int k)
{
    e_st_s_eip(c, OFF(eip), eip);
    e_exit_tail(c, k);
}

/* Continue at `target`, k instructions into this block: a block of this
 * page is entered with a branch back to the dispatch loop, as
 * long as the run stays under JIT_LOOP_LIMIT instructions (so interrupts and
 * the run loop's slices still come round); anything else returns to C.
 * poke: the block stopped before an instruction the compiler does not handle;
 * ask the interpreter to come back to the JIT right after running it. */
/* debug: 0 makes every block return to C (tests/x86_jit_fuzz.c lockstep) */
int x86_jit_chain = 1;

static void e_goto(Jc *c, uint32_t target, int k, int poke)
{
    int b = x86_jit_chain ? jc_block(c, target) : -1;
    if (b >= 0 && b == c->self + 1 && c->tail && c->ldepth == 0 && !poke) {
        /* The next block's code follows this one's: fall into it. Forward,
         * so the run's budget needs no check here. */
        e_get(c, L_CNT); e_i32(c, k); OP(W_I32_ADD); e_set(c, L_CNT);
        return;
    }
    if (b >= 0) {
        e_get(c, L_CNT); e_i32(c, k); OP(W_I32_ADD); e_tee(c, L_CNT);
        e_i32(c, JIT_LOOP_LIMIT); OP(W_I32_LT_U);
        OP(W_IF); OP(BT_VOID);
          e_i32(c, b); e_set(c, L_B);
          /* back to this block's own start: its own loop, without the
           * dispatch (+1 for this if) */
          OP(W_BR); b_uleb(&c->c, (b == c->self ? c->ldepth : c->depth) + 1);
        OP(W_END);
        k = 0;                                      /* already in L_CNT */
    }
    if (poke)
        e_st_s_const(c, OFF(jit_poke), 1);
    e_exit(c, target, k);
}

/* linear address of a memory operand into L_LA (with_seg: add the segment
 * base, i.e. a linear address; without: the offset, for lea) */
static void e_ea(Jc *c, const Mo *m, int with_seg, int dst)
{
    int have = 0;
    if (m->base >= 0) { e_getreg(c, m->base); have = 1; }
    if (m->index >= 0) {
        e_getreg(c, m->index);
        if (m->scale) { e_i32(c, m->scale); OP(W_I32_SHL); }
        if (have) OP(W_I32_ADD);
        have = 1;
    }
    if (!have) {
        e_i32(c, (uint32_t)m->disp);
    } else if (m->disp) {
        e_i32(c, (uint32_t)m->disp); OP(W_I32_ADD);
    }
    if (with_seg) { e_ld_s(c, OFF_SEGBASE(m->seg)); OP(W_I32_ADD); }
    e_set(c, dst);
}

/* L_E = &tlb[(la >> 12) & mask] relative to the state (add OFF_TLB* as the
 * memarg offset) */
static void e_tlb_slot(Jc *c)
{
    e_get(c, L_LA); e_i32(c, 12); OP(W_I32_SHR_U);
    e_i32(c, X86_TLB_SIZE - 1); OP(W_I32_AND);
    e_i32(c, 3); OP(W_I32_SHL);
    e_get(c, L_S); OP(W_I32_ADD);
    e_set(c, L_E);
}

/* load `size` bytes at L_LA onto the stack (zero-extended) */
static void e_load(Jc *c, const Insn *in, int size)
{
    uint32_t tlb = OFF_TLBR(c->mi);
    e_tlb_slot(c);
    e_get(c, L_LA); e_get(c, L_E); e_mem(c, W_I32_LOAD, 2, tlb); OP(W_I32_XOR);
    e_i32(c, 0x1000u - size); OP(W_I32_LE_U);
    OP(W_IF); OP(BT_I32);
      e_get(c, L_E); e_mem(c, W_I32_LOAD, 2, tlb + 4);
      e_get(c, L_LA); OP(W_I32_ADD);
      if (size == 4) e_mem(c, W_I32_LOAD, 0, 0);
      else if (size == 2) e_mem(c, W_I32_LOAD16_U, 0, 0);
      else e_mem(c, W_I32_LOAD8_U, 0, 0);
    OP(W_ELSE);
      e_st_s_eip(c, OFF(cur_eip), in->eip);
      e_pending(c, c->k);
      e_get(c, L_S); e_get(c, L_LA); e_i32(c, size);
      e_call(c, F_LD);
    OP(W_END);
}

/* store local `l` (`size` bytes) at L_LA; sets L_AB nonzero when the store
 * dropped compiled code (the caller exits after finishing the instruction) */
static void e_store(Jc *c, const Insn *in, int l, int size)
{
    uint32_t tlb = OFF_TLBW(c->mi);
    e_tlb_slot(c);
    e_get(c, L_LA); e_get(c, L_E); e_mem(c, W_I32_LOAD, 2, tlb); OP(W_I32_XOR);
    e_i32(c, 0x1000u - size); OP(W_I32_LE_U);
    OP(W_IF); OP(BT_VOID);
      e_get(c, L_E); e_mem(c, W_I32_LOAD, 2, tlb + 4);
      e_get(c, L_LA); OP(W_I32_ADD);
      e_get(c, l);
      if (size == 4) e_mem(c, W_I32_STORE, 0, 0);
      else if (size == 2) e_mem(c, W_I32_STORE16, 0, 0);
      else e_mem(c, W_I32_STORE8, 0, 0);
    OP(W_ELSE);
      e_st_s_eip(c, OFF(cur_eip), in->eip);
      e_pending(c, c->k);
      e_get(c, L_S); e_get(c, L_LA); e_get(c, l); e_i32(c, size);
      e_call(c, F_ST);
      e_set(c, L_AB);
    OP(W_END);
}
/* after an instruction that stored: leave if the store dropped compiled code */
static void e_abort_check(Jc *c, const Insn *in, int k)
{
    e_get(c, L_AB);
    OP(W_IF); OP(BT_VOID);
      e_exit(c, in->next, k + 1);
    OP(W_END);
}

/* value of a r/m operand onto the stack (memory: address already in L_LA) */
static void e_rm_read(Jc *c, const Insn *in, int size)
{
    if (in->m.is_reg) e_getreg_sz(c, in->m.rm, size);
    else e_load(c, in, size);
}

/* Write the deferred-flag record for an add/sub/logic result: FA, FB, FR
 * hold a, b, r (masked to size). */
static void e_cc_record(Jc *c, int ccop, int size)
{
    e_st_s(c, OFF(cc_a), L_FA);
    e_st_s(c, OFF(cc_b), L_FB);
    e_st_s(c, OFF(cc_r), L_FR);
    e_st_s_const(c, OFF(cc_op), (uint32_t)ccop | ((uint32_t)size << 8));
    c->cc_known = ccop;
    c->cc_size = size;
}

static void e_cf(Jc *c);

/* alu(): FA = T1, FB = T2, FR = T1 op T2 (T1, T2 masked to size). adc/sbb
 * take the carry in from the record before it is overwritten, and keep it
 * in cc_cin, as alu() does. */
static void e_alu(Jc *c, int op, int size)
{
    int ccop, wop, carry = (op == 2 || op == 3);
    switch (op) {
    case 0: wop = W_I32_ADD; ccop = CC_OP_ADD; break;
    case 1: wop = W_I32_OR;  ccop = CC_OP_LOGIC; break;
    case 2: wop = W_I32_ADD; ccop = CC_OP_ADC; break;
    case 3: wop = W_I32_SUB; ccop = CC_OP_SBB; break;
    case 4: wop = W_I32_AND; ccop = CC_OP_LOGIC; break;
    case 6: wop = W_I32_XOR; ccop = CC_OP_LOGIC; break;
    default: wop = W_I32_SUB; ccop = CC_OP_SUB; break;   /* sub, cmp */
    }
    if (carry) { e_cf(c); e_set(c, L_V); }
    e_get(c, L_T1); e_set(c, L_FA);
    e_get(c, L_T2); e_set(c, L_FB);
    e_get(c, L_T1); e_get(c, L_T2); OP(wop);
    if (carry) { e_get(c, L_V); OP(wop); }
    if (size == 1) { e_i32(c, 0xff); OP(W_I32_AND); }
    else if (size == 2) { e_i32(c, 0xffff); OP(W_I32_AND); }
    e_set(c, L_FR);
    if (!carry) {
        e_cc_record(c, ccop, size);
        return;
    }
    e_st_s(c, OFF(cc_a), L_FA);
    e_st_s(c, OFF(cc_b), L_FB);
    e_st_s(c, OFF(cc_r), L_FR);
    e_get(c, L_S);
    e_get(c, L_V); e_i32(c, 16); OP(W_I32_SHL);
    e_i32(c, (uint32_t)ccop | ((uint32_t)size << 8)); OP(W_I32_OR);
    e_mem(c, W_I32_STORE, 2, OFF(cc_op));
    c->cc_known = ccop;
    c->cc_size = size;
}

/* inc/dec of T1 (masked to size) into FR, CF carried over in cc_cin */
static void e_incdec(Jc *c, int dec, int size)
{
    e_cf(c); e_set(c, L_V);                 /* the CF inc/dec preserve */
    e_get(c, L_T1); e_tee(c, L_FA);
    e_i32(c, 1); OP(dec ? W_I32_SUB : W_I32_ADD);
    if (size == 1) { e_i32(c, 0xff); OP(W_I32_AND); }
    e_set(c, L_FR);
    e_i32(c, 1); e_set(c, L_FB);
    e_st_s(c, OFF(cc_a), L_FA);
    e_st_s(c, OFF(cc_b), L_FB);
    e_st_s(c, OFF(cc_r), L_FR);
    e_get(c, L_S);
    e_get(c, L_V); e_i32(c, 16); OP(W_I32_SHL);
    e_i32(c, (uint32_t)(dec ? CC_OP_DEC : CC_OP_INC) | ((uint32_t)size << 8)); OP(W_I32_OR);
    e_mem(c, W_I32_STORE, 2, OFF(cc_op));
    c->cc_known = dec ? CC_OP_DEC : CC_OP_INC;
    c->cc_size = size;
}

/* Before a helper that may raise (#DE): what a faulting load does. */
static void e_may_fault(Jc *c, const Insn *in)
{
    e_st_s_eip(c, OFF(cur_eip), in->eip);
    e_pending(c, c->k);
}

/* write FR-like local `l` back to the r/m operand (address in L_LA) and
 * leave if that store dropped compiled code */
static void e_rm_write(Jc *c, const Insn *in, int l, int size, int k)
{
    if (in->m.is_reg) {
        e_setreg_sz(c, in->m.rm, l, size);
    } else {
        e_i32(c, 0); e_set(c, L_AB);
        e_store(c, in, l, size);
        e_abort_check(c, in, k);
    }
}

/* CF of the current record onto the stack */
static void e_cf(Jc *c)
{
    switch (c->cc_known) {
    case CC_OP_ADD:   e_get(c, L_FR); e_get(c, L_FA); OP(W_I32_LT_U); return;
    case CC_OP_SUB:   e_get(c, L_FA); e_get(c, L_FB); OP(W_I32_LT_U); return;
    case CC_OP_LOGIC: e_i32(c, 0); return;
    case CC_OP_INC: case CC_OP_DEC:
        e_get(c, L_S); e_mem(c, W_I32_LOAD8_U, 0, OFF(cc_cin)); return;
    default:
        e_get(c, L_S); e_call(c, F_GETCF); return;
    }
}

/* condition `cc` (Jcc encoding) onto the stack, nonzero = taken */
static void e_cond(Jc *c, int cc)
{
    int base = cc >> 1, k = c->cc_known, s4 = (c->cc_size == 4), ok = 0;

    if (k) {
        switch (base) {
        case 2: /* Z */
            e_get(c, L_FR); OP(W_I32_EQZ); ok = 1; break;
        case 4: /* S */
            e_get(c, L_FR);
            if (s4) { e_i32(c, 0); OP(W_I32_LT_S); }
            else { e_i32(c, c->cc_size == 2 ? 0x8000 : 0x80); OP(W_I32_AND); }
            ok = 1; break;
        case 0: /* O */
            if (k == CC_OP_LOGIC) { e_i32(c, 0); ok = 1; }
            break;
        case 1: /* B */
            if (k == CC_OP_SUB || k == CC_OP_ADD || k == CC_OP_LOGIC) { e_cf(c); ok = 1; }
            break;
        case 3: /* BE */
            if (k == CC_OP_SUB) { e_get(c, L_FA); e_get(c, L_FB); OP(W_I32_LE_U); ok = 1; }
            else if (k == CC_OP_LOGIC) { e_get(c, L_FR); OP(W_I32_EQZ); ok = 1; }
            break;
        case 6: /* L: SF != OF */
            if (s4 && k == CC_OP_SUB) { e_get(c, L_FA); e_get(c, L_FB); OP(W_I32_LT_S); ok = 1; }
            else if (s4 && k == CC_OP_LOGIC) { e_get(c, L_FR); e_i32(c, 0); OP(W_I32_LT_S); ok = 1; }
            break;
        case 7: /* LE: ZF || SF != OF */
            if (s4 && k == CC_OP_SUB) { e_get(c, L_FA); e_get(c, L_FB); OP(W_I32_LE_S); ok = 1; }
            else if (s4 && k == CC_OP_LOGIC) { e_get(c, L_FR); e_i32(c, 0); OP(W_I32_LE_S); ok = 1; }
            break;
        }
    }
    if (!ok) {
        e_get(c, L_S); e_i32(c, cc); e_call(c, F_TESTCC);
        return;
    }
    if (cc & 1) OP(W_I32_EQZ);
}

/* Hand the instruction to x86_jit_step(): EIP at it, the ones before
 * counted if it faults; leave if it jumped (or term: always). */
static void e_step(Jc *c, const Insn *in, int k)
{
    e_st_s_eip(c, OFF(eip), in->eip);
    e_pending(c, k);
    e_get(c, L_S); e_eip(c, in->next); e_call(c, F_STEP);
    if (in->term) {
        OP(W_DROP);
        e_exit_tail(c, k + 1);
    } else {
        OP(W_IF); OP(BT_VOID);
          e_exit_tail(c, k + 1);               /* EIP is where it left it */
        OP(W_END);
    }
}

/* nonzero when SSE instructions may run: sse_check's conditions */
static void e_sse_ok(Jc *c)
{
    e_ld_s(c, OFF(cr0)); e_i32(c, CR0_EM | CR0_TS); OP(W_I32_AND); OP(W_I32_EQZ);
    /* 0/1 like the test above (a bare `cr4 & OSFXSR` is 0x200, and
     * 1 & 0x200 is 0: the guard never passed) */
    e_ld_s(c, OFF(cr4)); e_i32(c, CR4_OSFXSR); OP(W_I32_AND); OP(W_I32_EQZ); OP(W_I32_EQZ);
    OP(W_I32_AND);
}

/* Emit one instruction; k = instructions before it in this pass. */
static void e_insn(Jc *c, const Insn *in, int k)
{
    int sz = in->size;

    c->k = k;
    switch (in->kind) {
    case K_NOP:
        break;

    case K_MOV_R_I:
        e_i32(c, in->imm); e_set(c, L_V);
        e_setreg_sz(c, in->reg, L_V, sz);
        break;

    case K_MOV_RM_R:
        if (in->m.is_reg) {
            e_getreg_sz(c, in->reg, sz); e_set(c, L_V);
            e_setreg_sz(c, in->m.rm, L_V, sz);
        } else {
            e_ea(c, &in->m, 1, L_LA);
            e_getreg_sz(c, in->reg, sz); e_set(c, L_V);
            e_i32(c, 0); e_set(c, L_AB);
            e_store(c, in, L_V, sz);
            e_abort_check(c, in, k);
        }
        break;

    case K_MOV_RM_I:
        e_i32(c, in->imm); e_set(c, L_V);
        if (in->m.is_reg) {
            e_setreg_sz(c, in->m.rm, L_V, sz);
        } else {
            e_ea(c, &in->m, 1, L_LA);
            e_i32(c, 0); e_set(c, L_AB);
            e_store(c, in, L_V, sz);
            e_abort_check(c, in, k);
        }
        break;

    case K_MOV_R_RM:
        if (!in->m.is_reg) e_ea(c, &in->m, 1, L_LA);
        e_rm_read(c, in, sz); e_set(c, L_V);
        e_setreg_sz(c, in->reg, L_V, sz);
        break;

    case K_MOVZX:
    case K_MOVSX:
        if (in->m.is_reg) {
            if (in->msize == 1) e_getreg8(c, in->m.rm);
            else { e_getreg(c, in->m.rm); e_i32(c, 0xffff); OP(W_I32_AND); }
        } else {
            e_ea(c, &in->m, 1, L_LA);
            e_load(c, in, in->msize);
        }
        if (in->kind == K_MOVSX) {
            int sh = (in->msize == 1) ? 24 : 16;
            e_i32(c, sh); OP(W_I32_SHL); e_i32(c, sh); OP(W_I32_SHR_S);
        }
        e_set(c, L_V);
        e_setreg(c, in->reg, L_V);
        break;

    case K_LEA:
        e_ea(c, &in->m, 0, L_V);
        e_setreg(c, in->reg, L_V);
        break;

    case K_ALU: {
        int writes = (in->op != 7 && in->form != AF_TEST && in->form != AF_TEST_I);
        switch (in->form) {
        case AF_RM_R: case AF_TEST: case AF_RM_I: case AF_TEST_I:
            if (!in->m.is_reg) e_ea(c, &in->m, 1, L_LA);
            e_rm_read(c, in, sz); e_set(c, L_T1);
            if (in->form == AF_RM_I || in->form == AF_TEST_I)
                e_i32(c, sz == 1 ? (in->imm & 0xff) : sz == 2 ? (in->imm & 0xffff) : in->imm);
            else
                e_getreg_sz(c, in->reg, sz);
            e_set(c, L_T2);
            e_alu(c, in->op, sz);
            if (writes)
                e_rm_write(c, in, L_FR, sz, k);
            break;
        case AF_R_RM:
            if (!in->m.is_reg) e_ea(c, &in->m, 1, L_LA);
            e_rm_read(c, in, sz); e_set(c, L_T2);
            e_getreg_sz(c, in->reg, sz); e_set(c, L_T1);
            e_alu(c, in->op, sz);
            if (writes) e_setreg_sz(c, in->reg, L_FR, sz);
            break;
        case AF_ACC_I:
            e_getreg_sz(c, 0, sz); e_set(c, L_T1);
            e_i32(c, in->imm); e_set(c, L_T2);
            e_alu(c, in->op, sz);
            if (writes) e_setreg_sz(c, 0, L_FR, sz);
            break;
        }
        break;
    }

    case K_INCDEC:
        e_getreg(c, in->reg); e_set(c, L_T1);
        e_incdec(c, in->op, 4);
        e_setreg(c, in->reg, L_FR);
        break;

    case K_INCDEC_RM:
        if (!in->m.is_reg) e_ea(c, &in->m, 1, L_LA);
        e_rm_read(c, in, sz); e_set(c, L_T1);
        e_incdec(c, in->op, sz);
        e_rm_write(c, in, L_FR, sz, k);
        break;

    case K_NOT:     /* no flags: FA/FB/FR stay the last producer's */
        if (!in->m.is_reg) e_ea(c, &in->m, 1, L_LA);
        e_rm_read(c, in, sz); e_i32(c, 0xffffffffu); OP(W_I32_XOR);
        e_set(c, L_V);
        e_rm_write(c, in, L_V, sz, k);
        break;

    case K_NEG:     /* alu(sub, 0, a) */
        if (!in->m.is_reg) e_ea(c, &in->m, 1, L_LA);
        e_rm_read(c, in, sz); e_set(c, L_T2);
        e_i32(c, 0); e_set(c, L_T1);
        e_alu(c, 5, sz);
        e_rm_write(c, in, L_FR, sz, k);
        break;

    case K_SHIFT:   /* the interpreter's do_shift, flags and all */
        if (!in->m.is_reg) e_ea(c, &in->m, 1, L_LA);
        e_rm_read(c, in, sz); e_set(c, L_T1);
        e_get(c, L_S); e_i32(c, in->op); e_get(c, L_T1);
        if (in->form) e_getreg8(c, REG_ECX);    /* CL */
        else e_i32(c, in->imm & 0xff);
        e_i32(c, sz);
        e_call(c, F_SHIFT); e_set(c, L_FR);
        c->cc_known = 0;                        /* the flags are eager now */
        e_rm_write(c, in, L_FR, sz, k);
        break;

    case K_MULDIV:  /* group3 mul/imul/div/idiv: EAX/EDX in place; may #DE */
        if (!in->m.is_reg) e_ea(c, &in->m, 1, L_LA);
        e_rm_read(c, in, sz); e_set(c, L_T1);
        e_may_fault(c, in);
        e_get(c, L_S); e_i32(c, in->op); e_get(c, L_T1); e_i32(c, sz);
        e_call(c, F_MULDIV); OP(W_DROP);
        c->cc_known = 0;
        break;

    case K_IMUL:    /* r32 = a * b, CF/OF as the interpreter sets them */
        if (!in->m.is_reg) e_ea(c, &in->m, 1, L_LA);
        e_rm_read(c, in, 4); e_set(c, L_T2);
        if (in->form) e_i32(c, in->imm);
        else e_getreg(c, in->reg);
        e_set(c, L_T1);
        e_get(c, L_S); e_get(c, L_T1); e_get(c, L_T2);
        e_call(c, F_IMUL); e_set(c, L_FR);
        e_setreg(c, in->reg, L_FR);
        c->cc_known = 0;
        break;

    case K_CMOV:    /* the source is read whatever the condition */
        if (!in->m.is_reg) e_ea(c, &in->m, 1, L_LA);
        e_rm_read(c, in, 4); e_set(c, L_V);
        e_cond(c, in->cc);
        OP(W_IF); OP(BT_VOID);
          e_setreg(c, in->reg, L_V);
        OP(W_END);
        break;

    case K_SETCC:
        if (!in->m.is_reg) e_ea(c, &in->m, 1, L_LA);
        e_cond(c, in->cc); e_i32(c, 0); OP(W_I32_NE); e_set(c, L_V);
        e_rm_write(c, in, L_V, 1, k);
        break;

    case K_XCHG:    /* as the interpreter: read both, write r/m, then reg */
        if (!in->m.is_reg) e_ea(c, &in->m, 1, L_LA);
        e_rm_read(c, in, sz); e_set(c, L_T1);
        e_getreg_sz(c, in->reg, sz); e_set(c, L_T2);
        if (in->m.is_reg) {
            e_setreg_sz(c, in->m.rm, L_T2, sz);
            e_setreg_sz(c, in->reg, L_T1, sz);
        } else {
            /* the instruction must finish before a store that dropped
             * compiled code makes the block leave */
            e_i32(c, 0); e_set(c, L_AB);
            e_store(c, in, L_T2, sz);
            e_setreg_sz(c, in->reg, L_T1, sz);
            e_abort_check(c, in, k);
        }
        break;

    case K_CWDE:
        e_getreg(c, REG_EAX); e_i32(c, 16); OP(W_I32_SHL); e_i32(c, 16); OP(W_I32_SHR_S);
        e_set(c, L_V); e_setreg(c, REG_EAX, L_V);
        break;

    case K_CDQ:
        e_getreg(c, REG_EAX); e_i32(c, 31); OP(W_I32_SHR_S);
        e_set(c, L_V); e_setreg(c, REG_EDX, L_V);
        break;

    case K_LEAVE:   /* ESP = EBP first, then pop EBP, as the interpreter */
        e_getreg(c, REG_EBP); e_set(c, L_T1);
        e_setreg(c, REG_ESP, L_T1);
        e_get(c, L_T1);
        e_ld_s(c, OFF_SEGBASE(X86_CPU_SEG_SS)); OP(W_I32_ADD); e_set(c, L_LA);
        e_load(c, in, 4); e_set(c, L_V);
        e_get(c, L_T1); e_i32(c, 4); OP(W_I32_ADD); e_set(c, L_T1);
        e_setreg(c, REG_ESP, L_T1);
        e_setreg(c, REG_EBP, L_V);
        break;

    case K_PUSH_RM:
        if (!in->m.is_reg) e_ea(c, &in->m, 1, L_LA);
        e_rm_read(c, in, 4); e_set(c, L_V);
        e_getreg(c, REG_ESP); e_i32(c, 4); OP(W_I32_SUB); e_tee(c, L_T1);
        e_ld_s(c, OFF_SEGBASE(X86_CPU_SEG_SS)); OP(W_I32_ADD); e_set(c, L_LA);
        e_i32(c, 0); e_set(c, L_AB);
        e_store(c, in, L_V, 4);
        e_setreg(c, REG_ESP, L_T1);
        e_abort_check(c, in, k);
        break;

    case K_JMP_IND:
        if (!in->m.is_reg) e_ea(c, &in->m, 1, L_LA);
        e_rm_read(c, in, 4); e_set(c, L_V);
        e_st_s(c, OFF(eip), L_V);
        e_exit_tail(c, k + 1);
        break;

    case K_CALL_IND:    /* the target is read before the push, as the interpreter */
        if (!in->m.is_reg) e_ea(c, &in->m, 1, L_LA);
        e_rm_read(c, in, 4); e_set(c, L_T2);
        e_eip(c, in->next); e_set(c, L_V);
        e_getreg(c, REG_ESP); e_i32(c, 4); OP(W_I32_SUB); e_tee(c, L_T1);
        e_ld_s(c, OFF_SEGBASE(X86_CPU_SEG_SS)); OP(W_I32_ADD); e_set(c, L_LA);
        e_store(c, in, L_V, 4);
        e_setreg(c, REG_ESP, L_T1);
        e_st_s(c, OFF(eip), L_T2);
        e_exit_tail(c, k + 1);
        break;

    case K_PUSH:
    case K_PUSHI:
        if (in->kind == K_PUSH) e_getreg(c, in->reg); else e_i32(c, in->imm);
        e_set(c, L_V);
        e_getreg(c, REG_ESP); e_i32(c, 4); OP(W_I32_SUB); e_tee(c, L_T1);
        e_ld_s(c, OFF_SEGBASE(X86_CPU_SEG_SS)); OP(W_I32_ADD); e_set(c, L_LA);
        e_i32(c, 0); e_set(c, L_AB);
        e_store(c, in, L_V, 4);
        e_setreg(c, REG_ESP, L_T1);             /* commit only after the store */
        e_abort_check(c, in, k);
        break;

    case K_POP:
        e_getreg(c, REG_ESP);
        e_ld_s(c, OFF_SEGBASE(X86_CPU_SEG_SS)); OP(W_I32_ADD); e_set(c, L_LA);
        e_load(c, in, 4); e_set(c, L_V);
        e_getreg(c, REG_ESP); e_i32(c, 4); OP(W_I32_ADD); e_set(c, L_T1);
        e_setreg(c, REG_ESP, L_T1);
        e_setreg(c, in->reg, L_V);              /* pop %esp: the loaded value wins */
        break;

    case K_JCC:
        e_cond(c, in->cc);
        OP(W_IF); OP(BT_VOID);
        c->depth++; c->ldepth++;
        e_goto(c, in->target, k + 1, 0);
        c->depth--; c->ldepth--;
        OP(W_END);
        e_goto(c, in->next, k + 1, 0);
        break;

    case K_JMP:
        e_goto(c, in->target, k + 1, 0);
        break;

    case K_CALL:
        e_eip(c, in->next); e_set(c, L_V);
        e_getreg(c, REG_ESP); e_i32(c, 4); OP(W_I32_SUB); e_tee(c, L_T1);
        e_ld_s(c, OFF_SEGBASE(X86_CPU_SEG_SS)); OP(W_I32_ADD); e_set(c, L_LA);
        e_i32(c, 0); e_set(c, L_AB);
        e_store(c, in, L_V, 4);
        e_setreg(c, REG_ESP, L_T1);
        /* the push may have dropped this code: leave, at the call's target */
        e_get(c, L_AB);
        OP(W_IF); OP(BT_VOID);
          e_exit(c, in->target, k + 1);
        OP(W_END);
        e_goto(c, in->target, k + 1, 0);
        break;

    case K_RET:
        e_getreg(c, REG_ESP);
        e_ld_s(c, OFF_SEGBASE(X86_CPU_SEG_SS)); OP(W_I32_ADD); e_set(c, L_LA);
        e_load(c, in, 4); e_set(c, L_V);
        e_getreg(c, REG_ESP); e_i32(c, 4); OP(W_I32_ADD); e_set(c, L_T1);
        e_setreg(c, REG_ESP, L_T1);
        e_st_s(c, OFF(eip), L_V);
        e_exit_tail(c, k + 1);
        break;

    case K_STEP:
        e_step(c, in, k);
        c->cc_known = 0;                        /* it may have set any flags */
        break;

    case K_CLD:     /* DF is never deferred: set it in place */
        e_get(c, L_S);
        e_ld_s(c, OFF(eflags_));
        if (in->op) { e_i32(c, EFL_DF); OP(W_I32_OR); }
        else { e_i32(c, ~(uint32_t)EFL_DF); OP(W_I32_AND); }
        e_mem(c, W_I32_STORE, 2, OFF(eflags_));
        break;

    /* The SSE2 moves: native when SSE is usable (sse_check's conditions),
     * else the interpreter runs it and raises what sse_check raises. They
     * set no flags, so the record (and cc_known) stays. */
    case K_MOVD_X:
        e_sse_ok(c);
        OP(W_IF); OP(BT_VOID);
          if (in->m.is_reg) e_getreg(c, in->m.rm);
          else { e_ea(c, &in->m, 1, L_LA); e_load(c, in, 4); }
          e_set(c, L_V);
          e_st_s(c, OFF_XMM(in->reg), L_V);
          e_st_s_const(c, OFF_XMM(in->reg) + 4, 0);
          e_st_s_const(c, OFF_XMM(in->reg) + 8, 0);
          e_st_s_const(c, OFF_XMM(in->reg) + 12, 0);
        OP(W_ELSE);
          e_step(c, in, k);
        OP(W_END);
        break;

    case K_PUNPCKLDQ:   /* {d0, s0, d1, s1}; all read before any is written */
        e_sse_ok(c);
        OP(W_IF); OP(BT_VOID);
          e_ld_s(c, OFF_XMM(in->reg) + 4); e_set(c, L_T1);     /* d1 */
          e_ld_s(c, OFF_XMM(in->m.rm)); e_set(c, L_T2);       /* s0 */
          e_ld_s(c, OFF_XMM(in->m.rm) + 4); e_set(c, L_V);    /* s1 */
          e_st_s(c, OFF_XMM(in->reg) + 4, L_T2);
          e_st_s(c, OFF_XMM(in->reg) + 8, L_T1);
          e_st_s(c, OFF_XMM(in->reg) + 12, L_V);
        OP(W_ELSE);
          e_step(c, in, k);
        OP(W_END);
        break;

    case K_MOVQ_ST:
        e_sse_ok(c);
        OP(W_IF); OP(BT_VOID);
        if (in->m.is_reg) {
            e_get(c, L_S);
            e_get(c, L_S); e_mem(c, W_I64_LOAD, 3, OFF_XMM(in->reg));
            e_mem(c, W_I64_STORE, 3, OFF_XMM(in->m.rm));
            e_get(c, L_S); OP(W_I64_CONST); b_sleb(&c->c, 0);
            e_mem(c, W_I64_STORE, 3, OFF_XMM(in->m.rm) + 8);
        } else {
            /* both dwords in one page the TLB can write: one store; else
             * (a miss, a page edge) the interpreter, which probes both */
            uint32_t tlb = OFF_TLBW(c->mi);
            e_ea(c, &in->m, 1, L_LA);
            e_tlb_slot(c);
            e_get(c, L_LA); e_get(c, L_E); e_mem(c, W_I32_LOAD, 2, tlb); OP(W_I32_XOR);
            e_i32(c, 0x1000u - 8); OP(W_I32_LE_U);
            OP(W_IF); OP(BT_VOID);
              e_get(c, L_E); e_mem(c, W_I32_LOAD, 2, tlb + 4);
              e_get(c, L_LA); OP(W_I32_ADD);
              e_get(c, L_S); e_mem(c, W_I64_LOAD, 3, OFF_XMM(in->reg));
              e_mem(c, W_I64_STORE, 0, 0);
            OP(W_ELSE);
              e_step(c, in, k);
            OP(W_END);
        }
        OP(W_ELSE);
          e_step(c, in, k);
        OP(W_END);
        break;
    }
}

/* Wrap function bodies into one module: imports e.m (memory) and e.l/e.s/
 * e.c/e.f (the C helpers); exports the n block functions as "0".."n-1".
 * `bodies` holds the n code-section entries (size-prefixed) back to back. */
static void build_module(Buf *mod, const Buf *bodies, int n)
{
    static const uint8_t hdr[8] = { 0, 'a', 's', 'm', 1, 0, 0, 0 };
    Buf sec = { 0 };
    int i;

    b_bytes(mod, hdr, 8);

    /* types: 0 (i32 i32 i32)->i32, 1 (i32 i32 i32 i32)->i32,
     *        2 (i32 i32)->i32,     3 (i32)->i32, 4 (i32 x5)->i32 */
    b_uleb(&sec, 5);
    b_byte(&sec, 0x60); b_uleb(&sec, 3); b_byte(&sec, 0x7f); b_byte(&sec, 0x7f); b_byte(&sec, 0x7f);
    b_uleb(&sec, 1); b_byte(&sec, 0x7f);
    b_byte(&sec, 0x60); b_uleb(&sec, 4); b_byte(&sec, 0x7f); b_byte(&sec, 0x7f); b_byte(&sec, 0x7f); b_byte(&sec, 0x7f);
    b_uleb(&sec, 1); b_byte(&sec, 0x7f);
    b_byte(&sec, 0x60); b_uleb(&sec, 2); b_byte(&sec, 0x7f); b_byte(&sec, 0x7f);
    b_uleb(&sec, 1); b_byte(&sec, 0x7f);
    b_byte(&sec, 0x60); b_uleb(&sec, 1); b_byte(&sec, 0x7f);
    b_uleb(&sec, 1); b_byte(&sec, 0x7f);
    b_byte(&sec, 0x60); b_uleb(&sec, 5);
    for (i = 0; i < 5; i++) b_byte(&sec, 0x7f);
    b_uleb(&sec, 1); b_byte(&sec, 0x7f);
    b_byte(mod, 1); b_uleb(mod, sec.n); b_bytes(mod, sec.p, sec.n); sec.n = 0;

    /* imports, in F_* order after the memory */
    b_uleb(&sec, jit_tail ? 11 : 10);
    b_name(&sec, "e"); b_name(&sec, "m"); b_byte(&sec, 0x02); b_byte(&sec, 0x00); b_uleb(&sec, 0);
    if (jit_tail) {     /* the engine's function table: funcref, no minimum */
        b_name(&sec, "e"); b_name(&sec, "T"); b_byte(&sec, 0x01); b_byte(&sec, 0x70);
        b_byte(&sec, 0x00); b_uleb(&sec, 0);
    }
    b_name(&sec, "e"); b_name(&sec, "l"); b_byte(&sec, 0x00); b_uleb(&sec, 0);
    b_name(&sec, "e"); b_name(&sec, "s"); b_byte(&sec, 0x00); b_uleb(&sec, 1);
    b_name(&sec, "e"); b_name(&sec, "c"); b_byte(&sec, 0x00); b_uleb(&sec, 2);
    b_name(&sec, "e"); b_name(&sec, "f"); b_byte(&sec, 0x00); b_uleb(&sec, 3);
    b_name(&sec, "e"); b_name(&sec, "h"); b_byte(&sec, 0x00); b_uleb(&sec, 4);
    b_name(&sec, "e"); b_name(&sec, "d"); b_byte(&sec, 0x00); b_uleb(&sec, 1);
    b_name(&sec, "e"); b_name(&sec, "i"); b_byte(&sec, 0x00); b_uleb(&sec, 0);
    b_name(&sec, "e"); b_name(&sec, "t"); b_byte(&sec, 0x00); b_uleb(&sec, 2);
    b_name(&sec, "e"); b_name(&sec, "n"); b_byte(&sec, 0x00); b_uleb(&sec, 2);
    b_byte(mod, 2); b_uleb(mod, sec.n); b_bytes(mod, sec.p, sec.n); sec.n = 0;

    /* functions: page functions are (state, block) -> instructions run */
    b_uleb(&sec, n);
    for (i = 0; i < n; i++) b_uleb(&sec, 2);
    b_byte(mod, 3); b_uleb(mod, sec.n); b_bytes(mod, sec.p, sec.n); sec.n = 0;

    /* exports "0".."n-1" */
    b_uleb(&sec, n);
    for (i = 0; i < n; i++) {
        char name[12];
        snprintf(name, sizeof(name), "%d", i);
        b_name(&sec, name); b_byte(&sec, 0x00); b_uleb(&sec, F_BLOCK + i);
    }
    b_byte(mod, 7); b_uleb(mod, sec.n); b_bytes(mod, sec.p, sec.n); sec.n = 0;

    /* code */
    b_uleb(&sec, n);
    b_bytes(&sec, bodies->p, bodies->n);
    b_byte(mod, 10); b_uleb(mod, sec.n); b_bytes(mod, sec.p, sec.n);
    free(sec.p);
}

/* ---- page table -------------------------------------------------------- */

static inline uint32_t set_of(uint32_t hbase)
{
    return (hbase * 0x9e3779b1u) >> (32 - PG_SET_BITS);
}
static inline uint32_t hot_of(uint32_t la, uint32_t host)
{
    return ((la ^ (host * 0x85ebca6bu)) * 0x9e3779b1u) >> (32 - JIT_HOT_BITS);
}

/* index of the block at page offset `off`, or -1 */
static int page_block(const JitPage *p, uint32_t off)
{
    int lo = 0, hi = p->nblocks - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (p->blocks[mid] == off) return mid;
        if (p->blocks[mid] < off) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

static int has_sorted(const uint16_t *a, int n, uint32_t off)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (a[mid] == off) return 1;
        if (a[mid] < off) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

static int has_ofs(const uint16_t *a, int n, uint32_t off)
{
    int i;
    for (i = 0; i < n; i++)
        if (a[i] == off) return 1;
    return 0;
}

static void page_link(JitPage *p, uint32_t ppage)
{
    if (p->linked)
        return;
    p->ppage = ppage;
    p->pnext = jit_page_head[ppage & (JIT_PAGE_HASH - 1)];
    jit_page_head[ppage & (JIT_PAGE_HASH - 1)] = (uint32_t)(p - jit_tab) + 1;
    p->linked = 1;
}

static void page_unlink(JitPage *p)
{
    uint32_t idx = (uint32_t)(p - jit_tab) + 1;
    uint32_t *pp = &jit_page_head[p->ppage & (JIT_PAGE_HASH - 1)];
    if (!p->linked)
        return;
    while (*pp) {
        if (*pp == idx) { *pp = p->pnext; break; }
        pp = &jit_tab[*pp - 1].pnext;
    }
    p->pnext = 0;
    p->linked = 0;
}

/* Drop the page's code, compiled or still compiling: a compiling result is
 * thrown away when it arrives, since its generation no longer matches. */
static void page_drop_code(JitPage *p)
{
    if (p->fn) x86_jit_js_free((int)p->fn);
    p->fn = 0;
    p->pending = 0;
    p->gen++;
    page_unlink(p);
}

static int host_to_phys(X86CPUState *s, uint32_t host, uint32_t *pa)
{
    PhysMemoryMap *map = s->mem_map;
    int i;
    for (i = 0; i < map->n_phys_mem_range; i++) {
        PhysMemoryRange *pr = &map->phys_mem_range[i];
        uintptr_t h0 = (uintptr_t)pr->phys_mem;
        if (!pr->is_ram || !pr->phys_mem || !pr->size)
            continue;
        if (host >= h0 && host < h0 + pr->size) {
            *pa = (uint32_t)(pr->addr + (host - h0));
            return 1;
        }
    }
    return 0;
}

void x86_jit_invalidate_page(X86CPUState *s, uint32_t pa)
{
    uint32_t page = pa >> 12;
    uint32_t *pp = &jit_page_head[page & (JIT_PAGE_HASH - 1)];

    x86_jit_code_bits[page >> 5] &= ~(1u << (page & 31));
    while (*pp) {
        JitPage *p = &jit_tab[*pp - 1];
        if (p->ppage == page) {
            *pp = p->pnext;
            p->pnext = 0;
            p->linked = 0;
            if (p->fn) x86_jit_js_free((int)p->fn);
            p->fn = 0;
            p->pending = 0;
            p->gen++;
            /* the bytes changed: what was known about the page is stale */
            p->nblocks = p->nwant = p->nbad = 0;
            p->compiles = 0;
            /* the record stays, so a page rewritten again and again is
             * eventually left to the interpreter */
            if (++p->inval >= JIT_MAX_INVAL)
                p->bad = 1;
            x86_jit_stat_invalidated++;
        } else {
            pp = &p->pnext;
        }
    }
    s->jit_abort = 1;
}

/* ---- page compiler ----------------------------------------------------- */

/* Decode the block at page offset `off` of `p`. Returns the instruction
 * count; *why: 0 ended by a terminator, 1 by an instruction it doesn't
 * handle, 2 by the page end or the length cap, 3 by reaching another of
 * `stops` (sorted; NULL: none), which the block then branches to. */
static int decode_block(const JitPage *p, uint32_t off, Insn *ins, int *why,
                        const uint16_t *stops, int nstops)
{
    const uint8_t *q = (const uint8_t *)(uintptr_t)(p->hbase + off);
    int avail = 0x1000 - (int)off, n = 0;
    uint32_t eip = off;                 /* page-relative: see e_eip */

    *why = 2;
    while (n < JIT_MAX_INSNS) {
        if (n > 0 && stops && has_sorted(stops, nstops, eip & 0xfff)) {
            *why = 3;
            return n;
        }
        int len = decode(q, avail, eip, &ins[n]);
        if (!len) {
            *why = (avail > 15) ? 1 : 2;  /* near the end: may just not fit */
            return n;
        }
        q += len; avail -= len; eip += len;
        if (ins[n++].term) { *why = 0; return n; }
    }
    return n;
}

static void sorted_insert(uint16_t *a, int *n, int max, uint16_t v)
{
    int i;
    if (*n >= max || has_ofs(a, *n, v))
        return;
    for (i = *n; i > 0 && a[i - 1] > v; i--)
        a[i] = a[i - 1];
    a[i] = v;
    (*n)++;
}

/* Insn arrays are large; one static scratch area serves the compiler. */
static Insn jit_ins[JIT_MAX_INSNS];
static Buf  jit_bodies;        /* code-section entries of the open batch */

/* Choose the page's blocks: the ones it has, the hot ones asked for, and
 * everything they branch to inside the page, up to the caps. Fills
 * starts[] (sorted); returns their number.
 *
 * (A sweep that also took the code after every ret/jmp, to the end of the
 * page, was tried, for every page and for pages recompiled more than twice:
 * fewer recompiles, but the much larger functions made every workload
 * slower -- each new function also starts in V8's baseline tier and is
 * optimized only after it has run for a while.) */
static int page_starts(JitPage *p, uint16_t *starts)
{
    uint16_t work[PG_MAX_BLOCKS * 2];
    int nw = 0, ns = 0, total = 0, i, why, n;

    for (i = 0; i < p->nblocks; i++) work[nw++] = p->blocks[i];
    for (i = 0; i < p->nwant && nw < (int)(sizeof(work) / 2); i++) work[nw++] = p->want[i];

    while (nw > 0 && ns < PG_MAX_BLOCKS && total < PG_MAX_INSNS) {
        uint32_t off = work[--nw];
        const Insn *last;
        if (has_ofs(starts, ns, off) || has_ofs(p->badofs, p->nbad, off))
            continue;
        /* stop at known blocks: what follows them has been looked at */
        n = decode_block(p, off, jit_ins, &why, starts, ns);
        if (n == 0) {
            if (p->nbad < PG_MAX_BAD) p->badofs[p->nbad++] = (uint16_t)off;
            continue;
        }
        sorted_insert(starts, &ns, PG_MAX_BLOCKS, (uint16_t)off);
        total += n;
        last = &jit_ins[n - 1];
#define ADD(t) do { uint32_t _t = (t); \
            if ((_t & ~0xfffu) == 0 && nw < (int)(sizeof(work) / 2)) \
                work[nw++] = (uint16_t)(_t & 0xfff); } while (0)
        if (last->term) {
            if (last->kind == K_JCC || last->kind == K_JMP || last->kind == K_CALL)
                ADD(last->target);
            if (last->kind == K_JCC || last->kind == K_CALL || last->kind == K_CALL_IND)
                ADD(last->next);            /* a call's return lands there */
        } else if (why == 2) {
            ADD(last->next);                /* the length cap: carry on */
        }
#undef ADD
    }
    return ns;
}

/* Generate the page's function into jit_bodies. */
static void gen_page(JitPage *p, const uint16_t *starts, int ns)
{
    Jc c;
    int b, i, n, why;

    memset(&c, 0, sizeof(c));
    c.mi = p->mi;
    c.page = 0;
    c.starts = starts;
    c.nstarts = ns;

    b_uleb(&c.c, 1);                            /* one local group */
    b_uleb(&c.c, L_COUNT - N_PARAMS); b_byte(&c.c, 0x7f);
    /* L_PB: the page's linear base, from the entry EIP (in this page) */
    e_ld_s(&c, OFF(eip)); e_i32(&c, ~0xfffu); OP_C(&c, W_I32_AND); e_set(&c, L_PB);
    b_byte(&c.c, W_LOOP); b_byte(&c.c, BT_VOID);
    for (b = 0; b < ns; b++) { b_byte(&c.c, W_BLOCK); b_byte(&c.c, BT_VOID); }
    e_get(&c, L_B);
    b_byte(&c.c, W_BR_TABLE);
    b_uleb(&c.c, ns);
    for (b = 0; b < ns; b++) b_uleb(&c.c, b);
    b_uleb(&c.c, 0);                            /* default: block 0 */
    for (b = 0; b < ns; b++) {
        b_byte(&c.c, W_END);                    /* block b's code follows */
        b_byte(&c.c, W_LOOP); b_byte(&c.c, BT_VOID);  /* its own loop */
        c.depth = ns - 1 - b + 1;
        c.ldepth = 0;
        c.self = b;
        c.cc_known = 0;                         /* entered from anywhere */
        /* each instruction once: a block ends where the next one starts
         * and goes on into it */
        n = decode_block(p, starts[b], jit_ins, &why, starts, ns);
        for (i = 0; i < n; i++) {
            c.tail = (i == n - 1);
            e_insn(&c, &jit_ins[i], i);
        }
        if (!jit_ins[n - 1].term)
            e_goto(&c, jit_ins[n - 1].next, n, why == 1);
        c.tail = 0;
        b_byte(&c.c, W_END);                    /* its own loop */
    }
    b_byte(&c.c, W_END);                        /* the loop */
    b_byte(&c.c, W_UNREACHABLE);
    b_byte(&c.c, W_END);

    b_uleb(&jit_bodies, c.c.n);
    b_bytes(&jit_bodies, c.c.p, c.c.n);
    free(c.c.p);
}

/* ---- batches ------------------------------------------------------------
 *
 * A page to compile is generated at once and queued; a batch of pages
 * becomes one module, compiled by WebAssembly.instantiate() in the
 * background while the interpreter keeps going, and installed when it
 * arrives (x86_jit_installed). One synchronous module per block cost
 * ~0.3 ms each, and a boot compiles ~20000 blocks. The page bit is set when
 * the page is queued, so a write before the result arrives still drops it. */

#define JIT_BATCH_MAX    64
#define JIT_BATCH_BYTES  (256 * 1024)
#define JIT_SLOTS        4          /* batches compiling at once */

typedef struct {
    int      busy, n;
    uint32_t page[JIT_BATCH_MAX];   /* jit_tab index */
    uint32_t gen[JIT_BATCH_MAX];    /* page generation when queued */
    uint32_t idx[JIT_BATCH_MAX];    /* table indices, written by JS */
    /* the blocks of each new function: the page keeps running its current
     * one, with its own blocks, until this one is installed */
    uint16_t nblk[JIT_BATCH_MAX];
    uint16_t blk[JIT_BATCH_MAX][PG_MAX_BLOCKS];
} JitSlot;

static JitSlot  jit_slot[JIT_SLOTS];
static int      jit_open = -1;      /* slot being filled, or -1 */
static uint32_t jit_epoch;          /* bumped by x86_jit_forget */

/* Synchronous compiles (installed before x86_jit_run returns), for test
 * harnesses that never yield to the event loop. */
#ifndef X86_JIT_SYNC
#define X86_JIT_SYNC 0
#endif
int x86_jit_sync = X86_JIT_SYNC;

void x86_jit_forget(void)
{
    memset(jit_tab, 0, sizeof(jit_tab));
    memset(jit_rr, 0, sizeof(jit_rr));
    memset(jit_hot, 0, sizeof(jit_hot));
    memset(jit_page_head, 0, sizeof(jit_page_head));
    memset(x86_jit_code_bits, 0, sizeof(x86_jit_code_bits));
    memset(jit_slot, 0, sizeof(jit_slot));
    jit_bodies.n = 0;
    jit_open = -1;
    jit_epoch++;
    jit_state = 0;
}

/* Install a finished batch: every page whose generation is still the one it
 * was queued with gets its function; the rest are freed. */
static void slot_install(int k)
{
    JitSlot *sl = &jit_slot[k];
    int i;
    for (i = 0; i < sl->n; i++) {
        JitPage *p = &jit_tab[sl->page[i]];
        uint32_t idx = sl->idx[i];
        int live = p->pending && p->gen == sl->gen[i];
        if (live && idx) {
            /* the old function is not running: installs happen between
             * runs (from JS between ticks, or from x86_jit_run before it
             * calls anything) */
            if (p->fn) x86_jit_js_free((int)p->fn);
            p->pending = 0;
            p->fn = idx;
            p->nblocks = sl->nblk[i];
            memcpy(p->blocks, sl->blk[i], sl->nblk[i] * sizeof(p->blocks[0]));
            x86_jit_stat_compiled++;
            x86_jit_stat_blocks += p->nblocks;
        } else {
            if (idx) x86_jit_js_free((int)idx);
            if (live) {
                /* the module failed: leave this page to the interpreter */
                page_drop_code(p);
                p->bad = 1;
                x86_jit_stat_failed++;
            }
        }
    }
    sl->busy = 0;
    sl->n = 0;
}

/* Called from JS when a batch's module has been instantiated and its
 * functions written to idx[] (all 0 if compiling failed). */
EMSCRIPTEN_KEEPALIVE void x86_jit_installed(int k, uint32_t epoch)
{
    if (k < 0 || k >= JIT_SLOTS || epoch != jit_epoch || !jit_slot[k].busy)
        return;     /* from before a snapshot restore: its pages are gone */
    slot_install(k);
}

/* Send the open batch off to be compiled. A synchronous compile has filled
 * idx[] on return, and so has one that failed to start; an asynchronous one
 * installs from x86_jit_installed(). */
static void batch_submit(void)
{
    JitSlot *sl;
    Buf mod = { 0 };
    int k = jit_open, ok;

    if (k < 0)
        return;
    sl = &jit_slot[k];
    jit_open = -1;
    build_module(&mod, &jit_bodies, sl->n);
    jit_bodies.n = 0;
    ok = x86_jit_js_compile(mod.p, mod.n, sl->n, sl->idx, k, jit_epoch, x86_jit_sync);
    free(mod.p);
    if (!ok)
        memset(sl->idx, 0, sizeof(sl->idx[0]) * sl->n);
    if (!ok || x86_jit_sync)
        slot_install(k);
}

double x86_jit_blocks(void)
{
    return jit_state == 1 ? (double)x86_jit_stat_blocks : -1;
}

/* Compile whatever is queued; the run loop calls this at the end of a tick,
 * so a batch that never fills still gets compiled. */
void x86_jit_flush(void)
{
    if (jit_open >= 0 && jit_slot[jit_open].n)
        batch_submit();
}

/* (Re)compile page p with its blocks and the offsets it wants. Its current
 * function, if any, keeps running until the new one is installed (the
 * batch carries the new block list). Returns 0 if nothing could be queued. */
static int queue_page(X86CPUState *s, JitPage *p)
{
    uint16_t starts[PG_MAX_BLOCKS];
    JitSlot *sl;
    uint32_t pa, ppage;
    int k, ns;

    if (jit_open < 0) {
        for (k = 0; k < JIT_SLOTS && jit_slot[k].busy; k++)
            ;
        if (k == JIT_SLOTS)
            return 0;                   /* all compiling: try again later */
        jit_open = k;
        jit_slot[k].busy = 1;
        jit_slot[k].n = 0;
        jit_bodies.n = 0;
    }
    sl = &jit_slot[jit_open];
    if (!host_to_phys(s, p->hbase, &pa) || !(ns = page_starts(p, starts))) {
        p->nwant = 0;
        if (sl->n == 0) { sl->busy = 0; jit_open = -1; }
        return 0;
    }
    memcpy(sl->blk[sl->n], starts, ns * sizeof(starts[0]));
    sl->nblk[sl->n] = (uint16_t)ns;
    p->nwant = 0;
    p->compiles++;
    gen_page(p, starts, ns);

    ppage = pa >> 12;
    page_link(p, ppage);
    if (!(x86_jit_code_bits[ppage >> 5] & (1u << (ppage & 31)))) {
        /* From now on every write to this page must reach phys_write(). */
        x86_jit_code_bits[ppage >> 5] |= 1u << (ppage & 31);
        x86_cpu_flush_tlb_write_range_ram(s, NULL, 0);
    }
    p->pending = 1;
    sl->page[sl->n] = (uint32_t)(p - jit_tab);
    sl->gen[sl->n] = p->gen;
    sl->n++;
    if (x86_jit_sync || sl->n == JIT_BATCH_MAX || jit_bodies.n >= JIT_BATCH_BYTES)
        batch_submit();
    return 1;
}

/* A way for a new page: a free one, then one without code, then the set's
 * next in round-robin order. */
static JitPage *page_victim(uint32_t set)
{
    JitPage *w = &jit_tab[set * PG_WAYS];
    JitPage *p;
    int i;
    for (i = 0; i < PG_WAYS; i++)
        if (!w[i].used) return &w[i];
    for (i = 0; i < PG_WAYS; i++)
        if (!w[i].fn && !w[i].pending) return &w[i];
    p = &w[jit_rr[set]++ & (PG_WAYS - 1)];
    x86_jit_stat_evicted++;
    return p;
}

#ifdef X86_JIT_STATS
/* -DX86_JIT_STATS: histograms of the instructions blocks hand to the
 * interpreter, weighted by how often, printed to stderr every 2^20 of them:
 * where blocks stop (jit-stops: no length known, so the block ends) and
 * what x86_jit_step runs (jit-steps). Keys: the opcode byte; 0f:b for 0F b;
 * 66:, rep: (F2/F3) or seg: (segment/lock) for a prefixed b. Shows what to
 * compile next. */
typedef struct { const char *name; uint32_t total; uint32_t n[0x500]; } Hist;
static Hist stop_hist = { "jit-stops" }, step_hist = { "jit-steps" };

static void hist_note(Hist *h, const uint8_t *p, const uint8_t *end)
{
    static const char *const pfx[5] = { "", "0f:", "66:", "rep:", "seg:" };
    int key, i, j;
    if (end - p < 2)
        return;
    if (p[0] == 0x0f) key = 0x100 + p[1];
    else if (p[0] == 0x66) key = 0x200 + (p[1] == 0x0f && end - p > 2 ? p[2] : p[1]);
    else if (p[0] == 0xf2 || p[0] == 0xf3) key = 0x300 + p[1];
    else if (p[0] == 0x26 || p[0] == 0x2e || p[0] == 0x36 || p[0] == 0x3e || p[0] == 0x64 ||
             p[0] == 0x65 || p[0] == 0xf0) key = 0x400 + p[1];
    else key = p[0];
    h->n[key]++;
    if ((++h->total & 0xfffff) != 0)
        return;
    fprintf(stderr, "[%s] %u:", h->name, h->total);
    for (j = 0; j < 20; j++) {
        int best = -1;
        for (i = 0; i < 0x500; i++)
            if (h->n[i] && (best < 0 || h->n[i] > h->n[best]))
                best = i;
        if (best < 0) break;
        fprintf(stderr, " %s%02x=%u", pfx[best >> 8], best & 0xff, h->n[best]);
        h->n[best] = 0;
    }
    fprintf(stderr, "\n");
    memset(h->n, 0, sizeof(h->n));
}
void x86_jit_note_stop(const uint8_t *p, const uint8_t *end) { hist_note(&stop_hist, p, end); }
void x86_jit_note_step(const uint8_t *p, const uint8_t *end) { hist_note(&step_hist, p, end); }
#endif

typedef int (*JitFn)(X86CPUState *s, int block);

/* The compiled block for EIP, if its page has one: what x86_jit_run looks
 * up first, without the hotness counting. Returns the function's table
 * index, or 0. (A direct-mapped cache in front of this, keyed by target
 * host address, was measured: no difference, so the lookup is not what an
 * exit costs.) */
static inline uint32_t chain_lookup(X86CPUState *s, int *b)
{
    const X86Seg *cs = &s->seg[X86_CPU_SEG_CS];
    const X86TLBEntry *te;
    uint32_t la = s->eip, hbase;
    JitPage *w;
    int mi, i;

    if (cs->base != 0 || !(cs->flags & 0x4000))
        return 0;
    mi = (cs->sel & 3) == 3;
    te = &s->tlb_code[mi][(la >> 12) & (X86_TLB_SIZE - 1)];
    if ((la ^ te->vaddr) > 0xfffu)
        return 0;
    hbase = (uint32_t)(te->mem_addend + la) - (la & 0xfff);
    w = &jit_tab[set_of(hbase) * PG_WAYS];
    for (i = 0; i < PG_WAYS; i++)
        if (w[i].hbase == hbase && w[i].mi == mi && w[i].used) {
            if (!w[i].fn || (*b = page_block(&w[i], la & 0xfff)) < 0)
                return 0;
            return w[i].fn;
        }
    return 0;
}

/* Whether a chain must hand back to the interpreter: a deliverable
 * interrupt, an interrupt shadow, HLT, an instruction a block stopped before
 * (jit_poke), or the end of the run's budget. */
static inline int chain_stop(X86CPUState *s, int total)
{
    return total >= JIT_LOOP_LIMIT || !x86_jit_chain || s->halted || s->jit_poke ||
           s->int_shadow || (s->hard_irq && (s->eflags_ & EFL_IF));
}

/* Called by a page function's exit (tail-call builds): n instructions run in
 * it. Returns the next function's table index << 8 | its block, to jump to,
 * or 0 to return to run_chain. */
EMSCRIPTEN_KEEPALIVE uint32_t x86_jit_next(X86CPUState *s, int n)
{
    uint32_t fn;
    int b;
    if (chain_stop(s, s->jit_chain_n + n) || !(fn = chain_lookup(s, &b)))
        return 0;
    s->jit_chain_n += n;            /* the caller returns only the last one's */
    return fn << 8 | (uint32_t)b;
}

/* Run page p's block b, then -- as long as the next EIP has compiled code
 * too -- the next page's, and so on, without going back to the interpreter
 * loop. Almost every exit from a page function is to another page that
 * already has code for the target (a call or return across pages, an
 * indirect jump), and going round the interpreter loop for each one cost
 * more than the dozen instructions a run averages. The chain stops where the
 * interpreter has something to do: a deliverable interrupt, an interrupt
 * shadow, HLT, an instruction a block stopped before (jit_poke), or the end
 * of the run's budget (JIT_LOOP_LIMIT, as for one function). */
static int run_chain(X86CPUState *s, JitPage *p, int b)
{
    uint32_t fn = p->fn;
    int total = 0;
    for (;;) {
        s->jit_chain_n = 0;
        total += ((JitFn)(uintptr_t)fn)(s, b);
        total += s->jit_chain_n;        /* the functions it tail-called */
        if (chain_stop(s, total) || !(fn = chain_lookup(s, &b)))
            return total;
    }
}

#ifdef X86_JIT_STATS
/* -DX86_JIT_STATS also profiles x86_jit_run: how many instructions compiled
 * code runs per call and in all, why a call runs nothing (the interpreter
 * then runs to the next branch target), and which pages those land on.
 * (Timing single calls doesn't work: performance.now() ticks every 100 us
 * in a page that isn't cross-origin isolated, with jitter, and sampled
 * call times came out larger than the whole.) */
enum { WHY_RAN, WHY_CS, WHY_TLB, WHY_NOBLOCK, WHY_BAD, WHY_COLD, WHY_WAIT, WHY_POKE, WHY_N };
static const char *const why_name[WHY_N] = { "ran", "cs", "tlb", "noblock", "bad", "cold", "wait", "poke" };
uint64_t x86_jit_prof_by[WHY_N];    /* x86_cpu.c: interpreted, by the last miss */
int x86_jit_prof_cause;
uint64_t x86_jit_prof_steps, x86_jit_prof_step_exits;   /* x86_jit_step() */
static uint32_t why_count[WHY_N];
static uint64_t prof_insns, prof_calls, prof_interp0;
static int64_t prof_cycles0;
static double prof_cpu0;
double x86_jit_prof_cpu_ms;         /* x86_machine.c: time running the CPU */
uint64_t x86_jit_prof_interp;       /* x86_cpu.c: instructions interpreted */
static int x86_jit_run1(X86CPUState *s, int *why);
/* the pages cold calls land on: linear page | mmu index, and a count */
#define COLD_TAB 256
static uint32_t cold_page[COLD_TAB], cold_n[COLD_TAB];
static uint32_t prof_inval0, prof_evict0, prof_comp0, prof_fail0;

static void note_cold(X86CPUState *s, int noblock)
{
    uint32_t key = (s->eip & ~0xfffu) | ((s->seg[X86_CPU_SEG_CS].sel & 3) == 3) | (noblock << 1);
    uint32_t h = (key * 0x9e3779b1u) >> 24;
    if (cold_page[h] != key) {
        if (cold_n[h] > 16) { cold_n[h] -= 16; return; }
        cold_page[h] = key;
        cold_n[h] = 0;
    }
    cold_n[h]++;
}

/* where runs leave to: [0] same page, a block there; [1] same page, no
 * block; [2] another page with a block for the target; [3] another page,
 * none (or no TLB entry) */
static uint32_t exit_kind[4];
static JitPage *exit_lookup(X86CPUState *s, uint32_t la, int *b);

int x86_jit_run(X86CPUState *s)
{
    int why = WHY_COLD, n;
    uint32_t page0 = s->eip & ~0xfffu;
    n = x86_jit_run1(s, &why);
    if (n > 0) {
        int b;
        JitPage *q = exit_lookup(s, s->eip, &b);
        int same = (s->eip & ~0xfffu) == page0;
        exit_kind[same ? (q && b >= 0 ? 0 : 1) : (q && b >= 0 ? 2 : 3)]++;
    }
    if (n > 0)
        prof_insns += (uint64_t)n;
    why_count[n > 0 ? WHY_RAN : why]++;
    x86_jit_prof_cause = n > 0 ? WHY_RAN : why;
    if (n <= 0 && (why == WHY_COLD || why == WHY_NOBLOCK))
        note_cold(s, why == WHY_NOBLOCK);
    if ((++prof_calls & 0x3fffff) == 0) {
        int64_t cyc = s->cycle_count - prof_cycles0;
        double cpu = x86_jit_prof_cpu_ms - prof_cpu0;
        uint64_t interp = x86_jit_prof_interp - prof_interp0;
        int i;
        fprintf(stderr, "[jit-prof] %.1f%% of %lld insns compiled, %.1f per run, %llu interpreted;"
                " cpu %.0f ms;",
                cyc > 0 ? 100.0 * (double)prof_insns / (double)cyc : 0.0, (long long)cyc,
                why_count[WHY_RAN] ? (double)prof_insns / why_count[WHY_RAN] : 0.0,
                (unsigned long long)interp, cpu);
        for (i = 0; i < WHY_N; i++)
            fprintf(stderr, " %s=%u", why_name[i], why_count[i]);
        fprintf(stderr, "; exits: same page block %u, same page none %u, other page block %u, other none %u",
                exit_kind[0], exit_kind[1], exit_kind[2], exit_kind[3]);
        memset(exit_kind, 0, sizeof(exit_kind));
        fprintf(stderr, "; steps %llu (left after %llu)",
                (unsigned long long)x86_jit_prof_steps, (unsigned long long)x86_jit_prof_step_exits);
        x86_jit_prof_steps = x86_jit_prof_step_exits = 0;
        fprintf(stderr, "; interpreted after:");
        for (i = 0; i < WHY_N; i++)
            fprintf(stderr, " %s=%llu", why_name[i], (unsigned long long)x86_jit_prof_by[i]);
        memset(x86_jit_prof_by, 0, sizeof(x86_jit_prof_by));
        fprintf(stderr, "; compiled %u invalidated %u evicted %u failed %u; cold pages:",
                x86_jit_stat_compiled - prof_comp0, x86_jit_stat_invalidated - prof_inval0,
                x86_jit_stat_evicted - prof_evict0, x86_jit_stat_failed - prof_fail0);
        fprintf(stderr, "\n  pages (st: compiles, or 100 pending / 200 bad; nb: blocks; noblock = entry at a non-block):");
        prof_comp0 = x86_jit_stat_compiled; prof_inval0 = x86_jit_stat_invalidated;
        prof_evict0 = x86_jit_stat_evicted; prof_fail0 = x86_jit_stat_failed;
        for (i = 0; i < 8; i++) {
            int j, k, st = -1, inv = -1, nb = -1, nw = -1, best = -1;
            uint32_t key;
            const JitPage *w;
            for (j = 0; j < COLD_TAB; j++)
                if (cold_n[j] && (best < 0 || cold_n[j] > cold_n[best])) best = j;
            if (best < 0) break;
            key = cold_page[best];
            w = jit_tab;
            for (k = 0; k < PG_TAB_SIZE; k++)
                if (w[k].used && w[k].lpage == (key & ~0xfffu) && w[k].mi == (key & 1)) {
                    st = w[k].pending ? 100 : w[k].bad ? 200 : w[k].compiles;
                    inv = w[k].inval; nb = w[k].nblocks; nw = w[k].nwant;
                }
            fprintf(stderr, " %s %08x/%u=%u(st%d,nb%d,want%d,inv%d)", (key & 2) ? "noblock" : "cold",
                    key & ~0xfffu, key & 1, cold_n[best], st, nb, nw, inv);
            cold_n[best] = 0;
        }
        memset(cold_n, 0, sizeof(cold_n));
        fprintf(stderr, "\n");
        memset(why_count, 0, sizeof(why_count));
        prof_insns = 0;
        prof_cycles0 = s->cycle_count;
        prof_cpu0 = x86_jit_prof_cpu_ms;
        prof_interp0 = x86_jit_prof_interp;
    }
    return n;
}
/* the page record and block for linear `la`, as x86_jit_run finds them */
static JitPage *exit_lookup(X86CPUState *s, uint32_t la, int *b)
{
    const X86TLBEntry *te;
    uint32_t hbase;
    JitPage *w;
    int mi = (s->seg[X86_CPU_SEG_CS].sel & 3) == 3, i;
    *b = -1;
    te = &s->tlb_code[mi][(la >> 12) & (X86_TLB_SIZE - 1)];
    if ((la ^ te->vaddr) > 0xfffu)
        return NULL;
    hbase = (uint32_t)(te->mem_addend + la) - (la & 0xfff);
    w = &jit_tab[set_of(hbase) * PG_WAYS];
    for (i = 0; i < PG_WAYS; i++)
        if (w[i].used && w[i].hbase == hbase && w[i].mi == mi) {
            if (w[i].fn) *b = page_block(&w[i], la & 0xfff);
            return &w[i];
        }
    return NULL;
}
#define WHY(w) (*why = (w))
static int x86_jit_run1(X86CPUState *s, int *why)
#else
#define WHY(w) ((void)0)
int x86_jit_run(X86CPUState *s)
#endif
{
    const X86Seg *cs = &s->seg[X86_CPU_SEG_CS];
    const X86TLBEntry *te;
    JitPage *p, *w;
    uint32_t la, host, off, lpage, hbase, set, h;
    int mi, i, b;

    if (unlikely(jit_state != 1)) {
        if (jit_state == 0) {
            /* 0: unavailable; -1: on with the default threshold; n: threshold */
            int t = x86_jit_js_init();
            jit_state = t ? 1 : 2;
            if (t > 0)
                x86_jit_threshold = (uint32_t)t;
            jit_tail = t && x86_jit_js_tailcalls();
        }
        if (jit_state != 1)
            return 0;
    }
    if (cs->base != 0 || !(cs->flags & 0x4000))
        return WHY(WHY_CS), 0;
    la = s->eip;
    mi = (cs->sel & 3) == 3;
    te = &s->tlb_code[mi][(la >> 12) & (X86_TLB_SIZE - 1)];
    if ((la ^ te->vaddr) > 0xfffu)
        return WHY(WHY_TLB), 0;
    host = (uint32_t)(te->mem_addend + la);
    off = la & 0xfff;
    lpage = la - off;
    hbase = host - off;

    set = set_of(hbase);
    w = &jit_tab[set * PG_WAYS];
    p = NULL;
    for (i = 0; i < PG_WAYS; i++) {
        if (w[i].hbase == hbase && w[i].mi == mi && w[i].used) {
            p = &w[i];
            if (p->fn && (b = page_block(p, off)) >= 0)
                return run_chain(s, p, b);
            if (p->bad)
                return WHY(WHY_BAD), 0;
            WHY(p->fn ? WHY_NOBLOCK : WHY_COLD);
            break;
        }
    }

    /* A new entry into a page that has code must be hotter: a recompile
     * costs the compile, and then the new function runs in V8's baseline
     * tier until it is optimized, and some pages (CPython's opcode
     * handlers, each entered through an indirect jump) would otherwise be
     * recompiled for every handler that warms up. (Not with threshold 1:
     * the test harnesses compile every block on first sight.) */
    h = hot_of(la, host);
    if (++jit_hot[h] < (p && p->fn && x86_jit_threshold > 1 ? JIT_RECOMPILE_HOT * x86_jit_threshold
                                                               : x86_jit_threshold))
        return 0;
    WHY(WHY_WAIT);
    jit_hot[h] = 0;
    if (!p) {
        uint32_t gen;
        p = page_victim(set);
        if (p->used)
            page_drop_code(p);
        /* keep the generation counting up across reuse, so a batch result
         * for the record's previous page can never match the new one */
        gen = p->gen;
        memset(p, 0, offsetof(JitPage, blocks));
        p->gen = gen;
        p->used = 1;
        p->lpage = lpage;
        p->hbase = hbase;
        p->mi = (uint8_t)mi;
    }
    if (has_ofs(p->badofs, p->nbad, off) || p->compiles >= PG_MAX_COMPILES)
        return 0;
    if (!has_ofs(p->want, p->nwant, off) && p->nwant < PG_MAX_WANT)
        p->want[p->nwant++] = (uint16_t)off;
    if (p->pending || !queue_page(s, p))
        return 0;
    if (p->fn && (b = page_block(p, off)) >= 0)    /* synchronous compiles */
        return run_chain(s, p, b);
    return 0;
}


#else /* !X86_JIT */

void x86_jit_forget(void)
{
}

void x86_jit_flush(void)
{
}

double x86_jit_blocks(void)
{
    return -1;
}

#endif /* X86_JIT */
