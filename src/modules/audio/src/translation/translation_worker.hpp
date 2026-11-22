#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <semaphore>
#include <string>
#include <thread>
#include <variant>

#include "dth/audio/audio_translator.hpp"
#include "dth/audio/text_stream.hpp"
#include "dth/common/spsc_queue.hpp"
#include "translation/translation_engine.hpp"
#include "translation/translation_scheduler.hpp"

namespace dth::audio {

// The worker behind a TextStream: the producer pushes text into a bounded lock-free SPSC queue;
// the worker thread (started by the constructor) feeds it to a TranslationScheduler, translates
// the units it commits one at a time with a UnitTranslator, and passes deltas and finals to the
// callback. No mutex: the queue is lock-free and the worker sleeps on a semaphore (an atomic
// wait) that push() and stop() release.
//
// push() is single-producer (TextStream contract). stop() and the destructor must be ordered
// after the last push() (same thread, or a happens-before edge): text pushed concurrently with
// stop() may be dropped.
class TranslationWorker
{
  public:
    // Input the worker may fall behind by before push() fails.
    static constexpr std::size_t max_queued_inputs = 4096;

    struct Settings
    {
        AudioTranslator::Languages      languages;
        TranslationScheduler::Config    scheduler;
        AudioTranslator::StreamCallback callback  = nullptr;
        void*                           user_data = nullptr;
    };

    TranslationWorker(std::unique_ptr<UnitTranslator> translator, Settings settings);
    // Stops (see stop()) if still running.
    ~TranslationWorker();

    TranslationWorker(const TranslationWorker&)            = delete;
    TranslationWorker& operator=(const TranslationWorker&) = delete;
    TranslationWorker(TranslationWorker&&)                 = delete;
    TranslationWorker& operator=(TranslationWorker&&)      = delete;

    // TextStream::push / stop / is_running contracts.
    [[nodiscard]] StreamResult push(const TextStream::Text& text);
    [[nodiscard]] StreamResult push(const TextStream::Speakers& speakers);
    [[nodiscard]] StreamResult stop();
    [[nodiscard]] bool         is_running() const noexcept;

  private:
    using Clock = std::chrono::steady_clock;

    void run() noexcept;
    // Waits for input, stop, or the scheduler's deadline; moves the queued input into the
    // scheduler. Returns false once stopping with nothing left to read.
    bool                 receive();
    void                 translate(const TranslationJob& job);
    [[nodiscard]] double now() const;

    std::unique_ptr<UnitTranslator> translator_;
    Settings                        settings_;
    TranslationScheduler            scheduler_;  // worker thread only
    const Clock::time_point         epoch_ = Clock::now();

    using QueuedInput = std::variant<TranslationScheduler::Input, TranslationScheduler::SpeakerUpdate>;
    [[nodiscard]] StreamResult enqueue(QueuedInput input);

    common::SpscQueue<QueuedInput, max_queued_inputs> queue_;
    // Released after every push and by stop(); may run ahead of the queue (spurious wake-ups
    // are harmless), never behind it.
    std::counting_semaphore<> input_ready_{0};
    std::atomic<bool>         stopping_{false};

    std::string       error_;  // written by the worker, read after join
    std::atomic<bool> failed_{false};

    // Last: stops and joins before the state it uses is destroyed.
    std::jthread worker_;
};

}  // namespace dth::audio
