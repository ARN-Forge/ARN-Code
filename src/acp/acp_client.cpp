#include "acp/acp_client.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <future>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace arn::acp {

namespace {

std::string trim(std::string_view input) {
    std::size_t begin = 0;
    std::size_t end = input.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(input[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1]))) --end;
    return std::string(input.substr(begin, end - begin));
}

bool string_to_int64(const nlohmann::json& value, std::int64_t& out) {
    if (value.is_number_integer()) { out = value.get<std::int64_t>(); return true; }
    if (value.is_string()) {
        try { out = std::stoll(value.get<std::string>()); return true; }
        catch (...) { return false; }
    }
    return false;
}

AcpAgentCapabilities parse_capabilities(const nlohmann::json& caps) {
    AcpAgentCapabilities out;
    if (!caps.is_object()) return out;
    if (auto session = caps.find("session"); session != caps.end() && session->is_object()) {
        out.session_methods = session->is_object();
        if (auto load = session->find("loadSession"); load != session->end() && load->is_object())
            out.load_session = true;
        if (auto prompt = session->find("prompt"); prompt != session->end() && prompt->is_object()) {
            if (prompt->find("image") != prompt->end()) out.prompt_image = true;
            if (prompt->find("audio") != prompt->end()) out.prompt_audio = true;
            if (prompt->find("embeddedContext") != prompt->end())
                out.prompt_embedded_context = true;
        }
        if (auto mcp = session->find("mcp"); mcp != session->end() && mcp->is_object()) {
            if (mcp->find("http") != mcp->end()) out.mcp_http = true;
            if (mcp->find("stdio") != mcp->end()) out.mcp_stdio = true;
        }
    }
    if (auto auths = caps.find("authMethods"); auths != caps.end() && auths->is_array()) {
        for (const auto& entry : *auths) {
            if (entry.is_object()) {
                if (auto id = entry.find("id"); id != entry.end() && id->is_string())
                    out.auth_methods.push_back(id->get<std::string>());
            } else if (entry.is_string()) {
                out.auth_methods.push_back(entry.get<std::string>());
            }
        }
    }
    return out;
}

} // namespace

AcpClient::AcpClient() = default;

AcpClient::~AcpClient() {
    shutdown();
}

void AcpClient::start(const SubprocessSpec& spec, const AcpInitInfo& /*client_info*/) {
    if (running_.load()) return;
    shutdown(); // Also joins a reader that stopped because of EOF.
    cancel_requested_.store(false);
    line_buffer_.clear();
    struct OwnedPipes {
#if defined(_WIN32)
        std::array<HANDLE, 6> ends{};
        ~OwnedPipes() { for (auto fd : ends) if (fd) CloseHandle(fd); }
#else
        std::array<int, 6> ends{-1, -1, -1, -1, -1, -1};
        ~OwnedPipes() { for (auto fd : ends) if (fd >= 0) ::close(fd); }
#endif
    } pipes;
    for (int i = 0; i < 6; i += 2) {
#if defined(_WIN32)
        SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        if (!CreatePipe(&pipes.ends[i], &pipes.ends[i + 1], &sa, 0))
            throw std::runtime_error("CreatePipe failed for ACP subprocess.");
        const auto parent = pipes.ends[i == 0 ? i + 1 : i];
        if (!SetHandleInformation(parent, HANDLE_FLAG_INHERIT, 0))
            throw std::runtime_error("SetHandleInformation failed for ACP subprocess.");
#else
        if (::pipe(pipes.ends.data() + i) != 0)
            throw std::runtime_error("pipe() failed for ACP subprocess.");
        for (int index : {i, i + 1}) {
            if (::fcntl(pipes.ends[index], F_SETFD, FD_CLOEXEC) < 0)
                throw std::runtime_error("Could not protect ACP pipe inheritance.");
        }
#endif
    }
    subprocess_.spawn(spec, {pipes.ends[0], pipes.ends[3], pipes.ends[5]});
#if defined(_WIN32)
    pipes.ends[0] = pipes.ends[3] = pipes.ends[5] = nullptr;
    subprocess_.configure_parent_handles(pipes.ends[1], pipes.ends[2], pipes.ends[4]);
    pipes.ends[1] = pipes.ends[2] = pipes.ends[4] = nullptr;
#else
    pipes.ends[0] = pipes.ends[3] = pipes.ends[5] = -1;
    subprocess_.configure_parent_handles(pipes.ends[1], pipes.ends[2], pipes.ends[4]);
    pipes.ends[1] = pipes.ends[2] = pipes.ends[4] = -1;
#endif
    running_.store(true);
    try { reader_thread_ = std::thread([this] { reader_loop(); }); }
    catch (...) { shutdown(); throw; }
}

std::uint32_t AcpClient::initialize(const std::vector<std::uint32_t>& supported_versions) {
    if (!running_.load()) throw std::runtime_error("ACP backend not running.");

    nlohmann::json params;
    params["protocolVersion"] = supported_versions.front();
    params["capabilities"] = nlohmann::json::object();
    params["clientInfo"] = {
        {"name", "arn-code"},
        {"title", "ARN Code"},
        {"version",
#ifdef ARN_VERSION
            std::string(ARN_VERSION)
#else
            std::string("dev")
#endif
        },
    };

    const auto response = send_request("initialize", params, std::chrono::seconds(15));
    if (!response.ok) {
        const auto message = response.error.is_object() && response.error.contains("message")
            ? response.error["message"].dump() : std::string("(no error message)");
        throw std::runtime_error("ACP initialize failed: " + message);
    }
    const auto& result = response.result;
    if (result.contains("protocolVersion") && result["protocolVersion"].is_number_unsigned()) {
        negotiated_version_ = result["protocolVersion"].get<std::uint32_t>();
    } else {
        negotiated_version_ = supported_versions.front();
    }
    if (result.contains("capabilities")) {
        capabilities_ = parse_capabilities(result["capabilities"]);
    }
    if (result.contains("agentInfo") && result["agentInfo"].is_object()) {
        const auto& info = result["agentInfo"];
        if (info.contains("name") && info["name"].is_string())
            agent_name_ = info["name"].get<std::string>();
        if (info.contains("title") && info["title"].is_string())
            agent_title_ = info["title"].get<std::string>();
        if (info.contains("version") && info["version"].is_string())
            agent_version_ = info["version"].get<std::string>();
    } else {
        // Older agents emit "info".
        if (result.contains("info") && result["info"].is_object()) {
            const auto& info = result["info"];
            if (info.contains("name") && info["name"].is_string())
                agent_name_ = info["name"].get<std::string>();
            if (info.contains("title") && info["title"].is_string())
                agent_title_ = info["title"].get<std::string>();
            if (info.contains("version") && info["version"].is_string())
                agent_version_ = info["version"].get<std::string>();
        }
    }
    if (result.contains("authMethods") && result["authMethods"].is_array()) {
        for (const auto& m : result["authMethods"]) {
            if (m.is_object() && m.contains("id") && m["id"].is_string())
                capabilities_.auth_methods.push_back(m["id"].get<std::string>());
            else if (m.is_string())
                capabilities_.auth_methods.push_back(m.get<std::string>());
        }
    }
    return negotiated_version_;
}

void AcpClient::shutdown() {
    std::lock_guard shutdown_lock(shutdown_mutex_);
    cancel_requested_.store(true);
    cancel_permissions();
    try {
        nlohmann::json params;
        {
            std::lock_guard lock(active_session_mutex_);
            if (!active_session_.empty()) params["sessionId"] = active_session_;
        }
        if (!params.is_null()) {
            // Best-effort notification; ignore failures.
            try { send_notification("session/cancel", params); } catch (...) {}
        }
    } catch (...) {}
    running_.store(false);
    subprocess_.terminate();
    fail_pending("ACP backend stopped.");
    if (reader_thread_.joinable()) reader_thread_.join();
    std::lock_guard write_lock(write_mutex_);
    subprocess_.close_pipes();
    subprocess_ = Subprocess{};
}

void AcpClient::fail_pending(const char* reason) {
    std::vector<std::shared_ptr<PendingRequest>> requests;
    {
        std::lock_guard lock(pending_mutex_);
        for (const auto& [id, request] : pending_) requests.push_back(request);
    }
    for (const auto& request : requests) {
        {
            std::lock_guard lock(request->mutex);
            if (request->done) continue;
            request->response = AcpResponse{false, {}, {{"message", reason}}};
            request->done = true;
        }
        request->cv.notify_all();
    }
}

AcpSessionState AcpClient::new_session(const std::filesystem::path& working_directory) {
    if (!running_.load()) throw std::runtime_error("ACP backend not running.");
    nlohmann::json params;
    params["cwd"] = working_directory.string();
    params["mcpServers"] = nlohmann::json::array();

    const auto response = send_request("session/new", params, std::chrono::seconds(30));
    if (!response.ok) {
        const auto message = response.error.is_object() && response.error.contains("message")
            ? response.error["message"].dump() : std::string("(no error message)");
        throw std::runtime_error("session/new failed: " + message);
    }
    if (!response.result.contains("sessionId") || !response.result["sessionId"].is_string()) {
        throw std::runtime_error("session/new: missing sessionId in response.");
    }
    AcpSessionState session;
    session.session_id = response.result["sessionId"].get<std::string>();
    session.working_directory = working_directory.string();
    if (response.result.contains("models") && response.result["models"].is_array()) {
        for (const auto& m : response.result["models"]) {
            if (m.is_object()) {
                if (m.contains("modelId") && m["modelId"].is_string())
                    session.available_models.push_back(m["modelId"].get<std::string>());
                else if (m.contains("id") && m["id"].is_string())
                    session.available_models.push_back(m["id"].get<std::string>());
            } else if (m.is_string()) {
                session.available_models.push_back(m.get<std::string>());
            }
        }
    }
    if (response.result.contains("currentModelId") && response.result["currentModelId"].is_string()) {
        session.current_model = response.result["currentModelId"].get<std::string>();
    }
    return session;
}

bool AcpClient::write_permission_result(const nlohmann::json& id,
                                         const nlohmann::json& outcome) {
    const auto body = nlohmann::json{{"jsonrpc", "2.0"}, {"id", id},
                                    {"result", {{"outcome", outcome}}}}.dump() + "\n";
    std::lock_guard lock(write_mutex_);
    return running_.load() && subprocess_.write_stdin(body.data(), body.size());
}

AcpResponse AcpClient::request_permission(const std::string& session_id,
                                          const AcpPermissionRequest& request,
                                          const std::string& selected_option_id) {
    std::lock_guard lock(permission_mutex_);
    const auto found = permissions_.find(request.request_id.dump());
    if (found == permissions_.end() || found->second.session_id != session_id)
        return {false, {}, {{"message", "Permission request is no longer pending."}}};
    const auto& original = found->second;
    const bool selected = !cancel_requested_.load() && !selected_option_id.empty() &&
        std::ranges::any_of(original.options, [&](const auto& option) {
            return option.option_id == selected_option_id;
        });
    nlohmann::json outcome = {{"outcome", "cancelled"}};
    if (selected) outcome = {{"outcome", "selected"}, {"optionId", selected_option_id}};
    const bool written = write_permission_result(original.request_id, outcome);
    permissions_.erase(found); // A decision is final, even if the transport has closed.
    return {written, {{"outcome", outcome}}, written ? nlohmann::json{}
        : nlohmann::json{{"message", "ACP permission transport closed."}}};
}

void AcpClient::cancel_permissions(const std::string& session_id) {
    std::lock_guard lock(permission_mutex_);
    for (auto it = permissions_.begin(); it != permissions_.end();) {
        if (session_id.empty() || it->second.session_id == session_id) {
            write_permission_result(it->second.request_id, {{"outcome", "cancelled"}});
            it = permissions_.erase(it);
        } else ++it;
    }
}

AcpResponse AcpClient::prompt(const AcpSessionState& session,
                              std::string_view text,
                              const AcpStreamCallbacks& callbacks) {
    if (!running_.load()) {
        AcpResponse r;
        r.error = {{"message", "ACP backend not running."}};
        return r;
    }
    {
        std::lock_guard lock(active_session_mutex_);
        cancel_requested_.store(false);
        active_session_ = session.session_id;
        active_callbacks_ = &callbacks;
    }
    if (callbacks.on_status_change) callbacks.on_status_change("sending prompt");

    nlohmann::json params;
    params["sessionId"] = session.session_id;
    params["prompt"] = nlohmann::json::array({
        {{"type", "text"}, {"text", std::string(text)}}
    });
    const auto response = send_request("session/prompt", params, std::chrono::minutes(10));
    {
        std::lock_guard lock(active_session_mutex_);
        if (active_session_ == session.session_id) active_session_.clear();
        active_callbacks_ = nullptr;
    }
    cancel_permissions(session.session_id);
    if (callbacks.on_status_change) {
        callbacks.on_status_change(response.ok ? "turn complete" : "turn failed");
    }
    return response;
}

void AcpClient::cancel(const std::string& session_id) {
    cancel_requested_.store(true);
    cancel_permissions(session_id);
    if (!running_.load()) return;
    nlohmann::json params;
    params["sessionId"] = session_id;
    try { send_notification("session/cancel", params); } catch (...) {}
}

AcpResponse AcpClient::set_model(const AcpSessionState& session, std::string_view model_id) {
    if (!capabilities_.session_methods) {
        AcpResponse r;
        r.error = {{"message", "Agent does not advertise session methods."}};
        return r;
    }
    nlohmann::json params;
    params["sessionId"] = session.session_id;
    params["modelId"] = std::string(model_id);
    return send_request("session/set_model", params, std::chrono::seconds(15));
}

AcpResponse AcpClient::set_mode(const AcpSessionState& session, std::string_view mode_id) {
    nlohmann::json params;
    params["sessionId"] = session.session_id;
    params["modeId"] = std::string(mode_id);
    return send_request("session/set_mode", params, std::chrono::seconds(15));
}

std::vector<std::string> AcpClient::list_models(const AcpSessionState& /*session*/) {
    // The ACP core protocol does not expose model discovery today. Kiro CLI
    // (and other ACP backends) typically publish their catalogue via a
    // separate CLI subcommand. The Kiro adapter overrides this.
    return {};
}

AcpResponse AcpClient::send_request(const std::string& method, const nlohmann::json& params,
                                     std::chrono::milliseconds timeout) {
    if (!running_.load()) {
        AcpResponse r;
        r.error = {{"message", "ACP backend not running."}};
        return r;
    }
    auto pending = std::make_shared<PendingRequest>();
    std::int64_t id = 0;
    {
        std::lock_guard lock(pending_mutex_);
        if (!running_.load()) return AcpResponse{false, {}, {{"message", "ACP backend stopped."}}};
        id = ++next_request_id_;
        pending_[id] = pending;
    }
    nlohmann::json message = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", method},
        {"params", params},
    };
    {
        std::lock_guard lock(write_mutex_);
        const auto body = message.dump() + "\n";
        if (!running_.load() || !subprocess_.write_stdin(body.data(), body.size())) {
            std::lock_guard plock(pending_mutex_);
            pending_.erase(id);
            AcpResponse r;
            r.error = {{"message", "Failed to write to ACP backend."}};
            return r;
        }
    }

    std::unique_lock<std::mutex> response_lock(pending->mutex);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pending->done) {
        if (pending->cv.wait_until(response_lock, deadline) == std::cv_status::timeout) {
            std::lock_guard plock(pending_mutex_);
            pending_.erase(id);
            AcpResponse r;
            r.error = {{"message", "ACP request timed out: " + method}};
            return r;
        }
    }
    auto response = pending->response;
    {
        std::lock_guard plock(pending_mutex_);
        pending_.erase(id);
    }
    return response.value_or(AcpResponse{});
}

void AcpClient::send_notification(const std::string& method, const nlohmann::json& params) {
    if (!running_.load()) return;
    nlohmann::json message = {
        {"jsonrpc", "2.0"},
        {"method", method},
        {"params", params},
    };
    std::lock_guard lock(write_mutex_);
    if (!running_.load()) return;
    const auto body = message.dump() + "\n";
    subprocess_.write_stdin(body.data(), body.size());
}

void AcpClient::reader_loop() {
    constexpr std::size_t buffer_size = 8192;
    std::array<char, buffer_size> buffer{};
    while (running_.load()) {
        const int bytes_read = subprocess_.read_stdout(buffer.data(), buffer.size());
        if (bytes_read == -2) continue;
        if (bytes_read <= 0) break;
        line_buffer_.append(buffer.data(), static_cast<std::size_t>(bytes_read));
        std::size_t newline = line_buffer_.find('\n');
        while (newline != std::string::npos) {
            std::string line = line_buffer_.substr(0, newline);
            line_buffer_.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            line = trim(line);
            if (!line.empty()) {
                try {
                    auto parsed = nlohmann::json::parse(line);
                    std::lock_guard lock(active_session_mutex_);
                    dispatch_message(parsed, active_callbacks_);
                } catch (const std::exception&) {
                    // Malformed frame: notify any waiters with a parse error.
                }
            }
            newline = line_buffer_.find('\n');
        }
    }
    // Process the residual buffer if the subprocess wrote without trailing newline.
    if (!line_buffer_.empty()) {
        const auto line = trim(line_buffer_);
        line_buffer_.clear();
        if (!line.empty()) {
            try {
                auto parsed = nlohmann::json::parse(line);
                std::lock_guard lock(active_session_mutex_);
                dispatch_message(parsed, active_callbacks_);
            } catch (...) {}
        }
    }
    running_.store(false);
    { std::lock_guard lock(permission_mutex_); permissions_.clear(); }
    fail_pending("ACP backend closed its output.");
}

void AcpClient::dispatch_message(const nlohmann::json& message,
                                 const AcpStreamCallbacks* callbacks) {
    if (!message.is_object()) return;
    if (message.contains("id") && !message.contains("method")) {
        std::int64_t id = 0;
        if (!string_to_int64(message["id"], id)) return;
        std::shared_ptr<PendingRequest> pending;
        {
            std::lock_guard lock(pending_mutex_);
            auto it = pending_.find(id);
            if (it == pending_.end()) return;
            pending = it->second;
        }
        AcpResponse response;
        if (message.contains("error")) {
            response.ok = false;
            response.error = message["error"];
        } else {
            response.ok = true;
            response.result = message.value("result", nlohmann::json::object());
        }
        {
            std::lock_guard lock(pending->mutex);
            pending->response = std::move(response);
            pending->done = true;
        }
        pending->cv.notify_all();
        return;
    }
    if (message.contains("method")) {
        const auto& method = message["method"].get_ref<const std::string&>();
        if (method == "session/update") {
            const auto& params = message.value("params", nlohmann::json::object());
            const auto& update = params.value("update", nlohmann::json::object());
            handle_session_update(update, callbacks);
            return;
        }
        if (method == "session/request_permission") {
            // Permission is a server request, never a notification.
            if (!message.contains("id")) return;
            const auto& params = message.value("params", nlohmann::json::object());
            const auto text = [](const nlohmann::json& object, const char* key) {
                const auto it = object.find(key);
                return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
            };
            AcpPermissionRequest request;
            request.request_id = message["id"];
            request.session_id = text(params, "sessionId");
            const auto subject = params.value("subject", nlohmann::json::object());
            const auto tool_call = params.value("toolCall",
                subject.value("toolCall", nlohmann::json::object()));
            request.tool_call.tool_call_id = text(tool_call, "toolCallId");
            request.tool_call.title = text(tool_call, "title");
            request.tool_call.kind = text(tool_call, "kind");
            request.tool_call.status = text(tool_call, "status");
            request.tool_call.raw = tool_call;
            request.tool_call_id = request.tool_call.tool_call_id;
            request.description = text(params, "description");
            if (request.description.empty()) request.description = text(params, "title");
            if (request.description.empty()) request.description = request.tool_call.title;
            if (params.contains("options") && params["options"].is_array()) {
                for (const auto& opt : params["options"]) {
                    if (!opt.is_object()) continue;
                    AcpPermissionOption option;
                    option.kind = text(opt, "kind");
                    option.name = text(opt, "name");
                    option.option_id = text(opt, "optionId");
                    if (option.option_id.empty()) {
                        // Some agents label the id field differently.
                        option.option_id = text(opt, "id");
                    }
                    request.options.push_back(std::move(option));
                }
            }
            {
                std::lock_guard lock(permission_mutex_);
                if (!permissions_.emplace(request.request_id.dump(), request).second) return;
            }
            if (!running_.load() || cancel_requested_.load() || !callbacks ||
                !callbacks->on_permission_request || request.session_id != active_session_) {
                request_permission(request.session_id, request, {});
            } else {
                try { callbacks->on_permission_request(request); }
                catch (...) { request_permission(request.session_id, request, {}); }
            }
            return;
        }
        // Other agent-initiated methods are not consumed by ARN yet.
    }
}

void AcpClient::handle_session_update(const nlohmann::json& update,
                                      const AcpStreamCallbacks* callbacks) {
    if (!callbacks || !update.is_object()) return;
    const auto variant = update.value("sessionUpdate", std::string{});
    if (variant == "agent_message_chunk" && callbacks->on_agent_text) {
        if (update.contains("content") && update["content"].is_object()
            && update["content"].value("type", std::string{}) == "text") {
            callbacks->on_agent_text(update["content"].value("text", std::string{}));
        }
    } else if (variant == "agent_thought_chunk" && callbacks->on_agent_thought) {
        if (update.contains("content") && update["content"].is_object()
            && update["content"].value("type", std::string{}) == "text") {
            callbacks->on_agent_thought(update["content"].value("text", std::string{}));
        }
    } else if (variant == "tool_call") {
        AcpToolCall call;
        const auto& inner = update.value("toolCall", update);
        call.tool_call_id = inner.value("toolCallId", std::string{});
        call.title = inner.value("title", std::string{});
        call.kind = inner.value("kind", std::string{});
        call.status = inner.value("status", std::string{"pending"});
        call.raw = inner;
        if (!call.tool_call_id.empty()) {
            active_tool_calls_[call.tool_call_id] = call;
        }
        if (callbacks->on_tool_call) callbacks->on_tool_call(call);
    } else if (variant == "tool_call_update") {
        const auto& inner = update.value("toolCallUpdate", update);
        const auto id = inner.value("toolCallId", std::string{});
        AcpToolCall call;
        if (!id.empty()) {
            auto it = active_tool_calls_.find(id);
            if (it != active_tool_calls_.end()) {
                call = it->second;
            }
        }
        call.tool_call_id = id;
        if (inner.contains("title") && inner["title"].is_string())
            call.title = inner["title"].get<std::string>();
        if (inner.contains("kind") && inner["kind"].is_string())
            call.kind = inner["kind"].get<std::string>();
        if (inner.contains("status") && inner["status"].is_string())
            call.status = inner["status"].get<std::string>();
        call.raw = inner;
        if (!id.empty()) {
            active_tool_calls_[id] = call;
        }
        if (callbacks->on_tool_call_update) callbacks->on_tool_call_update(call);
    } else if ((variant == "turn_end" || variant == "agent_turn_end")) {
        active_tool_calls_.clear();
        if (callbacks->on_turn_end) callbacks->on_turn_end();
    }
}

} // namespace arn::acp
