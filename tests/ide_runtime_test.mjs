// Runs a built arn-ide.exe and checks, through the WebView2 DevTools protocol,
// that the production frontend works under its Content Security Policy:
//   - the page is served with the CSP from tauri.conf.json;
//   - the bundled Monaco editor starts, accepts input, and computes a diff in
//     its web worker (not on the main thread);
//   - a <script> from a CDN is blocked, and every request the page makes is
//     to the application itself or the Tauri IPC (no remote origin);
//   - the IPC answers the commands the page needs and refuses the removed
//     set_project_root, plugins without permission, and opening a project
//     that was not picked in the backend's dialog.
// Monaco's global API is enabled only for this test, by a script the test
// injects before the page loads (MonacoEnvironment.globalAPI); the
// application does not expose it.
//
//   node tests/ide_runtime_test.mjs <arn-ide.exe> [<identifier>]
// On a developer machine, build arn-ide.exe with
// TAURI_CONFIG='{"identifier":"com.arnecto.ide.e2etest"}' (the default
// <identifier>) so the test's WebView profile, %LOCALAPPDATA%\<identifier>,
// is separate from a real installation. The test removes that folder
// afterwards; on a CI runner the production identifier com.arnecto.ide is fine.
import assert from 'node:assert/strict';
import { spawn, execFileSync } from 'node:child_process';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';

const exe = path.resolve(process.argv[2] || '');
if (process.platform !== 'win32') { console.log('skip: Windows only'); process.exit(77); }
if (!fs.existsSync(exe)) throw new Error(`usage: node tests/ide_runtime_test.mjs <arn-ide.exe> (not found: ${exe})`);
const identifier = process.argv[3] || 'com.arnecto.ide.e2etest';
if (!/^com\.arnecto\.ide(\.[a-z0-9]+)?$/.test(identifier)) throw new Error(`unexpected identifier '${identifier}'`);
const profile = path.join(process.env.LOCALAPPDATA, identifier);
const port = 9300 + Math.floor(Math.random() * 500);
const sleep = ms => new Promise(r => setTimeout(r, ms));
const results = [];
const check = (name, ok, detail = '') => { results.push({ name, ok, detail }); console.log(`${ok ? 'ok' : 'FAIL'}: ${name}${detail ? ` (${detail})` : ''}`); };

// The IDE's own output is kept for diagnosis: the last 4 KB of each stream,
// with anything that looks like a key or token masked. The environment is
// never printed.
const tail = { stdout: '', stderr: '' };
const keep = (stream, chunk) => { tail[stream] = (tail[stream] + chunk.toString('utf8')).slice(-4096); };
const mask = text => text
  .replace(/\b(?:sk|pk|rk|ghp|gho|ghs|ghu|github_pat|xox[abprs]|AIza)[-_A-Za-z0-9]{8,}/g, '[masked]')
  .replace(/[A-Za-z0-9+/_-]{32,}={0,2}/g, '[masked]');
let spawnError = null;
let exited = null;
const child = spawn(exe, [], {
  env: { ...process.env, WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS: `--remote-debugging-port=${port}` },
  stdio: ['ignore', 'pipe', 'pipe'], windowsHide: false,
});
child.on('error', error => { spawnError = error; });
child.on('exit', (code, signal) => { exited = { code, signal }; });
child.stdout.on('data', chunk => keep('stdout', chunk));
child.stderr.on('data', chunk => keep('stderr', chunk));

// Whether anything accepts TCP connections on the DevTools port.
const tcpOpen = () => new Promise(resolve => {
  const socket = net.connect(port, '127.0.0.1');
  socket.setTimeout(1000, () => { socket.destroy(); resolve(false); });
  socket.once('connect', () => { socket.destroy(); resolve(true); });
  socket.once('error', () => resolve(false));
});

// Facts about the WebView2 Runtime and the runner, for a failed start.
function environmentReport() {
  const lines = [];
  const reg = key => {
    try {
      return execFileSync('reg', ['query', key, '/v', 'pv'], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] })
        .match(/pv\s+REG_SZ\s+(\S+)/)?.[1] || 'key without version';
    } catch { return 'not found'; }
  };
  const clientKey = 'Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}';
  lines.push(`WebView2 Runtime per machine: ${reg(`HKLM\\SOFTWARE\\WOW6432Node\\${clientKey}`)}; per user: ${reg(`HKCU\\SOFTWARE\\${clientKey}`)}`);
  let webviewProcesses = 'unknown';
  try {
    webviewProcesses = String(execFileSync('tasklist', ['/FI', 'IMAGENAME eq msedgewebview2.exe', '/FO', 'CSV', '/NH'], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] })
      .split(/\r?\n/).filter(line => /msedgewebview2\.exe/i.test(line)).length);
  } catch { /* tasklist unavailable */ }
  lines.push(`msedgewebview2.exe processes on this machine (any application): ${webviewProcesses}`);
  const webviewData = path.join(profile, 'EBWebView');
  lines.push(`WebView2 user data folder ${webviewData}: ${fs.existsSync(webviewData) ? 'created' : 'not created'}`);
  const activePort = path.join(webviewData, 'DevToolsActivePort');
  // Informational only: WebView2 does not always write this file, even when
  // the port is open.
  lines.push(`DevToolsActivePort file (informational): ${fs.existsSync(activePort) ? `present, port ${fs.readFileSync(activePort, 'utf8').split(/\r?\n/)[0]}` : 'absent'}`);
  // The WebView2 processes of this profile: did they get the debugging flag,
  // and what do they listen on? Only those flags and the ports are printed.
  try {
    const script = `$ErrorActionPreference='SilentlyContinue'; $d='${webviewData.toLowerCase().replace(/'/g, "''")}';` +
      `$ps=@(Get-CimInstance Win32_Process -Filter "Name='msedgewebview2.exe'" | Where-Object { $_.CommandLine -and $_.CommandLine.ToLower().Contains($d) });` +
      `$out=@($ps | ForEach-Object { $c=$_.CommandLine; [pscustomobject]@{ pid=$_.ProcessId;` +
      ` type=([regex]::Match($c,'--type=([a-z-]+)').Groups[1].Value);` +
      ` debug=([regex]::Match($c,'--remote-debugging-port=(\\d+)').Groups[1].Value);` +
      ` listen=@(Get-NetTCPConnection -State Listen -OwningProcess $_.ProcessId | ForEach-Object { "$($_.LocalAddress):$($_.LocalPort)" }) } });` +
      `ConvertTo-Json -Compress -Depth 3 @($out)`;
    const info = JSON.parse(execFileSync('powershell.exe', ['-NoProfile', '-NonInteractive', '-Command', script],
      { encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'], timeout: 30000 }) || '[]');
    const processes = Array.isArray(info) ? info : [info];
    lines.push(`WebView2 processes of this profile: ${processes.length}`);
    for (const p of processes) {
      lines.push(`  pid ${p.pid} ${p.type || 'browser'}: --remote-debugging-port ${p.debug ? `=${p.debug}` : 'absent'}; listening ${[].concat(p.listen || []).join(', ') || 'nothing'}`);
    }
  } catch (error) { lines.push(`WebView2 process inspection failed: ${error.code || error.status || 'error'}`); }
  // Edge/WebView2 group policies that can affect debugging or arguments.
  for (const key of ['HKLM\\SOFTWARE\\Policies\\Microsoft\\Edge\\WebView2', 'HKLM\\SOFTWARE\\Policies\\Microsoft\\Edge']) {
    try {
      const values = execFileSync('reg', ['query', key], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] })
        .split(/\r?\n/).map(l => l.trim()).filter(l => /\sREG_/.test(l));
      lines.push(`policy ${key}: ${values.length ? mask(values.slice(0, 15).join(' | ')) : 'no values'}`);
    } catch { lines.push(`policy ${key}: not present`); }
  }
  lines.push(`Windows ${os.release()} (${os.version()}), session ${process.env.SESSIONNAME || '(no SESSIONNAME)'}, ` +
    `GitHub Actions: ${process.env.GITHUB_ACTIONS === 'true' ? `yes, runner ${process.env.RUNNER_NAME || '?'} (${process.env.ImageOS || '?'} ${process.env.ImageVersion || ''})` : 'no'}`);
  return lines;
}

let socket;
try {
  // The application's page target. If it does not appear, the test fails and
  // says which stage was reached.
  let target;
  let everTcp = false;
  let everHttp = false;
  let lastHttpError = '';
  let lastList = [];
  const deadline = Date.now() + 60000;
  while (!target && !exited && !spawnError && Date.now() < deadline) {
    await sleep(250);
    if (!(await tcpOpen())) continue;
    everTcp = true;
    try {
      const list = await (await fetch(`http://127.0.0.1:${port}/json/list`, { signal: AbortSignal.timeout(3000) })).json();
      everHttp = true;
      lastList = list.map(t => `${t.type} ${t.url}`);
      target = list.find(t => t.type === 'page' && /^https?:\/\/tauri\.localhost\//.test(t.url));
    } catch (error) {
      lastHttpError = `${error.name}: ${error.message}`;
    }
  }
  if (!target) {
    if (!exited && !spawnError) await sleep(500);   // pick up an exit that is just happening
    let diagnosis;
    if (spawnError) diagnosis = `the IDE process did not start: ${spawnError.code || ''} ${spawnError.message}`;
    else if (exited) {
      const hex = typeof exited.code === 'number' ? ` (0x${(exited.code >>> 0).toString(16).toUpperCase()})` : '';
      diagnosis = `the IDE process exited with code ${exited.code}${hex}${exited.signal ? `, signal ${exited.signal}` : ''} before its page appeared`;
    } else if (!everTcp) diagnosis = `the IDE process is running, but nothing listened on DevTools port ${port} within 60 s (WebView2 did not open it)`;
    else if (!everHttp) diagnosis = `DevTools port ${port} accepted connections but never answered /json/list (${lastHttpError || 'no response'})`;
    else diagnosis = `DevTools answered, but no tauri.localhost page target appeared; targets: ${JSON.stringify(lastList)}`;
    console.log(`DIAGNOSIS: ${diagnosis}`);
    for (const line of environmentReport()) console.log(`  ${line}`);
    for (const stream of ['stdout', 'stderr']) {
      const text = mask(tail[stream]).trim();
      console.log(`  IDE ${stream} (last ${text.length} characters): ${text ? `\n${text.split(/\r?\n/).map(l => `    ${l}`).join('\n')}` : '(empty)'}`);
    }
  }
  assert.ok(target, 'the IDE page did not appear on the DevTools port within 60 s');

  socket = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((resolve, reject) => { socket.onopen = resolve; socket.onerror = reject; });
  let nextId = 1;
  const pending = new Map();
  const events = [];
  socket.onmessage = message => {
    const data = JSON.parse(message.data);
    if (data.id && pending.has(data.id)) { pending.get(data.id)(data); pending.delete(data.id); } else events.push(data);
  };
  const send = (method, params = {}) => new Promise((resolve, reject) => {
    const id = nextId++;
    pending.set(id, data => data.error ? reject(new Error(`${method}: ${data.error.message}`)) : resolve(data.result));
    socket.send(JSON.stringify({ id, method, params }));
  });
  const evaluate = async expression => {
    const result = await send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true });
    if (result.exceptionDetails) throw new Error(result.exceptionDetails.exception?.description || result.exceptionDetails.text);
    return result.result.value;
  };

  await send('Page.enable'); await send('Network.enable'); await send('Runtime.enable'); await send('Log.enable');
  await send('Page.addScriptToEvaluateOnNewDocument', { source: `
    globalThis.MonacoEnvironment = { globalAPI: true };
    globalThis.__violations = [];
    addEventListener('securitypolicyviolation', e => __violations.push(e.violatedDirective + ' ' + e.blockedURI));` });
  events.length = 0;
  await send('Page.reload', { ignoreCache: true });
  for (let i = 0; i < 120; i++) {
    await sleep(250);
    if (await evaluate(`typeof monaco === 'object' && document.querySelector('#root')?.childElementCount > 0`).catch(() => false)) break;
  }

  // 1. The CSP header of the page.
  const documentResponse = events.find(e => e.method === 'Network.responseReceived' && e.params.type === 'Document');
  const headers = Object.fromEntries(Object.entries(documentResponse?.params.response.headers || {}).map(([k, v]) => [k.toLowerCase(), v]));
  const csp = headers['content-security-policy'] || '';
  check('page is served with the restrictive CSP', /script-src 'self'/.test(csp) && /object-src 'none'/.test(csp) && !/unsafe-eval/.test(csp), csp.slice(0, 120));

  // Page errors, for diagnosing any failure below.
  const pageErrors = events.flatMap(e => {
    if (e.method === 'Runtime.exceptionThrown') return [`exception: ${e.params.exceptionDetails.exception?.description || e.params.exceptionDetails.text}`];
    if (e.method === 'Log.entryAdded' && ['error', 'warning'].includes(e.params.entry.level)) return [`log ${e.params.entry.level}: ${e.params.entry.text}`];
    if (e.method === 'Runtime.consoleAPICalled' && ['error', 'warning'].includes(e.params.type)) return [`console.${e.params.type}: ${e.params.args.map(a => a.value ?? a.description).join(' ')}`];
    return [];
  });
  for (const line of pageErrors.slice(0, 15)) console.log(`  page: ${line.split('\n').slice(0, 3).join(' | ')}`);
  check('no uncaught exception in the page', !events.some(e => e.method === 'Runtime.exceptionThrown'));

  // 2. The app rendered and Monaco is the bundled instance.
  check('application UI rendered', await evaluate(`document.querySelector('#root').childElementCount > 0`));
  check('bundled Monaco loaded', await evaluate(`typeof monaco?.editor?.create === 'function'`));

  // 3. Editing works.
  const edited = await evaluate(`(() => {
    const host = document.createElement('div'); host.style.cssText = 'position:fixed;left:0;top:0;width:600px;height:200px';
    document.body.append(host);
    const editor = monaco.editor.create(host, { value: 'hello', language: 'text' });
    editor.setPosition({ lineNumber: 1, column: 6 });
    editor.trigger('test', 'type', { text: ' world' });
    const value = editor.getValue();
    const rendered = !!host.querySelector('.monaco-editor .view-lines');
    editor.dispose(); host.remove();
    return { value, rendered };
  })()`);
  check('editor renders and accepts input', edited.rendered && edited.value === 'hello world', JSON.stringify(edited));

  // 4. A diff is computed by the editor worker.
  const diff = await evaluate(`new Promise(resolve => {
    const host = document.createElement('div'); host.style.cssText = 'position:fixed;left:0;top:0;width:600px;height:200px';
    document.body.append(host);
    const editor = monaco.editor.createDiffEditor(host, {});
    editor.setModel({ original: monaco.editor.createModel('a\\nb\\nc'), modified: monaco.editor.createModel('a\\nB\\nc') });
    const done = () => { const changes = editor.getLineChanges(); editor.dispose(); host.remove(); resolve(changes ? changes.length : -1); };
    editor.onDidUpdateDiff(done);
    setTimeout(done, 10000);
  })`);
  const workerLoaded = events.some(e => e.method === 'Network.requestWillBeSent' && /\/assets\/editor\.worker-[^/]*\.js$/.test(e.params.request.url))
    || await evaluate(`performance.getEntriesByType('resource').some(r => /editor\\.worker-/.test(r.name))`);
  const fallback = events.some(e => e.method === 'Runtime.consoleAPICalled' && JSON.stringify(e.params.args).includes('Could not create web worker'));
  check('diff computed in the bundled editor worker', diff === 1 && workerLoaded && !fallback, `changes=${diff} worker=${workerLoaded} fallback=${fallback}`);

  // 5. Remote script is blocked by the CSP.
  const blocked = await evaluate(`new Promise(resolve => {
    const s = document.createElement('script');
    s.src = 'https://cdn.jsdelivr.net/npm/monaco-editor@0.55.1/min/vs/loader.js';
    s.onload = () => resolve('loaded'); s.onerror = () => resolve('blocked');
    document.head.append(s);
    setTimeout(() => resolve('timeout'), 5000);
  })`);
  const violations = await evaluate(`__violations`);
  check('remote <script> is blocked by script-src', blocked === 'blocked' && violations.some(v => v.startsWith('script-src') && v.includes('cdn.jsdelivr.net')), `${blocked}; ${violations.join(' | ')}`);
  const unexpectedViolations = violations.filter(v => !v.includes('cdn.jsdelivr.net'));
  check('no CSP violation by the application itself', unexpectedViolations.length === 0, unexpectedViolations.join(' | '));

  // 6. Only local requests. DevTools also reports a request the CSP blocked
  // before it was sent (the probe above); those are listed apart, and the
  // probe must be the only one.
  const blockedIds = new Set(events.filter(e => e.method === 'Network.loadingFailed' && e.params.blockedReason === 'csp').map(e => e.params.requestId));
  const requests = events.filter(e => e.method === 'Network.requestWillBeSent').map(e => e.params);
  const isLocal = u => /^(https?:\/\/(tauri|ipc)\.localhost\/|data:|blob:)/.test(u);
  const remote = requests.filter(r => !isLocal(r.request.url) && !blockedIds.has(r.requestId)).map(r => r.request.url);
  const blockedRemote = requests.filter(r => !isLocal(r.request.url) && blockedIds.has(r.requestId)).map(r => r.request.url);
  check('every request that was sent is local (tauri.localhost / ipc.localhost)', requests.length > 0 && remote.length === 0,
    remote.slice(0, 5).join(' ') || `${requests.length} requests`);
  check('the only remote request attempt is the test probe, blocked by the CSP',
    blockedRemote.length === 1 && blockedRemote[0].includes('cdn.jsdelivr.net'), blockedRemote.join(' '));

  // 7. IPC surface.
  const ipc = async (cmd, args = {}) => evaluate(`window.__TAURI_INTERNALS__.invoke(${JSON.stringify(cmd)}, ${JSON.stringify(args)})
    .then(v => ({ ok: true, v }), e => ({ ok: false, e: String(e) }))`);
  const root = await ipc('get_project_root');
  check('IPC: get_project_root answers', root.ok && root.v === null, JSON.stringify(root));
  const setRoot = await ipc('set_project_root', { path: 'C:\\' });
  check('IPC: set_project_root(path) no longer exists', !setRoot.ok, setRoot.e);
  const openChosen = await ipc('open_chosen_project');
  check('IPC: open_chosen_project needs a folder picked in the backend dialog', !openChosen.ok && /Choose a project folder first/.test(openChosen.e), openChosen.e);
  const dialog = await ipc('plugin:dialog|open', { options: { directory: true } });
  check('IPC: dialog plugin is not available to the page', !dialog.ok, dialog.e);
  const shell = await ipc('plugin:shell|execute', { program: 'cmd', args: ['/c', 'echo'] });
  check('IPC: shell plugin is not available to the page', !shell.ok, shell.e);
  const fsRead = await ipc('read_file', { path: 'C:\\Windows\\win.ini' });
  check('IPC: read_file refuses without a project', !fsRead.ok, fsRead.e);
  // The agent panel listens for backend events: core:event:default must allow it.
  const listened = await evaluate(`(async () => {
    const internals = window.__TAURI_INTERNALS__;
    const handler = internals.transformCallback(() => {});
    try {
      const id = await internals.invoke('plugin:event|listen', { event: 'arn-runtime-test', target: { kind: 'Any' }, handler });
      await internals.invoke('plugin:event|unlisten', { event: 'arn-runtime-test', eventId: id });
      return { ok: true, id };
    } catch (e) { return { ok: false, e: String(e) }; }
  })()`);
  check('IPC: event listening (agent panel) is allowed', listened.ok, JSON.stringify(listened));
} finally {
  try { socket?.close(); } catch { /* ignore */ }
  try { execFileSync('taskkill', ['/PID', String(child.pid), '/T', '/F'], { stdio: 'ignore' }); } catch { /* already gone */ }
  await sleep(1500);
  if (fs.existsSync(profile)) {
    for (let i = 0; i < 10; i++) {
      try { fs.rmSync(profile, { recursive: true, force: true }); break; } catch { await sleep(1000); }
    }
  }
}
const failed = results.filter(r => !r.ok);
console.log(`${results.length} checks, ${failed.length} failure(s)`);
process.exit(failed.length ? 1 : 0);
