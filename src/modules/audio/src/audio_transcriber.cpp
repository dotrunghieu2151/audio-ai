#include "dth/audio/audio_transcriber.hpp"

#include <audiocpp.h>

#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include "dth/audio/audio_source.hpp"
#include "dth/audio/audio_system.hpp"
#include "dth/audio/pipeline_config.hpp"
#include "transcription/audiocpp_support.hpp"
#include "transcription/transcriber_sink.hpp"

namespace dth::audio {

namespace {

using audiocpp::check_status;

void validate_abi()
{
    const std::uint32_t version = audiocpp_abi_version();

    const std::uint32_t major = version >> 16;

    if (major != AUDIOCPP_ABI_VERSION_MAJOR) {
        throw std::runtime_error("Incompatible audio.cpp C ABI version");
    }
}

audiocpp::RegistryPtr create_registry()
{
    audiocpp_registry* registry = nullptr;
    check_status(audiocpp_registry_create(nullptr, &registry));
    return audiocpp::RegistryPtr{registry};
}

}  // namespace

class AudioTranscriber::Impl
{
  public:
    explicit Impl(Config config) : config_(std::move(config))
    {
        validate_abi();

        if (config_.model_path.empty()) {
            throw std::invalid_argument("model_path cannot be empty");
        }

        registry_ = create_registry();
        model_    = audiocpp::load_model(registry_.get(), config_.model_path, config_.model);

        if (!audiocpp_model_supports(model_.get(), "asr", "offline")) {
            throw std::runtime_error("Model does not support offline ASR");
        }

        audiocpp::OptionMap session_options = config_.session_options;
        if (uses_vad_chunking()) {
            // try_emplace: an explicit session option wins.
            session_options.try_emplace(config_.model + ".vad_model_path", config_.vad_model_path);
        }
        const auto options = audiocpp::make_options(session_options);
        session_           = audiocpp::create_session(model_.get(), "asr", "offline",
                                                      &backend_config_, options.get());
    }

    TranscribeResult transcribe(std::span<const float> samples, int sample_rate, int channels,
                                int frames, const audiocpp::OptionMap& request_options)
    {
        if (samples.empty()) {
            return {};
        }

        if (sample_rate <= 0) {
            throw std::invalid_argument("sample_rate must be positive");
        }

        if (channels <= 0) {
            throw std::invalid_argument("channels must be positive");
        }
        if (frames <= 0) {
            throw std::invalid_argument("frames must be positive");
        }

        // A fresh request per call: a request keeps every option ever set on it.
        const audiocpp::RequestPtr request = audiocpp::make_request();

        audiocpp::OptionMap options = request_options;
        if (uses_vad_chunking()) {
            // try_emplace: the caller's audio_chunk_mode wins.
            options.try_emplace("audio_chunk_mode", "vad");
        }
        audiocpp::set_request_options(request.get(), options);

        // audiocpp_request_set_audio copies the PCM data.
        check_status(
            audiocpp_request_set_audio(request.get(), samples.data(), frames, sample_rate, channels));

        audiocpp_result* raw_result = nullptr;
        check_status(audiocpp_session_run(session_.get(), request.get(), &raw_result));
        const audiocpp::ResultPtr result{raw_result};

        return audiocpp::read_text(result.get());
    }

    AudioStream make_stream(AudioSystem& system, std::unique_ptr<AudioSource> source,
                            const StreamConfig& config, StreamCallback callback, void* user_data)
    {
        auto sink = make_transcriber_sink(
            StreamAsrModel{
                .registry        = registry_.get(),
                .model           = model_.get(),
                .backend         = &backend_config_,
                .session_options = &config_.session_options,
            },
            config, callback, user_data);

        return system.make_stream(
            std::move(source), std::move(sink),
            PipelineConfig{
                .processing = ProcessingFlags::resample | ProcessingFlags::remap_channels,
                .target = {.sample_rate = audiocpp::sample_rate, .channels = audiocpp::channels},
            });
    }

  private:
    [[nodiscard]] bool uses_vad_chunking() const noexcept
    {
        return !config_.vad_model_path.empty();
    }

    Config config_;
    // Built once; backend_str() returns a string literal, so the name stays valid.
    audiocpp_backend_config backend_config_{config_.backend_str().data(), config_.device,
                                            config_.threads};

    // Declaration order: the session is freed before its model, the model before the registry.
    audiocpp::RegistryPtr registry_;
    audiocpp::ModelPtr    model_;
    audiocpp::SessionPtr  session_;
};

AudioTranscriber::AudioTranscriber(Config config) : impl_(std::make_unique<Impl>(std::move(config)))
{
}
AudioTranscriber::~AudioTranscriber() = default;

AudioTranscriber::TranscribeResult AudioTranscriber::transcribe(
    std::span<const float> samples, int sample_rate, int channels, int frames,
    const std::map<std::string, std::string>& request_options)
{
    return impl_->transcribe(samples, sample_rate, channels, frames, request_options);
}

AudioStream AudioTranscriber::make_stream(AudioSystem& system, std::unique_ptr<AudioSource> source,
                                          const StreamConfig& config, StreamCallback callback,
                                          void* user_data)
{
    return impl_->make_stream(system, std::move(source), config, callback, user_data);
}
}  // namespace dth::audio
