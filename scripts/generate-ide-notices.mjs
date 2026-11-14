// Writes the third-party notices for the Windows IDE (arn-ide.exe and its
// web frontend) from the license files that the exact dependency versions
// ship. Nothing is downloaded: Rust crates are read from Cargo's registry
// (after `cargo fetch`/`cargo build --locked`) and npm packages from
// ide/node_modules (after `npm ci`).
//
//   node scripts/generate-ide-notices.mjs <output file>
//
// Included:
// - Rust crates linked into arn-ide.exe for x86_64-pc-windows-msvc: normal
//   dependencies reachable from arn-ide, without build-only or dev-only
//   dependencies and without proc-macro crates (those run at build time only);
// - npm packages bundled into the frontend: the production dependency tree
//   of ide/package.json.
// Every license, licence, copying, notice and copyright file of a package is
// reproduced verbatim (identical texts once, with the packages that ship
// them). A package without any such file is listed with its declared license
// and repository; the script reports them and exits with status 2 if the
// list is not empty, unless --allow-missing is given.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { execFileSync, execSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('../', import.meta.url));
const ide = path.join(root, 'ide');
const output = process.argv[2];
const allowMissing = process.argv.includes('--allow-missing');
if (!output) throw new Error('usage: node scripts/generate-ide-notices.mjs <output file> [--allow-missing]');

// License, notice and copyright files, including third-party notice files
// such as monaco-editor's ThirdPartyNotices.txt.
const licenseFile = /^(licen[cs]e|copying|notice|copyright|unlicense|third[-_ ]?party[-_ ]?notices?)([-._].*)?$/i;
function licenseFiles(dir) {
  return fs.readdirSync(dir, { withFileTypes: true })
    .filter(e => e.isFile() && licenseFile.test(e.name))
    .map(e => e.name)
    .sort();
}

// Rust crates.
const metadata = JSON.parse(execFileSync('cargo', ['metadata', '--format-version', '1', '--locked',
  '--filter-platform', 'x86_64-pc-windows-msvc',
  '--manifest-path', path.join(ide, 'src-tauri', 'Cargo.toml')], { encoding: 'utf8', maxBuffer: 256 << 20 }));
const packages = new Map(metadata.packages.map(p => [p.id, p]));
const nodes = new Map(metadata.resolve.nodes.map(n => [n.id, n]));
const isProcMacro = p => p.targets.some(t => t.kind.includes('proc-macro'));
const crates = new Set();
for (const queue = [metadata.resolve.root]; queue.length;) {
  const id = queue.shift();
  for (const dep of nodes.get(id).deps) {
    if (!dep.dep_kinds.some(k => k.kind === null)) continue;   // build/dev only
    const p = packages.get(dep.pkg);
    if (isProcMacro(p) || crates.has(dep.pkg)) continue;
    crates.add(dep.pkg);
    queue.push(dep.pkg);
  }
}
const components = [...crates].map(id => {
  const p = packages.get(id);
  return { kind: 'crate', name: p.name, version: p.version, license: p.license || '',
           repository: p.repository || '', dir: path.dirname(p.manifest_path) };
});

// npm packages in the frontend bundle.
// A fixed command line without outside input (npm is a .cmd file on Windows).
const tree = JSON.parse(execSync('npm ls --omit=dev --all --json', { cwd: ide, encoding: 'utf8', maxBuffer: 64 << 20 }));
const seenNpm = new Set();
(function walk(deps) {
  for (const [name, info] of Object.entries(deps || {})) {
    const key = `${name}@${info.version}`;
    if (!seenNpm.has(key)) {
      seenNpm.add(key);
      const dir = path.join(ide, 'node_modules', ...name.split('/'));
      const manifest = JSON.parse(fs.readFileSync(path.join(dir, 'package.json'), 'utf8'));
      if (manifest.version !== info.version) throw new Error(`${name}: node_modules has ${manifest.version}, npm ls says ${info.version}`);
      const repo = typeof manifest.repository === 'string' ? manifest.repository : manifest.repository?.url || '';
      components.push({ kind: 'npm', name, version: info.version,
                        license: typeof manifest.license === 'string' ? manifest.license : JSON.stringify(manifest.license || ''),
                        repository: repo, dir });
    }
    walk(info.dependencies);
  }
})(tree.dependencies);

components.sort((a, b) => a.kind.localeCompare(b.kind) || a.name.localeCompare(b.name) || a.version.localeCompare(b.version));

// License files for crates whose package ships none, copied from their
// repository at the exact commit (ide/third-party-licenses/sources.json).
const thirdPartyDir = path.join(ide, 'third-party-licenses');
const upstream = JSON.parse(fs.readFileSync(path.join(thirdPartyDir, 'sources.json'), 'utf8')).crates;
const gitBlob = text => crypto.createHash('sha1').update(`blob ${Buffer.byteLength(text)}\0`).update(text).digest('hex');

// Group identical license texts.
const texts = new Map();   // hash -> { text, users: [] }
const missing = [];
const notes = [];
function addText(raw, user) {
  const text = raw.replace(/\r\n?/g, '\n').replace(/\s+$/, '') + '\n';
  const hash = crypto.createHash('sha256').update(text).digest('hex');
  if (!texts.has(hash)) texts.set(hash, { text, users: [] });
  texts.get(hash).users.push(user);
}
for (const c of components) {
  const files = licenseFiles(c.dir);
  for (const file of files) addText(fs.readFileSync(path.join(c.dir, file), 'utf8'), `${c.name} ${c.version} (${file})`);
  if (files.length) continue;
  const entry = c.kind === 'crate' ? upstream[`${c.name} ${c.version}`] : undefined;
  if (!entry) { missing.push(c); continue; }
  for (const file of entry.files) {
    const raw = fs.readFileSync(path.join(thirdPartyDir, ...file.path.split('/')), 'utf8').replace(/\r\n?/g, '\n');
    if (gitBlob(raw) !== file.blob) throw new Error(`${file.path} is not the upstream file (blob ${gitBlob(raw)}, expected ${file.blob})`);
    addText(raw, `${c.name} ${c.version} (${path.posix.basename(file.path)} from ${entry.repository} at ${entry.commit.slice(0, 12)}; not in the package)`);
  }
  if (entry.note) notes.push(`  ${c.name} ${c.version}: ${entry.note}`);
}

const rule = '\n' + '='.repeat(80) + '\n\n';
let out = `ARN IDE for Windows - third-party notices

The ARN IDE (arn-ide.exe and its web frontend) is licensed under the MIT
License (see LICENSE). It is built from the third-party components listed
below. Each component remains under its own license. The license, copyright
and notice files that each component's published package ships are
reproduced verbatim; identical texts appear once, followed by the components
that ship them.

The ARN Code backend bundled with the IDE (arn.exe, OpenSSL and Microsoft
Visual C++ runtime DLLs) is covered by THIRD-PARTY-NOTICES.txt.

The npm packages are the production dependency tree of the frontend, which
can be more than the bundle contains. The code editor, Monaco Editor, is
bundled with the IDE; its own third-party notices (ThirdPartyNotices.txt of
monaco-editor) are reproduced below with its license.

Components (${components.length}: ${components.filter(c => c.kind === 'crate').length} Rust crates linked into arn-ide.exe, ${components.filter(c => c.kind === 'npm').length} npm packages in the frontend's production dependency tree)
`;
for (const c of components) out += `  ${c.kind === 'crate' ? 'crate' : 'npm  '}  ${c.name} ${c.version}  [${c.license || 'no license declared'}]${c.repository ? '  ' + c.repository : ''}\n`;
// MPL-2.0 (section 3.2) asks that recipients of the executable form be told
// how to obtain the source code form: these are the published sources of the
// exact versions in use.
const mpl = components.filter(c => /\bMPL-2\.0\b/.test(c.license));
if (mpl.length) {
  out += rule + 'Source code of the components above that are offered under the Mozilla Public License 2.0\n' +
    '(the published package of the exact version in use; also see each repository):\n';
  for (const c of mpl) {
    const source = c.kind === 'crate'
      ? `https://crates.io/api/v1/crates/${c.name}/${c.version}/download`
      : `https://registry.npmjs.org/${c.name}/-/${c.name.split('/').pop()}-${c.version}.tgz`;
    out += `  ${c.name} ${c.version}  [${c.license}]  ${source}\n`;
  }
}
if (notes.length) out += rule + 'Notes on components whose license files are not in their package:\n' + notes.join('\n') + '\n';
if (missing.length) {
  out += rule + `Components whose published package contains no license file (declared license and source shown above):\n`;
  for (const c of missing) out += `  ${c.name} ${c.version}  [${c.license || 'no license declared'}]  ${c.repository}\n`;
}
for (const { text, users } of [...texts.values()].sort((a, b) => a.users[0].localeCompare(b.users[0]))) {
  out += rule + 'Used by:\n' + users.map(u => `  ${u}\n`).join('') + '\n' + text;
}
fs.writeFileSync(output, out);
console.log(`Wrote ${output}: ${components.length} components, ${texts.size} distinct license texts, ${missing.length} without a license file.`);
for (const c of missing) console.log(`  no license file: ${c.kind} ${c.name} ${c.version} [${c.license}]`);
if (missing.length && !allowMissing) process.exit(2);
