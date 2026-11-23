// Theme audit (docs/firmware_v0.2.0.md, 1.3): same features in every theme.
// Runs the mock on the built pack (mock_server.py --dist --no-auth), drives headless Chrome over CDP and, for every
// theme x width x page, records the visible interactive controls, horizontal overflow and console errors, saves a
// screenshot to screens/<theme>-<width>-<page>.png, then reports controls that exist in some themes but not others.
//
// Usage (from firmware/web, after python build.py):  node audit.mjs [--chrome PATH] [--no-shots]
// Node 22+ (built-in fetch/WebSocket). Exit code 1 if any difference, overflow or console error is found.
import { spawn } from 'node:child_process';
import { mkdirSync, mkdtempSync, writeFileSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const arg = n => { const i = process.argv.indexOf(n); return i > 0 ? process.argv[i + 1] : null; };
const CHROME = arg('--chrome') || ['C:/Program Files/Google/Chrome/Application/chrome.exe',
  'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe', '/usr/bin/google-chrome', '/usr/bin/chromium',
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'].find(p => existsSync(p));
const SHOTS = !process.argv.includes('--no-shots');
const THEMES = ['swiss', 'cyberpunk', 'zen', 'brutal'];
const WIDTHS = [390, 1280];
const ROUTES = ['ov', 'radio', 'alarm', 'cat', 'notify', 'pc', 'look', 'wifi', 'wx', 'sys'];
const PORT = 8093, CDP = 9335, BASE = `http://127.0.0.1:${PORT}/`;
const sleep = ms => new Promise(r => setTimeout(r, ms));

const mock = spawn('python', [join(HERE, 'mock_server.py'), '--dist', '--no-auth', '--port', String(PORT), '--host', '127.0.0.1'],
  { stdio: 'ignore' });
const chrome = spawn(CHROME, ['--headless=new', `--remote-debugging-port=${CDP}`, '--hide-scrollbars',
  `--user-data-dir=${mkdtempSync(join(tmpdir(), 'mbaudit-'))}`, '--no-first-run', 'about:blank'], { stdio: 'ignore' });
const cleanup = () => { try { chrome.kill(); } catch { } try { mock.kill(); } catch { } };
process.on('exit', cleanup);

let ws;
for (let i = 0; i < 60 && !ws; i++) {
  try {
    const pg = (await (await fetch(`http://127.0.0.1:${CDP}/json`)).json()).find(x => x.type === 'page');
    if (pg) ws = new WebSocket(pg.webSocketDebuggerUrl);
  } catch { }
  await sleep(250);
}
if (ws.readyState !== 1) await new Promise(r => ws.onopen = r);
let id = 0; const pend = new Map(); let errs = [];
ws.onmessage = e => {
  const m = JSON.parse(e.data);
  if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); }
  if (m.method === 'Runtime.exceptionThrown') errs.push(m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text);
  if (m.method === 'Log.entryAdded' && m.params.entry.level === 'error' && !/\/api\/screenshot/.test(m.params.entry.url || ''))
    errs.push(m.params.entry.text + ' ' + (m.params.entry.url || ''));
};
const cmd = (method, params = {}) => new Promise(r => { const i = ++id; pend.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
const ev = async x => (await cmd('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true })).result?.result?.value;
await cmd('Runtime.enable'); await cmd('Log.enable'); await cmd('Page.enable');
for (let i = 0; i < 40; i++) { try { await fetch(BASE + 'api/theme'); break; } catch { await sleep(250); } }

// visible controls as "kind:label"; elements inside [data-audit=theme] differ between themes on purpose and are skipped.
// label = aria-label/title/text, digits squashed so live values don't differ
const PROBE = `(() => {
  const vis = e => { const r = e.getBoundingClientRect(), s = getComputedStyle(e);
    return r.width > 1 && r.height > 1 && s.visibility !== 'hidden' && s.display !== 'none' && +s.opacity > 0.05; };
  const lab = e => (e.getAttribute('aria-label') || e.getAttribute('title') || e.textContent || e.value || e.name || e.type || '')
    .replace(/\\s+/g, ' ').replace(/[0-9]+/g, '#').trim().slice(0, 40);
  const out = [];
  for (const e of document.querySelectorAll('#main a[href], #main button, #main input, #main select, #main textarea, header a[href], header button'))
    if (vis(e) && !e.closest('[hidden]') && !e.closest('[data-audit=theme]')) out.push((e.tagName === 'INPUT' ? 'input/' + e.type : e.tagName.toLowerCase()) + ':' + lab(e));
  const de = document.documentElement;
  const wide = [...document.querySelectorAll('#main *')].filter(e => { const r = e.getBoundingClientRect(); return r.right > de.clientWidth + 1 && vis(e); })
    .slice(0, 3).map(e => e.tagName.toLowerCase() + '.' + [...e.classList].join('.'));
  return { ctl: out, overflow: de.scrollWidth > de.clientWidth + 1 ? wide : [] };
})()`;

const res = {}; const problems = [];
if (SHOTS) mkdirSync(join(HERE, 'screens'), { recursive: true });
for (const th of THEMES) {
  await fetch(BASE + 'api/config/display', { method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ theme: th }) });
  for (const w of WIDTHS) {
    await cmd('Emulation.setDeviceMetricsOverride', { width: w, height: w < 600 ? 844 : 900, deviceScaleFactor: 1, mobile: w < 600 });
    await cmd('Page.navigate', { url: 'about:blank' }); await sleep(150);
    await cmd('Page.navigate', { url: BASE + '#/ov' }); await sleep(1800);
    for (const r of ROUTES) {
      errs = [];
      await ev(`location.hash='#/${r}'`); await sleep(900);
      const p = await ev(PROBE);
      const key = `${w}/${r}`;
      (res[key] ||= {})[th] = p.ctl;
      if (p.overflow.length) problems.push(`${th} ${key}: horizontal overflow (${p.overflow.join(', ')})`);
      for (const e of errs) problems.push(`${th} ${key}: console: ${e}`);
      if (SHOTS) {
        const shot = await cmd('Page.captureScreenshot', { format: 'png', captureBeyondViewport: true });
        writeFileSync(join(HERE, 'screens', `${th}-${w}-${r}.png`), Buffer.from(shot.result.data, 'base64'));
      }
    }
  }
}

// controls present in some themes but not in others
const diffs = [];
for (const [key, byTheme] of Object.entries(res)) {
  const count = (arr, x) => arr.filter(y => y === x).length;
  const all = new Set(Object.values(byTheme).flat());
  for (const c of all) {
    const n = THEMES.map(t => count(byTheme[t] || [], c));
    if (new Set(n).size > 1) diffs.push(`${key}  ${c}  ->  ${THEMES.map((t, i) => `${t}:${n[i]}`).join(' ')}`);
  }
}
for (const [k, v] of Object.entries(res).slice(0, 20)) console.log('  ' + k + ' ' + THEMES.map(t => t + '=' + (v[t] || []).length).join(' '));
console.log(`audited ${THEMES.length} themes x ${WIDTHS.length} widths x ${ROUTES.length} pages`
  + (SHOTS ? ` (screenshots in ${join(HERE, 'screens')})` : ''));
console.log(`\n== controls that differ between themes (${diffs.length})`); diffs.forEach(d => console.log('  ' + d));
console.log(`\n== other problems (${problems.length})`); problems.forEach(d => console.log('  ' + d));
ws.close(); cleanup();
process.exit(diffs.length || problems.length ? 1 : 0);
