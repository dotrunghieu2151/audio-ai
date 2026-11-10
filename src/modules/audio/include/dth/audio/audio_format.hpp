#pragma once

#include <cstdint>

namespace dth::audio {

// Samples are always float32 interleaved inside the pipeline.
struct AudioFormat
{
    double        sample_rate = 0.0;
    std::uint32_t channels    = 0;

    friend bool operator==(const AudioFormat&, const AudioFormat&) = default;
};

}  // namespace dth::audio
