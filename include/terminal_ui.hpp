#pragma once

#include "terminal.hpp"
#include "tui/frame.hpp"
#include "tui/model.hpp"
#include "tui/text.hpp"
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace arn::terminal_ui {

constexpr std::string_view reset = "\x1b[0m", cyan = "\x1b[38;5;81m", amber = "\x1b[38;5;215m";
constexpr std::string_view orange = "\x1b[38;5;208m", green = "\x1b[38;5;114m";
constexpr std::string_view dim = "\x1b[90m", red = "\x1b[38;5;203m";
#if defined(__APPLE__)
constexpr std::string_view copy_shortcut = "Command+C";
#elif defined(_WIN32)
constexpr std::string_view copy_shortcut = "Ctrl+C";
#else
constexpr std::string_view copy_shortcut = "Ctrl+Shift+C";
#endif

inline std::size_t char_bytes(unsigned char byte) {
    if ((byte & 0x80) == 0) return 1;
    if ((byte & 0xE0) == 0xC0) return 2;
    if ((byte & 0xF0) == 0xE0) return 3;
    if ((byte & 0xF8) == 0xF0) return 4;
    return 1;
}

// Plain output writes text ARN does not control (model replies, tool
// summaries, provider and model names) to a pipe, a file or the console, so
// it checks every scalar before writing it. UTF-8 is strict here: overlong
// forms, surrogates and values above U+10FFFF are malformed, like invalid or
// truncated bytes, and each malformed byte decodes as U+FFFD. tui::decode,
// which the viewport uses, stays lenient.
inline tui::Scalar plain_scalar(std::string_view text, std::size_t index) {
    const auto scalar = tui::decode(text, index);
    const char32_t shortest = scalar.size == 2 ? 0x80 : scalar.size == 3 ? 0x800 : 0x10000;
    if (scalar.size > 1 && (scalar.code < shortest || scalar.code > 0x10FFFF ||
                            (scalar.code >= 0xD800 && scalar.code <= 0xDFFF)))
        return {0xFFFD, 1};
    return scalar;
}

// One line of plain text (the prompt line, a provider or model name): valid
// UTF-8 byte for byte, U+FFFD for malformed bytes, and no control characters
// at all (C0 including CR, LF and tab, DEL, C1), so no escape sequence or
// line break survives.
inline std::string plain_line(std::string_view text) {
    std::string out;
    for (std::size_t index = 0; index < text.size();) {
        const auto scalar = plain_scalar(text, index);
        if (scalar.code == 0xFFFD) out += "\xEF\xBF\xBD";
        else if (!tui::is_control(scalar.code)) out.append(text.substr(index, scalar.size));
        index += scalar.size;
    }
    return out;
}

inline std::string horizontal_rule(std::size_t cells) {
    std::string result;
    result.reserve(cells * 3);
    for (std::size_t index = 0; index < cells; ++index) result += "─";
    return result;
}

enum class Tone { normal, user, muted, good, warning, error };
struct Entry { Tone tone; std::string text; };

// Two presentations share this facade, chosen by the terminal session's
// native_scrollback():
//  - The viewport (an interactive terminal: POSIX, and Windows when input and
//    output are a console) owns an alternate screen. Every frame is composed
//    from the arn::tui model, and finish() leaves it and prints the
//    conversation once.
//  - Plain output (Windows with redirected input or output) only appends:
//    the startup banner once, new transcript text once, and the prompt line
//    when it changes. It never moves the cursor or edits earlier output, and
//    text it does not control passes plain_scalar() and drops control
//    characters first (see append_native_text and plain_line).
class Ui {
public:
    explicit Ui(const arn::TerminalSession& terminal) : terminal_(terminal) {
        if (terminal_.native_scrollback()) return;
        viewport_ = true;
        std::cout << "\x1b[?1049h\x1b[2J\x1b[H" << std::flush;
    }
    // Runs on every exit path that unwinds: /exit, end of input, a handled
    // signal, an exception.
    ~Ui() {
        try { finish(); } catch (...) {}
    }
    Ui(const Ui&) = delete;
    Ui& operator=(const Ui&) = delete;

    void add(Tone tone, std::string text) {
        log_.push_back({tone, std::move(text)});
        if (viewport_) {
            // Every entry is an open "reply" in the model, so append() can
            // continue it and a split UTF-8 character waits for its tail.
            transcript_.begin_reply();
            transcript_.stream(log_.back().text);
            // A message the user sends returns the view to the newest output;
            // tool notices and results never move what is being read.
            if (tone == Tone::user) view_ = {};
        }
        if (log_.size() > 180) {
            log_.erase(log_.begin(), log_.begin() + 20);
            if (printed_entries_ >= 20) printed_entries_ -= 20;
            else printed_entries_ = printed_bytes_ = 0;
            if (viewport_) rebuild_transcript(20);
        }
    }
    void append(std::string_view text) {
        // Streamed text continues a reply. In the viewport, text arriving
        // after a tool notice, a recorded answer or a result starts a new
        // reply entry instead of running on in that entry.
        if (log_.empty() || (viewport_ && log_.back().tone != Tone::normal)) add(Tone::normal, {});
        log_.back().text.append(text);
        if (viewport_) transcript_.stream(text);
    }
    void clear() {
        log_.clear();
        printed_entries_ = printed_bytes_ = 0;
        if (viewport_) {
            transcript_ = tui::Conversation(tui::Decoration::none);
            view_ = {};
        }
        if (terminal_.native_scrollback()) {
            // Plain output cannot erase what it printed: later entries are
            // appended below, and the startup banner is never repeated.
            if (stream_line_open_) std::cout << "\r\n" << std::flush;
            stream_line_open_ = false;
            native_line_columns_ = 0;
            native_plain_prompt_.clear();
        }
    }
    void input(std::string value) { input_ = std::move(value); }
    void hint(std::string value) { hint_ = std::move(value); }
    void status(std::string value) { status_ = std::move(value); }
    void copy_mode(bool enabled) { copy_mode_ = enabled; }
    void session(const std::string& provider, const std::string& model, bool context) {
        provider_ = provider; model_ = model; context_ = context;
    }

    // Positive amounts show older rows. The view stays pinned to the text at
    // its top while output arrives; reaching the newest rows follows again.
    void scroll(int amount) {
        if (terminal_.native_scrollback()) return;
        const auto [width, height] = dimensions();
        if (width < 38 || height < 13) return;
        view_ = tui::scrolled(transcript_, viewport_input(width, height), -static_cast<long long>(amount));
    }

    void scroll_to_bottom() { view_ = {}; }

    void render() const {
        if (terminal_.native_scrollback()) { render_transcript(); return; }
        if (finished_) return;
        const auto [width, height] = dimensions();
        if (width < 38 || height < 13) {
            std::cout << "\x1b[?2026h\x1b[?25l\x1b[2J\x1b[H" << cyan << "ARN" << reset
                      << " — enlarge terminal\x1b[?25h\x1b[?2026l" << std::flush;
            return;
        }
        // The whole frame is composed first and written at once, so a host
        // without synchronized output (classic conhost) never shows it half
        // drawn. Each row is erased right before it is redrawn in place:
        // nothing scrolls, so no frame can reach scrollback, and no earlier
        // frame can remain.
        BufferedFrame frame;
        std::cout << "\x1b[?2026h\x1b[?25l\x1b[?7l";
        const int left = 2, inner = width - 4, header = inner < 58 ? 5 : inner < 76 ? 7 : 9;
        for (int row = 1; row <= header + 1; ++row) clear_line(row);
        header_box(left, inner, header);
        const auto layout = tui::locate(transcript_, viewport_input(width, height));
        // Rows start below the header, as before; only a full view scrolls.
        const auto shown = std::min(layout.body, layout.rows.size() - layout.top);
        for (std::size_t index = 0; index < layout.body; ++index) {
            const int row_number = header + 2 + static_cast<int>(index);
            clear_line(row_number);
            if (index >= shown) continue;
            const auto& row = layout.rows[layout.top + index];
            const auto tone = row.start.entry < log_.size() ? log_[row.start.entry].tone : Tone::normal;
            at(row_number, left, color(tone) + row.text + std::string(reset));
        }
        draw_viewport_footer(width, height, true);
        clear_line(height); // The bottom row stays empty.
        std::cout << "\x1b[?7h\x1b[?25h\x1b[?2026l";
        frame.write();
    }

    void render_input() const {
        // Plain output cannot edit a line in place; it prints prompts on full
        // renders only, so keystrokes never append output.
        if (terminal_.native_scrollback()) return;
        if (finished_) return;
        const auto [width, height] = dimensions();
        if (width < 38 || height < 13) { render(); return; }
        BufferedFrame frame;
        std::cout << "\x1b[?2026h\x1b[?25l\x1b[?7l";
        draw_viewport_footer(width, height, false);
        std::cout << "\x1b[?7h\x1b[?25h\x1b[?2026l";
        frame.write();
    }

    // Leaves the POSIX viewport and prints the conversation into normal
    // scrollback, once. Later calls, and Windows, do nothing.
    void finish() {
        if (!viewport_ || finished_) return;
        finished_ = true;
        std::cout << reset << "\x1b[?2026l\x1b[?7h\x1b[?25h\x1b[?1049l" << transcript_text() << std::flush;
    }

    void show_confirmation(std::string_view choice) {
        input(std::string(choice));
        if (!terminal_.native_scrollback()) {
            render();
            return;
        }
        if (!confirmation_visible_) {
            // Print the transcript (the change preview) before the question:
            // the y/N prompt never appears without what it asks about.
            render_transcript(false);
            confirmation_visible_ = true;
        }
        print_plain_prompt();
    }

    void finish_confirmation(int answer) {
        add(Tone::muted, status_ + (answer == 'y' ? " y" : answer == 'n' ? " n" : ""));
        confirmation_visible_ = false;
        input({});
    }

    [[nodiscard]] std::string ready_status() const {
#ifdef _WIN32
        // Windows selects text natively (no mouse capture): no F2 copy mode.
        return "Type /help for commands";
#else
        return terminal_.native_scrollback() ? "Type /help for commands"
            : "Type /help for commands · F2 copy mode";
#endif
    }

private:
    // --- Plain output (redirected input or output) -------------------------

    // `plain`: the names pass plain_line() (the plain banner). The viewport
    // header keeps them as they are.
    std::string session_text(bool plain = false) const {
        const auto name = [&](const std::string& value) { return plain ? plain_line(value) : value; };
        return std::string(dim) + "Provider: " + name(provider_) + " · Model: " +
            (model_.empty() ? "not selected" : name(model_)) + " · Context: " +
            (context_ ? "active" : "empty") + std::string(reset);
    }

    // The startup banner as text lines: draw_header() paints into cells
    // instead of positioning the cursor, so plain output stays append-only.
    // Each cell keeps the colour codes that precede its glyph.
    std::vector<std::string> header_lines(int left, int inner, int height) const {
        std::vector<std::vector<std::string>> cells(
            static_cast<std::size_t>(height), std::vector<std::string>(static_cast<std::size_t>(left - 1 + inner), " "));
        const auto paint = [&](int row, int column, const std::string& text) {
            auto& line = cells[static_cast<std::size_t>(row - 1)];
            std::string pending; // Escape sequences waiting for the next glyph.
            auto cell = static_cast<std::size_t>(column - 1);
            std::size_t last = line.size();
            for (std::size_t index = 0; index < text.size();) {
                if (text[index] == '\x1b' && index + 1 < text.size() && text[index + 1] == '[') {
                    const auto begin = index;
                    index += 2;
                    while (index < text.size() && !(text[index] >= '@' && text[index] <= '~')) ++index;
                    index = std::min(index + 1, text.size());
                    pending.append(text, begin, index - begin);
                    continue;
                }
                const auto count = std::min(char_bytes(static_cast<unsigned char>(text[index])), text.size() - index);
                if (cell < line.size()) {
                    line[cell] = pending + text.substr(index, count);
                    pending.clear();
                    last = cell++;
                }
                index += count;
            }
            if (!pending.empty() && last < line.size()) line[last] += pending;
        };
        draw_header(left, inner, height, paint, true);
        std::vector<std::string> lines;
        for (auto& row : cells) {
            while (!row.empty() && row.back() == " ") row.pop_back();
            std::string line;
            for (const auto& cell : row) line += cell;
            lines.push_back(std::move(line));
        }
        return lines;
    }

    void emit_plain_startup() const {
        if (native_startup_emitted_) return;
        native_startup_emitted_ = true;
        const auto [width, height] = dimensions();
        if (width < 38 || height < 13) {
            std::cout << cyan << "ARN AGENT CODE" << reset << " — enlarge terminal\r\n";
            return;
        }
        const int inner = width - 4, header = inner < 58 ? 5 : inner < 76 ? 7 : 9;
        for (const auto& line : header_lines(2, inner, header)) std::cout << line << "\r\n";
        std::cout << "\r\n"; // The gap below the banner, as on the alternate screen.
    }

    // The prompt line, printed only when its text changes. A status can quote
    // a model's request, so the line passes plain_line().
    void print_plain_prompt() const {
        const auto text = current_hint();
        if (text == native_plain_prompt_) return;
        native_plain_prompt_ = text;
        if (stream_line_open_) std::cout << "\r\n";
        std::cout << dim << plain_line(text) << reset << "\r\n" << std::flush;
        stream_line_open_ = false;
        native_line_columns_ = 0;
    }

    // Writes conversation text: valid UTF-8 byte for byte, U+FFFD for each
    // malformed byte (plain_scalar), LF as a new line with the margin, tab
    // as it is; every other control character (CR, ESC and the rest of C0,
    // DEL, C1) is dropped, so stored text cannot drive the terminal. The
    // characters after a dropped ESC stay visible, as in the viewport.
    // Known limitation: wrapping counts one cell per character, so wide (CJK,
    // emoji) text can wrap later than the terminal would wrap it.
    void append_native_text(std::string_view text, Tone tone) const {
        const auto width = static_cast<std::size_t>(std::max(1, terminal_.size().width - 6));
        std::cout << color(tone);
        for (std::size_t index = 0; index < text.size();) {
            const auto scalar = plain_scalar(text, index);
            const auto bytes = text.substr(index, scalar.size);
            index += scalar.size;
            if (scalar.code == '\n') {
                std::cout << "\r\n";
                native_line_columns_ = 0;
                stream_line_open_ = false;
                continue;
            }
            if (scalar.code != '\t' && tui::is_control(scalar.code)) continue;
            if (native_line_columns_ >= width) {
                std::cout << "\r\n";
                native_line_columns_ = 0;
                stream_line_open_ = false;
            }
            if (!stream_line_open_) std::cout << ' '; // Same conversation margin.
            if (scalar.code == 0xFFFD) std::cout << "\xEF\xBF\xBD";
            else std::cout << bytes;
            ++native_line_columns_;
            stream_line_open_ = true;
        }
        std::cout << reset;
    }

    // Appends what the conversation gained since the last call, then the
    // prompt line when `show_prompt` is set and its text changed.
    void render_transcript(bool show_prompt = true) const {
        emit_plain_startup();
        for (std::size_t index = printed_entries_; index < log_.size(); ++index) {
            const auto& entry = log_[index];
            const auto offset = index == printed_entries_ ? printed_bytes_ : 0;
            if (index != printed_entries_ && stream_line_open_) {
                std::cout << "\r\n";
                native_line_columns_ = 0;
                stream_line_open_ = false;
            }
            // Streaming appends to the last entry, so it can end inside a
            // UTF-8 character that the next chunk completes: that tail waits.
            // An earlier entry is complete; an incomplete tail there is
            // malformed and becomes U+FFFD.
            const bool open = index + 1 == log_.size();
            auto rest = std::string_view(entry.text).substr(std::min(offset, entry.text.size()));
            if (open) rest = rest.substr(0, tui::complete_prefix(rest));
            if (!rest.empty()) append_native_text(rest, entry.tone);
            if (open) {
                printed_entries_ = index;
                printed_bytes_ = offset + rest.size();
            }
        }
        if (show_prompt) print_plain_prompt();
        std::cout << std::flush;
    }

    // --- Shared ------------------------------------------------------------

    [[nodiscard]] std::pair<int, int> dimensions() const {
        const auto dimensions = terminal_.size();
        return {dimensions.width, dimensions.height};
    }
    // render() clears the screen once before drawing. Clearing each individual
    // cell would erase content that was drawn earlier on the same row.
    static void at(int row, int column, const std::string& text) { std::cout << "\x1b[" << row << ';' << column << "H" << text; }
    static void clear_line(int row) { std::cout << "\x1b[" << row << ";1H\x1b[2K"; }
    std::string current_hint() const {
        if (copy_mode_) return "COPY MODE · Drag to select, then " + std::string(copy_shortcut) + " · F2 returns to scrolling";
#ifdef _WIN32
        // The Windows alternate screen does not read the mouse wheel.
        if (!view_.follow) return "Scrolled up · PgDn or End returns to the latest message";
#else
        if (!view_.follow) return "Scrolled up · Wheel down or End returns to the latest message · Shift+drag selects text";
#endif
        auto text = confirmation_visible_ || hint_.empty() ? status_ : hint_;
        if (terminal_.native_scrollback()) {
            // Plain output has a single prompt line: status and hint share it.
            if (!confirmation_visible_ && !hint_.empty() && status_ != ready_status())
                text = status_ + " · " + hint_;
            if (const auto copy = text.find(" · F2 copy mode"); copy != std::string::npos)
                text.erase(copy);
        }
#ifdef _WIN32
        else if (const auto copy = text.find(" · F2 copy mode"); copy != std::string::npos) {
            text.erase(copy);
        }
#endif
        return text;
    }
    // Collects what is written to std::cout while a frame is composed, then
    // writes it in one piece. Destroyed without write() (an exception while
    // composing), the partial frame is dropped.
    struct BufferedFrame {
        std::ostringstream text;
        std::streambuf* previous{std::cout.rdbuf(text.rdbuf())};
        BufferedFrame() = default;
        BufferedFrame(const BufferedFrame&) = delete;
        BufferedFrame& operator=(const BufferedFrame&) = delete;
        ~BufferedFrame() { std::cout.rdbuf(previous); }
        void write() {
            std::cout.rdbuf(previous);
            const auto bytes = text.str();
            std::cout.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            std::cout.flush();
        }
    };

    // The transcript area of the POSIX viewport, for the tui core: rows
    // header+2 .. height-4 at the old wrap width (inner - 2 cells), followed
    // by the three footer rows. The bottom row stays empty, as before.
    tui::FrameInput viewport_input(int width, int height) const {
        const int inner = width - 4, header = inner < 58 ? 5 : inner < 76 ? 7 : 9;
        tui::FrameInput input;
        input.width = inner - 1;            // Layout width inner - 2.
        input.height = height - header - 2; // Transcript rows plus three.
        input.view = view_;
        return input;
    }

    // Rule, hint and prompt rows. The prompt row is written last, so the
    // cursor rests right after the draft whatever the width of its glyphs.
    void draw_viewport_footer(int width, int height, bool rule) const {
        const int footer = height - 3;
        if (rule) {
            clear_line(footer);
            std::cout << dim << horizontal_rule(static_cast<std::size_t>(width - 2)) << reset;
        }
        clear_line(footer + 2);
        std::cout << dim << tui::fit_cells(current_hint(), width - 2) << reset;
        clear_line(footer + 1);
        std::cout << cyan << "arn" << amber << " › " << reset << tui::tail_cells(input_, width - 8);
    }

    // The model mirrors log_ entry for entry. After the cap drops the oldest
    // entries it is rebuilt from what remains, and a pinned view moves with
    // its text (or to the oldest row, if its text was dropped).
    void rebuild_transcript(std::size_t dropped) {
        transcript_ = tui::Conversation(tui::Decoration::none);
        for (const auto& entry : log_) {
            transcript_.begin_reply();
            transcript_.stream(entry.text);
        }
        if (view_.follow) return;
        if (view_.top.entry >= dropped) view_.top.entry -= dropped;
        else view_.top = {};
    }

    // The conversation for normal scrollback: one line per logical line,
    // left to the terminal to wrap. Control characters are dropped and
    // invalid UTF-8 becomes U+FFFD, so stored text cannot drive the terminal.
    std::string transcript_text() const {
        std::string out;
        for (const auto& entry : log_) {
            out += color(entry.tone);
            out += ' ';
            const std::string_view text = entry.text;
            for (std::size_t index = 0; index < text.size();) {
                const auto scalar = tui::decode(text, index);
                if (scalar.code == '\n') out += std::string(reset) + "\n" + color(entry.tone) + ' ';
                else if (scalar.code == '\t') out += '\t';
                else if (scalar.code == 0xFFFD) out += "\xEF\xBF\xBD";
                else if (!tui::is_control(scalar.code)) out.append(text.substr(index, scalar.size));
                index += scalar.size;
            }
            out += std::string(reset) + "\n";
        }
        return out;
    }

    static std::string color(Tone tone) {
        switch (tone) {
        case Tone::user: return std::string(cyan);
        case Tone::muted: return std::string(dim);
        case Tone::good: return std::string(green);
        case Tone::warning: return std::string(amber);
        case Tone::error: return std::string(red);
        default: return std::string(reset);
        }
    }
    void header_box(int left, int width, int height) const {
        draw_header(left, width, height, at);
    }

    // `plain`: drawn for the plain banner (see session_text).
    template<class Draw>
    void draw_header(int left, int width, int height, Draw&& at, bool plain = false) const {
        const std::string rule = horizontal_rule(static_cast<std::size_t>(width - 2));
        at(1, left, std::string(cyan) + "╭" + rule + "╮" + std::string(reset));
        for (int row = 2; row < height; ++row) { at(row, left, std::string(cyan) + "│" + std::string(reset)); at(row, left + width - 1, std::string(cyan) + "│" + std::string(reset)); }
        at(height, left, std::string(cyan) + "╰" + rule + "╯" + std::string(reset));
        at(2, left + 2, std::string(cyan) + "ARN AGENT CODE" + std::string(reset));
        if (height == 5) {
            at(3, left + 2, std::string(amber) + "Arny · native coding companion" + std::string(reset));
        } else if (height == 7) {
            at(3, left + 2, std::string(amber) + "Arny · native coding companion" + std::string(reset));
            at(4, left + 2, std::string(dim) + "Multi-model · streaming · safe file tools" + std::string(reset));
            at(5, left + 2, std::string(dim) + "Esc cancels · keys and context stay in memory" + std::string(reset));
        } else {
            at(4, left + 3, std::string(orange) + "▄▄          ▄▄" + std::string(reset));
            at(5, left + 3, std::string(orange) + "▄▀██▄▀██▀▄██▀▄" + std::string(reset));
            at(6, left + 3, std::string(orange) + "   ▀██████▀" + std::string(reset));
            at(7, left + 3, std::string(orange) + "    ▄▀▀▀▀▄" + std::string(reset));
            at(4, left + 30, std::string(amber) + "Arny · your coding companion" + std::string(reset));
            at(5, left + 30, "Native CLI · bring your own model");
            at(6, left + 30, "Streaming replies · safe local file tools");
            at(7, left + 30, std::string(dim) + "Esc cancels · keys and context stay in memory" + std::string(reset));
        }
        if (height > 5) {
            at(height - 1, left + 2, session_text(plain));
        }
    }
    std::vector<Entry> log_;
    const arn::TerminalSession& terminal_;
    std::string input_, hint_, status_{"Type /help for commands · F2 copy mode"}, provider_{"none"}, model_;
    // Viewport: the model mirrors log_; view_ is what the transcript area
    // shows. With plain output view_ always follows.
    bool viewport_{}, finished_{};
    tui::Conversation transcript_{tui::Decoration::none};
    tui::View view_{};
    bool copy_mode_{};
    bool context_{};
    // Plain output: how much of log_ has been printed, whether the startup
    // banner was printed, whether the last printed line is still open, and
    // the last prompt line printed.
    mutable std::size_t printed_entries_{}, printed_bytes_{};
    mutable bool native_startup_emitted_{}, stream_line_open_{};
    bool confirmation_visible_{};
    mutable std::string native_plain_prompt_;
    mutable std::size_t native_line_columns_{};
};

} // namespace arn::terminal_ui
