// System audio stream tests: SystemAudioSource -> DeviceOutputSink / callback, mostly through the
// public AudioSystem::make_system_audio_stream API. Need real audio hardware and the "System Audio
// Recording" permission (ctest label "device").
//
// The pipe test plays 1 kHz from another process (afplay) and expects it on the output device, then
// expects the output to fall silent once afplay ends (no feedback loop: this process is excluded
// from the tap). Other apps playing meanwhile can fail the silence check. The test is audible.

#include "pipeline/device_output_sink.hpp"
#include "pipeline/pipeline_stream.hpp"
#include "system_audio/system_audio_source.hpp"

#include <gtest/gtest.h>

#include <spawn.h>
#include <sys/wait.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "dth/audio/audio_stream.hpp"
#include "dth/audio/audio_system.hpp"
#include "rt_alloc_guard.hpp"

extern char** environ;

using namespace dth::audio;
using namespace std::chrono_literals;
using dth::audio::test::rt_allocation_count;
using dth::audio::test::RtScope;

namespace {

constexpr double tone_hz = 1000.0;  // played by afplay

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
    auto const path = std::filesystem::temp_directory_path() / "dth_system_audio_stream_tone.wav";

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

// Plays a file in another process; the destructor waits for it to finish.
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
    ~Afplay() { wait(); }

    void wait()
    {
        if (_pid > 0) {
            int status = 0;
            waitpid(_pid, &status, 0);
            _pid = -1;
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

// Wraps the output device callback: runs it inside an RtScope and records channel 0 of what it
// rendered into a preallocated buffer.
struct RecordingCallback
{
    OutputCallback             callback  = nullptr;
    void*                      user_data = nullptr;
    std::vector<float>         samples;  // sized before the stream starts
    std::atomic<std::size_t>   recorded{0};
    std::atomic<std::uint64_t> calls{0};

    static CallbackResult trampoline(void* self_ptr, MutableAudioBufferView output) noexcept
    {
        auto&         self = *static_cast<RecordingCallback*>(self_ptr);
        RtScope const rt;
        CallbackResult const result = self.callback(self.user_data, output);

        std::size_t const done = self.recorded.load(std::memory_order_relaxed);
        std::size_t const n    = std::min<std::size_t>(output.frames, self.samples.size() - done);
        for (std::size_t f = 0; f < n; ++f) {
            self.samples[done + f] = output.data[f * output.channels];
        }
        self.recorded.store(done + n, std::memory_order_release);
        self.calls.fetch_add(1, std::memory_order_relaxed);
        return result;
    }

    // Frames recorded so far, from any thread.
    [[nodiscard]]
    std::size_t frames() const
    {
        return recorded.load(std::memory_order_acquire);
    }

    // [from, to) of the recording. After the stream stopped.
    [[nodiscard]]
    std::vector<float> slice(std::size_t from, std::size_t to) const
    {
        to   = std::min(to, frames());
        from = std::min(from, to);
        return {samples.begin() + static_cast<std::ptrdiff_t>(from),
                samples.begin() + static_cast<std::ptrdiff_t>(to)};
    }
};

// 1 kHz amplitude per 100 ms window. Short windows: the drift-controlled resample ratio wobbles
// the output frequency by ~0.1 Hz, which smears a single long Goertzel window.
std::vector<double> windowed_amplitudes(const std::vector<float>& x, double sample_rate)
{
    auto const          window = static_cast<std::size_t>(0.1 * sample_rate);
    std::vector<double> result;
    for (std::size_t i = 0; i + window <= x.size(); i += window) {
        result.push_back(tone_amplitude({x.begin() + static_cast<std::ptrdiff_t>(i),
                                         x.begin() + static_cast<std::ptrdiff_t>(i + window)},
                                        tone_hz, sample_rate));
    }
    return result;
}

int env_int(const char* name, int fallback)
{
    char const* value = std::getenv(name);
    return value != nullptr ? std::atoi(value) : fallback;
}

#define ASSERT_OK(expr)                                                                          \
    do {                                                                                         \
        auto const result_ = (expr);                                                             \
        ASSERT_TRUE(result_.has_value())                                                         \
            << #expr << ": " << result_.error().message << " (native "                          \
            << result_.error().native_code.value_or(0) << ")";                                   \
    } while (false)

class SystemAudioStreamTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        _device = _system.default_output_device();
        if (!_device) {
            GTEST_SKIP() << "no default output device";
        }
    }

    AudioSystem                _system;
    std::optional<AudioDevice> _device;
};

}  // namespace

TEST_F(SystemAudioStreamTest, LifecycleTransitions)
{
    AudioStream stream = _system.make_system_audio_stream(_system.make_output_sink(*_device));
    EXPECT_TRUE(stream.is_closed());
    EXPECT_FALSE(stream.start().has_value());  // not open
    EXPECT_TRUE(stream.stop().has_value());    // no-op
    EXPECT_TRUE(stream.is_closed());

    ASSERT_OK(stream.open());
    EXPECT_TRUE(stream.is_open());
    ASSERT_OK(stream.open());  // no-op
    EXPECT_TRUE(stream.is_open());

    ASSERT_OK(stream.start());
    EXPECT_TRUE(stream.is_running());
    ASSERT_OK(stream.start());  // no-op
    ASSERT_OK(stream.stop());
    EXPECT_TRUE(stream.is_stopped());
    ASSERT_OK(stream.stop());  // no-op
    ASSERT_OK(stream.start());
    EXPECT_TRUE(stream.is_running());

    ASSERT_OK(stream.close());
    EXPECT_TRUE(stream.is_closed());
    ASSERT_OK(stream.close());  // no-op
    EXPECT_FALSE(stream.start().has_value());

    // Reopen after close.
    ASSERT_OK(stream.open());
    ASSERT_OK(stream.start());
    ASSERT_OK(stream.close());
}

// Plan step 8: open/start/stop/close cycled 100x (DTH_SYSTEM_AUDIO_CYCLES overrides).
TEST_F(SystemAudioStreamTest, OpenStartStopCloseCycles)
{
    int const   cycles = env_int("DTH_SYSTEM_AUDIO_CYCLES", 100);
    AudioStream stream = _system.make_system_audio_stream(_system.make_output_sink(*_device));
    for (int i = 0; i < cycles; ++i) {
        ASSERT_OK(stream.open());
        ASSERT_OK(stream.start());
        std::this_thread::sleep_for(20ms);
        ASSERT_OK(stream.stop());
        ASSERT_OK(stream.close());
    }
}

TEST_F(SystemAudioStreamTest, DestroyWhileRunning)
{
    for (int i = 0; i < 5; ++i) {
        AudioStream stream = _system.make_system_audio_stream(_system.make_output_sink(*_device));
        ASSERT_OK(stream.open());
        ASSERT_OK(stream.start());
        std::this_thread::sleep_for(100ms);
    }  // destroyed running
}

TEST_F(SystemAudioStreamTest, MovedStreamKeepsRunning)
{
    AudioStream stream = _system.make_system_audio_stream(_system.make_output_sink(*_device));
    ASSERT_OK(stream.open());
    ASSERT_OK(stream.start());
    AudioStream moved = std::move(stream);
    std::this_thread::sleep_for(100ms);
    EXPECT_TRUE(moved.is_running());
    ASSERT_OK(moved.stop());
    ASSERT_OK(moved.close());
}

// The stream keeps the backend alive: it can be opened after its AudioSystem is gone.
TEST(SystemAudioStream, OutlivesAudioSystem)
{
    std::optional<AudioStream> stream;
    {
        AudioSystem system;
        auto const  device = system.default_output_device();
        if (!device) {
            GTEST_SKIP() << "no default output device";
        }
        stream.emplace(system.make_system_audio_stream(system.make_output_sink(*device)));
    }
    ASSERT_OK(stream->open());
    ASSERT_OK(stream->start());
    std::this_thread::sleep_for(100ms);
    ASSERT_OK(stream->close());
}

// Internal: PipelineStream(SystemAudioSource -> DeviceOutputSink) with an output factory that
// records what the device plays.
TEST_F(SystemAudioStreamTest, PipesOtherProcessesToOutputWithoutFeedback)
{
    RecordingCallback recording;
    double            output_rate = 0.0;
    auto factory = [&](const AudioDevice& device, const OutputStreamConfig& config,
                       OutputCallback callback, void* user_data) {
        recording.callback  = callback;
        recording.user_data = user_data;
        output_rate         = config.common.sample_rate;
        recording.samples.assign(static_cast<std::size_t>(8.0 * config.common.sample_rate), 0.0f);
        return _system.make_stream(device, config, &RecordingCallback::trampoline, &recording);
    };

    auto const wav  = write_tone_wav(tone_hz, 2.0, 0.2);
    auto       sink = std::make_unique<DeviceOutputSink>(*_device, factory);
    DeviceOutputSink const& output = *sink;
    PipelineStream stream{std::make_unique<SystemAudioSource>(), std::move(sink)};
    ASSERT_OK(stream.open());

    std::size_t const allocations = rt_allocation_count();
    ASSERT_OK(stream.start());
    std::this_thread::sleep_for(300ms);  // pre-roll settles

    std::size_t tone_begin = 0;
    std::size_t tone_end   = 0;
    {
        tone_begin = recording.frames();
        Afplay player{wav};
        ASSERT_TRUE(player.started());
        player.wait();
        tone_end = recording.frames();
    }
    std::this_thread::sleep_for(1500ms);  // tail: must decay to silence
    std::size_t const rt_after = rt_allocation_count();
    ASSERT_OK(stream.stop());

    PipelineStats const stats = output.stats();
    auto const          rate  = output_rate;
    auto const          tail_from = tone_end + static_cast<std::size_t>(0.5 * rate);
    auto const          tone = recording.slice(tone_begin, tone_end);
    auto const          tail = recording.slice(tail_from, recording.frames());

    // Tone window spans afplay's lifetime (start-up + 2 s tone + exit): median is inside the tone.
    auto tone_windows = windowed_amplitudes(tone, rate);
    auto tail_windows = windowed_amplitudes(tail, rate);
    ASSERT_FALSE(tone_windows.empty());
    ASSERT_FALSE(tail_windows.empty());
    std::ranges::sort(tone_windows);
    double const tone_amp = tone_windows[tone_windows.size() / 2];
    double const tail_amp = std::ranges::max(tail_windows);
    std::cout << "output " << rate << " Hz, callbacks " << recording.calls.load() << ", tone window "
              << static_cast<double>(tone.size()) / rate << " s amp " << tone_amp << ", tail "
              << static_cast<double>(tail.size()) / rate << " s amp " << tail_amp << " rms "
              << rms(tail) << ", underruns " << stats.underruns << ", overruns "
              << stats.overruns << ", ratio " << stats.resample_ratio << '\n';

    // afplay plays 0.2 (unity path: tap is pre-volume, converter level +-0.5 dB).
    EXPECT_GT(tone_amp, 0.17);
    EXPECT_LT(tone_amp, 0.23);
    EXPECT_GT(tail.size(), static_cast<std::size_t>(0.8 * rate));
    EXPECT_LT(tail_amp, 1e-3) << "1 kHz still playing after afplay ended: feedback loop?";
    EXPECT_EQ(stats.underruns, 0U);
    EXPECT_EQ(stats.overruns, 0U);
    EXPECT_EQ(rt_after, allocations) << "allocations on the output device thread";

    ASSERT_OK(stream.close());
    std::filesystem::remove(wav);
}

namespace {

// Callback consumer: records channel 0 into a preallocated buffer, remembers the delivered format
// and ends the stream after `end_after_calls` calls (0 = never).
struct CallbackRecorder
{
    std::vector<float>         samples;
    std::atomic<std::size_t>   recorded{0};
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> frames{0};
    std::atomic<std::uint32_t> channels{0};
    std::atomic<std::uint64_t> end_after_calls{0};

    static CallbackResult callback(void* self_ptr, AudioBufferView buffer) noexcept
    {
        auto& self = *static_cast<CallbackRecorder*>(self_ptr);
        self.channels.store(buffer.channels, std::memory_order_relaxed);
        std::size_t const done = self.recorded.load(std::memory_order_relaxed);
        std::size_t const n    = std::min<std::size_t>(buffer.frames, self.samples.size() - done);
        for (std::size_t f = 0; f < n; ++f) {
            self.samples[done + f] = buffer.data[f * buffer.channels];
        }
        self.recorded.store(done + n, std::memory_order_release);
        self.frames.fetch_add(buffer.frames, std::memory_order_relaxed);
        std::uint64_t const calls = self.calls.fetch_add(1, std::memory_order_relaxed) + 1;
        std::uint64_t const end   = self.end_after_calls.load(std::memory_order_relaxed);
        return end != 0 && calls >= end ? CallbackResult::stop_stream
                                        : CallbackResult::continue_stream;
    }
};

// Plays the 1 kHz tone through afplay into a callback stream; checks format, rate and level.
void expect_callback_capture(AudioSystem& system, const PipelineConfig& config,
                             std::uint32_t expected_channels, double expected_rate)
{
    CallbackRecorder recorder;
    recorder.samples.assign(static_cast<std::size_t>(10.0 * expected_rate), 0.0f);
    AudioStream stream =
        system.make_system_audio_stream(&CallbackRecorder::callback, &recorder, config);
    ASSERT_OK(stream.open());
    ASSERT_OK(stream.start());

    auto const wav = write_tone_wav(tone_hz, 2.0, 0.2);
    auto const t0  = std::chrono::steady_clock::now();
    auto const f0  = recorder.frames.load();
    std::size_t tone_begin = recorder.recorded.load();
    {
        Afplay player{wav};
        ASSERT_TRUE(player.started());
        player.wait();
    }
    std::size_t const tone_end = recorder.recorded.load();
    double const      seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double const rate = static_cast<double>(recorder.frames.load() - f0) / seconds;
    ASSERT_OK(stream.stop());

    std::vector<float> tone(recorder.samples.begin() + static_cast<std::ptrdiff_t>(tone_begin),
                            recorder.samples.begin() + static_cast<std::ptrdiff_t>(tone_end));
    auto windows = windowed_amplitudes(tone, expected_rate);
    ASSERT_FALSE(windows.empty());
    std::ranges::sort(windows);
    double const amp = windows[windows.size() / 2];
    std::cout << "callback " << recorder.channels.load() << " ch, " << rate << " frames/s, "
              << recorder.calls.load() << " calls, 1 kHz amp " << amp << '\n';

    EXPECT_EQ(recorder.channels.load(), expected_channels);
    EXPECT_NEAR(rate, expected_rate, 0.02 * expected_rate);
    EXPECT_GT(amp, 0.17);
    EXPECT_LT(amp, 0.23);
    ASSERT_OK(stream.close());
    std::filesystem::remove(wav);
}

}  // namespace

// Raw: the callback gets the tap format (48 kHz stereo here) unconverted.
TEST(SystemAudioStreamCallback, RawCapture)
{
    AudioSystem system;
    expect_callback_capture(system, {}, 2, 48000.0);
}

// Resampled + remapped on the IO thread: 44.1 kHz mono.
TEST(SystemAudioStreamCallback, ConvertedCapture)
{
    AudioSystem    system;
    PipelineConfig config;
    config.processing = ProcessingFlags::resample | ProcessingFlags::remap_channels;
    config.target     = {44100.0, 1};
    expect_callback_capture(system, config, 1, 44100.0);
}

// stop_stream from the callback: no further calls, the stream reports stopped, start() resumes.
TEST(SystemAudioStreamCallback, CallbackResultEndsStream)
{
    AudioSystem      system;
    CallbackRecorder recorder;
    recorder.samples.assign(48000, 0.0f);
    AudioStream stream = system.make_system_audio_stream(&CallbackRecorder::callback, &recorder);
    ASSERT_OK(stream.open());

    for (int round = 1; round <= 3; ++round) {
        std::uint64_t const end = recorder.calls.load() + 5;
        recorder.end_after_calls.store(end);
        ASSERT_OK(stream.start());
        auto const deadline = std::chrono::steady_clock::now() + 2s;
        while (!stream.is_stopped() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        EXPECT_TRUE(stream.is_stopped()) << "round " << round;
        EXPECT_FALSE(stream.is_running());
        std::this_thread::sleep_for(100ms);
        EXPECT_EQ(recorder.calls.load(), end) << "callback after stop_stream, round " << round;
    }
    ASSERT_OK(stream.stop());
    EXPECT_TRUE(stream.is_stopped());
    ASSERT_OK(stream.close());
}
