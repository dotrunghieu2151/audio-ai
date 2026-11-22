#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "translation/translation_scheduler.hpp"

using dth::audio::find_forced_cut;
using dth::audio::find_sentence_end;
using dth::audio::TranslationJob;
using dth::audio::TranslationScheduler;

namespace {

using Input = TranslationScheduler::Input;

Input delta(std::string text, double start = 0.0, double end = 0.0)
{
    return {.text = std::move(text), .start_sec = start, .end_sec = end, .is_final = false};
}

Input final_text(std::string text, double start = 0.0, double end = 0.0)
{
    return {.text = std::move(text), .start_sec = start, .end_sec = end, .is_final = true};
}

std::size_t quarter_chars(std::string_view text)
{
    return (text.size() + 3) / 4;
}

class TranslationSchedulerTest : public ::testing::Test
{
  protected:
    TranslationScheduler make(TranslationScheduler::Config config = {})
    {
        return TranslationScheduler{config, quarter_chars};
    }

    // Takes every job, completing each with a translation derived from its text.
    static std::vector<TranslationJob> drain(TranslationScheduler& scheduler)
    {
        std::vector<TranslationJob> jobs;
        while (auto job = scheduler.next_job()) {
            scheduler.complete(job->unit_id, "T(" + job->text + ")");
            jobs.push_back(std::move(*job));
        }
        return jobs;
    }

    static std::vector<std::string> texts(const std::vector<TranslationJob>& jobs)
    {
        std::vector<std::string> out;
        for (const auto& job : jobs) {
            out.push_back(job.text);
        }
        return out;
    }
};

}  // namespace

// --- find_sentence_end -------------------------------------------------------------------------

TEST(FindSentenceEnd, AsciiTerminalsFollowedBySpace)
{
    EXPECT_EQ(find_sentence_end("Hi. There", 0, false), 3U);
    EXPECT_EQ(find_sentence_end("Why? Yes", 0, false), 4U);
    EXPECT_EQ(find_sentence_end("No!! Ok", 0, false), 4U);
    EXPECT_EQ(find_sentence_end("Really?! Ok", 0, false), 8U);
    EXPECT_EQ(find_sentence_end("no end here", 0, true), std::nullopt);
}

// At the end of text that may continue, only a terminal that could still change waits.
TEST(FindSentenceEnd, AmbiguousTerminalAtEndNeedsCompleteText)
{
    EXPECT_EQ(find_sentence_end("It costs 3.", 0, false), std::nullopt);  // "3.5"
    EXPECT_EQ(find_sentence_end("It costs 3.", 0, true), 11U);
    EXPECT_EQ(find_sentence_end("Ask Mr.", 0, false), std::nullopt);       // "Mr. Smith"
    EXPECT_EQ(find_sentence_end("Ask J.", 0, false), std::nullopt);        // "J. Smith"
    EXPECT_EQ(find_sentence_end("Well..", 0, false), std::nullopt);        // "Well..." maybe
    EXPECT_EQ(find_sentence_end("Well…", 0, false), std::nullopt);
}

TEST(FindSentenceEnd, UnambiguousTerminalAtEndEndsAtOnce)
{
    EXPECT_EQ(find_sentence_end("I studied in college.", 0, false), 21U);
    EXPECT_EQ(find_sentence_end("Is it?", 0, false), 6U);
    EXPECT_EQ(find_sentence_end("Stop!", 0, false), 5U);
    EXPECT_EQ(find_sentence_end("Neither do I.", 0, false), 13U);
    EXPECT_EQ(find_sentence_end("Wie geht's?", 0, false), 11U);
}

TEST(FindSentenceEnd, ClosingQuotesBelongToTheSentence)
{
    EXPECT_EQ(find_sentence_end("He said \"go.\" Then", 0, false), 13U);
    EXPECT_EQ(find_sentence_end("(Done.) Next", 0, false), 7U);
    EXPECT_EQ(find_sentence_end("「はい。」次", 0, false), std::string{"「はい。」"}.size());
}

TEST(FindSentenceEnd, NotAtDecimalsAbbreviationsOrInitials)
{
    EXPECT_EQ(find_sentence_end("Pi is 3.14 ok", 0, true), std::nullopt);
    EXPECT_EQ(find_sentence_end("Mr. Smith came", 0, true), std::nullopt);
    EXPECT_EQ(find_sentence_end("Ask Dr. Who", 0, true), std::nullopt);
    EXPECT_EQ(find_sentence_end("Use e.g. this", 0, true), std::nullopt);
    EXPECT_EQ(find_sentence_end("J. Smith wrote", 0, true), std::nullopt);
    EXPECT_EQ(find_sentence_end("the U.S.A is big", 0, true), std::nullopt);
    EXPECT_EQ(find_sentence_end("Mr. Smith came. Then", 0, false), 15U);
}

TEST(FindSentenceEnd, PronounIIsNoInitial)
{
    EXPECT_EQ(find_sentence_end("Neither do I. Then", 0, false), 13U);
}

TEST(FindSentenceEnd, EllipsisEndsOnlyBeforeANewSentence)
{
    EXPECT_EQ(find_sentence_end("well... maybe", 0, true), std::nullopt);
    EXPECT_EQ(find_sentence_end("well... Maybe", 0, true), 7U);
    EXPECT_EQ(find_sentence_end("well… maybe", 0, true), std::nullopt);
    EXPECT_EQ(find_sentence_end("well… Maybe", 0, true), std::string{"well…"}.size());
}

TEST(FindSentenceEnd, CjkTerminalsEndImmediately)
{
    const std::string text = "はい。次";
    EXPECT_EQ(find_sentence_end(text, 0, false), std::string{"はい。"}.size());
    EXPECT_EQ(find_sentence_end("本当！", 0, false), std::string{"本当！"}.size());
    EXPECT_EQ(find_sentence_end("本当？", 0, false), std::string{"本当？"}.size());
}

// Opt-in Japanese rules (japanese_endings): the ASR often leaves out 。. A polite sentence-final
// form ends the sentence when what follows cannot continue it (continuations - が か から ので ね
// よ ... - are hiragana).
constexpr bool japanese = true;

TEST(FindSentenceEnd, JapaneseRulesAreOffByDefault)
{
    EXPECT_EQ(find_sentence_end("テキストに変換します次に検出", 0, true), std::nullopt);
    EXPECT_EQ(find_sentence_end("説明します", 0, true), std::nullopt);
}

TEST(FindSentenceEnd, JapaneseSentenceFinalFormBeforeANewSentence)
{
    const auto length = [](std::string_view text) { return text.size(); };
    EXPECT_EQ(find_sentence_end("テキストに変換します次に検出", 0, false, japanese),
              length("テキストに変換します"));
    EXPECT_EQ(find_sentence_end("送りますモデル", 0, false, japanese), length("送ります"));
    EXPECT_EQ(find_sentence_end("予定です  今後", 0, false, japanese), length("予定です"));
    EXPECT_EQ(find_sentence_end("ありませんAI", 0, false, japanese), length("ありません"));
    EXPECT_EQ(find_sentence_end("問題ありません」次", 0, false, japanese), length("問題ありません」"));
}

TEST(FindSentenceEnd, JapaneseSentenceFinalFormAtTheEndNeedsCompleteText)
{
    EXPECT_EQ(find_sentence_end("説明します", 0, false, japanese), std::nullopt);
    EXPECT_EQ(find_sentence_end("説明します", 0, true, japanese), std::string_view{"説明します"}.size());
    EXPECT_EQ(find_sentence_end("ありがとうございました ", 0, true, japanese),
              std::string_view{"ありがとうございました"}.size());
}

TEST(FindSentenceEnd, JapaneseSentenceFinalFormThatContinuesIsNoBoundary)
{
    EXPECT_EQ(find_sentence_end("説明しますが次に", 0, true, japanese), std::nullopt);
    EXPECT_EQ(find_sentence_end("説明します、次に", 0, true, japanese), std::nullopt);
    EXPECT_EQ(find_sentence_end("雨ですから中止", 0, true, japanese), std::nullopt);
    EXPECT_EQ(find_sentence_end("ますます増える", 0, false, japanese), std::nullopt);
    // "ません" goes on as "ませんでした": one sentence end, at the end.
    EXPECT_EQ(find_sentence_end("知りませんでした", 0, true, japanese),
              std::string_view{"知りませんでした"}.size());
}

TEST(FindSentenceEnd, SentenceMarksOfOtherScripts)
{
    const auto end = [](std::string_view text) { return find_sentence_end(text, 0, false); };
    const auto length = [](std::string_view text) { return text.size(); };
    EXPECT_EQ(end("नमस्ते। आप"), length("नमस्ते।"));        // Devanagari danda
    EXPECT_EQ(end("আমি ভালো আছি। তুমি"), length("আমি ভালো আছি।"));
    EXPECT_EQ(end("كيف حالك؟ أنا"), length("كيف حالك؟"));    // Arabic question mark
    EXPECT_EQ(end("ٹھیک ہے۔ اب"), length("ٹھیک ہے۔"));        // Urdu full stop
    EXPECT_EQ(end("Πού είσαι\u037E Εδώ"), length("Πού είσαι\u037E"));  // Greek question mark
    EXPECT_EQ(end("ሰላም። እንዴት"), length("ሰላም።"));          // Ethiopic full stop
    EXPECT_EQ(end("Բարեւ։ Ինչ"), length("Բարեւ։"));           // Armenian full stop
    EXPECT_EQ(end("សួស្តី។ ខ្ញុំ"), length("សួស្តី។"));          // Khmer
    EXPECT_EQ(end("ဟုတ်ကဲ့။ နေ"), length("ဟုတ်ကဲ့။"));          // Myanmar
    EXPECT_EQ(end("好的．下"), length("好的．"));             // full-width full stop
    EXPECT_EQ(end("本当‼次"), length("本当‼"));
}

TEST(FindSentenceEnd, StartsAtFrom)
{
    EXPECT_EQ(find_sentence_end("One. Two. Three", 5, false), 9U);
}

// --- find_forced_cut ---------------------------------------------------------------------------

TEST(FindForcedCut, PrefersClauseMarks)
{
    EXPECT_EQ(find_forced_cut("one, two three four", 15), 4U);
    EXPECT_EQ(find_forced_cut("一、二三四", 12), std::string{"一、"}.size());
}

TEST(FindForcedCut, FallsBackToWhitespace)
{
    EXPECT_EQ(find_forced_cut("one two three four", 10), 8U);
}

TEST(FindForcedCut, ColonWithoutSpaceIsNoClause)
{
    EXPECT_EQ(find_forced_cut("at 3:45 we left home", 10), 8U);
}

TEST(FindForcedCut, UnpunctuatedTextCutsAtCodepointBoundary)
{
    const std::string text = "あいうえお";  // 15 bytes

    EXPECT_EQ(find_forced_cut(text, 7), 6U);
    EXPECT_EQ(find_forced_cut(text, 2), 3U);  // never empty: one codepoint
}

// --- TranslationScheduler: segmentation ---------------------------------------------------------

TEST_F(TranslationSchedulerTest, FinalUtteranceSplitsIntoSentences)
{
    auto scheduler = make();

    scheduler.push(final_text("Hello there. How are you? Fine.", 1.0, 4.0), 4.0);

    EXPECT_EQ(texts(drain(scheduler)),
              (std::vector<std::string>{"Hello there.", "How are you?", "Fine."}));
}

TEST_F(TranslationSchedulerTest, SentenceCompletedInDeltasCommitsBeforeTheUtteranceEnds)
{
    auto scheduler = make();

    scheduler.push(delta(" I studied", 0.0, 1.0), 1.0);
    scheduler.push(delta(" in college.", 0.0, 2.0), 2.0);
    const auto jobs = drain(scheduler);

    ASSERT_EQ(texts(jobs), (std::vector<std::string>{"I studied in college."}));
    EXPECT_DOUBLE_EQ(jobs[0].end_sec, 2.0);
}

TEST_F(TranslationSchedulerTest, AmbiguousPeriodAtTheEndOfTheDeltasWaitsForTheNext)
{
    auto scheduler = make();

    scheduler.push(delta(" It costs 3.", 0.0, 1.0), 1.0);
    EXPECT_FALSE(scheduler.has_job());  // may become "3.5"

    scheduler.push(delta("5 dollars. Then", 0.0, 2.0), 2.0);
    EXPECT_EQ(texts(drain(scheduler)), (std::vector<std::string>{"It costs 3.5 dollars."}));
}

TEST_F(TranslationSchedulerTest, FinalReplacesDeltasButCommittedUnitsStay)
{
    auto scheduler = make();
    scheduler.push(delta("One done. Tw", 0.0, 1.0), 1.0);
    ASSERT_EQ(texts(drain(scheduler)), (std::vector<std::string>{"One done."}));

    // The final differs from the deltas after the committed part.
    scheduler.push(final_text("One done. Two done.", 0.0, 2.0), 2.0);

    EXPECT_EQ(texts(drain(scheduler)), (std::vector<std::string>{"Two done."}));
}

TEST_F(TranslationSchedulerTest, FinalDivergingInsideCommittedTextKeepsTheRest)
{
    auto scheduler = make();
    scheduler.push(delta("Alpha beta. Gam", 0.0, 1.0), 1.0);
    ASSERT_EQ(texts(drain(scheduler)), (std::vector<std::string>{"Alpha beta."}));

    scheduler.push(final_text("Alpha betas. Gamma.", 0.0, 2.0), 2.0);

    // The committed unit is never retranslated; the text after its length follows the final
    // (here a lone ".", which has nothing to translate).
    EXPECT_EQ(texts(drain(scheduler)), (std::vector<std::string>{"Gamma."}));
}

TEST_F(TranslationSchedulerTest, RevisedFinalResumesAfterTheCommittedSentence)
{
    auto scheduler = make();
    scheduler.push(delta("Hello world. This", 0.0, 1.0), 1.0);
    ASSERT_EQ(texts(drain(scheduler)), (std::vector<std::string>{"Hello world."}));

    // The final revised the committed sentence: no part of it is translated twice.
    scheduler.push(final_text("Hello, world. This is it.", 0.0, 2.0), 2.0);

    EXPECT_EQ(texts(drain(scheduler)), (std::vector<std::string>{"This is it."}));
}

TEST_F(TranslationSchedulerTest, LongSentenceIsCutLikeLongText)
{
    auto scheduler = make({.max_unit_chars = 20});

    scheduler.push(final_text("This sentence is far too long for one unit.", 0.0, 1.0), 1.0);
    scheduler.finish();

    for (const auto& unit : texts(drain(scheduler))) {
        EXPECT_LE(unit.size(), 20U) << unit;
    }
}

TEST_F(TranslationSchedulerTest, PunctuationOnlyTextIsNotAUnit)
{
    auto scheduler = make();

    scheduler.push(final_text("「こんにちは。", 0.0, 1.0), 1.0);
    scheduler.push(final_text("」", 1.1, 1.2), 1.2);
    scheduler.push(final_text("...", 1.3, 1.4), 1.4);
    scheduler.push(final_text("।", 1.5, 1.6), 1.6);
    scheduler.finish();

    EXPECT_EQ(texts(drain(scheduler)), (std::vector<std::string>{"「こんにちは。"}));
}

TEST_F(TranslationSchedulerTest, FragmentWaitsForItsContinuation)
{
    auto scheduler = make({.fragment_hold_sec = 1.0});

    scheduler.push(final_text("I was", 0.0, 1.0), 1.0);
    EXPECT_FALSE(scheduler.has_job());
    EXPECT_EQ(scheduler.next_deadline(), 2.0);

    scheduler.tick(1.5);
    EXPECT_FALSE(scheduler.has_job());

    scheduler.push(final_text("obliged to come.", 1.2, 2.0), 1.6);

    EXPECT_EQ(texts(drain(scheduler)), (std::vector<std::string>{"I was obliged to come."}));
    EXPECT_EQ(scheduler.next_deadline(), std::nullopt);
}

TEST_F(TranslationSchedulerTest, FragmentCommitsAloneWhenItsWaitIsOver)
{
    auto scheduler = make({.fragment_hold_sec = 0.5});

    scheduler.push(final_text("Fragment without end", 0.0, 1.0), 1.0);
    scheduler.tick(1.49);
    EXPECT_FALSE(scheduler.has_job());
    scheduler.tick(1.5);

    EXPECT_EQ(texts(drain(scheduler)), (std::vector<std::string>{"Fragment without end"}));
    EXPECT_EQ(scheduler.next_deadline(), std::nullopt);
}

TEST_F(TranslationSchedulerTest, DeltasOfTheNextUtteranceCancelTheWait)
{
    auto scheduler = make({.fragment_hold_sec = 0.5});
    scheduler.push(final_text("We went", 0.0, 1.0), 1.0);

    scheduler.push(delta(" home", 1.1, 1.3), 1.3);
    EXPECT_EQ(scheduler.next_deadline(), std::nullopt);
    scheduler.tick(5.0);
    EXPECT_FALSE(scheduler.has_job());

    scheduler.push(delta(" early. And", 1.1, 1.6), 1.6);
    EXPECT_EQ(texts(drain(scheduler)), (std::vector<std::string>{"We went home early."}));
}

// --- Model boundaries (Config::find_boundary) ----------------------------------------------------

// Stand-in for the sentence model: a boundary after the first of `words` that has text after it
// (the model needs right context).
TranslationScheduler::BoundaryFinder boundary_after(std::vector<std::string> words)
{
    return [words = std::move(words)](std::string_view text,
                                      std::size_t      from) -> std::optional<std::size_t> {
        std::optional<std::size_t> first;
        for (const std::string& word : words) {
            const std::size_t pos = text.find(word, from);
            if (pos != std::string_view::npos && pos + word.size() < text.size() &&
                (!first || pos + word.size() < *first)) {
                first = pos + word.size();
            }
        }
        return first;
    };
}

TEST_F(TranslationSchedulerTest, ModelBoundaryCommitsUnpunctuatedSentenceMidUtterance)
{
    auto scheduler = make({.find_boundary = boundary_after({"translation"})});

    scheduler.push(delta(" about real time translation", 0.0, 1.0), 1.0);
    EXPECT_FALSE(scheduler.has_job());  // nothing after the boundary yet
    scheduler.push(delta(" the system", 0.0, 1.5), 1.5);

    const auto jobs = drain(scheduler);
    ASSERT_EQ(texts(jobs), (std::vector<std::string>{"about real time translation"}));
    EXPECT_DOUBLE_EQ(jobs[0].end_sec, 1.5);
}

TEST_F(TranslationSchedulerTest, EarlierOfPunctuationAndModelBoundaryWins)
{
    auto scheduler = make({.find_boundary = boundary_after({"変換します", "translation"})});

    scheduler.push(delta("テキストに変換しますに文の区切りを検出します。次", 0.0, 1.0), 1.0);
    scheduler.push(delta(" Hello. real time translation the", 0.0, 2.0), 2.0);

    EXPECT_EQ(texts(drain(scheduler)),
              (std::vector<std::string>{"テキストに変換します", "に文の区切りを検出します。",
                                        "次 Hello.", "real time translation"}));
}

TEST_F(TranslationSchedulerTest, ModelJoinsAHeldFragmentWithTheNextUtterance)
{
    auto scheduler = make({.fragment_hold_sec = 1.0, .find_boundary = boundary_after({"text"})});

    scheduler.push(final_text("it converts speech into text", 0.0, 1.0), 1.0);
    EXPECT_FALSE(scheduler.has_job());  // an utterance end needs text after it
    EXPECT_EQ(scheduler.next_deadline(), 2.0);

    scheduler.push(delta("then it splits", 1.1, 1.4), 1.4);
    EXPECT_EQ(texts(drain(scheduler)), (std::vector<std::string>{"it converts speech into text"}));
}

// The final revised text committed at a model boundary: resume after the committed unit's last
// word in the final, so nothing is lost or translated twice.
TEST_F(TranslationSchedulerTest, RevisedFinalResumesAfterAModelBoundary)
{
    auto scheduler = make({.find_boundary = boundary_after({"are you"})});
    scheduler.push(delta("hello world how are you", 0.0, 1.0), 1.0);
    scheduler.push(delta(" doing", 0.0, 1.5), 1.5);
    ASSERT_EQ(texts(drain(scheduler)), (std::vector<std::string>{"hello world how are you"}));

    scheduler.push(final_text("hello word how are you doing today", 0.0, 2.0), 2.0);
    scheduler.finish();

    EXPECT_EQ(texts(drain(scheduler)), (std::vector<std::string>{"doing today"}));
}

// Unspaced text whose committed end is not in the final: resume where the final differs
// rather than dropping the rest.
TEST_F(TranslationSchedulerTest, RevisedFinalAfterAModelBoundaryKeepsUnspacedText)
{
    auto scheduler = make({.find_boundary = boundary_after({"雨です"})});
    scheduler.push(delta("今日は雨です明日は", 0.0, 1.0), 1.0);
    ASSERT_EQ(texts(drain(scheduler)), (std::vector<std::string>{"今日は雨です"}));

    scheduler.push(final_text("今日は雨でした明日は晴れ", 0.0, 2.0), 2.0);
    scheduler.finish();

    const auto rest = texts(drain(scheduler));
    ASSERT_EQ(rest.size(), 1U);
    EXPECT_TRUE(rest[0].ends_with("明日は晴れ")) << rest[0];
}

TEST_F(TranslationSchedulerTest, RevisedFinalResumesAfterAJapaneseBoundary)
{
    auto scheduler = make({.japanese_sentence_endings = true});
    scheduler.push(delta("今日は雨です明日は", 0.0, 1.0), 1.0);
    ASSERT_EQ(texts(drain(scheduler)), (std::vector<std::string>{"今日は雨です"}));

    scheduler.push(final_text("今日は雨でした。明日は晴れ", 0.0, 2.0), 2.0);
    scheduler.finish();

    EXPECT_EQ(texts(drain(scheduler)), (std::vector<std::string>{"明日は晴れ"}));
}

// The model misjudges boundaries without the sentence's start: it gets the end of the committed
// text as left context, and only boundaries in the pending text count.
TEST_F(TranslationSchedulerTest, ModelSeesTheCommittedTextAsLeftContext)
{
    std::vector<std::string> contexts;
    auto scheduler = make({.find_boundary = [&contexts](std::string_view text,
                                                        std::size_t from) -> std::optional<std::size_t> {
        contexts.emplace_back(text.substr(0, from));
        return from > 0 ? std::optional<std::size_t>{from / 2} : std::nullopt;  // inside the context
    }});

    scheduler.push(delta("First sentence. second part", 0.0, 1.0), 1.0);
    scheduler.push(delta(" goes on", 0.0, 2.0), 2.0);

    EXPECT_EQ(texts(drain(scheduler)), (std::vector<std::string>{"First sentence."}));
    ASSERT_FALSE(contexts.empty());
    EXPECT_TRUE(std::string_view{contexts.back()}.starts_with("First sentence.")) << contexts.back();
}

TEST_F(TranslationSchedulerTest, ModelBoundaryAtTheEdgesIsIgnored)
{
    auto scheduler = make({.find_boundary = [](std::string_view text,
                                               std::size_t from) -> std::optional<std::size_t> {
        return text.size() % 2 == 0 ? from : text.size();
    }});

    scheduler.push(delta("no boundary", 0.0, 1.0), 1.0);
    scheduler.push(delta(" here", 0.0, 1.5), 1.5);
    EXPECT_FALSE(scheduler.has_job());
}

// --- Speakers (push_speakers) ------------------------------------------------------------------

using SpeakerUpdate = TranslationScheduler::SpeakerUpdate;

std::vector<std::string> speakers_of(const std::vector<TranslationJob>& jobs)
{
    std::vector<std::string> out;
    for (const auto& job : jobs) {
        out.push_back(job.speaker);
    }
    return out;
}

TEST_F(TranslationSchedulerTest, SpeakerTurnEndsAUnit)
{
    auto scheduler = make();
    scheduler.push_speakers({.turns = {{"speaker_0", 0.0, 2.2}, {"speaker_1", 2.5, 4.0}},
                             .complete_until_sec = 5.0});

    scheduler.push(delta("it converts speech", 0.0, 1.0), 1.0);
    scheduler.push(delta(" into text", 0.0, 2.0), 2.0);
    scheduler.push(delta(" yes it does", 0.0, 3.0), 3.0);
    auto jobs = drain(scheduler);
    ASSERT_EQ(texts(jobs), (std::vector<std::string>{"it converts speech into text"}));
    EXPECT_EQ(speakers_of(jobs), (std::vector<std::string>{"speaker_0"}));

    scheduler.finish();
    jobs = drain(scheduler);
    ASSERT_EQ(texts(jobs), (std::vector<std::string>{"yes it does"}));
    EXPECT_EQ(speakers_of(jobs), (std::vector<std::string>{"speaker_1"}));
}

// Turns are reported after they end: a unit waits until they are known through its end, so a
// turn reported late still splits it. Text of a turn still in progress has no speaker yet.
TEST_F(TranslationSchedulerTest, UnitWaitsUntilTurnsAreKnownThroughItsEnd)
{
    auto scheduler = make();
    scheduler.push_speakers({.turns = {}, .complete_until_sec = 1.0});

    scheduler.push(delta("Wow, that is great", 0.0, 1.5), 1.5);
    scheduler.push(delta(" I want to go. Me", 0.0, 2.5), 2.5);
    EXPECT_FALSE(scheduler.has_job());

    // A turn end alone may be the speaker's own pause: the next turn decides.
    scheduler.push_speakers({.turns = {{"speaker_0", 0.0, 1.7}}, .complete_until_sec = 3.0});
    EXPECT_FALSE(scheduler.has_job());

    scheduler.push_speakers({.turns = {{"speaker_1", 1.8, 2.6}}, .complete_until_sec = 3.5});
    const auto jobs = drain(scheduler);
    EXPECT_EQ(texts(jobs), (std::vector<std::string>{"Wow, that is great", "I want to go."}));
    EXPECT_EQ(speakers_of(jobs), (std::vector<std::string>{"speaker_0", "speaker_1"}));
}

TEST_F(TranslationSchedulerTest, HeldFragmentDoesNotMergeAcrossASpeakerChange)
{
    auto scheduler = make({.fragment_hold_sec = 1.0});
    scheduler.push(final_text("Switzerland was so beautiful", 0.0, 1.0), 1.0);
    scheduler.push_speakers({.turns = {{"speaker_0", 0.0, 1.05}}, .complete_until_sec = 2.0});
    EXPECT_FALSE(scheduler.has_job());
    scheduler.push_speakers({.turns = {{"speaker_1", 1.2, 2.0}}, .complete_until_sec = 2.5});

    scheduler.push(delta("I want to go", 1.2, 1.8), 1.8);

    EXPECT_EQ(texts(drain(scheduler)), (std::vector<std::string>{"Switzerland was so beautiful"}));
}

// The hold of a fragment can expire before the diarizer caught up: the held text still waits
// for its turns, and its sentences still become separate units.
TEST_F(TranslationSchedulerTest, ExpiredHoldWaitsForSpeakers)
{
    auto scheduler = make({.fragment_hold_sec = 0.5});
    scheduler.push_speakers({.turns = {}, .complete_until_sec = 0.5});
    scheduler.push(final_text("Are you coming? Yes I am", 0.0, 2.0), 2.0);
    EXPECT_EQ(scheduler.next_deadline(), std::nullopt);  // waiting for speakers, not the clock

    scheduler.tick(3.0);
    EXPECT_FALSE(scheduler.has_job());

    scheduler.push_speakers({.turns = {{"speaker_0", 0.0, 1.0}, {"speaker_1", 1.2, 2.0}},
                             .complete_until_sec = 2.5});
    scheduler.tick(3.1);
    const auto jobs = drain(scheduler);
    EXPECT_EQ(texts(jobs), (std::vector<std::string>{"Are you coming?", "Yes I am"}));
}

// A speaker's own pause ends a turn: no cut there, the sentence stays one unit.
TEST_F(TranslationSchedulerTest, SameSpeakerPauseDoesNotSplitASentence)
{
    auto scheduler = make({.fragment_hold_sec = 0.5});
    scheduler.push_speakers({.turns = {{"speaker_0", 0.0, 1.0}, {"speaker_0", 1.4, 3.0}},
                             .complete_until_sec = 4.0});
    scheduler.push(final_text("When I was a child", 0.0, 1.0), 1.0);
    scheduler.push(delta(" I was the same", 1.5, 2.5), 2.5);
    scheduler.push(final_text("I was the same as now.", 1.5, 3.0), 3.0);

    const auto jobs = drain(scheduler);
    EXPECT_EQ(texts(jobs), (std::vector<std::string>{"When I was a child I was the same as now."}));
    EXPECT_EQ(speakers_of(jobs), (std::vector<std::string>{"speaker_0"}));
}

// The low-latency diarizer flips to another voice for a few frames: no speaker change.
TEST_F(TranslationSchedulerTest, ShortTurnIsNoSpeakerChange)
{
    auto scheduler = make();
    scheduler.push_speakers({.turns = {{"speaker_1", 0.0, 1.5},
                                       {"speaker_0", 1.5, 1.8},
                                       {"speaker_1", 1.8, 3.5}},
                             .complete_until_sec = 4.0});
    scheduler.push(delta("a one hour", 0.0, 1.4), 1.4);
    scheduler.push(delta(" conversation is", 0.0, 2.0), 2.0);
    scheduler.push(delta(" planned. Then", 0.0, 3.2), 3.2);

    EXPECT_EQ(texts(drain(scheduler)),
              (std::vector<std::string>{"a one hour conversation is planned."}));
}

// The diarizer briefly hears another voice inside one speaker's long turn: no change.
TEST_F(TranslationSchedulerTest, TurnInsideALongerTurnOfAnotherSpeakerIsNoChange)
{
    auto scheduler = make();
    scheduler.push_speakers({.turns = {{"speaker_2", 0.0, 1.3},
                                       {"speaker_1", 0.5, 1.2},
                                       {"speaker_2", 1.4, 4.0},
                                       {"speaker_1", 1.5, 2.5}},
                             .complete_until_sec = 5.0});
    scheduler.push(delta("as for that", 0.0, 0.9), 0.9);
    scheduler.push(delta(" ticket we could", 0.0, 1.8), 1.8);
    scheduler.push(delta(" demo it sooner. And", 0.0, 3.5), 3.5);

    EXPECT_EQ(texts(drain(scheduler)),
              (std::vector<std::string>{"as for that ticket we could demo it sooner."}));
}

// A mid-sentence turn end whose next turn is not reported keeps the sentence whole once the
// speaker updates are speaker_change_wait_sec past it: the speaker kept talking.
TEST_F(TranslationSchedulerTest, UnreportedNextTurnKeepsTheSentenceAfterTheWait)
{
    auto scheduler = make({.speaker_change_wait_sec = 2.0});
    scheduler.push_speakers({.turns = {{"speaker_0", 0.0, 1.0}}, .complete_until_sec = 1.5});
    scheduler.push(delta("I went to", 0.0, 0.9), 0.9);
    scheduler.push(delta(" Switzerland in May. And", 0.0, 2.5), 2.5);
    scheduler.push_speakers({.turns = {}, .complete_until_sec = 3.0});
    EXPECT_FALSE(scheduler.has_job());

    scheduler.push_speakers({.turns = {}, .complete_until_sec = 3.4});
    EXPECT_EQ(texts(drain(scheduler)),
              (std::vector<std::string>{"I went to Switzerland in May."}));
}

TEST_F(TranslationSchedulerTest, LeadingClauseMarkIsDroppedFromAUnit)
{
    auto scheduler = make({.fragment_hold_sec = 0.5});
    scheduler.push(final_text("話しましょう", 0.0, 1.0), 1.0);
    scheduler.tick(1.6);
    scheduler.push(final_text("、はい、どうですか。", 1.0, 2.0), 2.0);

    EXPECT_EQ(texts(drain(scheduler)),
              (std::vector<std::string>{"話しましょう", "はい、どうですか。"}));
}

TEST_F(TranslationSchedulerTest, WithoutSpeakerUpdatesUnitsAreNotHeldBack)
{
    auto scheduler = make();
    scheduler.push(delta("Hello there. Next", 0.0, 1.0), 1.0);
    const auto jobs = drain(scheduler);
    ASSERT_EQ(texts(jobs), (std::vector<std::string>{"Hello there."}));
    EXPECT_EQ(jobs[0].speaker, "");
}

TEST_F(TranslationSchedulerTest, FinishCommitsWithoutWaitingForSpeakers)
{
    auto scheduler = make();
    scheduler.push_speakers({.turns = {{"speaker_0", 0.0, 1.4}, {"speaker_1", 1.5, 2.5}},
                             .complete_until_sec = 0.5});
    scheduler.push(delta("first part", 0.0, 1.0), 1.0);
    scheduler.push(delta(" second part", 0.0, 2.0), 2.0);
    EXPECT_FALSE(scheduler.has_job());

    scheduler.finish();
    const auto jobs = drain(scheduler);
    EXPECT_EQ(texts(jobs), (std::vector<std::string>{"first part", "second part"}));
    EXPECT_EQ(speakers_of(jobs), (std::vector<std::string>{"speaker_0", "speaker_1"}));
}

// --- Opt-in Japanese rules (japanese_sentence_endings) -----------------------------------------

TEST_F(TranslationSchedulerTest, JapaneseSentenceFinalFormIsNoBoundaryByDefault)
{
    auto scheduler = make({.fragment_hold_sec = 1.0});

    scheduler.push(delta("テキストに変換します次に区切りを", 0.0, 1.0), 1.0);
    scheduler.push(final_text("このシステムは翻訳します", 0.0, 1.2), 1.2);
    EXPECT_FALSE(scheduler.has_job());
    EXPECT_EQ(scheduler.next_deadline(), 2.2);
}

// The ASR often drops 。 at a pause. An utterance that ends in a polite sentence-final form
// ends its sentence: it commits at once instead of waiting for (or merging with) the next one.
TEST_F(TranslationSchedulerTest, JapaneseSentenceFinalFormAtUtteranceEndCommitsWithoutWait)
{
    auto scheduler = make({.fragment_hold_sec = 1.0, .japanese_sentence_endings = true});

    scheduler.push(final_text("このシステムは音声を英語に翻訳します", 0.0, 1.0), 1.0);
    EXPECT_EQ(scheduler.next_deadline(), std::nullopt);
    EXPECT_EQ(texts(drain(scheduler)),
              (std::vector<std::string>{"このシステムは音声を英語に翻訳します"}));

    scheduler.push(final_text("ご清聴ありがとうございました ", 1.1, 2.0), 2.0);
    scheduler.push(final_text("問題ありません」", 2.1, 3.0), 3.0);
    EXPECT_EQ(texts(drain(scheduler)), (std::vector<std::string>{"ご清聴ありがとうございました",
                                                                 "問題ありません」"}));
}

TEST_F(TranslationSchedulerTest, JapaneseSentenceFinalFormInsideTheTextIsNoBoundary)
{
    auto scheduler = make({.fragment_hold_sec = 1.0, .japanese_sentence_endings = true});

    // Deltas: the speaker may go on ("ますが、..."), so no commit before the utterance ends.
    scheduler.push(delta("説明します", 0.0, 1.0), 1.0);
    EXPECT_FALSE(scheduler.has_job());
    // A final that ends mid-sentence ("ますが") still waits for its continuation.
    scheduler.push(final_text("説明しますが", 0.0, 1.2), 1.2);
    EXPECT_FALSE(scheduler.has_job());
    EXPECT_EQ(scheduler.next_deadline(), 2.2);
}

TEST_F(TranslationSchedulerTest, JapaneseSentenceWithoutPeriodCommitsMidUtterance)
{
    auto scheduler = make({.japanese_sentence_endings = true});

    scheduler.push(delta("テキストに変換します", 0.0, 1.0), 1.0);
    EXPECT_FALSE(scheduler.has_job());
    scheduler.push(delta("次に区切りを", 0.0, 1.5), 1.5);

    EXPECT_EQ(texts(drain(scheduler)), (std::vector<std::string>{"テキストに変換します"}));
}

// A pause cut the utterance inside the verb: "送り" + "ます翻訳は...".
TEST_F(TranslationSchedulerTest, JapaneseVerbSplitAcrossUtterancesCommitsOnceComplete)
{
    auto scheduler = make({.japanese_sentence_endings = true});

    scheduler.push(final_text("モデルに送り", 0.0, 1.0), 1.0);
    scheduler.push(delta("ます翻訳は", 1.1, 1.5), 1.5);

    EXPECT_EQ(texts(drain(scheduler)), (std::vector<std::string>{"モデルに送ります"}));
}

TEST_F(TranslationSchedulerTest, JapaneseUtterancesJoinWithoutSpace)
{
    auto scheduler = make();

    scheduler.push(final_text("動画を作るのは", 0.0, 1.0), 1.0);
    scheduler.push(final_text("大変です。", 1.1, 2.0), 2.0);

    EXPECT_EQ(texts(drain(scheduler)), (std::vector<std::string>{"動画を作るのは大変です。"}));
}

TEST_F(TranslationSchedulerTest, LongTextIsCutAtClausesThenWords)
{
    auto scheduler = make({.max_unit_chars = 20});

    scheduler.push(final_text("first part, second part goes on and on", 0.0, 3.0), 3.0);
    scheduler.finish();

    EXPECT_EQ(texts(drain(scheduler)),
              (std::vector<std::string>{"first part,", "second part goes", "on and on"}));
}

// Without a clause mark within max_unit_chars (bytes), text runs on to twice the limit for one,
// and is only then cut inside a word ("いただけ|れば" split a sentence).
TEST_F(TranslationSchedulerTest, UnpunctuatedJapaneseRunsOnToTwiceTheLimitBeforeACut)
{
    auto scheduler = make({.max_unit_chars = 16});

    scheduler.push(delta("あいうえおかきくけこ", 0.0, 1.0), 1.0);  // 30 bytes
    EXPECT_FALSE(scheduler.has_job());

    scheduler.push(delta("さしすせそ", 0.0, 2.0), 2.0);
    EXPECT_EQ(texts(drain(scheduler)), (std::vector<std::string>{"あいうえおかきくけこ"}));
}

TEST_F(TranslationSchedulerTest, ClauseMarkPastTheLimitIsPreferredToACutInsideAWord)
{
    auto scheduler = make({.max_unit_chars = 16});

    scheduler.push(delta("あいうえおか、きくけこ", 0.0, 1.0), 1.0);

    EXPECT_EQ(texts(drain(scheduler)), (std::vector<std::string>{"あいうえおか、"}));
}

TEST_F(TranslationSchedulerTest, FinishCommitsEverythingPending)
{
    auto scheduler = make();
    scheduler.push(final_text("held fragment", 0.0, 1.0), 1.0);
    scheduler.push(delta(" and open text", 1.1, 2.0), 2.0);

    scheduler.finish();

    EXPECT_EQ(texts(drain(scheduler)),
              (std::vector<std::string>{"held fragment and open text"}));
}

TEST_F(TranslationSchedulerTest, BlankInputMakesNoUnits)
{
    auto scheduler = make();

    scheduler.push(delta("", 0.0, 0.5), 0.5);
    scheduler.push(final_text("   ", 0.0, 1.0), 1.0);
    scheduler.push(final_text("", 1.0, 2.0), 2.0);
    scheduler.finish();

    EXPECT_FALSE(scheduler.has_job());
    EXPECT_EQ(scheduler.next_deadline(), std::nullopt);
}

TEST_F(TranslationSchedulerTest, UnitIdsIncreaseAndTimesFollowTheInput)
{
    auto scheduler = make();

    scheduler.push(final_text("One. Two.", 1.0, 3.0), 3.0);
    scheduler.push(final_text("Three.", 4.0, 5.0), 5.0);
    const auto jobs = drain(scheduler);

    ASSERT_EQ(jobs.size(), 3U);
    EXPECT_LT(jobs[0].unit_id, jobs[1].unit_id);
    EXPECT_LT(jobs[1].unit_id, jobs[2].unit_id);
    EXPECT_DOUBLE_EQ(jobs[0].start_sec, 1.0);
    EXPECT_DOUBLE_EQ(jobs[2].start_sec, 4.0);
    EXPECT_DOUBLE_EQ(jobs[2].end_sec, 5.0);
}

// --- TranslationScheduler: jobs and context -----------------------------------------------------

TEST_F(TranslationSchedulerTest, JobsAreSequential)
{
    auto scheduler = make();
    scheduler.push(final_text("One. Two.", 0.0, 1.0), 1.0);

    const auto first = scheduler.next_job();
    ASSERT_TRUE(first);
    EXPECT_FALSE(scheduler.has_job());
    EXPECT_FALSE(scheduler.next_job().has_value());

    scheduler.complete(first->unit_id, "1.");
    EXPECT_TRUE(scheduler.has_job());
}

TEST_F(TranslationSchedulerTest, CompleteRejectsAnotherUnit)
{
    auto scheduler = make();
    scheduler.push(final_text("One.", 0.0, 1.0), 1.0);
    const auto job = scheduler.next_job();
    ASSERT_TRUE(job);

    EXPECT_THROW(scheduler.complete(job->unit_id + 1, "x"), std::logic_error);
}

TEST_F(TranslationSchedulerTest, EarlierUnitsOfTheParagraphAreContext)
{
    auto scheduler = make();
    scheduler.push(final_text("One. Two. Three.", 0.0, 1.0), 1.0);

    const auto jobs = drain(scheduler);

    ASSERT_EQ(jobs.size(), 3U);
    EXPECT_TRUE(jobs[0].context.empty());
    ASSERT_EQ(jobs[2].context.size(), 2U);
    EXPECT_EQ(jobs[2].context[0].source, "One.");
    EXPECT_EQ(jobs[2].context[0].translation, "T(One.)");
    EXPECT_EQ(jobs[2].context[1].source, "Two.");
}

TEST_F(TranslationSchedulerTest, ParagraphGapClearsContextAndEndsTheHeldFragment)
{
    auto scheduler = make({.paragraph_gap_sec = 3.0});
    scheduler.push(final_text("First topic. Unfinished", 0.0, 2.0), 2.0);

    scheduler.push(final_text("New topic.", 5.0, 6.0), 6.0);
    const auto jobs = drain(scheduler);

    ASSERT_EQ(texts(jobs),
              (std::vector<std::string>{"First topic.", "Unfinished", "New topic."}));
    EXPECT_EQ(jobs[1].context.size(), 1U);
    EXPECT_TRUE(jobs[2].context.empty());
}

TEST_F(TranslationSchedulerTest, ContextWindowDropsOldPairsInBlocks)
{
    auto scheduler = make({.context_units = 4, .context_tokens = 1000});
    scheduler.push(final_text("A1. A2. A3. A4. A5. A6. A7.", 0.0, 1.0), 1.0);

    const auto jobs = drain(scheduler);

    std::vector<std::size_t> sizes;
    std::vector<std::string> first_sources;
    for (const auto& job : jobs) {
        sizes.push_back(job.context.size());
        first_sources.push_back(job.context.empty() ? "" : job.context.front().source);
    }
    // At 5 pairs the window drops half (2): the first pair then stays the same for a while.
    EXPECT_EQ(sizes, (std::vector<std::size_t>{0, 1, 2, 3, 4, 3, 4}));
    EXPECT_EQ(first_sources,
              (std::vector<std::string>{"", "A1.", "A1.", "A1.", "A1.", "A3.", "A3."}));
}

TEST_F(TranslationSchedulerTest, ContextRespectsTheTokenBudget)
{
    // Each pair "Sentence N." + "T(Sentence N.)": 3 + 4 tokens with chars/4.
    auto scheduler = make({.context_units = 64, .context_tokens = 14});
    scheduler.push(final_text("Sentence 1. Sentence 2. Sentence 3. Sentence 4.", 0.0, 1.0), 1.0);

    for (const auto& job : drain(scheduler)) {
        std::size_t tokens = 0;
        for (const auto& pair : job.context) {
            tokens += quarter_chars(pair.source) + quarter_chars(pair.translation);
        }
        EXPECT_LE(tokens, 14U);
    }
}

TEST_F(TranslationSchedulerTest, ZeroContextUnitsDisablesContext)
{
    auto scheduler = make({.context_units = 0});
    scheduler.push(final_text("One. Two. Three.", 0.0, 1.0), 1.0);

    for (const auto& job : drain(scheduler)) {
        EXPECT_TRUE(job.context.empty());
    }
}

TEST_F(TranslationSchedulerTest, EmptyTranslationsAreNotContext)
{
    auto scheduler = make();
    scheduler.push(final_text("One. Two.", 0.0, 1.0), 1.0);

    auto first = scheduler.next_job();
    ASSERT_TRUE(first);
    scheduler.complete(first->unit_id, "  ");
    const auto second = scheduler.next_job();

    ASSERT_TRUE(second);
    EXPECT_TRUE(second->context.empty());
}
