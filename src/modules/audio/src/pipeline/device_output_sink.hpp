#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "dth/audio/audio-types.hpp"
#include "dth/audio/audio_device.hpp"
#include "dth/audio/audio_stream.hpp"
#include "dth/audio/stream_config.hpp"
#include "dth/common/spsc_ring_buffer.hpp"
#include "dth/audio/audio_format.hpp"
#include "dth/audio/audio_sink.hpp"
#include "dth/audio/pipeline_config.hpp"
#include "pipeline/drift_controller.hpp"
#include "pipeline/pipeline_stats.hpp"
#include "pipeline/format_converter.hpp"

namespace dth::audio {

// AudioSink that plays its input on an output device running on its own clock.
//
// Producer RT thread: write() pushes whole frames into an SPSC ring (input format).
// Consumer RT thread (device callback): render() pulls from the ring through a FormatConverter
// whose ratio is driven by a DriftController from the ring fill level.
//
// - OutputSinkFlags::convert_format (default): the device stream runs at the device's native
//   rate / channel count (no double resampling). Off: it runs at the input format, no converter.
// - OutputSinkFlags::drift_compensation (default, needs convert_format): the converter ratio
//   follows the DriftController. Off: ratio fixed at 1; the target still drives pre-roll.
// - Pre-roll: render() outputs silence until the ring holds the drift target, after start() and
//   after every underrun.
// - Full ring: the incoming write is truncated to whole frames, overruns++.
//   Short ring: the rest of the callback is silence, underruns++.
// - The drift target follows the largest write seen (DriftController::set_producer_block_frames).
//
// Not copyable or movable: its address is the device callback's user data.
class DeviceOutputSink final : public AudioSink
{
  public:
    // Creates (not opens) the device stream. Matches AudioSystem::make_stream for output.
    using OutputStreamFactory = std::function<AudioStream(
        const AudioDevice& device, const OutputStreamConfig& config, OutputCallback callback,
        void* user_data)>;

    // Device callback period requested from the backend.
    static constexpr double callback_seconds = 0.010;
    // Largest producer write the ring is sized for (larger writes still work, but may overrun).
    static constexpr std::size_t max_write_frames = 4096;

    DeviceOutputSink(AudioDevice device, OutputStreamFactory factory, OutputSinkConfig config = {});
    ~DeviceOutputSink() override;

    DeviceOutputSink(const DeviceOutputSink&)            = delete;
    DeviceOutputSink& operator=(const DeviceOutputSink&) = delete;
    DeviceOutputSink(DeviceOutputSink&&)                 = delete;
    DeviceOutputSink& operator=(DeviceOutputSink&&)      = delete;

    // Control thread. Opens the device stream and allocates ring / converter / drift state.
    // Re-prepare (not while running) reopens. drift_compensation without convert_format:
    // unsupported_configuration.
    StreamResult prepare(const AudioFormat& input) override;

    // Control thread. Clears the ring, re-arms pre-roll, starts the device stream. The producer
    // must not write() before start() returns.
    StreamResult start() override;

    // Control thread. Stops the device stream: no render() runs after it returns.
    StreamResult stop() override;

    // Control thread. Stops and releases the device stream and buffers. Idempotent.
    StreamResult close() override;

    // Producer RT thread. Always continue_stream (a full ring drops, it never ends the stream).
    CallbackResult write(AudioBufferView buffer) noexcept override;

    // Consumer RT thread: the device callback body. Public so tests can drive it without a
    // running device (prepare() first, stream not started).
    void render(MutableAudioBufferView output) noexcept;

    // Any thread. Counters are cumulative since prepare().
    [[nodiscard]]
    PipelineStats stats() const noexcept;

    // Device format chosen by prepare().
    [[nodiscard]]
    AudioFormat output_format() const noexcept;

    // Current drift target (ring frames). Consumer thread or with the stream stopped.
    [[nodiscard]]
    std::size_t target_fill() const noexcept;

  private:
    static CallbackResult output_callback(void* user_data, MutableAudioBufferView output) noexcept;
    static std::size_t    pull(void* user_data, std::span<float> dst) noexcept;

    void render_block(MutableAudioBufferView output) noexcept;
    void discard_ring() noexcept;
    // After the backend stream stopped: makes the last render()'s writes visible here.
    void sync_with_last_render() noexcept;

    AudioDevice         _device;
    OutputStreamFactory _factory;
    OutputSinkConfig    _config;

    AudioFormat _input{};
    AudioFormat _output{};

    std::unique_ptr<common::SpscRingBuffer<float>> _ring;
    FormatConverter                                _converter;
    DriftController                                _drift;
    std::size_t                                    _callback_frames{0};  // device frames

    // Consumer-thread state.
    bool        _prerolling{true};
    std::size_t _applied_write_frames{0};

    // Producer -> consumer: largest write so far (frames). Single writer, relaxed.
    std::atomic<std::size_t> _max_write_frames{0};

    std::atomic<std::uint64_t> _underruns{0};
    std::atomic<std::uint64_t> _overruns{0};
    std::atomic<double>        _ratio{1.0};

    // Released at the end of every render(), acquired after the stream stops. The backend's own
    // stop synchronization is invisible to TSan (uninstrumented Core Audio); this makes the
    // render() -> control thread hand-off explicit.
    std::atomic<std::uint64_t> _renders{0};

    // Declared last: destroyed (stopped) first, while everything its callback touches is alive.
    // optional: AudioStream's inline default constructor needs the (hidden) Impl to be complete.
    std::optional<AudioStream> _stream;
};

}  // namespace dth::audio
