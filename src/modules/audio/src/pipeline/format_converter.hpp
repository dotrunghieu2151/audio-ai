#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "dth/audio/audio-types.hpp"
#include "dth/audio/audio_format.hpp"

struct SpeexResamplerState_;

namespace dth::audio {

// Converts float32 interleaved audio between two AudioFormats: channel mapping + resampling with a
// runtime-adjustable ratio (for clock drift compensation).
//
// Pull model: process() fills an output buffer, pulling input frames on demand. Unconsumed input
// is kept internally between calls.
//
// Threading: prepare() on the control thread (allocates). set_ratio()/process() on one real-time
// thread; they never allocate, lock or do IO.
//
// Channel mapping (resampling runs at the smaller channel count):
// - identity; mono -> N: duplicate; N -> mono: average;
// - otherwise: copy the first min(in, out) channels, zero the rest.
class FormatConverter final
{
  public:
    // Writes up to dst.size() / input.channels frames into `dst` (interleaved, input format).
    // Returns frames written; 0 means no input available right now.
    using PullFn = std::size_t (*)(void* user_data, std::span<float> dst) noexcept;

    // Largest accepted |ratio - 1| for set_ratio(). prepare() sizes the resampler for this range,
    // which is what keeps set_ratio() allocation free.
    static constexpr double max_ratio_deviation = 0.005;

    FormatConverter();
    ~FormatConverter();

    FormatConverter(const FormatConverter&)            = delete;
    FormatConverter& operator=(const FormatConverter&) = delete;
    FormatConverter(FormatConverter&&) noexcept;
    FormatConverter& operator=(FormatConverter&&) noexcept;

    // `max_chunk_frames` bounds internal scratch buffers; any process() size still works.
    // Resets all state; ratio back to 1.
    StreamResult prepare(const AudioFormat& input, const AudioFormat& output,
                         std::size_t max_chunk_frames);

    // Multiplier on the nominal input/output rate ratio: > 1 consumes input faster. Clamped to
    // 1 +- max_ratio_deviation, quantized to ~15 ppm. Each effective change recomputes the
    // resampler's filter table (no allocation, but O(filter length x oversampling) work).
    void set_ratio(double ratio) noexcept;

    [[nodiscard]]
    double ratio() const noexcept;

    // Fills `output` (must match the output format's channel count). Returns frames written;
    // fewer than output.frames only when `pull` ran out of input. Remaining frames are untouched.
    std::size_t process(MutableAudioBufferView output, PullFn pull, void* user_data) noexcept;

  private:
    struct ResamplerDeleter
    {
        void operator()(SpeexResamplerState_* state) const noexcept;
    };

    void apply_ratio(double ratio) noexcept;

    std::unique_ptr<SpeexResamplerState_, ResamplerDeleter> _resampler;

    AudioFormat   _input{};
    AudioFormat   _output{};
    std::uint32_t _resample_channels{0};  // min(input.channels, output.channels)
    std::uint32_t _in_rate{0};
    std::uint32_t _out_rate{0};
    std::size_t   _max_chunk_frames{0};
    double        _ratio{1.0};
    std::uint32_t _ratio_num{0};  // numerator last passed to speex (over kRatioDen)

    std::vector<float> _pulled;     // input format
    std::vector<float> _staged;     // _resample_channels, used when downmixing before resampling
    std::vector<float> _resampled;  // _resample_channels, used when upmixing after resampling

    const float* _staged_data{nullptr};  // points into _pulled or _staged
    std::size_t  _staged_offset{0};      // frames already consumed by the resampler
    std::size_t  _staged_frames{0};
};

}  // namespace dth::audio
