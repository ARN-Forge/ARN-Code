// Test-only provider implementation. Never linked into the ARN application.
#include "api_client.hpp"
#include "server.hpp"
#include <chrono>
#include <thread>
namespace arn {
ApiClient::~ApiClient() = default;
ApiResult ApiClient::list_models(Provider, const std::string&, const std::atomic_bool*) {
    return {true, "", {"fixture-model"}};
}
void ApiClient::reset_session() {}
void ApiClient::cancel_active_request() {}
ApiResult ApiClient::submit_prompt(Provider, const std::string&, const std::string&,
                                   const std::string& prompt, const ToolExecutor& tools,
                                   const ToolExecutor::ConfirmationFn& confirm,
                                   const StreamCallback& stream, const std::atomic_bool* cancel,
                                   const StreamCallback& progress) {
    if (progress)
        progress("Waiting for provider response (attempt 1)");
    stream("fixture-start");
    if (prompt == "wait") {
        while (!cancel->load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return {false, "cancelled", {}, true};
    }
    const auto cmd = nlohmann::json::parse(prompt);
    auto result = tools.execute(cmd.at("name"), cmd.at("arguments"), confirm);
    stream(result.result.dump());
    return {true, "done"};
}
} // namespace arn
int main() {
    return arn::run_server();
}
