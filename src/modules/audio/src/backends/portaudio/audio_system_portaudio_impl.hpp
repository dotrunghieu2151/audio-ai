#pragma once

#include <memory>

#include "backends/portaudio/audio_stream_portaudio_impl.hpp"
#include "backends/portaudio/portaudio_backend.hpp"
#include "dth/audio/audio_system.hpp"

namespace dth::audio {

class AudioSystem::Impl
{
  public:
    std::vector<AudioDevice> devices() const { return _backend.enumerate_devices(); }

    [[nodiscard]] std::optional<AudioDevice> default_input_device() const
    {
        return _backend.default_input_device();
    }

    [[nodiscard]] std::optional<AudioDevice> default_output_device() const
    {
        return _backend.default_output_device();
    }

    [[nodiscard]]
    AudioStream make_stream(const AudioDevice& device, const InputStreamConfig& config,
                            InputCallback callback, void* user_data)
    {
        return AudioStream{std::make_unique<AudioStream::Impl>(
            _backend.make_stream(device, config, callback, user_data))};
    }

    [[nodiscard]]
    AudioStream make_stream(const AudioDevice& device, const OutputStreamConfig& config,
                            OutputCallback callback, void* user_data)
    {
        return AudioStream{std::make_unique<AudioStream::Impl>(
            _backend.make_stream(device, config, callback, user_data))};
    }

    [[nodiscard]]
    AudioStream make_stream(const AudioDevice& input, const AudioDevice& output,
                            const DuplexStreamConfig& config, DuplexCallback callback,
                            void* user_data)
    {
        return AudioStream{std::make_unique<AudioStream::Impl>(
            _backend.make_stream(input, output, config, callback, user_data))};
    }

  private:
    PortAudioBackend _backend{std::make_shared<PortAudioBackendRuntime>()};
};

}  // namespace dth::audio
