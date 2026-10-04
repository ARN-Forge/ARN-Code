// Offline AcpSession tests against real subprocess pipes and JSON-RPC frames.
// The fake server independently rejects non-advertised optionIds/wrong outcomes.
#include "acp/acp_session.hpp"

#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
using namespace arn::acp;

void check(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }

SubprocessSpec fake_spec() {
    return {ARN_TEST_NODE,
        {(std::filesystem::path(ARN_REPO_ROOT) / "tests/acp_fake_server.mjs").string()}, {}};
}

void activate(AcpSession& session) {
    check(session.activate(fake_spec()).empty(), "Fake ACP transport activation");
}

void decisions() {
    AcpSession session;
    activate(session);
    for (const bool allow : {true, false}) {
        std::string text, selected;
        int calls = 0;
        const auto result = session.chat("__FAKE_ACP_CMD__ permission-opaque",
            [&](std::string_view part) { text += part; }, {},
            [&](const AcpPermissionRequest& request, const std::atomic_bool& stopped) {
                check(!stopped.load(), "Permission gate was prematurely cancelled");
                check(!request.request_id.is_null(), "Original JSON-RPC request ID missing");
                check(request.tool_call_id == "same-looking-tool", "Tool metadata missing");
                check(request.options.size() == 4, "Full option set lost");
                ++calls;
                selected = confirmation_option_id(request, allow);
                check(!selected.empty(), "No once option selected");
                for (const auto& option : request.options)
                    check(selected != option.name && selected != option.kind,
                          "Display name/kind used as opaque optionId");
                return selected;
            }, nullptr);
        check(result.ok && !result.cancelled && calls == 1, "Single allow/reject permission");
        check(text.find("choice=" + selected) != std::string::npos, "Exact optionId round-trip");
    }
    std::string streamed;
    int tools = 0;
    const auto stream = session.chat("__FAKE_ACP_CMD__ stream-and-finish",
        [&](std::string_view part) { streamed += part; },
        [&](const std::string&) { ++tools; }, {}, nullptr);
    check(stream.ok && streamed == "hello world" && tools >= 2,
          "Session lost queued streaming/tool events when prompt completed");
    // A full callback can deliberately select any advertised option. y/N cannot.
    std::string text;
    const auto always = session.chat("__FAKE_ACP_CMD__ permission-opaque",
        [&](std::string_view part) { text += part; }, {},
        [](const AcpPermissionRequest& request, const std::atomic_bool&) {
            return request.options.front().option_id;
        }, nullptr);
    check(always.ok && text.find("choice=always/") != std::string::npos, "Full options abstraction");
    AcpPermissionRequest unsupported;
    unsupported.options = {{"allow_always", "Allow", "always-id"}};
    check(confirmation_option_id(unsupported, true).empty(), "y/N silently grants always");
    check(confirmation_option_id(unsupported, false).empty(), "Reject silently chooses allow");
}

void repeated_requests() {
    AcpSession session;
    activate(session);
    for (const auto command : {"permission-two", "permission-overlap"}) {
        std::vector<std::string> ids, choices;
        std::string text;
        const auto result = session.chat(std::string("__FAKE_ACP_CMD__ ") + command,
            [&](std::string_view part) { text += part; }, {},
            [&](const AcpPermissionRequest& request, const std::atomic_bool&) {
                ids.push_back(request.request_id.dump());
                choices.push_back(confirmation_option_id(request, choices.empty()));
                return choices.back();
            }, nullptr);
        check(result.ok && choices.size() == 2, "Two independent permissions in one turn");
        check(ids[0] != ids[1] && choices[0] != choices[1], "Request/option IDs conflated");
        check(text.find("choice=" + choices[0] + "," + choices[1]) != std::string::npos,
              "Repeated permission responses were not independently correlated");
    }
    int calls = 0;
    const auto next = session.chat("__FAKE_ACP_CMD__ permission-subject", {}, {},
        [&](const AcpPermissionRequest& request, const std::atomic_bool&) {
            ++calls;
            check(request.tool_call_id == "same-looking-tool", "v2 permission subject parsing");
            return confirmation_option_id(request, false);
        }, nullptr);
    check(next.ok && calls == 1, "Permission state leaked into a subsequent turn");
    session.deactivate();
    activate(session);
    std::string text;
    const auto fresh = session.chat("__FAKE_ACP_CMD__ permission-opaque",
        [&](std::string_view part) { text += part; }, {},
        [](const AcpPermissionRequest& request, const std::atomic_bool&) {
            return confirmation_option_id(request, true);
        }, nullptr);
    check(fresh.ok && text.find("session=sess_1;") != std::string::npos,
          "Previous process sessionId reused after activation");
}

void fail_closed() {
    AcpSession session;
    activate(session);
    for (int mode = 0; mode < 3; ++mode) {
        AcpPermissionGate gate;
        if (mode == 1) gate = [](const auto&, const auto&) -> std::string {
            throw std::runtime_error("Fake failed confirmation");
        };
        if (mode == 2) gate = [](const auto&, const auto&) { return "Reject-looking display name"; };
        std::string text;
        const auto result = session.chat("__FAKE_ACP_CMD__ permission-opaque",
            [&](std::string_view part) { text += part; }, {}, gate, nullptr);
        check(result.cancelled && text.find("choice=cancelled") != std::string::npos,
              "Missing/throwing/invalid callback did not fail closed");
    }
    int calls = 0;
    const auto foreign = session.chat("__FAKE_ACP_CMD__ permission-wrong-session", {}, {},
        [&](const auto&, const auto&) { ++calls; return "allow"; }, nullptr);
    check(foreign.cancelled && calls == 0, "Foreign-session permission reached confirmation");
    const auto cleanup = session.chat("__FAKE_ACP_CMD__ permission-opaque", {}, {},
        [&](const auto& request, const auto&) {
            session.deactivate();
            return confirmation_option_id(request, true);
        }, nullptr);
    check(cleanup.cancelled && !session.active(), "Gate cleanup waited for its own turn");
}

void pending_cleanup(const std::string& command, bool shutdown) {
    AcpSession session;
    activate(session);
    std::atomic_bool cancel{false};
    std::promise<void> entered;
    auto ready = entered.get_future();
    std::string text;
    auto chat = std::async(std::launch::async, [&] {
        return session.chat("__FAKE_ACP_CMD__ " + command,
            [&](std::string_view part) { text += part; }, {},
            [&](const AcpPermissionRequest& request, const std::atomic_bool& stopped) {
                entered.set_value();
                const auto deadline = std::chrono::steady_clock::now() + 4s;
                while (!stopped.load() && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(1ms);
                check(stopped.load(), "Pending gate not woken by cancel/EOF/shutdown");
                // A stale 'allow' result after cancellation must never be sent.
                return confirmation_option_id(request, true);
            }, &cancel);
    });
    // This promise is the ordering barrier, not a sleep racing an 80 ms server
    // exit. The deadline only bounds a broken test; EOF/exit happens afterward.
    check(ready.wait_for(3s) == std::future_status::ready, "Pending permission gate not entered");
    if (command == "permission-pending") {
        if (shutdown) session.deactivate();
        else cancel.store(true);
    } else {
        std::string error;
        check(session.set_model(command == "permission-exit" ? "__FAKE_ACP_EXIT__"
                                                             : "__FAKE_ACP_EOF__", error),
              "Fake transport control RPC failed");
    }
    check(chat.wait_for(5s) == std::future_status::ready, "Cleanup left chat unresolved");
    const auto result = chat.get();
    if (command == "permission-pending") {
        if (!result.cancelled) throw std::runtime_error("Cancellation/cleanup result lost: " + command + (shutdown ? " shutdown" : " cancel"));
        if (!shutdown) check(text.find("choice=cancelled") != std::string::npos,
                             "Server did not receive a cancelled permission outcome");
    } else check(!result.ok, "Transport EOF/exit falsely succeeded");
    if (command == "permission-pending" && !shutdown) {
        cancel.store(false);
        int calls = 0;
        const auto same_session = session.chat("__FAKE_ACP_CMD__ permission-opaque", {}, {},
            [&](const auto& request, const auto&) {
                ++calls;
                return confirmation_option_id(request, false);
            }, &cancel);
        check(same_session.ok && !same_session.cancelled && calls == 1,
              "Cancelled permission poisoned the next turn in the same session");
    }
    session.deactivate();
    activate(session);
    int calls = 0;
    const auto next = session.chat("__FAKE_ACP_CMD__ permission-opaque", {}, {},
        [&](const auto& request, const auto&) { ++calls; return confirmation_option_id(request, false); }, nullptr);
    check(next.ok && calls == 1, "Cleanup poisoned the next session");
}

int main() {
    try {
        decisions();
        repeated_requests();
        fail_closed();
        pending_cleanup("permission-pending", false);
        pending_cleanup("permission-pending", true);
        pending_cleanup("permission-exit", false);
        pending_cleanup("permission-eof", false);
        std::cout << "ACP session permission tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
