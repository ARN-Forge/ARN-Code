#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace arn::core::net {

class SseDecoder {
public:
    template <typename EventFn>
    void push(std::string_view bytes, EventFn&& on_event) {
        // Accept both SSE line endings. JSON carriage returns are escaped, so
        // stripping transport-level '\r' here cannot alter event data.
        for (const char byte : bytes) {
            if (byte != '\r')
                pending_.push_back(byte);
        }
        for (;;) {
            const auto end = pending_.find("\n\n");
            if (end == std::string::npos)
                break;
            std::string event = pending_.substr(0, end);
            pending_.erase(0, end + 2);

            std::string data;
            std::size_t start = 0;
            while (start <= event.size()) {
                const auto line_end = event.find('\n', start);
                std::string_view line(event.data() + start,
                                      (line_end == std::string::npos ? event.size() : line_end) -
                                          start);
                if (!line.empty() && line.back() == '\r')
                    line.remove_suffix(1);
                if (line.starts_with("data:")) {
                    line.remove_prefix(5);
                    if (!line.empty() && line.front() == ' ')
                        line.remove_prefix(1);
                    if (!data.empty())
                        data.push_back('\n');
                    data.append(line);
                }
                if (line_end == std::string::npos)
                    break;
                start = line_end + 1;
            }
            if (!data.empty())
                on_event(data);
        }
    }

    template <typename EventFn>
    void finish(EventFn&& on_event) {
        if (pending_.empty())
            return;

        // Some HTTP servers omit the final blank SSE line. Treat the tail as
        // one final event instead of silently dropping the model response.
        if (pending_.starts_with("data:")) {
            push("\n\n", std::forward<EventFn>(on_event));
            return;
        }
        std::string tail = std::move(pending_);
        pending_.clear();
        on_event(tail);
    }

private:
    std::string pending_;
};

} // namespace arn::core::net
