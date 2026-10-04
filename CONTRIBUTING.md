# Contributing to ARN Code

Thanks for contributing.

The default test-enabled build needs CMake 3.20+, a C++23 compiler, OpenSSL
development libraries, Git, and Node.js for the deterministic fake ACP fixtures.
Install Python 3 to include the server protocol tests. Native CLI execution
does not require Node.js or Python.

## Before opening a pull request

1. Create a focused branch from the default branch.
2. Keep one logical change per pull request.
3. Do not commit API keys, local DLLs, build directories, or account data.
4. Format modified C++ files with the repository's `.clang-format` settings.
5. Configure and build the project locally. On Windows:

   ```powershell
   cmake -S . -B build
   cmake --build build --config Release
   ctest --test-dir build -C Release --output-on-failure
   ```

   On Linux or macOS with Ninja:

   ```bash
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build build --parallel
   ctest --test-dir build --output-on-failure
   ```

   macOS contributors should also pass
   `-DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"` when configuring.

Normal CTest does not require Kiro authentication. Real-Kiro smoke tests are
opt-in with `-DARN_ENABLE_KIRO_INTEGRATION_TESTS=ON` and skip when unavailable.
For release/dependency validation, use a clean source checkout without an
adjacent or installed ARN Core override so CMake fetches the pinned Core commit.

## Pull requests

Explain the problem, the implementation, and how you tested it. Include
terminal output or screenshots for interactive CLI changes where useful.
