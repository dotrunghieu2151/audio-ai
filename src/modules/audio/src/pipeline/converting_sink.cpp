#include "pipeline/converting_sink.hpp"

#include <algorithm>
#include <cstring>

#include "dth/audio/audio_error.hpp"

namespace dth::audio {

ConvertingSink::ConvertingSink(AudioSink& next, PipelineConfig config) noexcept
    : _next{next}, _config{config}
{
}

StreamResult ConvertingSink::prepare(const AudioFormat& input)
{
    AudioFormat output = input;
    if (has_flag(_config.processing, ProcessingFlags::resample)) {
        output.sample_rate = _config.target.sample_rate;
    }
    if (has_flag(_config.processing, ProcessingFlags::remap_channels)) {
        output.channels = _config.target.channels;
    }
    if (!(output.sample_rate >= 1.0) || output.channels == 0) {
        return std::unexpected(AudioError(AudioErrorCode::unsupported_configuration,
                                          "ConvertingSink: invalid target format"));
    }

    _passthrough = output == input;
    if (_passthrough) {
        _scratch.clear();
        _scratch.shrink_to_fit();
    }
    else {
        if (auto r = _converter.prepare(input, output, chunk_frames); !r) {
            return r;
        }
        _scratch.assign(chunk_frames * output.channels, 0.0f);
    }
    _input  = input;
    _output = output;
    return _next.prepare(output);
}

StreamResult ConvertingSink::start()
{
    // Producer idle: drop resampler history from the previous run.
    if (!_passthrough) {
        if (auto r = _converter.prepare(_input, _output, chunk_frames); !r) {
            return r;
        }
    }
    return _next.start();
}

StreamResult ConvertingSink::stop()
{
    return _next.stop();
}

StreamResult ConvertingSink::close()
{
    return _next.close();
}

CallbackResult ConvertingSink::write(AudioBufferView buffer) noexcept
{
    if (_passthrough) {
        return _next.write(buffer);
    }
    if (buffer.data == nullptr || buffer.channels != _input.channels) {
        return CallbackResult::continue_stream;
    }

    _pending        = buffer;
    _pending_offset = 0;
    for (;;) {
        std::size_t const n = _converter.process({_scratch.data(), chunk_frames, _output.channels},
                                                 &ConvertingSink::pull, this);
        if (n > 0) {
            CallbackResult const result = _next.write({_scratch.data(), n, _output.channels});
            if (result != CallbackResult::continue_stream) {
                return result;
            }
        }
        if (n < chunk_frames) {
            return CallbackResult::continue_stream;  // input used up
        }
    }
}

AudioFormat ConvertingSink::output_format() const noexcept
{
    return _output;
}

std::size_t ConvertingSink::pull(void* user_data, std::span<float> dst) noexcept
{
    auto&             self     = *static_cast<ConvertingSink*>(user_data);
    std::size_t const channels = self._input.channels;
    std::size_t const frames =
        std::min<std::size_t>(dst.size() / channels, self._pending.frames - self._pending_offset);
    std::memcpy(dst.data(), self._pending.data + self._pending_offset * channels,
                frames * channels * sizeof(float));
    self._pending_offset += frames;
    return frames;
}

}  // namespace dth::audio
