# Build the WebAssembly (x86) version of TinyEMU with Emscripten.
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

EMCFLAGS = -O2 -Wall -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -MMD \
           -fno-strict-aliasing -DCONFIG_FS_NET -DCONFIG_X86EMU

EMLDFLAGS = -O3 --no-entry \
  -s NO_EXIT_RUNTIME=1 \
  -s NO_FILESYSTEM=1 \
  -s WASM=1 \
  -s INITIAL_MEMORY=67108864 \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s ENVIRONMENT=web,worker \
  -s "EXPORTED_FUNCTIONS=['_vm_start','_console_queue_char','_fs_import_file','_display_key_event','_display_mouse_event','_display_wheel_event','_net_write_packet','_net_set_carrier','_malloc','_free']" \
  -s "EXPORTED_RUNTIME_METHODS=['ccall','cwrap','HEAPU8','HEAPU32','UTF8ToString']" \
  --js-library $(LIBJS)

# x86 emulator object set (EMSCRIPTEN + CONFIG_X86EMU pulls in only the PC machine)
X86_OBJS = jsemu.js.o machine.js.o \
           x86_cpu.js.o x86_machine.js.o ide.js.o ps2.js.o vmmouse.js.o pckbd.js.o vga.js.o \
           virtio.js.o pci.js.o simplefb.js.o iomem.js.o cutils.js.o json.js.o \
           fs.js.o fs_net.js.o fs_wget.js.o fs_utils.js.o block_net.js.o \
           aes.js.o sha256.js.o softfp.js.o

all: $(OUTDIR)/x86emu-wasm.js

$(OUTDIR)/x86emu-wasm.js: $(X86_OBJS) $(LIBJS)
	$(EMCC) $(EMLDFLAGS) -o $@ $(X86_OBJS)

%.js.o: %.c
	$(EMCC) $(EMCFLAGS) -c -o $@ $<

clean:
	rm -f *.js.o *.d

-include $(wildcard *.d)
