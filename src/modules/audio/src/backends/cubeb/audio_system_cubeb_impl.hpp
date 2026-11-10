#pragma once

#include <memory>

#include "audio_stream_impl.hpp"
#include "pipeline/device_input_source.hpp"
#include "pipeline/device_output_sink.hpp"
#include "backends/cubeb/cubeb_backend.hpp"
#include "dth/audio/audio_system.hpp"

namespace dth::audio {

class AudioSystem::Impl
{
  public:
    std::vector<AudioDevice> devices() const { return _backend->enumerate_devices(); }

    [[nodiscard]] std::optional<AudioDevice> default_input_device() const
    {
        return _backend->default_input_device();
    }

    [[nodiscard]] std::optional<AudioDevice> default_output_device() const
    {
        return _backend->default_output_device();
    }

    [[nodiscard]]
    AudioStream make_stream(const AudioDevice& device, const InputStreamConfig& config,
                            InputCallback callback, void* user_data)
    {
        return AudioStream{std::make_unique<AudioStream::Impl>(
            _backend->make_stream(device, config, callback, user_data))};
    }

    [[nodiscard]]
    AudioStream make_stream(const AudioDevice& device, const OutputStreamConfig& config,
                            OutputCallback callback, void* user_data)
    {
        return AudioStream{std::make_unique<AudioStream::Impl>(
            _backend->make_stream(device, config, callback, user_data))};
    }

    [[nodiscard]]
    AudioStream make_stream(const AudioDevice& input, const AudioDevice& output,
                            const DuplexStreamConfig& config, DuplexCallback callback,
                            void* user_data)
    {
        return AudioStream{std::make_unique<AudioStream::Impl>(
            _backend->make_stream(input, output, config, callback, user_data))};
    }

    [[nodiscard]]
    std::unique_ptr<AudioSink> make_output_sink(const AudioDevice&      output,
                                                const OutputSinkConfig& config)
    {
        auto factory = [backend = _backend](const AudioDevice& device,
                                            const OutputStreamConfig& stream_config,
                                            OutputCallback callback, void* user_data) {
            return AudioStream{std::make_unique<AudioStream::Impl>(
                backend->make_stream(device, stream_config, callback, user_data))};
        };
        return std::make_unique<DeviceOutputSink>(output, std::move(factory), config);
    }

    [[nodiscard]]
    std::unique_ptr<AudioSource> make_input_source(const AudioDevice&       input,
                                                   const InputStreamConfig& config)
    {
        auto factory = [backend = _backend](const AudioDevice& device,
                                            const InputStreamConfig& stream_config,
                                            InputCallback callback, void* user_data) {
            return AudioStream{std::make_unique<AudioStream::Impl>(
                backend->make_stream(device, stream_config, callback, user_data))};
        };
        return std::make_unique<DeviceInputSource>(input, config, std::move(factory));
    }

  private:
    // shared: output sinks / input sources keep it alive to open their device stream later.
    std::shared_ptr<CubebBackend> _backend{
        std::make_shared<CubebBackend>(std::make_shared<CubebBackendRuntime>())};
};

}  // namespace dth::audio
