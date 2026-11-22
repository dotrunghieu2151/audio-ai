#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// UTF-8 helpers for text that arrives or leaves in pieces.
namespace dth::audio::utf8 {

[[nodiscard]] constexpr bool is_continuation(char byte) noexcept
{
    return (static_cast<unsigned char>(byte) & 0xC0U) == 0x80U;
}

// The largest codepoint boundary <= `pos` (clamped to text.size()).
[[nodiscard]] constexpr std::size_t floor_boundary(std::string_view text, std::size_t pos) noexcept
{
    if (pos >= text.size()) {
        return text.size();
    }
    while (pos > 0 && is_continuation(text[pos])) {
        --pos;
    }
    return pos;
}

// Bytes of the sequence a lead byte starts; 1 for ASCII and for invalid lead bytes.
[[nodiscard]] constexpr std::size_t sequence_length(char lead_byte) noexcept
{
    const auto lead = static_cast<unsigned char>(lead_byte);
    if ((lead >> 5U) == 0x6U) {
        return 2;
    }
    if ((lead >> 4U) == 0xEU) {
        return 3;
    }
    if ((lead >> 3U) == 0x1EU) {
        return 4;
    }
    return 1;
}

// Length of the leading part of `text` that holds only complete codepoints: an incomplete
// sequence at the end is excluded; invalid bytes count as complete (passed through).
[[nodiscard]] constexpr std::size_t complete_prefix(std::string_view text) noexcept
{
    if (text.empty()) {
        return 0;
    }
    const std::size_t start = floor_boundary(text, text.size() - 1);
    return text.size() - start >= sequence_length(text[start]) ? text.size() : start;
}

struct Decoded
{
    char32_t    codepoint = 0;
    std::size_t length    = 1;
};

// The codepoint at `pos` (< text.size()). Invalid or truncated sequences decode as U+FFFD, one
// byte long.
[[nodiscard]] constexpr Decoded decode(std::string_view text, std::size_t pos) noexcept
{
    constexpr char32_t replacement = 0xFFFD;
    const auto         lead        = static_cast<unsigned char>(text[pos]);
    if (lead < 0x80U) {
        return {.codepoint = lead, .length = 1};
    }
    const std::size_t length = sequence_length(text[pos]);
    if (length == 1 || pos + length > text.size()) {
        return {.codepoint = replacement, .length = 1};
    }
    char32_t codepoint = lead & (0x7FU >> length);
    for (std::size_t i = 1; i < length; ++i) {
        if (!is_continuation(text[pos + i])) {
            return {.codepoint = replacement, .length = 1};
        }
        codepoint = (codepoint << 6U) | (static_cast<unsigned char>(text[pos + i]) & 0x3FU);
    }
    return {.codepoint = codepoint, .length = length};
}

// Joins token pieces, which can end inside a codepoint, into complete UTF-8 text.
class Assembler
{
  public:
    // Appends `piece`; returns the newly completed text (empty while a codepoint is incomplete).
    std::string append(std::string_view piece)
    {
        pending_ += piece;
        const std::size_t complete = complete_prefix(pending_);
        std::string       out      = pending_.substr(0, complete);
        pending_.erase(0, complete);
        return out;
    }

    // End of the text: an incomplete sequence left over (generation stopped inside a codepoint)
    // is dropped, so the output stays valid UTF-8.
    void flush() noexcept { pending_.clear(); }

  private:
    std::string pending_;
};

}  // namespace dth::audio::utf8
