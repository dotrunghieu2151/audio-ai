#pragma once

#include <audiocpp.h>

#include <memory>

#include "dth/audio/audio_sink.hpp"
#include "dth/audio/audio_transcriber.hpp"
#include "transcription/audiocpp_support.hpp"

namespace dth::audio {

// The ASR model a stream transcribes with and how its sessions are created. Borrowed for the
// make_transcriber_sink() call only.
struct StreamAsrModel
{
    audiocpp_registry*             registry = nullptr;  // loads the stream's Silero VAD model
    const audiocpp_model*          model    = nullptr;
    const audiocpp_backend_config* backend  = nullptr;
    const audiocpp::OptionMap*     session_options = nullptr;
};

// The sink of AudioTranscriber::make_stream(): takes mono 16 kHz audio on the source's real-time
// thread and transcribes it on its own worker thread, per `config`. Owns its audio.cpp sessions,
// so it does not depend on `asr` after the call.
// Throws std::invalid_argument if `config` is out of range, std::runtime_error on audio.cpp
// failures (unsupported ASR mode, VAD model, session creation).
[[nodiscard]] std::unique_ptr<AudioSink> make_transcriber_sink(
    const StreamAsrModel& asr, const AudioTranscriber::StreamConfig& config,
    AudioTranscriber::StreamCallback callback, void* user_data);

}  // namespace dth::audio
