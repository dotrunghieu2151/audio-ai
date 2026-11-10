#include "dth/audio/audio_system.hpp"

// #include "backends/portaudio/audio_system_portaudio_impl.hpp"
#include "backends/cubeb/audio_system_cubeb_impl.hpp"
#include "internal/audio_system_impl_concept.hpp"
#include "pipeline/callback_sink.hpp"
#include "pipeline/pipeline_stream.hpp"
#include "system_audio/system_audio_source.hpp"
namespace dth::audio {

struct AudioSystemImplValidator
{
    static_assert(AudioSystemImpl<AudioSystem::Impl>,
                  "AudioSystem::Impl does not satisfy AudioSystemImpl constraints");
};

AudioSystem::AudioSystem() : _impl{std::make_unique<Impl>()} {}
AudioSystem::~AudioSystem()                                 = default;
AudioSystem::AudioSystem(AudioSystem&&) noexcept            = default;
AudioSystem& AudioSystem::operator=(AudioSystem&&) noexcept = default;

std::vector<AudioDevice> AudioSystem::devices() const { return _impl->devices(); }

std::optional<AudioDevice> AudioSystem::default_input_device() const
{
    return _impl->default_input_device();
}

std::optional<AudioDevice> AudioSystem::default_output_device() const
{
    return _impl->default_output_device();
}

AudioStream AudioSystem::make_stream(const AudioDevice& device, const InputStreamConfig& config,
                                     InputCallback callback, void* user_data)
{
    return _impl->make_stream(device, config, callback, user_data);
}

AudioStream AudioSystem::make_stream(const AudioDevice& device, const OutputStreamConfig& config,
                                     OutputCallback callback, void* user_data)
{
    return _impl->make_stream(device, config, callback, user_data);
}

AudioStream AudioSystem::make_stream(const AudioDevice& input, const AudioDevice& output,
                                     const DuplexStreamConfig& config, DuplexCallback callback,
                                     void* user_data)
{
    return _impl->make_stream(input, output, config, callback, user_data);
}

AudioStream AudioSystem::make_stream(std::unique_ptr<AudioSource> source,
                                     std::unique_ptr<AudioSink> sink, const PipelineConfig& config)
{
    return AudioStream{std::make_unique<AudioStream::Impl>(
        PipelineStream{std::move(source), std::move(sink), config})};
}

AudioStream AudioSystem::make_stream(std::unique_ptr<AudioSource> source, SinkCallback callback,
                                     void* user_data, const PipelineConfig& config)
{
    return make_stream(std::move(source), std::make_unique<CallbackSink>(callback, user_data),
                       config);
}

AudioStream AudioSystem::make_system_audio_stream(std::unique_ptr<AudioSink> sink,
                                                  const PipelineConfig&      config)
{
    return make_stream(make_system_audio_source(), std::move(sink), config);
}

AudioStream AudioSystem::make_system_audio_stream(SinkCallback callback, void* user_data,
                                                  const PipelineConfig& config)
{
    return make_stream(make_system_audio_source(), callback, user_data, config);
}

std::unique_ptr<AudioSink> AudioSystem::make_output_sink(const AudioDevice&      output,
                                                         const OutputSinkConfig& config)
{
    return _impl->make_output_sink(output, config);
}

std::unique_ptr<AudioSource> AudioSystem::make_input_source(const AudioDevice&       input,
                                                            const InputStreamConfig& config)
{
    return _impl->make_input_source(input, config);
}

std::unique_ptr<AudioSource> AudioSystem::make_system_audio_source()
{
    return std::make_unique<SystemAudioSource>();
}
}  // namespace dth::audio
