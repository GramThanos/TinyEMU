// SPDX-License-Identifier: MIT
// Runs one TinyEMU engine (x86emu-wasm.js) for the playground. The engine is a
// classic Emscripten script with a global Module, so each VM gets its own
// worker and resetting the VM is terminating the worker.
//
// Page -> worker: { type: 'start', engine, config, cols, rows, jit }
//                 { type: 'input', data: Uint8Array }
// Worker -> page: { type: 'out', data: Uint8Array }, { type: 'ready' },
//                 { type: 'stats', ... }, { type: 'error', message }
'use strict';

let started = false;

function startEngine(msg) {
  const engineBase = new URL('.', msg.engine).href;
  let cols = msg.cols, rows = msg.rows;

  self.Module = {
    // the .wasm sits next to the .js, not next to this worker
    locateFile: (path) => engineBase + path,
    // relative paths in a config resolve against the page, not this worker
    pageUrl: msg.pageUrl,
    x86jit: msg.jit !== false,
    print: (t) => console.log(t),
    printErr: (t) => console.warn(t),
    api: {
      terminal: {
        // a view into the engine's heap: copy before posting
        write: (bytes) => postMessage({ type: 'out', data: bytes.slice() }),
        getSize: () => [cols, rows],
      },
      wgetRequests: {
        next: 1, instance: {},
        new(xhr) { const h = this.next++; this.instance[h] = xhr; return h; },
        remove(h) { delete this.instance[h]; },
      },
      downloading: { update: (flag) => postMessage({ type: 'downloading', flag }) },
      export_file: () => {},
    },
    onRuntimeInitialized() {
      self.Module.ccall('vm_start', null,
        ['string', 'number', 'string', 'string', 'number', 'number', 'number'],
        // config, RAM from the config, no extra cmdline, no 9P password,
        // no framebuffer (console only), no network
        [msg.config, 0, '', null, 0, 0, 0]);
      postMessage({ type: 'ready' });
      let last = 0, lastT = performance.now();
      setInterval(() => {
        const M = self.Module;
        const cyc = M._vm_cycles(), now = performance.now();
        const ips = cyc >= 0 && last > 0 ? (cyc - last) * 1000 / (now - lastT) : -1;
        if (cyc >= 0) { last = cyc; lastT = now; }
        postMessage({ type: 'stats', ips, diskBytes: M._vm_disk_bytes(), jitBlocks: M._vm_jit_blocks() });
      }, 1000);
    },
  };
  self.setSize = (c, r) => { cols = c; rows = r; };

  try {
    importScripts(msg.engine);
  } catch (e) {
    postMessage({ type: 'error', message: 'Could not load the engine: ' + e.message });
  }
}

self.onmessage = (e) => {
  const msg = e.data;
  if (msg.type === 'start' && !started) {
    started = true;
    startEngine(msg);
  } else if (msg.type === 'input' && self.Module && self.Module._console_queue_char) {
    for (const b of msg.data) self.Module._console_queue_char(b);
  } else if (msg.type === 'resize' && self.setSize) {
    self.setSize(msg.cols, msg.rows);
  }
};

self.onerror = (e) => { postMessage({ type: 'error', message: String(e.message || e) }); };
