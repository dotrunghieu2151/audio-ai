#pragma once

#include <cstddef>
#include <vector>

#include "dth/audio/audio-types.hpp"
#include "dth/audio/audio_format.hpp"
#include "dth/audio/audio_sink.hpp"
#include "dth/audio/pipeline_config.hpp"
#include "pipeline/format_converter.hpp"

namespace dth::audio {

// AudioSink decorator: converts the pushed audio to the format selected by a PipelineConfig
// (fixed-ratio resample and / or channel remap) and pushes it into `next` on the same thread.
//
// - No flag set, or the target equals the input: plain passthrough, no copy.
// - Output blocks are at most chunk_frames long; one write() may produce several downstream
//   writes (or none, while the resampler fills its history). Stops at the first downstream
//   result other than continue_stream and returns it.
// - prepare/start/stop/close forward to `next` (prepare with the converted format).
class ConvertingSink final : public AudioSink
{
  public:
    static constexpr std::size_t chunk_frames = 1024;

    ConvertingSink(AudioSink& next, PipelineConfig config) noexcept;

    StreamResult prepare(const AudioFormat& input) override;
    StreamResult start() override;
    StreamResult stop() override;
    StreamResult close() override;

    CallbackResult write(AudioBufferView buffer) noexcept override;

    // Format pushed into `next`. Valid after prepare().
    [[nodiscard]]
    AudioFormat output_format() const noexcept;

  private:
    static std::size_t pull(void* user_data, std::span<float> dst) noexcept;

    AudioSink&      _next;
    PipelineConfig  _config;
    AudioFormat     _input{};
    AudioFormat     _output{};
    bool            _passthrough{true};
    FormatConverter _converter;
    std::vector<float> _scratch;  // chunk_frames * output channels

    // Producer RT thread: the write() being converted.
    AudioBufferView _pending{};
    std::size_t     _pending_offset{0};  // frames
};

}  // namespace dth::audio
