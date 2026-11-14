// Stage only the backend's imported DLLs, never whole SDK/bin directories.
// Uses PE import tables so OpenSSL 3/4 names are not guessed.
// The Microsoft Visual C++ runtime is not staged: it is a prerequisite that
// users install from Microsoft (https://aka.ms/vc14/vc_redist.x64.exe). The
// DLLs that arn.exe needs from it are written to installer/runtime/
// vc-runtime.nsh, which the installer hooks (installer/branding.nsh) use to
// stop with a clear message when they are missing.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { execFileSync } from 'node:child_process';

const root = fileURLToPath(new URL('../', import.meta.url));
const tauri = path.join(root, 'ide/src-tauri');
const stage = path.join(tauri, 'installer/runtime');
const backend = process.argv[2] || process.env.ARN_BIN ||
  ['build-ide/Release/arn.exe', 'build/Release/arn.exe'].map(p => path.join(root, p)).find(fs.existsSync);
if (process.platform !== 'win32') throw new Error('Build the Windows installer on Windows.');
if (!backend || !fs.existsSync(backend)) throw new Error('Build ARN first, or pass its absolute path to this script.');

export function imports(file) {
  const b = fs.readFileSync(file);
  const pe = b.readUInt32LE(0x3c);
  if (b.readUInt32LE(pe) !== 0x4550 || b.readUInt16LE(pe + 4) !== 0x8664)
    throw new Error(`Expected an x64 PE binary: ${file}`);
  const count = b.readUInt16LE(pe + 6);
  const optional = pe + 24;
  const sections = optional + b.readUInt16LE(pe + 20);
  function offset(rva) {
    for (let i = 0; i < count; i++) {
      const s = sections + i * 40;
      const va = b.readUInt32LE(s + 12);
      const size = Math.max(b.readUInt32LE(s + 8), b.readUInt32LE(s + 16));
      if (rva >= va && rva < va + size) return b.readUInt32LE(s + 20) + rva - va;
    }
    throw new Error(`Invalid import RVA in ${file}`);
  }
  const names = [];
  const directories = optional + 112; // PE32+ (x64)
  for (const [index, stride, nameField] of [[1, 20, 12], [13, 32, 4]]) {
    const rva = b.readUInt32LE(directories + index * 8);
    if (!rva) continue;
    for (let at = offset(rva); b.readUInt32LE(at + nameField); at += stride) {
      const name = offset(b.readUInt32LE(at + nameField));
      names.push(b.toString('ascii', name, b.indexOf(0, name)).toLowerCase());
    }
  }
  return names;
}

const search = [path.dirname(path.resolve(backend)), ...(process.env.ARN_RUNTIME_DIR || '').split(path.delimiter)];
// Local CMake builds may use an OpenSSL installation rather than vcpkg.
const cache = path.resolve(path.dirname(backend), '../CMakeCache.txt');
if (fs.existsSync(cache)) {
  const include = fs.readFileSync(cache, 'utf8').match(/^OPENSSL_INCLUDE_DIR:PATH=(.+)$/m)?.[1].trim();
  if (include) search.push(path.resolve(include, '../bin'));
}
search.push(...(process.env.PATH || '').split(path.delimiter));
const system = path.join(process.env.SystemRoot || 'C:/Windows', 'System32');
// Same rules as scripts/windows-runtime.ps1 (Test-VcRuntime, Test-DebugVcRuntime).
const vcRuntime = /^(vcruntime|msvcp|concrt|vccorlib|vcomp|vcamp|mfcm?)1\d{2}/i;
const debugVcRuntime = /^((vcruntime|msvcp|concrt|vccorlib|vcomp|vcamp|mfcm?)1\d{2}(_[0-9a-z_]+)?d|ucrtbased)\.dll$/i;
const queue = [path.resolve(backend)];
const files = new Map();
const runtimeDlls = new Set();
while (queue.length) {
  const file = queue.shift();
  const name = path.basename(file).toLowerCase();
  if (files.has(name)) continue;
  files.set(name, file);
  for (const dll of imports(file)) {
    if (/^(api-ms-|ext-ms-)/i.test(dll)) continue; // Windows API sets
    if (debugVcRuntime.test(dll)) throw new Error(`${name} imports ${dll}, the debug Visual C++ runtime, which no Redistributable installs; build ARN in Release.`);
    if (vcRuntime.test(dll)) { runtimeDlls.add(dll); continue; } // prerequisite, not bundled
    if (fs.existsSync(path.join(system, dll))) continue;        // part of Windows
    const candidate = search.filter(Boolean).map(dir => path.join(dir, dll)).find(fs.existsSync);
    if (!candidate) throw new Error(`Missing ${dll} imported by ${name}. Set ARN_RUNTIME_DIR to its redistributable directory.`);
    queue.push(candidate);
  }
}
fs.mkdirSync(stage, { recursive: true });
// A Visual C++ runtime DLL left from an older build must not be picked up.
for (const old of fs.readdirSync(stage)) if (vcRuntime.test(old)) fs.rmSync(path.join(stage, old));
const resources = {};
for (const [name, source] of files) {
  fs.copyFileSync(source, path.join(stage, name));
  resources[`installer/runtime/${name}`] = name;
}
// The installer's check for the Visual C++ runtime (see installer/branding.nsh).
// $R9 collects the missing DLLs; the check only reads System32, so it needs no
// administrator rights. A 32-bit installer sees System32 only with file system
// redirection disabled.
const runtimeList = [...runtimeDlls].sort();
if (!runtimeList.length) throw new Error(`${path.basename(backend)} imports no Visual C++ runtime DLL; check the build.`);
fs.writeFileSync(path.join(stage, 'vc-runtime.nsh'), [
  '; Generated by scripts/prepare-installer.mjs. Do not edit.',
  '; The Microsoft Visual C++ runtime DLLs that the bundled arn.exe needs. ARN does',
  '; not ship them: users install the Microsoft Visual C++ Redistributable (x64).',
  `!define ARN_VC_RUNTIME_DLLS "${runtimeList.join(', ')}"`,
  '!define ARN_VC_REDIST_URL "https://aka.ms/vc14/vc_redist.x64.exe"',
  '!macro ARN_FIND_MISSING_VC_RUNTIME',
  '  StrCpy $R9 ""',
  '  ${DisableX64FSRedirection}',
  ...runtimeList.flatMap(dll => [
    `  IfFileExists "$SYSDIR\\${dll}" +2 0`,
    `    StrCpy $R9 "$R9 ${dll}"`,
  ]),
  '  ${EnableX64FSRedirection}',
  '!macroend',
  '',
].join('\r\n'));
// Third-party notices ship next to arn-ide.exe in the installer and the
// portable ZIP: the CLI's (arn.exe and OpenSSL) and the
// IDE's own, generated from the license files of the exact Rust crates and
// npm packages in use. A component without a license file, in its package or
// in ide/third-party-licenses/sources.json, stops the build.
fs.copyFileSync(path.join(root, 'THIRD-PARTY-NOTICES.txt'), path.join(stage, 'THIRD-PARTY-NOTICES.txt'));
execFileSync(process.execPath, [path.join(root, 'scripts/generate-ide-notices.mjs'),
  path.join(stage, 'THIRD-PARTY-NOTICES-IDE.txt')], { stdio: 'inherit' });
for (const name of ['THIRD-PARTY-NOTICES.txt', 'THIRD-PARTY-NOTICES-IDE.txt']) {
  files.set(name, path.join(stage, name));
  resources[`installer/runtime/${name}`] = name;
}
fs.writeFileSync(path.join(stage, 'bundle-resources.json'), JSON.stringify({ bundle: { resources } }, null, 2) + '\n');
fs.writeFileSync(path.join(stage, 'manifest.json'), JSON.stringify([...files.keys()].sort(), null, 2) + '\n');
console.log(`Staged ${files.size} runtime files: ${[...files.keys()].join(', ')}`);
console.log(`Visual C++ runtime prerequisite (not bundled; the installer checks for it): ${runtimeList.join(', ')}`);
