#include "terminal_ui.hpp"
#include <sstream>
#include <stdexcept>

// Only the terminal dimensions/capability are substituted. The production UI
// renders unchanged into a small VT screen model, without a provider or console.
namespace {
arn::TerminalSize test_size{100, 30};
bool test_native = true;
void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
}
namespace arn {
struct TerminalSession::Impl {};
TerminalSession::TerminalSession() : impl_(std::make_unique<Impl>()) {}
TerminalSession::~TerminalSession() = default;
TerminalSize TerminalSession::size() const { return test_size; }
bool TerminalSession::native_scrollback() const noexcept { return test_native; }
}

namespace {
using arn::terminal_ui::Ui;
using arn::terminal_ui::Tone;

struct Screen {
    using Row = std::vector<std::string>;
    std::vector<Row> rows, history;
    int width, height, x{}, y{}, saved_x{}, saved_y{};
    explicit Screen(int w = 100, int h = 30)
        : rows(h, Row(w, " ")), width(w), height(h) {}
    void down() {
        if (++y == height) {
            history.push_back(rows.front());
            rows.erase(rows.begin());
            rows.emplace_back(width, " ");
            --y;
        }
    }
    void resize(int w) {
        width = w;
        for (auto& row : rows) row.resize(w, " ");
        x = std::min(x, w - 1);
    }
    void feed(std::string_view text) {
        for (std::size_t index = 0; index < text.size();) {
            if (text[index] == '\x1b' && index + 1 < text.size() && text[index + 1] == '[') {
                index += 2;
                std::string parameters;
                while (index < text.size() && !(text[index] >= '@' && text[index] <= '~'))
                    parameters += text[index++];
                check(index < text.size(), "Incomplete VT command");
                const char command = text[index++];
                if (parameters.starts_with('?')) continue; // Colors/modes do not move the cursor.
                const auto split = parameters.find(';');
                const int first = parameters.empty() ? 1 : std::stoi(parameters);
                const int second = split == std::string::npos ? 1 : std::stoi(parameters.substr(split + 1));
                switch (command) {
                case 'A': y = std::max(0, y - first); break;
                case 'B': y = std::min(height - 1, y + first); break;
                case 'G': x = std::clamp(first - 1, 0, width - 1); break;
                case 'H': y = std::clamp(first - 1, 0, height - 1);
                          x = std::clamp(second - 1, 0, width - 1); break;
                case 's': saved_x = x; saved_y = y; break;
                case 'u': x = saved_x; y = saved_y; break;
                case 'K': check(first == 2, "Unexpected partial line erase");
                          rows[y].assign(width, " "); break;
                case 'J': check(first == 2, "Unexpected history erase");
                          rows.assign(height, Row(width, " ")); break;
                default: break;
                }
            } else if (text[index] == '\r') { x = 0; ++index;
            } else if (text[index] == '\n') { down(); ++index;
            } else {
                const auto count = std::min(arn::terminal_ui::char_bytes(
                    static_cast<unsigned char>(text[index])), text.size() - index);
                rows[y][x] = text.substr(index, count);
                x = std::min(width - 1, x + 1);
                index += count;
            }
        }
    }
    static std::string joined(const Row& row) {
        std::string result;
        for (const auto& cell : row) result += cell;
        return result;
    }
    std::string line(int row) const { return joined(rows.at(row)); }
    std::string all() const {
        std::string result;
        for (const auto& row : history) result += joined(row) + '\n';
        for (const auto& row : rows) result += joined(row) + '\n';
        return result;
    }
};

struct Capture {
    std::ostringstream output;
    std::streambuf* original{std::cout.rdbuf(output.rdbuf())};
    ~Capture() { std::cout.rdbuf(original); }
    std::string take() { auto text = output.str(); output.str({}); return text; }
};

std::size_t occurrences(std::string_view text, std::string_view token) {
    std::size_t count = 0, from = 0;
    while ((from = text.find(token, from)) != std::string_view::npos) { ++count; from += token.size(); }
    return count;
}

void header_matches_existing_layout() {
    Capture capture;
    arn::TerminalSession terminal;
    for (const auto width : {100, 70, 50}) {
        test_size.width = width;
        test_native = false;
        Ui original(terminal);
        original.session("Gemini", "fake-model", false);
        original.render();
        Screen expected(width);
        expected.feed(capture.take());

        test_native = true;
        Ui native(terminal);
        native.session("Gemini", "fake-model", false);
        native.status(native.ready_status());
        native.render();
        Screen actual(width);
        const auto output = capture.take();
        actual.feed(output);
        const int header = width - 4 < 58 ? 5 : width - 4 < 76 ? 7 : 9;
        for (int row = 0; row < header; ++row)
            check(actual.line(row) == expected.line(row), "Windows header differs from original layout");
        check(actual.line(26) == expected.line(26), "Input separator differs from original layout");
        check(actual.line(27).find("arn › ") == 0, "Input area is missing");
        check(actual.line(28).find("Type /help") == 0, "Footer hint is missing");
        check(output.find("\x1b[2J") == std::string::npos, "Windows clears the viewport on refresh");
    }
    test_size = {100, 30};
}

void streaming_input_and_history() {
    Capture capture;
    arn::TerminalSession terminal;
    Ui ui(terminal);
    Screen screen;
    std::string emitted;
    const auto draw = [&] {
        const auto output = capture.take();
        emitted += output;
        screen.feed(output);
        return output;
    };
    ui.session("Gemini", "fake-model", false);
    ui.status(ui.ready_status());
    ui.render(); draw();
    check(screen.line(3).find("▄▄          ▄▄") != std::string::npos, "Arny is missing");
    check(screen.line(7).find("Context: empty") != std::string::npos, "Session row is missing");
    ui.add(Tone::normal, {});
    ui.status("Working · Esc or Ctrl+C cancels");
    ui.append("Вітаю"); ui.render(); draw();
    ui.append(" вас!"); ui.render();
    const auto delta = draw();
    check(delta.find("Вітаю") == std::string::npos, "Streaming replays committed text");
    check(screen.line(10).find("Вітаю вас!") != std::string::npos, "Streaming broke a conversation line");
    check(screen.line(28).find("Ctrl+C cancels") != std::string::npos, "Busy footer disappeared");
    ui.session("Gemini", "short", true);
    ui.status(ui.ready_status()); ui.render(); draw();
    check(screen.line(7).find("Model: short · Context: active") != std::string::npos,
          "Header metadata did not update in place");
    check(screen.line(7).find("fake-model") == std::string::npos, "Old model text was retained");
    ui.hint("Enter submits · Esc cancels");
    ui.input("*****"); ui.render_input(); draw();
    check(screen.line(27).find("arn › *****") == 0, "Masked input area disappeared");
    ui.input("****"); ui.render_input(); draw();
    check(screen.line(27).find("arn › **** ") == 0, "Backspace left a stale masking character");
    ui.hint({}); ui.input({});
    ui.status("Allow this tool call? [y/N]");
    ui.show_confirmation({}); draw();
    ui.show_confirmation("y"); draw();
    check(screen.line(27).find("arn › y") == 0, "Confirmation choice is not visible");
    check(screen.line(28).find("[y/N]") != std::string::npos, "Confirmation prompt is missing");
    ui.show_confirmation({}); draw();
    check(screen.line(27).find("arn ›   ") == 0, "Confirmation Backspace did not erase the choice");
    ui.show_confirmation("n"); ui.finish_confirmation('n'); draw();
    ui.status(ui.ready_status()); ui.render(); draw();
    check(screen.line(10).find("Вітаю вас!") != std::string::npos, "Confirmation damaged conversation");

    for (int line = 0; line < 60; ++line) {
        ui.append("\nstream-row-" + std::to_string(line) + "!");
        ui.render(); draw();
    }
    ui.session("Gemini", "short", false); ui.render(); draw();
    auto history = screen.all();
    check(occurrences(history, "Вітаю вас!") == 1, "Conversation was lost or duplicated in scrollback");
    for (int line = 0; line < 60; ++line)
        check(occurrences(history, "stream-row-" + std::to_string(line) + "!") == 1,
              "Streamed row was lost or replayed");
    check(!screen.history.empty(), "Output did not enter native history");
    ui.add(Tone::user, "arn › next turn"); ui.render(); draw();
    check(screen.line(1).find("ARN AGENT CODE") != std::string::npos,
          "The next turn did not restore the original visual frame");
    check(screen.line(7).find("Context: empty") != std::string::npos,
          "Deferred session update was lost after scrolling");
    check(screen.all().find("stream-row-59!") != std::string::npos,
          "New frame erased the previous response");
    for (int line = 0; line < 200; ++line) {
        ui.add(Tone::muted, "history-entry-" + std::to_string(line) + "!");
        ui.render(); draw();
    }
    history = screen.all();
    for (int line = 0; line < 200; ++line)
        check(occurrences(history, "history-entry-" + std::to_string(line) + "!") == 1,
              "Pruning internal log lost or duplicated native history");
    test_size.width = 70;
    screen.resize(70);
    ui.render(); draw();
    check(screen.line(0).find("╭") != std::string::npos && screen.line(6).find("╰") != std::string::npos,
          "Resize did not use the original compact header");
    ui.input("Після resize"); ui.render_input(); draw();
    check(screen.line(27).find("Після resize") != std::string::npos, "Input broke after resize");
    for (const auto forbidden : {"\x1b[2J", "\x1b[3J", "1049h", "1000h", "1006h", "F2 copy mode"})
        check(emitted.find(forbidden) == std::string::npos, "Native renderer captured mouse/history or advertised F2");
}
}

int main() {
    try {
        header_matches_existing_layout();
        streaming_input_and_history();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
