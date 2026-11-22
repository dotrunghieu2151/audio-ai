#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <latch>
#include <memory>
#include <mutex>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "translation/translation_worker.hpp"

using dth::audio::AudioTranslator;
using dth::audio::TextStream;
using dth::audio::TranslationJob;
using dth::audio::TranslationWorker;
using dth::audio::UnitTranslator;

namespace {

using Segment = AudioTranslator::TranslatedSegment;

// Translates "X" to "<X>", emitted as one delta per byte group of 2, so units have several
// deltas. Optionally blocks inside a translation until released, or throws.
class FakeTranslator final : public UnitTranslator
{
  public:
    struct Control
    {
        std::binary_semaphore entered{0};  // released when a translation starts
        std::binary_semaphore release{1};  // acquired before translating: hold it to block
        std::atomic<bool>     fail{false};
        std::mutex            mutex;
        std::vector<TranslationJob> jobs;
    };

    explicit FakeTranslator(std::shared_ptr<Control> control) : control_{std::move(control)} {}

    std::string translate(const TranslationJob& job, const AudioTranslator::Languages& /*languages*/,
                          const TextSink& on_text) override
    {
        {
            const std::scoped_lock lock{control_->mutex};
            control_->jobs.push_back(job);
        }
        control_->entered.release();
        control_->release.acquire();
        control_->release.release();
        if (control_->fail) {
            throw std::runtime_error("model exploded");
        }
        const std::string translation = "<" + job.text + ">";
        for (std::size_t pos = 0; pos < translation.size(); pos += 2) {
            on_text(std::string_view{translation}.substr(pos, 2));
        }
        return translation;
    }

    std::size_t count_tokens(std::string_view text) override { return text.size() / 4; }

  private:
    std::shared_ptr<Control> control_;
};

struct Recorder
{
    std::mutex                   mutex;
    std::vector<Segment>         segments;
    std::vector<std::thread::id> threads;
    std::promise<void>           first_final;
    bool                         first_final_set = false;

    static void callback(void* user_data, const Segment& segment)
    {
        auto&                  self = *static_cast<Recorder*>(user_data);
        const std::scoped_lock lock{self.mutex};
        self.segments.push_back(segment);
        self.threads.push_back(std::this_thread::get_id());
        if (segment.is_final && !self.first_final_set) {
            self.first_final_set = true;
            self.first_final.set_value();
        }
    }

    std::vector<Segment> finals()
    {
        const std::scoped_lock lock{mutex};
        std::vector<Segment>   out;
        for (const auto& segment : segments) {
            if (segment.is_final) {
                out.push_back(segment);
            }
        }
        return out;
    }
};

class TranslationWorkerTest : public ::testing::Test
{
  protected:
    std::unique_ptr<TranslationWorker> make(dth::audio::TranslationScheduler::Config config = {})
    {
        return std::make_unique<TranslationWorker>(
            std::make_unique<FakeTranslator>(control),
            TranslationWorker::Settings{.languages = {.source = "auto", .target = "ja"},
                                        .scheduler = config,
                                        .callback  = &Recorder::callback,
                                        .user_data = &recorder});
    }

    static TextStream::Text final_text(std::string_view text, double start = 0.0, double end = 0.0)
    {
        return {.text = text, .start_sec = start, .end_sec = end, .is_final = true};
    }

    std::shared_ptr<FakeTranslator::Control> control = std::make_shared<FakeTranslator::Control>();
    Recorder                                 recorder;  // outlives the workers (declared first)
};

}  // namespace

TEST_F(TranslationWorkerTest, DeltasThenOneFinalPerUnitInOrderOnTheWorkerThread)
{
    auto worker = make();

    ASSERT_TRUE(worker->push(final_text("One. Two.", 1.0, 2.0)));
    ASSERT_TRUE(worker->stop());

    const std::scoped_lock lock{recorder.mutex};
    std::string            deltas;
    std::vector<std::string> finals;
    std::uint64_t          last_id = 0;
    for (std::size_t i = 0; i < recorder.segments.size(); ++i) {
        const Segment& segment = recorder.segments[i];
        EXPECT_NE(recorder.threads[i], std::this_thread::get_id());
        EXPECT_GE(segment.unit_id, last_id);
        last_id = segment.unit_id;
        if (segment.is_final) {
            EXPECT_EQ(deltas, segment.text) << "deltas concatenate to the final";
            finals.push_back(segment.source_text + "=" + segment.text);
            deltas.clear();
        }
        else {
            deltas += segment.text;
        }
    }
    EXPECT_EQ(finals, (std::vector<std::string>{"One.=<One.>", "Two.=<Two.>"}));
}

TEST_F(TranslationWorkerTest, StopTranslatesPendingText)
{
    auto worker = make({.fragment_hold_sec = 60.0});
    ASSERT_TRUE(worker->push({.text = "no sentence end", .is_final = false}));

    ASSERT_TRUE(worker->stop());

    const auto finals = recorder.finals();
    ASSERT_EQ(finals.size(), 1U);
    EXPECT_EQ(finals[0].text, "<no sentence end>");
    EXPECT_FALSE(worker->is_running());
}

TEST_F(TranslationWorkerTest, HeldFragmentIsTranslatedWhenItsWaitIsOver)
{
    auto worker = make({.fragment_hold_sec = 0.05});
    auto done   = recorder.first_final.get_future();

    ASSERT_TRUE(worker->push(final_text("a fragment")));

    // No stop(): the worker wakes for the hold deadline by itself.
    ASSERT_EQ(done.wait_for(std::chrono::seconds(10)), std::future_status::ready);
    EXPECT_EQ(recorder.finals().at(0).text, "<a fragment>");
    ASSERT_TRUE(worker->stop());
}

TEST_F(TranslationWorkerTest, PushAfterStopFails)
{
    auto worker = make();
    ASSERT_TRUE(worker->stop());

    EXPECT_FALSE(worker->push(final_text("Late.")));
    EXPECT_TRUE(worker->stop()) << "stop is idempotent";
}

TEST_F(TranslationWorkerTest, TranslatorFailureEndsTheStreamAndIsReportedByStop)
{
    control->fail = true;
    auto worker   = make();
    ASSERT_TRUE(worker->push(final_text("Boom.")));

    // The worker fails asynchronously; push starts failing once it did.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (worker->is_running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    EXPECT_FALSE(worker->is_running());
    const auto pushed = worker->push(final_text("After."));
    ASSERT_FALSE(pushed);
    EXPECT_NE(pushed.error().message.find("model exploded"), std::string::npos);

    const auto stopped = worker->stop();
    ASSERT_FALSE(stopped);
    EXPECT_EQ(stopped.error().message, "model exploded");
}

TEST_F(TranslationWorkerTest, InputArrivingDuringATranslationIsTranslatedAfterIt)
{
    auto worker = make();
    control->release.acquire();  // block the first translation
    ASSERT_TRUE(worker->push(final_text("First.")));
    control->entered.acquire();

    ASSERT_TRUE(worker->push(final_text("Second.")));
    control->release.release();
    ASSERT_TRUE(worker->stop());

    const std::scoped_lock lock{control->mutex};
    ASSERT_EQ(control->jobs.size(), 2U);
    EXPECT_EQ(control->jobs[1].text, "Second.");
    ASSERT_EQ(control->jobs[1].context.size(), 1U);
    EXPECT_EQ(control->jobs[1].context[0].translation, "<First.>");
}

TEST_F(TranslationWorkerTest, FullInputQueueFailsPush)
{
    auto worker = make();
    control->release.acquire();
    ASSERT_TRUE(worker->push(final_text("Block.")));
    control->entered.acquire();  // the worker is inside the translation, not reading input

    for (std::size_t i = 0; i < TranslationWorker::max_queued_inputs; ++i) {
        ASSERT_TRUE(worker->push({.text = "x", .is_final = false})) << i;
    }
    const auto overflow = worker->push({.text = "x", .is_final = false});
    EXPECT_FALSE(overflow);

    control->release.release();
    ASSERT_TRUE(worker->stop());
}

TEST_F(TranslationWorkerTest, DestructorStopsAndDeliversTheBacklog)
{
    {
        auto worker = make();
        ASSERT_TRUE(worker->push(final_text("Bye.")));
    }

    EXPECT_EQ(recorder.finals().size(), 1U);
}

TEST_F(TranslationWorkerTest, ConcurrentProducerKeepsOrder)
{
    constexpr int units  = 200;
    auto          worker = make();

    std::thread producer{[&] {
        for (int i = 0; i < units; ++i) {
            const std::string text = "Sentence " + std::to_string(i) + ".";
            EXPECT_TRUE(worker->push(final_text(text, i, i + 0.5)));
        }
    }};
    producer.join();
    ASSERT_TRUE(worker->stop());

    const auto finals = recorder.finals();
    ASSERT_EQ(finals.size(), static_cast<std::size_t>(units));
    for (int i = 0; i < units; ++i) {
        EXPECT_EQ(finals[static_cast<std::size_t>(i)].source_text,
                  "Sentence " + std::to_string(i) + ".");
    }
}

TEST(TextStream, EmptyStreamRejectsInput)
{
    TextStream stream;

    EXPECT_FALSE(stream.is_running());
    EXPECT_FALSE(stream.push({.text = "x"}));
    EXPECT_TRUE(stream.stop());
}

TEST_F(TranslationWorkerTest, SpeakerTurnsSplitUnitsAndLabelTranslations)
{
    auto worker = make();

    const std::vector<TextStream::SpeakerTurn> turns = {{"speaker_0", 0.0, 1.2},
                                                        {"speaker_1", 1.3, 2.5}};
    ASSERT_TRUE(worker->push(TextStream::Speakers{.turns = turns, .complete_until_sec = 3.0}));
    ASSERT_TRUE(worker->push({.text = "so it was great", .start_sec = 0.0, .end_sec = 1.0,
                              .is_final = false}));
    ASSERT_TRUE(worker->push({.text = " yes indeed", .start_sec = 0.0, .end_sec = 2.0,
                              .is_final = false}));
    ASSERT_TRUE(worker->stop());

    std::vector<std::string> finals;
    for (const Segment& segment : recorder.finals()) {
        finals.push_back(segment.speaker + ":" + segment.source_text);
    }
    EXPECT_EQ(finals, (std::vector<std::string>{"speaker_0:so it was great", "speaker_1:yes indeed"}));
}
