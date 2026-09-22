// Stage only the backend's imported DLLs, never whole SDK/bin directories.
// Uses PE import tables so OpenSSL 3/4 and MSVC runtime names are not guessed.
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
const vswhere = path.join(process.env['ProgramFiles(x86)'] || '', 'Microsoft Visual Studio/Installer/vswhere.exe');
if (fs.existsSync(vswhere)) {
  const vs = execFileSync(vswhere, ['-latest', '-products', '*', '-property', 'installationPath'], { encoding: 'utf8' }).trim();
  const redist = path.join(vs, 'VC/Redist/MSVC');
  if (fs.existsSync(redist)) {
    for (const version of fs.readdirSync(redist).sort().reverse()) {
      const arch = path.join(redist, version, 'x64');
      if (fs.existsSync(arch)) for (const crt of fs.readdirSync(arch).filter(n => /^Microsoft\.VC\d+\.CRT$/i.test(n))) search.push(path.join(arch, crt));
    }
  }
}
search.push(...(process.env.PATH || '').split(path.delimiter));
const system = path.join(process.env.SystemRoot || 'C:/Windows', 'System32');
const queue = [path.resolve(backend)];
const files = new Map();
while (queue.length) {
  const file = queue.shift();
  const name = path.basename(file).toLowerCase();
  if (files.has(name)) continue;
  files.set(name, file);
  for (const dll of imports(file)) {
    if (/^(api-ms-|ext-ms-)/i.test(dll)) continue; // Windows API sets
    const isCrt = /^(msvcp|vcruntime|concrt)\d/i.test(dll);
    // OS components are supplied by Windows; CRT redistributables must be bundled.
    if (!isCrt && fs.existsSync(path.join(system, dll))) continue;
    const candidate = search.filter(Boolean).map(dir => path.join(dir, dll)).find(fs.existsSync);
    if (!candidate) throw new Error(`Missing ${dll} imported by ${name}. Set ARN_RUNTIME_DIR to its redistributable directory.`);
    queue.push(candidate);
  }
}
fs.mkdirSync(stage, { recursive: true });
const resources = {};
for (const [name, source] of files) {
  fs.copyFileSync(source, path.join(stage, name));
  resources[`installer/runtime/${name}`] = name;
}
fs.writeFileSync(path.join(stage, 'bundle-resources.json'), JSON.stringify({ bundle: { resources } }, null, 2) + '\n');
fs.writeFileSync(path.join(stage, 'manifest.json'), JSON.stringify([...files.keys()].sort(), null, 2) + '\n');
console.log(`Staged ${files.size} runtime files: ${[...files.keys()].join(', ')}`);
