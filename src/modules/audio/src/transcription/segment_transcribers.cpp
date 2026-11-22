#include "transcription/segment_transcribers.hpp"

#include <algorithm>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace dth::audio {

namespace {

using audiocpp::check_status;

double to_seconds(std::int64_t sample) noexcept
{
    return static_cast<double>(sample) / audiocpp::sample_rate;
}

// Hands out the events a family queued itself (PULL_EVENTS policy) after `first`.
template <typename OnEvent>
void for_each_event(audiocpp_session* session, audiocpp_event* first, OnEvent&& on_event)
{
    on_event(audiocpp::EventPtr{first});
    for (;;) {
        audiocpp_event* raw_event = nullptr;
        check_status(audiocpp_stream_next_event(session, &raw_event));
        if (!raw_event) {
            return;
        }
        on_event(audiocpp::EventPtr{raw_event});
    }
}

// Shorter "utterances" are clicks and breaths: Silero's min_speech_duration_ms only applies offline.
constexpr std::int64_t min_utterance_samples = audiocpp::sample_rate / 4;

}  // namespace

// ---- VoiceActivityDetector ----

VoiceActivityDetector::VoiceActivityDetector(std::vector<Level> levels) noexcept
    : levels_{std::move(levels)}
{
}

void VoiceActivityDetector::start()
{
    for (const auto& level : levels_) {
        audiocpp::start_stream(level.session.get());
    }
    next_sample_ = 0;
}

void VoiceActivityDetector::process(std::span<const float> frame, SpeechSegmenter& segmenter)
{
    for (const auto& level : levels_) {
        audiocpp_event* raw_event = nullptr;
        check_status(audiocpp_stream_push(level.session.get(), frame.data(), frame.size(),
                                          audiocpp::sample_rate, audiocpp::channels, next_sample_,
                                          &raw_event));
        for_each_event(level.session.get(), raw_event, [&](const audiocpp::EventPtr& event) {
            report(event, level.min_utterance_samples, segmenter);
        });
    }
    next_sample_ += static_cast<std::int64_t>(frame.size());
}

void VoiceActivityDetector::report(const audiocpp::EventPtr& event,
                                   std::int64_t min_length, SpeechSegmenter& segmenter)
{
    if (!event) {
        return;
    }
    const std::size_t count = audiocpp_event_voice_activity_count(event.get());
    for (std::size_t i = 0; i < count; ++i) {
        audiocpp_voice_activity_kind kind{};
        std::int64_t                 sample = 0;
        check_status(audiocpp_event_voice_activity(event.get(), i, &kind, &sample, nullptr));

        if (kind == AUDIOCPP_VOICE_ACTIVITY_SPEECH_START) {
            segmenter.speech_start(sample);
        }
        else if (kind == AUDIOCPP_VOICE_ACTIVITY_SPEECH_END) {
            segmenter.speech_end(sample, min_length);
        }
        // SPEECH_SEGMENT repeats an END's range.
    }
}

// ---- SileroPauseFinder ----

SileroPauseFinder::SileroPauseFinder(audiocpp::SessionPtr session)
    : session_{std::move(session)}, request_{audiocpp::make_request()}
{
}

std::optional<std::int64_t> SileroPauseFinder::find_pause(std::span<const float> samples)
{
    if (samples.empty()) {
        return std::nullopt;
    }

    // audiocpp_request_set_audio copies the PCM data.
    check_status(audiocpp_request_set_audio(request_.get(), samples.data(), samples.size(),
                                            audiocpp::sample_rate, audiocpp::channels));

    audiocpp_result* raw_result = nullptr;
    check_status(audiocpp_session_run(session_.get(), request_.get(), &raw_result));
    const audiocpp::ResultPtr result{raw_result};

    // Only gaps between two speeches: the window is cut out of ongoing speech, so a gap at its
    // edge is Silero warming up or cooling down, not a pause.
    std::optional<std::int64_t> previous_end;
    std::int64_t                best_start = 0;
    std::int64_t                best_end   = 0;

    const std::size_t count = audiocpp_result_segment_count(result.get());
    for (std::size_t i = 0; i < count; ++i) {
        std::int64_t start = 0;
        std::int64_t end   = 0;
        check_status(audiocpp_result_segment(result.get(), i, &start, &end, nullptr, nullptr));
        if (previous_end && start - *previous_end >= best_end - best_start) {
            best_start = *previous_end;
            best_end   = start;
        }
        previous_end = std::max(previous_end.value_or(end), end);
    }

    if (best_end <= best_start) {
        return std::nullopt;
    }
    return best_start + (best_end - best_start) / 2;
}

// ---- OfflineSegmentTranscriber ----

OfflineSegmentTranscriber::OfflineSegmentTranscriber(audiocpp::SessionPtr             session,
                                                     const audiocpp::OptionMap& request_options,
                                                     AudioTranscriber::StreamCallback callback,
                                                     void*                            user_data)
    : session_{std::move(session)},
      request_{audiocpp::make_request()},
      callback_{callback},
      user_data_{user_data}
{
    // Kept by the request across utterances; only the audio changes.
    audiocpp::set_request_options(request_.get(), request_options);
}

void OfflineSegmentTranscriber::segment_end(std::int64_t start_sample, std::int64_t end_sample,
                                            std::span<const float> samples)
{
    if (end_sample - start_sample < min_utterance_samples) {
        return;
    }

    // audiocpp_request_set_audio copies the PCM data.
    check_status(audiocpp_request_set_audio(request_.get(), samples.data(), samples.size(),
                                            audiocpp::sample_rate, audiocpp::channels));

    audiocpp_result* raw_result = nullptr;
    check_status(audiocpp_session_run(session_.get(), request_.get(), &raw_result));
    const audiocpp::ResultPtr result{raw_result};

    auto text = audiocpp::read_text(result.get());
    if (text.text.empty()) {
        return;
    }
    callback_(user_data_, AudioTranscriber::TranscribedSegment{
                              .text      = std::move(text.text),
                              .language  = std::move(text.language),
                              .start_sec = to_seconds(start_sample),
                              .end_sec   = to_seconds(end_sample),
                          });
}

// ---- ContinuousStreamTranscriber ----

ContinuousStreamTranscriber::ContinuousStreamTranscriber(audiocpp::SessionPtr session,
                                                         audiocpp::OptionMap request_options,
                                                         std::int64_t text_latency_samples,
                                                         std::int64_t history_samples,
                                                         AudioTranscriber::StreamCallback callback,
                                                         void* user_data)
    : session_{std::move(session)},
      request_options_{std::move(request_options)},
      text_latency_samples_{text_latency_samples},
      history_samples_{history_samples},
      callback_{callback},
      user_data_{user_data}
{
}

void ContinuousStreamTranscriber::start()
{
    audiocpp::start_stream(session_.get(), request_options_);

    std::int64_t preferred_samples = 0;
    check_status(
        audiocpp_stream_policy(session_.get(), nullptr, nullptr, &preferred_samples, nullptr));
    // No preference: push as the VAD frames arrive.
    chunk_samples_ = preferred_samples > 0 ? static_cast<std::size_t>(preferred_samples)
                                           : VoiceActivityDetector::frame_samples;

    stream_samples_ = 0;
    skipped_.clear();
    skipped_start_ = 0;
    pending_audio_.clear();
    model_samples_ = 0;
    fed_runs_.clear();
    fed_samples_     = 0;
    fed_stream_next_ = -1;
    published_.clear();
    text_.clear();
    language_.clear();
    open_start_.reset();
    closed_.reset();
    last_end_ = 0;
}

void ContinuousStreamTranscriber::audio(std::span<const float> samples)
{
    const std::int64_t position = stream_samples_;
    stream_samples_ += static_cast<std::int64_t>(samples.size());

    if (feeding(position)) {
        feed(samples, position);
    }
    else {
        skip(position, samples);
    }
    emit_if_due();
}

void ContinuousStreamTranscriber::segment_begin(std::int64_t start_sample)
{
    // A new utterance before the previous one's lag elapsed: its text so far is all it gets.
    if (closed_) {
        emit(*closed_);
    }
    open_start_ = start_sample;

    // The VAD reports a start late: hand the model the skipped audio since then.
    if (!skipped_.empty()) {
        const std::int64_t from = std::max(start_sample, skipped_start_);
        const auto         tail = std::span{skipped_}.subspan(
            static_cast<std::size_t>(std::min(from - skipped_start_,
                                              static_cast<std::int64_t>(skipped_.size()))));
        std::vector<float> resumed{tail.begin(), tail.end()};
        const std::int64_t resumed_start =
            skipped_start_ + static_cast<std::int64_t>(skipped_.size() - tail.size());
        skipped_.clear();
        feed(resumed, resumed_start);
    }
}

void ContinuousStreamTranscriber::segment_end(std::int64_t start_sample, std::int64_t end_sample,
                                              std::span<const float> /*samples*/)
{
    open_start_.reset();
    closed_   = Range{start_sample, end_sample};
    last_end_ = end_sample;
}

// In speech, or within the lag after it: the model still owes that speech's last words.
bool ContinuousStreamTranscriber::feeding(std::int64_t position) const noexcept
{
    return open_start_ || (closed_ && position < closed_->end + text_latency_samples_);
}

void ContinuousStreamTranscriber::skip(std::int64_t position, std::span<const float> samples)
{
    if (skipped_.empty()) {
        skipped_start_ = position;
    }
    skipped_.insert(skipped_.end(), samples.begin(), samples.end());

    const auto excess = static_cast<std::int64_t>(skipped_.size()) - history_samples_;
    if (excess > 0) {
        skipped_.erase(skipped_.begin(), skipped_.begin() + static_cast<std::ptrdiff_t>(excess));
        skipped_start_ += excess;
    }
}

void ContinuousStreamTranscriber::feed(std::span<const float> samples, std::int64_t position)
{
    skipped_.clear();
    if (position != fed_stream_next_) {
        fed_runs_.push_back({.model_start = fed_samples_, .stream_start = position});
    }
    fed_samples_ += static_cast<std::int64_t>(samples.size());
    fed_stream_next_ = position + static_cast<std::int64_t>(samples.size());

    pending_audio_.insert(pending_audio_.end(), samples.begin(), samples.end());
    if (pending_audio_.size() < chunk_samples_) {
        return;
    }

    std::size_t offset = 0;
    for (; pending_audio_.size() - offset >= chunk_samples_; offset += chunk_samples_) {
        push(std::span{pending_audio_}.subspan(offset, chunk_samples_));
    }
    pending_audio_.erase(pending_audio_.begin(),
                         pending_audio_.begin() + static_cast<std::ptrdiff_t>(offset));
}

// The closed utterance's text has all been emitted once the lag after its end has passed.
void ContinuousStreamTranscriber::emit_if_due()
{
    if (closed_ && stream_samples_ >= closed_->end + text_latency_samples_) {
        emit(*closed_);
    }
}

void ContinuousStreamTranscriber::finish()
{
    if (!pending_audio_.empty()) {
        push(pending_audio_);
        pending_audio_.clear();
    }

    // Finishing a stream that received no audio is an error in audio.cpp.
    if (model_samples_ > 0) {
        audiocpp_result* raw_result = nullptr;
        check_status(audiocpp_stream_finish(session_.get(), &raw_result));
        const audiocpp::ResultPtr result{raw_result};

        // The final transcript is cumulative; what lies beyond the emitted length is new. After a
        // revision the emitted text no longer matches it byte for byte (a delta restarts at the
        // divergence), so the lengths only approximate: the tail may repeat or skip a few
        // characters around the revision, but is never lost.
        const auto  full  = audiocpp::read_text(result.get());
        std::size_t start = published_.size();
        while (start < full.text.size() &&
               (static_cast<unsigned char>(full.text[start]) & 0xC0) == 0x80) {
            ++start;  // not into the middle of a UTF-8 character
        }
        if (start < full.text.size()) {
            deliver_partial(full.text.substr(start), stream_samples_);  // all audio is heard
        }
    }

    if (closed_) {
        emit(*closed_);
    }
    else if (open_start_) {
        emit({*open_start_, stream_samples_});
    }
    else {
        emit({last_end_, stream_samples_});
    }
}

void ContinuousStreamTranscriber::push(std::span<const float> samples)
{
    audiocpp_event* raw_event = nullptr;
    check_status(audiocpp_stream_push(session_.get(), samples.data(), samples.size(),
                                      audiocpp::sample_rate, audiocpp::channels, model_samples_,
                                      &raw_event));
    model_samples_ += static_cast<std::int64_t>(samples.size());
    // Runs before the one holding the oldest position text can still cover are no longer needed.
    const std::int64_t oldest = model_samples_ - text_latency_samples_;
    while (fed_runs_.size() > 1 && fed_runs_[1].model_start <= oldest) {
        fed_runs_.erase(fed_runs_.begin());
    }

    for_each_event(session_.get(), raw_event,
                   [this](const audiocpp::EventPtr& event) { collect(event); });
}

// Partial text is incremental: the text decoded since the previous event.
void ContinuousStreamTranscriber::collect(const audiocpp::EventPtr& event)
{
    if (!event) {
        return;
    }
    auto partial = audiocpp::read_text(audiocpp_event_as_result(event.get()));
    published_ += partial.text;
    if (!partial.language.empty()) {
        language_ = std::move(partial.language);
    }
    deliver_partial(std::move(partial.text));
}

// Adds `text` to the current utterance and hands it out right away. Text arriving between
// utterances belongs to the next one, whose start is not known yet: it spans from the last end.
void ContinuousStreamTranscriber::deliver_partial(std::string text, std::optional<std::int64_t> heard_at)
{
    if (text.empty()) {
        return;
    }
    text_ += text;

    const std::int64_t start = open_start_ ? *open_start_ : closed_ ? closed_->start : last_end_;
    const std::int64_t heard = std::max(start, heard_at.value_or(heard_position()));
    callback_(user_data_, AudioTranscriber::TranscribedSegment{
                              .text      = std::move(text),
                              .language  = language_,
                              .start_sec = to_seconds(start),
                              .end_sec   = to_seconds(heard),
                              .is_final  = false,
                          });
}

std::int64_t ContinuousStreamTranscriber::heard_position() const
{
    // text_latency is one chunk plus the model's lookahead; the text covers the pushed audio up
    // to the lookahead.
    const std::int64_t lookahead =
        std::max<std::int64_t>(0, text_latency_samples_ - static_cast<std::int64_t>(chunk_samples_));
    const std::int64_t model = std::max<std::int64_t>(0, model_samples_ - lookahead);
    const auto run = std::ranges::find_if(fed_runs_ | std::views::reverse,
                                          [model](const FedRun& r) { return r.model_start <= model; });
    if (run == (fed_runs_ | std::views::reverse).end()) {
        return stream_samples_;
    }
    return run->stream_start + (model - run->model_start);
}

void ContinuousStreamTranscriber::emit(Range range)
{
    closed_.reset();
    if (text_.empty()) {
        return;
    }
    callback_(user_data_, AudioTranscriber::TranscribedSegment{
                              .text      = std::exchange(text_, {}),
                              .language  = language_,
                              .start_sec = to_seconds(range.start),
                              .end_sec   = to_seconds(range.end),
                          });
}

}  // namespace dth::audio
