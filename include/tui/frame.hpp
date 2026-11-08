#pragma once

// Frame composition and scrolling for the terminal renderer core. A frame is
// the full screen as rows, built only from the model and the current input;
// paint_frame() turns it into terminal bytes. Nothing here performs I/O or
// touches a platform terminal API.

#include "model.hpp"
#include "text.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace arn::tui {

// What the transcript area shows. Following means "always the newest rows";
// otherwise the top row is pinned to a logical position, so streaming does
// not move what you are reading and a resize re-wraps around the same text.
struct View {
    bool follow{true};
    Position top{};
};

struct FrameInput {
    int width{80};
    int height{24};
    std::string draft;
    std::string status;
    View view{};
};

enum class RowStyle { transcript, rule, prompt, status };

struct Row {
    std::string text;
    RowStyle style;
};

inline constexpr std::string_view prompt_text = "arn › "; // Six cells.

// Geometry shared by drawing and scrolling. The last column is left unused
// so no row can trigger a host autowrap.
// Refers to the conversation's cached rows; valid until its next layout.
struct Layout {
    const std::vector<LayoutRow>& rows;
    std::size_t body{}; // Transcript rows on screen.
    std::size_t top{};  // Index of the first transcript row shown.
    [[nodiscard]] std::size_t last_top() const { return rows.size() > body ? rows.size() - body : 0; }
    [[nodiscard]] std::size_t below() const { return rows.size() - std::min(rows.size(), top + body); }
};

inline Layout locate(const Conversation& conversation, const FrameInput& input) {
    Layout layout{conversation.layout(std::max(1, input.width - 1))};
    layout.body = static_cast<std::size_t>(std::max(0, std::max(1, input.height) - 3));
    layout.top = layout.last_top();
    if (!input.view.follow && layout.body > 0 && !layout.rows.empty()) {
        // The row that contains the pinned position: the last one starting
        // at or before it (rows are ordered by start). Never scroll past the
        // newest rows.
        const auto after = std::upper_bound(layout.rows.begin(), layout.rows.end(), input.view.top,
                                            [](const Position& position, const LayoutRow& row) {
                                                return position < row.start;
                                            });
        const auto row = static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, after - layout.rows.begin() - 1));
        layout.top = std::min(row, layout.last_top());
    }
    return layout;
}

// Move the view by `delta` rows (negative is older). Reaching the newest
// rows resumes following them.
inline View scrolled(const Conversation& conversation, const FrameInput& input, long long delta) {
    const auto layout = locate(conversation, input);
    if (layout.body == 0 || layout.rows.empty()) return {};
    const auto target = std::clamp<long long>(static_cast<long long>(layout.top) + delta, 0,
                                              static_cast<long long>(layout.last_top()));
    if (static_cast<std::size_t>(target) >= layout.last_top()) return {};
    return {false, layout.rows[static_cast<std::size_t>(target)].start};
}

// One screen minus a row of overlap, so context carries across pages.
inline long long page_rows(const FrameInput& input) {
    return std::max(1, std::max(1, input.height) - 4);
}

// Exactly max(1, height) rows: the transcript view, then the rule, the
// prompt and the status row (fewer footer rows on tiny terminals).
inline std::vector<Row> compose_frame(const Conversation& conversation, const FrameInput& input) {
    const int width = std::max(1, input.width - 1);
    const int height = std::max(1, input.height);
    const int footer = std::min(3, height);
    const auto layout = locate(conversation, input);
    const auto shown = std::min(layout.body, layout.rows.size() - layout.top);

    std::vector<Row> frame;
    for (std::size_t pad = layout.body - shown; pad > 0; --pad) frame.push_back({{}, RowStyle::transcript});
    for (std::size_t index = layout.top; index < layout.top + shown; ++index)
        frame.push_back({layout.rows[index].text, RowStyle::transcript});

    std::string rule;
    for (int cell = 0; cell < width; ++cell) rule += "─";
    if (footer == 3) frame.push_back({std::move(rule), RowStyle::rule});
    frame.push_back({std::string(prompt_text) + tail_cells(input.draft, std::max(0, width - 6)),
                     RowStyle::prompt});
    if (footer >= 2) frame.push_back({fit_cells(input.status, width), RowStyle::status});
    return frame;
}

inline std::string styled(const Row& row) {
    switch (row.style) {
    case RowStyle::rule:
    case RowStyle::status: return "\x1b[90m" + row.text + "\x1b[0m";
    case RowStyle::prompt:
        return "\x1b[38;5;81marn\x1b[38;5;215m ›\x1b[0m " + row.text.substr(prompt_text.size());
    default: return row.text;
    }
}

// Bytes that repaint every row of a full-screen frame in place: absolute row
// addressing, autowrap off and no line feeds, so painting can never scroll
// the screen. The prompt row is written last, so the cursor then rests right
// after the draft whatever the cell width of its characters. Entering or
// leaving a screen mode is the caller's responsibility.
inline std::string paint_frame(const std::vector<Row>& frame) {
    std::string out = "\x1b[?2026h\x1b[?25l\x1b[?7l";
    std::size_t prompt = frame.size();
    for (std::size_t index = 0; index < frame.size(); ++index) {
        if (frame[index].style == RowStyle::prompt) { prompt = index; continue; }
        out += "\x1b[" + std::to_string(index + 1) + ";1H" + styled(frame[index]) + "\x1b[K";
    }
    if (prompt < frame.size())
        out += "\x1b[" + std::to_string(prompt + 1) + ";1H" + styled(frame[prompt]) + "\x1b[K";
    out += "\x1b[?7h\x1b[?25h\x1b[?2026l";
    return out;
}

} // namespace arn::tui
