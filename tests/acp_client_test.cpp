// Offline integration tests for arn::acp::AcpClient and arn::acp::KiroAdapter.
//
// All tests use the fake ACP server in tests/acp_fake_server.mjs. They do NOT
// require a real Kiro CLI installation, real Kiro credentials, or any
// network access.

#include "acp/acp_client.hpp"
#include "acp/acp_session.hpp"
#include "acp/kiro_adapter.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_failed = 0;
int g_passed = 0;

#define EXPECT(cond, msg)                                                       \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++g_failed;                                                          \
            std::fprintf(stderr, "FAIL: %s (%s) @ %s:%d\n",                    \
                         msg, #cond, __FILE__, __LINE__);                       \
            std::fflush(stderr);                                                 \
        } else { ++g_passed; }                                                  \
    } while (0)

#define INFO(msg)                                                               \
    do {                                                                        \
        std::fprintf(stderr, "%s\n", msg);                                      \
        std::fflush(stderr);                                                     \
    } while (0)

fs::path fake_server_path() {
    return fs::path(ARN_REPO_ROOT) / "tests" / "acp_fake_server.mjs";
}

arn::acp::SubprocessSpec fake_spec() {
    arn::acp::SubprocessSpec spec;
    spec.command = ARN_TEST_NODE;
    spec.args = { fake_server_path().string() };
    return spec;
}

void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void test_initialize_and_session_new() {
    INFO("test_initialize_and_session_new: start");
    arn::acp::AcpClient client;
    client.start(fake_spec(), arn::acp::AcpInitInfo{});
    const auto version = client.initialize({arn::acp::kLatestProtocolVersion, 1});
    EXPECT(version == arn::acp::kLatestProtocolVersion, "negotiated protocol version");
    EXPECT(client.agent_name() == "fake-acp", "agent name from initialize");
    EXPECT(client.capabilities().session_methods, "session methods advertised");

    const auto session = client.new_session(fs::current_path());
    EXPECT(session.session_id.rfind("sess_", 0) == 0, "session/new returned sessionId");
    EXPECT(!session.get_models().empty(), "session/new returned models");
    client.shutdown();
    INFO("test_initialize_and_session_new: done");
}

void test_streaming_messages() {
    INFO("test_streaming_messages: start");
    arn::acp::AcpClient client;
    client.start(fake_spec(), arn::acp::AcpInitInfo{});
    client.initialize({arn::acp::kLatestProtocolVersion, 1});
    const auto session = client.new_session(fs::current_path());

    std::vector<std::string> received_text;
    int tool_titles = 0;
    bool turn_ended = false;
    arn::acp::AcpStreamCallbacks cb;
    cb.on_agent_text = [&](std::string_view text) {
        received_text.emplace_back(text);
    };
    cb.on_tool_call = [&](const arn::acp::AcpToolCall& call) { if (!call.title.empty()) ++tool_titles; };
    cb.on_tool_call_update = [&](const arn::acp::AcpToolCall& call) { if (!call.title.empty()) ++tool_titles; };
    cb.on_turn_end = [&] { turn_ended = true; };

    const auto response = client.prompt(session,
        "__FAKE_ACP_CMD__ stream-and-finish", cb);
    EXPECT(response.ok, "stream-and-finish prompt ok");
    EXPECT(received_text.size() == 2, "two agent_message_chunk notifications");
    if (received_text.size() == 2) {
        EXPECT(received_text[0] == "hello " && received_text[1] == "world",
               "chunk text order");
    }
    EXPECT(tool_titles >= 2, "tool_call + tool_call_update delivered");
    EXPECT(turn_ended, "turn_end delivered");

    client.shutdown();
    INFO("test_streaming_messages: done");
}

void test_permission_flow_allow() {
    INFO("test_permission_flow_allow: start");
    // Regression test for the ACP/Kiro permission prompt input bug.
    // AcpClient must dispatch session/request_permission through
    // on_permission_request, allow the host to choose an option, send
    // that choice back as a JSON-RPC response to the original request, and then
    // complete the prompt without deadlocking.
    //
    // The worker also exercises the independent permission response write path.
    arn::acp::AcpClient client;
    client.start(fake_spec(), arn::acp::AcpInitInfo{});
    client.initialize({arn::acp::kLatestProtocolVersion, 1});
    const auto session = client.new_session(fs::current_path());

    std::atomic<bool> gate_called{false};
    std::string selected;
    std::string reported_description;
    std::vector<std::string> reported_options;
    std::string reply_text;
    std::thread permission_worker;

    arn::acp::AcpStreamCallbacks cb;
    cb.on_permission_request = [&](const arn::acp::AcpPermissionRequest& request) {
        gate_called.store(true);
        reported_description = request.description;
        for (const auto& opt : request.options) reported_options.push_back(opt.option_id);
        std::string choice = "reject";
        for (const auto& opt : request.options) {
            if (opt.option_id == "allow") { choice = "allow"; break; }
        }
        selected = choice;
        permission_worker = std::thread([&client, sid = session.session_id, request, choice]() {
            const auto reply = client.request_permission(sid, request, choice);
            (void)reply;
        });
    };
    cb.on_agent_text = [&](std::string_view text) { reply_text.append(text); };

    const auto response = client.prompt(session,
        "__FAKE_ACP_CMD__ permission-then-finish", cb);
    if (permission_worker.joinable()) permission_worker.join();
    EXPECT(response.ok, "permission-then-finish prompt ok");
    EXPECT(gate_called.load(), "permission gate was invoked");
    EXPECT(selected == "allow", "host selected the allow option_id");
    EXPECT(reported_description == "delete important file",
           "permission request description was forwarded");
    EXPECT(reported_options.size() == 2,
           "permission request options were forwarded");
    EXPECT(reply_text.find("choice=allow") != std::string::npos,
           "fake backend echoed the allow option_id back to the host");

    client.shutdown();
    INFO("test_permission_flow_allow: done");
}

void test_permission_flow_deny() {
    INFO("test_permission_flow_deny: start");
    // Same as test_permission_flow_allow but exercises the deny path:
    // the host must be able to choose a non-allow option_id, and the
    // reply must reach the backend without deadlocking the reader thread.
    arn::acp::AcpClient client;
    client.start(fake_spec(), arn::acp::AcpInitInfo{});
    client.initialize({arn::acp::kLatestProtocolVersion, 1});
    const auto session = client.new_session(fs::current_path());

    std::atomic<bool> gate_called{false};
    std::string selected;
    std::string reply_text;
    std::thread permission_worker;

    arn::acp::AcpStreamCallbacks cb;
    cb.on_permission_request = [&](const arn::acp::AcpPermissionRequest& request) {
        gate_called.store(true);
        std::string choice = "allow";
        for (const auto& opt : request.options) {
            if (opt.option_id == "reject") { choice = "reject"; break; }
        }
        selected = choice;
        permission_worker = std::thread([&client, sid = session.session_id, request, choice]() {
            const auto reply = client.request_permission(sid, request, choice);
            (void)reply;
        });
    };
    cb.on_agent_text = [&](std::string_view text) { reply_text.append(text); };

    const auto response = client.prompt(session,
        "__FAKE_ACP_CMD__ permission-then-finish", cb);
    if (permission_worker.joinable()) permission_worker.join();
    EXPECT(response.ok, "permission-then-finish deny prompt ok");
    EXPECT(gate_called.load(), "permission gate was invoked on deny");
    EXPECT(selected == "reject", "host selected the reject option_id");
    EXPECT(reply_text.find("choice=reject") != std::string::npos,
           "fake backend echoed the reject option_id back to the host");

    client.shutdown();
    INFO("test_permission_flow_deny: done");
}

void test_missing_kiro_cli(const std::string& fake_kiro) {
    INFO("test_missing_kiro_cli: start");
    const auto old_path = std::getenv("PATH") ? std::string(std::getenv("PATH")) : std::string{};
    const auto old_override = std::getenv("ARN_KIRO_BIN")
        ? std::optional<std::string>(std::getenv("ARN_KIRO_BIN")) : std::nullopt;
    const auto old_unauthorized = std::getenv("ARN_ACP_FAKE_UNAUTHORIZED")
        ? std::optional<std::string>(std::getenv("ARN_ACP_FAKE_UNAUTHORIZED")) : std::nullopt;
    const auto set = [](const char* name, const char* value) {
#ifdef _WIN32
        _putenv_s(name, value ? value : "");
#else
        if (value) setenv(name, value, 1); else unsetenv(name);
#endif
    };
    set("PATH", "");
    set("ARN_KIRO_BIN", nullptr);
    set("ARN_ACP_FAKE_UNAUTHORIZED", nullptr);
    const auto availability = arn::acp::probe_kiro_cli();
    set("ARN_KIRO_BIN", fake_kiro.c_str());
    const auto ready = arn::acp::probe_kiro_cli();
    EXPECT(ready.status == arn::acp::KiroStatus::ready, "Fake metadata CLI can be probed without PATH");
    const std::vector<std::string> expected_models{"fake-model-a", "fake-model-b"};
    EXPECT(arn::acp::KiroAdapter::list_models(fake_kiro) == expected_models,
           "Model discovery uses the same cross-platform pipe/spawn path");
    set("ARN_ACP_FAKE_UNAUTHORIZED", "1");
    const auto unauthorized = arn::acp::probe_kiro_cli();
    EXPECT(unauthorized.status == arn::acp::KiroStatus::not_authenticated, "Fake unauthorized CLI is not ready");
    set("ARN_ACP_FAKE_UNAUTHORIZED", old_unauthorized ? old_unauthorized->c_str() : nullptr);
    set("PATH", old_path.c_str());
    set("ARN_KIRO_BIN", old_override ? old_override->c_str() : nullptr);
    EXPECT(availability.status == arn::acp::KiroStatus::not_installed,
           "Controlled empty PATH must not probe a real Kiro installation");
    EXPECT(!availability.detail.empty(), "Missing backend has a readable error");
}

void test_backend_exit_and_restart() {
    INFO("test_backend_exit_and_restart: start");
    arn::acp::AcpClient client;
    for (int round = 0; round < 3; ++round) {
        client.start(fake_spec(), {});
        client.initialize();
        const auto session = client.new_session(fs::current_path());
        const auto start = std::chrono::steady_clock::now();
        const auto result = client.prompt(session, "__FAKE_ACP_CMD__ crash", {});
        EXPECT(!result.ok, "Backend EOF fails the pending prompt");
        EXPECT(std::chrono::steady_clock::now() - start < std::chrono::seconds(3),
               "EOF wakes pending requests without waiting for RPC timeout");
        client.shutdown();
        client.shutdown();
    }
    client.start(fake_spec(), {});
    client.initialize();
    client.shutdown();
}

void test_cancel() {
    arn::acp::AcpClient client;
    client.start(fake_spec(), {});
    client.initialize();
    const auto session = client.new_session(fs::current_path());
    std::promise<void> started;
    auto ready = started.get_future();
    arn::acp::AcpStreamCallbacks callbacks;
    callbacks.on_agent_text = [&](std::string_view) { started.set_value(); };
    auto turn = std::async(std::launch::async, [&] {
        return client.prompt(session, "__FAKE_ACP_CMD__ wait-for-cancel", callbacks);
    });
    EXPECT(ready.wait_for(std::chrono::seconds(3)) == std::future_status::ready, "Fake turn started");
    client.cancel(session.session_id);
    EXPECT(turn.wait_for(std::chrono::seconds(3)) == std::future_status::ready, "Cancellation completes pending RPC");
    const auto response = turn.get();
    EXPECT(response.ok && response.result.value("stopReason", "") == "cancelled", "Cancelled stop reason preserved");
    client.shutdown();
}


void test_executable_on_path() {
    INFO("test_executable_on_path: start");
    EXPECT(arn::acp::executable_on_path(ARN_TEST_NODE), "node on PATH on this host");
    EXPECT(!arn::acp::executable_on_path("__definitely_not_a_real_binary_arn__"),
           "non-existent binary not on PATH");
    INFO("test_executable_on_path: done");
}

} // namespace

int main(int argc, char** argv) {
    // Native metadata fixture. No installed Kiro binary or account is involved.
    if (argc > 1 && std::string_view(argv[1]) == "whoami") {
        if (std::getenv("ARN_ACP_FAKE_UNAUTHORIZED")) {
            std::cout << "Not logged in\n";
            return 1;
        }
        std::cout << "fake-test-account\n";
        return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "chat") {
        std::cout << R"({"models":["fake-model-a","fake-model-b"]})" << '\n';
        return 0;
    }
#ifndef ARN_REPO_ROOT
#error "ARN_REPO_ROOT must be defined for the test build"
#endif
    test_executable_on_path();
    test_missing_kiro_cli(fs::absolute(argv[0]).string());
    test_initialize_and_session_new();
    test_streaming_messages();
    test_permission_flow_allow();
    test_permission_flow_deny();
    test_backend_exit_and_restart();
    test_cancel();
    std::fprintf(stderr, "acp_client_test: %d passed, %d failed\n",
                 g_passed, g_failed);
    std::fflush(stderr);
    std::fprintf(stdout, "acp_client_test: %d passed, %d failed\n",
                 g_passed, g_failed);
    std::fflush(stdout);
    return g_failed == 0 ? 0 : 1;
}
