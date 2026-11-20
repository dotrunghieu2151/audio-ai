#include "dth/audio/audio_transcriber.hpp"

#include <audiocpp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <expected>
#include <memory>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "dth/audio/audio_sink.hpp"
#include "dth/audio/audio_source.hpp"
#include "dth/audio/audio_system.hpp"
#include "dth/audio/pipeline_config.hpp"
#include "dth/common/spsc_ring_buffer.hpp"

namespace {
void check_status(audiocpp_status status)
{
    if (status == AUDIOCPP_OK) {
        return;
    }

    // Read this immediately after the failing call.
    const char* detail = audiocpp_last_error();

    std::string message = "audio.cpp error: ";

    if (detail && *detail) {
        message += detail;
    }
    else {
        message += audiocpp_status_string(status);
    }

    throw std::runtime_error(message);
}

void validate_abi()
{
    const std::uint32_t version = audiocpp_abi_version();

    const std::uint32_t major = version >> 16;

    if (major != AUDIOCPP_ABI_VERSION_MAJOR) {
        throw std::runtime_error("Incompatible audio.cpp C ABI version");
    }
}

template <auto Free>
struct HandleDeleter
{
    template <typename T>
    void operator()(T* handle) const noexcept
    {
        Free(handle);
    }
};

using SessionPtr = std::unique_ptr<audiocpp_session, HandleDeleter<&audiocpp_session_free>>;
using RequestPtr = std::unique_ptr<audiocpp_request, HandleDeleter<&audiocpp_request_free>>;
using ResultPtr  = std::unique_ptr<audiocpp_result, HandleDeleter<&audiocpp_result_free>>;
using EventPtr   = std::unique_ptr<audiocpp_event, HandleDeleter<&audiocpp_event_free>>;

// Copies the text out of `result`: the strings are borrowed from it.
// Empty when the task produced no text.
dth::audio::AudioTranscriber::TranscribeResult read_text(const audiocpp_result* result)
{
    const char* text     = nullptr;
    const char* language = nullptr;

    const audiocpp_status status = audiocpp_result_text(result, &text, &language);
    if (status == AUDIOCPP_ERR_NOT_AVAILABLE) {
        return {};
    }

    check_status(status);

    return text ? dth::audio::AudioTranscriber::TranscribeResult{.text{text},
                                                                .language{language ? language : ""}}
                : dth::audio::AudioTranscriber::TranscribeResult{};
}

// audio.cpp streaming ASR takes contiguous mono 16 kHz chunks (parakeet_tdt rejects anything else).
constexpr int stream_sample_rate = 16000;
constexpr int stream_channels    = 1;

// Real-time -> worker headroom: how far transcription may fall behind before audio is dropped.
constexpr std::size_t stream_ring_samples = std::size_t{stream_sample_rate} * stream_channels * 30;

// The real-time thread cannot wake the worker, so the worker polls the ring.
constexpr std::chrono::milliseconds stream_poll_interval{50};

// End of a transcriber stream: the source's real-time thread writes into a ring, a worker thread
// pushes it to the audio.cpp streaming session in the family's preferred chunk size and reports
// transcripts through the callback. The worker lives between start() and stop().
class TranscriberSink final : public dth::audio::AudioSink
{
  public:
    using StreamCallback = dth::audio::AudioTranscriber::StreamCallback;

    TranscriberSink(SessionPtr session, StreamCallback callback, void* user_data) noexcept
        : session_{std::move(session)}, callback_{callback}, user_data_{user_data}
    {
    }

    dth::audio::StreamResult prepare(const dth::audio::AudioFormat& input) override
    {
        using dth::audio::AudioError;
        using dth::audio::AudioErrorCode;

        if (input != dth::audio::AudioFormat{stream_sample_rate, stream_channels}) {
            return std::unexpected(AudioError(AudioErrorCode::unsupported_configuration,
                                              "Transcriber stream needs mono 16 kHz input"));
        }

        std::int64_t preferred_samples = 0;
        if (audiocpp_stream_policy(session_.get(), nullptr, nullptr, &preferred_samples, nullptr) !=
            AUDIOCPP_OK) {
            return std::unexpected(AudioError(AudioErrorCode::stream_open_failed,
                                              last_error("audiocpp_stream_policy")));
        }
        // No preference: push what has arrived each poll.
        if (preferred_samples <= 0) {
            preferred_samples = stream_sample_rate * stream_poll_interval.count() / 1000;
        }

        chunk_.resize(static_cast<std::size_t>(preferred_samples) * stream_channels);
        ring_ = std::make_unique<dth::common::SpscRingBuffer<float>>(
            std::max(stream_ring_samples, chunk_.size() * 2));
        return {};
    }

    dth::audio::StreamResult start() override
    {
        using dth::audio::AudioError;
        using dth::audio::AudioErrorCode;

        // stream_start prepares the session from the request's audio format; only the format is
        // read, the sample itself is never transcribed.
        const RequestPtr request{audiocpp_request_create()};
        if (!request) {
            return std::unexpected(AudioError(AudioErrorCode::stream_start_failed,
                                              "Failed to create audio.cpp request"));
        }
        const float silence = 0.0F;
        if (audiocpp_request_set_audio(request.get(), &silence, 1, stream_sample_rate,
                                       stream_channels) != AUDIOCPP_OK) {
            return std::unexpected(AudioError(AudioErrorCode::stream_start_failed,
                                              last_error("audiocpp_request_set_audio")));
        }
        if (audiocpp_stream_start(session_.get(), request.get()) != AUDIOCPP_OK) {
            return std::unexpected(AudioError(AudioErrorCode::stream_start_failed,
                                              last_error("audiocpp_stream_start")));
        }

        // Audio a failed worker left behind belongs to the previous stream. No producer or
        // consumer runs here: the source starts after this sink, the last worker was joined.
        while (ring_->read(chunk_) > 0) {
        }

        next_sample_ = 0;
        error_.clear();
        failed_.store(false, std::memory_order_relaxed);
        dropped_samples_.store(0, std::memory_order_relaxed);
        worker_ =std::jthread{[this](std::stop_token stop) { run(stop); }};
        return {};
    }

    // Runs after the source stopped: the worker drains the ring, finishes the stream and exits.
    dth::audio::StreamResult stop() override
    {
        if (worker_.joinable()) {
            worker_.request_stop();
            worker_.join();
        }
        // join() orders the worker's error_ write before this read.
        if (failed_.load(std::memory_order_relaxed)) {
            return std::unexpected(
                dth::audio::AudioError(dth::audio::AudioErrorCode::stream_stop_failed, error_));
        }
        // The source is stopped, so the count is final.
        if (const auto dropped = dropped_samples_.load(std::memory_order_relaxed); dropped > 0) {
            return std::unexpected(dth::audio::AudioError(
                dth::audio::AudioErrorCode::stream_stop_failed,
                "Transcription fell behind: dropped " + std::to_string(dropped) + " samples"));
        }
        return {};
    }

    // Real-time thread. A full ring drops the excess (the worker fell more than
    // stream_ring_samples behind); stop() reports it.
    dth::audio::CallbackResult write(dth::audio::AudioBufferView buffer) noexcept override
    {
        if (failed_.load(std::memory_order_relaxed)) {
            return dth::audio::CallbackResult::stop_stream;
        }
        const auto samples = buffer.samples();
        if (const std::size_t written = ring_->write(samples); written < samples.size()) {
            dropped_samples_.fetch_add(samples.size() - written, std::memory_order_relaxed);
        }
        return dth::audio::CallbackResult::continue_stream;
    }

  private:
    static std::string last_error(const char* operation)
    {
        const char* detail = audiocpp_last_error();
        return std::string{operation} + " failed: " + (detail && *detail ? detail : "unknown");
    }

    void run(const std::stop_token& stop) noexcept
    {
        try {
            while (!stop.stop_requested()) {
                if (ring_->size() < chunk_.size()) {
                    std::this_thread::sleep_for(stream_poll_interval);
                    continue;
                }
                ring_->read(chunk_);
                push(chunk_);
            }

            // The source is stopped: what is left in the ring is the tail of the stream.
            while (const std::size_t count = ring_->read(chunk_)) {
                push(std::span{chunk_}.first(count));
            }
            finish();
        } catch (const std::exception& e) {
            error_ = e.what();
            failed_.store(true, std::memory_order_relaxed);
        } catch (...) {
            error_ = "Transcriber stream worker failed";
            failed_.store(true, std::memory_order_relaxed);
        }
    }

    void push(std::span<const float> samples)
    {
        const std::size_t frames = samples.size() / stream_channels;

        audiocpp_event* raw_event = nullptr;
        check_status(audiocpp_stream_push(session_.get(), samples.data(), frames,
                                          stream_sample_rate, stream_channels, next_sample_,
                                          &raw_event));
        next_sample_ += static_cast<std::int64_t>(frames);

        report(EventPtr{raw_event});

        // Families that queue events themselves (PULL_EVENTS policy) hand them out here.
        for (;;) {
            raw_event = nullptr;
            check_status(audiocpp_stream_next_event(session_.get(), &raw_event));
            if (!raw_event) {
                break;
            }
            report(EventPtr{raw_event});
        }
    }

    void report(const EventPtr& event)
    {
        if (!event) {
            return;
        }
        const auto result = read_text(audiocpp_event_as_result(event.get()));
        if (!result.text.empty()) {
            callback_(user_data_, result, false);
        }
    }

    void finish()
    {
        // Finishing a stream that received no audio is an error in audio.cpp.
        if (next_sample_ == 0) {
            return;
        }

        audiocpp_result* raw_result = nullptr;
        check_status(audiocpp_stream_finish(session_.get(), &raw_result));
        const ResultPtr result{raw_result};

        callback_(user_data_, read_text(result.get()), true);
    }

    SessionPtr     session_;
    StreamCallback callback_;
    void*          user_data_;

    std::unique_ptr<dth::common::SpscRingBuffer<float>> ring_;
    std::vector<float>                                  chunk_;  // worker only

    std::int64_t      next_sample_ = 0;  // worker only while running
    std::string       error_;            // written by the worker, read after join
    std::atomic<bool> failed_{false};
    std::atomic<std::uint64_t> dropped_samples_{0};

    // Last: stops and joins before the state it uses is destroyed.
    std::jthread worker_;
};
}  // namespace
namespace dth::audio {

class AudioTranscriber::Impl
{
  public:
    explicit Impl(Config config) : config_(std::move(config))
    {
        try {
            initialize();
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~Impl() { cleanup(); }

    Impl(const Impl&)            = delete;
    Impl& operator=(const Impl&) = delete;

    TranscribeResult transcribe(std::span<const float> samples, int sample_rate, int channels,
                                int frames)
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

        //
        // Release the previous result before running another request.
        audiocpp_result_free(result_);
        result_ = nullptr;

        audiocpp_request_set_option(request_, "audio_chunk_mode", "vad");

        // audiocpp_request_set_option(request_, "audio_chunk_duration_sec", "5.0");

        // audiocpp_request_set_audio copies the PCM data.
        check_status(
            audiocpp_request_set_audio(request_, samples.data(), frames, sample_rate, channels));

        check_status(audiocpp_session_run(session_, request_, &result_));

        return read_text(result_);
    }

    AudioStream make_stream(AudioSystem& system, std::unique_ptr<AudioSource> source,
                            StreamCallback callback, void* user_data)
    {
        if (callback == nullptr) {
            throw std::invalid_argument("callback cannot be null");
        }

        if (!audiocpp_model_supports(model_, "asr", "streaming")) {
            throw std::runtime_error("Model does not support streaming ASR");
        }

        const audiocpp_backend_config backend_config{config_.backend_str().data(), config_.device,
                                                     config_.threads};

        // A session per stream: handles are not thread-safe, and the stream's worker thread drives
        // this one. It keeps the model alive on its own.
        audiocpp_session* raw_session = nullptr;
        check_status(audiocpp_session_create(model_, "asr", "streaming", &backend_config, nullptr,
                                             &raw_session));
        SessionPtr session{raw_session};

        return system.make_stream(
            std::move(source),
            std::make_unique<TranscriberSink>(std::move(session), callback, user_data),
            PipelineConfig{
                .processing = ProcessingFlags::resample | ProcessingFlags::remap_channels,
                .target     = {.sample_rate = stream_sample_rate, .channels = stream_channels},
            });
    }

  private:
    void initialize()
    {
        validate_abi();

        if (config_.model_path.empty()) {
            throw std::invalid_argument("model_path cannot be empty");
        }

        // 1. Create model registry.
        check_status(audiocpp_registry_create(nullptr, &registry_));

        // 2. Load Model
        const audiocpp_model_config model_config{config_.model.c_str(), nullptr, nullptr, nullptr};

        check_status(audiocpp_model_load(registry_, config_.model_path.c_str(), &model_config,
                                         nullptr, &model_));

        // 3. Verify model capabilities.
        if (!audiocpp_model_supports(model_, "asr", "offline")) {
            throw std::runtime_error("Model does not support offline ASR");
        }

        // 4. Configure Metal backend.
        const audiocpp_backend_config backend_config{config_.backend_str().data(), config_.device,
                                                     config_.threads};

        // 5. Optional session options.
        audiocpp_options* options = nullptr;
        options                   = audiocpp_options_create();

        if (!options) {
            throw std::runtime_error("Failed to create audio.cpp options");
        }

        try {
            // if (!config_.model_options.empty()) {
            // check_status(audiocpp_options_set(options, "haas_asr.matmul_weight_type",
            //                                   config_.matmul_weight_type.c_str()));
            // }
            check_status(audiocpp_options_set(options, "parakeet_tdt.offline_mode", "auto"));
            check_status(audiocpp_options_set(options, "parakeet_tdt.vad_model_path",
                                              "assets/models/silero_vad"));
            check_status(
                audiocpp_options_set(options, "parakeet_tdt.audio_chunk_duration_sec", "10"));

            // if (!config.vad_model_path.empty()) {
            //     check_status(
            //         audiocpp_request_set_option(
            //             request,
            //             "haas_asr.vad_model_path",
            //             config.vad_model_path.c_str()
            //         ),
            //         "Set VAD model path"
            //     );
            // }
            check_status(audiocpp_session_create(model_, "asr", "offline", &backend_config, options,
                                                 &session_));
        } catch (...) {
            audiocpp_options_free(options);
            throw;
        }

        audiocpp_options_free(options);

        // 6. Allocate a reusable request.
        request_ = audiocpp_request_create();

        if (!request_) {
            throw std::runtime_error("Failed to create audio.cpp request");
        }
    }

    void cleanup() noexcept
    {
        audiocpp_result_free(result_);
        result_ = nullptr;

        audiocpp_request_free(request_);
        request_ = nullptr;

        audiocpp_session_free(session_);
        session_ = nullptr;

        audiocpp_model_free(model_);
        model_ = nullptr;

        audiocpp_registry_free(registry_);
        registry_ = nullptr;
    }

  private:
    Config config_;

    audiocpp_registry* registry_ = nullptr;
    audiocpp_model*    model_    = nullptr;
    audiocpp_session*  session_  = nullptr;
    audiocpp_request*  request_  = nullptr;
    audiocpp_result*   result_   = nullptr;
};

AudioTranscriber::AudioTranscriber(Config config) : impl_(std::make_unique<Impl>(std::move(config)))
{
}
AudioTranscriber::~AudioTranscriber() = default;

AudioTranscriber::TranscribeResult AudioTranscriber::transcribe(std::span<const float> samples,
                                                                int sample_rate, int channels,
                                                                int frames)
{
    return impl_->transcribe(samples, sample_rate, channels, frames);
}

AudioStream AudioTranscriber::make_stream(AudioSystem& system, std::unique_ptr<AudioSource> source,
                                          StreamCallback callback, void* user_data)
{
    return impl_->make_stream(system, std::move(source), callback, user_data);
}
}  // namespace dth::audio
