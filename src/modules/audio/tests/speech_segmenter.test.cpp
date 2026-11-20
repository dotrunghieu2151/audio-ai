#include "transcription/speech_segmenter.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <numeric>
#include <optional>
#include <span>
#include <vector>

using namespace dth::audio;

namespace {

struct Segment
{
    std::int64_t       start = 0;
    std::int64_t       end   = 0;
    std::vector<float> samples;
};

struct RecordingSink final : SegmentSink
{
    void audio(std::span<const float> samples) override { audio_samples += samples.size(); }
    void segment_begin(std::int64_t start_sample) override { begins.push_back(start_sample); }
    void segment_end(std::int64_t start_sample, std::int64_t end_sample,
                     std::span<const float> samples) override
    {
        segments.push_back({start_sample, end_sample, {samples.begin(), samples.end()}});
    }

    std::size_t               audio_samples = 0;
    std::vector<std::int64_t> begins;
    std::vector<Segment>      segments;
};

// Sample value == its stream position, so a segment's content proves which range it holds.
class Feeder
{
  public:
    explicit Feeder(SpeechSegmenter& segmenter) : segmenter_{segmenter} {}

    void feed(std::int64_t count)
    {
        std::vector<float> block(static_cast<std::size_t>(count));
        std::iota(block.begin(), block.end(), static_cast<float>(position_));
        position_ += count;
        segmenter_.append(block);
    }

  private:
    SpeechSegmenter& segmenter_;
    std::int64_t     position_ = 0;
};

std::vector<float> range(std::int64_t from, std::int64_t to)
{
    std::vector<float> values(static_cast<std::size_t>(to - from));
    std::iota(values.begin(), values.end(), static_cast<float>(from));
    return values;
}

constexpr SpeechSegmenter::Config config{.history_samples = 100, .max_segment_samples = 1000};

}  // namespace

TEST(SpeechSegmenter, ForwardsAllAudioAndEmitsUtteranceBetweenBoundaries)
{
    RecordingSink   sink;
    SpeechSegmenter segmenter{sink, nullptr, config};
    Feeder          feeder{segmenter};

    feeder.feed(200);
    segmenter.speech_start(150);
    feeder.feed(200);
    segmenter.speech_end(350);
    feeder.feed(50);

    EXPECT_EQ(sink.audio_samples, 450u);
    EXPECT_EQ(sink.begins, (std::vector<std::int64_t>{150}));
    ASSERT_EQ(sink.segments.size(), 1u);
    EXPECT_EQ(sink.segments[0].start, 150);
    EXPECT_EQ(sink.segments[0].end, 350);
    EXPECT_EQ(sink.segments[0].samples, range(150, 350));
}

TEST(SpeechSegmenter, StartOlderThanHistoryIsClampedToRetainedAudio)
{
    RecordingSink   sink;
    SpeechSegmenter segmenter{sink, nullptr, config};
    Feeder          feeder{segmenter};

    feeder.feed(500);  // only 400..500 retained
    segmenter.speech_start(10);
    segmenter.speech_end(500);

    ASSERT_EQ(sink.segments.size(), 1u);
    EXPECT_EQ(sink.segments[0].start, 400);
    EXPECT_EQ(sink.segments[0].samples, range(400, 500));
}

TEST(SpeechSegmenter, PaddedStartDoesNotOverlapPreviousUtterance)
{
    RecordingSink   sink;
    SpeechSegmenter segmenter{sink, nullptr, config};
    Feeder          feeder{segmenter};

    feeder.feed(100);
    segmenter.speech_start(0);
    feeder.feed(100);
    segmenter.speech_end(180);
    feeder.feed(20);
    segmenter.speech_start(150);  // padding reaches back into the first utterance
    feeder.feed(30);
    segmenter.speech_end(250);

    ASSERT_EQ(sink.segments.size(), 2u);
    EXPECT_EQ(sink.segments[1].start, 180);
    EXPECT_EQ(sink.segments[1].samples, range(180, 250));
}

TEST(SpeechSegmenter, EndPositionsAreClampedToUtteranceAndBufferedAudio)
{
    RecordingSink   sink;
    SpeechSegmenter segmenter{sink, nullptr, config};
    Feeder          feeder{segmenter};

    feeder.feed(100);
    segmenter.speech_start(50);
    segmenter.speech_end(10'000);  // beyond the audio
    segmenter.speech_start(100);
    feeder.feed(10);
    segmenter.speech_end(0);  // before the utterance

    ASSERT_EQ(sink.segments.size(), 2u);
    EXPECT_EQ(sink.segments[0].end, 100);
    EXPECT_EQ(sink.segments[1].start, 100);
    EXPECT_EQ(sink.segments[1].end, 100);
    EXPECT_TRUE(sink.segments[1].samples.empty());
}

TEST(SpeechSegmenter, ContinuousSpeechIsCutAtMaxLengthAndContinues)
{
    RecordingSink   sink;
    SpeechSegmenter segmenter{sink, nullptr, config};
    Feeder          feeder{segmenter};

    segmenter.speech_start(0);
    for (int i = 0; i < 25; ++i) {
        feeder.feed(100);
    }
    segmenter.speech_end(2500);

    ASSERT_EQ(sink.segments.size(), 3u);
    EXPECT_EQ(sink.begins, (std::vector<std::int64_t>{0, 1000, 2000}));
    EXPECT_EQ(sink.segments[0].samples, range(0, 1000));
    EXPECT_EQ(sink.segments[1].samples, range(1000, 2000));
    EXPECT_EQ(sink.segments[2].samples, range(2000, 2500));
}

namespace {

// Returns a fixed offset and records the window it was asked about.
struct FakePauseFinder final : PauseFinder
{
    explicit FakePauseFinder(std::optional<std::int64_t> pause) : pause{pause} {}

    std::optional<std::int64_t> find_pause(std::span<const float> samples) override
    {
        windows.emplace_back(samples.begin(), samples.end());
        return pause;
    }

    std::optional<std::int64_t>     pause;
    std::vector<std::vector<float>> windows;
};

}  // namespace

TEST(SpeechSegmenter, MaxLengthCutsAtPauseFoundInSecondHalfAndCarriesTheRest)
{
    RecordingSink   sink;
    FakePauseFinder finder{300};
    SpeechSegmenter segmenter{sink, &finder, config};
    Feeder          feeder{segmenter};

    segmenter.speech_start(0);
    for (int i = 0; i < 10; ++i) {
        feeder.feed(100);
    }
    feeder.feed(200);
    segmenter.speech_end(1200);

    ASSERT_EQ(finder.windows.size(), 1u);
    EXPECT_EQ(finder.windows[0], range(500, 1000));
    EXPECT_EQ(sink.begins, (std::vector<std::int64_t>{0, 800}));
    ASSERT_EQ(sink.segments.size(), 2u);
    EXPECT_EQ(sink.segments[0].samples, range(0, 800));
    EXPECT_EQ(sink.segments[1].samples, range(800, 1200));
}

TEST(SpeechSegmenter, MaxLengthWithoutPauseCutsAtCurrentSample)
{
    for (const std::optional<std::int64_t> pause : {std::optional<std::int64_t>{},
                                                    std::optional<std::int64_t>{0},
                                                    std::optional<std::int64_t>{500}}) {
        RecordingSink   sink;
        FakePauseFinder finder{pause};
        SpeechSegmenter segmenter{sink, &finder, config};
        Feeder          feeder{segmenter};

        segmenter.speech_start(0);
        for (int i = 0; i < 10; ++i) {
            feeder.feed(100);
        }

        EXPECT_EQ(sink.begins, (std::vector<std::int64_t>{0, 1000}));
        ASSERT_EQ(sink.segments.size(), 1u);
        EXPECT_EQ(sink.segments[0].samples, range(0, 1000));
    }
}

TEST(SpeechSegmenter, EndWithMinLengthOnlyClosesUtterancesThatLong)
{
    RecordingSink   sink;
    SpeechSegmenter segmenter{sink, nullptr, config};
    Feeder          feeder{segmenter};

    feeder.feed(100);
    segmenter.speech_start(100);
    feeder.feed(300);
    segmenter.speech_end(350, 300);  // 250 long: too young
    feeder.feed(200);
    segmenter.speech_end(450, 300);  // 350 long

    ASSERT_EQ(sink.segments.size(), 1u);
    EXPECT_EQ(sink.segments[0].start, 100);
    EXPECT_EQ(sink.segments[0].samples, range(100, 450));
}

TEST(SpeechSegmenter, RedundantBoundariesAreIgnored)
{
    RecordingSink   sink;
    SpeechSegmenter segmenter{sink, nullptr, config};
    Feeder          feeder{segmenter};

    segmenter.speech_end(0);  // idle
    feeder.feed(100);
    segmenter.speech_start(10);
    segmenter.speech_start(20);  // already in speech
    feeder.feed(100);
    segmenter.speech_end(150);
    segmenter.speech_end(160);  // idle again

    EXPECT_EQ(sink.begins, (std::vector<std::int64_t>{10}));
    ASSERT_EQ(sink.segments.size(), 1u);
    EXPECT_EQ(sink.segments[0].samples, range(10, 150));
}

TEST(SpeechSegmenter, FinishClosesOpenUtteranceOnly)
{
    RecordingSink   sink;
    SpeechSegmenter segmenter{sink, nullptr, config};
    Feeder          feeder{segmenter};

    feeder.feed(100);
    segmenter.finish();
    EXPECT_TRUE(sink.segments.empty());

    segmenter.speech_start(60);
    feeder.feed(40);
    segmenter.finish();

    ASSERT_EQ(sink.segments.size(), 1u);
    EXPECT_EQ(sink.segments[0].samples, range(60, 140));
}

TEST(SpeechSegmenter, ResetRestartsPositions)
{
    RecordingSink   sink;
    SpeechSegmenter segmenter{sink, nullptr, config};

    {
        Feeder feeder{segmenter};
        feeder.feed(300);
        segmenter.speech_start(250);
        segmenter.reset();
    }
    Feeder feeder{segmenter};
    feeder.feed(50);
    segmenter.speech_start(0);
    segmenter.speech_end(50);

    ASSERT_EQ(sink.segments.size(), 1u);
    EXPECT_EQ(sink.segments[0].start, 0);
    EXPECT_EQ(sink.segments[0].samples, range(0, 50));
}
