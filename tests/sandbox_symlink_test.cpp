// A link inside the project that points outside it must not let a tool read
// or write the target. Creating the link needs a privilege some systems do
// not grant (Windows without Developer Mode or administrator rights); there
// the test checks nothing, says so and exits with 77, which CTest reports as
// skipped (SKIP_RETURN_CODE), never as passed.

#include "tool_executor.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <source_location>
#include <string>

namespace {

void check(bool value, std::source_location where = std::source_location::current()) {
    if (!value) {
        std::cerr << "Assertion failed at line " << where.line() << std::endl;
        std::exit(1);
    }
}

std::string contents(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("arn-symlink-test-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "project");
    std::ofstream(root / "outside") << "outside";
    std::error_code ec;
    std::filesystem::create_symlink(root / "outside", root / "project/link", ec);
    if (ec) {
        std::cout << "SKIPPED: a symbolic link could not be created (" << ec.message()
                  << "), so links out of the project were not tested.\n";
        std::filesystem::remove_all(root); // Unique test-owned temporary tree only.
        return 77;
    }
    arn::ToolExecutor tools(root / "project");
    auto yes = [](const arn::ToolRequest&) { return true; };
    check(!tools.execute("write_file", {{"path", "link"}, {"content", "bad"}}, yes).ok);
    check(!tools.execute("read_file", {{"path", "link"}}, yes).ok);
    check(contents(root / "outside") == "outside");
    std::filesystem::remove_all(root); // Unique test-owned temporary tree only.
    std::cout << "sandbox symlink tests passed\n";
    return 0;
}
