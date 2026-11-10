#include "pipeline/format_converter.hpp"

#include <speex/speex_resampler.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace dth::audio {
namespace {

constexpr int kQuality = 5;

// Fixed ratio denominator; ratio resolution 1 / 2^16 (~15 ppm). Must stay <= 2^16: on a ratio
// change speex rescales its fractional phase (< old den) by the new den in 32-bit arithmetic and
// fails half-way through, leaving a corrupt state, if that product overflows.
// Odd numerators keep the fraction irreducible, see prime_for_ratio_range().
constexpr std::uint32_t kRatioDen = 1u << 16;

AudioError make_error(std::string message, std::optional<int> native_code = std::nullopt)
{
    return AudioError{AudioErrorCode::unsupported_configuration, std::move(message), native_code};
}

std::uint64_t ratio_num(double nominal, double ratio) noexcept
{
    return static_cast<std::uint64_t>(std::llround(nominal * ratio * kRatioDen));
}

// speex reallocates its filter table / history in set_rate_frac() whenever they must grow. Their
// size depends on the ratio (filter length grows with the downsampling factor, oversampling halves
// past 2x/4x/8x/16x). Visit the largest-size points of the allowed range once here, so later
// set_rate_frac() calls within the range never allocate.
int prime_for_ratio_range(SpeexResamplerState* st, double nominal, std::uint32_t in_rate,
                          std::uint32_t out_rate) noexcept
{
    double const lo = nominal * (1.0 - FormatConverter::max_ratio_deviation);
    double const hi = nominal * (1.0 + FormatConverter::max_ratio_deviation);

    std::array<std::uint64_t, 6> nums{};
    std::size_t                  count = 0;
    // Odd numerators: the fraction stays num / 2^20, forcing the interpolating (largest) table.
    nums[count++] = ratio_num(lo, 1.0) | 1u;
    nums[count++] = ratio_num(hi, 1.0) | 1u;
    for (double const boundary : {2.0, 4.0, 8.0, 16.0}) {
        if (lo < boundary && boundary <= hi) {
            // Just below the boundary: longest filter before oversampling halves.
            nums[count++] = static_cast<std::uint64_t>(boundary) * kRatioDen - 1;
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        int const err = speex_resampler_set_rate_frac(st, static_cast<std::uint32_t>(nums[i]),
                                                      kRatioDen, in_rate, out_rate);
        if (err != RESAMPLER_ERR_SUCCESS) {
            return err;
        }
    }
    return RESAMPLER_ERR_SUCCESS;
}

void map_channels(const float* src, std::uint32_t src_channels, float* dst,
                  std::uint32_t dst_channels, std::size_t frames) noexcept
{
    if (src_channels == 1) {
        for (std::size_t f = 0; f < frames; ++f) {
            std::fill_n(dst + f * dst_channels, dst_channels, src[f]);
        }
    }
    else if (dst_channels == 1) {
        float const scale = 1.0f / static_cast<float>(src_channels);
        for (std::size_t f = 0; f < frames; ++f) {
            float const* in  = src + f * src_channels;
            float        sum = 0.0f;
            for (std::uint32_t c = 0; c < src_channels; ++c) {
                sum += in[c];
            }
            dst[f] = sum * scale;
        }
    }
    else {
        std::uint32_t const common = std::min(src_channels, dst_channels);
        for (std::size_t f = 0; f < frames; ++f) {
            float const* in  = src + f * src_channels;
            float*       out = dst + f * dst_channels;
            std::copy_n(in, common, out);
            std::fill(out + common, out + dst_channels, 0.0f);
        }
    }
}

}  // namespace

void FormatConverter::ResamplerDeleter::operator()(SpeexResamplerState_* state) const noexcept
{
    speex_resampler_destroy(state);
}

FormatConverter::FormatConverter()                                      = default;
FormatConverter::~FormatConverter()                                     = default;
FormatConverter::FormatConverter(FormatConverter&&) noexcept            = default;
FormatConverter& FormatConverter::operator=(FormatConverter&&) noexcept = default;

StreamResult FormatConverter::prepare(const AudioFormat& input, const AudioFormat& output,
                                      std::size_t max_chunk_frames)
{
    _resampler.reset();

    if (!(input.sample_rate >= 1.0) || !(output.sample_rate >= 1.0) || input.channels == 0 ||
        output.channels == 0 || max_chunk_frames == 0 ||
        max_chunk_frames > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(make_error("FormatConverter: invalid format or chunk size"));
    }

    auto const in_rate  = static_cast<std::uint32_t>(std::lround(input.sample_rate));
    auto const out_rate = static_cast<std::uint32_t>(std::lround(output.sample_rate));
    double const nominal = static_cast<double>(in_rate) / static_cast<double>(out_rate);
    if (ratio_num(nominal, 1.0 + max_ratio_deviation) >= std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(make_error("FormatConverter: sample rate ratio too large"));
    }

    std::uint32_t const resample_channels = std::min(input.channels, output.channels);

    int  err = RESAMPLER_ERR_SUCCESS;
    auto st  = std::unique_ptr<SpeexResamplerState_, ResamplerDeleter>{speex_resampler_init_frac(
        resample_channels, static_cast<std::uint32_t>(ratio_num(nominal, 1.0)), kRatioDen, in_rate,
        out_rate, kQuality, &err)};
    if (!st) {
        return std::unexpected(
            make_error(std::string{"speex_resampler_init_frac: "} + speex_resampler_strerror(err),
                       err));
    }
    err = prime_for_ratio_range(st.get(), nominal, in_rate, out_rate);
    if (err == RESAMPLER_ERR_SUCCESS) {
        err = speex_resampler_set_rate_frac(
            st.get(), static_cast<std::uint32_t>(ratio_num(nominal, 1.0)), kRatioDen, in_rate,
            out_rate);
    }
    if (err == RESAMPLER_ERR_SUCCESS) {
        err = speex_resampler_reset_mem(st.get());
    }
    if (err != RESAMPLER_ERR_SUCCESS) {
        return std::unexpected(
            make_error(std::string{"speex_resampler_set_rate_frac: "} +
                           speex_resampler_strerror(err),
                       err));
    }

    _input             = input;
    _output            = output;
    _resample_channels = resample_channels;
    _in_rate           = in_rate;
    _out_rate          = out_rate;
    _max_chunk_frames  = max_chunk_frames;
    _ratio             = 1.0;
    _ratio_num         = static_cast<std::uint32_t>(ratio_num(nominal, 1.0));

    _pulled.assign(max_chunk_frames * input.channels, 0.0f);
    _staged.assign(output.channels < input.channels ? max_chunk_frames * resample_channels : 0,
                   0.0f);
    _resampled.assign(output.channels > input.channels ? max_chunk_frames * resample_channels : 0,
                      0.0f);
    _staged_data   = nullptr;
    _staged_offset = 0;
    _staged_frames = 0;

    _resampler = std::move(st);
    return {};
}

void FormatConverter::set_ratio(double ratio) noexcept
{
    if (_resampler) {
        apply_ratio(ratio);
    }
}

double FormatConverter::ratio() const noexcept
{
    return _ratio;
}

void FormatConverter::apply_ratio(double ratio) noexcept
{
    ratio = std::clamp(ratio, 1.0 - max_ratio_deviation, 1.0 + max_ratio_deviation);
    double const nominal = static_cast<double>(_in_rate) / static_cast<double>(_out_rate);
    auto const   num     = static_cast<std::uint32_t>(ratio_num(nominal, ratio));
    _ratio               = ratio;
    // Skip unchanged fractions here: speex stores the reduced fraction, so its own early return
    // misses even numerators and would rebuild the filter table on every call.
    if (num == _ratio_num) {
        return;
    }
    // Within the primed range: never allocates.
    // Cannot fail: rates were validated in prepare() and kRatioDen avoids the phase overflow.
    [[maybe_unused]] int const err =
        speex_resampler_set_rate_frac(_resampler.get(), num, kRatioDen, _in_rate, _out_rate);
    assert(err == RESAMPLER_ERR_SUCCESS);
    _ratio_num = num;
}

std::size_t FormatConverter::process(MutableAudioBufferView output, PullFn pull,
                                     void* user_data) noexcept
{
    if (!_resampler || output.data == nullptr || output.channels != _output.channels) {
        return 0;
    }

    bool const   upmix       = _output.channels > _input.channels;
    bool const   downmix     = _output.channels < _input.channels;
    double const in_per_out  = static_cast<double>(_in_rate) / _out_rate * _ratio;
    std::size_t  produced    = 0;
    std::size_t const wanted = output.frames;

    while (produced < wanted) {
        if (_staged_offset == _staged_frames) {
            // Pull about as much input as the remaining output needs, so input stays in the
            // caller's buffer (e.g. a ring whose fill level drives drift control).
            auto const estimate = static_cast<std::size_t>(
                std::ceil(static_cast<double>(wanted - produced) * in_per_out)) + 2;
            std::size_t const request = std::min(estimate, _max_chunk_frames);
            std::size_t const pulled  = std::min(
                pull(user_data, std::span<float>{_pulled.data(), request * _input.channels}),
                request);
            if (pulled == 0) {
                break;
            }
            if (downmix) {
                map_channels(_pulled.data(), _input.channels, _staged.data(), _output.channels,
                             pulled);
                _staged_data = _staged.data();
            }
            else {
                _staged_data = _pulled.data();
            }
            _staged_offset = 0;
            _staged_frames = pulled;
        }

        std::size_t const out_room =
            upmix ? std::min(wanted - produced, _max_chunk_frames) : wanted - produced;
        float* const out_ptr =
            upmix ? _resampled.data() : output.data + produced * _output.channels;

        auto in_len  = static_cast<spx_uint32_t>(_staged_frames - _staged_offset);
        auto out_len = static_cast<spx_uint32_t>(
            std::min<std::size_t>(out_room, std::numeric_limits<spx_uint32_t>::max()));
        speex_resampler_process_interleaved_float(
            _resampler.get(), _staged_data + _staged_offset * _resample_channels, &in_len, out_ptr,
            &out_len);

        if (upmix) {
            map_channels(_resampled.data(), _input.channels,
                         output.data + produced * _output.channels, _output.channels, out_len);
        }
        _staged_offset += in_len;
        produced += out_len;

        if (in_len == 0 && out_len == 0) {
            break;  // Defensive: the resampler made no progress.
        }
    }
    return produced;
}

}  // namespace dth::audio
