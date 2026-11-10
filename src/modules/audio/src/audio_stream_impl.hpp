#pragma once

#include <utility>
#include <variant>

// #include "backends/portaudio/audio_stream_portaudio_impl.hpp"
#include "backends/cubeb/audio_stream_cubeb_impl.hpp"
#include "dth/audio/audio_stream.hpp"
#include "pipeline/pipeline_stream.hpp"

namespace dth::audio {

// Static dispatch over every stream kind. New kinds are new variant
// alternatives; each must satisfy the AudioStreamImpl concept (checked in audio_stream.cpp).
class AudioStream::Impl
{
  public:
    // In-place construction: backend handles have deleted move assignment.
    explicit Impl(BackendStreamHandle handle)
        : _stream{std::in_place_type<BackendStreamHandle>, std::move(handle)}
    {
    }

    explicit Impl(PipelineStream stream)
        : _stream{std::in_place_type<PipelineStream>, std::move(stream)}
    {
    }

    bool is_open() const noexcept
    {
        return std::visit([](const auto& s) { return s.is_open(); }, _stream);
    }

    bool is_running() const noexcept
    {
        return std::visit([](const auto& s) { return s.is_running(); }, _stream);
    }

    bool is_stopped() const noexcept
    {
        return std::visit([](const auto& s) { return s.is_stopped(); }, _stream);
    }

    bool is_closed() const noexcept
    {
        return std::visit([](const auto& s) { return s.is_closed(); }, _stream);
    }

    StreamResult open()
    {
        return std::visit([](auto& s) { return s.open(); }, _stream);
    }

    StreamResult start()
    {
        return std::visit([](auto& s) { return s.start(); }, _stream);
    }

    StreamResult stop()
    {
        return std::visit([](auto& s) { return s.stop(); }, _stream);
    }

    StreamResult close()
    {
        return std::visit([](auto& s) { return s.close(); }, _stream);
    }

  private:
    std::variant<BackendStreamHandle, PipelineStream> _stream;
};

}  // namespace dth::audio
