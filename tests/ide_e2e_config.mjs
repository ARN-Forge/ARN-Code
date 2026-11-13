// Prints the TAURI_CONFIG JSON for the test-only build of arn-ide.exe that
// tests/ide_runtime_test.mjs drives through the WebView2 DevTools protocol.
//
//   node tests/ide_e2e_config.mjs <port>
//
// The production configuration (ide/src-tauri/tauri.conf.json) is not
// changed. The test build differs from it in exactly two ways:
//   - identifier com.arnecto.ide.e2etest, so its WebView2 profile is separate
//     from a real installation;
//   - additionalBrowserArgs on its windows: wry's default arguments plus
//     --remote-debugging-port=<port>. Tauri passes them to WebView2 through
//     its API (CoreWebView2EnvironmentOptions), not through an environment
//     variable.
// TAURI_CONFIG is merged with JSON merge patch (RFC 7396), which replaces
// arrays as a whole, so every window is copied from the production
// configuration and only the browser arguments are added. Everything else,
// including the Content Security Policy and capabilities, comes from the
// production files unchanged.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const port = Number(process.argv[2]);
if (!Number.isInteger(port) || port < 1024 || port > 65535) throw new Error('usage: node tests/ide_e2e_config.mjs <port 1024-65535>');
const root = fileURLToPath(new URL('../', import.meta.url));
const config = JSON.parse(fs.readFileSync(path.join(root, 'ide/src-tauri/tauri.conf.json'), 'utf8'));

// wry's own default when a window sets no browser arguments (wry 0.55,
// src/webview2/mod.rs); the production windows set none, so the test build
// keeps exactly these and adds the debugging port.
const wryDefault = '--disable-features=msWebOOUI,msPdfOOUI,msSmartScreenProtection';
const windows = config.app.windows.map(window => {
  if ('additionalBrowserArgs' in window || 'additional-browser-args' in window) {
    throw new Error('the production window already sets browser arguments; update tests/ide_e2e_config.mjs');
  }
  return { ...window, additionalBrowserArgs: `${wryDefault} --remote-debugging-port=${port}` };
});
process.stdout.write(JSON.stringify({ identifier: 'com.arnecto.ide.e2etest', app: { windows } }));
