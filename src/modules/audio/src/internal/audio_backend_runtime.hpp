#pragma once

#include <expected>

#include "dth/audio/audio_error.hpp"
namespace dth::audio {
class IAudioBackendRuntime
{
  public:
    virtual ~IAudioBackendRuntime() = default;

    virtual std::expected<void, AudioError> initialize() = 0;
};
}  // namespace dth::audio
