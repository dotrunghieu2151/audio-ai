// SentenceBoundaryModel with the SaT model in assets/models/sat-12l-sm (skipped if absent).
// Goldens: token ids / byte ends from the Hugging Face xlm-roberta-base tokenizer; boundaries
// from the same ONNX model run from Python (probability >= 0.8, one token after it).
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "translation/sentence_boundary_model.hpp"

using dth::audio::SentenceBoundaryModel;

namespace {

const std::filesystem::path model_dir = DTH_TEST_ASSETS_DIR "/models/sat-12l-sm";

// Our nemotron transcript with punctuation removed (incl. the ASR's "変換しますに文の" garble).
const std::string japanese =
    "本日は新しい翻訳システムについてご説明しますこのシステムは音声をリアルタイムで文字に起こしす"
    "ぐに英語に翻訳します会議や公園などさまざまな場面で使うことができますまずマイクから入力された"
    "音声を音声に認識モデルがテキストに変換しますに文の区切りを検出して翻訳モデルに送ります翻訳は"
    "一問ずつ行われるので話している途中でも結果が表示されます";
const std::string english =
    "Today I want to talk about real time translation the system listens to speech and converts "
    "it into text then it splits the text into sentences and translates each one our goal is to "
    "show the translation less than one second after the speaker finishes a sentence this "
    "requires careful engineering of every stage";

class SentenceBoundaryModelTest : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        if (std::filesystem::exists(model_dir)) {
            model = std::make_unique<SentenceBoundaryModel>(model_dir);
        }
    }

    static void TearDownTestSuite() { model.reset(); }

    void SetUp() override
    {
        if (!model) {
            GTEST_SKIP() << "model not found: " << model_dir;
        }
    }

    // Every boundary as the scheduler would see it with the defaults.
    static std::vector<std::size_t> boundaries(const std::string& text)
    {
        const auto scored = model->score(text);
        std::vector<std::size_t> out;
        for (std::size_t i = 0; i + 1 < scored.size(); ++i) {
            if (scored[i].probability >= 0.8F) {
                out.push_back(scored[i].end);
            }
        }
        return out;
    }

    static inline std::unique_ptr<SentenceBoundaryModel> model;
};

}  // namespace

TEST_F(SentenceBoundaryModelTest, TokenizesLikeXlmRoberta)
{
    struct Case
    {
        std::string               text;
        std::vector<std::int64_t> ids;
        std::vector<std::size_t>  ends;
    };
    const std::vector<Case> cases = {
        {"Hello world", {35378, 8999}, {5, 11}},
        {" the system listens", {70, 5426, 44632, 7}, {4, 11, 18, 19}},
        {"Grüße aus Köln", {184191, 1312, 77399}, {7, 11, 17}},
        {"AI翻訳は3.5秒です", {38730, 228244, 342, 72516, 21075, 1453}, {2, 8, 11, 14, 17, 23}},
        {"本日は新しい翻訳システムについてご説明します",
         {19071, 56444, 38851, 228244, 43415, 12081, 5496, 26795, 5182},
         {3, 9, 18, 24, 36, 48, 51, 57, 66}},
    };
    for (const Case& c : cases) {
        const auto tokens = model->tokenize(c.text);
        EXPECT_EQ(tokens.ids, c.ids) << c.text;
        EXPECT_EQ(tokens.ends, c.ends) << c.text;
    }
}

TEST_F(SentenceBoundaryModelTest, FindsSentencesOfUnpunctuatedJapanese)
{
    EXPECT_EQ(boundaries(japanese), (std::vector<std::size_t>{66, 168, 240, 342, 405}));
}

TEST_F(SentenceBoundaryModelTest, FindsSentencesOfUnpunctuatedEnglish)
{
    EXPECT_EQ(boundaries(english), (std::vector<std::size_t>{48, 166, 261}));
}

TEST_F(SentenceBoundaryModelTest, FirstBoundaryNeedsTextAfterIt)
{
    EXPECT_EQ(model->first_boundary(japanese, 0.8F, 1), 66U);
    // The first sentence alone: nothing after its end, so no boundary yet.
    EXPECT_EQ(model->first_boundary(japanese.substr(0, 66), 0.8F, 1), std::nullopt);
    EXPECT_EQ(model->first_boundary("", 0.8F, 1), std::nullopt);
    // Text before `from` is left context: its boundaries are skipped.
    EXPECT_EQ(model->first_boundary(japanese, 0.8F, 1, 66), 168U);
}

TEST_F(SentenceBoundaryModelTest, ScoresAtMostMaxTokens)
{
    std::string long_text;
    while (long_text.size() < 8000) {
        long_text += english + " ";
    }
    EXPECT_EQ(model->score(long_text).size(), SentenceBoundaryModel::max_tokens);
    EXPECT_TRUE(model->score("").empty());
}

TEST(SentenceBoundaryModelLoad, MissingFilesThrow)
{
    EXPECT_THROW(SentenceBoundaryModel{"/nonexistent/sat"}, std::runtime_error);
}
