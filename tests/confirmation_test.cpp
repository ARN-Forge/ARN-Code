#include "confirmation_gate.hpp"
#include "tool_executor.hpp"
#include <fstream>
#include <future>
#include <iostream>
#include <source_location>
#include <stdexcept>
using namespace std::chrono_literals;
void check(bool value, std::source_location where = std::source_location::current()) {
    if (!value) {
        std::cerr << "Assertion failed at line " << where.line() << std::endl;
        std::exit(1);
    }
}
int main() {
    arn::ConfirmationGate gate;
    std::string old;
    check(!gate.wait([&](auto id) { old = id; }, 10ms));
    check(!gate.answer(old, true));
    for (const bool approve : {false, true}) {
        std::promise<std::string> ready;
        auto result = std::async(std::launch::async, [&] {
            return gate.wait([&](auto id) { ready.set_value(id); }, 1s);
        });
        const auto id = ready.get_future().get();
        check(id != old);
        old = id;
        check(!gate.answer("stale", true));
        check(gate.answer(id, approve));
        check(result.get() == approve);
        check(!gate.answer(id, true));
    }
    std::promise<void> ready;
    auto cancelled =
        std::async(std::launch::async, [&] { return gate.wait([&](auto) { ready.set_value(); }); });
    ready.get_future().wait();
    gate.cancel();
    check(!cancelled.get());
    check(!gate.wait([](auto) {}, 1ms));
    gate.reset();
    check(!gate.wait([](auto) {}, 1ms));
    const auto root = std::filesystem::temp_directory_path() /
                      ("arn-tools-test-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "project");
    arn::ToolExecutor tools(root / "project");
    int confirmations = 0;
    auto yes = [&](const arn::ToolRequest&) {
        ++confirmations;
        return true;
    };
    auto no = [](const arn::ToolRequest&) { return false; };
    check(!tools.execute("write_file", {{"path", "../outside"}, {"content", "bad"}}, yes).ok);
    check(!tools.execute("write_file", {{"path", ".env.production"}, {"content", "bad"}}, yes).ok);
    check(!tools.execute("write_file", {{"path", ".GIT/config"}, {"content", "bad"}}, yes).ok);
    check(confirmations == 0);
    check(!tools.execute("write_file", {{"path", "x.txt"}, {"content", "one"}}, no).ok);
    check(!std::filesystem::exists(root / "project/x.txt"));
    check(tools.execute("write_file", {{"path", "x.txt"}, {"content", "one"}}, yes).ok);
    check(!tools
               .execute("replace_text",
                        {{"path", "x.txt"}, {"old_text", "one"}, {"new_text", "two"}},
                        [&](const arn::ToolRequest& request) {
                            check(request.arguments["before"] == "one" &&
                                  request.arguments["after"] == "two");
                            std::ofstream(root / "project/x.txt") << "external edit";
                            return true;
                        })
               .ok);
    check(tools.execute("read_file", {{"path", "x.txt"}}, no).result["content"] == "external edit");
    std::ofstream(root / "outside") << "outside";
    std::error_code ec;
    std::filesystem::create_symlink(root / "outside", root / "project/link", ec);
    if (!ec) {
        check(!tools.execute("write_file", {{"path", "link"}, {"content", "bad"}}, yes).ok);
        check(!tools.execute("read_file", {{"path", "link"}}, yes).ok);
    } else
        std::cout << "SKIP symlink creation: " << ec.message() << '\n';
    check(!tools.execute("delete_file", {{"path", "x.txt"}}, no).ok);
    check(tools.execute("delete_file", {{"path", "x.txt"}}, yes).ok);
    std::filesystem::remove_all(root); // Unique test-owned temporary tree only.
    std::cout << "confirmation/tool tests passed\n";
}
