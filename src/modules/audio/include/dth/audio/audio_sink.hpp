#pragma once

#include "dth/audio/audio-types.hpp"
#include "dth/audio/audio_format.hpp"
#include "dth_audio/export_dth_audio.hpp"

namespace dth::audio {

// Receives audio pushed by an AudioSource or an upstream processor.
//
// Threading:
// - prepare/start/stop/close: control thread. May allocate, lock and fail.
// - write: producer real-time thread, only between start() and stop(). Must not allocate, lock,
//   do IO or syscalls. `buffer` is borrowed and valid only during the call.
//
// write() result (same meaning as a stream callback's):
// - continue_stream: keep writing.
// - stop_stream / complete_stream: the sink wants no more audio. The producer stops calling
//   write() until the next start(); the owning AudioStream then reports itself stopped. A real-time
//   thread cannot stop a device, so the device itself is stopped by the next control call
//   (stop(), start(), close() or destruction of the stream).
class DTH_AUDIO_EXPORT AudioSink
{
  public:
    virtual ~AudioSink() = default;

    // Allocate everything the real-time path needs for `input`.
    virtual StreamResult prepare(const AudioFormat& input) = 0;
    virtual StreamResult start()                           = 0;
    virtual StreamResult stop()                            = 0;
    // Releases what prepare() allocated. Default: nothing to release.
    virtual StreamResult close() { return {}; }

    virtual CallbackResult write(AudioBufferView buffer) noexcept = 0;
};

}  // namespace dth::audio
