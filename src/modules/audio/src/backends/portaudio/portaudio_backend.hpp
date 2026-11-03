#pragma once

#include <memory>

#include "backends/portaudio/portaudio_backend_runtime.hpp"
#include "internal/audio_backend.hpp"

namespace dth::audio {
class PortAudioBackend final : public IAudioBackend
{
  public:
    ~PortAudioBackend() override;

    explicit PortAudioBackend(std::shared_ptr<PortAudioBackendRuntime> runtime);

    PortAudioBackend(const PortAudioBackend&)            = delete;
    PortAudioBackend& operator=(const PortAudioBackend&) = delete;

    PortAudioBackend(PortAudioBackend&&)            = default;
    PortAudioBackend& operator=(PortAudioBackend&&) = default;

    std::vector<AudioDevice> enumerate_devices() override;

    std::optional<AudioDevice> default_input_device() override;

    std::optional<AudioDevice> default_output_device() override;

    std::unique_ptr<IAudioStreamHandle> make_stream(const AudioDevice&       device,
                                                    const InputStreamConfig& config,
                                                    InputCallback            callback,
                                                    void*                    user_data) override;

    std::unique_ptr<IAudioStreamHandle> make_stream(const AudioDevice&        device,
                                                    const OutputStreamConfig& config,
                                                    OutputCallback            callback,
                                                    void*                     user_data) override;
    std::unique_ptr<IAudioStreamHandle> make_stream(const AudioDevice&        input,
                                                    const AudioDevice&        output,
                                                    const DuplexStreamConfig& config,
                                                    DuplexCallback            callback,
                                                    void*                     user_data) override;

  private:
    std::shared_ptr<PortAudioBackendRuntime> _runtime;
};
}  // namespace dth::audio
