// Stand-in for arn.exe and arn-ide.exe in tests/ide_package_test.ps1. Each
// build prints a fixed `--version` line and carries a Windows version
// resource (tests/ide_package_fixture.rc.in), both set by CMake.
#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--version") == 0) {
        std::puts(ARN_FIXTURE_VERSION_LINE);
    }
    return 0;
}
