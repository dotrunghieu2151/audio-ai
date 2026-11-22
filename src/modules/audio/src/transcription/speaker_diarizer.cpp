#include "transcription/speaker_diarizer.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace dth::audio {

namespace {

// Shorter turns (10 ms frames) are dropped: the low-latency profile flips between similar voices
// for 0.1-0.3 s.
const audiocpp::OptionMap request_options = {{"speaker_min_frames", "30"}};

double to_seconds(std::int64_t samples)
{
    return static_cast<double>(samples) / audiocpp::sample_rate;
}

}  // namespace

SpeakerDiarizer::SpeakerDiarizer(audiocpp::SessionPtr              session,
                                 AudioTranscriber::SpeakerCallback callback, void* user_data)
    : session_{std::move(session)}, callback_{callback}, user_data_{user_data}
{
    std::int64_t preferred = 0;
    audiocpp::check_status(
        audiocpp_stream_policy(session_.get(), nullptr, nullptr, &preferred, nullptr));
    if (preferred <= 0) {
        throw std::runtime_error("Diarization model reports no streaming chunk size");
    }
    chunk_samples_ = static_cast<std::size_t>(preferred);
    buffer_.reserve(chunk_samples_);
}

void SpeakerDiarizer::start()
{
    audiocpp::start_stream(session_.get(), request_options);
    buffer_.clear();
    pushed_samples_ = 0;
    reported_.clear();
}

void SpeakerDiarizer::push(std::span<const float> samples)
{
    while (!samples.empty()) {
        const std::size_t take = std::min(samples.size(), chunk_samples_ - buffer_.size());
        buffer_.insert(buffer_.end(), samples.begin(),
                       samples.begin() + static_cast<std::ptrdiff_t>(take));
        samples = samples.subspan(take);
        if (buffer_.size() == chunk_samples_) {
            push_chunk(buffer_);
            buffer_.clear();
        }
    }
}

void SpeakerDiarizer::finish()
{
    if (!buffer_.empty()) {
        push_chunk(buffer_);
        buffer_.clear();
    }
    audiocpp_result* raw = nullptr;
    audiocpp::check_status(audiocpp_stream_finish(session_.get(), &raw));
    const audiocpp::ResultPtr       result{raw};
    AudioTranscriber::SpeakerUpdate update;
    report(result.get(), to_seconds(pushed_samples_), update);
    callback_(user_data_, update);
}

void SpeakerDiarizer::push_chunk(std::span<const float> chunk)
{
    audiocpp_event* raw = nullptr;
    audiocpp::check_status(audiocpp_stream_push(session_.get(), chunk.data(), chunk.size(),
                                                audiocpp::sample_rate, audiocpp::channels,
                                                pushed_samples_, &raw));
    pushed_samples_ += static_cast<std::int64_t>(chunk.size());
    const double complete_until = complete_until_sec();

    // One update per chunk, also without turns: complete_until_sec advances.
    AudioTranscriber::SpeakerUpdate update;
    for (audiocpp::EventPtr event{raw}; event;) {
        report(audiocpp_event_as_result(event.get()), complete_until, update);
        audiocpp_event* next = nullptr;
        audiocpp::check_status(audiocpp_stream_next_event(session_.get(), &next));
        event.reset(next);
    }
    update.complete_until_sec = complete_until;
    callback_(user_data_, update);
}

double SpeakerDiarizer::complete_until_sec() const
{
    const auto chunk   = static_cast<std::int64_t>(chunk_samples_);
    const auto decided = std::max<std::int64_t>(0, pushed_samples_ - right_context_samples);
    return to_seconds(decided / chunk * chunk);
}

void SpeakerDiarizer::report(const audiocpp_result* result, double complete_until_sec,
                             AudioTranscriber::SpeakerUpdate& update)
{
    update.complete_until_sec = complete_until_sec;
    if (result == nullptr) {
        return;
    }
    const std::size_t count = audiocpp_result_speaker_turn_count(result);
    for (std::size_t i = 0; i < count; ++i) {
        std::int64_t start   = 0;
        std::int64_t end     = 0;
        const char*  speaker = nullptr;
        audiocpp::check_status(
            audiocpp_result_speaker_turn(result, i, &start, &end, &speaker, nullptr, nullptr));
        std::string name = speaker != nullptr ? speaker : "";
        if (reported_.emplace(start, name).second) {
            update.turns.push_back(
                {.speaker = std::move(name), .start_sec = to_seconds(start), .end_sec = to_seconds(end)});
        }
    }
    std::ranges::sort(update.turns, {}, &AudioTranscriber::SpeakerTurn::start_sec);
}

}  // namespace dth::audio
