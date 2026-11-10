#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "dth/audio/audio-types.hpp"
#include "dth/audio/audio_format.hpp"
#include "dth/audio/audio_sink.hpp"
#include "dth/audio/audio_source.hpp"

namespace dth::audio {

// AudioSource that captures everything the OS plays (system audio), excluding this process so
// piping it to an output device does not feed back.
//
// macOS 14.2+: global stereo Core Audio process tap inside a private aggregate device; the
// device's IO thread calls sink.write() (silence while nothing plays). Other OSes: open() returns
// unsupported_configuration.
//
// Threading: all members on the control thread. sink.write() runs on the device IO thread
// between start() and stop(). When write() returns anything but continue_stream, the IO callback
// stops delivering (AudioDeviceStop is not real-time safe; the device keeps running idle until
// stop()/close(), and start() restarts it).
//
// Not copyable or movable: its address is the IO callback's user data.
class SystemAudioSource final : public AudioSource
{
  public:
    // Non-interleaved input is interleaved through scratch in chunks of this many frames.
    static constexpr std::size_t scratch_frames = 4096;

    SystemAudioSource() = default;
    ~SystemAudioSource() override;

    SystemAudioSource(const SystemAudioSource&)            = delete;
    SystemAudioSource& operator=(const SystemAudioSource&) = delete;
    SystemAudioSource(SystemAudioSource&&)                 = delete;
    SystemAudioSource& operator=(SystemAudioSource&&)      = delete;

    // Creates the tap and the aggregate device and reads format(). No-op when already open.
    StreamResult open() override;

    // Stops, then destroys the aggregate device and the tap. Idempotent.
    StreamResult close() override;

    [[nodiscard]]
    bool is_open() const noexcept;
    [[nodiscard]]
    bool is_running() const noexcept;

    // Tap format. Valid after open().
    [[nodiscard]]
    AudioFormat format() const noexcept override;

    StreamResult start(AudioSink& sink) override;
    StreamResult stop() override;

  private:
    struct IoProc;  // defined by the platform implementation
    friend struct IoProc;

    AudioFormat   _format{};
    std::uint32_t _tap{0};        // AudioObjectID; 0 = none
    std::uint32_t _aggregate{0};  // AudioObjectID; 0 = none
    void*         _io_proc{nullptr};  // AudioDeviceIOProcID while running
    bool          _interleaved{true};
    std::vector<float> _scratch;  // scratch_frames * channels, for non-interleaved input

    // stop() clears _sink, then waits until no callback is inside. seq_cst on both sides (Dekker):
    // after the wait no callback can still see the old sink. Does not rely on AudioDeviceStop's
    // (undocumented) synchronization, and makes the hand-off visible to TSan.
    std::atomic<AudioSink*>    _sink{nullptr};
    std::atomic<std::uint32_t> _in_callback{0};
};

}  // namespace dth::audio
