#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
namespace dth::audio {
struct AudioBufferView
{
    const float* data = nullptr;

    std::uint32_t frames   = 0;
    std::uint32_t channels = 0;

    [[nodiscard]]
    std::span<const float> samples() const noexcept
    {
        return {data, static_cast<std::size_t>(channels) * frames};
    }
};

struct MutableAudioBufferView
{
    float* data = nullptr;

    std::uint32_t frames   = 0;
    std::uint32_t channels = 0;

    [[nodiscard]]
    std::span<float> samples() const noexcept
    {
        return {data, static_cast<std::size_t>(channels) * frames};
    }
};
struct DuplexAudioBufferView
{
    AudioBufferView        input;
    MutableAudioBufferView output;
};
}  // namespace dth::audio
