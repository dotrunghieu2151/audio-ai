#include "pipeline/pipeline_stream.hpp"

#include <string>
#include <utility>

#include "dth/audio/audio_error.hpp"

namespace dth::audio {

StreamResult PipelineStream::EndDetector::start()
{
    _ended.store(false, std::memory_order_relaxed);
    return _next.start();
}

CallbackResult PipelineStream::EndDetector::write(AudioBufferView buffer) noexcept
{
    if (_ended.load(std::memory_order_relaxed)) {
        return CallbackResult::stop_stream;
    }
    CallbackResult const result = _next.write(buffer);
    if (result != CallbackResult::continue_stream) {
        _ended.store(true, std::memory_order_release);
    }
    return result;
}

PipelineStream::PipelineStream(std::unique_ptr<AudioSource> source,
                               std::unique_ptr<AudioSink> sink, PipelineConfig config)
    : _source{std::move(source)}, _sink{std::move(sink)}
{
    if (_sink) {
        _chain = std::make_unique<Chain>(*_sink, config);
    }
}

PipelineStream::PipelineStream(PipelineStream&& other) noexcept
    : _source{std::move(other._source)},
      _sink{std::move(other._sink)},
      _chain{std::move(other._chain)},
      _state{std::exchange(other._state, StreamState::closed)}
{
}

PipelineStream::~PipelineStream()
{
    (void)close();
}

StreamResult PipelineStream::open()
{
    if (_state != StreamState::closed) {
        return {};
    }
    if (!_source || !_chain) {
        return std::unexpected(AudioError(AudioErrorCode::unsupported_configuration,
                                          "PipelineStream: no source or sink"));
    }
    if (auto r = _source->open(); !r) {
        return r;
    }
    if (auto r = _chain->head.prepare(_source->format()); !r) {
        (void)_source->close();
        return r;
    }
    _state = StreamState::open;
    return {};
}

StreamResult PipelineStream::start()
{
    if (_state == StreamState::closed) {
        return std::unexpected(AudioError(AudioErrorCode::stream_start_failed,
                                          "PipelineStream: not open. open() first"));
    }
    if (_state == StreamState::running) {
        if (!ended()) {
            return {};
        }
        // Ended by the sink: stop the idle source before restarting.
        if (auto r = stop(); !r) {
            return r;
        }
    }
    // Consumer first: the sink must be started before the source writes into it.
    if (auto r = _chain->head.start(); !r) {
        return r;
    }
    if (auto r = _source->start(_chain->head); !r) {
        (void)_chain->head.stop();
        return r;
    }
    _state = StreamState::running;
    return {};
}

StreamResult PipelineStream::stop()
{
    if (_state != StreamState::running) {
        return {};
    }
    // Producer first. AudioSource::stop() guarantees no write() after it returns.
    StreamResult       result = _source->stop();
    StreamResult const sink   = _chain->head.stop();
    if (result && !sink) {
        result = sink;
    }
    _state = StreamState::stopped;
    return result;
}

StreamResult PipelineStream::close()
{
    if (_state == StreamState::closed) {
        return {};
    }
    StreamResult result = stop();
    if (auto r = _source->close(); result && !r) {
        result = r;
    }
    if (auto r = _chain->head.close(); result && !r) {
        result = r;
    }
    _state = StreamState::closed;
    return result;
}

bool PipelineStream::ended() const noexcept
{
    return _chain && _chain->head.ended();
}

bool PipelineStream::is_open() const noexcept
{
    return _state == StreamState::open;
}

bool PipelineStream::is_running() const noexcept
{
    return _state == StreamState::running && !ended();
}

bool PipelineStream::is_stopped() const noexcept
{
    return _state == StreamState::stopped || (_state == StreamState::running && ended());
}

bool PipelineStream::is_closed() const noexcept
{
    return _state == StreamState::closed;
}

}  // namespace dth::audio
