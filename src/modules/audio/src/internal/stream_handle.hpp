#pragma once

#include "dth/audio/audio-types.hpp"

namespace dth::audio {
class IAudioStreamHandle
{
  public:
    virtual ~IAudioStreamHandle() = default;

    virtual StreamResult open()  = 0;
    virtual StreamResult start() = 0;
    virtual StreamResult stop()  = 0;
    virtual StreamResult close() = 0;

    [[nodiscard]] virtual bool is_open() const noexcept    = 0;
    [[nodiscard]] virtual bool is_running() const noexcept = 0;
    [[nodiscard]] virtual bool is_stopped() const noexcept = 0;

    // virtual StreamInfo info() const = 0;
};
}  // namespace dth::audio
