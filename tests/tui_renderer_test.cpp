// Unit tests for the terminal renderer core (include/tui). Ported from the
// standalone renderer proof of concept's --unit suite, plus regression tests
// for the model and layout behaviours later stages rely on. No terminal is
// needed: everything here is pure computation.

#include "tui/frame.hpp"
#include "tui/model.hpp"
#include "tui/text.hpp"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace tui = arn::tui;

int checks = 0;
void check(bool ok, const std::string& message) {
    ++checks;
    if (!ok) throw std::runtime_error(message);
}

std::size_t count(std::string_view text, std::string_view token) {
    std::size_t found = 0;
    for (auto at = text.find(token); at != std::string_view::npos; at = text.find(token, at + token.size())) ++found;
    return found;
}

// Scripted reply: Cyrillic, CJK, emoji, combining marks, a tab, and a line
// far longer than any terminal width.
std::string sample_reply() {
    std::string long_line = "Long line:";
    for (int repeat = 0; repeat < 12; ++repeat) long_line += " abcdefghij Привіт 你好 🙂";
    return "Привіт! Це тестова відповідь експерименту ARN.\n"
           "中文测试：你好，世界。全角字符占两个单元格。\n"
           "Emoji: 🙂 👍 🎉 🚀 with text around them.\n" +
           long_line +
           "\n"
           "int main() {\n\treturn 0;\n}\n"
           "Combining: é and Ukrainian ї, є, ґ.\n"
           "Done.";
}

// Deterministic chunks of 1–7 bytes, so UTF-8 sequences are often split
// across chunks the way network reads can split them.
class Chunker {
public:
    explicit Chunker(std::string text) : text_(std::move(text)) {}
    [[nodiscard]] std::string_view next() {
        seed_ = seed_ * 1103515245u + 12345u;
        const std::size_t size = 1 + (seed_ >> 16) % 7;
        const auto chunk = std::string_view(text_).substr(position_, size);
        position_ += chunk.size();
        return chunk;
    }
    [[nodiscard]] bool done() const noexcept { return position_ >= text_.size(); }

private:
    std::string text_;
    std::size_t position_{};
    std::uint32_t seed_{20261107};
};

tui::Conversation sample_conversation() {
    tui::Conversation conversation;
    conversation.add(tui::Role::notice, "Renderer test notice.");
    conversation.add(tui::Role::user, "Покажи приклад 你好 🙂");
    conversation.begin_reply();
    conversation.stream(sample_reply());
    conversation.end_reply();
    return conversation;
}

tui::Conversation long_conversation() {
    tui::Conversation conversation;
    conversation.add(tui::Role::notice, "Scrolling test notice.");
    for (int turn = 0; turn < 6; ++turn) {
        conversation.add(tui::Role::user, "Question " + std::to_string(turn) + " Привіт 你好");
        conversation.begin_reply();
        conversation.stream(sample_reply());
        conversation.end_reply();
    }
    return conversation;
}

std::vector<std::string> transcript_rows(const std::vector<tui::Row>& frame) {
    std::vector<std::string> rows;
    for (const auto& row : frame)
        if (row.style == tui::RowStyle::transcript) rows.push_back(row.text);
    return rows;
}

// --- Text ---------------------------------------------------------------

void widths_and_utf8() {
    check(tui::text_width("Привіт") == 6, "Cyrillic should be one cell per letter");
    check(tui::text_width("你好") == 4, "CJK should be two cells per glyph");
    check(tui::text_width("🙂") == 2, "Emoji should be two cells");
    check(tui::text_width("é") == 1, "A combining mark should take no cell");
    const std::string ukr = "Пр";
    check(tui::complete_prefix(std::string_view(ukr).substr(0, 3)) == 2, "Split UTF-8 tail was not held back");
    check(tui::complete_prefix("🙂") == 4 && tui::complete_prefix(std::string_view("🙂").substr(0, 2)) == 0,
          "Split 4-byte sequence was not held back");
}

void invalid_utf8_decodes_one_byte_at_a_time() {
    const std::string truncated = "\xD0";       // Lead byte of a 2-byte sequence, nothing after it.
    const std::string stray = "\x80";           // Continuation byte without a lead byte.
    const std::string broken = "\xE4\xBD" "A";  // 3-byte lead, then a non-continuation byte.
    for (const auto& text : {truncated, stray}) {
        const auto scalar = tui::decode(text, 0);
        check(scalar.code == 0xFFFD && scalar.size == 1, "Invalid byte did not decode as one U+FFFD");
    }
    const auto first = tui::decode(broken, 0);
    check(first.code == 0xFFFD && first.size == 1, "Broken sequence must consume only its first byte");
    check(tui::text_width(broken) == 3, "Each invalid byte should take one cell, then 'A' one cell");
    check(tui::complete_prefix("") == 0 && tui::complete_prefix("abc") == 3, "ASCII prefixes are complete");
}

void wrapping_respects_width() {
    const std::string line = "Long line: abcdefghij Привіт 你好 🙂 é 中文测试：你好，世界。🎉🚀";
    for (int width = 2; width <= 60; ++width) {
        std::string joined;
        for (const auto& row : tui::wrap(line, width)) {
            check(tui::text_width(row.text) <= width, "Wrapped row exceeds width " + std::to_string(width));
            // Each row records where its text starts in the source line.
            check(std::string_view(line).substr(row.begin).starts_with(row.text),
                  "Wrapped row start offset is wrong at width " + std::to_string(width));
            joined += row.text;
        }
        check(joined == line, "Wrapping lost or reordered text at width " + std::to_string(width));
    }
}

void wrapping_edge_cases() {
    const auto empty = tui::wrap("", 10);
    check(empty.size() == 1 && empty.front().text.empty() && empty.front().begin == 0,
          "An empty line must still produce one empty row");

    // A wide glyph that does not fit moves whole to the next row.
    const auto wide = tui::wrap("a你好", 3);
    check(wide.size() == 2 && wide[0].text == "a你" && wide[1].text == "好" && wide[1].begin == 4,
          "A wide glyph straddled two rows or its row start is wrong");

    // A glyph wider than the row still lands on a row of its own.
    const auto narrow = tui::wrap("你好", 1);
    check(narrow.size() == 2 && narrow[0].text == "你" && narrow[1].text == "好",
          "A glyph wider than the width must take a row of its own");

    // Tabs expand to the next multiple of four cells.
    const auto tabbed = tui::wrap("ab\tc", 20);
    check(tabbed.size() == 1 && tabbed[0].text == "ab  c", "Tab did not expand to the next tab stop");
    check(tui::wrap("\tx", 20)[0].text == "    x", "Leading tab should expand to four cells");

    // Other control characters are dropped, combining marks are kept.
    check(tui::wrap("a\x01\x1b" "b\r", 20)[0].text == "ab", "Control characters must not be drawn");
    check(tui::wrap("é", 1)[0].text == "é", "A combining mark must stay with its base character");
}

void fitting_and_tails() {
    check(tui::fit_cells("你好", 3) == "你", "fit_cells split or overflowed a wide glyph");
    check(tui::fit_cells("abc", 0).empty(), "Nothing fits in zero cells");
    check(tui::fit_cells("Привіт", 10) == "Привіт", "Text that fits must be returned whole");
    check(tui::tail_cells("你好", 3) == "好", "tail_cells split or overflowed a wide glyph");
    check(tui::tail_cells("abc", 0).empty(), "No tail fits in zero cells");
    check(tui::tail_cells("abcdef", 3) == "def", "tail_cells should keep the newest characters");
    check(tui::tail_cells("x🙂", 2) == "🙂", "tail_cells should keep a whole emoji");
}

// --- Model --------------------------------------------------------------

void streaming_keeps_text_intact() {
    const std::string text = sample_reply();
    tui::Conversation conversation;
    conversation.begin_reply();
    Chunker chunker(text);
    while (!chunker.done()) {
        conversation.stream(chunker.next());
        const auto& partial = conversation.entries().back().text;
        check(text.starts_with(partial), "Streaming produced text that is not a prefix of the reply");
        check(tui::complete_prefix(partial) == partial.size(), "Streaming exposed a split UTF-8 sequence");
    }
    conversation.end_reply();
    check(conversation.entries().back().text == text, "Streaming lost or altered reply bytes");
}

void reply_lifecycle() {
    tui::Conversation conversation;
    check(!conversation.streaming() && conversation.entries().empty(), "A new conversation is empty");

    // Streaming without begin_reply starts an assistant reply.
    conversation.stream("hi");
    check(conversation.streaming() && conversation.entries().size() == 1 &&
              conversation.entries().back().role == tui::Role::assistant,
          "stream() should start an assistant reply");

    // add() ends the open reply first, so later text never joins it.
    conversation.add(tui::Role::notice, "Request cancelled.");
    check(!conversation.streaming() && conversation.entries().size() == 2 &&
              conversation.entries()[0].text == "hi",
          "add() must close the reply before appending an entry");

    // A reply cut off inside a UTF-8 sequence keeps its bytes when it ends.
    conversation.stream("ok \xD0");
    check(conversation.entries().back().text == "ok ", "An incomplete tail must be held back while streaming");
    conversation.end_reply();
    check(conversation.entries().back().text == "ok \xD0", "end_reply() must keep a truncated tail");
    conversation.end_reply();
    check(conversation.entries().size() == 3, "A second end_reply() must do nothing");

    // begin_reply() always opens a fresh entry, even right after another.
    conversation.begin_reply();
    conversation.begin_reply();
    check(conversation.entries().size() == 5 && conversation.entries().back().text.empty(),
          "begin_reply() should open a new empty entry");
}

void layout_rows_and_positions() {
    tui::Conversation conversation;
    conversation.add(tui::Role::user, "question");
    conversation.add(tui::Role::assistant, "line one\nline two");
    conversation.add(tui::Role::notice, "note");
    const auto& rows = conversation.layout(40);
    check(rows.size() == 6, "Expected 4 text rows plus 2 separators, got " + std::to_string(rows.size()));
    check(rows[0].text == "› question" && rows[0].start == tui::Position{0, 0}, "User row marker or start");
    check(rows[1].text.empty() && rows[1].start == tui::Position{1, 0}, "Separator must be blank at entry start");
    check(rows[2].text == "  line one" && rows[2].start == tui::Position{1, 0}, "Assistant first row");
    check(rows[3].text == "  line two" && rows[3].start == tui::Position{1, 9}, "Second line starts after '\\n'");
    check(rows[5].text == "· note" && rows[5].start == tui::Position{2, 0}, "Notice row marker");
    for (std::size_t index = 1; index < rows.size(); ++index)
        check(!(rows[index].start < rows[index - 1].start), "Layout rows must be ordered by start");

    // Continuation rows of a wrapped line are indented, not marked again.
    tui::Conversation wrapped;
    wrapped.add(tui::Role::user, "abcdefgh");
    const auto& narrow = wrapped.layout(6); // Four cells of text after the marker.
    check(narrow.size() == 2 && narrow[0].text == "› abcd" && narrow[1].text == "  efgh" &&
              narrow[1].start == tui::Position{0, 4},
          "Wrapped continuation row is wrong");
    check(tui::Conversation{}.layout(80).empty(), "An empty conversation has no rows");
}

// The cached layout must always equal a layout built from scratch.
void layout_cache_matches_fresh_layout() {
    tui::Conversation conversation;
    int steps = 0;
    const auto compare = [&](int width) {
        ++steps;
        tui::Conversation fresh = conversation;
        (void)fresh.layout(width + 1); // A different width discards the copied cache.
        const auto expected = fresh.layout(width);
        const auto& actual = conversation.layout(width);
        check(actual.size() == expected.size(), "Cached layout has a different row count at step " +
                                                    std::to_string(steps));
        for (std::size_t index = 0; index < actual.size(); ++index)
            check(actual[index].text == expected[index].text && actual[index].start == expected[index].start,
                  "Cached layout row differs at step " + std::to_string(steps));
    };
    conversation.add(tui::Role::notice, "Cache test.");
    compare(80);
    for (int turn = 0; turn < 3; ++turn) {
        conversation.add(tui::Role::user, "Question " + std::to_string(turn) + " 你好");
        compare(80);
        conversation.begin_reply();
        Chunker chunker(sample_reply());
        for (int chunk = 0; !chunker.done(); ++chunk) {
            conversation.stream(chunker.next());
            compare(chunk % 25 == 0 ? 47 : 80); // Width changes mid-stream, then back.
        }
        conversation.end_reply();
        compare(80);
    }
    // A reply that ended and was followed by an entry is now cached as stable.
    conversation.add(tui::Role::notice, "After the replies.");
    compare(80);
    compare(23);
}

// Decoration::none: bare text rows for a host that draws its own margins.
void bare_layout_rows_and_positions() {
    tui::Conversation conversation(tui::Decoration::none);
    conversation.add(tui::Role::user, "question");
    conversation.add(tui::Role::notice, {});
    conversation.add(tui::Role::assistant, "abcdefgh\nnext");
    const auto& rows = conversation.layout(4);
    const std::vector<std::pair<std::string, tui::Position>> expected = {
        {"ques", {0, 0}}, {"tion", {0, 4}}, {"", {1, 0}}, {"abcd", {2, 0}}, {"efgh", {2, 4}}, {"next", {2, 9}}};
    check(rows.size() == expected.size(), "Bare layout must have no separators, got " + std::to_string(rows.size()));
    for (std::size_t index = 0; index < rows.size(); ++index)
        check(rows[index].text == expected[index].first && rows[index].start == expected[index].second,
              "Bare layout row " + std::to_string(index) + " is wrong");
    check(tui::Conversation(tui::Decoration::none).layout(10).empty(), "An empty bare conversation has no rows");
    // The default stays the chat layout.
    tui::Conversation chat;
    chat.add(tui::Role::user, "a");
    chat.add(tui::Role::user, "b");
    check(chat.layout(10).size() == 3 && chat.layout(10)[0].text == "› a", "The default layout changed");
}

void bare_layout_cache_and_scrolling() {
    tui::Conversation conversation(tui::Decoration::none);
    int steps = 0;
    const auto compare = [&](int width) {
        ++steps;
        tui::Conversation fresh = conversation;
        (void)fresh.layout(width + 1);
        const auto expected = fresh.layout(width);
        const auto& actual = conversation.layout(width);
        check(actual.size() == expected.size(), "Bare cached layout row count differs at step " + std::to_string(steps));
        for (std::size_t index = 0; index < actual.size(); ++index) {
            check(actual[index].text == expected[index].text && actual[index].start == expected[index].start,
                  "Bare cached layout row differs at step " + std::to_string(steps));
            check(tui::text_width(actual[index].text) <= width, "Bare row exceeds the layout width");
        }
    };
    for (int turn = 0; turn < 4; ++turn) {
        conversation.begin_reply();
        conversation.stream("Question " + std::to_string(turn));
        compare(60);
        conversation.begin_reply();
        Chunker chunker(sample_reply());
        for (int chunk = 0; !chunker.done(); ++chunk) {
            conversation.stream(chunker.next());
            compare(chunk % 30 == 0 ? 33 : 60);
        }
    }
    // Pinned scrolling works the same without decoration.
    tui::FrameInput input{61, 20, {}, {}, {}};
    input.view = tui::scrolled(conversation, input, -2 * tui::page_rows(input));
    const auto before = transcript_rows(tui::compose_frame(conversation, input));
    conversation.stream("\nmore output\nand more");
    conversation.begin_reply();
    conversation.stream("another entry");
    check(transcript_rows(tui::compose_frame(conversation, input)) == before,
          "New output moved the rows being read (bare layout)");
}

void controls_are_never_drawn() {
    for (const char32_t code : {char32_t{0x00}, char32_t{0x1B}, char32_t{0x7F}, char32_t{0x80}, char32_t{0x9B},
                                char32_t{0x9F}})
        check(tui::is_control(code) && tui::cell_width(code) == 0, "A control character must take no cell");
    for (const char32_t code : {char32_t{0x20}, char32_t{0xA0}, char32_t{0x0456}, char32_t{0xFFFD}})
        check(!tui::is_control(code) && tui::cell_width(code) == 1, "A printable character must take one cell");
    // DEL and C1 (in UTF-8 form) are dropped like C0; NBSP is kept.
    const auto rows = tui::wrap("a\x7f" "b\xC2\x80" "c\xC2\x9B" "d\xC2\x9F" "e\xC2\xA0" "f", 40);
    check(rows.size() == 1 && rows[0].text == "abcde\xC2\xA0" "f", "DEL or C1 controls reached a row");
}

// --- Frames -------------------------------------------------------------

void frames_fit_every_size() {
    const auto conversation = sample_conversation();
    for (const int width : {1, 10, 20, 37, 80, 120, 200}) {
        for (const int height : {1, 2, 3, 4, 10, 30, 60}) {
            tui::FrameInput input{width, height, "чернетка 你好 🙂", "status", {}};
            const auto frame = tui::compose_frame(conversation, input);
            const std::string label = std::to_string(width) + "x" + std::to_string(height);
            check(static_cast<int>(frame.size()) == height, "Frame row count differs from height at " + label);
            int prompts = 0;
            for (std::size_t index = 0; index < frame.size(); ++index) {
                check(tui::text_width(frame[index].text) <= std::max(1, width - 1) || width < 8,
                      "Frame row exceeds width at " + label);
                if (frame[index].style == tui::RowStyle::prompt) {
                    ++prompts;
                    check(static_cast<int>(index) == height - (height >= 2 ? 2 : 1),
                          "Prompt is not fixed at the bottom at " + label);
                }
            }
            check(prompts == 1, "Frame must contain exactly one prompt at " + label);
            if (height >= 4) {
                const auto rows = conversation.layout(width - 1);
                check(frame[static_cast<std::size_t>(height - 4)].text == rows.back().text,
                      "Newest output is not just above the rule at " + label);
            }
        }
    }
}

void frame_footer_contents() {
    tui::Conversation conversation;
    conversation.add(tui::Role::user, "hello");
    const auto frame = tui::compose_frame(conversation, {20, 6, "draft", "Ready", {}});
    check(frame.size() == 6, "Frame height");
    check(frame[0].text.empty() && frame[1].text.empty() && frame[2].text == "› hello",
          "Short transcripts must sit just above the footer, padded on top");
    check(frame[3].style == tui::RowStyle::rule && tui::text_width(frame[3].text) == 19, "Rule spans width - 1");
    check(frame[4].text == "arn › draft", "Prompt row shows the draft");
    check(frame[5].style == tui::RowStyle::status && frame[5].text == "Ready", "Status row");

    // A long draft shows its newest characters; a long status is clipped.
    const auto clipped = tui::compose_frame(conversation, {12, 4, "abcdefghijkl", "status text that is long", {}});
    check(clipped[2].text == "arn › hijkl" && clipped[3].text == "status text",
          "Draft tail or status clipping is wrong");
}

void paint_never_scrolls() {
    const auto conversation = sample_conversation();
    for (const int width : {30, 80, 140}) {
        for (const int height : {1, 2, 5, 24, 50}) {
            const auto frame = tui::compose_frame(conversation, {width, height, "draft", "status", {}});
            const auto bytes = tui::paint_frame(frame);
            // Rows are addressed absolutely and never followed by a line feed,
            // so a frame cannot scroll the screen or reach scrollback.
            check(bytes.find('\n') == std::string::npos && bytes.find('\r') == std::string::npos,
                  "Painted frame contains a line break");
            for (int row = 1; row <= height; ++row)
                check(count(bytes, "\x1b[" + std::to_string(row) + ";1H") == 1, "A row was not painted exactly once");
            // The prompt row is written last, so the cursor rests after the draft.
            const auto prompt_row = "\x1b[" + std::to_string(height - (height >= 2 ? 2 : 1) + 1) + ";1H";
            check(bytes.rfind(";1H") == bytes.rfind(prompt_row) + prompt_row.size() - 3,
                  "The prompt row must be painted last");
            check(bytes.starts_with("\x1b[?2026h") && bytes.ends_with("\x1b[?2026l"),
                  "A frame must be one synchronized update");
            check(bytes.find("\x1b[?7l") < bytes.find(";1H") && bytes.find("\x1b[?7h") > bytes.rfind(";1H"),
                  "Autowrap must be off while rows are painted");
            // Screen modes belong to the caller; painting never switches screens.
            check(bytes.find("\x1b[?1049") == std::string::npos && bytes.find("\x1b[2J") == std::string::npos,
                  "paint_frame must not switch or clear screens");
        }
    }
    check(tui::paint_frame({}) == "\x1b[?2026h\x1b[?25l\x1b[?7l\x1b[?7h\x1b[?25h\x1b[?2026l",
          "An empty frame paints no rows");
}

// --- Scrolling ----------------------------------------------------------

void scrolling_moves_and_returns() {
    const auto conversation = long_conversation();
    tui::FrameInput input{80, 20, "draft while scrolled 你好", "status", {}};
    const auto newest = tui::compose_frame(conversation, input);
    const auto all = conversation.layout(79);

    input.view = tui::scrolled(conversation, input, -tui::page_rows(input));
    check(!input.view.follow, "PgUp did not leave the newest output");
    auto frame = tui::compose_frame(conversation, input);
    check(transcript_rows(frame) != transcript_rows(newest), "PgUp did not change the visible rows");
    check(frame[18].style == tui::RowStyle::prompt && frame[18].text.find("draft while scrolled") != std::string::npos,
          "Prompt is not usable while scrolled");
    check(tui::locate(conversation, input).below() == static_cast<std::size_t>(tui::page_rows(input)),
          "One page up should leave one page of rows below");

    for (int page = 0; page < 1000; ++page) input.view = tui::scrolled(conversation, input, -tui::page_rows(input));
    check(transcript_rows(tui::compose_frame(conversation, input)).front() == all.front().text,
          "Scrolling up should stop at the first row");

    int pages = 0;
    while (!input.view.follow && pages < 1000) {
        input.view = tui::scrolled(conversation, input, tui::page_rows(input));
        ++pages;
    }
    check(input.view.follow, "PgDn never returned to the newest output");
    check(transcript_rows(tui::compose_frame(conversation, input)) == transcript_rows(newest),
          "Returning to the bottom does not show the newest rows");

    input.view = tui::scrolled(conversation, input, -3);
    input.view = {}; // End
    check(transcript_rows(tui::compose_frame(conversation, input)) == transcript_rows(newest),
          "End does not return to the newest rows");
    check(tui::scrolled(conversation, input, 5).follow, "PgDn at the bottom must keep following");
}

void streaming_while_scrolled_keeps_view() {
    auto conversation = long_conversation();
    conversation.begin_reply();
    conversation.stream("Partial reply that keeps growing");
    tui::FrameInput input{70, 18, {}, {}, {}};
    input.view = tui::scrolled(conversation, input, -2 * tui::page_rows(input));
    const auto before = transcript_rows(tui::compose_frame(conversation, input));
    const auto below = tui::locate(conversation, input).below();
    Chunker chunker(sample_reply());
    while (!chunker.done()) conversation.stream(chunker.next());
    check(transcript_rows(tui::compose_frame(conversation, input)) == before,
          "New output moved the rows being read");
    check(tui::locate(conversation, input).below() > below, "New rows below were not counted");

    // New entries (tool notices, errors) do not move the view either.
    conversation.add(tui::Role::notice, "• tool call");
    conversation.add(tui::Role::notice, "Done.");
    check(transcript_rows(tui::compose_frame(conversation, input)) == before,
          "A new entry moved the rows being read");
}

void resize_while_scrolled_keeps_position() {
    const auto conversation = long_conversation();
    tui::FrameInput input{90, 25, {}, {}, {}};
    input.view = tui::scrolled(conversation, input, -3 * tui::page_rows(input));
    const auto pinned = input.view.top;
    for (const auto& [width, height] : {std::pair{40, 15}, std::pair{150, 45}, std::pair{60, 10}, std::pair{200, 60},
                                      std::pair{30, 8}, std::pair{90, 25}}) {
        input.width = width;
        input.height = height;
        const auto layout = tui::locate(conversation, input);
        const auto& top = layout.rows[layout.top];
        const bool contains = top.start <= pinned &&
                              (layout.top + 1 >= layout.rows.size() || pinned < layout.rows[layout.top + 1].start ||
                               layout.top == layout.last_top());
        check(contains, "Resize lost the reading position at " + std::to_string(width) + "x" + std::to_string(height));
        check(static_cast<int>(tui::compose_frame(conversation, input).size()) == height,
              "Scrolled frame has the wrong height after resize");
    }
}

void pinned_view_never_passes_newest_rows() {
    // A view pinned near the end shows a full screen, not a short one.
    const auto conversation = long_conversation();
    tui::FrameInput input{80, 20, {}, {}, {}};
    const auto& rows = conversation.layout(79);
    input.view = {false, rows.back().start};
    const auto layout = tui::locate(conversation, input);
    check(layout.top == layout.last_top() && layout.below() == 0, "A pinned view scrolled past the newest rows");
    // A position beyond the end of the conversation clamps the same way.
    input.view = {false, {conversation.entries().size() + 5, 0}};
    check(tui::locate(conversation, input).top == layout.last_top(), "An out-of-range position must clamp");
    check(tui::scrolled(tui::Conversation{}, input, -5).follow, "An empty conversation cannot scroll");
}

void tiny_windows_do_not_scroll() {
    const auto conversation = long_conversation();
    for (const int height : {1, 2, 3}) {
        tui::FrameInput input{40, height, {}, {}, {}};
        check(tui::scrolled(conversation, input, -5).follow, "A window with no transcript rows cannot scroll");
        check(static_cast<int>(tui::compose_frame(conversation, input).size()) == height, "Tiny frame height");
    }
    check(tui::page_rows({40, 1, {}, {}, {}}) == 1 && tui::page_rows({40, 24, {}, {}, {}}) == 20,
          "A page is one screen minus the footer and one row of overlap");
}

} // namespace

int main() {
    try {
        widths_and_utf8();
        invalid_utf8_decodes_one_byte_at_a_time();
        wrapping_respects_width();
        wrapping_edge_cases();
        fitting_and_tails();
        streaming_keeps_text_intact();
        reply_lifecycle();
        layout_rows_and_positions();
        layout_cache_matches_fresh_layout();
        bare_layout_rows_and_positions();
        bare_layout_cache_and_scrolling();
        controls_are_never_drawn();
        frames_fit_every_size();
        frame_footer_contents();
        paint_never_scrolls();
        scrolling_moves_and_returns();
        streaming_while_scrolled_keeps_view();
        resize_while_scrolled_keeps_position();
        pinned_view_never_passes_newest_rows();
        tiny_windows_do_not_scroll();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "tui_renderer_test FAILED after %d checks: %s\n", checks, error.what());
        return 1;
    }
    std::printf("tui_renderer_test: %d checks passed\n", checks);
    return 0;
}
