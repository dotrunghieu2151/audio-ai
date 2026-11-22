// Tests that run the real translation model (label "model"; skipped when the model file is
// missing). Exclude with ctest -LE model.
#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "dth/audio/audio_translator.hpp"
#include "translation/llama_support.hpp"
#include "translation/translation_engine.hpp"

using dth::audio::AudioTranslator;
using dth::audio::ContextPair;
using dth::audio::TranslationEngine;
using dth::audio::TranslationJob;
using dth::audio::TranslationModel;

namespace {

const std::string model_path = DTH_TEST_ASSETS_DIR "/models/plamo-3-translate-2b-Q4_K_M.gguf";
// Empty when absent: streams then cut at punctuation only.
const std::string sentence_model_path =
    std::filesystem::exists(DTH_TEST_ASSETS_DIR "/models/sat-12l-sm")
        ? DTH_TEST_ASSETS_DIR "/models/sat-12l-sm"
        : "";

std::string lower(std::string text)
{
    std::ranges::transform(text, text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

class TranslationModelTest : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        if (!std::filesystem::exists(model_path)) {
            return;
        }
        translator = std::make_unique<AudioTranslator>(AudioTranslator::Config{
            .model_path = model_path, .sentence_model_path = sentence_model_path});
        auto model = dth::audio::llama::load_model(model_path, -1);
        shared_model = std::make_shared<const TranslationModel>(TranslationModel{
            .model   = std::move(model),
            .adapter = dth::audio::make_translation_model_adapter("plamo_translate")});
    }

    static void TearDownTestSuite()
    {
        translator.reset();
        shared_model.reset();
    }

    void SetUp() override
    {
        if (!translator) {
            GTEST_SKIP() << "model not found: " << model_path;
        }
    }

    static std::unique_ptr<TranslationEngine> engine(int context_tokens = 4096)
    {
        return std::make_unique<TranslationEngine>(
            shared_model, TranslationEngine::Settings{.context_tokens = context_tokens});
    }

    static inline std::unique_ptr<AudioTranslator>         translator;
    static inline std::shared_ptr<const TranslationModel> shared_model;
};

const AudioTranslator::Languages en_ja{.source = "en", .target = "ja"};

}  // namespace

TEST_F(TranslationModelTest, TranslatesEnglishToJapanese)
{
    const auto result = translator->translate("I studied computer science in college.", en_ja);

    EXPECT_NE(result.text.find("コンピュータ"), std::string::npos) << result.text;
    EXPECT_EQ(result.text.find("<|plamo"), std::string::npos) << result.text;
}

TEST_F(TranslationModelTest, DetectsTheSourceLanguage)
{
    const auto to_english =
        translator->translate("動画を作るのは大変です。", {.source = "auto", .target = "en"});
    const auto to_japanese = translator->translate("Making videos is hard.", {.target = "ja"});

    EXPECT_NE(lower(to_english.text).find("video"), std::string::npos) << to_english.text;
    EXPECT_NE(to_japanese.text.find("動画"), std::string::npos) << to_japanese.text;
}

TEST_F(TranslationModelTest, GreedyDecodingIsDeterministic)
{
    const std::string text = "The weather was nice, so we walked to the station.";

    EXPECT_EQ(translator->translate(text, en_ja).text, translator->translate(text, en_ja).text);
}

TEST_F(TranslationModelTest, BlankTextGivesEmptyText)
{
    EXPECT_EQ(translator->translate("  \n", en_ja).text, "");
}

TEST_F(TranslationModelTest, InvalidLanguagesThrow)
{
    EXPECT_THROW((void)translator->translate("Hi.", {.source = "en", .target = "auto"}),
                 std::invalid_argument);
    EXPECT_THROW((void)translator->translate("Hi.", {.source = "EN", .target = "ja"}),
                 std::invalid_argument);
}

TEST_F(TranslationModelTest, DeltasConcatenateToTheTranslation)
{
    auto        translation_engine = engine();
    std::string deltas;
    int         calls = 0;

    const std::string translation = translation_engine->translate(
        TranslationJob{.text = "Good morning, everyone. Let's begin."}, en_ja,
        [&](std::string_view text) {
            deltas += text;
            ++calls;
        });

    EXPECT_EQ(deltas, translation);
    EXPECT_GT(calls, 1) << "streamed in several pieces";
}

// The KV prefix reuse must not change results: the same jobs give the same translations whether
// each runs on a fresh context or after the previous ones on one context.
TEST_F(TranslationModelTest, PrefixReuseGivesTheSameTranslationsAsAFreshContext)
{
    const std::vector<std::string> sentences{"I wanted to give you a quick update.",
                                             "Making videos can be hard.",
                                             "I have been working as a developer."};
    auto                           reused = engine();
    std::vector<ContextPair>       context;
    const auto                     ignore = [](std::string_view) {};

    for (const std::string& sentence : sentences) {
        const TranslationJob job{.text = sentence, .context = context};
        const std::string    with_reuse = reused->translate(job, en_ja, ignore);
        const std::string    fresh      = engine()->translate(job, en_ja, ignore);

        EXPECT_EQ(with_reuse, fresh) << sentence;
        context.push_back({.source = sentence, .translation = with_reuse});
    }
}

TEST_F(TranslationModelTest, DropsContextThatDoesNotFitThenFails)
{
    auto                     small = engine(256);
    std::vector<ContextPair> context(20, {.source = "This is a fairly long earlier sentence.",
                                          .translation = "これはかなり長い以前の文です。"});
    const auto               ignore = [](std::string_view) {};

    EXPECT_FALSE(small->translate({.text = "Short.", .context = context}, en_ja, ignore).empty());

    std::string long_text;
    for (int i = 0; i < 60; ++i) {
        long_text += "Another sentence. ";
    }
    EXPECT_THROW((void)small->translate({.text = long_text}, en_ja, ignore), std::runtime_error);
}

TEST_F(TranslationModelTest, StreamTranslatesEverySentenceOnce)
{
    struct Collected
    {
        std::mutex               mutex;
        std::vector<std::string> sources;
        std::vector<std::string> finals;
    } collected;
    auto stream = translator->make_stream(
        {.languages = en_ja},
        [](void* user_data, const AudioTranslator::TranslatedSegment& segment) {
            auto& out = *static_cast<Collected*>(user_data);
            if (segment.is_final) {
                const std::scoped_lock lock{out.mutex};
                out.sources.push_back(segment.source_text);
                out.finals.push_back(segment.text);
            }
        },
        &collected);

    // Transcriber-style input: deltas, then the whole utterance.
    ASSERT_TRUE(stream.push({.text = "Hello everyone.", .start_sec = 0.0, .end_sec = 1.0, .is_final = false}));
    ASSERT_TRUE(stream.push({.text = " Today we talk", .start_sec = 0.0, .end_sec = 2.0, .is_final = false}));
    ASSERT_TRUE(stream.push({.text = "Hello everyone. Today we talk about trains.", .start_sec = 0.0,
                             .end_sec = 3.0}));
    ASSERT_TRUE(stream.stop());

    EXPECT_EQ(collected.sources,
              (std::vector<std::string>{"Hello everyone.", "Today we talk about trains."}));
    for (const auto& text : collected.finals) {
        EXPECT_FALSE(text.empty());
    }
}

TEST_F(TranslationModelTest, MakeStreamValidatesItsArguments)
{
    const auto callback = [](void*, const AudioTranslator::TranslatedSegment&) {};

    EXPECT_THROW((void)translator->make_stream({.languages = en_ja}, nullptr, nullptr),
                 std::invalid_argument);
    EXPECT_THROW((void)translator->make_stream({.languages = {.target = ""}}, callback, nullptr),
                 std::invalid_argument);
    EXPECT_THROW(
        (void)translator->make_stream({.languages = en_ja, .max_unit_chars = 8}, callback, nullptr),
        std::invalid_argument);
    EXPECT_THROW((void)translator->make_stream({.languages = en_ja, .fragment_hold_sec = -1.0},
                                               callback, nullptr),
                 std::invalid_argument);
    EXPECT_THROW(
        (void)translator->make_stream({.languages = en_ja, .context_units = 65}, callback, nullptr),
        std::invalid_argument);
    // 6 * 2000 + 96 tokens would not fit context_tokens 4096.
    EXPECT_THROW((void)translator->make_stream({.languages = en_ja, .max_unit_chars = 2000},
                                               callback, nullptr),
                 std::invalid_argument);
    for (const double threshold : {0.0, 1.0}) {
        EXPECT_THROW((void)translator->make_stream(
                         {.languages = en_ja, .sentence_threshold = threshold}, callback, nullptr),
                     std::invalid_argument);
    }
    for (const int lookahead : {0, 17}) {
        EXPECT_THROW((void)translator->make_stream(
                         {.languages = en_ja, .sentence_lookahead_tokens = lookahead}, callback,
                         nullptr),
                     std::invalid_argument);
    }
    // Japanese rules need a Japanese source.
    EXPECT_THROW((void)translator->make_stream(
                     {.languages = en_ja, .japanese_sentence_endings = true}, callback, nullptr),
                 std::invalid_argument);
    EXPECT_THROW((void)translator->make_stream(
                     {.languages = {.target = "en"}, .japanese_sentence_endings = true}, callback,
                     nullptr),
                 std::invalid_argument);
}
