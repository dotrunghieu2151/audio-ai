
#include "backends/portaudio/portaudio_backend.hpp"

#include <memory>
#include <utility>

namespace dth::audio {

PortAudioBackend::PortAudioBackend(std::shared_ptr<PortAudioBackendRuntime> runtime)
    : _runtime{std::move(runtime)}
{
}

PortAudioBackend::~PortAudioBackend() = default;

std::vector<AudioDevice> PortAudioBackend::enumerate_devices() {}

std::optional<AudioDevice> PortAudioBackend::default_input_device() {}

std::optional<AudioDevice> PortAudioBackend::default_output_device() {}

std::unique_ptr<IAudioStreamHandle> PortAudioBackend::make_stream(const AudioDevice&       device,
                                                                  const InputStreamConfig& config,
                                                                  InputCallback            callback,
                                                                  void* user_data)
{
}

std::unique_ptr<IAudioStreamHandle> PortAudioBackend::make_stream(const AudioDevice&        device,
                                                                  const OutputStreamConfig& config,
                                                                  OutputCallback callback,
                                                                  void*          user_data)
{
}
std::unique_ptr<IAudioStreamHandle> PortAudioBackend::make_stream(const AudioDevice&        input,
                                                                  const AudioDevice&        output,
                                                                  const DuplexStreamConfig& config,
                                                                  DuplexCallback callback,
                                                                  void*          user_data)
{
}

}  // namespace dth::audio
