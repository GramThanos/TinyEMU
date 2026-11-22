# Build the WebAssembly version of TinyEMU with Emscripten: one engine with both
# machines, the PC (x86) and RISC-V (riscv32 / riscv64), chosen by the config's
# `machine:`. (The file keeps its historical x86emu-wasm name.)
#
# Output: web/js/x86emu-wasm.js + web/js/x86emu-wasm.wasm, built from the current
# C sources and bound to the browser via web/js/lib.js (the Module.api binding).
# Run inside the emscripten/emsdk container, e.g.:
#   docker run --rm -v "$PWD":/src -w /src emscripten/emsdk:3.1.74 make -f Makefile.js
#
# The classic (non-MODULARIZE) output is intentional: web/js/vm/EmulatorFactory.js
# wraps it in a closure to run many isolated instances (see web/README.md).

EMCC = emcc
LIBJS = web/js/lib.js
OUTDIR = web/js

# Inflate a gzip bzImage on the host instead of running the kernel's own
# decompressor on the emulated CPU, which takes ~35 s of a boot here.
# `make -f Makefile.js CONFIG_KERNEL_INFLATE=` turns it off.
CONFIG_KERNEL_INFLATE = y

# -O3 + LTO: the interpreter's hot path spans translation units (x86_cpu.c ->
# iomem.h / cutils), so cross-TU inlining of the memory accessors matters more
# here than the small code-size cost.
#
# -sSUPPORT_LONGJMP=wasm: x86_cpu_interp() calls setjmp() and faults unwind via
# longjmp(). Emscripten's default (=emscripten) rewrites every non-inlined call
# inside a setjmp-containing function into an indirect call through a JS
# `invoke_*` trampoline with try/catch -- i.e. the whole interpreter hot loop
# round-trips through JavaScript. The wasm variant uses native Wasm exception
# handling and stays inside the module. Must be passed at compile *and* link
# time. Needs a browser with Wasm EH: Chrome 95+, Safari 15.4+, Firefox 131+.
#
# -mbulk-memory: gives memory.copy/memory.fill instead of a called-out loop,
# which is what the `rep movs`/`rep stos` bulk path (string_bulk) and the
# device DMA paths spend their time in.
# -mnontrapping-fptoint: without it every float->int conversion (x87 fistp/fisttp,
# and the RISC-V fcvt family) lowers to a guarded helper sequence instead of a
# single instruction.
# Both are older wasm features than the Wasm EH required by -sSUPPORT_LONGJMP=wasm
# below, so they do not narrow the set of supported browsers. Features are
# selected at compile time, so they must be on EMCFLAGS, not just at link.
EMCFLAGS = -O3 -flto -Wall -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -MMD \
           -fno-strict-aliasing -sSUPPORT_LONGJMP=wasm \
           -mbulk-memory -mnontrapping-fptoint \
           -DCONFIG_FS_NET -DCONFIG_X86EMU -DCONFIG_RISCV_MAX_XLEN=64

EMLDFLAGS = -O3 -flto --no-entry \
  -sSUPPORT_LONGJMP=wasm \
  -mbulk-memory -mnontrapping-fptoint \
  -s NO_EXIT_RUNTIME=1 \
  -s NO_FILESYSTEM=1 \
  -s WASM=1 \
  -s INITIAL_MEMORY=67108864 \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s ENVIRONMENT=web,worker \
  -s "EXPORTED_FUNCTIONS=['_vm_start','_vm_pause','_vm_resume','_vm_stop','_console_queue_char','_fs_import_file','_display_key_event','_display_mouse_event','_display_wheel_event','_net_write_packet','_net_set_carrier','_js_block_complete','_vm_snapshot_clock','_vm_reserve_memory','_vm_restore','_vm_cycles','_vm_disk_chunks','_vm_disk_bytes','_malloc','_free']" \
  -s "EXPORTED_RUNTIME_METHODS=['ccall','cwrap','HEAPU8','HEAPU32','UTF8ToString']" \
  --js-library $(LIBJS)

# The PC machine and the shared devices, then the RISC-V machine. riscv_cpu.c is
# built once per XLEN; riscv_cpu_init() picks the one the machine asks for.
X86_OBJS = jsemu.js.o machine.js.o \
           x86_cpu.js.o x86_machine.js.o ide.js.o ps2.js.o vmmouse.js.o pckbd.js.o vga.js.o \
           virtio.js.o pci.js.o simplefb.js.o iomem.js.o cutils.js.o json.js.o \
           fs.js.o fs_net.js.o fs_js.js.o fs_wget.js.o fs_utils.js.o block_net.js.o block_js.js.o \
           aes.js.o sha256.js.o softfp.js.o \
           riscv_machine.js.o riscv_cpu32.js.o riscv_cpu64.js.o \
           inflate.js.o
# inflate.js.o is always in: block_net.c inflates gzip disk chunks. The flag
# only decides whether x86 kernels are inflated on the host too.
ifndef CONFIG_KERNEL_INFLATE
EMCFLAGS += -DX86_NO_KERNEL_INFLATE
endif

all: $(OUTDIR)/x86emu-wasm.js

$(OUTDIR)/x86emu-wasm.js: $(X86_OBJS) $(LIBJS)
	$(EMCC) $(EMLDFLAGS) -o $@ $(X86_OBJS)

riscv_cpu32.js.o: riscv_cpu.c
	$(EMCC) $(EMCFLAGS) -DMAX_XLEN=32 -c -o $@ $<

riscv_cpu64.js.o: riscv_cpu.c
	$(EMCC) $(EMCFLAGS) -DMAX_XLEN=64 -c -o $@ $<

%.js.o: %.c
	$(EMCC) $(EMCFLAGS) -c -o $@ $<

clean:
	rm -f *.js.o *.d

-include $(wildcard *.d)
