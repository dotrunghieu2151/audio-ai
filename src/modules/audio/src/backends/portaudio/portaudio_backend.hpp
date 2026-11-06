#pragma once

#include <portaudio.h>

#include <memory>

#include "backends/portaudio/portaudio_backend_runtime.hpp"
#include "backends/portaudio/portaudio_stream.hpp"
#include "dth/audio/audio-types.hpp"
#include "dth/audio/audio_device.hpp"
#include "dth/audio/stream_config.hpp"
namespace dth::audio {
class PortAudioBackend final
{
  public:
    ~PortAudioBackend() = default;

    explicit PortAudioBackend(std::shared_ptr<PortAudioBackendRuntime> runtime);

    PortAudioBackend(const PortAudioBackend&)            = delete;
    PortAudioBackend& operator=(const PortAudioBackend&) = delete;

    PortAudioBackend(PortAudioBackend&&)            = default;
    PortAudioBackend& operator=(PortAudioBackend&&) = default;

    std::vector<AudioDevice> enumerate_devices() const;

    std::optional<AudioDevice> default_input_device() const;

    std::optional<AudioDevice> default_output_device() const;

    PortAudioStreamHandle make_stream(const AudioDevice& device, const InputStreamConfig& config,
                                      InputCallback callback, void* user_data);

    PortAudioStreamHandle make_stream(const AudioDevice& device, const OutputStreamConfig& config,
                                      OutputCallback callback, void* user_data);
    PortAudioStreamHandle make_stream(const AudioDevice& input, const AudioDevice& output,
                                      const DuplexStreamConfig& config, DuplexCallback callback,
                                      void* user_data);

  private:
    static AudioDevice pa_device_to_audio_device(const PaDeviceIndex index,
                                                 const PaDeviceInfo& info)
    {
        return AudioDevice{
            .id                  = static_cast<std::uint32_t>(index),
            .name                = info.name,
            .host_api            = Pa_GetHostApiInfo(info.hostApi)->name,
            .max_input_channels  = static_cast<std::uint32_t>(info.maxInputChannels),
            .max_output_channels = static_cast<std::uint32_t>(info.maxOutputChannels),
            .default_sample_rate = info.defaultSampleRate,
        };
    }

    static PortAudioStreamHandle::StreamConfig make_stream_config(const InputStreamConfig& config,
                                                                  const AudioDevice&       device)
    {
        PaStreamParameters inputParams{};
        inputParams.device           = device.id;
        inputParams.channelCount     = config.channels;
        inputParams.suggestedLatency = config.common.suggested_latency.has_value()
                                           ? config.common.suggested_latency.value()
                                           : Pa_GetDeviceInfo(device.id)->defaultLowInputLatency;
        inputParams.sampleFormat     = paFloat32;
        return PortAudioStreamHandle::StreamConfig{
            .sample_rate       = config.common.sample_rate,
            .frames_per_buffer = config.common.frames_per_buffer,
            .input             = inputParams,
        };
    }

    static PortAudioStreamHandle::StreamConfig make_stream_config(const OutputStreamConfig& config,
                                                                  const AudioDevice&        device)
    {
        PaStreamParameters outputParams{};
        outputParams.device           = device.id;
        outputParams.channelCount     = config.channels;
        outputParams.suggestedLatency = config.common.suggested_latency.has_value()
                                            ? config.common.suggested_latency.value()
                                            : Pa_GetDeviceInfo(device.id)->defaultLowOutputLatency;
        outputParams.sampleFormat     = paFloat32;
        return PortAudioStreamHandle::StreamConfig{
            .sample_rate       = config.common.sample_rate,
            .frames_per_buffer = config.common.frames_per_buffer,
            .output            = outputParams,
        };
    }

    static PortAudioStreamHandle::StreamConfig make_stream_config(const DuplexStreamConfig& config,
                                                                  const AudioDevice& inputDevice,
                                                                  const AudioDevice& outputDevice)
    {
        PaStreamParameters inputParams{};
        inputParams.device       = inputDevice.id;
        inputParams.channelCount = config.input_channels;
        inputParams.suggestedLatency =
            config.common.suggested_latency.has_value()
                ? config.common.suggested_latency.value()
                : Pa_GetDeviceInfo(inputDevice.id)->defaultLowInputLatency;
        inputParams.sampleFormat = paFloat32;
        PaStreamParameters outputParams{};
        outputParams.device       = outputDevice.id;
        outputParams.channelCount = config.output_channels;
        outputParams.suggestedLatency =
            config.common.suggested_latency.has_value()
                ? config.common.suggested_latency.value()
                : Pa_GetDeviceInfo(outputDevice.id)->defaultLowOutputLatency;
        outputParams.sampleFormat = paFloat32;
        return PortAudioStreamHandle::StreamConfig{
            .sample_rate       = config.common.sample_rate,
            .frames_per_buffer = config.common.frames_per_buffer,
            .input             = inputParams,
            .output            = outputParams,
        };
    }

    static PortAudioStreamHandle::StreamCallback make_stream_callback(
        const InputStreamConfig& config, InputCallback cb)
    {
        return [fn = std::move(cb), channels = config.channels](
                   const void* input, void* output, unsigned long frameCount,
                   const PaStreamCallbackTimeInfo* timeInfo, PaStreamCallbackFlags statusFlags,
                   void* userData) {
            return fn(userData, AudioBufferView{
                                    .data     = static_cast<const float*>(input),
                                    .frames   = frameCount,
                                    .channels = channels,
                                });
        };
    }

    static PortAudioStreamHandle::StreamCallback make_stream_callback(
        const OutputStreamConfig& config, OutputCallback cb)
    {
        return [fn = std::move(cb), channels = config.channels](
                   const void* input, void* output, unsigned long frameCount,
                   const PaStreamCallbackTimeInfo* timeInfo, PaStreamCallbackFlags statusFlags,
                   void* userData) {
            return fn(userData, MutableAudioBufferView{
                                    .data     = static_cast<float*>(output),
                                    .frames   = frameCount,
                                    .channels = channels,
                                });
        };
    }

    static PortAudioStreamHandle::StreamCallback make_stream_callback(
        const DuplexStreamConfig config, DuplexCallback cb)
    {
        return [fn = std::move(cb), inputChannels = config.input_channels,
                outputChannels = config.output_channels](
                   const void* input, void* output, unsigned long frameCount,
                   const PaStreamCallbackTimeInfo* timeInfo, PaStreamCallbackFlags statusFlags,
                   void* userData) {
            return fn(userData,
                      DuplexAudioBufferView{.input =
                                                AudioBufferView{
                                                    .data     = static_cast<const float*>(input),
                                                    .frames   = frameCount,
                                                    .channels = inputChannels,
                                                },
                                            .output = MutableAudioBufferView{
                                                .data     = static_cast<float*>(output),
                                                .frames   = frameCount,
                                                .channels = outputChannels,
                                            }});
        };
    }
    std::shared_ptr<PortAudioBackendRuntime> _runtime;
};
}  // namespace dth::audio
