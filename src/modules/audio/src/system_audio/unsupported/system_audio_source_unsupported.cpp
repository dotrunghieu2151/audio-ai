#include "system_audio/system_audio_source.hpp"

#include "dth/audio/audio_error.hpp"

namespace dth::audio {

SystemAudioSource::~SystemAudioSource() = default;

StreamResult SystemAudioSource::open()
{
    return std::unexpected(AudioError(AudioErrorCode::unsupported_configuration,
                                      "SystemAudioSource: system audio capture is not supported "
                                      "on this platform"));
}

StreamResult SystemAudioSource::close()
{
    return {};
}

bool SystemAudioSource::is_open() const noexcept
{
    return false;
}

bool SystemAudioSource::is_running() const noexcept
{
    return false;
}

AudioFormat SystemAudioSource::format() const noexcept
{
    return _format;
}

StreamResult SystemAudioSource::start(AudioSink& /*sink*/)
{
    return std::unexpected(
        AudioError(AudioErrorCode::stream_start_failed, "SystemAudioSource: not open"));
}

StreamResult SystemAudioSource::stop()
{
    return {};
}

}  // namespace dth::audio
