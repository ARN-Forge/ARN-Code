# ARN IDE / C++ backend integration

The IDE starts a separate `arn --server` process with the opened project as its working directory. Rust is only a JSONL transport and process supervisor; provider access, model discovery, conversation history and agent file tools run in C++. The earlier `ide/src-tauri/src/provider.rs` file is retained from the unfinished work but is not compiled or used.

## Run on Windows

From the repository root:

```powershell
cmake -S . -B build-ide
cmake --build build-ide --config Release
Set-Location ide
$env:CARGO_BUILD_JOBS = '1'
$env:CARGO_PROFILE_DEV_DEBUG = '0'
npm run tauri:dev
```

Build dependencies must already be installed (CMake, the C++ toolchain, OpenSSL development libraries, Rust/Tauri prerequisites, Node dependencies). This checkout has `ide/node_modules`; a new checkout also needs `npm ci` in `ide`.

Executable discovery checks `ARN_BIN`, Tauri's resource directory, the directory beside the IDE executable, repository-relative development build directories, then PATH. Set `ARN_BIN` to an absolute executable path to override discovery. Runtime DLLs must be discoverable by the Windows loader (beside ARN or on PATH). No user-specific fallback path is embedded. The Windows portable release ZIP includes ARN and its OpenSSL DLLs beside the IDE; it is not an installer.

Open a project, enter a Gemini or DeepSeek API key, click **Verify access and load models**, then choose a returned model and click **Use selected model**. Keys are sent through the private stdin pipe and HTTPS headers, kept in process memory, and never written to configuration, arguments, URLs or logs. A successful model-list request verifies provider access; it does not guarantee a particular model's generation quota or tool support.

## File review and editor behavior

- Create, replace and delete tools show the normalized project-relative path and full before/after text. Approval expires after 120 seconds and defaults to denial. Duplicate, stale and cross-session confirmations are rejected/ignored.
- Save or close dirty tabs before sending a prompt. While the agent is active the editor is read-only and manual saves are rejected. This deliberately conservative policy prevents a user edit racing an already approved agent write.
- File events refresh the tree and clean editor buffers. Deleted/unreadable open buffers are preserved as dirty recovery buffers; close them if no longer needed.
- Before an approved operation executes, C++ resolves the entire path again (including symlink leaves) and compares the current file with the preview snapshot. Traversal, targets outside the root, protected paths (`.git`, `.env*`, private-key files and service directories), and targets changed during review are refused.
- Project switching stops/reaps the previous process and discards its credentials and conversation. New projects require provider verification again. Session tokens prevent delayed commands/events from affecting a new project.
- Normal close requests cancellation and EOF, waits up to two seconds, then kills/reaps an unresponsive child. Unexpected EOF rejects pending Rust requests and clears connection status. Reopen the project to restart a failed process.

## Protocol 2

Startup: `{"type":"ready","protocol":2}`. Request commands carry a unique `id`; replies/events carry the corresponding `requestId`.

Commands: `configure` (provider and apiKey; verifies access and returns `models_listed`), `list_models` (verified session catalogue), `select_model`, `prompt`, `clear_session`, `cancel`, `confirmation_response`. All work commands are serialized; concurrent work gets a correlated busy error. Cancel/approval remain responsive on the stdin thread while the worker runs. EOF cancels and joins the worker, including confirmation waits.

Events: `progress`, `stream`, `confirmation_required`, `confirmation_resolved`, `files_changed`, and terminal `models_listed`, `configured`, `session_cleared`, `complete`, `cancelled`, `error`. `progress` reports provider attempts, retry backoff and tool execution without completing the request. The panel displays this stage and elapsed time instead of an empty assistant bubble. Each successful file operation emits `files_changed`; it does not imply the overall prompt succeeded. Prompt errors/cancellation reset conversation state to avoid retaining incomplete tool exchanges.

The bridge waits five seconds for readiness, 120 seconds for configuration/control operations, and 15 minutes for a prompt. A transport timeout shuts down the process rather than letting late replies match a later request. Network retries check cancellation during backoff. A streaming HTTP read failure is returned immediately after that attempt, rather than repeating a full 90-second idle timeout. Other retryable failures retain the existing retry policy and now report progress. Provider error bodies are not exposed by the server, as they can echo credentials.

## Local verification

```powershell
cmake --build build-ide --config Release
ctest --test-dir build-ide -C Release --output-on-failure
npm run build --prefix ide
$env:CARGO_BUILD_JOBS = '1'
$env:CARGO_PROFILE_DEV_DEBUG = '0'
cargo check --manifest-path ide/src-tauri/Cargo.toml
$env:ARN_TEST_BIN = (Resolve-Path build-ide/Release/arn_server_fixture.exe).Path
cargo test --manifest-path ide/src-tauri/Cargo.toml -- --nocapture
```

The C++ fixture is a separate test executable with a fake provider, linked to the real server and tool executor. The production ARN binary has no fake-provider mode. Tests cover command correlation, concurrent command rejection, no write before approval, denial, approval, stale operation IDs, cancellation, confirmation timeout, external edits during review, traversal/protected paths, EOF (including a final line without newline), and Rust transport/process shutdown. Symlink tests explicitly report a skip if the host cannot create symlinks.

Windows sandbox restrictions can prevent MSBuild FileTracker, child-process access to temporary test projects, or the CMake-selected Python executable from working; run these checks in a normal developer terminal when that occurs.

No live provider request or interactive desktop UI validation was performed for this implementation. These local tests do not prove real Gemini/DeepSeek tool-call behavior. Test one small file creation, rejection, cancellation, project switch and IDE close with your own key before relying on the integration for valuable work. This is project-boundary validation, not an OS sandbox against another local process actively racing filesystem changes.

## Verification result for this checkout

- C++ Release build: passed (`build-ide/Release/arn.exe`).
- CTest: 3/3 passed. Windows junction escape tests passed. Ordinary symlink creation was skipped because the account lacks the symbolic-link privilege.
- `npm run build`: passed.
- `cargo check`: passed with `CARGO_BUILD_JOBS=1`, `CARGO_PROFILE_DEV_DEBUG=0`.
- Rust integration test: 1/1 passed with the same limits. It exercises streaming, correlated replies, busy rejection, approval, cancellation, process replacement identity and shutdown during pending approval; child processes are reaped.
- `arn --version`: passed (`arn 0.5.0`).
- `git diff --check`: passed (Git reports existing LF/CRLF normalization warnings).
- No live API credentials were used. Real-provider generation/tool calls and interactive desktop behavior remain unverified.

The subsequent contrast/progress update was visually checked in a local browser preview: connection controls, disabled buttons and placeholder text use explicit dark-theme colors. Provider settings collapse after connection. Live provider latency was not measured; progress events and the read-failure retry policy make waiting visible and prevent repeating a full idle read timeout.
