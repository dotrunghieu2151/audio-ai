#pragma once

#include <cubeb/cubeb.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "backends/cubeb/cubeb_backend_runtime.hpp"
#include "backends/cubeb/cubeb_stream.hpp"
#include "dth/audio/audio-types.hpp"
#include "dth/audio/audio_device.hpp"
#include "dth/audio/stream_config.hpp"

namespace dth::audio {
class CubebBackend final
{
  public:
    ~CubebBackend() = default;

    explicit CubebBackend(std::shared_ptr<CubebBackendRuntime> runtime);

    CubebBackend(const CubebBackend&)            = delete;
    CubebBackend& operator=(const CubebBackend&) = delete;

    CubebBackend(CubebBackend&&)            = default;
    CubebBackend& operator=(CubebBackend&&) = default;

    std::vector<AudioDevice> enumerate_devices() const;

    std::optional<AudioDevice> default_input_device() const;

    std::optional<AudioDevice> default_output_device() const;

    CubebStreamHandle make_stream(const AudioDevice& device, const InputStreamConfig& config,
                                  InputCallback callback, void* user_data);

    CubebStreamHandle make_stream(const AudioDevice& device, const OutputStreamConfig& config,
                                  OutputCallback callback, void* user_data);

    CubebStreamHandle make_stream(const AudioDevice& input, const AudioDevice& output,
                                  const DuplexStreamConfig& config, DuplexCallback callback,
                                  void* user_data);

  private:
    // cubeb enumerates input and output devices separately; each becomes its own AudioDevice.
    struct DeviceEntry
    {
        AudioDevice                                  device;
        cubeb_device_type                            type;
        cubeb_devid                                  devid;
        bool                                         preferred;
        std::uint32_t                                latency_lo;
        std::shared_ptr<const CubebDeviceCollection> owner;
    };

    // Re-enumerates devices. AudioDevice::id stays stable across refreshes for the same device.
    void refresh_devices() const;

    std::optional<AudioDevice> default_device(cubeb_device_type type) const;

    CubebStreamHandle::Endpoint make_endpoint(const AudioDevice& device, cubeb_device_type type,
                                              std::uint32_t channels) const;

    static CubebStreamHandle::StreamConfig make_common_config(const StreamConfig& common)
    {
        return CubebStreamHandle::StreamConfig{
            .sample_rate       = common.sample_rate,
            .frames_per_buffer = common.frames_per_buffer,
            .suggested_latency = common.suggested_latency,
        };
    }

    static CubebStreamHandle::StreamCallback make_stream_callback(
        const InputStreamConfig& config, InputCallback cb)
    {
        return [fn = cb, channels = config.channels](const void* input, void* /*output*/,
                                                     long frameCount, void* userData) {
            return fn(userData, AudioBufferView{
                                    .data     = static_cast<const float*>(input),
                                    .frames   = static_cast<unsigned long>(frameCount),
                                    .channels = channels,
                                });
        };
    }

    static CubebStreamHandle::StreamCallback make_stream_callback(
        const OutputStreamConfig& config, OutputCallback cb)
    {
        return [fn = cb, channels = config.channels](const void* /*input*/, void* output,
                                                     long frameCount, void* userData) {
            return fn(userData, MutableAudioBufferView{
                                    .data     = static_cast<float*>(output),
                                    .frames   = static_cast<unsigned long>(frameCount),
                                    .channels = channels,
                                });
        };
    }

    static CubebStreamHandle::StreamCallback make_stream_callback(
        const DuplexStreamConfig& config, DuplexCallback cb)
    {
        return [fn = cb, inputChannels = config.input_channels,
                outputChannels = config.output_channels](const void* input, void* output,
                                                         long frameCount, void* userData) {
            const auto frames = static_cast<unsigned long>(frameCount);
            return fn(userData, DuplexAudioBufferView{
                                    .input =
                                        AudioBufferView{
                                            .data     = static_cast<const float*>(input),
                                            .frames   = frames,
                                            .channels = inputChannels,
                                        },
                                    .output = MutableAudioBufferView{
                                        .data     = static_cast<float*>(output),
                                        .frames   = frames,
                                        .channels = outputChannels,
                                    }});
        };
    }

    std::shared_ptr<CubebBackendRuntime> _runtime;
    // Snapshot of the last enumeration (mutable: refreshed from const queries).
    mutable std::vector<DeviceEntry> _devices;
    // "<type>:<cubeb device_id>" -> stable AudioDevice::id.
    mutable std::unordered_map<std::string, std::uint32_t> _ids;
};
}  // namespace dth::audio
