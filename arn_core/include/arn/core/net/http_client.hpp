#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <thread>

#include <httplib.h>

namespace arn::core::net {

constexpr int max_request_attempts = 3;

using ProgressCallback = std::function<void(std::string_view message)>;
using EventCallback = std::function<void(std::string_view event_data)>;

[[nodiscard]] bool should_retry(const httplib::Result& response);

[[nodiscard]] std::chrono::milliseconds retry_delay(const httplib::Result& response, int attempt);

template <typename RequestFn>
auto execute_with_retry(RequestFn&& request, const std::atomic_bool* cancel_requested = nullptr,
                        int max_attempts = max_request_attempts) {
    for (int attempt = 0;; ++attempt) {
        auto response = request();
        if ((cancel_requested && cancel_requested->load()) || !should_retry(response) ||
            attempt + 1 >= max_attempts)
            return response;
        const auto deadline = std::chrono::steady_clock::now() + retry_delay(response, attempt);
        while (std::chrono::steady_clock::now() < deadline) {
            if (cancel_requested && cancel_requested->load())
                return response;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
}

template <typename RequestFn, typename ProgressFn = ProgressCallback>
auto execute_stream_with_retry(RequestFn&& request, const bool& received_event,
                               const std::atomic_bool* cancel_requested = nullptr,
                               const ProgressFn& on_progress = {},
                               int max_attempts = max_request_attempts) {
    for (int attempt = 0;; ++attempt) {
        if (on_progress)
            on_progress("Waiting for provider response (attempt " + std::to_string(attempt + 1) +
                        ")");
        auto response = request();
        // Never replay a partially printed answer: retrying then would show
        // duplicated text to the person using ARN.
        if ((cancel_requested && cancel_requested->load(std::memory_order_relaxed)) ||
            received_event || !should_retry(response) || attempt + 1 >= max_attempts ||
            (!response && response.error() == httplib::Error::Read))
            return response;
        // A full idle read timeout already waited 90 seconds. Do not silently repeat it.
        if (on_progress)
            on_progress("Provider unavailable or rate-limited; waiting before retry");
        const auto deadline = std::chrono::steady_clock::now() + retry_delay(response, attempt);
        while (std::chrono::steady_clock::now() < deadline) {
            if (cancel_requested && cancel_requested->load())
                return response;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
}

httplib::Result stream_post(httplib::Client& client, const std::string& path,
                            const httplib::Headers& headers, const std::string& body,
                            const EventCallback& on_event,
                            std::string& error_body, bool& received_event,
                            const std::atomic_bool* cancel_requested = nullptr);

} // namespace arn::core::net
