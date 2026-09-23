#include "arn/core/net/http_client.hpp"
#include "arn/core/net/sse_decoder.hpp"

#include <chrono>
#include <random>
#include <string>

namespace arn::core::net {

bool should_retry(const httplib::Result& response) {
    if (!response)
        return true;
    const int status = response->status;
    return status == 429 || status == 500 || status == 502 || status == 503 || status == 504;
}

std::chrono::milliseconds retry_delay(const httplib::Result& response, int attempt) {
    if (response && response->has_header("Retry-After")) {
        try {
            const auto seconds = std::stoi(response->get_header_value("Retry-After"));
            if (seconds >= 0 && seconds <= 60)
                return std::chrono::seconds(seconds);
        } catch (const std::exception&) {
            // A date-form Retry-After header is uncommon for these APIs; use
            // exponential backoff when it cannot be parsed as seconds.
        }
    }

    const int base_ms = 500 * (1 << attempt);
    std::uniform_int_distribution<int> jitter(0, 250);
    static thread_local std::mt19937 generator(std::random_device{}());
    return std::chrono::milliseconds(base_ms + jitter(generator));
}

httplib::Result stream_post(httplib::Client& client, const std::string& path,
                            const httplib::Headers& headers, const std::string& body,
                            const EventCallback& on_event,
                            std::string& error_body, bool& received_event,
                            const std::atomic_bool* cancel_requested) {
    httplib::Request request;
    request.method = "POST";
    request.path = path;
    request.headers = headers;
    request.headers.emplace("Accept", "text/event-stream");
    request.headers.emplace("Content-Type", "application/json");
    request.body = body;

    int status = 0;
    SseDecoder decoder;
    request.response_handler = [&status](const httplib::Response& response) {
        status = response.status;
        return true;
    };
    request.progress = [cancel_requested](std::uint64_t, std::uint64_t) {
        return !cancel_requested || !cancel_requested->load(std::memory_order_relaxed);
    };
    request.content_receiver = [&](const char* data, std::size_t size, std::uint64_t,
                                   std::uint64_t) {
        if (cancel_requested && cancel_requested->load(std::memory_order_relaxed))
            return false;
        if (status < 200 || status >= 300) {
            error_body.append(data, size);
            return true;
        }
        decoder.push(std::string_view(data, size), [&](std::string_view event) {
            received_event = true;
            on_event(event);
        });
        return true;
    };
    auto response = client.send(request);
    if (response && response->status >= 200 && response->status < 300) {
        decoder.finish([&](std::string_view event) {
            received_event = true;
            on_event(event);
        });
    }
    return response;
}

} // namespace arn::core::net
