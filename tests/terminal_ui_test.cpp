#include "terminal_ui.hpp"
#include <sstream>
#include <stdexcept>
#include <unordered_map>

// The backend shim reads the same flushed screen as the renderer writes.
// Protected transcript assertions remain independent of ownership snapshots.
namespace {
arn::TerminalSize test_size{100, 30};
bool test_native = true;
bool anchor_available = true;
// Region read-back fails while new regions can still be appended, as with
// redirected stdout or an unreadable console buffer.
bool capture_fails = false;
std::size_t capture_count{};
struct Screen;
Screen* backend_screen{};
std::function<void()> sync_screen;
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
    struct Cell {
        std::string text{" "};
        std::size_t committed{};
    };
    struct Row {
        std::vector<Cell> cells;
        bool continues{}; // A model-generated soft wrap, not an explicit newline.
        explicit Row(int width) : cells(static_cast<std::size_t>(width)) {}
    };
    std::vector<Row> rows, history;
    std::unordered_map<std::size_t, std::string> committed;
    std::size_t next_owner{1}, owned_writes{}, stale_restores{};
    int width, height, x{}, y{}, saved_x{}, saved_y{};
    bool saved_valid{}, scrollback;
    std::uint64_t generation{1};
    explicit Screen(int w = 100, int h = 30, bool native = true)
        : rows(h, Row(w)), width(w), height(h), scrollback(native) {}
    void down() {
        if (++y == height) {
            if (scrollback) history.push_back(std::move(rows.front()));
            rows.erase(rows.begin());
            rows.emplace_back(width);
            --y;
        }
    }
    void resize(int w) { resize(w, height); }
    void resize(int w, int h) {
        check(w > 0 && h > 0, "Invalid model geometry");
        if (w == width && h == height) return;
        ++generation;
        saved_valid = false; // Old saved coordinates are not a reflow anchor.
        if (!scrollback) {
            // An alternate-screen owner can repaint its entire viewport.
            rows.resize(h, Row(w));
            for (auto& row : rows) row.cells.resize(w);
            x = std::clamp(x, 0, w - 1);
            y = std::clamp(y, 0, h - 1);
            width = w; height = h;
            return;
        }
        // Conservative main-buffer reflow: preserve cells and their ownership,
        // split/join only soft wraps, and move rows across the history boundary.
        // This is not a Windows Terminal emulator (Unicode cell widths, host
        // selection and viewport policies intentionally remain outside it).
        std::vector<Row> physical = history;
        physical.insert(physical.end(), rows.begin(), rows.end());
        const auto cursor_row = history.size() + static_cast<std::size_t>(y);
        std::vector<Row> reflowed;
        std::vector<Cell> logical;
        int cursor_offset = -1, new_cursor_row = 0, new_cursor_x = 0;
        for (std::size_t index = 0; index < physical.size(); ++index) {
            const auto& row = physical[index];
            std::size_t used = row.cells.size();
            if (!row.continues) {
                while (used && row.cells[used - 1].text == " " &&
                       row.cells[used - 1].committed == 0) --used;
            }
            if (index == cursor_row) {
                cursor_offset = static_cast<int>(logical.size()) + x;
                used = std::max(used, static_cast<std::size_t>(x + 1));
            }
            logical.insert(logical.end(), row.cells.begin(), row.cells.begin() + used);
            if (row.continues && index + 1 < physical.size()) continue;
            const auto count = std::max<std::size_t>(1, (logical.size() + w - 1) / w);
            if (cursor_offset >= 0) {
                new_cursor_row = static_cast<int>(reflowed.size()) + cursor_offset / w;
                new_cursor_x = cursor_offset % w;
            }
            for (std::size_t part = 0; part < count; ++part) {
                Row next(w);
                const auto begin = part * w;
                const auto cells = begin < logical.size()
                    ? std::min<std::size_t>(w, logical.size() - begin) : 0;
                if (cells) std::copy_n(logical.begin() + begin, cells, next.cells.begin());
                next.continues = part + 1 < count;
                reflowed.push_back(std::move(next));
            }
            logical.clear();
            cursor_offset = -1;
        }
        while (reflowed.size() < static_cast<std::size_t>(h)) reflowed.emplace_back(w);
        const auto boundary = reflowed.size() - h;
        history.assign(reflowed.begin(), reflowed.begin() + boundary);
        rows.assign(reflowed.begin() + boundary, reflowed.end());
        width = w; height = h;
        x = std::clamp(new_cursor_x, 0, w - 1);
        y = std::clamp(new_cursor_row - static_cast<int>(boundary), 0, h - 1);
    }
    void host_cursor(int row, int column) {
        // An explicit host event, not a claim about Windows Terminal's policy.
        // The backend must independently verify the physical cursor/region.
        y = std::clamp(row, 0, height - 1);
        x = std::clamp(column, 0, width - 1);
    }
    void protect(std::string_view token) {
        bool found = false;
        const auto mark = [&](Row& row) {
            const auto text = joined(row);
            std::size_t from = 0, position;
            while ((position = text.find(token, from)) != std::string::npos) {
                found = true;
                std::size_t byte = 0;
                for (auto& cell : row.cells) {
                    const auto end = byte + cell.text.size();
                    if (byte < position + token.size() && end > position && !cell.committed) {
                        cell.committed = next_owner++;
                        committed.emplace(cell.committed, cell.text);
                    }
                    byte = end;
                }
                from = position + token.size();
            }
        };
        for (auto& row : history) mark(row);
        for (auto& row : rows) mark(row);
        check(found, "Protected transcript token is missing from fixture");
    }
    bool committed_intact() const {
        std::unordered_map<std::size_t, std::string> retained;
        const auto collect = [&](const Row& row) {
            for (const auto& cell : row.cells) if (cell.committed) {
                if (!retained.emplace(cell.committed, cell.text).second) return false;
            }
            return true;
        };
        for (const auto& row : history) if (!collect(row)) return false;
        for (const auto& row : rows) if (!collect(row)) return false;
        return retained == committed;
    }
    int visible_row(std::string_view token) const {
        for (int row = 0; row < height; ++row)
            if (line(row).find(token) != std::string::npos) return row;
        throw std::runtime_error("Host cursor fixture needs a visible transcript row");
    }
    void erase_row(Row& row) {
        for (const auto& cell : row.cells) owned_writes += cell.committed != 0;
        row = Row(width);
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
                if (parameters.starts_with('?')) continue;
                const auto split = parameters.find(';');
                const int first = parameters.empty() ? 1 : std::stoi(parameters);
                const int second = split == std::string::npos ? 1 : std::stoi(parameters.substr(split + 1));
                switch (command) {
                case 'A': y = std::max(0, y - first); break;
                case 'B': y = std::min(height - 1, y + first); break;
                case 'G': x = std::clamp(first - 1, 0, width - 1); break;
                case 'H': y = std::clamp(first - 1, 0, height - 1);
                          x = std::clamp(second - 1, 0, width - 1); break;
                case 's': saved_x = x; saved_y = y; saved_valid = true; break;
                case 'u': if (!saved_valid) { ++stale_restores; break; }
                          x = saved_x; y = saved_y; break;
                case 'K': check(first == 2, "Unexpected partial line erase");
                          erase_row(rows[y]); break;
                case 'J': check(first == 2, "Unexpected history erase");
                          for (auto& row : rows) erase_row(row); break;
                default: break;
                }
            } else if (text[index] == '\r') { x = 0; ++index;
            } else if (text[index] == '\n') { down(); ++index;
            } else {
                const auto count = std::min(arn::terminal_ui::char_bytes(
                    static_cast<unsigned char>(text[index])), text.size() - index);
                owned_writes += rows[y].cells[x].committed != 0;
                rows[y].cells[x] = {std::string(text.substr(index, count)), 0};
                x = std::min(width - 1, x + 1);
                index += count;
            }
        }
    }
    static std::string joined(const Row& row) {
        std::string result;
        for (const auto& cell : row.cells) result += cell.text;
        return result;
    }
    std::string line(int row) const { return joined(rows.at(row)); }
    std::string all() const {
        std::string result;
        for (const auto& row : history) result += joined(row) + (row.continues ? "" : "\n");
        for (const auto& row : rows) result += joined(row) + (row.continues ? "" : "\n");
        return result;
    }
};

} // namespace

namespace arn {
struct TerminalLiveAnchor::State {
    TerminalSize geometry;
    std::uint64_t generation{};
    int x{}, y{}, before{}, after{}, top{}, bottom{};
    std::size_t history{};
    std::vector<std::string> rows;
};

TerminalLiveAnchor TerminalSession::capture_live_anchor(int before, int after) const {
    ++capture_count;
    if (!anchor_available || capture_fails) return {};
    if (sync_screen) sync_screen();
    auto state = std::make_shared<TerminalLiveAnchor::State>();
    state->geometry = test_size;
    state->before = before; state->after = after;
    // Byte-emission-only tests have no screen; their token checks geometry.
    // Ownership, layout and streaming tests use the independently bound model.
    if (backend_screen) {
        const auto& screen = *backend_screen;
        state->generation = screen.generation;
        state->x = screen.x; state->y = screen.y;
        state->history = screen.history.size();
        state->top = std::max(0, screen.y - before);
        state->bottom = std::min(screen.height - 1, screen.y + after);
        for (int row = state->top; row <= state->bottom; ++row)
            state->rows.push_back(screen.line(row));
    }
    return {std::move(state)};
}

bool TerminalSession::live_anchor_valid(const TerminalLiveAnchor& anchor) const {
    if (!anchor.state) return false;
    const auto current = capture_live_anchor(anchor.state->before, anchor.state->after);
    if (!current.state) return false;
    const auto& a = *anchor.state;
    const auto& b = *current.state;
    return a.geometry.width == b.geometry.width && a.geometry.height == b.geometry.height &&
           a.generation == b.generation && a.x == b.x && a.y == b.y &&
           a.history == b.history && a.top == b.top && a.bottom == b.bottom && a.rows == b.rows;
}

bool TerminalSession::begin_output_region() const {
    if (!anchor_available) return false;
    std::cout << "\x1b[" << test_size.height << ";1H\r\n" << std::flush;
    return true;
}
} // namespace arn

namespace {
struct Capture {
    std::ostringstream output;
    std::streambuf* original{std::cout.rdbuf(output.rdbuf())};
    Screen* bound{};
    std::size_t consumed{};
    void bind(Screen& screen) {
        bound = backend_screen = &screen;
        sync_screen = [this] { flush_screen(); };
    }
    void flush_screen() {
        if (!bound) return;
        const auto text = output.str();
        bound->feed(std::string_view(text).substr(consumed));
        consumed = text.size();
    }
    void unbind() {
        flush_screen();
        bound = backend_screen = nullptr;
        sync_screen = {};
    }
    ~Capture() {
        sync_screen = {};
        backend_screen = nullptr;
        std::cout.rdbuf(original);
    }
    std::string take() {
        flush_screen();
        auto text = output.str(); output.str({}); consumed = 0; return text;
    }
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
        Screen actual(width);
        capture.bind(actual);
        native.render();
        const auto output = capture.take();
        const int header = width - 4 < 58 ? 5 : width - 4 < 76 ? 7 : 9;
        for (int row = 0; row < header; ++row)
            check(actual.line(row) == expected.line(row), "Windows header differs from original layout");
        check(actual.line(26) == expected.line(26), "Input separator differs from original layout");
        check(actual.line(27).find("arn › ") == 0, "Input area is missing");
        check(actual.line(28).find("Type /help") == 0, "Footer hint is missing");
        check(output.find("\x1b[2J") == std::string::npos, "Windows clears the viewport on refresh");
        capture.unbind();
    }
    test_size = {100, 30};
}

void streaming_input_and_history() {
    Capture capture;
    arn::TerminalSession terminal;
    Ui ui(terminal);
    Screen screen;
    capture.bind(screen);
    std::string emitted;
    const auto draw = [&] {
        const auto output = capture.take();
        emitted += output;
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
    check(occurrences(screen.all(), "ARN AGENT CODE") == 1,
          "The next turn duplicated the startup block in history");
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
    check(occurrences(screen.all(), "ARN AGENT CODE") == 1,
          "Resize duplicated the startup block in history");
    ui.input("Після resize"); ui.render_input(); draw();
    check(screen.line(screen.y).find("Після resize") != std::string::npos, "Input broke after resize");
    for (const auto forbidden : {"\x1b[2J", "\x1b[3J", "1049h", "1000h", "1006h", "F2 copy mode"})
        check(emitted.find(forbidden) == std::string::npos, "Native renderer captured mouse/history or advertised F2");
}

void startup_is_not_a_redraw_frame() {
    Capture capture;
    arn::TerminalSession terminal;
    test_native = true;
    test_size = {100, 30};
    Ui ui(terminal);
    ui.render();
    auto emitted = capture.take();
    check(occurrences(emitted, "ARN AGENT CODE") == 1, "Startup must emit one welcome banner");
    for (const auto size : {arn::TerminalSize{70, 24}, {120, 45}, {50, 18},
                            {100, 30}, {30, 10}, {100, 30}}) {
        test_size = size;
        for (int redraw = 0; redraw < 3; ++redraw) ui.render();
        emitted += capture.take();
        check(occurrences(emitted, "ARN AGENT CODE") == 1,
              "Resize/font geometry redraw replayed the startup banner");
    }
    for (int turn = 0; turn < 240; ++turn) {
        ui.session(turn % 2 ? "OmniRoute" : "Gemini", "fake-model-" + std::to_string(turn), true);
        ui.add(Tone::user, "unique-turn-" + std::to_string(turn) + "!");
        ui.add(Tone::normal, {});
        ui.append("Привіт\nunique-reply-" + std::to_string(turn) + "!");
        ui.render();
        for (int redraw = 0; redraw < 3; ++redraw) ui.render();
        emitted += capture.take();
        check(occurrences(emitted, "ARN AGENT CODE") == 1,
              "Transcript growth/provider changes replayed the startup banner");
    }
    for (int turn = 0; turn < 240; ++turn) {
        check(occurrences(emitted, "unique-turn-" + std::to_string(turn) + "!") == 1,
              "Redraw/pruning duplicated logical user output");
        check(occurrences(emitted, "unique-reply-" + std::to_string(turn) + "!") == 1,
              "Redraw/pruning duplicated logical assistant output");
    }
    ui.show_confirmation("y"); ui.show_confirmation({}); ui.finish_confirmation('n');
    ui.input("*****"); ui.render_input(); ui.render();
    ui.scroll(3); ui.scroll_to_bottom(); ui.render();
    ui.clear(); ui.render();
    emitted += capture.take();
    check(occurrences(emitted, "ARN AGENT CODE") == 1,
          "Editor/confirmation/clear replayed the startup banner");

    // POSIX repaints an alternate-screen viewport, rather than appending a
    // native transcript. Repeated paints must leave only one visible header.
    test_native = false;
    test_size = {100, 30};
    Ui viewport(terminal);
    Screen screen(100, 30, false);
    viewport.add(Tone::normal, "posix-log-entry!");
    for (int redraw = 0; redraw < 12; ++redraw) {
        viewport.session("Gemini", "fake-model-" + std::to_string(redraw), true);
        test_size.width = redraw % 2 ? 70 : 100;
        screen.resize(test_size.width);
        viewport.render(); screen.feed(capture.take());
        check(occurrences(screen.all(), "ARN AGENT CODE") == 1,
              "POSIX viewport redraw duplicated its visible header");
        check(occurrences(screen.all(), "posix-log-entry!") == 1,
              "POSIX viewport redraw mutated logical output");
        check(screen.history.empty(), "POSIX viewport redraw appended history");
    }
    test_native = true;
    test_size = {100, 30};
}
struct NativeFixture {
    Capture capture;
    arn::TerminalSession terminal;
    Ui ui{terminal};
    Screen screen;
    std::string emitted;
    NativeFixture() {
        capture.bind(screen);
        test_native = true;
        test_size = {100, 30};
        ui.session("Gemini", "fake-model", false);
        ui.status(ui.ready_status());
        ui.render();
        draw();
    }
    std::string draw() {
        auto output = capture.take();
        emitted += output;
        return output;
    }
    void turns(int count) {
        for (int turn = 0; turn < count; ++turn) {
            ui.add(Tone::user, "owner-user-" + std::to_string(turn) + "!");
            ui.add(Tone::normal, "owner-reply-" + std::to_string(turn) + "!");
            ui.render();
            draw();
        }
        screen.protect("ARN AGENT CODE");
        for (int turn = 0; turn < count; ++turn) {
            screen.protect("owner-user-" + std::to_string(turn) + "!");
            screen.protect("owner-reply-" + std::to_string(turn) + "!");
        }
    }
    void resize(arn::TerminalSize size) {
        screen.resize(size.width, size.height);
        test_size = size;
        check(screen.committed_intact(), "Host resize itself damaged committed cells");
    }
    void check_history() const {
        if (screen.owned_writes || !screen.committed_intact())
            throw std::runtime_error("Renderer wrote/erased committed transcript cells: " +
                                     std::to_string(screen.owned_writes));
        check(screen.stale_restores == 0, "Renderer restored an invalid saved cursor");
    }
};

void model_reflow_preserves_ownership() {
    Screen screen(12, 6);
    screen.feed("history-a\r\nhistory-b\r\nlive");
    screen.protect("history-a");
    screen.protect("history-b");
    screen.feed("\x1b[s");
    screen.resize(6, 3);
    check(!screen.history.empty(), "Height/width shrink did not move rows to history");
    check(screen.committed_intact(), "Model shrink dropped protected cells");
    screen.resize(24, 9);
    check(screen.committed_intact(), "Model grow dropped protected cells");
    check(occurrences(screen.all(), "history-a") == 1 &&
          occurrences(screen.all(), "history-b") == 1,
          "Model reflow split/join lost or duplicated logical history");
    const auto x = screen.x, y = screen.y;
    screen.feed("\x1b[u");
    check(screen.stale_restores == 1 && screen.x == x && screen.y == y,
          "Model silently trusted a cursor saved before resize");
    screen.host_cursor(screen.visible_row("history-b"), 0);
    screen.feed("\x1b[2K");
    check(screen.owned_writes > 0 && !screen.committed_intact(),
          "Ownership oracle failed to detect destructive history cleanup");
}

void geometry_redraw_preserves_transcript() {
    NativeFixture fixture;
    fixture.turns(40);
    for (int cycle = 0; cycle < 3; ++cycle) {
        for (const auto size : {arn::TerminalSize{70, 30}, {120, 30}, {100, 16},
                                {100, 45}, {42, 13}, {100, 30}}) {
            fixture.resize(size);
            fixture.ui.render(); fixture.draw();
            fixture.ui.render_input(); fixture.draw();
            fixture.ui.render(); fixture.draw();
            fixture.check_history();
        }
    }
    check(occurrences(fixture.emitted, "ARN AGENT CODE") == 1,
          "Geometry redraw emitted another startup block");
    for (int turn = 0; turn < 40; ++turn) {
        check(occurrences(fixture.emitted, "owner-user-" + std::to_string(turn) + "!") == 1,
              "Geometry-only redraw appended historical user output");
        check(occurrences(fixture.emitted, "owner-reply-" + std::to_string(turn) + "!") == 1,
              "Geometry-only redraw appended historical assistant output");
    }
    for (const auto forbidden : {"1049h", "1000h", "1006h", "\x1b[3J", "\x1b[2J"})
        check(fixture.emitted.find(forbidden) == std::string::npos,
              "Native ownership redraw captured the mouse or cleared history");
}

void input_survives_width_and_height_resize() {
    NativeFixture fixture;
    const std::string input = "Привіт: незавершений ввід збережено INPUT_SENTINEL";
    fixture.ui.input(input); // Deliberately never re-supplied after resize.
    fixture.ui.render_input(); fixture.draw();
    for (int cycle = 0; cycle < 3; ++cycle) {
        for (const auto size : {arn::TerminalSize{40, 16}, {100, 45}, {70, 13},
                                {120, 30}, {100, 30}}) {
            fixture.resize(size);
            fixture.ui.render(); fixture.draw();
            fixture.ui.render_input();
            const auto output = fixture.draw();
            if (size.width >= 100)
                check(output.find(input) != std::string::npos,
                      "Resize mutated the logical non-empty UTF-8 input buffer");
        }
    }
    check(occurrences(fixture.emitted, "ARN AGENT CODE") == 1,
          "Input resize redrew startup output");
    // Ui/read_line currently support an end-of-buffer cursor only. This test
    // does not pretend to exercise a nonexistent mid-buffer editor API.
}

void streaming_resize_keeps_committed_prefix() {
    NativeFixture fixture;
    fixture.ui.add(Tone::normal, {});
    fixture.ui.append("STREAM_PREFIX_Привіт");
    fixture.ui.render(); fixture.draw();
    fixture.screen.protect("STREAM_PREFIX_Привіт");
    for (const auto size : {arn::TerminalSize{60, 17}, {110, 42}, {100, 30}}) {
        fixture.resize(size);
        fixture.ui.render(); fixture.draw();
        fixture.check_history();
    }
    fixture.ui.append("_STREAM_SUFFIX");
    fixture.ui.render(); fixture.draw();
    fixture.ui.render(); fixture.draw();
    fixture.check_history();
    check(occurrences(fixture.emitted, "STREAM_PREFIX_Привіт") == 1 &&
          occurrences(fixture.emitted, "_STREAM_SUFFIX") == 1,
          "Resize replayed or omitted a streaming delta");
}

enum class Redraw { transcript, input, confirmation, finish_confirmation };

void anchor_evidence_is_not_just_geometry() {
    {
        NativeFixture fixture;
        const auto anchor = fixture.terminal.capture_live_anchor(1, 1);
        check(fixture.terminal.live_anchor_valid(anchor), "Fresh live anchor is invalid");
        // Host changes a live cell, keeping both cursor and geometry identical.
        fixture.screen.rows[fixture.screen.y].cells[0].text = "!";
        check(!fixture.terminal.live_anchor_valid(anchor),
              "Anchor trusted changed contents at the same cursor/geometry");
        fixture.ui.render_input(); fixture.draw();
        check(fixture.screen.line(fixture.screen.y).starts_with("arn › "),
              "Invalidated content did not establish a new input area");
    }
    {
        NativeFixture fixture;
        const auto anchor = fixture.terminal.capture_live_anchor(1, 1);
        fixture.resize({70, 20});
        fixture.resize({100, 30});
        check(!fixture.terminal.live_anchor_valid(anchor),
              "Returning to old geometry resurrected an earlier generation");
        fixture.ui.input("Збережений ввід");
        fixture.ui.render_input(); fixture.draw();
        check(fixture.screen.line(fixture.screen.y).find("Збережений ввід") != std::string::npos,
              "Generation invalidation lost logical input");
    }
}

void same_geometry_host_relocation(Redraw path) {
    NativeFixture fixture;
    fixture.turns(12);
    if (path == Redraw::confirmation || path == Redraw::finish_confirmation) {
        fixture.ui.status("Allow this tool call? [y/N]");
        fixture.ui.show_confirmation("y"); fixture.draw();
    }
    fixture.screen.host_cursor(fixture.screen.visible_row("owner-reply-11!"), 20);
    switch (path) {
    case Redraw::transcript: fixture.ui.render(); break;
    case Redraw::input: fixture.ui.render_input(); break;
    case Redraw::confirmation: fixture.ui.show_confirmation({}); break;
    case Redraw::finish_confirmation: fixture.ui.finish_confirmation('n'); break;
    }
    fixture.draw();
    fixture.check_history();
}

void new_live_frame_follows_geometry() {
    NativeFixture fixture;
    fixture.turns(12);
    fixture.ui.input("Привіт INPUT_SENTINEL");
    for (const auto size : {arn::TerminalSize{64, 17}, {110, 40}, {70, 22}, {100, 30}}) {
        fixture.resize(size);
        fixture.ui.render_input(); fixture.draw();
        fixture.check_history();
        check(fixture.screen.line(fixture.screen.y - 1).starts_with(
                  arn::terminal_ui::horizontal_rule(static_cast<std::size_t>(size.width - 2))),
              "Reanchored frame retained the old width");
        check(fixture.screen.line(fixture.screen.y).find("Привіт INPUT_SENTINEL") != std::string::npos,
              "Reanchored frame lost input");
    }
}

void unavailable_anchor_retries_without_mutation() {
    NativeFixture fixture;
    fixture.turns(12);
    fixture.ui.input("Збережений INPUT_SENTINEL");
    fixture.screen.host_cursor(fixture.screen.visible_row("owner-reply-11!"), 20);
    anchor_available = false;
    fixture.ui.render_input();
    check(fixture.draw().empty(), "Unavailable anchor still mutated terminal cells");
    fixture.ui.render();
    check(fixture.draw().empty(), "Retry with unavailable anchor wrote unowned cells");
    fixture.check_history();
    anchor_available = true;
    fixture.ui.render(); fixture.draw();
    fixture.check_history();
    check(fixture.screen.line(fixture.screen.y).find("Збережений INPUT_SENTINEL") != std::string::npos,
          "Recovered anchor did not redraw preserved input");
}

void stale_footer_after_host_relocation(Redraw path) {
    NativeFixture fixture;
    fixture.turns(12);
    if (path == Redraw::confirmation || path == Redraw::finish_confirmation) {
        fixture.ui.status("Allow this tool call? [y/N]");
        fixture.ui.show_confirmation("y");
        fixture.draw();
        fixture.check_history();
    }
    fixture.resize({64, 17});
    // Geometry alone cannot promise where a host has placed the cursor.
    // Preserve every historical cell, then explicitly exercise a different,
    // valid cursor location. This is a contract counterexample, not an
    // assertion that Windows Terminal always makes this particular move.
    fixture.screen.host_cursor(fixture.screen.visible_row("owner-reply-11!"), 20);
    check(fixture.screen.committed_intact(), "Cursor relocation changed history");
    switch (path) {
    case Redraw::transcript: fixture.ui.render(); break;
    case Redraw::input: fixture.ui.render_input(); break;
    case Redraw::confirmation: fixture.ui.show_confirmation({}); break;
    case Redraw::finish_confirmation: fixture.ui.finish_confirmation('n'); break;
    }
    fixture.draw();
    fixture.check_history();
}

// Test-only one-row adapter: intentionally not installed in the production Ui.
// It evaluates the existing backend contract without a separator or hint row.
// A refused edit is evidence of infeasibility, not a resize-redraw solution.
struct InlineOwnershipProbe {
    arn::TerminalSession& terminal;
    std::string prompt, logical;
    arn::TerminalLiveAnchor anchor;
    void begin() {
        std::cout << prompt << logical << std::flush;
        anchor = terminal.capture_live_anchor(0, 0);
    }
    bool edit(std::string value) {
        logical = std::move(value);
        if (!terminal.live_anchor_valid(anchor)) return false;
        std::cout << "\r\x1b[2K" << prompt << logical << std::flush;
        anchor = terminal.capture_live_anchor(0, 0);
        return true;
    }
};

void inline_probe_requires_current_ownership(bool confirmation) {
    Capture capture;
    arn::TerminalSession terminal;
    Screen screen;
    capture.bind(screen);
    std::cout << "inline-transcript!\r\n";
    capture.take();
    screen.protect("inline-transcript!");
    InlineOwnershipProbe probe{terminal, confirmation ? "Allow? [y/N] " : "arn › ", {}};
    probe.begin(); capture.take();
    check(probe.edit(confirmation ? "y" : "Привіт"), "One-row edit failed at stable geometry");
    capture.take();
    check(probe.edit({}), "One-row Backspace display failed at stable geometry");
    capture.take();
    const auto emitted_before = screen.all();
    for (const auto size : {arn::TerminalSize{64,17}, {120,45}, {100,30},
                            {90,27}, {80,24}, {100,30}}) {
        screen.resize(size.width, size.height); test_size = size;
        check(!probe.edit(confirmation ? "n" : "Збережений ввід"),
              "One-row adapter guessed ownership after geometry invalidation");
        check(capture.take().empty(), "Refused one-row edit emitted destructive output");
        check(probe.logical == (confirmation ? "n" : "Збережений ввід"),
              "Refused display mutation must still retain logical state");
        check(screen.committed_intact() && screen.owned_writes == 0,
              "One-row adapter corrupted adjacent transcript");
    }
    check(occurrences(screen.all(), confirmation ? "Allow? [y/N] " : "arn › ") == 1,
          "Probe allocated another historical input on resize");
    // No stacking here is achieved by REFUSING all post-resize edits. That
    // cannot satisfy the future classic editor's continued-editing requirement.
    check(!emitted_before.empty(), "Probe did not establish an initial line");
    // Make the protected row visible in the model for a separate relocation
    // counterexample. This does not prescribe Windows Terminal's policy.
    screen.resize(100, 100); test_size = {100, 100};
    screen.host_cursor(screen.visible_row("inline-transcript!"), 5);
    check(!probe.edit("n"), "One-row adapter accepted a relocated transcript cursor");
    check(capture.take().empty() && screen.committed_intact(),
          "One-row prompt bypassed protected-transcript ownership");
}

// A byte sink (redirected stdout) never certifies a region. Output must stay
// append-only and bounded: no live footer, nothing per keystroke.
void redirected_output_is_plain_and_bounded() {
    Capture capture;
    arn::TerminalSession terminal;
    Screen screen;
    capture.bind(screen);
    capture_fails = true;
    Ui ui(terminal);
    ui.session("Gemini", "fake-model", false);
    ui.status(ui.ready_status());
    ui.render();
    std::string emitted = capture.take();
    check(occurrences(emitted, "Type /help for commands") == 1, "Plain output did not print the prompt");
    std::string draft;
    for (int key = 0; key < 50; ++key) {
        draft += 'x';
        ui.input(draft); ui.hint("hint-" + std::to_string(key)); ui.render_input();
        check(capture.take().empty(), "Plain output appended output for a keystroke");
    }
    ui.input({}); ui.hint({});
    ui.add(Tone::user, "arn › plain-user!");
    ui.status("Working · Esc or Ctrl+C cancels");
    ui.add(Tone::normal, {});
    ui.render();
    for (const auto chunk : {"chunk-a", "chunk-b", "chunk-c"}) {
        ui.append(chunk); ui.render();
    }
    ui.add(Tone::normal, "PREVIEW_SENTINEL!");
    ui.status("Allow file change? [y/N]");
    for (const auto choice : {"", "y", "", "n"}) ui.show_confirmation(choice);
    ui.finish_confirmation('n');
    ui.status(ui.ready_status()); ui.render();
    emitted += capture.take();
    const auto all = screen.all();
    check(occurrences(all, "ARN AGENT CODE") == 1, "Plain output replayed the startup block");
    check(occurrences(all, "plain-user!") == 1 && occurrences(all, "chunk-achunk-bchunk-c") == 1,
          "Plain output lost, split or replayed transcript text");
    check(occurrences(all, "Working · Esc or Ctrl+C cancels") == 1,
          "Plain output repeated an unchanged prompt");
    const auto preview = all.find("PREVIEW_SENTINEL!");
    const auto prompt = all.find("Allow file change? [y/N]");
    check(preview != std::string::npos && prompt != std::string::npos && preview < prompt,
          "Plain confirmation prompt was missing or preceded its preview");
    check(all.find("Allow file change? [y/N] n") != std::string::npos,
          "Plain confirmation did not record the decision");
    check(occurrences(all, "Type /help for commands") == 2, "Ready prompt did not return after the turn");
    check(occurrences(all, "arn › ") == 1 &&
          all.find(arn::terminal_ui::horizontal_rule(98)) == std::string::npos,
          "Plain output drew a live input area");
    for (const auto movement : {"\x1b[1A", "\x1b[1B", "\x1b[2K"})
        check(emitted.find(movement, emitted.find("Type /help")) == std::string::npos,
              "Plain output moved the cursor or erased a row");
    capture_fails = false;
}

// A console whose region cannot be read back after a footer exists: one
// re-anchor is allowed, then output becomes plain instead of stacking footers.
void capture_failure_falls_back_once() {
    NativeFixture fixture;
    fixture.turns(12);
    fixture.ui.input("draft");
    fixture.ui.render_input(); fixture.draw();
    capture_fails = true;
    fixture.ui.render_input(); fixture.draw();
    for (int key = 0; key < 50; ++key) {
        fixture.ui.input("draft" + std::string(static_cast<std::size_t>(key), 'x'));
        fixture.ui.render_input();
        check(fixture.draw().empty(), "Failed capture still redrew the input area per keystroke");
    }
    fixture.ui.input({});
    fixture.ui.add(Tone::normal, {});
    for (int chunk = 0; chunk < 20; ++chunk) {
        fixture.ui.append("streamed-" + std::to_string(chunk) + ' ');
        fixture.ui.render(); fixture.draw();
    }
    fixture.ui.status("Allow file change? [y/N]");
    fixture.ui.show_confirmation("y"); fixture.draw();
    fixture.ui.finish_confirmation('y');
    fixture.ui.render(); fixture.draw();
    fixture.check_history();
    const auto all = fixture.screen.all();
    const auto rule = arn::terminal_ui::horizontal_rule(98);
    check(occurrences(all, rule) == 1 && occurrences(all, "arn › draft") == 1,
          "Failed capture stacked additional input areas");
    check(occurrences(all, "streamed-19 ") == 1 && occurrences(all, "Allow file change? [y/N]") == 2,
          "Plain fallback lost streamed text or the confirmation prompt");
    capture_fails = false;
}

// Setup failure must not show a y/N prompt without the change preview; the
// preview is flushed first once a region can be established again.
void confirmation_retries_preview_after_failed_setup() {
    NativeFixture fixture;
    fixture.turns(12);
    fixture.ui.add(Tone::normal, "PREVIEW_SENTINEL!");
    fixture.ui.status("Allow file change? [y/N]");
    anchor_available = false;
    fixture.ui.show_confirmation({});
    fixture.ui.show_confirmation("y");
    check(fixture.draw().empty(), "Failed setup wrote unowned cells");
    anchor_available = true;
    fixture.ui.show_confirmation("y"); fixture.draw();
    fixture.check_history();
    const auto all = fixture.screen.all();
    check(occurrences(all, "PREVIEW_SENTINEL!") == 1, "Confirmation lost its change preview");
    check(fixture.screen.line(fixture.screen.y).starts_with("arn › y") &&
          fixture.screen.line(fixture.screen.y + 1).find("[y/N]") != std::string::npos,
          "Recovered confirmation prompt is not usable");
    check(all.find("PREVIEW_SENTINEL!") < all.find("Allow file change? [y/N]"),
          "Confirmation prompt appeared before its preview");
}

// Each frame verifies the region once and certifies it once.
void frames_read_region_twice() {
    NativeFixture fixture;
    fixture.turns(3);
    fixture.ui.add(Tone::normal, {});
    fixture.ui.append("start"); fixture.ui.render(); fixture.draw();
    for (int chunk = 0; chunk < 10; ++chunk) {
        capture_count = 0;
        fixture.ui.append(" chunk"); fixture.ui.render(); fixture.draw();
        check(capture_count == 2, "A streamed chunk read the live region more than twice");
        capture_count = 0;
        fixture.ui.input("x"); fixture.ui.render_input(); fixture.draw();
        check(capture_count == 2, "A keystroke read the live region more than twice");
    }
    fixture.check_history();
}

// read_line handles a host resize record with render(). When geometry did not
// actually change, the existing footer must be updated in place.
void same_geometry_resize_render_keeps_one_footer() {
    NativeFixture fixture;
    fixture.turns(3);
    fixture.ui.input("resize-draft");
    fixture.ui.render_input(); fixture.draw();
    for (int event = 0; event < 20; ++event) {
        fixture.ui.render();
        check(fixture.draw().find("\x1b[30;1H\r\n") == std::string::npos,
              "Unchanged geometry allocated a new output region");
    }
    fixture.check_history();
    check(occurrences(fixture.screen.all(), "arn › resize-draft") == 1,
          "Unchanged geometry left an abandoned footer");
}

}

int main() {
    const std::pair<const char*, void(*)()> cases[] = {
        {"header_matches_existing_layout", header_matches_existing_layout},
        {"startup_is_not_a_redraw_frame", startup_is_not_a_redraw_frame},
        {"streaming_input_and_history", streaming_input_and_history},
        {"model_reflow_preserves_ownership", model_reflow_preserves_ownership},
        {"geometry_redraw_preserves_transcript", geometry_redraw_preserves_transcript},
        {"input_survives_width_and_height_resize", input_survives_width_and_height_resize},
        {"streaming_resize_keeps_committed_prefix", streaming_resize_keeps_committed_prefix},
        {"stale_footer_transcript", +[] { stale_footer_after_host_relocation(Redraw::transcript); }},
        {"stale_footer_input", +[] { stale_footer_after_host_relocation(Redraw::input); }},
        {"stale_footer_confirmation", +[] { stale_footer_after_host_relocation(Redraw::confirmation); }},
        {"stale_footer_finish_confirmation", +[] { stale_footer_after_host_relocation(Redraw::finish_confirmation); }},
        {"anchor_evidence_is_not_just_geometry", anchor_evidence_is_not_just_geometry},
        {"new_live_frame_follows_geometry", new_live_frame_follows_geometry},
        {"unavailable_anchor_retries_without_mutation", unavailable_anchor_retries_without_mutation},
        {"same_geometry_transcript", +[] { same_geometry_host_relocation(Redraw::transcript); }},
        {"same_geometry_input", +[] { same_geometry_host_relocation(Redraw::input); }},
        {"same_geometry_confirmation", +[] { same_geometry_host_relocation(Redraw::confirmation); }},
        {"same_geometry_finish_confirmation", +[] { same_geometry_host_relocation(Redraw::finish_confirmation); }},
        {"inline_probe_reflow_blocks_edit", +[] { inline_probe_requires_current_ownership(false); }},
        {"inline_confirmation_probe_reflow_blocks_edit", +[] { inline_probe_requires_current_ownership(true); }},
        {"redirected_output_is_plain_and_bounded", redirected_output_is_plain_and_bounded},
        {"capture_failure_falls_back_once", capture_failure_falls_back_once},
        {"confirmation_retries_preview_after_failed_setup", confirmation_retries_preview_after_failed_setup},
        {"frames_read_region_twice", frames_read_region_twice},
        {"same_geometry_resize_render_keeps_one_footer", same_geometry_resize_render_keeps_one_footer},
    };
    int failures = 0;
    for (const auto& [name, test] : cases) {
        test_native = true;
        anchor_available = true;
        capture_fails = false;
        test_size = {100, 30};
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    return failures ? 1 : 0;
}
