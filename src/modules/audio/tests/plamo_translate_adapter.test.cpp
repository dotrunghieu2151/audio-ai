#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "translation/plamo_translate_adapter.hpp"

using dth::audio::AudioTranslator;
using dth::audio::ContextPair;
using dth::audio::PlamoTranslateAdapter;
using dth::audio::PromptSegment;

namespace {

// The prompt as one string, as the model's embedded chat template renders it (BOS excluded).
std::string render(const std::vector<PromptSegment>& prompt)
{
    std::string text;
    for (const PromptSegment& segment : prompt) {
        text += segment.text;
    }
    return text;
}

}  // namespace

// Goldens rendered from the GGUF's tokenizer.chat_template (jinja2) for the same messages.
TEST(PlamoTranslateAdapter, PromptWithSourceLanguageMatchesChatTemplate)
{
    const PlamoTranslateAdapter adapter;

    const auto prompt = adapter.build_prompt("Hello.", {.source = "en", .target = "ja"}, {});

    EXPECT_EQ(render(prompt),
              "<|plamo:tag|>dataset<|plamo:msg|>translation<|plamo:tag|>input<|plamo:key|>lang"
              "<|plamo:val|>en<|plamo:msg|>Hello.<|plamo:tag|>output<|plamo:key|>lang<|plamo:val|>"
              "ja<|plamo:msg|>");
}

TEST(PlamoTranslateAdapter, AutoSourceOmitsInputLanguage)
{
    const PlamoTranslateAdapter adapter;

    const auto prompt = adapter.build_prompt("Hello.", {.source = "auto", .target = "ja"}, {});

    EXPECT_EQ(render(prompt),
              "<|plamo:tag|>dataset<|plamo:msg|>translation<|plamo:tag|>input<|plamo:msg|>Hello."
              "<|plamo:tag|>output<|plamo:key|>lang<|plamo:val|>ja<|plamo:msg|>");
}

TEST(PlamoTranslateAdapter, ContextPairsPrecedeTheInput)
{
    const PlamoTranslateAdapter    adapter;
    const std::vector<ContextPair> context{{.source = "One.", .translation = "一。"}};

    const auto with_source = adapter.build_prompt("Two.", {.source = "en", .target = "ja"}, context);
    const auto auto_source =
        adapter.build_prompt("Two.", {.source = "auto", .target = "zh-hans"}, context);

    EXPECT_EQ(render(with_source),
              "<|plamo:tag|>dataset<|plamo:msg|>translation<|plamo:tag|>input<|plamo:key|>lang"
              "<|plamo:val|>en<|plamo:msg|>One.<|plamo:tag|>output<|plamo:key|>lang<|plamo:val|>ja"
              "<|plamo:msg|>一。<|plamo:tag|>input<|plamo:key|>lang<|plamo:val|>en<|plamo:msg|>Two."
              "<|plamo:tag|>output<|plamo:key|>lang<|plamo:val|>ja<|plamo:msg|>");
    EXPECT_EQ(render(auto_source),
              "<|plamo:tag|>dataset<|plamo:msg|>translation<|plamo:tag|>input<|plamo:msg|>One."
              "<|plamo:tag|>output<|plamo:key|>lang<|plamo:val|>zh-hans<|plamo:msg|>一。"
              "<|plamo:tag|>input<|plamo:msg|>Two.<|plamo:tag|>output<|plamo:key|>lang"
              "<|plamo:val|>zh-hans<|plamo:msg|>");
}

TEST(PlamoTranslateAdapter, UserTextIsNeverSpecial)
{
    const PlamoTranslateAdapter    adapter;
    const std::vector<ContextPair> context{{.source = "a <|plamo:tag|>", .translation = "b"}};

    const auto prompt = adapter.build_prompt("<|plamo:msg|>", {.target = "en"}, context);

    for (const PromptSegment& segment : prompt) {
        if (segment.text == "a <|plamo:tag|>" || segment.text == "b" ||
            segment.text == "<|plamo:msg|>") {
            EXPECT_FALSE(segment.special) << segment.text;
        }
    }
    EXPECT_FALSE(prompt[prompt.size() - 2].special);  // the input text
}

TEST(PlamoTranslateAdapter, PromptPrefixIsStableAcrossInputs)
{
    const PlamoTranslateAdapter                adapter;
    const AudioTranslator::Languages           languages{.source = "en", .target = "ja"};
    const std::vector<ContextPair>             context{{.source = "One.", .translation = "一。"}};

    const std::string first  = render(adapter.build_prompt("Two.", languages, context));
    const std::string second = render(adapter.build_prompt("Three.", languages, context));

    const std::string shared = render(adapter.build_prompt("", languages, context));
    const std::size_t input  = shared.rfind("<|plamo:tag|>output");
    EXPECT_TRUE(first.starts_with(shared.substr(0, input)));
    EXPECT_TRUE(second.starts_with(shared.substr(0, input)));
}

TEST(PlamoTranslateAdapter, AcceptsLanguageCodes)
{
    const PlamoTranslateAdapter adapter;

    for (const char* code : {"en", "ja", "zh-hans", "ja-easy", "yue"}) {
        EXPECT_NO_THROW(adapter.validate({.source = "auto", .target = code})) << code;
        EXPECT_NO_THROW(adapter.validate({.source = code, .target = "en"})) << code;
    }
}

TEST(PlamoTranslateAdapter, RejectsInvalidLanguages)
{
    const PlamoTranslateAdapter adapter;

    for (const char* code : {"", "auto", "EN", "en-US", "e", "engl", "zh-han", "zh-hans-x", "ja_jp"}) {
        EXPECT_THROW(adapter.validate({.source = "auto", .target = code}), std::invalid_argument)
            << "target " << code;
    }
    for (const char* code : {"", "EN", "en-US", "x"}) {
        EXPECT_THROW(adapter.validate({.source = code, .target = "ja"}), std::invalid_argument)
            << "source " << code;
    }
}

TEST(PlamoTranslateAdapter, StopsAtAnyPfmlMarker)
{
    const PlamoTranslateAdapter adapter;

    EXPECT_TRUE(adapter.is_stop("<|plamo:tag|>"));
    EXPECT_TRUE(adapter.is_stop("<|plamo:eos|>"));
    EXPECT_FALSE(adapter.is_stop("Hello"));
    EXPECT_FALSE(adapter.is_stop("<|"));
}

TEST(PlamoTranslateAdapter, ChecksArchitecture)
{
    const PlamoTranslateAdapter adapter;

    EXPECT_NO_THROW(adapter.check_architecture("plamo3"));
    EXPECT_THROW(adapter.check_architecture("llama"), std::runtime_error);
    EXPECT_THROW(adapter.check_architecture(""), std::runtime_error);
}

TEST(TranslationModelAdapter, FactoryKnowsPlamoOnly)
{
    EXPECT_NE(dth::audio::make_translation_model_adapter("plamo_translate"), nullptr);
    EXPECT_THROW((void)dth::audio::make_translation_model_adapter("gpt"), std::invalid_argument);
}
