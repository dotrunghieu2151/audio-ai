#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>

#include "dth/audio/audio-types.hpp"
#include "dth/audio/audio_device.hpp"
#include "dth/audio/audio_format.hpp"
#include "dth/audio/audio_sink.hpp"
#include "dth/audio/audio_source.hpp"
#include "dth/audio/audio_stream.hpp"
#include "dth/audio/stream_config.hpp"

namespace dth::audio {

// AudioSource that captures an input device through a backend input stream. The backend callback
// pushes into the sink and returns its result to the backend, which ends the stream natively
// (stop_stream / complete_stream behave like on a plain input stream).
//
// format() = {config.common.sample_rate, config.channels} (the backend converts from the device).
//
// Not copyable or movable: its address is the backend callback's user data.
class DeviceInputSource final : public AudioSource
{
  public:
    // Creates (not opens) the device stream. Matches AudioSystem::make_stream for input.
    using InputStreamFactory = std::function<AudioStream(
        const AudioDevice& device, const InputStreamConfig& config, InputCallback callback,
        void* user_data)>;

    DeviceInputSource(AudioDevice device, InputStreamConfig config, InputStreamFactory factory);
    ~DeviceInputSource() override;

    DeviceInputSource(const DeviceInputSource&)            = delete;
    DeviceInputSource& operator=(const DeviceInputSource&) = delete;
    DeviceInputSource(DeviceInputSource&&)                 = delete;
    DeviceInputSource& operator=(DeviceInputSource&&)      = delete;

    StreamResult open() override;
    StreamResult close() override;

    [[nodiscard]]
    AudioFormat format() const noexcept override;

    StreamResult start(AudioSink& sink) override;
    StreamResult stop() override;

  private:
    static CallbackResult input_callback(void* user_data, AudioBufferView input) noexcept;
    // After the backend stream stopped: makes the last callback's effects visible here (the
    // backend's own stop synchronization is invisible to TSan).
    void sync_with_last_callback() noexcept;

    AudioDevice        _device;
    InputStreamConfig  _config;
    InputStreamFactory _factory;

    std::atomic<AudioSink*>    _sink{nullptr};
    std::atomic<std::uint64_t> _callbacks{0};  // released at the end of every callback

    // Declared last: destroyed (stopped) first.
    // optional: AudioStream's inline default constructor needs the (hidden) Impl to be complete.
    std::optional<AudioStream> _stream;
};

}  // namespace dth::audio
