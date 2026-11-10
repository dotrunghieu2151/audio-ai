// Contract test for the pipeline interfaces with fake stages:
// prepare -> start -> writes -> stop ordering, no write after AudioSource::stop() returns, and no
// allocation on the real-time path.

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include "dth/audio/audio_format.hpp"
#include "dth/audio/audio_sink.hpp"
#include "dth/audio/audio_source.hpp"
#include "rt_alloc_guard.hpp"

using namespace dth::audio;
using dth::audio::test::rt_allocation_count;
using dth::audio::test::RtScope;

namespace {

constexpr std::uint32_t kChannels = 2;
constexpr unsigned long kFrames   = 64;

// Records lifecycle violations; write() only touches atomics.
class FakeSink final : public AudioSink
{
  public:
    StreamResult prepare(const AudioFormat& input) override
    {
        _format = input;
        _prepared.store(true);
        return {};
    }

    StreamResult start() override
    {
        if (!_prepared.load()) {
            return std::unexpected(AudioError{AudioErrorCode::stream_start_failed, "not prepared", std::nullopt});
        }
        _running.store(true);
        return {};
    }

    StreamResult stop() override
    {
        _running.store(false);
        return {};
    }

    // Each write takes a while so a source whose stop() does not wait for the in-flight write
    // is caught by the counters changing after stop() returned.
    CallbackResult write(AudioBufferView buffer) noexcept override
    {
        auto const until = std::chrono::steady_clock::now() + std::chrono::microseconds(200);
        while (std::chrono::steady_clock::now() < until) {
        }
        if (!_running.load(std::memory_order_acquire) || buffer.channels != _format.channels ||
            buffer.data == nullptr) {
            _violations.fetch_add(1, std::memory_order_relaxed);
        }
        _writes.fetch_add(1, std::memory_order_relaxed);
        _frames.fetch_add(buffer.frames, std::memory_order_relaxed);
        return CallbackResult::continue_stream;
    }

    [[nodiscard]] std::uint64_t writes() const noexcept { return _writes.load(); }
    [[nodiscard]] std::uint64_t frames() const noexcept { return _frames.load(); }
    [[nodiscard]] std::uint64_t violations() const noexcept { return _violations.load(); }

  private:
    AudioFormat                _format{};
    std::atomic<bool>          _prepared{false};
    std::atomic<bool>          _running{false};
    std::atomic<std::uint64_t> _writes{0};
    std::atomic<std::uint64_t> _frames{0};
    std::atomic<std::uint64_t> _violations{0};
};

// Pushes fixed-size buffers from its own thread (marked real-time) until stopped.
class FakeSource final : public AudioSource
{
  public:
    StreamResult open() override { return {}; }
    StreamResult close() override { return stop(); }

    [[nodiscard]] AudioFormat format() const noexcept override { return {48000.0, kChannels}; }

    StreamResult start(AudioSink& sink) override
    {
        _stop.store(false);
        _thread = std::thread{[this, &sink] {
            RtScope rt;
            while (!_stop.load(std::memory_order_acquire)) {
                sink.write({_buffer.data(), kFrames, kChannels});
            }
        }};
        return {};
    }

    StreamResult stop() override
    {
        _stop.store(true, std::memory_order_release);
        if (_thread.joinable()) {
            _thread.join();
        }
        return {};
    }

  private:
    std::array<float, kFrames * kChannels> _buffer{};
    std::atomic<bool>                      _stop{false};
    std::thread                            _thread;
};

}  // namespace

TEST(PipelineContract, SinkRejectsStartBeforePrepare)
{
    FakeSink sink;
    EXPECT_FALSE(sink.start().has_value());
}

TEST(PipelineContract, LifecycleOrderingAndNoWriteAfterStop)
{
    FakeSource source;
    FakeSink   sink;

    for (int cycle = 0; cycle < 3; ++cycle) {
        auto const allocations_before = rt_allocation_count();
        auto const writes_before      = sink.writes();

        ASSERT_TRUE(sink.prepare(source.format()).has_value());
        ASSERT_TRUE(sink.start().has_value());
        ASSERT_TRUE(source.start(sink).has_value());

        while (sink.writes() < writes_before + 100) {
            std::this_thread::yield();
        }

        ASSERT_TRUE(source.stop().has_value());
        auto const writes_at_stop = sink.writes();
        ASSERT_TRUE(sink.stop().has_value());

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        EXPECT_EQ(sink.writes(), writes_at_stop) << "write() after AudioSource::stop() returned";
        EXPECT_EQ(sink.violations(), 0u);
        EXPECT_EQ(sink.frames(), sink.writes() * kFrames);
        EXPECT_EQ(rt_allocation_count(), allocations_before);
    }
}
