// SPDX-License-Identifier: MIT
'use strict';

const REPO = 'https://github.com/GramThanos/TinyEMU';
const DISTROS = 'https://gramthanos.github.io/TinyEMU-distros/';

// ---- theme ----
document.getElementById('theme-toggle').addEventListener('click', () => {
  const dark = document.documentElement.classList.toggle('dark');
  localStorage.setItem('tinyemu-theme', dark ? 'dark' : 'light');
});

// ---- releases (index.json, written by .github/workflows/pages.yml) ----
const $ = (id) => document.getElementById(id);
const el = (tag, props, ...kids) => {
  const e = Object.assign(document.createElement(tag), props || {});
  e.append(...kids);
  return e;
};
const link = (text, href) => el('a', { href, textContent: text });

function showReleases(idx) {
  const versions = idx.versions || [];
  const latest = versions.find((v) => v.version === idx.latest) || versions[0];
  if (!latest) throw new Error('no releases');

  // big download cards for the newest release
  $('latest-version').textContent = latest.version;
  $('latest-date').textContent = (latest.date || '').slice(0, 10);
  const cards = [
    ['Linux x86_64', 'temu-soft (software x86 + RISC-V) and temu-kvm', latest.bin + 'temu-linux-x86_64.tar.gz'],
    ['Windows', 'temu.exe with its DLLs, headless', latest.bin + 'temu-windows.zip'],
    ['WebAssembly engine', 'x86emu-wasm.js + .wasm, both machines', latest.engine + 'x86emu-wasm.js'],
  ];
  const grid = $('dl-grid');
  grid.textContent = '';
  for (const [title, desc, href] of cards) {
    grid.append(el('a', { className: 'dl', href },
      el('strong', { textContent: title }), el('span', { textContent: desc }),
      el('em', { textContent: href.split('/').pop() })));
  }

  // every release
  const body = $('releases');
  body.textContent = '';
  for (const e of versions) {
    const name = el('td', {}, el('code', { textContent: e.version }));
    if (e === latest) name.append(el('span', { className: 'badge', textContent: 'latest' }));
    const engine = el('td', {}, link('js', e.engine + 'x86emu-wasm.js'), ' · ',
      link('wasm', e.engine + 'x86emu-wasm.wasm'), ' · ', link('sums', e.engine + 'SHA256SUMS'));
    const bins = el('td', {}, link('linux', e.bin + 'temu-linux-x86_64.tar.gz'), ' · ',
      link('windows', e.bin + 'temu-windows.zip'), ' · ', link('sums', e.bin + 'SHA256SUMS'));
    const commit = el('td', {}, e.commit
      ? el('a', { href: REPO + '/commit/' + e.commit }, el('code', { textContent: e.commit.slice(0, 7) }))
      : '');
    body.append(el('tr', {}, name, el('td', { textContent: (e.date || '').slice(0, 10) }), engine, bins, commit));
  }

  // pin the snippets to the newest version
  const base = new URL('engine/' + latest.version + '/', location.href).href;
  for (const code of document.querySelectorAll('[data-engine-url]')) {
    code.textContent = code.textContent.replaceAll('https://gramthanos.github.io/TinyEMU/engine/<version>/', base);
  }
  for (const code of document.querySelectorAll('[data-version]')) {
    code.textContent = code.textContent.replaceAll('<version>', latest.version);
  }
}

fetch('index.json', { cache: 'no-cache' }).then((r) => {
  if (!r.ok) throw new Error(r.status);
  return r.json();
}).then(showReleases).catch(() => {
  $('dl-grid').textContent = '';
  $('dl-grid').append(el('p', { className: 'muted', textContent: 'No releases published here yet.' }));
  $('releases').innerHTML = '<tr><td colspan="5" class="muted">No releases published yet.</td></tr>';
});

// ---- playground ----
const params = new URLSearchParams(location.search);
// ?engine=<version> boots an older published engine (default: the latest)
const engineVersion = /^[A-Za-z0-9._-]+$/.test(params.get('engine') || '') ? params.get('engine') : 'latest';
const ENGINE = new URL('engine/' + engineVersion + '/x86emu-wasm.js', location.href).href;

const term = new Terminal({
  cursorBlink: true, convertEol: false, fontSize: 14, scrollback: 5000,
  fontFamily: 'ui-monospace, "SF Mono", "Cascadia Code", Menlo, Consolas, monospace',
  theme: { background: '#16161a', foreground: '#dfdfd6', cursor: '#5ea8ff', selectionBackground: '#3a4a66' },
});
const fit = new FitAddon.FitAddon();
term.loadAddon(fit);
term.open($('terminal'));
fit.fit();
addEventListener('resize', () => {
  fit.fit();
  if (worker) worker.postMessage({ type: 'resize', cols: term.cols, rows: term.rows });
});

let worker = null;
let distros = [];
const enc = new TextEncoder();
term.onData((d) => { if (worker) worker.postMessage({ type: 'input', data: enc.encode(d) }); });

function fmtBytes(n) {
  if (n < 1024 * 1024) return (n / 1024).toFixed(0) + ' KB';
  return (n / 1048576).toFixed(1) + ' MB';
}

function setRunning(on) {
  $('play-boot').disabled = on;
  $('play-reset').disabled = !on;
  for (const b of document.querySelectorAll('.play-cmds button')) b.disabled = !on;
}

function stop() {
  if (worker) worker.terminate();
  worker = null;
  $('term-stats').textContent = '';
  setRunning(false);
}

function boot() {
  const d = distros.find((x) => x.name === $('play-distro').value);
  if (!d) return;
  stop();
  term.reset();
  $('term-overlay').hidden = true;
  $('term-title').textContent = d.name;
  term.write('\x1b[90mLoading the engine and ' + d.name + '…\x1b[0m\r\n');
  term.write('\x1b[90m' + ENGINE + '\r\n' + d.url + '\x1b[0m\r\n\r\n');
  setRunning(true);
  fit.fit();

  worker = new Worker('play-worker.js');
  const w = worker;
  let seenOutput = false, downloading = false;
  w.onmessage = (e) => {
    if (w !== worker) return;
    const m = e.data;
    if (m.type === 'out') {
      if (!seenOutput) { seenOutput = true; }
      term.write(m.data);
    } else if (m.type === 'downloading') {
      downloading = m.flag;
    } else if (m.type === 'stats') {
      const parts = [];
      if (m.ips >= 0) parts.push(m.ips < 1e6 ? 'idle' : (m.ips / 1e6).toFixed(0) + ' MIPS');
      parts.push(fmtBytes(m.diskBytes) + ' fetched');
      if (m.jitBlocks > 0) parts.push('JIT ' + m.jitBlocks + ' blocks');
      if (downloading) parts.push('⇣');
      $('term-stats').textContent = parts.join(' · ');
    } else if (m.type === 'error') {
      term.write('\r\n\x1b[31m' + m.message + '\x1b[0m\r\n');
    }
  };
  w.onerror = (e) => term.write('\r\n\x1b[31m' + (e.message || 'worker error') + '\x1b[0m\r\n');
  w.postMessage({
    type: 'start', engine: ENGINE, config: d.url, pageUrl: location.href,
    cols: term.cols, rows: term.rows, jit: params.get('jit') !== '0',
  });
  term.focus();
}

$('play-boot').addEventListener('click', boot);
$('play-overlay-boot').addEventListener('click', boot);
$('play-reset').addEventListener('click', () => {
  stop();
  term.reset();
  $('term-title').textContent = 'TinyEMU';
  $('term-overlay').hidden = false;
});
$('play-distro').addEventListener('change', () => {
  const d = distros.find((x) => x.name === $('play-distro').value);
  $('play-desc').textContent = d ? d.description : '';
});
for (const b of document.querySelectorAll('.play-cmds button')) {
  b.addEventListener('click', () => {
    if (!worker) return;
    worker.postMessage({ type: 'input', data: enc.encode(b.textContent + '\r') });
    term.focus();
  });
}
setRunning(false);

fetch(DISTROS + 'manifest.json').then((r) => r.json()).then((m) => {
  distros = (m.distros || []).filter((d) => d.arch === 'x86' || d.arch === 'riscv64' || d.arch === 'riscv32');
  // lite images first: they boot straight to a shell
  distros.sort((a, b) => (b.name.endsWith('-lite') - a.name.endsWith('-lite')) || a.name.localeCompare(b.name));
  const sel = $('play-distro');
  sel.textContent = '';
  for (const d of distros) sel.append(el('option', { value: d.name, textContent: d.name + '  (' + d.arch + ')' }));
  const want = params.get('distro');
  sel.value = distros.some((d) => d.name === want) ? want : 'alpine-3.20-x86-lite';
  if (!sel.value && distros[0]) sel.value = distros[0].name;
  sel.dispatchEvent(new Event('change'));
  $('play-overlay-boot').disabled = false;
}).catch(() => {
  $('play-desc').textContent = 'Could not load the image list from TinyEMU-distros.';
});
