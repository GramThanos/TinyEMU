#!/bin/sh
# Checks for the x86 -> WebAssembly JIT (x86_jit.c), which exists only in
# Emscripten builds, so these run under node. Needs emcc and node on PATH
# (an emsdk install, or the emscripten/emsdk:3.1.74 image), run from the
# repository root:
#
#   sh tests/x86_jit_test.sh [fuzz-programs] [seed]
#
# 1. the x86 selftest with every block compiled on first sight (threshold 1,
#    synchronous compiles). Its only expected failure is the x87 RC test,
#    which fails in every WASM build: Emscripten ignores fesetround;
# 2. tests/x86_jit_fuzz.c: random programs run interpreted and compiled,
#    compared register for register and byte for byte;
# 3. both again with tail-call chaining (Module.x86jitTailcalls), which is
#    opt-in in the browser but must stay correct;
# 4. bench/microbench.c with and without the JIT.
set -e
N=${1:-2000}
SEED=${2:-1}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
echo 'Module.x86jit = true;' > "$OUT/jiton.js"
echo 'Module.x86jit = true; Module.x86jitTailcalls = true;' > "$OUT/jittail.js"

F="-O3 -flto -fno-strict-aliasing -sSUPPORT_LONGJMP=wasm -mbulk-memory -mnontrapping-fptoint -D_GNU_SOURCE -I."
L="-O3 -flto -sSUPPORT_LONGJMP=wasm -mbulk-memory -mnontrapping-fptoint -sALLOW_MEMORY_GROWTH=1 \
   -sALLOW_TABLE_GROWTH=1 -sENVIRONMENT=node --js-library wasm/x86jit.js --pre-js $OUT/jiton.js"

for f in iomem cutils x86_cpu; do emcc $F -c $f.c -o "$OUT/$f.o"; done
emcc $F -DX86_NO_JIT -c x86_cpu.c -o "$OUT/x86_nojit.o"
emcc $F -DX86_JIT_SYNC=1 -c x86_jit.c -o "$OUT/jit.o"
emcc $F -DX86_JIT_SYNC=1 -DX86_JIT_THRESHOLD=1 -c x86_jit.c -o "$OUT/jit1.o"
COMMON="$OUT/iomem.o $OUT/cutils.o -lm"

emcc $F $L -o "$OUT/st.js" tests/x86_cpu_selftest.c "$OUT/x86_cpu.o" "$OUT/jit1.o" $COMMON
emcc $F $L -o "$OUT/fuzz.js" tests/x86_jit_fuzz.c "$OUT/x86_cpu.o" "$OUT/jit.o" $COMMON
LT=$(echo "$L" | sed "s|jiton.js|jittail.js|")
emcc $F $LT -o "$OUT/st_t.js" tests/x86_cpu_selftest.c "$OUT/x86_cpu.o" "$OUT/jit1.o" $COMMON
emcc $F $LT -o "$OUT/fuzz_t.js" tests/x86_jit_fuzz.c "$OUT/x86_cpu.o" "$OUT/jit.o" $COMMON
emcc $F $L -o "$OUT/mb.js" bench/microbench.c "$OUT/x86_cpu.o" "$OUT/jit.o" $COMMON
emcc $F $L -o "$OUT/mb0.js" bench/microbench.c "$OUT/x86_nojit.o" "$OUT/jit.o" $COMMON

echo "== selftest, every block compiled"
node "$OUT/st.js" > "$OUT/st.txt" || true
FAILS=$(grep -c '\*\*\* FAIL' "$OUT/st.txt" || true)
grep '\*\*\* FAIL' "$OUT/st.txt" || true
if [ "$FAILS" != 1 ] || ! grep -q 'FAIL: round-up frndint' "$OUT/st.txt"; then
    echo "selftest: unexpected failures"; exit 1
fi
echo "selftest: only the expected WASM x87 RC failure"

echo "== JIT fuzz: $N programs, seed $SEED"
node "$OUT/fuzz.js" "$N" "$SEED"

echo "== tail-call chaining: selftest, fuzz ($((N / 4)) programs)"
node "$OUT/st_t.js" > "$OUT/st_t.txt" || true
if [ "$(grep -c '\*\*\* FAIL' "$OUT/st_t.txt" || true)" != 1 ]; then
    grep '\*\*\* FAIL' "$OUT/st_t.txt"; echo "selftest (tail calls): unexpected failures"; exit 1
fi
grep -q 'tail calls' "$OUT/st_t.txt" || { echo "tail calls not detected by this node"; exit 1; }
node "$OUT/fuzz_t.js" "$((N / 4))" "$((SEED + 1000))"

echo "== microbench (JIT, then interpreter)"
node "$OUT/mb.js" 100000000
node "$OUT/mb0.js" 30000000
