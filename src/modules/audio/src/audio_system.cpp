#include "dth/audio/audio_system.hpp"

// #include "backends/portaudio/audio_system_portaudio_impl.hpp"
#include "backends/cubeb/audio_system_cubeb_impl.hpp"
#include "internal/audio_system_impl_concept.hpp"
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
}  // namespace dth::audio
