// SystemAudioSource tests (macOS Core Audio process tap). Need real audio hardware and the
// "System Audio Recording" permission (ctest label "device").
//
// The capture tests play tones: another process (afplay) plays 1 kHz, this process plays 700 Hz
// on the default output. Expected: 1 kHz captured, 700 Hz not (own process is excluded).
// Other apps playing loud audio around 700 Hz meanwhile can fail the exclusion check.

#include "system_audio/system_audio_source.hpp"

#include <gtest/gtest.h>

#include <spawn.h>
#include <sys/wait.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <numbers>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "dth/audio/audio_system.hpp"
#include "rt_alloc_guard.hpp"

extern char** environ;

using namespace dth::audio;
using namespace std::chrono_literals;
using dth::audio::test::rt_allocation_count;
using dth::audio::test::rt_guard_covers_malloc;
using dth::audio::test::RtScope;

namespace {

constexpr double other_tone_hz = 1000.0;  // played by afplay
constexpr double own_tone_hz   = 700.0;   // played by this process

// Marks the calling thread real-time for the rest of its life (RtScope never destroyed). Called
// from the first sink write, so everything on the device IO thread after it is counted: the
// source's IO callback and the HAL code around it, not only the sink.
void mark_thread_real_time() noexcept
{
    alignas(RtScope) static thread_local unsigned char storage[sizeof(RtScope)];
    static thread_local bool                           marked = false;
    if (!marked) {
        marked = true;
        ::new (static_cast<void*>(storage)) RtScope;
    }
}

// Records channel 0 into a preallocated buffer; counts frames and contract violations.
class CaptureSink final : public AudioSink
{
  public:
    explicit CaptureSink(double capacity_seconds) : _capacity_seconds{capacity_seconds} {}

    StreamResult prepare(const AudioFormat& input) override
    {
        _format = input;
        _samples.assign(static_cast<std::size_t>(_capacity_seconds * input.sample_rate), 0.0f);
        return {};
    }
    StreamResult start() override
    {
        _stopped.store(false);
        return {};
    }
    StreamResult stop() override
    {
        _stopped.store(true);
        return {};
    }

    CallbackResult write(AudioBufferView buffer) noexcept override
    {
        mark_thread_real_time();
        if (_stopped.load()) {
            _writes_after_stop.fetch_add(1);
        }
        if (buffer.channels != _format.channels || buffer.data == nullptr) {
            _bad_writes.fetch_add(1);
            return CallbackResult::continue_stream;
        }
        std::size_t const recorded = _recorded.load(std::memory_order_relaxed);
        std::size_t const n        = std::min<std::size_t>(buffer.frames, _samples.size() - recorded);
        for (std::size_t f = 0; f < n; ++f) {
            _samples[recorded + f] = buffer.data[f * buffer.channels];
        }
        _recorded.store(recorded + n, std::memory_order_release);
        _frames.fetch_add(buffer.frames, std::memory_order_relaxed);
        _plain_frames += buffer.frames;
        std::uint64_t const writes = _writes.fetch_add(1, std::memory_order_relaxed) + 1;
        std::uint64_t const end    = _end_after_writes.load(std::memory_order_relaxed);
        return end != 0 && writes >= end ? CallbackResult::stop_stream
                                         : CallbackResult::continue_stream;
    }

    // After the source stopped.
    [[nodiscard]]
    std::vector<float> recorded() const
    {
        return {_samples.begin(),
                _samples.begin() +
                    static_cast<std::ptrdiff_t>(_recorded.load(std::memory_order_acquire))};
    }

    std::atomic<std::uint64_t> _frames{0};
    // Not atomic: read only after stop(). TSan checks stop() hands the IO thread's writes over.
    std::uint64_t              _plain_frames{0};
    std::atomic<std::uint64_t> _writes{0};
    std::atomic<std::uint64_t> _bad_writes{0};
    std::atomic<std::uint64_t> _writes_after_stop{0};
    std::atomic<bool>          _stopped{true};
    // Non-zero: write() returns stop_stream once this many writes happened in total.
    std::atomic<std::uint64_t> _end_after_writes{0};

  private:
    double                   _capacity_seconds;
    AudioFormat              _format{};
    std::vector<float>       _samples;
    std::atomic<std::size_t> _recorded{0};
};

double rms(const std::vector<float>& x)
{
    double sum = 0.0;
    for (float v : x) {
        sum += static_cast<double>(v) * v;
    }
    return x.empty() ? 0.0 : std::sqrt(sum / static_cast<double>(x.size()));
}

// Amplitude of the `frequency` component of x (Goertzel, rectangular window).
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

// 16-bit mono WAV sine.
std::filesystem::path write_tone_wav(double frequency, double seconds, double amplitude)
{
    constexpr std::uint32_t rate   = 48000;
    auto const              frames = static_cast<std::uint32_t>(seconds * rate);
    auto const path = std::filesystem::temp_directory_path() / "dth_system_audio_tone.wav";

    std::ofstream out(path, std::ios::binary);
    auto put32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto put16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4);
    put32(36 + frames * 2);
    out.write("WAVEfmt ", 8);
    put32(16);
    put16(1);  // PCM
    put16(1);  // mono
    put32(rate);
    put32(rate * 2);
    put16(2);
    put16(16);
    out.write("data", 4);
    put32(frames * 2);
    for (std::uint32_t i = 0; i < frames; ++i) {
        double const v = amplitude * std::sin(2.0 * std::numbers::pi * frequency * i / rate);
        put16(static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(v * 32767.0))));
    }
    return path;
}

// Plays a file in another process.
class Afplay
{
  public:
    explicit Afplay(const std::filesystem::path& file)
    {
        std::string program = "/usr/bin/afplay";
        std::string arg     = file.string();
        char*       argv[]  = {program.data(), arg.data(), nullptr};
        if (posix_spawn(&_pid, program.c_str(), nullptr, nullptr, argv, environ) != 0) {
            _pid = -1;
        }
    }
    ~Afplay()
    {
        if (_pid > 0) {
            int status = 0;
            waitpid(_pid, &status, 0);
        }
    }
    [[nodiscard]]
    bool started() const
    {
        return _pid > 0;
    }

  private:
    pid_t _pid{-1};
};

// Sine on the default output device, from this process.
struct OwnTone
{
    double        sample_rate = 48000.0;
    std::uint32_t channels    = 2;
    std::uint64_t position    = 0;

    static CallbackResult callback(void* user_data, MutableAudioBufferView out) noexcept
    {
        auto& self = *static_cast<OwnTone*>(user_data);
        for (unsigned long f = 0; f < out.frames; ++f, ++self.position) {
            auto const v = static_cast<float>(
                0.1 * std::sin(2.0 * std::numbers::pi * own_tone_hz *
                               static_cast<double>(self.position) / self.sample_rate));
            for (std::uint32_t c = 0; c < out.channels; ++c) {
                out.data[f * out.channels + c] = v;
            }
        }
        return CallbackResult::continue_stream;
    }
};

// Waits up to 1 s until the sink saw `count` writes.
bool wait_for_writes(const CaptureSink& sink, std::uint64_t count)
{
    auto const deadline = std::chrono::steady_clock::now() + 1s;
    while (sink._writes.load() < count) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// Opens the source; fails the test with the Core Audio status when it cannot.
#define OPEN_OR_FAIL(source)                                                                    \
    do {                                                                                        \
        auto const opened = (source).open();                                                    \
        ASSERT_TRUE(opened.has_value())                                                         \
            << opened.error().message << " (native " << opened.error().native_code.value_or(0) \
            << ")";                                                                             \
    } while (false)

}  // namespace

TEST(SystemAudioSource, RejectsInvalidUse)
{
    SystemAudioSource source;
    CaptureSink       sink{1.0};
    EXPECT_FALSE(source.start(sink).has_value());
    EXPECT_TRUE(source.stop().has_value());
    EXPECT_TRUE(source.close().has_value());
    EXPECT_TRUE(source.close().has_value());
    EXPECT_FALSE(source.is_open());
}

TEST(SystemAudioSource, OpenReportsStereoFloatFormat)
{
    SystemAudioSource source;
    OPEN_OR_FAIL(source);
    EXPECT_TRUE(source.is_open());
    EXPECT_TRUE(source.open().has_value());  // no-op
    AudioFormat const format = source.format();
    std::cout << "  tap format: " << format.sample_rate << " Hz x " << format.channels << '\n';
    EXPECT_GE(format.sample_rate, 8000.0);
    EXPECT_EQ(format.channels, 2u);
    EXPECT_TRUE(source.close().has_value());
    EXPECT_FALSE(source.is_open());
    EXPECT_EQ(source.format().channels, 0u);
}

// Captures another process's audio, not this process's, at the reported rate, without
// allocating on the IO thread.
TEST(SystemAudioSource, CapturesOtherProcessesButNotOwn)
{
    AudioSystem system;
    auto const  output = system.default_output_device();
    ASSERT_TRUE(output.has_value());

    OwnTone            own{output->default_sample_rate, 2, 0};
    OutputStreamConfig own_config;
    own_config.common.sample_rate = own.sample_rate;
    own_config.channels           = own.channels;
    AudioStream own_stream = system.make_stream(*output, own_config, &OwnTone::callback, &own);
    ASSERT_TRUE(own_stream.open().has_value());
    ASSERT_TRUE(own_stream.start().has_value());

    SystemAudioSource source;
    OPEN_OR_FAIL(source);
    AudioFormat const format = source.format();

    constexpr double capture_seconds = 3.0;
    CaptureSink      sink{capture_seconds + 1.0};
    ASSERT_TRUE(sink.prepare(format).has_value());
    ASSERT_TRUE(sink.start().has_value());

    auto const tone = write_tone_wav(other_tone_hz, 2.0, 0.2);

    std::size_t const allocations_before = rt_allocation_count();
    auto const        started            = std::chrono::steady_clock::now();
    ASSERT_TRUE(source.start(sink).has_value());
    EXPECT_TRUE(source.is_running());
    std::uint64_t frames_begin = 0;
    std::uint64_t frames_end   = 0;
    double        rate_seconds = 0.0;
    std::size_t   allocations_after = 0;
    {
        std::this_thread::sleep_for(300ms);
        Afplay const player{tone};
        ASSERT_TRUE(player.started());
        auto const rate_begin = std::chrono::steady_clock::now();
        frames_begin          = sink._frames.load();
        std::this_thread::sleep_until(started + std::chrono::duration<double>(capture_seconds));
        frames_end   = sink._frames.load();
        rate_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - rate_begin).count();
        // Before stop(): the HAL allocates while tearing its IO thread down, which is not the
        // real-time path under test.
        allocations_after = rt_allocation_count();
        ASSERT_TRUE(source.stop().has_value());
    }
    ASSERT_TRUE(sink.stop().has_value());
    EXPECT_FALSE(source.is_running());
    (void)own_stream.stop();
    (void)own_stream.close();
    std::filesystem::remove(tone);

    std::vector<float> const captured = sink.recorded();
    double const             other    = tone_amplitude(captured, other_tone_hz, format.sample_rate);
    double const             mine     = tone_amplitude(captured, own_tone_hz, format.sample_rate);
    double const rate = static_cast<double>(frames_end - frames_begin) / rate_seconds;
    std::cout << "  writes " << sink._writes.load() << ", frames " << sink._frames.load()
              << " (" << rate << " /s over " << rate_seconds << " s), rms " << rms(captured)
              << ", 1 kHz amp " << other << ", 700 Hz amp " << mine
              << ", rt allocations " << (allocations_after - allocations_before)
              << (rt_guard_covers_malloc() ? " (incl. malloc)" : " (operator new only)") << '\n';

    EXPECT_GT(sink._writes.load(), 0u);
    EXPECT_EQ(sink._bad_writes.load(), 0u);
    EXPECT_EQ(sink._writes_after_stop.load(), 0u);
    EXPECT_EQ(sink._plain_frames, sink._frames.load());
    EXPECT_NEAR(rate, format.sample_rate, 0.02 * format.sample_rate);
    // afplay plays 0.2 for ~2 of the ~3 recorded seconds: expect roughly 0.13.
    EXPECT_GT(other, 0.03) << "other process not captured (permission denied?)";
    // Own tone plays at 0.1 the whole time; captured it would read ~0.1.
    EXPECT_LT(mine, 0.01) << "own process captured (not excluded)";
    EXPECT_EQ(allocations_after, allocations_before);
}

// Nothing else plays: the tap delivers (near) silence. Only meaningful when no other app plays
// audio, so opt-in: DTH_SYSTEM_AUDIO_EXPECT_SILENCE=1.
TEST(SystemAudioSource, SilentWhenNothingPlays)
{
    if (std::getenv("DTH_SYSTEM_AUDIO_EXPECT_SILENCE") == nullptr) {
        GTEST_SKIP() << "set DTH_SYSTEM_AUDIO_EXPECT_SILENCE=1 when no other app plays audio";
    }
    SystemAudioSource source;
    OPEN_OR_FAIL(source);
    CaptureSink sink{2.0};
    ASSERT_TRUE(sink.prepare(source.format()).has_value());
    ASSERT_TRUE(sink.start().has_value());
    ASSERT_TRUE(source.start(sink).has_value());
    std::this_thread::sleep_for(1s);
    ASSERT_TRUE(source.stop().has_value());
    std::vector<float> const captured = sink.recorded();
    std::cout << "  frames " << captured.size() << ", rms " << rms(captured) << '\n';
    EXPECT_GT(captured.size(), 0u);
    EXPECT_LT(rms(captured), 1e-4);
}

TEST(SystemAudioSource, StartStopCyclesAndDestroyWhileRunning)
{
    SystemAudioSource source;
    OPEN_OR_FAIL(source);
    CaptureSink sink{1.0};
    ASSERT_TRUE(sink.prepare(source.format()).has_value());

    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(sink.start().has_value());
        ASSERT_TRUE(source.start(sink).has_value());
        EXPECT_TRUE(source.start(sink).has_value());  // no-op while running
        EXPECT_TRUE(wait_for_writes(sink, sink._writes.load() + 1)) << "cycle " << i;
        ASSERT_TRUE(source.stop().has_value());
        ASSERT_TRUE(sink.stop().has_value());
        std::this_thread::sleep_for(5ms);  // a late callback would be counted now
    }
    EXPECT_GT(sink._writes.load(), 0u);
    EXPECT_EQ(sink._writes_after_stop.load(), 0u);

    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(source.close().has_value());
        OPEN_OR_FAIL(source);
    }

    {
        SystemAudioSource running;
        OPEN_OR_FAIL(running);
        ASSERT_TRUE(sink.start().has_value());
        ASSERT_TRUE(running.start(sink).has_value());
        EXPECT_TRUE(wait_for_writes(sink, sink._writes.load() + 1));
    }  // destroyed while running: stops before sink goes away
    ASSERT_TRUE(sink.stop().has_value());
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(sink._writes_after_stop.load(), 0u);
}

// A write() result other than continue_stream ends delivery on the IO thread (the device keeps
// running idle); start() restarts delivery.
TEST(SystemAudioSource, SinkResultEndsDeliveryAndStartResumes)
{
    SystemAudioSource source;
    OPEN_OR_FAIL(source);
    CaptureSink sink{1.0};
    ASSERT_TRUE(sink.prepare(source.format()).has_value());
    ASSERT_TRUE(sink.start().has_value());

    for (int round = 1; round <= 3; ++round) {
        std::uint64_t const end = sink._writes.load() + 3;
        sink._end_after_writes.store(end);
        ASSERT_TRUE(source.start(sink).has_value());
        EXPECT_TRUE(wait_for_writes(sink, end)) << "round " << round;
        std::this_thread::sleep_for(100ms);  // ~9 IO cycles at 512 frames / 48 kHz
        EXPECT_EQ(sink._writes.load(), end) << "write() after stop_stream, round " << round;
        EXPECT_TRUE(source.is_running());  // device idles until stop() / start()
    }
    ASSERT_TRUE(source.stop().has_value());
    ASSERT_TRUE(sink.stop().has_value());
}
