// PipelineStream tests with fake stages (no audio device): lifecycle, ownership, processing
// flags, end of stream from the sink / callback, and no allocation on the real-time path.

#include "pipeline/pipeline_stream.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>
#include <thread>
#include <vector>

#include "dth/audio/audio_stream.hpp"
#include "dth/audio/audio_system.hpp"
#include "rt_alloc_guard.hpp"

using namespace dth::audio;
using namespace std::chrono_literals;
using dth::audio::test::rt_allocation_count;
using dth::audio::test::RtScope;

namespace {

// Signal pushed by FakeSource: every channel carries the same sine, or a ramp (bit-exact checks).
struct Signal
{
    double frequency = 1000.0;  // 0 = ramp
    double amplitude = 0.5;

    [[nodiscard]] float at(std::uint64_t frame, std::uint32_t channel, double rate) const noexcept
    {
        if (frequency == 0.0) {
            return static_cast<float>(frame % 100000) * 1e-5f + static_cast<float>(channel);
        }
        return static_cast<float>(
            amplitude *
            std::sin(2.0 * std::numbers::pi * frequency * static_cast<double>(frame) / rate));
    }
};

struct Lifetime
{
    std::atomic<int> sources{0};
    std::atomic<int> sinks{0};
};

// Pushes `total_frames` in `block_frames` blocks from its own RtScope thread, then idles. Honors
// the AudioSource contract: stops writing after a non-continue result.
class FakeSource final : public AudioSource
{
  public:
    FakeSource(AudioFormat format, Signal signal, std::size_t block_frames,
               std::uint64_t total_frames, Lifetime* lifetime = nullptr)
        : _format{format},
          _signal{signal},
          _block_frames{block_frames},
          _total_frames{total_frames},
          _lifetime{lifetime},
          _block(block_frames * format.channels)
    {
        if (_lifetime != nullptr) {
            _lifetime->sources.fetch_add(1);
        }
    }

    ~FakeSource() override
    {
        (void)close();
        if (_lifetime != nullptr) {
            _lifetime->sources.fetch_sub(1);
        }
    }

    StreamResult open() override
    {
        _open = true;
        return {};
    }
    StreamResult close() override
    {
        _open = false;
        return stop();
    }

    [[nodiscard]] AudioFormat format() const noexcept override { return _format; }

    StreamResult start(AudioSink& sink) override
    {
        if (!_open) {
            return std::unexpected(AudioError{AudioErrorCode::stream_start_failed, "not open", {}});
        }
        _stop.store(false);
        _thread = std::thread{[this, &sink] {
            RtScope const rt;
            bool          ended = false;
            while (!_stop.load(std::memory_order_acquire)) {
                if (ended || _produced >= _total_frames) {
                    std::this_thread::yield();
                    continue;
                }
                auto const n = static_cast<std::size_t>(
                    std::min<std::uint64_t>(_block_frames, _total_frames - _produced));
                for (std::size_t f = 0; f < n; ++f) {
                    for (std::uint32_t c = 0; c < _format.channels; ++c) {
                        _block[f * _format.channels + c] =
                            _signal.at(_produced + f, c, _format.sample_rate);
                    }
                }
                _produced += n;
                _writes.fetch_add(1, std::memory_order_relaxed);
                ended = sink.write({_block.data(), n, _format.channels}) !=
                        CallbackResult::continue_stream;
                _pushed.store(_produced, std::memory_order_release);
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

    // Frames pushed so far (any thread).
    [[nodiscard]] std::uint64_t pushed() const noexcept { return _pushed.load(); }
    [[nodiscard]] std::uint64_t writes() const noexcept { return _writes.load(); }

  private:
    AudioFormat                _format;
    Signal                     _signal;
    std::size_t                _block_frames;
    std::uint64_t              _total_frames;
    Lifetime*                  _lifetime;
    std::vector<float>         _block;
    bool                       _open{false};
    std::uint64_t              _produced{0};  // source thread, handed over by join()
    std::atomic<std::uint64_t> _pushed{0};
    std::atomic<std::uint64_t> _writes{0};
    std::atomic<bool>          _stop{false};
    std::thread                _thread;
};

// Records every sample it receives into a preallocated buffer; checks lifecycle ordering.
class RecordingSink final : public AudioSink
{
  public:
    explicit RecordingSink(std::size_t capacity_samples, Lifetime* lifetime = nullptr)
        : _lifetime{lifetime}, _samples(capacity_samples)
    {
        if (_lifetime != nullptr) {
            _lifetime->sinks.fetch_add(1);
        }
    }
    ~RecordingSink() override
    {
        if (_lifetime != nullptr) {
            _lifetime->sinks.fetch_sub(1);
        }
    }

    StreamResult prepare(const AudioFormat& input) override
    {
        _format = input;
        _prepares.fetch_add(1);
        return {};
    }
    StreamResult start() override
    {
        _starts.fetch_add(1);
        _running.store(true);
        return {};
    }
    StreamResult stop() override
    {
        _running.store(false);
        return {};
    }
    StreamResult close() override
    {
        _closes.fetch_add(1);
        return {};
    }

    CallbackResult write(AudioBufferView buffer) noexcept override
    {
        if (!_running.load(std::memory_order_acquire) || buffer.channels != _format.channels) {
            _violations.fetch_add(1, std::memory_order_relaxed);
        }
        std::size_t const done = _recorded.load(std::memory_order_relaxed);
        std::size_t const n    = std::min(buffer.samples().size(), _samples.size() - done);
        std::copy_n(buffer.data, n, _samples.begin() + static_cast<std::ptrdiff_t>(done));
        _recorded.store(done + n, std::memory_order_release);
        std::uint64_t const writes = _writes.fetch_add(1, std::memory_order_relaxed) + 1;
        std::uint64_t const end    = end_after_writes.load(std::memory_order_relaxed);
        return end != 0 && writes >= end ? end_result : CallbackResult::continue_stream;
    }

    [[nodiscard]] AudioFormat format() const noexcept { return _format; }
    [[nodiscard]] std::uint64_t writes() const noexcept { return _writes.load(); }
    [[nodiscard]] std::uint64_t violations() const noexcept { return _violations.load(); }
    [[nodiscard]] int starts() const noexcept { return _starts.load(); }
    [[nodiscard]] int prepares() const noexcept { return _prepares.load(); }
    [[nodiscard]] int closes() const noexcept { return _closes.load(); }
    // Frames recorded so far (any thread).
    [[nodiscard]] std::size_t frames() const noexcept
    {
        return _format.channels == 0 ? 0 : _recorded.load(std::memory_order_acquire) / _format.channels;
    }
    // Channel `c` of the recording. After the stream stopped.
    [[nodiscard]] std::vector<float> channel(std::uint32_t c) const
    {
        std::vector<float> out(frames());
        for (std::size_t f = 0; f < out.size(); ++f) {
            out[f] = _samples[f * _format.channels + c];
        }
        return out;
    }
    [[nodiscard]] const std::vector<float>& samples() const noexcept { return _samples; }

    std::atomic<std::uint64_t> end_after_writes{0};  // 0 = never end
    CallbackResult             end_result{CallbackResult::stop_stream};

  private:
    Lifetime*                  _lifetime;
    AudioFormat                _format{};
    std::vector<float>         _samples;
    std::atomic<std::size_t>   _recorded{0};
    std::atomic<std::uint64_t> _writes{0};
    std::atomic<std::uint64_t> _violations{0};
    std::atomic<bool>          _running{false};
    std::atomic<int>           _starts{0};
    std::atomic<int>           _prepares{0};
    std::atomic<int>           _closes{0};
};

double tone_amplitude(const std::vector<float>& x, double frequency, double sample_rate)
{
    double const w     = 2.0 * std::numbers::pi * frequency / sample_rate;
    double const coeff = 2.0 * std::cos(w);
    double       s1 = 0.0, s2 = 0.0;
    for (float v : x) {
        double const s0 = v + coeff * s1 - s2;
        s2              = s1;
        s1              = s0;
    }
    double const power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return x.empty() ? 0.0 : 2.0 * std::sqrt(std::max(power, 0.0)) / static_cast<double>(x.size());
}

template <typename Predicate>
bool wait_until(Predicate predicate, std::chrono::milliseconds timeout = 2000ms)
{
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

#define ASSERT_OK(expr)                                                                          \
    do {                                                                                         \
        auto const result_ = (expr);                                                             \
        ASSERT_TRUE(result_.has_value()) << #expr << ": " << result_.error().message;            \
    } while (false)

struct Pipeline
{
    FakeSource*    source;
    RecordingSink* sink;
    PipelineStream stream;
};

Pipeline make_pipeline(AudioFormat format, Signal signal, std::size_t block,
                       std::uint64_t total_frames, PipelineConfig config,
                       std::size_t sink_capacity_samples, Lifetime* lifetime = nullptr)
{
    auto source = std::make_unique<FakeSource>(format, signal, block, total_frames, lifetime);
    auto sink   = std::make_unique<RecordingSink>(sink_capacity_samples, lifetime);
    FakeSource*    s = source.get();
    RecordingSink* k = sink.get();
    return {s, k, PipelineStream{std::move(source), std::move(sink), config}};
}

PipelineConfig converting(ProcessingFlags flags, AudioFormat target)
{
    PipelineConfig config;
    config.processing = flags;
    config.target     = target;
    return config;
}

}  // namespace

TEST(PipelineStream, RejectsMissingStages)
{
    PipelineStream no_source{nullptr, std::make_unique<RecordingSink>(16)};
    EXPECT_FALSE(no_source.open().has_value());
    PipelineStream no_sink{std::make_unique<FakeSource>(AudioFormat{48000.0, 2}, Signal{}, 64, 0),
                           nullptr};
    EXPECT_FALSE(no_sink.open().has_value());
    EXPECT_TRUE(no_sink.close().has_value());
}

TEST(PipelineStream, InvalidTargetFailsOpen)
{
    auto p = make_pipeline({48000.0, 2}, {}, 64, 0,
                           converting(ProcessingFlags::resample, {0.0, 0}), 16);
    EXPECT_FALSE(p.stream.open().has_value());
    EXPECT_TRUE(p.stream.is_closed());
}

TEST(PipelineStream, LifecycleTransitions)
{
    auto p = make_pipeline({48000.0, 2}, {}, 64, 1'000'000'000, {}, 1024);
    EXPECT_TRUE(p.stream.is_closed());
    EXPECT_FALSE(p.stream.start().has_value());
    ASSERT_OK(p.stream.open());
    EXPECT_TRUE(p.stream.is_open());
    EXPECT_EQ(p.sink->prepares(), 1);
    ASSERT_OK(p.stream.open());  // no-op
    EXPECT_EQ(p.sink->prepares(), 1);
    ASSERT_OK(p.stream.start());
    EXPECT_TRUE(p.stream.is_running());
    EXPECT_TRUE(wait_until([&] { return p.sink->writes() > 10; }));
    ASSERT_OK(p.stream.stop());
    EXPECT_TRUE(p.stream.is_stopped());
    auto const writes = p.sink->writes();
    std::this_thread::sleep_for(10ms);
    EXPECT_EQ(p.sink->writes(), writes) << "write() after stop()";
    ASSERT_OK(p.stream.start());
    EXPECT_EQ(p.sink->starts(), 2);
    ASSERT_OK(p.stream.close());
    EXPECT_TRUE(p.stream.is_closed());
    EXPECT_EQ(p.sink->closes(), 1);
    EXPECT_EQ(p.sink->violations(), 0u);
}

TEST(PipelineStream, OwnsSourceAndSink)
{
    Lifetime lifetime;
    {
        auto p = make_pipeline({48000.0, 2}, {}, 64, 1'000'000'000, {}, 1024, &lifetime);
        EXPECT_EQ(lifetime.sources.load(), 1);
        EXPECT_EQ(lifetime.sinks.load(), 1);
        ASSERT_OK(p.stream.open());
        ASSERT_OK(p.stream.start());
        PipelineStream moved = std::move(p.stream);
        EXPECT_TRUE(moved.is_running());
        EXPECT_TRUE(p.stream.is_closed());
        EXPECT_EQ(lifetime.sources.load(), 1);
    }  // destroyed running
    EXPECT_EQ(lifetime.sources.load(), 0);
    EXPECT_EQ(lifetime.sinks.load(), 0);
}

TEST(PipelineStream, RawPassthroughIsBitExact)
{
    AudioFormat const   format{48000.0, 3};
    std::uint64_t const total = 10'000;
    auto p = make_pipeline(format, Signal{0.0}, 37, total, {}, total * format.channels);
    ASSERT_OK(p.stream.open());
    EXPECT_EQ(p.sink->format(), format);
    ASSERT_OK(p.stream.start());
    ASSERT_TRUE(wait_until([&] { return p.sink->frames() == total; }));
    ASSERT_OK(p.stream.stop());

    for (std::uint64_t f = 0; f < total; ++f) {
        for (std::uint32_t c = 0; c < format.channels; ++c) {
            ASSERT_EQ(p.sink->samples()[f * format.channels + c],
                      Signal{0.0}.at(f, c, format.sample_rate))
                << f << "/" << c;
        }
    }
}

// Each flag alone and both: delivered format, frame count, 1 kHz kept at level, no aliasing.
TEST(PipelineStream, ResampleAndRemapFlags)
{
    struct Case
    {
        ProcessingFlags flags;
        AudioFormat     expected;
    };
    AudioFormat const input{48000.0, 2};
    AudioFormat const target{44100.0, 1};
    for (Case const c : {Case{ProcessingFlags::resample, {44100.0, 2}},
                         Case{ProcessingFlags::remap_channels, {48000.0, 1}},
                         Case{ProcessingFlags::resample | ProcessingFlags::remap_channels,
                              {44100.0, 1}}}) {
        SCOPED_TRACE(static_cast<int>(c.flags));
        std::uint64_t const total = 48000;  // 1 s
        auto p = make_pipeline(input, {1000.0, 0.5}, 512, total, converting(c.flags, target),
                               2 * total * input.channels);
        ASSERT_OK(p.stream.open());
        EXPECT_EQ(p.sink->format(), c.expected);
        ASSERT_OK(p.stream.start());
        ASSERT_TRUE(wait_until([&] { return p.source->pushed() == total; }));
        std::this_thread::sleep_for(20ms);
        ASSERT_OK(p.stream.stop());

        auto const expected_frames = static_cast<double>(total) * c.expected.sample_rate / 48000.0;
        EXPECT_NEAR(static_cast<double>(p.sink->frames()), expected_frames, 200.0);

        // Skip the resampler's start-up transient.
        auto       ch0 = p.sink->channel(0);
        auto const skip = static_cast<std::ptrdiff_t>(0.05 * c.expected.sample_rate);
        std::vector<float> steady(ch0.begin() + skip, ch0.end() - skip);
        double const amp   = tone_amplitude(steady, 1000.0, c.expected.sample_rate);
        double const wrong = tone_amplitude(steady, 1000.0 * 48000.0 / 44100.0,
                                            c.expected.sample_rate);
        EXPECT_NEAR(20.0 * std::log10(amp / 0.5), 0.0, 0.5);
        EXPECT_LT(wrong, 0.01);
        if (c.expected.channels == 2) {
            EXPECT_EQ(p.sink->channel(1), ch0);
        }
        EXPECT_EQ(p.sink->violations(), 0u);
    }
}

TEST(PipelineStream, NoRealTimeAllocations)
{
    AudioFormat const target{44100.0, 1};
    for (ProcessingFlags const flags :
         {ProcessingFlags::none, ProcessingFlags::resample, ProcessingFlags::remap_channels,
          ProcessingFlags::resample | ProcessingFlags::remap_channels}) {
        for (std::size_t const block : {1u, 37u, 512u, 4096u}) {
            SCOPED_TRACE(testing::Message() << "flags " << static_cast<int>(flags) << " block "
                                            << block);
            std::uint64_t const total = 20'000;
            auto p = make_pipeline({48000.0, 2}, {1000.0, 0.5}, block, total,
                                   converting(flags, target), 2 * total * 2);
            ASSERT_OK(p.stream.open());
            auto const before = rt_allocation_count();
            ASSERT_OK(p.stream.start());
            ASSERT_TRUE(wait_until([&] { return p.source->pushed() == total; }));
            ASSERT_OK(p.stream.stop());
            EXPECT_EQ(rt_allocation_count(), before);
        }
    }
}

// stop_stream / complete_stream from the sink: no further writes reach it, the stream reports
// stopped; start() restarts; stop() then works as usual.
TEST(PipelineStream, SinkResultEndsStream)
{
    for (CallbackResult const result : {CallbackResult::stop_stream, CallbackResult::complete_stream}) {
        SCOPED_TRACE(static_cast<int>(result));
        // Resample on: the end must also cut a converter mid-write.
        auto p = make_pipeline({48000.0, 2}, {}, 4096, 1'000'000'000,
                               converting(ProcessingFlags::resample, {44100.0, 2}), 1'000'000);
        p.sink->end_result = result;
        ASSERT_OK(p.stream.open());
        for (int round = 1; round <= 3; ++round) {
            std::uint64_t const end = p.sink->writes() + 5;
            p.sink->end_after_writes.store(end);
            ASSERT_OK(p.stream.start());
            ASSERT_TRUE(wait_until([&] { return p.stream.is_stopped(); })) << round;
            EXPECT_FALSE(p.stream.is_running());
            std::this_thread::sleep_for(10ms);
            EXPECT_EQ(p.sink->writes(), end) << "write after end, round " << round;
        }
        EXPECT_EQ(p.sink->starts(), 3);
        ASSERT_OK(p.stream.stop());
        EXPECT_TRUE(p.stream.is_stopped());
        ASSERT_OK(p.stream.close());
        EXPECT_EQ(p.sink->violations(), 0u);
    }
}

// Public API: AudioSystem::make_stream(source, callback) with conversion; the callback ends it.
TEST(PipelineStream, PublicCallbackStream)
{
    struct State
    {
        std::atomic<std::uint64_t> calls{0};
        std::atomic<std::uint64_t> frames{0};
        std::atomic<std::uint32_t> channels{0};
    } state;

    AudioSystem system;
    auto source = std::make_unique<FakeSource>(AudioFormat{48000.0, 2}, Signal{}, 480, 1'000'000'000);
    AudioStream stream = system.make_stream(
        std::move(source),
        [](void* user_data, AudioBufferView buffer) noexcept {
            auto& s = *static_cast<State*>(user_data);
            s.channels.store(buffer.channels);
            s.frames.fetch_add(buffer.frames);
            return s.calls.fetch_add(1) + 1 >= 50 ? CallbackResult::stop_stream
                                                  : CallbackResult::continue_stream;
        },
        &state, converting(ProcessingFlags::remap_channels, {0.0, 1}));
    ASSERT_OK(stream.open());
    ASSERT_OK(stream.start());
    ASSERT_TRUE(wait_until([&] { return stream.is_stopped(); }));
    std::this_thread::sleep_for(10ms);
    EXPECT_EQ(state.calls.load(), 50u);
    EXPECT_EQ(state.frames.load(), 50u * 480u);
    EXPECT_EQ(state.channels.load(), 1u);
    ASSERT_OK(stream.close());
}

// Public API: make_stream(source, sink) owns both.
TEST(PipelineStream, PublicSinkStreamOwnsStages)
{
    Lifetime lifetime;
    {
        AudioSystem system;
        AudioStream stream = system.make_stream(
            std::make_unique<FakeSource>(AudioFormat{48000.0, 2}, Signal{}, 64, 1'000'000'000,
                                         &lifetime),
            std::make_unique<RecordingSink>(1024, &lifetime));
        ASSERT_OK(stream.open());
        ASSERT_OK(stream.start());
        std::this_thread::sleep_for(10ms);
        EXPECT_TRUE(stream.is_running());
    }
    EXPECT_EQ(lifetime.sources.load(), 0);
    EXPECT_EQ(lifetime.sinks.load(), 0);
}
