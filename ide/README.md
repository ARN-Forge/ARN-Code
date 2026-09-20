# ARN IDE - Prototype

A lightweight desktop IDE for the ARN coding agent, built with Tauri 2, React, and TypeScript.

## Architecture

The IDE consists of three main components:

1. **C++ ARN Core** (`../src`, `../include`, `../CMakeLists.txt`)
   - Existing ARN CLI with new `--server` mode
   - Provides headless JSONL protocol for IDE communication
   - Manages API connections (Gemini, DeepSeek) and tool execution

2. **Tauri Backend** (`src-tauri/`)
   - Launches and supervises the ARN child process in server mode
   - Implements file system access with project-root sandboxing
   - Parses JSONL events and forwards them to the React frontend

3. **React Frontend** (`src/`)
   - File tree browser with syntax highlighting
   - Monaco Editor with multi-tab support
   - Chat panel for AI interaction
   - Change review and confirmation flow

## Prerequisites

- Node.js 18+
- Rust 1.70+
- CMake 3.20+
- A C++23 compiler (MSVC, Clang, or GCC)
- OpenSSL 3 development libraries
- Tauri CLI: `npm install --global @tauri-apps/cli`

## Development

### Build and run the IDE

```bash
cd ide
npm install
npm run tauri:dev
```

This command:
1. Builds the ARN C++ executable in the parent directory if needed
2. Starts the Vite dev server on localhost:5173
3. Launches the Tauri window

### Build the C++ ARN executable separately

```bash
cd ..
cmake -S . -B build
cmake --build build --config Release
```

On Windows, ensure `libssl-3-x64.dll` and `libcrypto-3-x64.dll` are in the build output directory.

### Production build

```bash
cd ide
npm run tauri:build
```

Output binaries are in `src-tauri/target/release/`.

## Protocol Overview

When the IDE starts, it launches `arn --server` and communicates via JSONL (one JSON object per line):

### Commands (sent by IDE to ARN)

```json
{"type":"configure","provider":"gemini","apiKey":"...","model":"gemini-2.0-flash"}
{"type":"prompt","id":"req-1","text":"Explain this code"}
{"type":"cancel","id":"req-1"}
{"type":"clear_session"}
{"type":"list_models"}
```

### Events (emitted by ARN to IDE)

```json
{"type":"ready"}
{"type":"configured","provider":"gemini","model":"..."}
{"type":"stream","requestId":"req-1","text":"..."}
{"type":"complete","requestId":"req-1"}
{"type":"error","requestId":"req-1","message":"..."}
{"type":"confirmation_required","id":"change-1","requestId":"req-1","operation":"write_file","path":"src/foo.cpp","summary":"...","diff":"..."}
```

API keys are never logged, persisted, or emitted in events.

## File Structure

```
ide/
├── index.html               Tauri HTML entry point
├── src/
│   ├── main.tsx            React entry point
│   ├── App.tsx             Main IDE component
│   ├── index.css           Global styles
│   └── ...                 Other React components (future)
├── src-tauri/
│   ├── tauri.conf.json     Tauri configuration
│   ├── src/
│   │   ├── main.rs         Tauri app entry point
│   │   └── lib.rs          Commands for file I/O
│   ├── Cargo.toml          Rust dependencies
│   ├── build.rs            Tauri build script
│   └── capabilities/       Security capability definitions
├── vite.config.ts          Vite dev server config
├── tsconfig.json           TypeScript config
├── package.json            npm dependencies
└── README.md               This file
```

## Current Status (MVP)

### Implemented

- Project folder selection and file tree browser
- Multi-tab editor with syntax highlighting (Monaco)
- File read/write with project-root sandboxing
- Basic Tauri integration
- Rust backend commands for file operations

### Not Yet Implemented

- ARN server process communication
- Chat panel and provider configuration
- Streaming response display
- File change confirmation flow
- Save/unsaved state handling
- Error messages and status display

### Known Limitations

- No symlink resolution (symlinks are treated as-is)
- File size limit of 5MB for reads/writes
- No git integration
- No terminal integration
- No debugger support
- API keys stored in memory only (not persisted)

## Future Improvements

- Persistent API key storage using OS credential managers
- Built-in terminal for shell commands
- Git UI and status integration
- Debugger support
- Plugin marketplace
- Collaborative editing
- Project indexing and smart search
- Auto-update mechanism

## Testing

Run the Rust backend tests:

```bash
cd src-tauri
cargo test
```

Run the frontend in dev mode with hot reload:

```bash
npm run dev
```

## License

Same as ARN (MIT License).
