// DeviceOutputSink tests. Need a real output device (ctest label "device"; skipped when there is
// none). Most tests open the device stream but never start it and drive render() directly, so
// they are deterministic and silent. DeviceOutputSinkDevice.* play a sine on the default output.
//
// DTH_DEVICE_TEST_SECONDS overrides the 60 s playback duration.

#include "pipeline/device_output_sink.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <optional>
#include <thread>
#include <vector>

#include "dth/audio/audio_system.hpp"

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#endif
#include "rt_alloc_guard.hpp"

using namespace dth::audio;
using dth::audio::test::rt_allocation_count;
using dth::audio::test::rt_guard_covers_malloc;
using dth::audio::test::RtScope;

namespace {

// Wraps the sink's device callback so every invocation runs inside an RtScope.
struct GuardedCallback
{
    OutputCallback             callback  = nullptr;
    void*                      user_data = nullptr;
    std::atomic<std::uint64_t> calls{0};

    static CallbackResult trampoline(void* self_ptr, MutableAudioBufferView output) noexcept
    {
        auto&         self = *static_cast<GuardedCallback*>(self_ptr);
        RtScope const rt;
        self.calls.fetch_add(1, std::memory_order_relaxed);
        return self.callback(self.user_data, output);
    }
};

// Endless sine, every channel `amplitude * sin(2 pi f t)`.
struct Sine
{
    double        sample_rate = 48000.0;
    std::uint32_t channels    = 2;
    double        frequency   = 440.0;
    double        amplitude   = 0.1;
    std::uint64_t position    = 0;

    void fill(std::vector<float>& block, std::size_t frames)
    {
        block.resize(frames * channels);
        for (std::size_t f = 0; f < frames; ++f, ++position) {
            auto const v = static_cast<float>(
                amplitude * std::sin(2.0 * std::numbers::pi * frequency *
                                     static_cast<double>(position) / sample_rate));
            std::fill_n(block.begin() + static_cast<std::ptrdiff_t>(f * channels), channels, v);
        }
    }
};

AudioBufferView view(const std::vector<float>& samples, std::uint32_t channels)
{
    return {samples.data(), samples.size() / channels, channels};
}

double env_seconds(const char* name, double fallback)
{
    char const* value = std::getenv(name);
    return value != nullptr ? std::atof(value) : fallback;
}

class DeviceOutputSinkTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        _device = _system.default_output_device();
        if (!_device) {
            GTEST_SKIP() << "no default output device";
        }
    }

    DeviceOutputSink::OutputStreamFactory factory()
    {
        return [this](const AudioDevice& device, const OutputStreamConfig& config,
                      OutputCallback callback, void* user_data) {
            _guard.callback  = callback;
            _guard.user_data = user_data;
            return _system.make_stream(device, config, &GuardedCallback::trampoline, &_guard);
        };
    }

    // render() a device-sized block; returns it.
    std::vector<float> render(DeviceOutputSink& sink, std::size_t frames)
    {
        AudioFormat const  out = sink.output_format();
        std::vector<float> block(frames * out.channels, -100.0f);
        sink.render({block.data(), frames, out.channels});
        return block;
    }

    AudioSystem                _system;
    std::optional<AudioDevice> _device;
    GuardedCallback            _guard;
};

bool all_zero(const std::vector<float>& v)
{
    return std::ranges::all_of(v, [](float s) { return s == 0.0f; });
}

}  // namespace

// --- Driven render(), stream opened but not started ------------------------------------------

TEST_F(DeviceOutputSinkTest, RejectsInvalidUse)
{
    DeviceOutputSink sink{*_device, factory()};
    EXPECT_FALSE(sink.start().has_value());
    EXPECT_FALSE(sink.prepare({0.0, 2}).has_value());
    EXPECT_FALSE(sink.prepare({48000.0, 0}).has_value());
    EXPECT_TRUE(sink.stop().has_value());
    EXPECT_TRUE(sink.close().has_value());
    EXPECT_TRUE(sink.close().has_value());
}

TEST_F(DeviceOutputSinkTest, OutputFormatIsDeviceNative)
{
    DeviceOutputSink sink{*_device, factory()};
    ASSERT_TRUE(sink.prepare({44100.0, 1}).has_value());
    EXPECT_EQ(sink.output_format().sample_rate, _device->default_sample_rate);
    EXPECT_EQ(sink.output_format().channels, _device->max_output_channels);
}

TEST_F(DeviceOutputSinkTest, DriftWithoutConvertIsRejected)
{
    DeviceOutputSink sink{*_device, factory(), {OutputSinkFlags::drift_compensation}};
    auto const       r = sink.prepare({48000.0, 2});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, AudioErrorCode::unsupported_configuration);
}

// No convert_format: device opened at the input format, ring copied straight to the output
// (bit-exact after pre-roll).
TEST_F(DeviceOutputSinkTest, WithoutConvertPassesInputFormatThrough)
{
    DeviceOutputSink sink{*_device, factory(), {OutputSinkFlags::none}};
    AudioFormat const input{44100.0, 1};
    ASSERT_TRUE(sink.prepare(input).has_value());
    EXPECT_EQ(sink.output_format(), input);

    // Small writes: the target only follows the largest write.
    std::size_t const  block  = 128;
    std::size_t const  frames = (sink.target_fill() + 512 + block - 1) / block * block;
    std::vector<float> in(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        in[i] = static_cast<float>(i) * 1e-4f;
    }
    for (std::size_t i = 0; i < frames; i += block) {
        sink.write({in.data() + i, block, 1});
    }
    auto const out = render(sink, 512);
    for (std::size_t i = 0; i < 512; ++i) {
        ASSERT_EQ(out[i], in[i]) << i;
    }
    EXPECT_EQ(sink.stats().resample_ratio, 1.0);
}

// convert_format without drift_compensation: ratio stays exactly 1 whatever the fill level.
TEST_F(DeviceOutputSinkTest, WithoutDriftRatioStaysUnity)
{
    DeviceOutputSink sink{*_device, factory(), {OutputSinkFlags::convert_format}};
    ASSERT_TRUE(sink.prepare({48000.0, 2}).has_value());
    Sine               sine;
    std::vector<float> in;
    // Overfill far above target: drift control would speed up to the clamp.
    sine.fill(in, 4 * sink.target_fill());
    sink.write(view(in, 2));
    for (int i = 0; i < 5; ++i) {
        (void)render(sink, 480);
        EXPECT_EQ(sink.stats().resample_ratio, 1.0);
    }
    EXPECT_EQ(sink.stats().underruns, 0u);
}

TEST_F(DeviceOutputSinkTest, PrerollsUntilTargetThenPlays)
{
    DeviceOutputSink sink{*_device, factory()};
    ASSERT_TRUE(sink.prepare({48000.0, 2}).has_value());
    std::size_t const block = 256;
    Sine              sine;
    std::vector<float> in;

    std::size_t written = 0;
    while (written + block < sink.target_fill()) {
        sine.fill(in, block);
        sink.write(view(in, 2));
        written += block;
        EXPECT_TRUE(all_zero(render(sink, 1)));
    }
    EXPECT_TRUE(all_zero(render(sink, 64))) << "plays before reaching the target";

    sine.fill(in, block);
    sink.write(view(in, 2));
    EXPECT_FALSE(all_zero(render(sink, 512)));
    EXPECT_EQ(sink.stats().underruns, 0u);
    EXPECT_EQ(sink.stats().overruns, 0u);
}

TEST_F(DeviceOutputSinkTest, UnderrunCountsOnceAndRearmsPreroll)
{
    DeviceOutputSink sink{*_device, factory()};
    ASSERT_TRUE(sink.prepare({48000.0, 2}).has_value());
    // Blocks below the 10 ms margin, so the target stays put.
    Sine               sine;
    std::vector<float> in;
    sine.fill(in, 256);
    for (std::size_t written = 0; written < sink.target_fill(); written += 256) {
        sink.write(view(in, 2));
    }

    // Drain past the end: exactly one underrun, then silence without further counting.
    for (int i = 0; i < 20; ++i) {
        render(sink, 480);
    }
    EXPECT_EQ(sink.stats().underruns, 1u);

    // Less than the target again: still pre-rolling, silent.
    sink.write(view(in, 2));
    EXPECT_TRUE(all_zero(render(sink, 480)));
    EXPECT_EQ(sink.stats().underruns, 1u);
}

TEST_F(DeviceOutputSinkTest, OverrunKeepsWholeFrames)
{
    // 3 channels, distinct DC per channel: a split frame would swap channel values.
    DeviceOutputSink sink{*_device, factory()};
    ASSERT_TRUE(sink.prepare({48000.0, 3}).has_value());
    std::uint32_t const out_channels = sink.output_format().channels;
    ASSERT_GE(out_channels, 2u);

    std::vector<float> in;
    for (std::size_t f = 0; f < 1001; ++f) {  // odd size: free space rarely a multiple of it
        in.insert(in.end(), {0.5f, -0.25f, 0.125f});
    }
    for (int i = 0; i < 100; ++i) {
        sink.write(view(in, 3));
    }
    EXPECT_GT(sink.stats().overruns, 0u);

    auto const expect_channels = [&](const std::vector<float>& out, std::size_t from) {
        for (std::size_t f = from; f < out.size() / out_channels; ++f) {
            ASSERT_NEAR(out[f * out_channels + 0], 0.5f, 0.02f) << "frame " << f;
            ASSERT_NEAR(out[f * out_channels + 1], -0.25f, 0.02f) << "frame " << f;
        }
    };
    expect_channels(render(sink, 4096), 1024);  // past the resampler start-up

    // Keep writing while the ring drains: frames written after a split one would be shifted.
    for (int i = 0; i < 1000 && sink.stats().underruns == 0; ++i) {
        sink.write(view(in, 3));
        auto const out = render(sink, 4096);
        if (sink.stats().underruns == 0) {
            expect_channels(out, 0);
        }
    }
    EXPECT_GT(sink.stats().underruns, 0u);
}

TEST_F(DeviceOutputSinkTest, LargeWritesRaiseTarget)
{
    DeviceOutputSink sink{*_device, factory()};
    ASSERT_TRUE(sink.prepare({48000.0, 2}).has_value());
    std::size_t const initial = sink.target_fill();

    Sine               sine;
    std::vector<float> in;
    sine.fill(in, 3000);
    sink.write(view(in, 2));
    render(sink, 1);  // applies the largest write seen
    EXPECT_GT(sink.target_fill(), initial);
    EXPECT_EQ(sink.target_fill() - initial, 3000u - 480u);  // replaces the 10 ms margin
}

TEST_F(DeviceOutputSinkTest, RealTimePathsDoNotAllocate)
{
    for (AudioFormat const input : {AudioFormat{48000.0, 2}, AudioFormat{44100.0, 1},
                                    AudioFormat{96000.0, 2}, AudioFormat{48000.0, 6}}) {
        DeviceOutputSink sink{*_device, factory()};
        ASSERT_TRUE(sink.prepare(input).has_value());
        AudioFormat const out = sink.output_format();
        Sine              sine{.sample_rate = input.sample_rate, .channels = input.channels};
        std::vector<float> in;
        sine.fill(in, 777);
        std::vector<float> block(1000 * out.channels);

        std::size_t const before = rt_allocation_count();
        {
            RtScope const rt;
            for (int i = 0; i < 2000; ++i) {
                sink.write(view(in, input.channels));
                for (std::size_t const frames : {1u, 37u, 480u, 1000u}) {
                    sink.render({block.data(), frames, out.channels});
                }
            }
        }
        EXPECT_EQ(rt_allocation_count(), before)
            << input.sample_rate << " Hz x " << input.channels;
        EXPECT_GT(sink.stats().overruns + sink.stats().underruns, 0u);  // both paths exercised
    }
    std::cout << "  guard covers malloc: " << rt_guard_covers_malloc() << '\n';
}

// --- Real device playback ----------------------------------------------------------------------

namespace {

// Real-time (time-constraint) scheduling for the calling thread, like a Core Audio IOProc thread.
// Without it the producer's sleep wake-ups run up to ~16 ms late, eating the ring's headroom.
void make_thread_real_time(double period_seconds)
{
#if defined(__APPLE__)
    mach_timebase_info_data_t timebase{};
    mach_timebase_info(&timebase);
    auto const to_abs = [&](double seconds) {
        return static_cast<std::uint32_t>(seconds * 1e9 * timebase.denom / timebase.numer);
    };
    thread_time_constraint_policy_data_t policy{
        .period      = to_abs(period_seconds),
        .computation = to_abs(period_seconds * 0.1),
        .constraint  = to_abs(period_seconds * 0.5),
        .preemptible = 1,
    };
    kern_return_t const kr = thread_policy_set(
        mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY,
        reinterpret_cast<thread_policy_t>(&policy), THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (kr != KERN_SUCCESS) {
        std::cout << "  thread_policy_set failed: " << kr << '\n';
    }
#else
    (void)period_seconds;
#endif
}

// Paced producer on its own (steady_clock, real-time priority) thread: block-wise sine writes
// inside an RtScope.
class SineProducer
{
  public:
    SineProducer(DeviceOutputSink& sink, AudioFormat format, std::size_t block)
        : _sink{sink}, _sine{.sample_rate = format.sample_rate, .channels = format.channels},
          _block{block}
    {
        _sine.fill(_buffer, _block);
        _thread = std::thread{[this] { run(); }};
    }

    ~SineProducer()
    {
        _stop.store(true);
        _thread.join();
    }

    // Worst wake-up lateness of the (non real-time) producer thread.
    [[nodiscard]]
    std::int64_t max_late_us() const noexcept
    {
        return _max_late_us.load();
    }

  private:
    void run()
    {
        auto const period = std::chrono::duration<double>(static_cast<double>(_block) /
                                                          _sine.sample_rate);
        make_thread_real_time(period.count());
        auto       next   = std::chrono::steady_clock::now();
        while (!_stop.load()) {
            {
                RtScope const rt;
                _sink.write(view(_buffer, _sine.channels));
            }
            _sine.fill(_buffer, _block);
            next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
            std::this_thread::sleep_until(next);
            auto const late = std::chrono::steady_clock::now() - next;
            _max_late_us.store(std::max<std::int64_t>(
                _max_late_us.load(),
                std::chrono::duration_cast<std::chrono::microseconds>(late).count()));
        }
    }

    DeviceOutputSink&  _sink;
    Sine               _sine;
    std::size_t        _block;
    std::vector<float> _buffer;
    std::atomic<bool>         _stop{false};
    std::atomic<std::int64_t> _max_late_us{0};
    std::thread        _thread;
};

}  // namespace

TEST_F(DeviceOutputSinkTest, DevicePlaysSineWithoutUnderruns)
{
    double const seconds = env_seconds("DTH_DEVICE_TEST_SECONDS", 60.0);

    DeviceOutputSink sink{*_device, factory()};
    ASSERT_TRUE(sink.prepare({48000.0, 2}).has_value());
    std::cout << "  device '" << _device->name << "' " << sink.output_format().sample_rate
              << " Hz x " << sink.output_format().channels << ", playing 440 Hz for " << seconds
              << " s\n";

    std::size_t const before = rt_allocation_count();
    std::int64_t      late_us = 0;
    ASSERT_TRUE(sink.start().has_value());
    {
        SineProducer const producer{sink, {48000.0, 2}, 512};
        std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
        ASSERT_TRUE(sink.stop().has_value());  // after this, no more render()
        late_us = producer.max_late_us();
    }
    PipelineStats const stats = sink.stats();
    std::cout << "  callbacks " << _guard.calls.load() << " underruns " << stats.underruns
              << " overruns " << stats.overruns << " ratio " << stats.resample_ratio
              << " target " << sink.target_fill() << " producer max late " << late_us
              << " us\n";

    EXPECT_GT(_guard.calls.load(), 0u);
    EXPECT_EQ(stats.underruns, 0u);
    EXPECT_EQ(stats.overruns, 0u);
    EXPECT_EQ(rt_allocation_count(), before);
    EXPECT_TRUE(sink.close().has_value());
}

TEST_F(DeviceOutputSinkTest, DeviceStartStopCycles)
{
    DeviceOutputSink sink{*_device, factory()};
    AudioFormat      format{44100.0, 1};
    ASSERT_TRUE(sink.prepare(format).has_value());
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(sink.start().has_value());
        {
            SineProducer const producer{sink, format, 441};
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            ASSERT_TRUE(sink.stop().has_value());
        }
        if (i == 4) {
            format = {48000.0, 2};
            ASSERT_TRUE(sink.prepare(format).has_value());  // re-prepare while stopped
        }
    }
    // Destroyed while running.
    ASSERT_TRUE(sink.start().has_value());
}
