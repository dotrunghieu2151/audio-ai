#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "dth/audio/audio-types.hpp"
#include "dth/audio/audio_device.hpp"
#include "dth/audio/audio_stream.hpp"
#include "dth/audio/stream_config.hpp"
#include "dth_audio/export_dth_audio.hpp"

namespace dth::audio {

class DTH_AUDIO_EXPORT AudioSystem
{
  public:
    AudioSystem();
    ~AudioSystem();

    AudioSystem(const AudioSystem&)            = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    AudioSystem(AudioSystem&&) noexcept;
    AudioSystem& operator=(AudioSystem&&) noexcept;

    [[nodiscard]]
    std::vector<AudioDevice> devices() const;

    [[nodiscard]]
    std::optional<AudioDevice> default_input_device() const;

    [[nodiscard]]
    std::optional<AudioDevice> default_output_device() const;

    [[nodiscard]]
    AudioStream make_stream(const AudioDevice& device, const InputStreamConfig& config,
                            InputCallback callback, void* user_data);

    [[nodiscard]]
    AudioStream make_stream(const AudioDevice& device, const OutputStreamConfig& config,
                            OutputCallback callback, void* user_data);

    [[nodiscard]]
    AudioStream make_stream(const AudioDevice& input, const AudioDevice& output,
                            const DuplexStreamConfig& config, DuplexCallback callback,
                            void* user_data);

  private:
    class Impl;
    std::unique_ptr<Impl> _impl;

    friend struct AudioSystemImplValidator;
};

}  // namespace dth::audio
