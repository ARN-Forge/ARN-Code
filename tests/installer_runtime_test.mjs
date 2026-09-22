import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdtempSync, readFileSync, readdirSync, rmdirSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import test from 'node:test';

const root = fileURLToPath(new URL('../', import.meta.url));
const runtime = path.resolve(process.argv[2] || path.join(root, 'ide/src-tauri/installer/runtime'));
const cwd = mkdtempSync(path.join(tmpdir(), 'arn-installer-check-'));
// A deployed backend must work without SDKs, repository-relative paths or PATH DLLs.
const env = { SystemRoot: process.env.SystemRoot, WINDIR: process.env.WINDIR,
  TEMP: tmpdir(), TMP: tmpdir(), PATH: path.join(process.env.SystemRoot, 'System32') };
function run(args, input) {
  const result = spawnSync(path.join(runtime, 'arn.exe'), args,
    { cwd, env, input, encoding: 'utf8', timeout: 10000, windowsHide: true });
  assert.ifError(result.error);
  assert.equal(result.status, 0, result.stderr || `Loader/process exit: ${result.status}`);
  return result.stdout;
}
test('deployed backend loads with only Windows on PATH', () => {
  const version = JSON.parse(readFileSync(path.join(root, 'ide/src-tauri/tauri.conf.json'))).version;
  assert.equal(run(['--version']).trim(), `arn ${version}`);
});
test('deployed server starts in a separate project and handles EOF', () => {
  const messages = run(['--server'], '').trim().split(/\r?\n/).map(s => JSON.parse(s));
  assert.ok(messages.some(m => m.type === 'ready' && m.protocol === 2));
  assert.deepEqual(readdirSync(cwd), [], 'Server startup must not create project files');
});
test.after(() => rmdirSync(cwd)); // Empty test directory only; never recursive.
