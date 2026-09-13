# Contributing to Arn Agent Code

Thanks for contributing.

## Before opening a pull request

1. Create a focused branch from the default branch.
2. Keep one logical change per pull request.
3. Do not commit API keys, local DLLs, build directories, or account data.
4. Format modified C++ files with the repository's `.clang-format` settings.
5. Configure and build the project locally:

   ```powershell
   cmake -S . -B build
   cmake --build build --config Release
   ```

## Pull requests

Explain the problem, the implementation, and how you tested it. Include
terminal output or screenshots for interactive CLI changes where useful.
