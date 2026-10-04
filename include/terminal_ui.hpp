#pragma once

#include "terminal.hpp"
#include <algorithm>
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
        if (tone == Tone::user) native_new_turn_ = true;
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
            input_visible_ = false;
            transcript_started_ = false;
            native_header_state_.clear();
            native_line_columns_ = 0;
            native_body_rows_ = 0;
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
            render_native_footer();
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
            if (!confirmation_visible_) {
                render_transcript(false);
                confirmation_visible_ = true;
            }
            input(std::string(choice));
            render_native_footer();
        } else {
            input(std::string(choice));
            render();
        }
    }

    void finish_confirmation(int answer) {
        if (terminal_.native_scrollback()) {
            erase_native_footer();
            confirmation_visible_ = false;
            input({});
        } else {
            add(Tone::muted, status_ + (answer == 'y' ? " y" : answer == 'n' ? " n" : ""));
            input({});
        }
    }

    [[nodiscard]] std::string ready_status() const {
        return terminal_.native_scrollback() ? "Type /help for commands"
            : "Type /help for commands · F2 copy mode";
    }

private:
    // Windows uses the original layout on the main buffer. Only the live
    // three-row footer is replaced; header blocks and conversation become
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

    void erase_native_footer() const {
        if (!input_visible_) return;
        // The cursor always rests on the middle (input) row. These are live
        // rows, never previously committed conversation or terminal history.
        std::cout << "\x1b[1A\r\x1b[2K\x1b[1B\r\x1b[2K\x1b[1B\r\x1b[2K\x1b[2A\r";
        input_visible_ = false;
        if (native_footer_gap_ > 0)
            std::cout << "\x1b[" << native_footer_gap_ << "A\r";
        if (stream_line_open_)
            std::cout << "\x1b[1A\r\x1b[" << native_line_columns_ + 2 << 'G';
    }

    void render_native_header() const {
        const auto [width, height] = dimensions();
        const auto state = provider_ + '\n' + model_ + (context_ ? "\nactive" : "\nempty");
        const int inner = std::max(1, width - 4);
        const int header = inner < 58 ? 5 : inner < 76 ? 7 : 9;
        const bool same_size = native_header_width_ == width && native_header_height_ == header &&
            native_screen_height_ == height;
        const bool header_visible = same_size &&
            native_body_rows_ + (stream_line_open_ ? 1 : 0) <= height - header - 5;
        const bool new_frame = !transcript_started_ || !same_size ||
            (native_new_turn_ && !header_visible);
        native_new_turn_ = false;
        if (!new_frame && (native_header_state_ == state || !header_visible)) return;
        if (new_frame) {
            if (stream_line_open_) std::cout << "\r\n";
            stream_line_open_ = false;
            native_line_columns_ = 0;
            native_body_rows_ = 0;
        }
        if (width < 38 || height < 13) {
            std::cout << cyan << "ARN" << reset << " — enlarge terminal\r\n";
        } else {
            // Start a frame by scrolling normally, never clearing history.
            // Within a frame the header can be updated while it is visible;
            // once it has scrolled away, leave it alone until the next turn.
            if (new_frame) {
                std::cout << "\r";
                for (int row = 1; row < height; ++row) std::cout << "\n";
                std::cout << "\x1b[" << height - 1 << "A\r";
            }
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
            if (new_frame) std::cout << "\x1b[" << header + 2 << ";1H";
            else std::cout << "\x1b[u";
            std::cout << "\x1b[?7h";
        }
        transcript_started_ = true;
        native_header_state_ = state;
        native_header_width_ = width;
        native_header_height_ = header;
        native_screen_height_ = height;
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
        const auto [columns, height] = dimensions();
        const auto width = std::max(9, columns);
        const auto lines = wrap(input_, static_cast<std::size_t>(width - 8));
        const auto& tail = lines.back();
        std::cout << "\x1b[?2026h\x1b[?25l\x1b[?7l";
        if (!input_visible_) {
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
        if (input_visible_) std::cout << "\x1b[1B\r\x1b[2K";
        else std::cout << "\r\n";
        std::cout << dim << clipped(current_hint(), static_cast<std::size_t>(width - 2))
                  << reset << "\x1b[1A\r\x1b[" << 7 + display_columns(tail)
                  << "G\x1b[?7h\x1b[?25h\x1b[?2026l" << std::flush;
        input_visible_ = true;
    }

    void render_transcript(bool show_footer = true) const {
        erase_native_footer();
        render_native_header();
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
        if (show_footer) render_native_footer();
        std::cout << std::flush;
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
            at(height - 1, left + 2, std::string(dim) + "Provider: " + provider_ + " · Model: " + (model_.empty() ? "not selected" : model_) + " · Context: " + (context_ ? "active" : "empty") + std::string(reset));
        }
    }
    std::vector<Entry> log_;
    const arn::TerminalSession& terminal_;
    std::string input_, hint_, status_{"Type /help for commands · F2 copy mode"}, provider_{"none"}, model_;
    std::size_t scroll_offset_{};
    bool copy_mode_{};
    bool context_{};
    mutable std::size_t printed_entries_{}, printed_bytes_{};
    mutable bool transcript_started_{}, stream_line_open_{}, input_visible_{};
    bool confirmation_visible_{};
    mutable std::string native_header_state_;
    mutable int native_header_width_{}, native_header_height_{}, native_screen_height_{};
    mutable int native_body_rows_{}, native_footer_gap_{};
    mutable bool native_new_turn_{};
    mutable std::size_t native_line_columns_{};
};

} // namespace arn::terminal_ui
