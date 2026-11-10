#pragma once

#include <cstdint>

namespace dth::audio {

// Real-time errors are reported as counters, never as AudioError (which allocates).
struct PipelineStats
{
    std::uint64_t underruns      = 0;
    std::uint64_t overruns       = 0;
    double        resample_ratio = 1.0;
};

}  // namespace dth::audio
