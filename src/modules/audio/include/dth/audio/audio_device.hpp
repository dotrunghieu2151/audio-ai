#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dth_audio/export_dth_audio.hpp"

namespace dth::audio {
struct DTH_AUDIO_EXPORT AudioDevice
{
    std::uint32_t id;
    std::string   name;
    std::string   host_api;
    // Additional metadata.
    std::uint32_t max_input_channels;
    std::uint32_t max_output_channels;
    double        default_sample_rate;
};
}  // namespace dth::audio
