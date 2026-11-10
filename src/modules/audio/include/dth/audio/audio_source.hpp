#pragma once

#include "dth/audio/audio-types.hpp"
#include "dth/audio/audio_format.hpp"
#include "dth/audio/audio_sink.hpp"
#include "dth_audio/export_dth_audio.hpp"

namespace dth::audio {

// Produces audio on its own real-time thread and pushes it synchronously into an AudioSink.
//
// Threading: all members are called on the control thread.
//
// Lifecycle: open -> (start -> stop)* -> close.
class DTH_AUDIO_EXPORT AudioSource
{
  public:
    virtual ~AudioSource() = default;

    // Acquires the underlying resource; format() is valid afterwards.
    virtual StreamResult open()  = 0;
    // Stops if running, releases the resource. Idempotent.
    virtual StreamResult close() = 0;

    [[nodiscard]]
    virtual AudioFormat format() const noexcept = 0;

    // `sink` must already be prepared with format() and started. It must outlive stop().
    // Once sink.write() returns anything but continue_stream, no further write() until the next
    // start().
    virtual StreamResult start(AudioSink& sink) = 0;

    // Returns only after the last sink.write() has returned; no write() happens afterwards.
    virtual StreamResult stop() = 0;
};

}  // namespace dth::audio
