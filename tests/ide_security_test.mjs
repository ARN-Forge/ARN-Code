// Static security checks of the Windows IDE (ide/): the editor and all other
// executable code are bundled (no CDN), the Content Security Policy is
// restrictive, the WebView's Tauri capabilities are minimal, and the web page
// cannot choose the project folder. With ARN_IDE_DIST set to a built frontend
// (ide/dist), the production bundle is checked as well.
//
//   node --test tests/ide_security_test.mjs
//   ARN_IDE_DIST=ide/dist node --test tests/ide_security_test.mjs
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('../', import.meta.url));
const ide = path.join(root, 'ide');
const read = (...parts) => fs.readFileSync(path.join(ide, ...parts), 'utf8');
const config = JSON.parse(read('src-tauri', 'tauri.conf.json'));

function directives(policy) {
  const map = new Map();
  for (const part of policy.split(';').map(s => s.trim()).filter(Boolean)) {
    const [name, ...sources] = part.split(/\s+/);
    assert.ok(!map.has(name), `CSP repeats ${name}`);
    map.set(name, sources);
  }
  return map;
}

test('the production CSP is restrictive', () => {
  const security = config.app.security;
  assert.equal(typeof security.csp, 'string', 'app.security.csp must be a policy string, not null');
  const csp = directives(security.csp);
  const exactly = (name, expected) => assert.deepEqual(csp.get(name), expected, `${name} must be ${expected.join(' ')}`);
  exactly('default-src', ["'self'"]);
  exactly('script-src', ["'self'"]);              // no remote, inline or eval'd scripts
  exactly('worker-src', ["'self'"]);
  exactly('object-src', ["'none'"]);
  exactly('frame-src', ["'none'"]);
  exactly('base-uri', ["'self'"]);
  exactly('form-action', ["'none'"]);
  for (const source of csp.get('connect-src') || []) {
    assert.ok(["'self'", 'ipc:', 'http://ipc.localhost'].includes(source), `unexpected connect-src ${source}`);
  }
  for (const [name, sources] of csp) {
    for (const source of sources) {
      assert.notEqual(source, "'unsafe-eval'", `${name} allows eval`);
      assert.notEqual(source, "'wasm-unsafe-eval'", `${name} allows wasm eval`);
      assert.ok(!source.includes('*'), `${name} has a wildcard source ${source}`);
      if (/^https?:/.test(source)) assert.equal(source, 'http://ipc.localhost', `${name} allows the remote origin ${source}`);
    }
  }
  assert.ok(!security.dangerousDisableAssetCspModification, 'Tauri must keep adding hashes/nonces to the CSP');
  assert.ok(!('dangerousRemoteDomainIpcAccess' in security), 'no remote domain may reach the IPC');
  assert.ok(!config.app.withGlobalTauri, 'window.__TAURI__ must not be exposed');
});

test('the WebView capabilities allow only event listening', () => {
  const dir = path.join(ide, 'src-tauri', 'capabilities');
  const files = fs.readdirSync(dir).filter(f => f.endsWith('.json'));
  assert.deepEqual(files, ['default.json'], 'one capability file is expected');
  const capability = JSON.parse(fs.readFileSync(path.join(dir, 'default.json'), 'utf8'));
  assert.ok(!('remote' in capability), 'remote URLs must not get capabilities');
  assert.deepEqual(capability.windows, ['main']);
  assert.deepEqual(capability.permissions, ['core:event:default']);
});

test('the page cannot choose the project folder or reach plugins it does not need', () => {
  const main = read('src-tauri', 'src', 'main.rs');
  const handler = main.match(/generate_handler!\[([^\]]*)\]/);
  assert.ok(handler, 'generate_handler! not found');
  const commands = handler[1].split(',').map(s => s.trim()).filter(Boolean).sort();
  assert.deepEqual(commands, ['cancel_agent', 'choose_project_folder', 'configure_agent', 'confirm_agent_change',
    'get_agent_status', 'get_project_root', 'list_agent_models', 'list_files', 'open_chosen_project',
    'read_file', 'reset_agent_session', 'select_agent_model', 'send_agent_message', 'write_file']);
  const open = main.match(/async fn open_chosen_project\(([^)]*)\)/);
  assert.ok(open && !/path/.test(open[1]), 'open_chosen_project must not take a path from the page');
  assert.ok(!/fn set_project_root\b/.test(main), 'set_project_root(path) must not exist');
  const plugins = [...main.matchAll(/\.plugin\(([a-z_]+)::init\(\)\)/g)].map(m => m[1]);
  assert.deepEqual(plugins, ['tauri_plugin_dialog']);
  const cargo = read('src-tauri', 'Cargo.toml');
  for (const plugin of ['tauri-plugin-shell', 'tauri-plugin-fs', 'tauri-plugin-http', 'tauri-plugin-process', 'tauri-plugin-opener']) {
    assert.ok(!cargo.includes(plugin), `${plugin} must not be a dependency`);
  }
});

test('Monaco is bundled at a pinned version, never loaded from a CDN', () => {
  const pkg = JSON.parse(read('package.json'));
  assert.match(pkg.dependencies['monaco-editor'] || '', /^\d+\.\d+\.\d+$/, 'monaco-editor must be an exact dependency');
  const setup = read('src', 'monaco.ts');
  assert.match(setup, /import \* as monaco from 'monaco-editor';/);
  assert.match(setup, /loader\.config\(\{ monaco \}\)/);
  assert.match(read('src', 'main.tsx'), /^import '\.\/monaco';/m, 'main.tsx must load the bundled Monaco first');
  for (const file of fs.readdirSync(path.join(ide, 'src'))) {
    const text = read('src', file);
    assert.ok(!/cdn\.jsdelivr|unpkg\.com|cdnjs|https?:\/\/[^'"\s]*\.js\b/.test(text), `${file} references remote code`);
    assert.ok(!/loader\.config\(\{\s*paths/.test(text), `${file} points the Monaco loader at remote paths`);
  }
  const html = read('index.html');
  for (const [, src] of html.matchAll(/<script[^>]*\ssrc="([^"]+)"/g)) assert.ok(src.startsWith('/'), `remote script ${src}`);
});

test('the production bundle contains Monaco and no remote code', { skip: !process.env.ARN_IDE_DIST && 'ARN_IDE_DIST not set' }, () => {
  const dist = path.resolve(process.env.ARN_IDE_DIST);
  const html = fs.readFileSync(path.join(dist, 'index.html'), 'utf8');
  for (const [, url] of html.matchAll(/\s(?:src|href)="([^"]+)"/g)) assert.ok(url.startsWith('/'), `index.html loads ${url}`);
  assert.ok(!/<script(?![^>]*\ssrc=)[^>]*>\s*\S/.test(html), 'index.html has an inline script');
  const assets = fs.readdirSync(path.join(dist, 'assets'));
  assert.ok(assets.some(f => /^editor\.worker-.*\.js$/.test(f)), 'the editor worker is not bundled');
  assert.ok(assets.some(f => /^codicon-.*\.ttf$/.test(f)), 'the codicon font is not bundled');
  let jsdelivr = 0;
  for (const file of assets.filter(f => f.endsWith('.js'))) {
    const text = fs.readFileSync(path.join(dist, 'assets', file), 'utf8');
    assert.ok(!/unpkg\.com|cdnjs\.cloudflare/.test(text), `${file} references a CDN`);
    assert.ok(!/importScripts\(\s*["']https?:/.test(text), `${file} imports a remote worker script`);
    // @monaco-editor/loader keeps its unused default CDN path in the bundle;
    // loader.config({ monaco }) means it is never requested. Nothing else may.
    for (const match of text.matchAll(/cdn\.jsdelivr\.net[^"']*/g)) {
      jsdelivr++;
      assert.match(match[0], /^cdn\.jsdelivr\.net\/npm\/monaco-editor@[\d.]+\/min\/vs$/, `${file}: unexpected ${match[0]}`);
    }
  }
  assert.ok(jsdelivr <= 1, 'more than the loader default reference to cdn.jsdelivr.net');
});
