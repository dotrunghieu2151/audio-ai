#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string>

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

        // Generic model flags
        std::string model_options;

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

    // Receives transcripts of a stream made by make_stream(), on that stream's worker thread (not
    // real-time: may allocate, lock and do IO; must not throw). `result` is borrowed for the call.
    // The final call can run inside the stream's stop() / close() / destructor while the control
    // thread waits for it: `user_data` must outlive the stream, and the callback must not call
    // into the stream.
    // - is_final false: text decoded since the previous partial (append to build the transcript).
    // - is_final true: the whole transcript, once per start() / stop() cycle that received audio.
    using StreamCallback = void (*)(void* user_data, const TranscribeResult& result,
                                    bool is_final);

    // Streams `source` through this transcriber's model (audio.cpp streaming ASR). The audio is
    // converted to mono 16 kHz on the source's real-time thread and transcribed on a worker thread
    // that runs between the stream's start() and stop(). stop() returns after the final callback,
    // so it blocks while the worker transcribes its backlog (up to 30 s of audio when it lags).
    // Worker failures end the stream at the next audio buffer (is_running() false) and are returned
    // by stop(), as is audio dropped because the worker fell more than 30 s behind.
    //
    // The stream owns its own audio.cpp session: it may outlive this transcriber and run alongside
    // transcribe(). Call make_stream() and transcribe() from one thread.
    // Throws if the model does not support streaming ASR or the session cannot be created.
    [[nodiscard]]
    AudioStream make_stream(AudioSystem& system, std::unique_ptr<AudioSource> source,
                            StreamCallback callback, void* user_data);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace dth::audio
