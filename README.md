# Arn Agent Code

**Arn Agent Code** is a fast, native C++23 terminal agent for coding workflows.
It talks directly to LLM APIs—currently Gemini and DeepSeek—without Node.js,
Python, or a heavyweight runtime.

> Status: early prototype. API-backed chat, provider streaming, in-memory
> conversation context, and confirmed local file tools work. Persistent
> configuration and shell command execution are planned.

## Highlights

- Native C++23 CLI executable: `arn`
- Interactive shell with live `/` hints and Tab completion
- Retained terminal UI: resize-safe transcript, adaptive layout, and a clean
  return to the original PowerShell screen on exit
- Mouse-wheel transcript scrolling, plus `Page Up`, `Page Down`, and `End`
- Text selection with `Shift` + mouse drag and copying with `Ctrl+C`
- Gemini and DeepSeek API support
- Model discovery per API key and `/model` selection
- API keys held only in process memory
- In-memory conversation context for the active provider and model
- Reused HTTPS connections and automatic retry for temporary API failures
- Streaming model responses for Gemini and DeepSeek
- Cancel an active model request with `Esc` or `Ctrl+C`
- Agent tools for listing, reading, creating, editing, and deleting project files
- HTTPS via OpenSSL-backed `cpp-httplib`

## Quick start

### Prerequisites

- CMake 3.30 or later
- A C++23 compiler (MSVC, Clang, or GCC)
- OpenSSL development libraries
- Git and network access for CMake dependencies

### Build

```powershell
cmake -S . -B build
cmake --build build --config Release
```

On Windows, make sure `libssl-4-x64.dll` and `libcrypto-4-x64.dll` are beside
`arn.exe` before launching it.

### Run

```powershell
.\build\Release\arn.exe
```

Inside Arn:

```text
/key-gemini YOUR_KEY
/models
/model gemini-...
Explain how this CMake project is organized.
```

Use `/help` to view every available command.

Long transcripts can be reviewed with the mouse wheel or `Page Up` and
`Page Down`; press `End` to return to the latest message. Because ARN captures
mouse input for scrolling, hold `Shift` while dragging to select terminal text.

### Install a Windows release

After a Windows release has been published, install the latest version from
PowerShell with:

```powershell
irm https://raw.githubusercontent.com/arnecto/arn/main/scripts/install.ps1 | iex
```

This installer downloads `arn.exe` and its OpenSSL DLLs to
`%LOCALAPPDATA%\Arn\bin`, then adds that directory to the current user's PATH.
Open a new terminal and run `arn`.

The repository and its releases must be public for that one-line installer to
work on an arbitrary computer. For a private repository, download a release
while signed in to GitHub or use `gh auth login` first.

## Providers and key handling

Keys are entered through `/key-gemini` or `/key-deepseek` and are kept only in
memory until the process exits. Do not paste production keys into issue reports,
screenshots, commits, or shell-history-friendly commands.

Provider usage can incur costs and is subject to each provider's account limits.

## Local agent tools

When you ask the selected model to work on a project, ARN supplies the same
local tools to Gemini and DeepSeek. The model can request these actions:

- `list_files` and `read_file` — inspect the current project automatically;
- `write_file` and `replace_text` — create or change a file after a `y/N`
  confirmation;
- `delete_file` — permanently remove one regular file, only after an explicit
  request and confirmation.

ARN captures the directory from which it was launched as its project root. It
rejects absolute paths and paths that escape that folder, skips `.git` and
environment files, and limits reads to 256 KiB. It does not execute shell
commands in this release.

## Conversation context

ARN keeps the current chat in memory while it is running, so follow-up prompts
can refer to previous messages and tool results. Context is never written to
disk. Changing provider, setting a new key, selecting another model, using
`/clear-session`, or exiting ARN starts a fresh chat. To keep requests bounded,
the oldest entries are discarded after the history reaches 40 entries.

## Architecture

```text
include/              Public interfaces
src/main.cpp          Terminal UI, command routing, and interactive input
src/api_client.cpp    Provider HTTP calls and JSON response parsing
src/config_manager.cpp
                      Configuration placeholder
src/tool_executor.cpp Safe local file tools and project-root boundary checks
```

The dependency stack is deliberately small:

- [cpp-httplib](https://github.com/yhirose/cpp-httplib) for HTTPS requests
- [nlohmann/json](https://github.com/nlohmann/json) for JSON

## Development

See [CONTRIBUTING.md](CONTRIBUTING.md) for the contribution workflow and
[SECURITY.md](SECURITY.md) for reporting vulnerabilities.

## Website

The GitHub Pages landing page lives in [`docs/index.html`](docs/index.html).
To publish it, open the repository's **Settings → Pages**, select **Deploy from
a branch**, then choose `main` and the `/docs` folder. GitHub will publish it
at `https://arnecto.github.io/arn/`.

## License

ARN is available under the [MIT License](LICENSE). You may use, modify, and
redistribute the project under the license terms; keep the included copyright
and license notice with substantial copies of the software.
