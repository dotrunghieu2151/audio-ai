#include "pipeline/device_input_source.hpp"

#include <string>
#include <utility>

#include "dth/audio/audio_error.hpp"

namespace dth::audio {

DeviceInputSource::DeviceInputSource(AudioDevice device, InputStreamConfig config,
                                     InputStreamFactory factory)
    : _device{std::move(device)}, _config{config}, _factory{std::move(factory)}
{
}

DeviceInputSource::~DeviceInputSource()
{
    (void)close();
}

StreamResult DeviceInputSource::open()
{
    if (_stream) {
        return {};
    }
    _stream.emplace(_factory(_device, _config, &DeviceInputSource::input_callback, this));
    if (auto r = _stream->open(); !r) {
        _stream.reset();
        return r;
    }
    return {};
}

StreamResult DeviceInputSource::close()
{
    if (!_stream) {
        return {};
    }
    StreamResult result = _stream->close();
    _stream.reset();
    sync_with_last_callback();
    _sink.store(nullptr, std::memory_order_relaxed);
    return result;
}

AudioFormat DeviceInputSource::format() const noexcept
{
    return {_config.common.sample_rate, _config.channels};
}

StreamResult DeviceInputSource::start(AudioSink& sink)
{
    if (!_stream) {
        return std::unexpected(
            AudioError(AudioErrorCode::stream_start_failed, "DeviceInputSource: not open"));
    }
    if (_stream->is_running()) {
        return {};
    }
    // Ended by a callback result: the backend stream is no longer running but may still need a
    // stop before it can restart.
    if (auto r = _stream->stop(); !r) {
        return r;
    }
    sync_with_last_callback();
    _sink.store(&sink, std::memory_order_release);
    if (auto r = _stream->start(); !r) {
        _sink.store(nullptr, std::memory_order_relaxed);
        return r;
    }
    return {};
}

StreamResult DeviceInputSource::stop()
{
    if (!_stream) {
        return {};
    }
    StreamResult result = _stream->stop();
    if (result) {
        sync_with_last_callback();
        _sink.store(nullptr, std::memory_order_relaxed);
    }
    return result;
}

CallbackResult DeviceInputSource::input_callback(void* user_data, AudioBufferView input) noexcept
{
    auto&          self   = *static_cast<DeviceInputSource*>(user_data);
    CallbackResult result = CallbackResult::continue_stream;
    if (AudioSink* sink = self._sink.load(std::memory_order_acquire); sink != nullptr) {
        result = sink->write(input);
    }
    // Single writer: plain load + release store, no read-modify-write.
    self._callbacks.store(self._callbacks.load(std::memory_order_relaxed) + 1,
                          std::memory_order_release);
    return result;
}

void DeviceInputSource::sync_with_last_callback() noexcept
{
    (void)_callbacks.load(std::memory_order_acquire);
}

}  // namespace dth::audio
