// DeviceInputSource through the public API: AudioSystem::make_stream(make_input_source(...),
// callback). Needs a real input device and microphone permission (ctest label "device").

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include "dth/audio/audio_system.hpp"

using namespace dth::audio;
using namespace std::chrono_literals;

namespace {

struct State
{
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> frames{0};
    std::atomic<std::uint32_t> channels{0};
    std::atomic<std::uint64_t> end_after_calls{0};  // 0 = never
};

CallbackResult record(void* user_data, AudioBufferView buffer) noexcept
{
    auto& s = *static_cast<State*>(user_data);
    s.channels.store(buffer.channels);
    s.frames.fetch_add(buffer.frames);
    std::uint64_t const calls = s.calls.fetch_add(1) + 1;
    std::uint64_t const end   = s.end_after_calls.load();
    return end != 0 && calls >= end ? CallbackResult::stop_stream : CallbackResult::continue_stream;
}

template <typename Predicate>
bool wait_until(Predicate predicate, std::chrono::milliseconds timeout = 3000ms)
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

}  // namespace

// Raw input at the requested rate / channels, then converted to mono 16 kHz.
TEST(DeviceInputSource, CapturesAtRequestedFormat)
{
    AudioSystem system;
    auto const  device = system.default_input_device();
    if (!device) {
        GTEST_SKIP() << "no default input device";
    }
    InputStreamConfig config;
    config.common.sample_rate = 48000.0;
    config.channels           = 1;

    for (bool const convert : {false, true}) {
        SCOPED_TRACE(convert);
        PipelineConfig pipeline;
        if (convert) {
            pipeline.processing = ProcessingFlags::resample;
            pipeline.target     = {16000.0, 0};
        }
        State       state;
        AudioStream stream =
            system.make_stream(system.make_input_source(*device, config), &record, &state, pipeline);
        ASSERT_OK(stream.open());
        ASSERT_OK(stream.start());
        ASSERT_TRUE(wait_until([&] { return state.frames.load() > 0; }));
        auto const f0 = state.frames.load();
        auto const t0 = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(1s);
        double const rate =
            static_cast<double>(state.frames.load() - f0) /
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        ASSERT_OK(stream.stop());
        EXPECT_EQ(state.channels.load(), 1u);
        EXPECT_NEAR(rate, convert ? 16000.0 : 48000.0, convert ? 800.0 : 2400.0);
        ASSERT_OK(stream.close());
    }
}

// stop_stream from the callback ends the backend input stream; start() resumes.
TEST(DeviceInputSource, CallbackResultEndsStream)
{
    AudioSystem system;
    auto const  device = system.default_input_device();
    if (!device) {
        GTEST_SKIP() << "no default input device";
    }
    State       state;
    AudioStream stream = system.make_stream(system.make_input_source(*device), &record, &state);
    ASSERT_OK(stream.open());
    for (int round = 1; round <= 3; ++round) {
        std::uint64_t const end = state.calls.load() + 5;
        state.end_after_calls.store(end);
        ASSERT_OK(stream.start());
        ASSERT_TRUE(wait_until([&] { return stream.is_stopped(); })) << round;
        std::this_thread::sleep_for(100ms);
        EXPECT_EQ(state.calls.load(), end) << "callback after stop_stream, round " << round;
    }
    ASSERT_OK(stream.close());
}
