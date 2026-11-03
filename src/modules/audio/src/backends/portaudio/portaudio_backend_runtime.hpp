#pragma once

#include <portaudio.h>

#include <expected>
#include <optional>
#include <print>
#include <string>

#include "dth/audio/audio_error.hpp"
#include "internal/audio_backend_runtime.hpp"
namespace dth::audio {
class PortAudioBackendRuntime final : public IAudioBackendRuntime
{
  public:
    ~PortAudioBackendRuntime() override
    {
        const auto err = Pa_Terminate();
        if (err != paNoError) {
            std::println("PortAudio error: {}", Pa_GetErrorText(err));
        }
    }

    PortAudioBackendRuntime(const PortAudioBackendRuntime&)            = delete;
    PortAudioBackendRuntime& operator=(const PortAudioBackendRuntime&) = delete;

    PortAudioBackendRuntime(PortAudioBackendRuntime&&)            = default;
    PortAudioBackendRuntime& operator=(PortAudioBackendRuntime&&) = default;

    std::expected<void, AudioError> initialize() override
    {
        if (_initialized) {
            return {};
        }
        const PaError error = Pa_Initialize();

        if (error != paNoError) {
            return std::unexpected(AudioError(
                AudioErrorCode::initialization_failed,
                "PortAudio initialization failed: " + std::string(Pa_GetErrorText(error)),
                std::make_optional<int>(error)));
        }
        _initialized = true;
        return {};
    }

  private:
    bool _initialized{false};
};
}  // namespace dth::audio
