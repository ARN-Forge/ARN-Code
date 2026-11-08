#pragma once

#include "terminal.hpp"
#include <algorithm>
#include <cstdint>
#include <iostream>
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

inline std::size_t display_columns(std::string_view text) {
    std::size_t columns = 0;
    for (std::size_t index = 0; index < text.size();) {
        index += std::min(char_bytes(static_cast<unsigned char>(text[index])), text.size() - index);
        ++columns;
    }
    return columns;
}

inline std::vector<std::string> wrap(std::string_view text, std::size_t width) {
    std::vector<std::string> output;
    std::string line;
    std::size_t columns = 0;
    for (std::size_t index = 0; index < text.size();) {
        if (text[index] == '\n') { output.push_back(std::move(line)); line.clear(); columns = 0; ++index; continue; }
        const auto count = std::min(char_bytes(static_cast<unsigned char>(text[index])), text.size() - index);
        if (columns == width) { output.push_back(std::move(line)); line.clear(); columns = 0; }
        line.append(text.substr(index, count));
        index += count;
        ++columns;
    }
    if (!line.empty() || output.empty()) output.push_back(std::move(line));
    return output;
}

inline std::string horizontal_rule(std::size_t cells) {
    std::string result;
    result.reserve(cells * 3);
    for (std::size_t index = 0; index < cells; ++index) result += "─";
    return result;
}

enum class Tone { normal, user, muted, good, warning, error };
struct Entry { Tone tone; std::string text; };

class Ui {
public:
    explicit Ui(const arn::TerminalSession& terminal) : terminal_(terminal) {}

    void add(Tone tone, std::string text) {
        log_.push_back({tone, std::move(text)});
        if (log_.size() > 180) {
            log_.erase(log_.begin(), log_.begin() + 20);
            if (printed_entries_ >= 20) printed_entries_ -= 20;
            else printed_entries_ = printed_bytes_ = 0;
        }
        scroll_offset_ = 0;
    }
    void append(std::string_view text) { if (log_.empty()) add(Tone::normal, {}); log_.back().text.append(text); scroll_offset_ = 0; }
    void clear() {
        log_.clear(); scroll_offset_ = 0;
        printed_entries_ = printed_bytes_ = 0;
        if (terminal_.native_scrollback()) {
            // /clear explicitly clears the viewport, never native scrollback.
            std::cout << "\x1b[2J\x1b[H";
            stream_line_open_ = false;
            live_ = {};
            native_anchor_lost_ = false;
            // Clearing the viewport is not a new session. The startup block
            // has already become terminal history and must never be replayed.
            native_header_anchored_ = false;
            native_line_columns_ = 0;
            native_body_rows_ = terminal_.size().height;
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

    void scroll(int amount) {
        if (terminal_.native_scrollback()) return;
        const auto [width, height] = dimensions();
        if (width < 38 || height < 13) return;
        const int inner = width - 4;
        const int header = inner < 58 ? 5 : inner < 76 ? 7 : 9;
        const int visible_rows = std::max(0, height - 3 - (header + 2));
        std::size_t total_rows = 0;
        for (const auto& entry : log_) total_rows += wrap(entry.text, static_cast<std::size_t>(inner - 2)).size();
        const auto maximum = total_rows > static_cast<std::size_t>(visible_rows)
            ? total_rows - static_cast<std::size_t>(visible_rows) : 0;
        if (amount > 0) scroll_offset_ = std::min(maximum, scroll_offset_ + static_cast<std::size_t>(amount));
        else scroll_offset_ = static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, static_cast<std::ptrdiff_t>(scroll_offset_) + amount));
    }

    void scroll_to_bottom() { scroll_offset_ = 0; }

    void render() const {
        if (terminal_.native_scrollback()) { render_transcript(); return; }
        const auto [width, height] = dimensions();
        std::cout << "\x1b[?2026h\x1b[?25l\x1b[2J\x1b[H";
        if (width < 38 || height < 13) {
            std::cout << cyan << "ARN" << reset << " — enlarge terminal\x1b[?25h\x1b[?2026l" << std::flush;
            return;
        }
        const int left = 2, inner = width - 4, header = inner < 58 ? 5 : inner < 76 ? 7 : 9;
        header_box(left, inner, header);
        const int content_top = header + 2, footer = height - 3, rows = std::max(0, footer - content_top);
        std::vector<Entry> lines;
        for (const auto& entry : log_) for (auto line : wrap(entry.text, static_cast<std::size_t>(inner - 2))) lines.push_back({entry.tone, std::move(line)});
        const std::size_t bottom = lines.size() > static_cast<std::size_t>(rows) ? lines.size() - static_cast<std::size_t>(rows) : 0;
        const std::size_t start = bottom > scroll_offset_ ? bottom - scroll_offset_ : 0;
        for (int row = 0; row < rows; ++row) if (start + row < lines.size()) at(content_top + row, left, color(lines[start + row].tone) + lines[start + row].text + std::string(reset));
        at(footer, 1, std::string(dim) + horizontal_rule(static_cast<std::size_t>(width - 2)) + std::string(reset));
        at(footer + 1, 1, std::string(cyan) + "arn" + std::string(amber) + " › " + std::string(reset) + input_);
        const auto footer_hint = current_hint();
        at(footer + 2, 1, std::string(dim) + footer_hint + std::string(reset));
        place_cursor(footer, width);
        std::cout << "\x1b[?25h\x1b[?2026l" << std::flush;
    }

    void render_input() const {
        if (terminal_.native_scrollback()) {
            // Plain output cannot edit a line in place; it prints prompts on
            // full renders only, so keystrokes never append output.
            if (!native_plain_) render_native_footer();
            return;
        }
        const auto [width, height] = dimensions();
        if (width < 38 || height < 13) { render(); return; }
        const int footer = height - 3;
        const auto footer_hint = current_hint();
        std::cout << "\x1b[?2026h\x1b[?25l";
        clear_line(footer + 1);
        std::cout << cyan << "arn" << amber << " › " << reset << input_;
        clear_line(footer + 2);
        std::cout << dim << footer_hint << reset;
        place_cursor(footer, width);
        std::cout << "\x1b[?25h\x1b[?2026l" << std::flush;
    }

    void show_confirmation(std::string_view choice) {
        if (terminal_.native_scrollback()) {
            input(std::string(choice));
            if (!confirmation_visible_) {
                // Retry on every display until the change preview is flushed:
                // never show the y/N prompt without the transcript above it.
                if (!render_transcript(false)) return;
                confirmation_visible_ = true;
            }
            render_native_footer();
        } else {
            input(std::string(choice));
            render();
        }
    }

    void finish_confirmation(int answer) {
        if (terminal_.native_scrollback() && !native_plain_) {
            const bool had_footer = live_.footer;
            if (erase_native_footer() && had_footer) remember_native_region(false);
        } else {
            add(Tone::muted, status_ + (answer == 'y' ? " y" : answer == 'n' ? " n" : ""));
        }
        confirmation_visible_ = false;
        input({});
    }

    [[nodiscard]] std::string ready_status() const {
        return terminal_.native_scrollback() ? "Type /help for commands"
            : "Type /help for commands · F2 copy mode";
    }

private:
    // Windows uses the original layout on the main buffer. Only the live
    // three-row footer is replaced; the startup header and conversation become
    // native terminal history rather than being repainted from log_.
    static std::string clipped(std::string_view text, std::size_t width) {
        std::string result;
        std::size_t columns = 0;
        for (std::size_t index = 0; index < text.size();) {
            if (text[index] == '\x1b' && index + 1 < text.size() && text[index + 1] == '[') {
                const auto begin = index;
                index += 2;
                while (index < text.size()) {
                    const auto value = static_cast<unsigned char>(text[index++]);
                    if (value >= 0x40 && value <= 0x7e) break;
                }
                result.append(text.substr(begin, index - begin));
                continue;
            }
            if (columns == width || text[index] == '\r' || text[index] == '\n') break;
            const auto count = std::min(char_bytes(static_cast<unsigned char>(text[index])),
                                        text.size() - index);
            result.append(text.substr(index, count));
            index += count;
            ++columns;
        }
        return result;
    }

    struct LiveRegionState {
        bool exists{}, footer{};
        std::uint64_t generation{};
        arn::TerminalLiveAnchor anchor;
    };

    void observe_native_geometry() const {
        const auto [width, height] = dimensions();
        if (width == geometry_.width && height == geometry_.height) return;
        geometry_ = {width, height};
        ++geometry_generation_;
    }

    void remember_native_region(bool footer) const {
        if (native_plain_) return;
        const auto creation_generation = geometry_generation_;
        observe_native_geometry();
        live_.exists = true;
        live_.footer = footer;
        live_.generation = creation_generation;
        if (creation_generation != geometry_generation_) {
            // Geometry changed during output: do not certify the torn frame
            // as belonging to the newly observed generation.
            live_.anchor = {};
            return;
        }
        // Certify the gap and stream append position cleanup may navigate to.
        // While header metadata is live, certify its viewport rows as well.
        const int before = native_header_anchored_ ? geometry_.height
            : footer ? 1 + native_footer_gap_ + (stream_line_open_ ? 1 : 0) : 0;
        live_.anchor = terminal_.capture_live_anchor(before, footer ? 1 : 0);
        if (live_.anchor.state) return;
        // Output we just wrote cannot be read back (redirected stdout or an
        // unreadable console). Re-anchoring would only stack another footer.
        observe_native_geometry();
        if (creation_generation == geometry_generation_) enter_plain_output(footer);
    }

    // Plain output is append-only and permanent for this Ui: no live footer,
    // no cursor movement, prompts printed once per change. It is never left,
    // because a backend that failed to certify one region may fail again.
    void enter_plain_output(bool below_footer) const {
        // The footer just drawn stays as history; continue below its hint row.
        if (below_footer) {
            std::cout << "\x1b[1B\r\n";
            stream_line_open_ = false;
            native_line_columns_ = 0;
        }
        native_plain_ = true;
        live_ = {};
        native_anchor_lost_ = false;
        native_header_anchored_ = false;
        native_footer_gap_ = 0;
        std::cout << std::flush;
    }

    void print_plain_prompt() const {
        const auto text = current_hint();
        if (text == native_plain_prompt_) return;
        native_plain_prompt_ = text;
        if (stream_line_open_) std::cout << "\r\n";
        std::cout << dim << text << reset << "\r\n" << std::flush;
        stream_line_open_ = false;
        native_line_columns_ = 0;
    }

    bool verify_native_region() const {
        if (native_plain_) return true;
        observe_native_geometry();
        if (!live_.exists && !native_anchor_lost_) return true;
        if (live_.exists && live_.generation == geometry_generation_ &&
            terminal_.live_anchor_valid(live_.anchor)) return true;
        // The old presentation belongs to host reflow/history now. Never
        // navigate back to it; preserve logical input/confirmation/log offsets.
        live_ = {};
        native_anchor_lost_ = true;
        native_header_anchored_ = false;
        native_footer_gap_ = 0;
        native_body_rows_ = geometry_.height;
        stream_line_open_ = false;
        native_line_columns_ = 0;
        if (!terminal_.begin_output_region()) return false;
        native_anchor_lost_ = false;
        remember_native_region(false);
        return true;
    }

    bool erase_native_footer() const {
        if (!verify_native_region()) return false;
        if (!live_.footer) return true;
        // Contents, cursor and geometry still match our flushed snapshot.
        std::cout << "\x1b[1A\r\x1b[2K\x1b[1B\r\x1b[2K\x1b[1B\r\x1b[2K\x1b[2A\r";
        live_.footer = false;
        if (native_footer_gap_ > 0)
            std::cout << "\x1b[" << native_footer_gap_ << "A\r";
        if (stream_line_open_)
            std::cout << "\x1b[1A\r\x1b[" << native_line_columns_ + 2 << 'G';
        // The caller certifies the region after its remaining output.
        return true;
    }

    void emit_native_startup() const {
        if (native_startup_emitted_) return;
        const auto [width, height] = dimensions();
        const int inner = std::max(1, width - 4);
        const int header = inner < 58 ? 5 : inner < 76 ? 7 : 9;
        if (width < 38 || height < 13) {
            std::cout << cyan << "ARN AGENT CODE" << reset << " — enlarge terminal\r\n";
        } else {
            // Reserve the initial layout once, using normal scrollback.
            std::cout << "\r";
            for (int row = 1; row < height; ++row) std::cout << "\n";
            std::cout << "\x1b[" << height - 1 << "A\r";
            std::cout << "\x1b[s\x1b[?7l";
            const auto paint = [&](int row, int column, const std::string& text) {
                std::cout << "\x1b[" << row << ';' << column << 'H';
                const auto right = 2 + inner - 1;
                const auto available = column == 2 ? inner
                    : column == right ? 1 : std::max(0, right - column);
                std::cout << clipped(text, static_cast<std::size_t>(available)) << reset;
            };
            for (int row = 2; row < header; ++row)
                paint(row, 3, std::string(static_cast<std::size_t>(inner - 2), ' '));
            draw_header(2, inner, header, paint);
            std::cout << "\x1b[" << header + 2 << ";1H";
            std::cout << "\x1b[?7h";
            native_header_anchored_ = true;
        }
        native_startup_emitted_ = true;
        native_header_state_ = session_state();
        native_header_width_ = width;
        native_header_height_ = header;
        native_screen_height_ = height;
    }

    std::string session_state() const {
        return provider_ + '\n' + model_ + (context_ ? "\nactive" : "\nempty");
    }

    std::string session_text() const {
        return std::string(dim) + "Provider: " + provider_ + " · Model: " +
            (model_.empty() ? "not selected" : model_) + " · Context: " +
            (context_ ? "active" : "empty") + std::string(reset);
    }

    void repaint_native_session() const {
        const auto [width, height] = dimensions();
        if (width != native_header_width_ || height != native_screen_height_ ||
            native_body_rows_ + (stream_line_open_ ? 1 : 0) > height - native_header_height_ - 5)
            native_header_anchored_ = false;
        // Only repaint the status row while its original location is known.
        // Once scrolled/reflowed, the host owns the startup block as history.
        // Repainting must never recreate a welcome frame or transcript entry.
        const auto state = session_state();
        if (!native_header_anchored_ || native_header_height_ <= 5 || state == native_header_state_) return;
        const auto available = static_cast<std::size_t>(std::max(0, width - 7));
        std::cout << "\x1b[s\x1b[?7l\x1b[" << native_header_height_ - 1 << ";4H"
                  << std::string(available, ' ') << "\x1b[" << native_header_height_ - 1 << ";4H"
                  << clipped(session_text(), available) << reset << "\x1b[u\x1b[?7h";
        native_header_state_ = state;
    }

    void append_native_text(std::string_view text, Tone tone) const {
        const auto width = static_cast<std::size_t>(std::max(1, terminal_.size().width - 6));
        std::cout << color(tone);
        for (std::size_t index = 0; index < text.size();) {
            if (text[index] == '\n') {
                std::cout << "\r\n";
                native_line_columns_ = 0;
                stream_line_open_ = false;
                ++native_body_rows_;
                ++index;
                continue;
            }
            if (native_line_columns_ >= width) {
                std::cout << "\r\n";
                native_line_columns_ = 0;
                stream_line_open_ = false;
                ++native_body_rows_;
            }
            if (!stream_line_open_) std::cout << ' '; // Same conversation margin.
            const auto count = std::min(char_bytes(static_cast<unsigned char>(text[index])),
                                        text.size() - index);
            std::cout << text.substr(index, count);
            native_line_columns_ += text[index] != '\r';
            stream_line_open_ = true;
            index += count;
        }
        std::cout << reset;
    }

    void render_native_footer() const {
        if (!verify_native_region()) return;
        draw_native_footer();
    }

    // Callers must have verified the region in this frame.
    void draw_native_footer() const {
        if (native_plain_) {
            print_plain_prompt();
            return;
        }
        const auto [columns, height] = dimensions();
        const auto width = std::max(9, columns);
        const auto lines = wrap(input_, static_cast<std::size_t>(width - 8));
        const auto& tail = lines.back();
        std::cout << "\x1b[?2026h\x1b[?25l\x1b[?7l";
        if (!live_.footer) {
            if (stream_line_open_) std::cout << "\r\n";
            native_footer_gap_ = std::max(0, height - native_header_height_ - 5 -
                native_body_rows_ - (stream_line_open_ ? 1 : 0));
            for (int row = 0; row < native_footer_gap_; ++row) std::cout << "\r\n";
            std::cout << dim << horizontal_rule(static_cast<std::size_t>(width - 2))
                      << reset << "\r\n";
        } else {
            std::cout << "\r\x1b[2K";
        }
        std::cout << cyan << "arn" << amber << " › " << reset << tail;
        if (live_.footer) std::cout << "\x1b[1B\r\x1b[2K";
        else std::cout << "\r\n";
        std::cout << dim << clipped(current_hint(), static_cast<std::size_t>(width - 2))
                  << reset << "\x1b[1A\r\x1b[" << 7 + display_columns(tail)
                  << "G\x1b[?7h\x1b[?25h\x1b[?2026l" << std::flush;
        remember_native_region(true);
    }

    // Returns false when no output region could be established.
    bool render_transcript(bool show_footer = true) const {
        if (!erase_native_footer()) return false;
        emit_native_startup();
        repaint_native_session();
        for (std::size_t index = printed_entries_; index < log_.size(); ++index) {
            const auto& entry = log_[index];
            const auto offset = index == printed_entries_ ? printed_bytes_ : 0;
            if (index != printed_entries_ && stream_line_open_) {
                std::cout << "\r\n";
                native_line_columns_ = 0;
                stream_line_open_ = false;
                ++native_body_rows_;
            }
            if (entry.text.size() > offset)
                append_native_text(std::string_view(entry.text).substr(offset), entry.tone);
            // Retain the last entry offset because streaming appends to it.
            if (index + 1 == log_.size()) {
                printed_entries_ = index;
                printed_bytes_ = entry.text.size();
            }
        }
        // A region certified earlier was verified when this frame began and
        // has received only our output since. Unless geometry changed, draw
        // the footer directly: re-reading here would only repeat that check.
        // The first frame has no certified region and is checked first.
        const auto [width, height] = dimensions();
        if (show_footer && live_.exists && width == geometry_.width && height == geometry_.height) {
            draw_native_footer();
        } else {
            remember_native_region(false);
            if (show_footer) render_native_footer();
        }
        std::cout << std::flush;
        return true;
    }
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
        if (scroll_offset_ != 0) return "Scrolled up · Wheel down or End returns to the latest message · Shift+drag selects text";
        auto text = confirmation_visible_ || hint_.empty() ? status_ : hint_;
        if (terminal_.native_scrollback()) {
            if (!confirmation_visible_ && !hint_.empty() && status_ != ready_status())
                text = status_ + " · " + hint_;
            if (const auto copy = text.find(" · F2 copy mode"); copy != std::string::npos)
                text.erase(copy);
        }
        return text;
    }
    void place_cursor(int footer, int width) const {
        std::cout << "\x1b[" << footer + 1 << ';'
                  << std::min(width - 1, 7 + static_cast<int>(display_columns(input_))) << 'H';
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

    template<class Draw>
    void draw_header(int left, int width, int height, Draw&& at) const {
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
            at(height - 1, left + 2, session_text());
        }
    }
    std::vector<Entry> log_;
    const arn::TerminalSession& terminal_;
    std::string input_, hint_, status_{"Type /help for commands · F2 copy mode"}, provider_{"none"}, model_;
    std::size_t scroll_offset_{};
    bool copy_mode_{};
    bool context_{};
    mutable std::size_t printed_entries_{}, printed_bytes_{};
    mutable bool native_startup_emitted_{}, native_header_anchored_{}, stream_line_open_{};
    bool confirmation_visible_{};
    mutable std::string native_header_state_;
    mutable int native_header_width_{}, native_header_height_{}, native_screen_height_{};
    mutable int native_body_rows_{}, native_footer_gap_{};
    mutable LiveRegionState live_;
    mutable bool native_anchor_lost_{}, native_plain_{};
    mutable std::string native_plain_prompt_;
    mutable arn::TerminalSize geometry_{};
    mutable std::uint64_t geometry_generation_{1};
    mutable std::size_t native_line_columns_{};
};

} // namespace arn::terminal_ui
