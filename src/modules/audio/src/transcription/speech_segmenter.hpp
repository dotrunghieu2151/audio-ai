#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace dth::audio {

// Receives a SpeechSegmenter's output. Sample positions count from the start of the stream.
class SegmentSink
{
  public:
    virtual ~SegmentSink() = default;

    // Every appended sample, speech or not, in stream order.
    virtual void audio(std::span<const float> samples) = 0;

    // Speech starts at `start_sample` (possibly before the audio just appended: VAD padding).
    virtual void segment_begin(std::int64_t start_sample) = 0;

    // The utterance begun at `start_sample` ends at `end_sample`; `samples` holds it, borrowed for
    // the call.
    virtual void segment_end(std::int64_t start_sample, std::int64_t end_sample,
                             std::span<const float> samples) = 0;
};

// Finds where speech can be cut without splitting a word.
class PauseFinder
{
  public:
    virtual ~PauseFinder() = default;

    // The offset in `samples` (mono, stream rate) of the best pause to cut at, or nothing when
    // there is none. Offsets outside (0, samples.size()) are treated as nothing.
    [[nodiscard]] virtual std::optional<std::int64_t> find_pause(std::span<const float> samples) = 0;
};

// Turns a contiguous mono stream plus voice-activity boundaries into utterances. VAD reports
// boundaries late and padded (a start can lie before the audio that confirmed it), so the
// segmenter keeps `history_samples` of audio while idle and the whole utterance while in speech.
//
// Continuous speech is cut once it reaches `max_segment_samples`, to bound latency and the
// transcriber's input: at the pause the PauseFinder picks in the utterance's second half, so
// neither part is shorter than half the maximum; the audio after it opens the next utterance.
// Without a pause it is cut at the current sample (end + immediate begin).
//
// Single-threaded; not real-time safe (allocates).
class SpeechSegmenter
{
  public:
    struct Config
    {
        std::int64_t history_samples     = 0;
        std::int64_t max_segment_samples = 0;
    };

    // `pause_finder` may be null: continuous speech is then always cut at the current sample.
    SpeechSegmenter(SegmentSink& sink, PauseFinder* pause_finder, Config config);

    // Appends the next samples of the stream.
    void append(std::span<const float> samples);

    // VAD boundaries, positions clamped to the audio still held; a start also to the previous
    // utterance's end, so utterances never overlap. A start while in speech and an end while idle
    // are ignored, as is an end that would leave an utterance shorter than `min_length` (lets a
    // VAD tuned to shorter silences end only utterances that have grown long).
    void speech_start(std::int64_t sample);
    void speech_end(std::int64_t sample, std::int64_t min_length = 0);

    // End of stream: closes an open utterance at the last sample.
    void finish();

    // Forgets all audio and state; positions restart at 0.
    void reset();

  private:
    [[nodiscard]] std::int64_t end_sample() const noexcept;
    [[nodiscard]] std::int64_t forced_cut();
    void                       close_segment(std::int64_t end);
    void                       trim();

    SegmentSink&                sink_;
    PauseFinder*                pause_finder_;
    Config                      config_;
    std::vector<float>          buffer_;
    std::int64_t                buffer_start_ = 0;  // stream position of buffer_[0]
    std::int64_t                last_end_     = 0;  // end of the previous utterance
    std::optional<std::int64_t> speech_start_;
};

}  // namespace dth::audio
