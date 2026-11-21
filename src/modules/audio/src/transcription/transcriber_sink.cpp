#include "transcription/transcriber_sink.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <expected>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "dth/common/spsc_ring_buffer.hpp"
#include "transcription/segment_transcribers.hpp"
#include "transcription/speech_segmenter.hpp"

namespace dth::audio {

namespace {

using StreamConfig = AudioTranscriber::StreamConfig;

constexpr std::int64_t seconds_to_samples(double seconds)
{
    return static_cast<std::int64_t>(seconds * audiocpp::sample_rate);
}

// Real-time -> worker headroom: how far transcription may fall behind before audio is dropped.
constexpr std::size_t stream_ring_samples = std::size_t{audiocpp::sample_rate} * 30;

// The real-time thread cannot wake the worker, so the worker polls the ring.
constexpr std::chrono::milliseconds stream_poll_interval{50};

// Cutting utterances that reach the maximum: any silence between words counts as a pause, and
// speech is not padded so the gaps keep their true extent.
constexpr int pause_min_silence_ms = 50;
constexpr int pause_speech_pad_ms  = 0;
// Kept while idle so a padded speech start finds its audio: the padding plus the frames Silero
// needs to confirm speech.
constexpr double vad_detection_margin_sec = 0.8;

// Bounds of the StreamConfig values seconds_to_samples() and Silero take.
constexpr double max_text_latency_sec = 60.0;
constexpr double min_utterance_sec    = 2.0;
constexpr double max_utterance_sec    = 60.0;
constexpr int    max_min_silence_ms   = 5000;
constexpr int    max_speech_pad_ms    = 1000;

// End of a transcriber stream: the source's real-time thread writes into a ring; a worker thread
// feeds it in VAD frames to the VoiceActivityDetector and the SpeechSegmenter (which cuts long
// speech at pauses the SileroPauseFinder picks), whose utterances the SegmentTranscriber turns
// into callbacks. The worker lives between start() and stop().
class TranscriberSink final : public AudioSink
{
  public:
    TranscriberSink(VoiceActivityDetector vad, SileroPauseFinder pause_finder,
                    std::unique_ptr<SegmentTranscriber> transcriber,
                    SpeechSegmenter::Config             segmenter_config)
        : vad_{std::move(vad)},
          pause_finder_{std::move(pause_finder)},
          transcriber_{std::move(transcriber)},
          segmenter_{*transcriber_, &pause_finder_, segmenter_config}
    {
    }

    StreamResult prepare(const AudioFormat& input) override
    {
        if (input != AudioFormat{audiocpp::sample_rate, audiocpp::channels}) {
            return std::unexpected(AudioError(AudioErrorCode::unsupported_configuration,
                                              "Transcriber stream needs mono 16 kHz input"));
        }

        frame_.resize(VoiceActivityDetector::frame_samples);
        ring_ = std::make_unique<dth::common::SpscRingBuffer<float>>(stream_ring_samples);
        return {};
    }

    StreamResult start() override
    {
        try {
            vad_.start();
            transcriber_->start();
        } catch (const std::exception& e) {
            return std::unexpected(AudioError(AudioErrorCode::stream_start_failed, e.what()));
        }
        segmenter_.reset();

        // Audio a failed worker left behind belongs to the previous stream. No producer or
        // consumer runs here: the source starts after this sink, the last worker was joined.
        while (ring_->read(frame_) > 0) {
        }

        error_.clear();
        failed_.store(false, std::memory_order_relaxed);
        dropped_samples_.store(0, std::memory_order_relaxed);
        worker_ = std::jthread{[this](std::stop_token stop) { run(stop); }};
        return {};
    }

    // Runs after the source stopped: the worker drains the ring, closes the last utterance and
    // exits.
    StreamResult stop() override
    {
        if (worker_.joinable()) {
            worker_.request_stop();
            worker_.join();
        }
        // join() orders the worker's error_ write before this read.
        if (failed_.load(std::memory_order_relaxed)) {
            return std::unexpected(AudioError(AudioErrorCode::stream_stop_failed, error_));
        }
        // The source is stopped, so the count is final.
        if (const auto dropped = dropped_samples_.load(std::memory_order_relaxed); dropped > 0) {
            return std::unexpected(AudioError(
                AudioErrorCode::stream_stop_failed,
                "Transcription fell behind: dropped " + std::to_string(dropped) + " samples"));
        }
        return {};
    }

    // Real-time thread. A full ring drops the excess (the worker fell more than
    // stream_ring_samples behind); stop() reports it.
    CallbackResult write(AudioBufferView buffer) noexcept override
    {
        if (failed_.load(std::memory_order_relaxed)) {
            return CallbackResult::stop_stream;
        }
        const auto samples = buffer.samples();
        if (const std::size_t written = ring_->write(samples); written < samples.size()) {
            dropped_samples_.fetch_add(samples.size() - written, std::memory_order_relaxed);
        }
        return CallbackResult::continue_stream;
    }

  private:
    void run(const std::stop_token& stop) noexcept
    {
        try {
            while (!stop.stop_requested()) {
                if (ring_->size() < frame_.size()) {
                    std::this_thread::sleep_for(stream_poll_interval);
                    continue;
                }
                ring_->read(frame_);
                process(frame_);
            }

            // The source is stopped: what is left in the ring is the tail of the stream. A last
            // partial frame is too short for the VAD; it still reaches the open utterance.
            while (const std::size_t count = ring_->read(frame_)) {
                if (count == frame_.size()) {
                    process(frame_);
                }
                else {
                    segmenter_.append(std::span{frame_}.first(count));
                }
            }
            segmenter_.finish();
            transcriber_->finish();
        } catch (const std::exception& e) {
            error_ = e.what();
            failed_.store(true, std::memory_order_relaxed);
        } catch (...) {
            error_ = "Transcriber stream worker failed";
            failed_.store(true, std::memory_order_relaxed);
        }
    }

    void process(std::span<const float> frame)
    {
        segmenter_.append(frame);
        vad_.process(frame, segmenter_);
    }

    VoiceActivityDetector               vad_;
    SileroPauseFinder                   pause_finder_;
    std::unique_ptr<SegmentTranscriber> transcriber_;
    SpeechSegmenter                     segmenter_;  // worker only while running

    std::unique_ptr<dth::common::SpscRingBuffer<float>> ring_;
    std::vector<float>                                  frame_;  // worker only while running

    std::string                error_;  // written by the worker, read after join
    std::atomic<bool>          failed_{false};
    std::atomic<std::uint64_t> dropped_samples_{0};

    // Last: stops and joins before the state it uses is destroyed.
    std::jthread worker_;
};

void validate(const StreamConfig& config)
{
    if (!(config.text_latency_sec >= 0.0 && config.text_latency_sec <= max_text_latency_sec)) {
        throw std::invalid_argument("text_latency_sec must be within [0, 60]");
    }
    if (!(config.max_utterance_sec >= min_utterance_sec &&
          config.max_utterance_sec <= max_utterance_sec)) {
        throw std::invalid_argument("max_utterance_sec must be within [2, 60]");
    }
    if (config.vad_min_silence_ms < 0 || config.vad_min_silence_ms > max_min_silence_ms) {
        throw std::invalid_argument("vad_min_silence_ms must be within [0, 5000]");
    }
    if (config.vad_speech_pad_ms < 0 || config.vad_speech_pad_ms > max_speech_pad_ms) {
        throw std::invalid_argument("vad_speech_pad_ms must be within [0, 1000]");
    }
    int    previous_silence_ms = config.vad_min_silence_ms;
    double previous_after_sec  = 0.0;
    for (const auto& step : config.vad_adaptive_silence) {
        if (!(step.after_sec > previous_after_sec)) {
            throw std::invalid_argument("vad_adaptive_silence: after_sec must increase from 0");
        }
        if (step.min_silence_ms < 0 || step.min_silence_ms >= previous_silence_ms) {
            throw std::invalid_argument(
                "vad_adaptive_silence: min_silence_ms must decrease, staying >= 0");
        }
        previous_after_sec  = step.after_sec;
        previous_silence_ms = step.min_silence_ms;
    }
}

// Silero on the CPU: a 512-sample frame is too little work to pay a GPU dispatch for.
audiocpp::SessionPtr create_vad_session(const audiocpp_model* vad_model, const char* mode,
                                        int min_silence_ms, int speech_pad_ms)
{
    if (!audiocpp_model_supports(vad_model, "vad", mode)) {
        throw std::runtime_error(std::string{"VAD model does not support "} + mode + " VAD");
    }
    const auto options = audiocpp::make_options({
        {"min_silence_duration_ms", std::to_string(min_silence_ms)},
        {"speech_pad_ms", std::to_string(speech_pad_ms)},
    });
    return audiocpp::create_session(vad_model, "vad", mode, nullptr, options.get());
}

// One level per silence setting: the base, then each adaptive step the utterance can reach.
VoiceActivityDetector make_vad(const audiocpp_model* vad_model, const StreamConfig& config)
{
    std::vector<VoiceActivityDetector::Level> levels;
    levels.push_back({create_vad_session(vad_model, "streaming", config.vad_min_silence_ms,
                                         config.vad_speech_pad_ms),
                      0});
    for (const auto& step : config.vad_adaptive_silence) {
        // Never reached: the utterance is cut at max_utterance_sec first.
        if (step.after_sec >= config.max_utterance_sec) {
            break;
        }
        levels.push_back({create_vad_session(vad_model, "streaming", step.min_silence_ms,
                                             config.vad_speech_pad_ms),
                          seconds_to_samples(step.after_sec)});
    }
    return VoiceActivityDetector{std::move(levels)};
}

std::unique_ptr<SegmentTranscriber> make_segment_transcriber(
    const StreamAsrModel& asr, const StreamConfig& config, std::int64_t history_samples,
    AudioTranscriber::StreamCallback callback, void* user_data)
{
    const bool offline = config.strategy == StreamConfig::Strategy::offline_per_utterance;
    const char* mode   = offline ? "offline" : "streaming";
    if (!audiocpp_model_supports(asr.model, "asr", mode)) {
        throw std::runtime_error(std::string{"Model does not support "} + mode + " ASR");
    }

    const auto options = audiocpp::make_options(*asr.session_options);
    auto session = audiocpp::create_session(asr.model, "asr", mode, asr.backend, options.get());
    if (offline) {
        return std::make_unique<OfflineSegmentTranscriber>(
            std::move(session), config.request_options, callback, user_data);
    }
    return std::make_unique<ContinuousStreamTranscriber>(
        std::move(session), config.request_options, seconds_to_samples(config.text_latency_sec),
        history_samples, callback, user_data);
}

}  // namespace

std::unique_ptr<AudioSink> make_transcriber_sink(const StreamAsrModel&  asr,
                                                 const StreamConfig&    config,
                                                 AudioTranscriber::StreamCallback callback,
                                                 void*                            user_data)
{
    if (callback == nullptr) {
        throw std::invalid_argument("callback cannot be null");
    }
    validate(config);

    const std::int64_t history_samples =
        seconds_to_samples(vad_detection_margin_sec + config.vad_speech_pad_ms / 1000.0);

    // Sessions per stream: handles are not thread-safe, and the stream's worker thread drives
    // these. They keep their models alive on their own.
    auto transcriber = make_segment_transcriber(asr, config, history_samples, callback, user_data);

    const auto vad_model = audiocpp::load_model(asr.registry, config.vad_model_path, "silero_vad");
    auto       vad       = make_vad(vad_model.get(), config);
    auto pause_vad = create_vad_session(vad_model.get(), "offline", pause_min_silence_ms,
                                        pause_speech_pad_ms);

    return std::make_unique<TranscriberSink>(
        std::move(vad), SileroPauseFinder{std::move(pause_vad)}, std::move(transcriber),
        SpeechSegmenter::Config{
            .history_samples     = history_samples,
            .max_segment_samples = seconds_to_samples(config.max_utterance_sec),
        });
}

}  // namespace dth::audio
