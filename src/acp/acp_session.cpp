#include "acp/acp_session.hpp"

#include "acp/acp_client.hpp"
#include "acp/kiro_adapter.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace arn::acp {

AcpSession::AcpSession() = default;
AcpSession::~AcpSession() {
    deactivate();
}

namespace {

SubprocessSpec make_spec_for(const AcpBackendInfo& backend) {
    if (backend.kind == AcpBackendKind::kiro) {
        return KiroAdapter::make_spec(backend.binary_path, backend.agent_name, std::nullopt);
    }
    return {};
}

} // namespace

std::string confirmation_option_id(const AcpPermissionRequest& request, bool allow) {
    const std::string_view kind = allow ? "allow_once" : "reject_once";
    for (const auto& option : request.options)
        if (option.kind == kind && !option.option_id.empty()) return option.option_id;
    return {}; // Unknown/always-only options cannot be represented by y/N.
}

std::string AcpSession::activate(const AcpBackendInfo& backend) {
    if (backend.kind == AcpBackendKind::none) { deactivate(); return {}; }
    const auto error = activate_transport(backend, make_spec_for(backend));
    if (error.empty() && backend.kind == AcpBackendKind::kiro) {
        std::lock_guard lock(state_mutex_);
        available_models_ = KiroAdapter::list_models(backend.binary_path);
    }
    return error;
}

std::string AcpSession::activate(const SubprocessSpec& spec) {
    AcpBackendInfo backend;
    backend.kind = AcpBackendKind::subprocess;
    backend.label = "ACP subprocess";
    backend.binary_path = spec.command;
    return activate_transport(backend, spec);
}

std::string AcpSession::activate_transport(const AcpBackendInfo& backend,
                                           const SubprocessSpec& spec) {
    deactivate();
    auto client = std::make_shared<AcpClient>();
    try {
        client->start(spec, AcpInitInfo{});
        client->initialize({kLatestProtocolVersion, 1});
    } catch (const std::exception& e) {
        client->shutdown();
        return std::string("Failed to initialize ACP backend: ") + e.what();
    }
    std::lock_guard lock(state_mutex_);
    backend_ = backend;
    client_ = std::move(client);
    session_state_ = {};
    available_models_.clear();
    current_model_.reset();
    return {};
}

void AcpSession::deactivate() {
    std::shared_ptr<AcpClient> client;
    bool own_turn = false;
    {
        std::lock_guard lock(state_mutex_);
        if (active_stop_) active_stop_->store(true);
        own_turn = turn_thread_ == std::this_thread::get_id();
        client = std::move(client_);
        backend_ = {};
        session_state_ = {};
        available_models_.clear();
        current_model_.reset();
    }
    if (client) client->shutdown();
    if (own_turn) return; // A gate may deactivate; never wait for itself.
    std::unique_lock lock(state_mutex_);
    turn_finished_.wait(lock, [&] { return !turn_in_flight_.load(); });
}

bool AcpSession::set_model(std::string_view model_id, std::string& error) {
    std::lock_guard lock(state_mutex_);
    if (!client_) {
        error = "No ACP backend is active.";
        return false;
    }
    try {
        const auto response = client_->set_model(session_state_, model_id);
        if (response.ok) {
            current_model_ = std::string(model_id);
            return true;
        }
        error = response.error.is_object() && response.error.contains("message")
            ? response.error["message"].dump() : std::string("ACP set_model failed.");
        return false;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

AcpChatResult AcpSession::chat(std::string_view prompt,
                               const AcpTextSink& on_text,
                               const AcpToolSink& on_tool,
                               AcpPermissionGate permission_gate,
                               const std::atomic_bool* cancelled) {
    AcpChatResult result;
    std::shared_ptr<AcpClient> client;
    AcpSessionState state;
    const auto stopped = std::make_shared<std::atomic_bool>(false);
    const auto abort_requested = std::make_shared<std::atomic_bool>(false);
    {
        std::lock_guard lock(state_mutex_);
        if (!client_ || turn_in_flight_.exchange(true)) {
            result.message = client_ ? "An ACP turn is already active." : "No ACP backend is active.";
            return result;
        }
        client = client_;
        state = session_state_;
        active_stop_ = abort_requested;
        turn_thread_ = std::this_thread::get_id();
    }
    // Cleanup always runs after callbacks/monitor have been joined.
    const auto finish = [this, abort_requested](void*) {
        std::lock_guard lock(state_mutex_);
        if (active_stop_ == abort_requested) active_stop_.reset();
        turn_in_flight_.store(false);
        turn_thread_ = {};
        turn_finished_.notify_all();
    };
    std::unique_ptr<void, decltype(finish)> finish_turn(this, finish);
    try {
        if (state.session_id.empty()) {
            state = client->new_session(std::filesystem::current_path());
            std::lock_guard lock(state_mutex_);
            if (client_ == client) session_state_ = state;
        }
    } catch (const std::exception& e) {
        result.message = std::string("Failed to create ACP session: ") + e.what();
        return result;
    }

    struct Permissions {
        std::mutex mutex;
        struct Event {
            std::optional<AcpPermissionRequest> request;
            std::function<void()> output;
        };
        std::deque<Event> queue;
    };
    const auto pending = std::make_shared<Permissions>();
    AcpStreamCallbacks callbacks;
    callbacks.on_agent_text = [pending, on_text](std::string_view fragment) {
        std::lock_guard lock(pending->mutex);
        pending->queue.push_back({{}, [on_text, text = std::string(fragment)] {
            if (on_text) on_text(text);
        }});
    };
    callbacks.on_agent_thought = callbacks.on_agent_text;
    callbacks.on_tool_call = [pending, on_tool](const AcpToolCall& call) {
        std::lock_guard lock(pending->mutex);
        pending->queue.push_back({{}, [on_tool, title = call.title] {
            if (on_tool && !title.empty()) on_tool(title);
        }});
    };
    callbacks.on_tool_call_update = callbacks.on_tool_call;
    callbacks.on_permission_request = [pending](const AcpPermissionRequest& request) {
        std::lock_guard lock(pending->mutex);
        pending->queue.push_back({request, {}});
    };
    auto future = std::async(std::launch::async,
        [client, state, prompt = std::string(prompt), callbacks] {
            return client->prompt(state, prompt, callbacks);
        }).share();
    std::atomic_bool was_cancelled{false};
    // A gate may be waiting for keyboard input. Transport completion and EOF
    // must wake it too, without making the reader thread own terminal input.
    std::jthread monitor([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            if ((cancelled && cancelled->load()) || abort_requested->load()) {
                was_cancelled.store(true);
                stopped->store(true);
                client->cancel(state.session_id);
                if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready)
                    client->shutdown();
                return;
            }
            if (!client->running() ||
                future.wait_for(std::chrono::milliseconds(10)) == std::future_status::ready) {
                stopped->store(true);
                return;
            }
        }
    });
    // Serialize rendering and confirmation on the caller's thread. The reader
    // can continue consuming EOF, cancellation and other permission requests.
    for (;;) {
        std::optional<Permissions::Event> event;
        const bool finished = future.wait_for(std::chrono::milliseconds(0)) ==
                              std::future_status::ready;
        {
            std::lock_guard lock(pending->mutex);
            if (!pending->queue.empty()) {
                event = std::move(pending->queue.front());
                pending->queue.pop_front();
            }
        }
        if (!event) {
            if (finished) break;
            future.wait_for(std::chrono::milliseconds(10));
            continue;
        }
        if (event->output) {
            try { event->output(); }
            catch (...) { abort_requested->store(true); stopped->store(true); }
            continue;
        }
        const auto& request = *event->request;
        std::string selection;
        try {
            if (!finished && !stopped->load() && !(cancelled && cancelled->load()) && permission_gate)
                selection = permission_gate(request, *stopped);
        } catch (...) {} // A failed gate cancels, never chooses the first option.
        if (stopped->load() || (cancelled && cancelled->load())) selection.clear();
        client->request_permission(state.session_id, request, selection);
    }
    const auto response = future.get();
    monitor.request_stop();
    monitor.join();
    stopped->store(true);
    // prompt() has cancelled any remaining request IDs before returning.
    if (abort_requested->load() || was_cancelled.load() || (cancelled && cancelled->load())) {
        result.cancelled = true;
        result.message = "Request cancelled.";
        return result;
    }
    if (response.ok) {
        result.ok = true;
        if (response.result.is_object() && response.result.contains("stopReason")) {
            const nlohmann::json reason = response.result["stopReason"];
            if (reason.is_string()) {
                const auto reason_str = reason.get<std::string>();
                if (reason_str == "cancelled") {
                    result.cancelled = true;
                    result.message = "Request cancelled.";
                    return result;
                }
                if (reason_str == "max_tokens" || reason_str == "max_output_tokens") {
                    result.message = "ACP backend hit its token limit.";
                    return result;
                }
            }
        }
        return result;
    }
    result.message = response.error.is_object() && response.error.contains("message")
        ? response.error["message"].dump()
        : std::string("ACP backend returned an error.");
    return result;
}

} // namespace arn::acp
