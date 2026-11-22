#pragma once

#include <memory>
#include <span>
#include <string_view>

#include "dth/audio/audio-types.hpp"

namespace dth::audio {

class TranslationWorker;

// Text in, translated text out: the stream AudioTranslator::make_stream() returns. Text pushed
// into it is translated on the stream's own worker thread, which runs from make_stream() until
// stop() (or the destructor) and delivers the translations to the stream's callback.
class TextStream
{
  public:
    // Text to translate, with the same contract as AudioTranscriber::TranscribedSegment, so a
    // transcriber callback can push its segments unchanged:
    // - is_final false: new text of the current utterance, appended to what came before (delta).
    // - is_final true: the whole utterance; replaces its deltas and ends it.
    // Times are seconds on the producer's clock (e.g. since the transcriber stream started); they
    // are carried into the translations and decide paragraph breaks (see
    // AudioTranslator::StreamConfig::paragraph_gap_sec).
    struct Text
    {
        std::string_view text;
        double           start_sec = 0.0;
        double           end_sec   = 0.0;
        bool             is_final  = true;
    };

    // Speaker turns that ended, with the same contract as AudioTranscriber::SpeakerUpdate, so a
    // transcriber's speaker callback can push its updates unchanged. Each turn's start and end
    // ends a translation unit, and once turns have been pushed a unit waits until they are known
    // through its end (complete_until_sec, on the clock of Text times), so no unit holds two
    // speakers' words; translations carry the speaker of their turn
    // (TranslatedSegment::speaker), once that turn has ended.
    struct SpeakerTurn
    {
        std::string_view speaker;
        double           start_sec = 0.0;
        double           end_sec   = 0.0;
    };
    struct Speakers
    {
        std::span<const SpeakerTurn> turns;
        double                       complete_until_sec = 0.0;
    };

    TextStream() noexcept;
    // Stops the stream (see stop()) if it is still running.
    ~TextStream();

    TextStream(const TextStream&)            = delete;
    TextStream& operator=(const TextStream&) = delete;

    TextStream(TextStream&&) noexcept;
    TextStream& operator=(TextStream&&) noexcept;

    // Queues `text` (copied) for the worker; does not wait for its translation. Call from one
    // thread at a time (any thread, e.g. a transcriber's worker). Fails after stop(), after a
    // worker failure (the error stop() returns), on an empty stream, and when the worker fell so
    // far behind that its input queue is full.
    [[nodiscard]]
    StreamResult push(const Text& text);
    // Queues speaker turns (copied); same thread and failure rules as push(const Text&).
    [[nodiscard]]
    StreamResult push(const Speakers& speakers);

    // Ends the stream: no more input; the pending text is translated and the last callbacks run
    // before this returns (it blocks while the worker translates its backlog). Returns the worker's
    // failure, if any; later calls return the same result. Final: the stream cannot be restarted.
    // Call stop() (and the destructor) from one control thread, never from the callback.
    [[nodiscard]]
    StreamResult stop();

    // True from make_stream() until stop() or a worker failure.
    [[nodiscard]]
    bool is_running() const noexcept;

  private:
    std::unique_ptr<TranslationWorker> worker_;

    explicit TextStream(std::unique_ptr<TranslationWorker> worker) noexcept;

    friend class AudioTranslator;
};

}  // namespace dth::audio
