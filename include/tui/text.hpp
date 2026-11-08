#pragma once

// UTF-8 decoding and terminal cell widths for the terminal renderer core.
// Widths approximate East Asian Width: wide/fullwidth and emoji take two
// cells, combining marks zero. Real hosts can differ; a presenter should draw
// every row with autowrap off, so a mismatch clips that row instead of
// shifting the others.

#include <string>
#include <string_view>
#include <vector>

namespace arn::tui {

struct Scalar {
    char32_t code;
    std::size_t size;
};

inline std::size_t utf8_length(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead >> 5) == 0x6) return 2;
    if ((lead >> 4) == 0xE) return 3;
    if ((lead >> 3) == 0x1E) return 4;
    return 1;
}

// Invalid or truncated sequences decode as U+FFFD, one byte at a time.
inline Scalar decode(std::string_view text, std::size_t index) {
    const auto lead = static_cast<unsigned char>(text[index]);
    const auto length = utf8_length(lead);
    if (length == 1) return {lead < 0x80 ? char32_t{lead} : char32_t{0xFFFD}, 1};
    if (index + length > text.size()) return {0xFFFD, 1};
    char32_t code = lead & (0xFFu >> (length + 1));
    for (std::size_t offset = 1; offset < length; ++offset) {
        const auto byte = static_cast<unsigned char>(text[index + offset]);
        if ((byte & 0xC0) != 0x80) return {0xFFFD, 1};
        code = (code << 6) | (byte & 0x3Fu);
    }
    return {code, length};
}

// Length of the longest prefix that does not end inside a UTF-8 sequence.
inline std::size_t complete_prefix(std::string_view text) {
    for (std::size_t back = 1; back <= 3 && back <= text.size(); ++back) {
        const auto byte = static_cast<unsigned char>(text[text.size() - back]);
        if ((byte & 0xC0) == 0x80) continue;
        return byte >= 0xC0 && utf8_length(byte) > back ? text.size() - back : text.size();
    }
    return text.size();
}

// C0, DEL and C1 controls. Terminals act on them (C1 also in UTF-8 form, as
// U+0080..U+009F), so text from a model or network must never draw them.
inline bool is_control(char32_t code) { return code < 0x20 || (code >= 0x7F && code <= 0x9F); }

inline int cell_width(char32_t code) {
    if (is_control(code)) return 0; // Control characters are not drawn.
    if ((code >= 0x0300 && code <= 0x036F) || (code >= 0x1AB0 && code <= 0x1AFF) ||
        (code >= 0x200B && code <= 0x200F) || code == 0x2060 ||
        (code >= 0x20D0 && code <= 0x20FF) || (code >= 0xFE00 && code <= 0xFE0F))
        return 0;
    if ((code >= 0x1100 && code <= 0x115F) || (code >= 0x2E80 && code <= 0xA4CF) ||
        (code >= 0xAC00 && code <= 0xD7A3) || (code >= 0xF900 && code <= 0xFAFF) ||
        (code >= 0xFE30 && code <= 0xFE4F) || (code >= 0xFF00 && code <= 0xFF60) ||
        (code >= 0xFFE0 && code <= 0xFFE6) || (code >= 0x1F300 && code <= 0x1F64F) ||
        (code >= 0x1F680 && code <= 0x1F6FF) || (code >= 0x1F900 && code <= 0x1F9FF) ||
        (code >= 0x1FA70 && code <= 0x1FAFF) || (code >= 0x20000 && code <= 0x3FFFD))
        return 2;
    return 1;
}

inline int text_width(std::string_view text) {
    int width = 0;
    for (std::size_t index = 0; index < text.size();) {
        const auto scalar = decode(text, index);
        width += cell_width(scalar.code);
        index += scalar.size;
    }
    return width;
}

// One display row and the byte offset in the source line where it starts.
struct WrappedRow {
    std::string text;
    std::size_t begin{};
};

// Split one logical line (no '\n') into rows of at most `width` cells. Tabs
// expand to the next multiple of four cells; other control characters are
// dropped. A wide glyph never straddles two rows.
inline std::vector<WrappedRow> wrap(std::string_view line, int width) {
    std::vector<WrappedRow> rows(1);
    int used = 0;
    const auto place = [&](std::string_view glyph, int cells, std::size_t source) {
        if (used + cells > width && used > 0) {
            rows.push_back({{}, source});
            used = 0;
        }
        rows.back().text.append(glyph);
        used += cells;
    };
    for (std::size_t index = 0; index < line.size();) {
        const auto scalar = decode(line, index);
        if (scalar.code == '\t') {
            for (int spaces = 4 - used % 4; spaces > 0; --spaces) place(" ", 1, index);
        } else if (!is_control(scalar.code)) {
            place(line.substr(index, scalar.size), cell_width(scalar.code), index);
        }
        index += scalar.size;
    }
    return rows;
}

// The longest prefix of `text` that fits in `width` cells.
inline std::string fit_cells(std::string_view text, int width) {
    std::string result;
    int used = 0;
    for (std::size_t index = 0; index < text.size();) {
        const auto scalar = decode(text, index);
        const int cells = cell_width(scalar.code);
        if (used + cells > width) break;
        result.append(text.substr(index, scalar.size));
        used += cells;
        index += scalar.size;
    }
    return result;
}

// The longest suffix of `text` that fits in `width` cells.
inline std::string tail_cells(std::string_view text, int width) {
    std::vector<Scalar> scalars;
    for (std::size_t index = 0; index < text.size();) {
        scalars.push_back(decode(text, index));
        index += scalars.back().size;
    }
    std::size_t start = text.size();
    int used = 0;
    for (auto it = scalars.rbegin(); it != scalars.rend(); ++it) {
        const int cells = cell_width(it->code);
        if (used + cells > width) break;
        used += cells;
        start -= it->size;
    }
    return std::string(text.substr(start));
}

} // namespace arn::tui
