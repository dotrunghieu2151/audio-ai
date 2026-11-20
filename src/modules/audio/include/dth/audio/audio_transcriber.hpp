#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "dth/audio/audio_stream.hpp"

namespace dth::audio {
class AudioSource;
class AudioSystem;

class AudioTranscriber
{
  public:
    struct Config
    {
        enum Backend {
            Cuda,
            Metal,
            NativeCpu,
        };
        std::string model_path;
        std::string model = "haas_asr";

        Backend backend = Backend::NativeCpu;
        int     device  = 0;
        int     threads = 4;

        // audio.cpp model options, passed through verbatim: keys as the model family declares
        // them (audio.cpp docs/models/<family>.md), e.g. "parakeet_tdt.right_context_sec" -> "1".
        // Unknown keys or invalid values fail where the options are applied: session options
        // in the constructor / make_stream(), request options in transcribe() / a stream's start().
        // - session_options (audiocpp --session-option): every ASR session of this transcriber,
        //   transcribe()'s and make_stream()'s; they override transcribe()'s built-in defaults.
        // - request_options (audiocpp --request-option): every transcribe() request, every
        //   offline_per_utterance utterance and every continuous_stream stream start.
        std::map<std::string, std::string> session_options;
        std::map<std::string, std::string> request_options;

        // How make_stream() turns the utterances voice activity detection finds into text.
        // audio.cpp cannot tell which a model needs: its streaming mode may be buffered (fixed
        // windows re-encoded, no context carried: parakeet_tdt, haas_asr) or cache-aware (context
        // carried across the whole stream).
        enum class StreamStrategy {
            // Each utterance is transcribed on its own, offline, with full context inside it.
            // Best for models whose streaming mode is buffered.
            offline_per_utterance,
            // One streaming session runs for the whole stream and keeps its context across
            // utterances; utterance boundaries only group its text. For cache-aware models.
            continuous_stream,
        };
        StreamStrategy stream_strategy = StreamStrategy::offline_per_utterance;

        // Silero VAD model directory, used by make_stream() to find utterances.
        std::string vad_model_path = "assets/models/silero_vad";

        // make_stream() utterance segmentation. Shorter utterances give final results sooner;
        // with offline_per_utterance they also give the model less context (fewer than ~3-5 s
        // costs accuracy). With continuous_stream they only change how text is grouped.
        // Continuous speech is cut after this long, at a pause found in its second half when
        // there is one. Range [2, 60].
        double max_utterance_sec = 20.0;
        // Silence that ends an utterance. ~300 ends at phrase pauses, ~500 at sentence pauses;
        // below ~200 splits at gaps between words. Range [0, 5000].
        int vad_min_silence_ms = 500;
        // Adaptive end of utterance: once an utterance is `after_sec` long, a silence of
        // `min_silence_ms` ends it, so long speech ends at the first shorter pause instead of being
        // cut at max_utterance_sec. Steps in increasing `after_sec` (> 0; steps at or past
        // max_utterance_sec are never reached and run no session), each `min_silence_ms` below the
        // previous level's. Each step runs one more Silero session
        // (~0.6% of a CPU core). Empty: vad_min_silence_ms only. Silero's speech score decays over
        // several 32 ms frames, so a level ends only on pauses ~150 ms longer than its setting
        // (measured: 150 catches 300 ms pauses but not 200 ms; 50 catches 200 ms). The default
        // ends utterances older than 5 s at ~300 ms pauses, older than 10 s at ~200 ms pauses.
        struct SilenceStep
        {
            double after_sec      = 0.0;
            int    min_silence_ms = 0;
        };
        std::vector<SilenceStep> vad_adaptive_silence = {{5.0, 150}, {10.0, 50}};
        // Audio kept before and after detected speech, so onsets and endings are not clipped.
        // Range [0, 1000].
        int vad_speech_pad_ms = 200;

        // continuous_stream: the model's output lag, i.e. how long after audio is heard its text is
        // emitted (preferred chunk + lookahead), within [0, 60]. An utterance collects text until
        // this long after its end (or until the next begins), and this much silence after speech
        // is still pushed so the model emits the last words; longer silence is skipped. Too small:
        // an utterance's last words are lost to the next one or delayed until the next speech.
        // Default: nemotron_asr (320 ms chunk + 240 ms lookahead).
        double stream_text_latency_sec = 0.6;

        constexpr std::string_view backend_str()
        {
            switch (backend) {
                case Config::Backend::Cuda:
                    return "cuda";
                case Config::Backend::Metal:
                    return "metal";
                case Config::Backend::NativeCpu:
                    return "cpu";
            }
            return "UNKNOWN";
        }
    };

    struct TranscribeResult
    {
        std::string text;
        std::string language;
    };

    explicit AudioTranscriber(Config config);

    ~AudioTranscriber();
    AudioTranscriber(const AudioTranscriber&)            = delete;
    AudioTranscriber& operator=(const AudioTranscriber&) = delete;

    AudioTranscriber(AudioTranscriber&&)            = delete;
    AudioTranscriber& operator=(AudioTranscriber&&) = delete;

    // samples: interleaved float32 PCM
    // frames: number of frames per channel
    // sample_rate: input sample rate
    // channels: number of channels
    //
    // Returns an empty string if no text was produced.
    TranscribeResult transcribe(std::span<const float> samples, int sample_rate, int channels,
                                int frames);

    // Text of a stream. Times are seconds since the stream's start().
    // - is_final true: one whole utterance; [start_sec, end_sec] is its span.
    // - is_final false (continuous_stream only): new text as soon as the model emits it, part of
    //   the utterance the next final call delivers whole. Concatenating the partials of an
    //   utterance gives its text; [start_sec, end_sec] runs from the utterance's start to the
    //   moment the text was emitted (the model lags the audio by stream_text_latency_sec).
    struct TranscribedSegment
    {
        std::string text;
        std::string language;
        double      start_sec = 0.0;
        double      end_sec   = 0.0;
        bool        is_final  = true;
    };

    // Receives the text of a stream made by make_stream(), in order, on that stream's worker
    // thread (not real-time: may allocate, lock and do IO; must not throw). `segment` is borrowed
    // for the call. The last calls can run inside the stream's stop() / close() / destructor while
    // the control thread waits for them: `user_data` must outlive the stream, and the callback
    // must not call into the stream. Consumers that only want utterances ignore partials.
    using StreamCallback = void (*)(void* user_data, const TranscribedSegment& segment);

    // Streams `source` through this transcriber's model, one final callback per utterance (plus
    // partials with continuous_stream). The audio is
    // converted to mono 16 kHz on the source's real-time thread; a worker thread running between
    // the stream's start() and stop() finds utterances with Silero VAD (Config::vad_model_path;
    // see Config::max_utterance_sec / vad_*) and transcribes them per Config::stream_strategy.
    // stop() returns after the last callback, so it blocks while the worker transcribes its
    // backlog (up to 30 s of audio when it lags). Worker failures end the stream at the next audio
    // buffer (is_running() false) and are returned by stop(), as is audio dropped because the
    // worker fell more than 30 s behind.
    //
    // The stream owns its own audio.cpp sessions: it may outlive this transcriber and run alongside
    // transcribe(). Call make_stream() and transcribe() from one thread.
    // Throws if the model does not support the strategy's ASR mode, the VAD model cannot be
    // loaded, or a session cannot be created.
    [[nodiscard]]
    AudioStream make_stream(AudioSystem& system, std::unique_ptr<AudioSource> source,
                            StreamCallback callback, void* user_data);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace dth::audio
