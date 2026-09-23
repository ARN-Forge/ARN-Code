#include "arn/core/net/sse_decoder.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

} // namespace

int main() {
    try {
        // Test 1: Single complete event
        {
            arn::core::net::SseDecoder decoder;
            std::vector<std::string> events;
            decoder.push("data: hello world\n\n", [&](std::string_view ev) {
                events.emplace_back(ev);
            });
            check(events.size() == 1, "Expected 1 event");
            check(events[0] == "hello world", "Expected 'hello world'");
        }

        // Test 2: Multi-line data fields
        {
            arn::core::net::SseDecoder decoder;
            std::vector<std::string> events;
            decoder.push("data: first line\ndata: second line\n\n", [&](std::string_view ev) {
                events.emplace_back(ev);
            });
            check(events.size() == 1, "Expected 1 multi-line event");
            check(events[0] == "first line\nsecond line", "Expected lines joined with newline");
        }

        // Test 3: CRLF line endings
        {
            arn::core::net::SseDecoder decoder;
            std::vector<std::string> events;
            decoder.push("data: crlf event\r\n\r\n", [&](std::string_view ev) {
                events.emplace_back(ev);
            });
            check(events.size() == 1, "Expected 1 CRLF event");
            check(events[0] == "crlf event", "Expected 'crlf event'");
        }

        // Test 4: Fragmented delivery (chunked / streaming bytes)
        {
            arn::core::net::SseDecoder decoder;
            std::vector<std::string> events;
            auto emit = [&](std::string_view ev) { events.emplace_back(ev); };

            decoder.push("da", emit);
            check(events.empty(), "Should not emit on incomplete 'da'");
            decoder.push("ta: chunk", emit);
            check(events.empty(), "Should not emit before delimiter");
            decoder.push("1\n", emit);
            check(events.empty(), "Should not emit before double newline");
            decoder.push("\n", emit);
            check(events.size() == 1 && events[0] == "chunk1", "Expected 'chunk1'");

            decoder.push("data: chunk2\n\n", emit);
            check(events.size() == 2 && events[1] == "chunk2", "Expected 'chunk2'");
        }

        // Test 5: Trailing data without trailing blank line handled by finish()
        {
            arn::core::net::SseDecoder decoder;
            std::vector<std::string> events;
            auto emit = [&](std::string_view ev) { events.emplace_back(ev); };

            decoder.push("data: unclosed tail", emit);
            check(events.empty(), "Should not emit before finish()");
            decoder.finish(emit);
            check(events.size() == 1 && events[0] == "unclosed tail",
                  "finish() should emit unclosed tail");
        }

        // Test 6: Space after data: prefix handling
        {
            arn::core::net::SseDecoder decoder;
            std::vector<std::string> events;
            auto emit = [&](std::string_view ev) { events.emplace_back(ev); };

            decoder.push("data:with_space\n\ndata: without_space\n\n", emit);
            check(events.size() == 2, "Expected 2 events");
            check(events[0] == "with_space", "Expected 'with_space'");
            check(events[1] == "without_space", "Expected 'without_space'");
        }

        // Test 7: Ignore comments and empty lines
        {
            arn::core::net::SseDecoder decoder;
            std::vector<std::string> events;
            auto emit = [&](std::string_view ev) { events.emplace_back(ev); };

            decoder.push(": this is a comment\n\ndata: payload\n\n", emit);
            check(events.size() == 1 && events[0] == "payload", "Expected comment to be ignored");
        }

        std::cout << "All SseDecoder tests passed successfully.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "SseDecoder test failed: " << ex.what() << '\n';
        return 1;
    }
}
