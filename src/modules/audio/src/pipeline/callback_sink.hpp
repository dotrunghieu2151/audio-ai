#pragma once

#include "dth/audio/audio-types.hpp"
#include "dth/audio/audio_format.hpp"
#include "dth/audio/audio_sink.hpp"

namespace dth::audio {

// AudioSink that forwards each write() to a C callback (fn pointer + user data, no allocation).
class CallbackSink final : public AudioSink
{
  public:
    CallbackSink(SinkCallback callback, void* user_data) noexcept
        : _callback{callback}, _user_data{user_data}
    {
    }

    StreamResult prepare(const AudioFormat& /*input*/) override { return {}; }
    StreamResult start() override { return {}; }
    StreamResult stop() override { return {}; }

    CallbackResult write(AudioBufferView buffer) noexcept override
    {
        return _callback != nullptr ? _callback(_user_data, buffer) : CallbackResult::stop_stream;
    }

  private:
    SinkCallback _callback;
    void*        _user_data;
};

}  // namespace dth::audio
