#include "translation/translation_worker.hpp"

#include <exception>
#include <optional>
#include <variant>
#include <vector>
#include <utility>

namespace dth::audio {

namespace {

AudioError stream_error(std::string message)
{
    return AudioError{.code = AudioErrorCode::stream_stop_failed, .message = std::move(message)};
}

}  // namespace

TranslationWorker::TranslationWorker(std::unique_ptr<UnitTranslator> translator, Settings settings)
    : translator_{std::move(translator)},
      settings_{std::move(settings)},
      scheduler_{settings_.scheduler,
                 [this](std::string_view text) { return translator_->count_tokens(text); }},
      worker_{[this] { run(); }}
{
}

TranslationWorker::~TranslationWorker() { (void)stop(); }

StreamResult TranslationWorker::push(const TextStream::Text& text)
{
    return enqueue(TranslationScheduler::Input{.text      = std::string{text.text},
                                               .start_sec = text.start_sec,
                                               .end_sec   = text.end_sec,
                                               .is_final  = text.is_final});
}

StreamResult TranslationWorker::push(const TextStream::Speakers& speakers)
{
    TranslationScheduler::SpeakerUpdate update{.complete_until_sec = speakers.complete_until_sec};
    update.turns.reserve(speakers.turns.size());
    for (const TextStream::SpeakerTurn& turn : speakers.turns) {
        update.turns.push_back({.speaker   = std::string{turn.speaker},
                                .start_sec = turn.start_sec,
                                .end_sec   = turn.end_sec});
    }
    return enqueue(std::move(update));
}

StreamResult TranslationWorker::enqueue(QueuedInput input)
{
    if (failed_.load(std::memory_order_acquire)) {
        return std::unexpected(stream_error("Translation failed: " + error_));
    }
    if (stopping_.load(std::memory_order_acquire)) {
        return std::unexpected(stream_error("Translation stream is stopped"));
    }
    if (!queue_.Push(std::move(input))) {
        return std::unexpected(stream_error("Translation fell behind: input queue full"));
    }
    input_ready_.release();  // after Push: a wake-up never precedes its input
    return {};
}

StreamResult TranslationWorker::stop()
{
    if (worker_.joinable()) {
        // Release store: the worker that sees stopping_ also sees every earlier push.
        stopping_.store(true, std::memory_order_release);
        input_ready_.release();  // wakes the wait
        worker_.join();
    }
    // join() orders the worker's error_ write before this read.
    if (failed_.load(std::memory_order_relaxed)) {
        return std::unexpected(stream_error(error_));
    }
    return {};
}

bool TranslationWorker::is_running() const noexcept
{
    return worker_.joinable() && !failed_.load(std::memory_order_acquire);
}

void TranslationWorker::run() noexcept
{
    try {
        while (receive()) {
            scheduler_.tick(now());
            // One job per round: input that arrived meanwhile is taken before the next one.
            if (const auto job = scheduler_.next_job()) {
                translate(*job);
            }
        }
        scheduler_.finish();
        while (const auto job = scheduler_.next_job()) {
            translate(*job);
        }
    } catch (const std::exception& e) {
        error_ = e.what();
        failed_.store(true, std::memory_order_release);
    } catch (...) {
        error_ = "Translation stream worker failed";
        failed_.store(true, std::memory_order_release);
    }
}

bool TranslationWorker::receive()
{
    // Work is ready without waiting: a job, or text whose hold time is over.
    const std::optional<double> deadline = scheduler_.next_deadline();
    const bool job_ready = scheduler_.has_job() || (deadline && *deadline <= now());
    // stopping_ is never waited on after it is set: stop() releases the semaphore only once.
    if (!job_ready && !stopping_.load(std::memory_order_acquire)) {
        if (deadline) {
            const auto wake = epoch_ + std::chrono::duration_cast<Clock::duration>(
                                           std::chrono::duration<double>(*deadline));
            (void)input_ready_.try_acquire_until(wake);
        }
        else {
            input_ready_.acquire();
        }
    }
    // Drop the releases of input drained below. Done before draining, so a push landing in
    // between leaves at most a spurious wake-up, never a missed one.
    while (input_ready_.try_acquire()) {
    }

    // Read before draining: once stopping_ is seen, every push is visible to the drain.
    const bool   stopping = stopping_.load(std::memory_order_acquire);
    const double time     = now();
    bool         received = false;
    QueuedInput  input;
    // Consecutive text is pushed as one batch: the scheduler runs the sentence model once for
    // it (deltas pile up during a translation).
    std::vector<TranslationScheduler::Input> text;
    while (queue_.Pop(input)) {
        received = true;
        if (auto* item = std::get_if<TranslationScheduler::Input>(&input)) {
            text.push_back(std::move(*item));
            continue;
        }
        scheduler_.push(text, time);
        text.clear();
        scheduler_.push_speakers(std::get<TranslationScheduler::SpeakerUpdate>(input));
    }
    scheduler_.push(text, time);
    return received || job_ready || !stopping;
}

void TranslationWorker::translate(const TranslationJob& job)
{
    AudioTranslator::TranslatedSegment segment{.unit_id     = job.unit_id,
                                               .source_text = job.text,
                                               .start_sec   = job.start_sec,
                                               .end_sec     = job.end_sec,
                                               .is_final    = false,
                                               .speaker     = job.speaker};
    std::string                        translation =
        translator_->translate(job, settings_.languages, [&](std::string_view text) {
            segment.text = text;
            settings_.callback(settings_.user_data, segment);
        });

    segment.text     = translation;
    segment.is_final = true;
    if (segment.speaker.empty()) {  // the unit's turn may have ended meanwhile
        segment.speaker = scheduler_.speaker_of(job.heard_from_sec, job.heard_to_sec);
    }
    settings_.callback(settings_.user_data, segment);
    scheduler_.complete(job.unit_id, std::move(translation));
}

double TranslationWorker::now() const
{
    return std::chrono::duration<double>(Clock::now() - epoch_).count();
}

}  // namespace dth::audio
