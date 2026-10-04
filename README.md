# ARN Code

**ARN Code is a lightweight native C++23 coding assistant for projects on your
machine.** Its native providers stream answers from Gemini, DeepSeek, OpenRouter,
or a local OmniRoute server, with project file tools that require approval for
changes. The CLI also supports Kiro through ACP as an external agent backend.
Native provider mode has no Node.js or Python runtime dependency; external
backends have their own installation and authentication requirements.

Use it when you want a small terminal-first assistant—or the included Windows
IDE—while keeping provider calls, model selection, and local file access under
your control.

ARN is the broader ecosystem. ARN Code is built on
[ARN Core](https://github.com/ARN-Forge/arn-core), the reusable native C++23 AI
agent framework shared by ARN applications.

![ARN Code terminal demo](docs/assets/arn-demo.gif)

## What ARN Code does today

- Runs as a native `arn` executable on Windows, Linux, and macOS release
  targets. Windows also has a portable Tauri 2 + React IDE.
- Connects to **Google Gemini**, **DeepSeek**, **OpenRouter**, and **OmniRoute**
  using your own API key, or to an installed/authenticated **Kiro CLI via ACP**.
- Verifies native provider access through model discovery and lets you choose
  a model with `/models` and `/model`. A listed model is not a guarantee of
  generation access for the current account/session.
- Streams responses, retries temporary API failures, and cancels an active
  request with `Esc` or `Ctrl+C`.
- Keeps native provider keys and conversation history in memory only. Kiro
  manages its own credentials, sessions, and tools outside ARN Code.
- Lets native models list and read project files, or request creates, edits, and
  deletions that require explicit approval.
- Provides Explorer, Planner, Coder, and Reviewer workflows through `/agent`,
  with the same provider, sandbox, and confirmation boundaries as native chat.

The provider foundation from **Phase 1** of
[AGENTS_ROADMAP.md](AGENTS_ROADMAP.md) remains in place: `ApiClient` is a
provider-neutral facade over ARN Core. The CLI's ACP integration is a separate
transport for external agents, not an additional native model provider.

## Install

### Windows

Run this in **PowerShell**, not Command Prompt (`cmd.exe`):

```powershell
irm https://raw.githubusercontent.com/ARN-Forge/ARN-Code/main/scripts/install.ps1 | iex
arn
```

The installer uses `%LOCALAPPDATA%\Arn\bin`, adds it to your user `PATH`, and
verifies the installed executable. Open a new terminal if `arn` is not yet
visible in another terminal window.

### Linux and macOS

```bash
curl -fsSL https://raw.githubusercontent.com/ARN-Forge/ARN-Code/main/scripts/install.sh | sh
arn
```

The script installs to `~/.local/bin`, adds that directory to your shell
profile when needed, and verifies the executable. Unix archives use system
OpenSSL libraries: on Linux install the OpenSSL runtime supplied by your
distribution; on macOS run `brew install openssl@3` first.

### Manual download

Download an archive from the [latest release page](https://github.com/ARN-Forge/ARN-Code/releases/latest),
extract it, and keep its files together so the executable can find its bundled
runtime libraries.

Available release archives are:

- **Windows x64 CLI:** `arn-windows-x64.zip`
- **Windows x64 IDE:** `arn-ide-windows-x64.zip` — extract it and start
  `arn-ide.exe`. It requires the [Microsoft Edge WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/).
- **Windows x64 IDE installer:** `arn-ide-windows-x64-setup.exe`
- **Linux x64 CLI:** `arn-linux-x64.tar.gz`
- **macOS Apple Silicon CLI:** `arn-macos-arm64.tar.gz`
- **macOS Intel CLI:** `arn-macos-x64.tar.gz`

## First request

Open a terminal in the project you want ARN Code to work on and start the CLI:

```text
arn
```

Then configure one provider for this session:

```text
/key-gemini
/models
/model <model-id-from-/models>
Explain how this project is organized.
```

Replace the model placeholder with an exact ID from `/models`. Use
`/key-deepseek`, `/key-openrouter`, or `/key-omniroute` for another native
provider. ARN Code opens a masked prompt; the key is never echoed and remains
in memory only.
`/help` lists all interactive commands, including `/provider`, `/status`,
`/clear-session`, and `/clear`.

Provider and agent commands:

- `/provider <gemini|deepseek|openrouter|omniroute>` selects a native provider
  and clears the previous key/model/context. Enter its key to verify access.
- `/models` lists the native provider's returned catalog, or Kiro's models if
  its CLI exposes discovery.
- `/model <id>` selects a catalog model in native mode; in ACP mode the backend
  validates the selected ID.
- `/agent <task>` runs Explorer → Planner, asks whether to continue, then runs
  Coder → Reviewer.
- `/agent --auto <task>` runs the same workflow without the plan checkpoint;
  every requested file change still needs confirmation.
- `/agent explorer <task>`, `/agent planner <task>`, `/agent coder <task>`, and
  `/agent reviewer <task>` run one profile directly. These workflows use native
  providers, not the external Kiro ACP session.

### OmniRoute

Start your local OmniRoute server, then run `/provider omniroute` and
`/key-omniroute`. The built-in endpoint is **`http://localhost:20128/v1`**;
no environment setup is needed for a standard local installation.
`ARN_OMNIROUTE_BASE_URL` is an optional override for a different server, and
`/status` shows the effective endpoint. Key verification uses that endpoint.

`/models` shows OmniRoute's actual catalog. Availability can differ between
`kiro/` and `kr/` IDs and depends on the backend/account/session. If the backend
returns HTTP 400 `Invalid model`, ARN Code explains that the model is unavailable
and suggests selecting another with `/models`; it does not maintain an allowlist.

### Kiro through ACP

Install a Kiro CLI version that supports `kiro-cli acp` and authenticate it
separately with `kiro-cli login`. Keep `kiro-cli` on `PATH`, or set the optional
`ARN_KIRO_BIN` override to its executable path. It is not bundled with ARN Code.

Run `/provider kiro` (equivalently `/acp kiro`), then send a normal chat message.
`/acp` shows backend status; `/models` attempts Kiro model discovery, and
`/model <id>` asks the backend to switch models. `/acp off` leaves ACP mode;
select a native provider and enter its key again to reconnect.

ARN Code displays permission requests received over ACP and maps each `y/N`
choice to the server's exact permission option ID. Rejection or cancellation
does not grant access. Kiro owns its tools and security policy: ARN Code's
native project-root and sensitive-file restrictions do **not** sandbox that
external process, and only actions for which the backend requests permission
are presented for approval.

Native provider API keys stay in process memory until ARN Code exits. Inline key arguments are
disabled, so enter keys only through the masked prompt. Never put keys in commits,
issue reports, or screenshots. Provider usage may
incur charges and is subject to the provider's quota and availability.

## Safe local tools

In native provider mode, ARN Code treats its launch directory as the project
root. A model may use these tools:

- `list_files` and `read_file` inspect the project.
- `write_file` and `replace_text` show the proposed content or change and wait
  for a `y/N` confirmation.
- `delete_file` requires an explicit request and confirmation.

Paths are confined to the project root, including checks intended to prevent
escapes through `..` and links. ARN Code protects `.git`, environment and common
secret files, and limits reads to 256 KiB. It does not execute shell commands.
The IDE applies the same confirmation protocol and keeps unsaved editor buffers
from being overwritten by an approved agent operation.

## ARN Code IDE

The optional Windows desktop IDE for ARN Code uses Tauri 2, React, and Monaco
for the user interface. It does **not** reimplement provider or file-tool logic
in Rust: it starts the existing C++ executable with `arn --server` and exchanges
JSONL messages with it.

Its current agent panel supports Gemini and DeepSeek. OmniRoute and Kiro ACP
are CLI features. It includes a project file tree, multi-tab editor, provider
verification and model selection, streamed chat, cancellation, and a before/after review for
each requested file change. Read [the IDE guide](ide/README.md) and the
[bridge protocol guide](docs/ide-arn-bridge.md) for setup, limitations, and
test commands.

## Build from source

Building the CLI requires CMake 3.20+, a C++23 compiler, OpenSSL development
files, Git, and network access for CMake's fetched dependencies.

The default test-enabled build also requires Node.js for the fake ACP fixtures;
install Python 3 to include server protocol tests. These are test dependencies,
not native CLI runtime dependencies. Use `-DBUILD_TESTING=OFF` for a CLI-only
build without the test dependencies.

CMake uses an explicit/local sibling ARN Core checkout first, then an installed
Core package, otherwise fetches the pinned canonical Core revision. For a release
build, use a clean checkout without an adjacent Core checkout or installed Core
package so developer changes cannot override the pin.

Clone the canonical repository first:

```bash
git clone https://github.com/ARN-Forge/ARN-Code.git
cd ARN-Code
```

### Windows

```powershell
cmake -S . -B build
cmake --build build --config Release
.\build\Release\arn.exe
```

Ensure the OpenSSL runtime DLLs selected by your build are available beside
`arn.exe` or on `PATH`.

### Linux

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build libssl-dev nodejs python3
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/arn
```

### macOS

```bash
brew install cmake ninja openssl@3 node python
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
cmake --build build --parallel
./build/arn
```

Run the full test suite with `ctest --test-dir build -C Release --output-on-failure`.
Windows x64, Linux x64, macOS arm64, and macOS x64 builds/tests run in CI.
Interactive macOS compatibility remains experimental. To develop
the IDE, see the extra Node.js and Rust/Tauri requirements in
[ide/README.md](ide/README.md); those are IDE build requirements, not CLI
runtime requirements.

## Architecture

```text
ARN Code terminal UI / desktop IDE
          |
     ApiClient facade
          |
     ARN Core ModelProvider interface
          |
     Gemini / DeepSeek / OpenRouter / OmniRoute
          |
     HTTPS + streaming API

     ToolExecutor -> confirmed, project-root-safe file operations

CLI ACP mode -> AcpSession -> AcpClient -> kiro-cli acp
                                         (external tools/authentication)
```

Provider-specific authentication, request formats, streaming parsing, model
discovery, errors, cancellation, and session history stay behind ARN Core's
provider interface. The ARN Code terminal UI, JSONL server, and IDE consume the
common behavior.

## Current limits and roadmap

ARN Code is still an early project. It has no persistent native API-key storage,
integrated terminal, debugger, Git UI, built-in offline model backend, or
guarantee that a provider accepts a particular key or model. Real provider
requests require your own valid credentials; local builds and tests do not
prove provider access.

Native tools do not execute shell commands. External ACP agents may offer their
own command execution and persistence. Normal CTest uses fake ACP fixtures and
does not require a Kiro account; real-Kiro smoke tests are opt-in with
`-DARN_ENABLE_KIRO_INTEGRATION_TESTS=ON` and skip when Kiro is unavailable.

[AGENTS_ROADMAP.md](AGENTS_ROADMAP.md) describes later work. The initial
Explorer, Planner, Coder, and Reviewer profiles are available. Skills,
automatic model routing, and isolated subagents remain planned concepts.

## Development and security

See [CONTRIBUTING.md](CONTRIBUTING.md) for the contribution workflow,
[SECURITY.md](SECURITY.md) for vulnerability reporting, and
[docs/releases/v0.8.0.md](docs/releases/v0.8.0.md) for the v0.8.0 release
notes. The project is released under the [MIT License](LICENSE).
