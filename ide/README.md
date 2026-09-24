# ARN IDE v0.6.0

ARN's desktop editor uses Tauri 2, React and Monaco. Its AI providers, model
catalogue, conversation and file tools run in the C++ ARN engine through
`arn --server`. Rust supervises that process and forwards JSONL events.

## Windows download

Extract **arn-ide-windows-x64.zip** from the
[GitHub release](https://github.com/ARN-Forge/ARN-Code/releases/tag/v0.5.0) and run
`arn-ide.exe`. Keep `arn.exe` and the OpenSSL DLLs beside it. Windows WebView2
Runtime is required. This is an unsigned portable archive.

## Development

Install Node.js, a current stable Rust toolchain, CMake, a C++23 compiler,
OpenSSL development libraries, and the platform's Tauri prerequisites.
From the repository root on Windows:

```powershell
cmake -S . -B build-ide
cmake --build build-ide --config Release
Set-Location ide
npm ci
$env:CARGO_BUILD_JOBS = '1'
$env:CARGO_PROFILE_DEV_DEBUG = '0'
npm run tauri:dev
```

The development command starts Vite and Tauri. Build C++ separately as above.
ARN is found in repository-relative build folders; `ARN_BIN` can override its
path. An existing process is stopped when its project changes or the IDE exits.

To build the standalone IDE executable from `ide`:

```powershell
npm run tauri -- build --no-bundle
```

The executable is in `src-tauri/target/release`. For distribution it must be
packaged with ARN and its runtime libraries; the release workflow does this.

## Windows installer

The branded NSIS installer uses Arny artwork, English/Ukrainian setup text,
per-user installation, Start menu integration, optional desktop shortcut,
upgrade handling and the Windows uninstaller. Missing WebView2 is downloaded
from Microsoft during setup (internet required in that case).

Build the C++ backend first, then run from `ide`:

```powershell
$env:CARGO_BUILD_JOBS = '1'
$env:CARGO_PROFILE_DEV_DEBUG = '0'
npm run installer:build
npm run installer:test
```

Output: `src-tauri/target/release/bundle/nsis/ARN IDE_0.6.0_x64-setup.exe`.
`ARN_BIN` can select the C++ executable; `ARN_RUNTIME_DIR` can supply DLL search
directories separated by semicolons. The packager follows x64 PE imports and
includes the matching OpenSSL and MSVC redistributable DLLs. It fails if a
required dependency cannot be found. Runtime payloads are ignored by Git.

Use `installer:build` for distribution; a raw `tauri build` does not stage
the backend. The Windows release job uploads both the installer and portable ZIP.
The installer is unsigned until a code-signing identity is configured; Windows
may display an unknown-publisher warning. It does not request provider credentials.

Regenerate the checked-in artwork with `./scripts/generate-installer-art.ps1`
from the repository root. `installer/branding.nsh` customizes Tauri's maintained
NSIS UI without replacing its installation and upgrade logic.

## Using the agent

Open a project, verify your Gemini or DeepSeek API key, load the provider's
models and select one. Requests stream text and report progress. Every agent
file mutation requires approval of its before/after preview, with a
120-second default-deny timeout. Keys remain in memory.

Save or close unsaved tabs before sending a prompt. While a request is active,
the editor is read-only; file events refresh the tree and clean editor buffers.
Agent paths are resolved against the project root, including symlinks and
junctions. This is path validation, not an OS sandbox against hostile local
processes racing filesystem operations.

The IDE currently has no terminal, debugger or Git UI. Agent tools accept
text files up to 256 KiB; editor reads are limited to 5 MiB.

## Tests and protocol

See the [integration guide](../docs/ide-arn-bridge.md) for protocol 2,
executable discovery, security behavior, local test commands and validation
limits. The [release notes](../docs/releases/v0.5.0.md) describe this version.

## Icon

`app-icon.svg` reproduces the CLI's orange block-character Arny crab as vector
geometry. Regenerate it and the Tauri assets from the repository root:

```powershell
node scripts/generate-ide-icon.mjs
Set-Location ide
npm run tauri -- icon app-icon.svg --output src-tauri/icons
```

## License

MIT, like the rest of ARN.
