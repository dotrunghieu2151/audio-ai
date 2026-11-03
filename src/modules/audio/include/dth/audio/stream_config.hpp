#pragma once

#include <cstdint>
#include <optional>

namespace dth::audio {
struct StreamConfig
{
    double sample_rate = 48000.0;

    // 0 means the backend choose
    std::uint32_t frames_per_buffer = 0;

    // suggested latency, in seconds
    std::optional<double> suggested_latency;

    // whether the stream should use a low-latency configuration
    bool low_latency = true;
};

struct InputStreamConfig
{
    StreamConfig common;

    std::uint32_t channels = 1;
};

struct OutputStreamConfig
{
    StreamConfig common;

    std::uint32_t channels = 2;
};

struct DuplexStreamConfig
{
    StreamConfig common;

    std::uint32_t input_channels  = 1;
    std::uint32_t output_channels = 2;
};
}  // namespace dth::audio
