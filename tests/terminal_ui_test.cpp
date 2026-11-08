#include "terminal_ui.hpp"
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

// A shim terminal session (size and presentation chosen by the test) and a
// model screen that interprets what the renderer writes.
// test_native: plain output (redirected input or output) when true, the
// alternate-screen viewport when false.
namespace {
arn::TerminalSize test_size{100, 30};
bool test_native = true;
void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
// Cells a host gives one UTF-8 scalar: two for CJK and emoji, otherwise one.
int cell_width(std::string_view glyph) {
    char32_t code = static_cast<unsigned char>(glyph[0]);
    if (glyph.size() > 1) {
        code &= 0xFFu >> (glyph.size() + 1);
        for (std::size_t index = 1; index < glyph.size(); ++index)
            code = (code << 6) | (static_cast<unsigned char>(glyph[index]) & 0x3Fu);
    }
    return (code >= 0x1100 && code <= 0x115F) || (code >= 0x2E80 && code <= 0xA4CF) ||
           (code >= 0xAC00 && code <= 0xD7A3) || (code >= 0xF900 && code <= 0xFAFF) ||
           (code >= 0xFF00 && code <= 0xFF60) || (code >= 0x1F300 && code <= 0x1FAFF) ? 2 : 1;
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
                std::string glyph(text.substr(index, count));
                // A wide glyph occupies its cell plus an empty continuation
                // cell, so joined text stays contiguous.
                const int end = x + std::min(cell_width(glyph), width - x);
                auto& cells = rows[y].cells;
                const auto blank = [&](int at) {
                    owned_writes += cells[at].committed != 0;
                    cells[at] = {" ", 0};
                };
                // Overwriting either half of a wide glyph blanks the other half.
                if (cells[x].text.empty() && x > 0) blank(x - 1);
                if (end < width && cells[end].text.empty()) blank(end);
                for (int cell = x; cell < end; ++cell) owned_writes += cells[cell].committed != 0;
                cells[x] = {std::move(glyph), 0};
                if (end - x == 2) cells[x + 1] = {"", 0};
                x = std::min(width - 1, end);
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


namespace {
struct Capture {
    std::ostringstream output;
    std::streambuf* original{std::cout.rdbuf(output.rdbuf())};
    Screen* bound{};
    std::size_t consumed{};
    void bind(Screen& screen) { bound = &screen; }
    void flush_screen() {
        if (!bound) return;
        const auto text = output.str();
        bound->feed(std::string_view(text).substr(consumed));
        consumed = text.size();
    }
    void unbind() {
        flush_screen();
        bound = nullptr;
    }
    ~Capture() { std::cout.rdbuf(original); }
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

// Plain output may colour text, nothing else: no cursor movement, erasing,
// saved positions or screen switches.
bool only_colour_sequences(std::string_view bytes) {
    for (auto at = bytes.find('\x1b'); at != std::string_view::npos; at = bytes.find('\x1b', at + 1)) {
        if (at + 1 >= bytes.size() || bytes[at + 1] != '[') return false;
        auto end = at + 2;
        while (end < bytes.size() && !(bytes[end] >= '@' && bytes[end] <= '~')) ++end;
        if (end >= bytes.size() || bytes[end] != 'm' || bytes[at + 2] == '?') return false;
    }
    return true;
}

// Plain output prints the startup banner as text lines; they match the
// alternate-screen header row for row.
void header_matches_existing_layout() {
    Capture capture;
    arn::TerminalSession terminal;
    for (const auto width : {100, 70, 50}) {
        test_size.width = width;
        test_native = false;
        Ui viewport(terminal);
        viewport.session("Gemini", "fake-model", false);
        viewport.render();
        Screen expected(width);
        expected.feed(capture.take());

        test_native = true;
        Ui plain(terminal);
        plain.session("Gemini", "fake-model", false);
        plain.status(plain.ready_status());
        Screen actual(width);
        capture.bind(actual);
        plain.render();
        const auto output = capture.take();
        const int header = width - 4 < 58 ? 5 : width - 4 < 76 ? 7 : 9;
        for (int row = 0; row < header; ++row)
            check(actual.line(row) == expected.line(row), "The plain banner differs from the viewport header");
        check(actual.line(header).find_first_not_of(' ') == std::string::npos, "The gap below the banner is missing");
        check(actual.line(header + 1).find("Type /help") == 0, "The prompt line does not follow the banner");
        check(actual.all().find("arn › ") == std::string::npos, "Plain output drew an input area");
        check(only_colour_sequences(output), "Plain output used more than colour sequences");
        capture.unbind();
    }
    test_size = {100, 30};
}

// Plain output prints the banner once and every entry once, however often it
// renders, resizes, confirms or clears. The viewport keeps a single header.
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
// Redirected input or output gets plain output: append-only and bounded, no
// live input area, nothing per keystroke.
void redirected_output_is_plain_and_bounded() {
    Capture capture;
    arn::TerminalSession terminal;
    Screen screen;
    capture.bind(screen);
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
    // From the banner on, nothing moves the cursor or erases a row.
    check(only_colour_sequences(emitted), "Plain output moved the cursor or erased a row");
    // /clear cannot erase plain output; later entries simply follow.
    ui.add(Tone::normal, "before-clear!");
    ui.render();
    ui.clear();
    ui.add(Tone::normal, "after-clear!");
    ui.render();
    const auto cleared = capture.take();
    check(only_colour_sequences(cleared) && cleared.find("after-clear!") != std::string::npos &&
              cleared.find("ARN AGENT CODE") == std::string::npos,
          "/clear in plain output erased output or replayed the banner");
    check(occurrences(screen.all(), "before-clear!") == 1, "/clear in plain output removed printed text");
}

// Plain output never moves the cursor, so streamed wide text is written as
// it arrives and never needs to be resumed at a column.
void plain_output_never_moves_cursor() {
    Capture capture;
    arn::TerminalSession terminal;
    Screen screen;
    capture.bind(screen);
    Ui ui(terminal);
    ui.status(ui.ready_status());
    ui.render();
    ui.add(Tone::normal, {});
    for (const auto chunk : {"你好", "世界", "🙂", "Привіт"}) {
        ui.append(chunk); ui.render();
    }
    const auto emitted = capture.take();
    check(only_colour_sequences(emitted), "Plain output moved the cursor");
    check(occurrences(screen.all(), "你好世界🙂Привіт") == 1, "Plain output split wide text");
}

}

// --- POSIX alternate-screen viewport (native_scrollback() == false) -----

std::string trimmed(std::string text) {
    while (!text.empty() && text.back() == ' ') text.pop_back();
    return text;
}

// A POSIX Ui on a model screen without scrollback, like an alternate screen.
// Rows (0-based) for width 100: header 0..8, transcript 10..height-5, rule
// height-4, prompt height-3, hint height-2, the last row empty.
struct ViewportFixture {
    Capture capture;
    arn::TerminalSession terminal;
    Screen screen;
    std::optional<Ui> ui;
    std::string emitted, entered;
    explicit ViewportFixture(int width = 100, int height = 30) : screen(width, height, false) {
        test_native = false;
        test_size = {width, height};
        ui.emplace(terminal);
        entered = capture.take(); // Entering the alternate screen; frames come after.
        screen.feed(entered);
    }
    std::string paint() {
        ui->render();
        auto output = capture.take();
        screen.feed(output);
        emitted += output;
        return output;
    }
    void resize(int width, int height) {
        test_size = {width, height};
        screen.resize(width, height);
    }
    int header() const { const int inner = screen.width - 4; return inner < 58 ? 5 : inner < 76 ? 7 : 9; }
    std::vector<std::string> transcript() const {
        std::vector<std::string> rows;
        for (int row = header() + 1; row <= screen.height - 5; ++row) rows.push_back(trimmed(screen.line(row)));
        return rows;
    }
    std::string row(int index) const { return trimmed(screen.line(index)); }
    std::string prompt() const { return row(screen.height - 3); }
    std::string hint() const { return row(screen.height - 2); }
    // What finish() printed after leaving the alternate screen.
    std::string finish() {
        ui->finish();
        const auto output = capture.take();
        const auto leave = output.find("\x1b[?1049l");
        check(leave != std::string::npos, "finish() did not leave the alternate screen");
        return output.substr(leave + 8);
    }
};

bool valid_utf8(std::string_view text) {
    for (std::size_t index = 0; index < text.size();) {
        const auto scalar = arn::tui::decode(text, index);
        if (scalar.code == 0xFFFD && text.substr(index, 3) != "\xEF\xBF\xBD") return false;
        index += scalar.size;
    }
    return true;
}

void viewport_lifecycle_prints_once() {
    {
        // Windows' native Ui neither switches screens nor prints on exit.
        Capture capture;
        arn::TerminalSession terminal;
        test_native = true;
        { Ui native(terminal); native.add(Tone::normal, "native-entry!"); }
        check(capture.take().empty(), "Native Ui emitted output on construction or destruction");
    }
    std::string printed;
    {
        ViewportFixture fixture;
        check(occurrences(fixture.entered, "\x1b[?1049h") == 1, "Viewport did not enter the alternate screen once");
        fixture.ui->session("Gemini", "fake-model", true);
        fixture.ui->add(Tone::user, "arn › first question");
        fixture.ui->add(Tone::normal, {});
        fixture.ui->append("first answer\nsecond line");
        fixture.ui->add(Tone::muted, "Done.");
        fixture.ui->status("Arnie is working… Esc or Ctrl+C cancels");
        fixture.ui->hint("secret-hint-text");
        fixture.ui->input("*****");
        fixture.paint();
        printed = fixture.finish();
        fixture.ui->finish();
        fixture.ui->render();
        fixture.ui->render_input();
        check(fixture.capture.take().empty(), "Output after finish()");
        fixture.ui.reset();
        check(fixture.capture.take().empty(), "Destruction after finish() printed again");
    }
    for (const auto token : {"arn › first question", "first answer", "second line", "Done."})
        check(occurrences(printed, token) == 1, "Transcript entry not printed exactly once");
    check(printed.find("first answer") > printed.find("first question") &&
              printed.find("Done.") > printed.find("second line"),
          "Transcript order differs from the conversation");
    for (const auto absent : {"ARN AGENT CODE", "Arnie is working", "secret-hint-text", "*****", "Type /help", "─"})
        check(printed.find(absent) == std::string::npos, "Transcript contains frame, status, hint or draft text");
    check(printed.find("\x1b[?") == std::string::npos && printed.find('\r') == std::string::npos,
          "Transcript must be plain lines for scrollback");

    // Destruction alone (any exit path that unwinds) prints once too.
    {
        Capture capture;
        arn::TerminalSession terminal;
        test_native = false;
        { Ui ui(terminal); ui.add(Tone::normal, "only-on-destruction!"); }
        const auto output = capture.take();
        check(occurrences(output, "\x1b[?1049l") == 1 && occurrences(output, "only-on-destruction!") == 1,
              "Destruction did not leave the viewport and print once");
    }
}

void viewport_exception_unwinds_once() {
    Capture capture;
    arn::TerminalSession terminal;
    test_native = false;
    try {
        Ui ui(terminal);
        ui.add(Tone::normal, "before-the-error!");
        ui.render();
        throw std::runtime_error("simulated failure");
    } catch (const std::runtime_error&) {
    }
    const auto output = capture.take();
    const auto leave = output.find("\x1b[?1049l");
    check(leave != std::string::npos && occurrences(output, "\x1b[?1049l") == 1,
          "An exception did not leave the alternate screen exactly once");
    check(occurrences(output.substr(leave), "before-the-error!") == 1, "An exception lost the transcript");
}

void viewport_keeps_six_tones() {
    ViewportFixture fixture;
    const std::pair<Tone, std::string_view> tones[] = {
        {Tone::normal, arn::terminal_ui::reset}, {Tone::user, arn::terminal_ui::cyan},
        {Tone::muted, arn::terminal_ui::dim}, {Tone::good, arn::terminal_ui::green},
        {Tone::warning, arn::terminal_ui::amber}, {Tone::error, arn::terminal_ui::red}};
    int index = 0;
    for (const auto& [tone, code] : tones) fixture.ui->add(tone, "tone-text-" + std::to_string(index++) + "!");
    const auto frame = fixture.paint();
    index = 0;
    for (const auto& [tone, code] : tones) {
        const auto text = "tone-text-" + std::to_string(index++) + "!";
        check(frame.find(std::string(code) + text) != std::string::npos, "A frame row lost its tone");
        check(occurrences(frame, text) == 1, "A frame painted an entry twice");
    }
    const auto printed = fixture.finish();
    index = 0;
    for (const auto& [tone, code] : tones) {
        const auto text = "tone-text-" + std::to_string(index++) + "!";
        check(printed.find(std::string(code) + ' ' + text) != std::string::npos, "The transcript lost a tone");
    }
}

void viewport_streaming_holds_split_utf8() {
    ViewportFixture fixture;
    fixture.ui->add(Tone::user, "arn › hi");
    fixture.ui->add(Tone::normal, {});
    const std::string reply = "Привіт 你好 🙂 ok";
    for (std::size_t byte = 0; byte < reply.size(); ++byte) {
        fixture.ui->append(reply.substr(byte, 1));
        const auto frame = fixture.paint();
        check(valid_utf8(frame), "A frame showed a split UTF-8 character");
        check(frame.find('\n') == std::string::npos && frame.find('\r') == std::string::npos,
              "A viewport frame contains a line break");
        const auto line = fixture.row(fixture.header() + 2);
        check(line.empty() || reply.starts_with(line.substr(1)),
              "Streaming showed text that is not a prefix of the reply");
    }
    check(fixture.row(fixture.header() + 2) == " " + reply, "The streamed reply is incomplete");
    check(fixture.screen.history.empty(), "Streaming frames reached scrollback");
    check(occurrences(fixture.finish(), reply) == 1, "The streamed reply was not printed once");
}

void viewport_pinned_scrolling() {
    ViewportFixture fixture;
    for (int line = 0; line < 60; ++line) fixture.ui->add(Tone::normal, "line-" + std::to_string(line) + "!");
    fixture.paint();
    const auto newest = fixture.transcript();
    check(newest.back() == " line-59!", "The view does not follow the newest output");

    fixture.ui->scroll(8); // PgUp
    fixture.paint();
    const auto pinned = fixture.transcript();
    check(pinned != newest && pinned.back() == " line-51!", "PgUp did not move eight rows up");
    check(fixture.hint().starts_with("Scrolled up"), "The hint does not say the view is scrolled");

    // A reply streams, tools report and a result arrives: the view stays.
    fixture.ui->add(Tone::normal, {});
    for (int chunk = 0; chunk < 40; ++chunk) fixture.ui->append("more-" + std::to_string(chunk) + "\n");
    fixture.ui->add(Tone::muted, "• read_file");
    fixture.ui->add(Tone::warning, "Request cancelled.");
    fixture.paint();
    check(fixture.transcript() == pinned, "New output moved the rows being read");

    // PgUp past the start stops at the first row; PgDn returns and follows.
    for (int page = 0; page < 50; ++page) fixture.ui->scroll(8);
    fixture.paint();
    check(fixture.transcript().front() == " line-0!", "Scrolling up should stop at the first row");
    for (int page = 0; page < 50; ++page) fixture.ui->scroll(-8);
    fixture.paint();
    check(fixture.transcript().back() == " Request cancelled.", "PgDn did not return to the newest rows");
    check(!fixture.hint().starts_with("Scrolled up"), "Returning to the bottom kept the scrolled hint");

    // End, and a message the user sends, return to the newest output.
    fixture.ui->scroll(3);
    fixture.ui->scroll_to_bottom();
    fixture.paint();
    check(fixture.transcript().back() == " Request cancelled.", "End did not return to the newest rows");
    fixture.ui->scroll(16);
    fixture.ui->add(Tone::user, "arn › next question");
    fixture.paint();
    check(fixture.transcript().back() == " arn › next question", "Sending a message did not follow the newest output");
}

void viewport_resize_keeps_layout() {
    ViewportFixture fixture;
    for (int line = 0; line < 120; ++line) fixture.ui->add(Tone::normal, "row-" + std::to_string(line) + "!");
    fixture.ui->input("draft 你好");
    fixture.paint();
    // Far enough up that the largest size below still shows the same top.
    fixture.ui->scroll(40);
    fixture.paint();
    const auto top = fixture.transcript().front();
    for (const auto& [width, height] : {std::pair{70, 24}, std::pair{120, 40}, std::pair{50, 20},
                                        std::pair{38, 13}, std::pair{100, 30}}) {
        fixture.resize(width, height);
        fixture.paint();
        const auto label = std::to_string(width) + "x" + std::to_string(height);
        check(occurrences(fixture.screen.all(), "ARN AGENT CODE") == 1, ("Header not shown once at " + label).c_str());
        check(occurrences(fixture.screen.all(), "arn › ") == 1, ("Prompt not shown once at " + label).c_str());
        check(fixture.prompt() == "arn › draft 你好", ("Prompt row moved or lost the draft at " + label).c_str());
        check(fixture.row(height - 4).starts_with("─"), ("Rule row moved at " + label).c_str());
        check(fixture.hint().starts_with("Scrolled up"), ("Hint row moved at " + label).c_str());
        check(fixture.row(height - 1).empty(), ("The bottom row is no longer empty at " + label).c_str());
        check(fixture.transcript().front() == top, ("Resize lost the reading position at " + label).c_str());
        check(fixture.screen.history.empty(), ("A resize frame reached scrollback at " + label).c_str());
    }
    fixture.resize(30, 10);
    const auto tiny = fixture.paint();
    check(tiny.find("enlarge terminal") != std::string::npos, "A tiny terminal must ask to be enlarged");
    fixture.ui->scroll(8); // Ignored while nothing can be shown.
    fixture.resize(100, 30);
    fixture.paint();
    check(fixture.transcript().front() == top, "A tiny terminal changed the reading position");
}

void viewport_confirmation_rows() {
    ViewportFixture fixture;
    fixture.ui->add(Tone::normal, "before the question");
    fixture.ui->status("ACP tool: delete important file [y/N]");
    fixture.ui->show_confirmation({});
    fixture.screen.feed(fixture.capture.take());
    check(fixture.hint() == "ACP tool: delete important file [y/N]", "The question is not in the status row");
    check(fixture.prompt() == "arn ›", "An empty choice must leave the prompt empty");
    fixture.ui->show_confirmation("y");
    fixture.screen.feed(fixture.capture.take());
    check(fixture.prompt() == "arn › y", "The choice is not in the prompt row");
    fixture.ui->finish_confirmation('y');
    // The agent's reply continues after the answer, on its own row.
    fixture.ui->append("reply after the answer");
    fixture.ui->add(Tone::muted, "• read_file");
    fixture.ui->append("text after the tool");
    fixture.ui->status(fixture.ui->ready_status());
    fixture.paint();
    const auto rows = fixture.transcript();
    check(rows[1] == " ACP tool: delete important file [y/N] y", "The answer was not recorded");
    check(rows[2] == " reply after the answer" && rows[3] == " • read_file" && rows[4] == " text after the tool",
          "Streamed text ran on into a recorded answer or a tool notice");
    check(fixture.prompt() == "arn ›" && fixture.hint() == fixture.ui->ready_status(), "Footer not restored");
    const auto printed = fixture.finish();
    check(printed.find("[y/N] y" + std::string(arn::terminal_ui::reset) + "\n") != std::string::npos &&
              printed.find("• read_file" + std::string(arn::terminal_ui::reset) + "\n") != std::string::npos,
          "The transcript joined streamed text to an answer or a notice");
}

void viewport_cap_keeps_view_and_transcript() {
    ViewportFixture fixture;
    for (int entry = 0; entry < 170; ++entry) fixture.ui->add(Tone::normal, "e-" + std::to_string(entry) + "!");
    fixture.paint();
    // Pin to e-50, then let the cap drop the 20 oldest entries.
    fixture.ui->scroll(1000);
    fixture.ui->scroll(-50);
    fixture.paint();
    check(fixture.transcript().front() == " e-50!", "Fixture did not pin to e-50");
    for (int entry = 170; entry < 185; ++entry) fixture.ui->add(Tone::normal, "e-" + std::to_string(entry) + "!");
    fixture.paint();
    check(fixture.transcript().front() == " e-50!", "Dropping old entries moved the rows being read");
    // A view pinned to dropped text moves to the oldest remaining row.
    fixture.ui->scroll(1000);
    fixture.paint();
    check(fixture.transcript().front() == " e-20!", "The oldest retained entry is not e-20");
    for (int entry = 185; entry < 400; ++entry) fixture.ui->add(Tone::muted, "e-" + std::to_string(entry) + "!");
    fixture.paint();
    check(fixture.transcript().front().starts_with(" e-") && fixture.hint().starts_with("Scrolled up"),
          "A dropped pinned position did not fall back to the oldest row");
    const auto oldest = fixture.transcript().front();
    const auto printed = fixture.finish();
    check(occurrences(printed, "e-399!") == 1 && printed.find("e-0!") == std::string::npos &&
              occurrences(printed, oldest.substr(1)) == 1,
          "The transcript must hold the retained entries, once each");
    std::size_t lines = 0;
    for (const char value : printed) lines += value == '\n';
    check(lines > 160 && lines <= 180, "The 180-entry cap no longer bounds the transcript");
}

void viewport_strips_controls() {
    ViewportFixture fixture;
    fixture.ui->add(Tone::normal, {});
    fixture.ui->append("safe\x1b[2Jtext\xC2\x9B" "more\x07" "end\r!");
    const auto frame = fixture.paint();
    check(frame.find("\x1b[2J") == std::string::npos && frame.find("\xC2\x9B") == std::string::npos &&
              frame.find('\x07') == std::string::npos,
          "A frame passed control characters from the conversation to the terminal");
    check(fixture.row(fixture.header() + 1) == " safe[2Jtextmoreend!", "Visible text around controls changed");
    const auto printed = fixture.finish();
    check(printed.find("safe[2Jtextmoreend!") != std::string::npos && printed.find("\x1b[2J") == std::string::npos &&
              printed.find("\xC2\x9B") == std::string::npos && printed.find('\x07') == std::string::npos,
          "The transcript passed control characters to the terminal");
}

int main() {
    const std::pair<const char*, void(*)()> cases[] = {
        {"viewport_lifecycle_prints_once", viewport_lifecycle_prints_once},
        {"viewport_exception_unwinds_once", viewport_exception_unwinds_once},
        {"viewport_keeps_six_tones", viewport_keeps_six_tones},
        {"viewport_streaming_holds_split_utf8", viewport_streaming_holds_split_utf8},
        {"viewport_pinned_scrolling", viewport_pinned_scrolling},
        {"viewport_resize_keeps_layout", viewport_resize_keeps_layout},
        {"viewport_confirmation_rows", viewport_confirmation_rows},
        {"viewport_cap_keeps_view_and_transcript", viewport_cap_keeps_view_and_transcript},
        {"viewport_strips_controls", viewport_strips_controls},
        {"header_matches_existing_layout", header_matches_existing_layout},
        {"startup_is_not_a_redraw_frame", startup_is_not_a_redraw_frame},
        {"redirected_output_is_plain_and_bounded", redirected_output_is_plain_and_bounded},
        {"plain_output_never_moves_cursor", plain_output_never_moves_cursor},
    };
    int failures = 0;
    for (const auto& [name, test] : cases) {
        test_native = true;
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
