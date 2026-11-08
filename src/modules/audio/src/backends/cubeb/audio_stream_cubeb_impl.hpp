#pragma once
#include <memory>

#include "backends/cubeb/cubeb_stream.hpp"
#include "dth/audio/audio_stream.hpp"

namespace dth::audio {

class AudioStream::Impl
{
  public:
    Impl(CubebStreamHandle handle) : _handle{std::move(handle)} {}

    bool is_open() const noexcept { return _handle.is_open(); }

    bool is_running() const noexcept { return _handle.is_running(); }

    bool is_stopped() const noexcept { return _handle.is_stopped(); }

    bool is_closed() const noexcept { return _handle.is_closed(); }

    StreamResult open() { return _handle.open(); }

    StreamResult start() { return _handle.start(); }

    StreamResult stop() { return _handle.stop(); }

    StreamResult close() { return _handle.close(); }

  private:
    CubebStreamHandle _handle;
};
}  // namespace dth::audio
