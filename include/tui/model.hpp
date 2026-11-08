#pragma once

// The retained conversation model of the terminal renderer core. Frames are
// composed from it; nothing depends on the terminal remembering what was
// drawn before.

#include "text.hpp"

#include <algorithm>
#include <compare>
#include <string>
#include <string_view>
#include <vector>

namespace arn::tui {

enum class Role { user, assistant, notice };

// How entries are laid out. `chat` marks each entry's first row by role and
// separates entries with a blank row. `none` lays out the bare text, for a
// host that draws its own margins, spacing and colours.
enum class Decoration { chat, none };

struct Entry {
    Role role;
    std::string text;
};

// A logical position in the conversation: an entry and a byte offset in its
// text. Unlike a row number it survives re-wrapping at another width.
struct Position {
    std::size_t entry{};
    std::size_t offset{};
    auto operator<=>(const Position&) const = default;
};

struct LayoutRow {
    std::string text;
    Position start; // Where the row's text begins (blank separators: entry start).
};

class Conversation {
public:
    explicit Conversation(Decoration decoration = Decoration::chat) : decoration_(decoration) {}

    void add(Role role, std::string text) {
        end_reply();
        entries_.push_back({role, std::move(text)});
    }

    void begin_reply() {
        end_reply();
        entries_.push_back({Role::assistant, {}});
        streaming_ = true;
    }

    // Network chunks may split a UTF-8 sequence; keep the incomplete tail
    // until the next chunk instead of rendering a replacement character.
    void stream(std::string_view bytes) {
        if (!streaming_) begin_reply();
        pending_.append(bytes);
        const auto ready = complete_prefix(pending_);
        entries_.back().text.append(pending_, 0, ready);
        pending_.erase(0, ready);
    }

    void end_reply() {
        if (!streaming_) return;
        entries_.back().text.append(pending_); // A truncated tail renders as U+FFFD.
        pending_.clear();
        streaming_ = false;
    }

    [[nodiscard]] bool streaming() const noexcept { return streaming_; }
    [[nodiscard]] const std::vector<Entry>& entries() const noexcept { return entries_; }

    // Display rows for a given width: each entry's logical lines wrapped to
    // the width minus a two-cell marker column, with a blank row between
    // entries. Only the last entry can still change, so the rows of all
    // earlier entries are cached per width; each call re-wraps one entry.
    // The returned rows stay valid until the next call.
    [[nodiscard]] const std::vector<LayoutRow>& layout(int width) const {
        if (width != cached_width_) {
            cached_width_ = width;
            cached_entries_ = cached_rows_ = 0;
            rows_.clear();
        }
        rows_.resize(cached_rows_); // Drop the previous rows of the last entry.
        const std::size_t stable = entries_.empty() ? 0 : entries_.size() - 1;
        for (; cached_entries_ < stable; ++cached_entries_) append_rows(cached_entries_, width);
        cached_rows_ = rows_.size();
        if (!entries_.empty()) append_rows(entries_.size() - 1, width);
        return rows_;
    }

private:
    void append_rows(std::size_t index, int width) const {
        const bool chat = decoration_ == Decoration::chat;
        const int body = std::max(1, chat ? width - 2 : width);
        if (index && chat) rows_.push_back({{}, {index, 0}});
        const auto& entry = entries_[index];
        const char* marker = !chat ? "" : entry.role == Role::user ? "› " : entry.role == Role::notice ? "· " : "  ";
        const char* indent = chat ? "  " : "";
        bool first = true;
        std::size_t start = 0;
        for (;;) {
            const auto end = entry.text.find('\n', start);
            const auto line = std::string_view(entry.text).substr(
                start, end == std::string::npos ? std::string::npos : end - start);
            for (auto& row : wrap(line, body)) {
                rows_.push_back({(first ? marker : indent) + row.text, {index, start + row.begin}});
                first = false;
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }

    Decoration decoration_;
    std::vector<Entry> entries_;
    std::string pending_;
    bool streaming_{};
    // Layout cache: rows of entries [0, cached_entries_) at cached_width_.
    mutable std::vector<LayoutRow> rows_;
    mutable int cached_width_{-1};
    mutable std::size_t cached_entries_{}, cached_rows_{};
};

} // namespace arn::tui
