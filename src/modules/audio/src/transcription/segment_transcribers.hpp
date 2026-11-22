#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "dth/audio/audio_transcriber.hpp"
#include "transcription/audiocpp_support.hpp"
#include "transcription/speech_segmenter.hpp"

// The worker-thread stages of AudioTranscriber::make_stream(): voice activity detection feeding a
// SpeechSegmenter, and the strategies turning its utterances into text. All throw
// std::runtime_error on audio.cpp failures. Single-threaded; not real-time safe.
namespace dth::audio {

// Adaptive Silero VAD: one audio.cpp "vad"/"streaming" session per silence level, each ending
// speech after its own minimum silence. Any level's start opens an utterance (all share one speech
// threshold, so they start together); a level's end closes it only once the utterance is at least
// that level's `min_utterance_samples` long. A long utterance thus ends at the first shorter pause
// instead of running into the segmenter's maximum. A level with a longer silence cannot end
// speech another level has restarted: no level starts during the silence it waits for.
class VoiceActivityDetector
{
  public:
    // Silero takes exactly 512-sample frames at 16 kHz.
    static constexpr std::size_t frame_samples = 512;

    struct Level
    {
        audiocpp::SessionPtr session;
        std::int64_t         min_utterance_samples = 0;
    };

    explicit VoiceActivityDetector(std::vector<Level> levels) noexcept;

    // Starts a new stream; positions restart at 0.
    void start();

    // Classifies the next `frame_samples` samples and reports speech boundaries to `segmenter`.
    void process(std::span<const float> frame, SpeechSegmenter& segmenter);

  private:
    static void report(const audiocpp::EventPtr& event, std::int64_t min_length,
                       SpeechSegmenter& segmenter);

    std::vector<Level> levels_;
    std::int64_t       next_sample_ = 0;
};

// Silero VAD on an audio.cpp "vad"/"offline" session, set to detect short silences: the pause is
// the middle of the longest gap between two speeches it finds (ties: the later one). Silero
// resolves 32 ms frames with hysteresis, so gaps of about 100 ms between words can go unseen.
class SileroPauseFinder final : public PauseFinder
{
  public:
    explicit SileroPauseFinder(audiocpp::SessionPtr session);

    [[nodiscard]] std::optional<std::int64_t> find_pause(std::span<const float> samples) override;

  private:
    audiocpp::SessionPtr session_;
    audiocpp::RequestPtr request_;
};

// A SegmentSink that delivers TranscribedSegments through a StreamCallback.
class SegmentTranscriber : public SegmentSink
{
  public:
    // Before the first audio of a stream.
    virtual void start() = 0;
    // After the stream's last segment_end: delivers whatever text is still pending.
    virtual void finish() = 0;
};

// StreamConfig::Strategy::offline_per_utterance: each utterance is one request on an "offline"
// session.
class OfflineSegmentTranscriber final : public SegmentTranscriber
{
  public:
    // `request_options` go on every utterance's request.
    OfflineSegmentTranscriber(audiocpp::SessionPtr session, const audiocpp::OptionMap& request_options,
                              AudioTranscriber::StreamCallback callback, void* user_data);

    void start() override {}
    void finish() override {}

    void audio(std::span<const float> /*samples*/) override {}
    void segment_begin(std::int64_t /*start_sample*/) override {}
    void segment_end(std::int64_t start_sample, std::int64_t end_sample,
                     std::span<const float> samples) override;

  private:
    audiocpp::SessionPtr             session_;
    audiocpp::RequestPtr             request_;
    AudioTranscriber::StreamCallback callback_;
    void*                            user_data_;
};

// StreamConfig::Strategy::continuous_stream: one "streaming" session, finished only by finish(), so
// its context spans the whole stream; audio goes in the family's preferred chunk size.
//
// Only speech is pushed: the model hears each utterance from its (padded) start, plus
// `text_latency_samples` of what follows (a model emits an utterance's last words only once its
// lookahead has heard it); longer silence is skipped. The model therefore sees speech
// concatenated; positions here stay in stream time. Up to `history_samples` of skipped audio is
// kept, so a start the VAD reports late still reaches the model.
//
// Each piece of text the model emits is delivered at once as a partial (is_final false); each
// utterance is then delivered whole (is_final true).
//
// Text is grouped into utterances by when it is emitted, `text_latency_samples` being the model's
// output lag: an utterance collects text until the lag after its end has passed or the next
// utterance begins, whichever is first; text emitted between utterances goes to the next one.
// Approximate: a model's lag varies (words come in bursts) and a forced cut is decided after the
// fact, so words near a boundary can land in the neighbouring utterance. Exact grouping would need
// the model's word timestamps.
class ContinuousStreamTranscriber final : public SegmentTranscriber
{
  public:
    // `request_options` go on the request that starts each stream.
    ContinuousStreamTranscriber(audiocpp::SessionPtr session, audiocpp::OptionMap request_options,
                                std::int64_t text_latency_samples, std::int64_t history_samples,
                                AudioTranscriber::StreamCallback callback, void* user_data);

    void start() override;
    void finish() override;

    void audio(std::span<const float> samples) override;
    void segment_begin(std::int64_t start_sample) override;
    void segment_end(std::int64_t start_sample, std::int64_t end_sample,
                     std::span<const float> samples) override;

  private:
    struct Range
    {
        std::int64_t start = 0;
        std::int64_t end   = 0;
    };

    [[nodiscard]] bool feeding(std::int64_t position) const noexcept;
    void               skip(std::int64_t position, std::span<const float> samples);
    // `samples` start at stream position `position`.
    void               feed(std::span<const float> samples, std::int64_t position);
    // The stream position of the audio the model's text covers so far: its pushed audio minus the
    // lookahead, mapped back across skipped silence.
    [[nodiscard]] std::int64_t heard_position() const;
    void               push(std::span<const float> samples);
    void               collect(const audiocpp::EventPtr& event);
    // `heard`: the stream position the text covers; default heard_position().
    void               deliver_partial(std::string text, std::optional<std::int64_t> heard = {});
    void               emit_if_due();
    void               emit(Range range);

    audiocpp::SessionPtr             session_;
    audiocpp::OptionMap              request_options_;
    std::int64_t                     text_latency_samples_;
    std::int64_t                     history_samples_;
    AudioTranscriber::StreamCallback callback_;
    void*                            user_data_;

    std::int64_t stream_samples_ = 0;  // all audio received (stream time)

    std::vector<float> skipped_;  // last skipped audio, <= history_samples_
    std::int64_t       skipped_start_ = 0;  // stream position of skipped_[0]

    std::size_t        chunk_samples_ = 0;
    std::vector<float> pending_audio_;  // < chunk_samples_ not yet pushed
    std::int64_t       model_samples_ = 0;  // pushed to the model (model time)

    // Where fed audio sits on the stream: runs of (model time, stream time) starts. The model
    // gets audio in bursts (a VAD start hands it the history since the start), so the time text
    // is emitted says little about when it was spoken.
    struct FedRun
    {
        std::int64_t model_start  = 0;
        std::int64_t stream_start = 0;
    };
    std::vector<FedRun> fed_runs_;
    std::int64_t        fed_samples_     = 0;  // given to feed() (model time, incl. pending_audio_)
    std::int64_t        fed_stream_next_ = -1;  // stream position following the last fed sample

    std::string published_;  // all text the session emitted this stream
    std::string text_;       // text not yet delivered
    std::string language_;

    std::optional<std::int64_t> open_start_;  // utterance in progress
    std::optional<Range>        closed_;      // ended, still collecting its late text
    std::int64_t                last_end_ = 0;
};

}  // namespace dth::audio
