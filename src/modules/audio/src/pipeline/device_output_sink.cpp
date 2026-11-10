#include "pipeline/device_output_sink.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <string>
#include <utility>

#include "dth/audio/audio_error.hpp"

namespace dth::audio {
namespace {

AudioError make_error(AudioErrorCode code, std::string message)
{
    return AudioError(code, "DeviceOutputSink: " + std::move(message));
}

void fill_silence(MutableAudioBufferView output, std::size_t from_frame) noexcept
{
    std::span<float> const samples = output.samples();
    std::fill(samples.begin() + static_cast<std::ptrdiff_t>(from_frame * output.channels),
              samples.end(), 0.0f);
}

}  // namespace

DeviceOutputSink::DeviceOutputSink(AudioDevice device, OutputStreamFactory factory,
                                   OutputSinkConfig config)
    : _device{std::move(device)}, _factory{std::move(factory)}, _config{config}
{
}

DeviceOutputSink::~DeviceOutputSink()
{
    (void)close();
}

StreamResult DeviceOutputSink::prepare(const AudioFormat& input)
{
    if (_stream && _stream->is_running()) {
        return std::unexpected(
            make_error(AudioErrorCode::unsupported_configuration, "prepare() while running"));
    }
    (void)close();

    if (!(input.sample_rate >= 1.0) || input.channels == 0) {
        return std::unexpected(
            make_error(AudioErrorCode::unsupported_configuration, "invalid input format"));
    }
    if (!(_device.default_sample_rate >= 1.0) || _device.max_output_channels == 0) {
        return std::unexpected(make_error(AudioErrorCode::unsupported_configuration,
                                          "device has no output format"));
    }

    bool const convert = has_flag(_config.flags, OutputSinkFlags::convert_format);
    if (has_flag(_config.flags, OutputSinkFlags::drift_compensation) && !convert) {
        return std::unexpected(make_error(AudioErrorCode::unsupported_configuration,
                                          "drift_compensation requires convert_format"));
    }
    if (!convert && input.channels > _device.max_output_channels) {
        return std::unexpected(make_error(AudioErrorCode::unsupported_configuration,
                                          "input has more channels than the device"));
    }

    AudioFormat const output = convert
                                   ? AudioFormat{_device.default_sample_rate,
                                                 _device.max_output_channels}
                                   : input;
    auto const callback_frames =
        static_cast<std::size_t>(std::lround(callback_seconds * output.sample_rate));

    if (auto r = _converter.prepare(input, output, callback_frames); !r) {
        return r;
    }

    auto const ring_callback_frames = std::max<std::size_t>(
        1, static_cast<std::size_t>(std::lround(static_cast<double>(callback_frames) *
                                                input.sample_rate / output.sample_rate)));
    if (auto r = _drift.prepare({input.sample_rate, ring_callback_frames}); !r) {
        return r;
    }

    // Largest possible target plus 4 x the largest block on either side (plan 8.5).
    DriftController sizing = _drift;
    sizing.set_producer_block_frames(max_write_frames);
    std::size_t const ring_frames =
        sizing.target_fill() + 4 * std::max(ring_callback_frames, max_write_frames);
    _ring = std::make_unique<common::SpscRingBuffer<float>>(ring_frames * input.channels);

    _input           = input;
    _output          = output;
    _callback_frames = callback_frames;
    _prerolling      = true;
    _applied_write_frames = 0;
    _max_write_frames.store(0, std::memory_order_relaxed);
    _underruns.store(0, std::memory_order_relaxed);
    _overruns.store(0, std::memory_order_relaxed);
    _ratio.store(1.0, std::memory_order_relaxed);

    OutputStreamConfig config;
    config.common.sample_rate       = output.sample_rate;
    config.common.frames_per_buffer = static_cast<std::uint32_t>(callback_frames);
    config.channels                 = output.channels;

    _stream.emplace(_factory(_device, config, &DeviceOutputSink::output_callback, this));
    if (auto r = _stream->open(); !r) {
        (void)close();
        return r;
    }
    return {};
}

StreamResult DeviceOutputSink::start()
{
    if (!_stream || _ring == nullptr) {
        return std::unexpected(make_error(AudioErrorCode::stream_start_failed, "not prepared"));
    }
    if (_stream->is_running()) {
        return {};
    }

    // Producer and consumer are both idle here: take the consumer role to drop stale audio.
    discard_ring();
    if (auto r = _converter.prepare(_input, _output, _callback_frames); !r) {
        return r;
    }
    _drift.reset();
    _prerolling = true;
    _ratio.store(1.0, std::memory_order_relaxed);

    return _stream->start();
}

StreamResult DeviceOutputSink::stop()
{
    if (!_stream) {
        return {};
    }
    StreamResult result = _stream->stop();
    if (result) {
        sync_with_last_render();
    }
    return result;
}

StreamResult DeviceOutputSink::close()
{
    StreamResult result{};
    if (_stream) {
        result = _stream->close();
        _stream.reset();
        sync_with_last_render();
    }
    _ring.reset();
    return result;
}

CallbackResult DeviceOutputSink::write(AudioBufferView buffer) noexcept
{
    if (_ring == nullptr || buffer.data == nullptr || buffer.channels != _input.channels) {
        return CallbackResult::continue_stream;
    }

    std::size_t const frames = buffer.frames;
    if (frames > _max_write_frames.load(std::memory_order_relaxed)) {
        _max_write_frames.store(frames, std::memory_order_relaxed);
    }

    // Whole frames only. Free space only grows from the producer's view, so all of it fits.
    std::size_t const free_frames = _ring->free_space() / _input.channels;
    std::size_t const n           = std::min(frames, free_frames);
    _ring->write(std::span<const float>{buffer.data, n * _input.channels});
    if (n < frames) {
        _overruns.fetch_add(1, std::memory_order_relaxed);
    }
    return CallbackResult::continue_stream;
}

void DeviceOutputSink::render(MutableAudioBufferView output) noexcept
{
    render_block(output);
    // Single writer: plain load + release store, no read-modify-write.
    _renders.store(_renders.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}

void DeviceOutputSink::sync_with_last_render() noexcept
{
    (void)_renders.load(std::memory_order_acquire);
}

void DeviceOutputSink::render_block(MutableAudioBufferView output) noexcept
{
    if (_ring == nullptr || output.data == nullptr || output.channels != _output.channels) {
        return;
    }

    std::size_t const max_write = _max_write_frames.load(std::memory_order_relaxed);
    if (max_write != _applied_write_frames) {
        _drift.set_producer_block_frames(std::min(max_write, max_write_frames));
        _applied_write_frames = max_write;
    }

    std::size_t const fill = _ring->size() / _input.channels;
    if (_prerolling) {
        if (fill < _drift.target_fill()) {
            fill_silence(output, 0);
            return;
        }
        _prerolling = false;
    }

    std::size_t rendered = 0;
    if (!has_flag(_config.flags, OutputSinkFlags::convert_format)) {
        // Same format on both sides: straight from the ring.
        rendered = pull(this, output.samples());
    } else {
        if (has_flag(_config.flags, OutputSinkFlags::drift_compensation)) {
            double const ratio = _drift.update(fill);
            _converter.set_ratio(ratio);
            _ratio.store(ratio, std::memory_order_relaxed);
        }
        rendered = _converter.process(output, &DeviceOutputSink::pull, this);
    }
    if (rendered < output.frames) {
        fill_silence(output, rendered);
        _underruns.fetch_add(1, std::memory_order_relaxed);
        _prerolling = true;
    }
}

PipelineStats DeviceOutputSink::stats() const noexcept
{
    return PipelineStats{
        .underruns      = _underruns.load(std::memory_order_relaxed),
        .overruns       = _overruns.load(std::memory_order_relaxed),
        .resample_ratio = _ratio.load(std::memory_order_relaxed),
    };
}

AudioFormat DeviceOutputSink::output_format() const noexcept
{
    return _output;
}

std::size_t DeviceOutputSink::target_fill() const noexcept
{
    return _drift.target_fill();
}

CallbackResult DeviceOutputSink::output_callback(void* user_data,
                                                 MutableAudioBufferView output) noexcept
{
    static_cast<DeviceOutputSink*>(user_data)->render(output);
    return CallbackResult::continue_stream;
}

std::size_t DeviceOutputSink::pull(void* user_data, std::span<float> dst) noexcept
{
    auto&             self     = *static_cast<DeviceOutputSink*>(user_data);
    std::size_t const channels = self._input.channels;
    std::size_t const read     = self._ring->read(dst.first(dst.size() / channels * channels));
    return read / channels;
}

void DeviceOutputSink::discard_ring() noexcept
{
    std::array<float, 1024> scratch{};
    while (_ring->read(scratch) != 0) {
    }
}

}  // namespace dth::audio
