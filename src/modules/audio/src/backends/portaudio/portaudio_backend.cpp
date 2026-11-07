
#include "backends/portaudio/portaudio_backend.hpp"

#include <portaudio.h>

#include <memory>
#include <utility>
namespace dth::audio {

PortAudioBackend::PortAudioBackend(std::shared_ptr<PortAudioBackendRuntime> runtime)
    : _runtime{std::move(runtime)}
{
    _runtime->initialize();
}

std::vector<AudioDevice> PortAudioBackend::enumerate_devices() const
{
    std::vector<AudioDevice> devices;
    for (PaDeviceIndex i = 0; i < Pa_GetDeviceCount(); ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        devices.push_back(pa_device_to_audio_device(i, *info));
    }
    return devices;
}

std::optional<AudioDevice> PortAudioBackend::default_input_device() const
{
    const PaDeviceIndex defaultInputDeviceIndex = Pa_GetDefaultInputDevice();
    if (defaultInputDeviceIndex == paNoDevice) {
        return std::nullopt;
    }
    const PaDeviceInfo* defaultInputDeviceInfo = Pa_GetDeviceInfo(defaultInputDeviceIndex);

    return pa_device_to_audio_device(defaultInputDeviceIndex, *defaultInputDeviceInfo);
}

std::optional<AudioDevice> PortAudioBackend::default_output_device() const
{
    const PaDeviceIndex defaultOutputDeviceIndex = Pa_GetDefaultOutputDevice();
    if (defaultOutputDeviceIndex == paNoDevice) {
        return std::nullopt;
    }
    const PaDeviceInfo* defaultOutputDeviceInfo = Pa_GetDeviceInfo(defaultOutputDeviceIndex);

    return pa_device_to_audio_device(defaultOutputDeviceIndex, *defaultOutputDeviceInfo);
}

PortAudioStreamHandle PortAudioBackend::make_stream(const AudioDevice&       device,
                                                    const InputStreamConfig& config,
                                                    InputCallback callback, void* user_data)
{
    return {_runtime, make_stream_config(config, device), make_stream_callback(config, callback),
            user_data};
}

PortAudioStreamHandle PortAudioBackend::make_stream(const AudioDevice&        device,
                                                    const OutputStreamConfig& config,
                                                    OutputCallback callback, void* user_data)
{
    return {_runtime, make_stream_config(config, device), make_stream_callback(config, callback),
            user_data};
}
PortAudioStreamHandle PortAudioBackend::make_stream(const AudioDevice&        input,
                                                    const AudioDevice&        output,
                                                    const DuplexStreamConfig& config,
                                                    DuplexCallback callback, void* user_data)
{
    return {_runtime, make_stream_config(config, input, output),
            make_stream_callback(config, callback), user_data};
}

}  // namespace dth::audio
