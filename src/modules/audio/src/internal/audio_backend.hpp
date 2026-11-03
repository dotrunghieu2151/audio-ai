#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "dth/audio/audio-types.hpp"
#include "dth/audio/audio_device.hpp"
#include "dth/audio/stream_config.hpp"
#include "internal/stream_handle.hpp"

namespace dth::audio {

class IAudioBackend
{
  public:
    virtual ~IAudioBackend() = default;

    virtual std::vector<AudioDevice> enumerate_devices() = 0;

    virtual std::optional<AudioDevice> default_input_device() = 0;

    virtual std::optional<AudioDevice> default_output_device() = 0;

    virtual std::unique_ptr<IAudioStreamHandle> make_stream(const AudioDevice&       device,
                                                            const InputStreamConfig& config,
                                                            InputCallback            callback,
                                                            void*                    user_data) = 0;

    virtual std::unique_ptr<IAudioStreamHandle> make_stream(const AudioDevice&        device,
                                                            const OutputStreamConfig& config,
                                                            OutputCallback            callback,
                                                            void* user_data) = 0;
    virtual std::unique_ptr<IAudioStreamHandle> make_stream(const AudioDevice&        input,
                                                            const AudioDevice&        output,
                                                            const DuplexStreamConfig& config,
                                                            DuplexCallback            callback,
                                                            void* user_data) = 0;
};
}  // namespace dth::audio
