#include "dth/audio/audio_stream.hpp"

#include <memory>

#include "audio_stream_impl.hpp"
#include "internal/audio_stream_impl_concept.hpp"

namespace dth::audio {

struct AudioStreamImplValidator
{
    static_assert(AudioStreamImpl<BackendStreamHandle>,
                  "BackendStreamHandle does not satisfy AudioStreamImpl constraints");
    static_assert(AudioStreamImpl<PipelineStream>,
                  "PipelineStream does not satisfy AudioStreamImpl constraints");
    static_assert(AudioStreamImpl<AudioStream::Impl>,
                  "AudioStream::Impl does not satisfy AudioStreamImpl constraints");
};

AudioStream::AudioStream(std::unique_ptr<Impl> impl) : _impl{std::move(impl)} {}
AudioStream::~AudioStream() = default;

AudioStream::AudioStream(AudioStream&&) noexcept            = default;
AudioStream& AudioStream::operator=(AudioStream&&) noexcept = default;

bool AudioStream::is_open() const noexcept { return _impl->is_open(); }

bool AudioStream::is_running() const noexcept { return _impl->is_running(); }

bool AudioStream::is_stopped() const noexcept { return _impl->is_stopped(); }

bool AudioStream::is_closed() const noexcept { return _impl->is_closed(); }

StreamResult AudioStream::open() { return _impl->open(); }

StreamResult AudioStream::start() { return _impl->start(); }

StreamResult AudioStream::stop() { return _impl->stop(); }

StreamResult AudioStream::close() { return _impl->close(); }

}  // namespace dth::audio
