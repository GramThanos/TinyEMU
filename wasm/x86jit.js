/*
 * JS side of the x86 -> WebAssembly block JIT (x86_jit.c). Linked into the
 * engine with --js-library (Makefile.js) and needs -sALLOW_TABLE_GROWTH.
 *
 * The C side queues hot blocks and hands over a batch as one module whose
 * functions are exported as "0".."n-1". Each module imports the engine's
 * memory and seven of its exports (the interpreter's memory accessors and
 * flag, shift, mul/div and imul helpers), so its loads, stores and helper calls are direct
 * wasm-to-wasm, and a guest fault raised in a helper unwinds through a block
 * like any other call. The functions go into the engine's function table,
 * where C calls them through function pointers.
 *
 * Batches compile with WebAssembly.instantiate(), on the engine's background
 * threads, while the emulator keeps interpreting; the result is installed by
 * calling back into C (x86_jit_installed) between run-loop ticks.
 *
 * Module.x86jit = true before the engine starts turns it on; otherwise the
 * engine only interprets. The web runtime (EmulatorFactory) sets it unless
 * the VM config has x86jit: false (the agent page's VM tab, ?jit=0).
 * Module.x86jitThreshold = n: executions before a target is compiled.
 * Module.x86jitTailcalls = true: chain page functions with tail calls where
 * the browser has them (Chrome 112, Firefox 121, Safari 18.2). Off by
 * default: measured slower than chaining through C (x86_jit.c, x86_jit_next).
 * Module.x86jitStats: counters, filled in as batches arrive.
 */
mergeInto(LibraryManager.library, {
    $x86jitImports: null,
    $x86jitTail: 0,

    x86_jit_js_init__deps: ['$x86jitImports', '$x86jitTail'],
    x86_jit_js_init: function () {
        /* EmulatorFactory sets Module.x86jit (on unless the config says false) */
        if (typeof WebAssembly !== 'object' || Module['x86jit'] !== true)
            return 0;
        try {
            x86jitImports = {
                e: {
                    m: wasmMemory,
                    l: wasmExports['x86_jit_ld'],
                    s: wasmExports['x86_jit_st'],
                    c: wasmExports['x86_jit_test_cc'],
                    f: wasmExports['x86_jit_get_cf'],
                    h: wasmExports['x86_jit_shift'],
                    d: wasmExports['x86_jit_muldiv'],
                    i: wasmExports['x86_jit_imul'],
                    t: wasmExports['x86_jit_step'],
                    n: wasmExports['x86_jit_next'],
                },
            };
            /* tail calls: a module whose one function is `return_call 0` */
            var table = (typeof wasmTable !== 'undefined' && wasmTable) ||
                        wasmExports['__indirect_function_table'];
            x86jitTail = Module['x86jitTailcalls'] === true && !!table && !!x86jitImports.e.n &&
                WebAssembly.validate(new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0,
                    1, 4, 1, 0x60, 0, 0, 3, 2, 1, 0, 10, 6, 1, 4, 0, 0x12, 0, 0x0b])) ? 1 : 0;
            if (x86jitTail) x86jitImports.e.T = table;
            if (!x86jitImports.e.l || !x86jitImports.e.s || !x86jitImports.e.h || !x86jitImports.e.t)
                return 0;
            Module['x86jitStats'] = { batches: 0, blocks: 0, bytes: 0, failed: 0 };
            var t = Module['x86jitThreshold'];
            out('x86 jit: on' + (t > 0 ? ', threshold ' + t : '') + (x86jitTail ? ', tail calls' : ''));
            return t > 0 ? t : -1;
        } catch (e) {
            return 0;
        }
    },

    x86_jit_js_tailcalls__deps: ['$x86jitTail'],
    x86_jit_js_tailcalls: function () {
        return x86jitTail;
    },

    x86_jit_js_compile__deps: ['$x86jitImports', '$addFunction'],
    x86_jit_js_compile: function (ptr, len, n, idxPtr, slot, epoch, sync) {
        var bytes = HEAPU8.slice(ptr, ptr + len);       /* C reuses the buffer */
        var st = Module['x86jitStats'];
        function install(inst) {
            var ex = inst.exports, i, base = idxPtr >> 2;
            for (i = 0; i < n; i++)
                HEAPU32[base + i] = addFunction(ex[String(i)], 'ii');
            st.batches++; st.blocks += n; st.bytes += len;
        }
        function failed(e) {
            var base = idxPtr >> 2, i;
            for (i = 0; i < n; i++) HEAPU32[base + i] = 0;
            st.failed++;
            err('x86 jit: ' + e);
        }
        if (sync) {
            try {
                install(new WebAssembly.Instance(new WebAssembly.Module(bytes), x86jitImports));
            } catch (e) {
                failed(e);
            }
            return 1;
        }
        WebAssembly.instantiate(bytes, x86jitImports).then(function (r) {
            install(r.instance);
        }, failed).then(function () {
            wasmExports['x86_jit_installed'](slot, epoch);
        });
        return 1;
    },

    x86_jit_js_free__deps: ['$removeFunction'],
    x86_jit_js_free: function (idx) {
        removeFunction(idx);
    },
});
