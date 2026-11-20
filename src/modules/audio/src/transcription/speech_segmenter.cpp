#include "transcription/speech_segmenter.hpp"

#include <algorithm>

namespace dth::audio {

SpeechSegmenter::SpeechSegmenter(SegmentSink& sink, PauseFinder* pause_finder, Config config)
    : sink_{sink}, pause_finder_{pause_finder}, config_{config}
{
}

void SpeechSegmenter::append(std::span<const float> samples)
{
    buffer_.insert(buffer_.end(), samples.begin(), samples.end());
    sink_.audio(samples);

    if (speech_start_ && end_sample() - *speech_start_ >= config_.max_segment_samples) {
        const std::int64_t cut = forced_cut();
        close_segment(cut);
        speech_start_ = cut;
        sink_.segment_begin(cut);
    }
    trim();
}

// The pause the finder picks in the second half of the open utterance, else the current sample.
std::int64_t SpeechSegmenter::forced_cut()
{
    const std::int64_t end = end_sample();
    if (pause_finder_ == nullptr) {
        return end;
    }

    const std::int64_t search_start = *speech_start_ + (end - *speech_start_) / 2;
    const auto         first        = static_cast<std::size_t>(search_start - buffer_start_);
    const auto         window       = std::span{buffer_}.subspan(first);

    const auto pause = pause_finder_->find_pause(window);
    if (!pause || *pause <= 0 || *pause >= static_cast<std::int64_t>(window.size())) {
        return end;
    }
    return search_start + *pause;
}

void SpeechSegmenter::speech_start(std::int64_t sample)
{
    if (speech_start_) {
        return;
    }
    // Not before the previous utterance's end: no audio goes to two utterances.
    speech_start_ = std::clamp(sample, std::max(buffer_start_, last_end_), end_sample());
    sink_.segment_begin(*speech_start_);
}

void SpeechSegmenter::speech_end(std::int64_t sample, std::int64_t min_length)
{
    if (!speech_start_) {
        return;
    }
    const std::int64_t end = std::clamp(sample, *speech_start_, end_sample());
    if (end - *speech_start_ < min_length) {
        return;
    }
    close_segment(end);
    speech_start_.reset();
    trim();
}

void SpeechSegmenter::finish()
{
    if (speech_start_) {
        close_segment(end_sample());
        speech_start_.reset();
    }
    trim();
}

void SpeechSegmenter::reset()
{
    buffer_.clear();
    buffer_start_ = 0;
    last_end_     = 0;
    speech_start_.reset();
}

std::int64_t SpeechSegmenter::end_sample() const noexcept
{
    return buffer_start_ + static_cast<std::int64_t>(buffer_.size());
}

void SpeechSegmenter::close_segment(std::int64_t end)
{
    const auto first = static_cast<std::size_t>(*speech_start_ - buffer_start_);
    const auto count = static_cast<std::size_t>(end - *speech_start_);
    sink_.segment_end(*speech_start_, end, std::span{buffer_}.subspan(first, count));
    last_end_ = end;
}

// Idle: keep `history_samples`. In speech: keep the utterance.
void SpeechSegmenter::trim()
{
    const std::int64_t keep_from = speech_start_
                                       ? *speech_start_
                                       : std::max(buffer_start_, end_sample() - config_.history_samples);
    const auto drop = static_cast<std::ptrdiff_t>(keep_from - buffer_start_);
    buffer_.erase(buffer_.begin(), buffer_.begin() + drop);
    buffer_start_ = keep_from;
}

}  // namespace dth::audio
