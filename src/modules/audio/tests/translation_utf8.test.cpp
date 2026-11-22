#include <gtest/gtest.h>

#include <string>

#include "translation/utf8.hpp"

namespace utf8 = dth::audio::utf8;

TEST(Utf8, FloorBoundaryNeverSplitsACodepoint)
{
    const std::string text = "aあb";  // 'a', 3 bytes, 'b'

    EXPECT_EQ(utf8::floor_boundary(text, 0), 0U);
    EXPECT_EQ(utf8::floor_boundary(text, 1), 1U);
    EXPECT_EQ(utf8::floor_boundary(text, 2), 1U);
    EXPECT_EQ(utf8::floor_boundary(text, 3), 1U);
    EXPECT_EQ(utf8::floor_boundary(text, 4), 4U);
    EXPECT_EQ(utf8::floor_boundary(text, 99), text.size());
}

TEST(Utf8, CompletePrefixExcludesATrailingPartialSequence)
{
    const std::string kana = "あ";

    EXPECT_EQ(utf8::complete_prefix(""), 0U);
    EXPECT_EQ(utf8::complete_prefix("ab"), 2U);
    EXPECT_EQ(utf8::complete_prefix("a" + kana), 4U);
    EXPECT_EQ(utf8::complete_prefix("a" + kana.substr(0, 2)), 1U);
    EXPECT_EQ(utf8::complete_prefix(kana.substr(0, 1)), 0U);
    EXPECT_EQ(utf8::complete_prefix("\xF0\x9F\x98"), 0U);   // 3 of 4 bytes of an emoji
    EXPECT_EQ(utf8::complete_prefix("a\xFF"), 2U);          // invalid byte passes through
}

TEST(Utf8Assembler, JoinsPiecesSplitInsideCodepoints)
{
    utf8::Assembler   assembler;
    const std::string text = "日本";  // 6 bytes

    EXPECT_EQ(assembler.append(text.substr(0, 2)), "");
    EXPECT_EQ(assembler.append(text.substr(2, 2)), "日");
    EXPECT_EQ(assembler.append(text.substr(4, 2)), "本");
}

TEST(Utf8Assembler, FlushDropsAnIncompleteTail)
{
    utf8::Assembler assembler;

    EXPECT_EQ(assembler.append("ok\xE3\x81"), "ok");
    assembler.flush();
    EXPECT_EQ(assembler.append("x"), "x");
}

TEST(Utf8, DecodesCodepoints)
{
    const std::string text = "aé」😀";

    EXPECT_EQ(utf8::decode(text, 0).codepoint, U'a');
    EXPECT_EQ(utf8::decode(text, 1).codepoint, U'é');
    EXPECT_EQ(utf8::decode(text, 1).length, 2U);
    EXPECT_EQ(utf8::decode(text, 3).codepoint, U'」');
    EXPECT_EQ(utf8::decode(text, 6).codepoint, U'😀');
    EXPECT_EQ(utf8::decode(text, 6).length, 4U);
    EXPECT_EQ(utf8::decode("\xE3\x81", 0).codepoint, 0xFFFDU);  // truncated
    EXPECT_EQ(utf8::decode("\x81", 0).codepoint, 0xFFFDU);       // stray continuation
}
