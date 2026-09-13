# Arn Agent Code

**Arn Agent Code** is a fast, native C++23 terminal agent for coding workflows.
It talks directly to LLM APIs—currently Gemini and DeepSeek—without Node.js,
Python, or a heavyweight runtime.

> Status: early prototype. API-backed chat works; tool execution, persistent
> configuration, conversation history, and provider streaming are planned.

## Highlights

- Native C++23 CLI executable: `arn`
- Interactive shell with live `/` hints and Tab completion
- Gemini and DeepSeek API support
- Model discovery per API key and `/model` selection
- API keys held only in process memory
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

## Providers and key handling

Keys are entered through `/key-gemini` or `/key-deepseek` and are kept only in
memory until the process exits. Do not paste production keys into issue reports,
screenshots, commits, or shell-history-friendly commands.

Provider usage can incur costs and is subject to each provider's account limits.

## Architecture

```text
include/              Public interfaces
src/main.cpp          Terminal UI, command routing, and interactive input
src/api_client.cpp    Provider HTTP calls and JSON response parsing
src/config_manager.cpp
                      Configuration placeholder
src/tool_executor.cpp Local-tool execution placeholder
```

The dependency stack is deliberately small:

- [cpp-httplib](https://github.com/yhirose/cpp-httplib) for HTTPS requests
- [nlohmann/json](https://github.com/nlohmann/json) for JSON

## Development

See [CONTRIBUTING.md](CONTRIBUTING.md) for the contribution workflow and
[SECURITY.md](SECURITY.md) for reporting vulnerabilities.

## License

No license has been selected yet. Until a `LICENSE` file is added, the source is
not automatically available for reuse or redistribution. Before publishing,
choose a license—for example MIT or Apache-2.0.
