#include "pipeline/format_converter.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <numbers>
#include <random>
#include <span>
#include <vector>

#include "rt_alloc_guard.hpp"

using namespace dth::audio;
using dth::audio::test::rt_allocation_count;
using dth::audio::test::rt_guard_covers_malloc;
using dth::audio::test::RtScope;

namespace {

constexpr std::size_t kMaxChunk = 512;
constexpr std::size_t kSettle   = 2048;  // output frames skipped before measuring

// Endless test signal: per channel `gain[c] * amplitude * sin(2 pi f t) + dc[c]`.
struct TestSignal
{
    double                  sample_rate = 48000.0;
    std::uint32_t           channels    = 2;
    double                  frequency   = 1000.0;
    double                  amplitude   = 0.5;
    std::array<float, 8>    gain{1, 1, 1, 1, 1, 1, 1, 1};
    std::array<float, 8>    dc{};
    std::uint64_t           position = 0;
    std::uint64_t           limit    = UINT64_MAX;  // frames available in total

    static std::size_t pull(void* user_data, std::span<float> dst) noexcept
    {
        auto&             self   = *static_cast<TestSignal*>(user_data);
        std::size_t const frames = std::min<std::uint64_t>(dst.size() / self.channels,
                                                           self.limit - self.position);
        for (std::size_t f = 0; f < frames; ++f, ++self.position) {
            double const phase = 2.0 * std::numbers::pi * self.frequency *
                                 static_cast<double>(self.position) / self.sample_rate;
            auto const v = static_cast<float>(self.amplitude * std::sin(phase));
            for (std::uint32_t c = 0; c < self.channels; ++c) {
                dst[f * self.channels + c] = self.gain[c] * v + self.dc[c];
            }
        }
        return frames;
    }
};

// Renders `total` output frames in blocks of `block`; every block must be filled completely.
std::vector<float> render(FormatConverter& converter, TestSignal& signal, std::uint32_t channels,
                          std::size_t total, std::size_t block)
{
    std::vector<float> out(total * channels, -100.0f);
    for (std::size_t done = 0; done < total;) {
        std::size_t const n = std::min(block, total - done);
        MutableAudioBufferView view{out.data() + done * channels, n, channels};
        std::size_t const produced = converter.process(view, &TestSignal::pull, &signal);
        EXPECT_EQ(produced, n);
        if (produced != n) {
            break;
        }
        done += n;
    }
    return out;
}

std::vector<float> channel(const std::vector<float>& interleaved, std::uint32_t channels,
                           std::uint32_t index, std::size_t skip)
{
    std::vector<float> out;
    for (std::size_t i = skip * channels + index; i < interleaved.size(); i += channels) {
        out.push_back(interleaved[i]);
    }
    return out;
}

// Goertzel power at `frequency`, Hann windowed to keep leakage from distant bins low.
double goertzel_power(const std::vector<float>& x, double sample_rate, double frequency)
{
    double const w     = 2.0 * std::numbers::pi * frequency / sample_rate;
    double const coeff = 2.0 * std::cos(w);
    double       s1 = 0.0, s2 = 0.0;
    auto const   n  = static_cast<double>(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) {
        double const hann = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / (n - 1));
        double const s    = x[i] * hann + coeff * s1 - s2;
        s2                = s1;
        s1                = s;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

double rms(const std::vector<float>& x)
{
    double sum = 0.0;
    for (float v : x) {
        sum += static_cast<double>(v) * v;
    }
    return std::sqrt(sum / static_cast<double>(x.size()));
}

double mean(const std::vector<float>& x)
{
    double sum = 0.0;
    for (float v : x) {
        sum += v;
    }
    return sum / static_cast<double>(x.size());
}

double max_abs_second_difference(const std::vector<float>& x)
{
    double worst = 0.0;
    for (std::size_t i = 2; i < x.size(); ++i) {
        worst = std::max(worst, std::abs(static_cast<double>(x[i]) - 2.0 * x[i - 1] + x[i - 2]));
    }
    return worst;
}

struct RateCase
{
    double in_rate;
    double out_rate;
};

class FormatConverterRates : public ::testing::TestWithParam<RateCase>
{};

}  // namespace

TEST(FormatConverter, RejectsInvalidFormats)
{
    FormatConverter converter;
    EXPECT_FALSE(converter.prepare({0.0, 2}, {48000.0, 2}, kMaxChunk).has_value());
    EXPECT_FALSE(converter.prepare({48000.0, 0}, {48000.0, 2}, kMaxChunk).has_value());
    EXPECT_FALSE(converter.prepare({48000.0, 2}, {48000.0, 0}, kMaxChunk).has_value());
    EXPECT_FALSE(converter.prepare({48000.0, 2}, {48000.0, 2}, 0).has_value());
    auto const result = converter.prepare({48000.0, 2}, {1.0, 2}, kMaxChunk);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, AudioErrorCode::unsupported_configuration);
}

TEST(FormatConverter, ProcessWithoutPrepareOrWrongChannelsProducesNothing)
{
    FormatConverter    converter;
    TestSignal         signal;
    std::vector<float> out(64 * 2);
    EXPECT_EQ(converter.process({out.data(), 64, 2}, &TestSignal::pull, &signal), 0u);
    ASSERT_TRUE(converter.prepare({48000.0, 2}, {48000.0, 2}, kMaxChunk).has_value());
    EXPECT_EQ(converter.process({out.data(), 32, 4}, &TestSignal::pull, &signal), 0u);
}

TEST_P(FormatConverterRates, SineKeepsFrequencyAndLevel)
{
    auto const [in_rate, out_rate] = GetParam();
    FormatConverter converter;
    ASSERT_TRUE(converter.prepare({in_rate, 2}, {out_rate, 2}, kMaxChunk).has_value());

    TestSignal signal{.sample_rate = in_rate};
    auto const out  = render(converter, signal, 2, kSettle + 32768, 512);
    auto const left = channel(out, 2, 0, kSettle);

    double best_frequency = 0.0;
    double best_power     = 0.0;
    for (double f = 990.0; f <= 1010.0; f += 0.25) {
        double const p = goertzel_power(left, out_rate, f);
        if (p > best_power) {
            best_power     = p;
            best_frequency = f;
        }
    }
    EXPECT_NEAR(best_frequency, 1000.0, 1.0);
    EXPECT_GT(goertzel_power(left, out_rate, 1000.0), 1e6 * goertzel_power(left, out_rate, 2000.0));
    EXPECT_GT(goertzel_power(left, out_rate, 1000.0), 1e6 * goertzel_power(left, out_rate, 500.0));

    for (std::uint32_t c = 0; c < 2; ++c) {
        double const level_db =
            20.0 * std::log10(rms(channel(out, 2, c, kSettle)) * std::numbers::sqrt2 / 0.5);
        EXPECT_NEAR(level_db, 0.0, 0.5) << "channel " << c;
    }
}

TEST_P(FormatConverterRates, RatioChangesMidStreamStayContinuous)
{
    auto const [in_rate, out_rate] = GetParam();
    FormatConverter converter;
    ASSERT_TRUE(converter.prepare({in_rate, 2}, {out_rate, 2}, kMaxChunk).has_value());

    TestSignal         signal{.sample_rate = in_rate};
    std::vector<float> out((kSettle + 48000) * 2);
    std::mt19937       rng{42};
    std::uniform_real_distribution<double> deviation{-1.0, 1.0};
    std::size_t                            done = 0;
    for (int block = 0; done < out.size() / 2; ++block) {
        // Alternate between the extremes, then random values within the range.
        double const ratio =
            block < 40 ? (block % 2 == 0 ? 1.0 - FormatConverter::max_ratio_deviation
                                         : 1.0 + FormatConverter::max_ratio_deviation)
                       : 1.0 + deviation(rng) * FormatConverter::max_ratio_deviation;
        converter.set_ratio(ratio);
        std::size_t const n = std::min<std::size_t>(256, out.size() / 2 - done);
        ASSERT_EQ(converter.process({out.data() + done * 2, n, 2}, &TestSignal::pull, &signal), n);
        done += n;
    }

    // A clean sine of amplitude A, angular step w per sample has |x''| <= A w^2. Output frequency
    // shifts by at most the ratio deviation.
    double const w     = 2.0 * std::numbers::pi * 1000.0 *
                     (1.0 + FormatConverter::max_ratio_deviation) / out_rate;
    double const bound = 0.5 * w * w * 1.05 + 1e-3;
    for (std::uint32_t c = 0; c < 2; ++c) {
        EXPECT_LT(max_abs_second_difference(channel(out, 2, c, kSettle)), bound) << "channel " << c;
        double const level_db =
            20.0 * std::log10(rms(channel(out, 2, c, kSettle)) * std::numbers::sqrt2 / 0.5);
        EXPECT_NEAR(level_db, 0.0, 0.5) << "channel " << c;
    }
}

TEST_P(FormatConverterRates, AnyCallbackSizeGivesSameOutput)
{
    auto const [in_rate, out_rate] = GetParam();
    constexpr std::size_t kTotal   = 3 * 4096 + 123;

    FormatConverter reference_converter;
    ASSERT_TRUE(reference_converter.prepare({in_rate, 2}, {out_rate, 2}, kMaxChunk).has_value());
    TestSignal reference_signal{.sample_rate = in_rate};
    auto const reference = render(reference_converter, reference_signal, 2, kTotal, 512);

    for (std::size_t const block : {1u, 37u, 4096u}) {
        FormatConverter converter;
        ASSERT_TRUE(converter.prepare({in_rate, 2}, {out_rate, 2}, kMaxChunk).has_value());
        TestSignal signal{.sample_rate = in_rate};
        auto const out = render(converter, signal, 2, kTotal, block);
        ASSERT_EQ(out.size(), reference.size());
        float worst = 0.0f;
        for (std::size_t i = 0; i < out.size(); ++i) {
            worst = std::max(worst, std::abs(out[i] - reference[i]));
        }
        EXPECT_LT(worst, 1e-5f) << "block " << block;
    }
}

INSTANTIATE_TEST_SUITE_P(Rates, FormatConverterRates,
                         ::testing::Values(RateCase{48000.0, 44100.0}, RateCase{44100.0, 48000.0},
                                           RateCase{48000.0, 48000.0}),
                         [](const auto& info) {
                             return std::to_string(static_cast<int>(info.param.in_rate)) + "_to_" +
                                    std::to_string(static_cast<int>(info.param.out_rate));
                         });

TEST(FormatConverter, ChannelMapping)
{
    struct Case
    {
        double                in_rate;
        double                out_rate;
        std::uint32_t         in_channels;
        std::uint32_t         out_channels;
        std::array<float, 8>  in_dc;
        std::vector<float>    expected;
    };
    std::vector<Case> const cases{
        {48000.0, 48000.0, 1, 2, {0.3f}, {0.3f, 0.3f}},
        {48000.0, 48000.0, 2, 1, {0.2f, 0.6f}, {0.4f}},
        {48000.0, 48000.0, 3, 1, {0.1f, 0.2f, 0.6f}, {0.3f}},
        {48000.0, 48000.0, 2, 4, {0.2f, 0.6f}, {0.2f, 0.6f, 0.0f, 0.0f}},
        {48000.0, 48000.0, 6, 2, {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f}, {0.1f, 0.2f}},
        {44100.0, 48000.0, 1, 2, {0.3f}, {0.3f, 0.3f}},   // upmix after resampling
        {48000.0, 44100.0, 2, 1, {0.2f, 0.6f}, {0.4f}},   // downmix before resampling
        {44100.0, 48000.0, 2, 4, {0.2f, 0.6f}, {0.2f, 0.6f, 0.0f, 0.0f}},
    };

    for (auto const& tc : cases) {
        SCOPED_TRACE(::testing::Message() << tc.in_rate << "Hz x" << tc.in_channels << " -> "
                                          << tc.out_rate << "Hz x" << tc.out_channels);
        FormatConverter converter;
        ASSERT_TRUE(converter
                        .prepare({tc.in_rate, tc.in_channels}, {tc.out_rate, tc.out_channels},
                                 kMaxChunk)
                        .has_value());
        TestSignal signal{.sample_rate = tc.in_rate, .channels = tc.in_channels, .amplitude = 0.0};
        signal.dc = tc.in_dc;
        auto const out = render(converter, signal, tc.out_channels, kSettle + 4096, 256);
        for (std::uint32_t c = 0; c < tc.out_channels; ++c) {
            EXPECT_NEAR(mean(channel(out, tc.out_channels, c, kSettle)), tc.expected[c], 2e-3)
                << "channel " << c;
        }
    }
}

TEST(FormatConverter, ReturnsShortCountWhenInputRunsOutThenResumes)
{
    FormatConverter converter;
    ASSERT_TRUE(converter.prepare({48000.0, 2}, {48000.0, 2}, kMaxChunk).has_value());
    TestSignal signal{.limit = 1000};

    std::vector<float> out(4096 * 2);
    std::size_t const  first = converter.process({out.data(), 4096, 2}, &TestSignal::pull, &signal);
    EXPECT_GT(first, 0u);
    EXPECT_LE(first, 1000u);
    EXPECT_EQ(converter.process({out.data(), 64, 2}, &TestSignal::pull, &signal), 0u);

    signal.limit = UINT64_MAX;
    EXPECT_EQ(converter.process({out.data(), 4096, 2}, &TestSignal::pull, &signal), 4096u);
}

TEST(FormatConverter, RatioIsClamped)
{
    FormatConverter converter;
    ASSERT_TRUE(converter.prepare({48000.0, 2}, {48000.0, 2}, kMaxChunk).has_value());
    EXPECT_EQ(converter.ratio(), 1.0);
    converter.set_ratio(2.0);
    EXPECT_DOUBLE_EQ(converter.ratio(), 1.0 + FormatConverter::max_ratio_deviation);
    converter.set_ratio(0.0);
    EXPECT_DOUBLE_EQ(converter.ratio(), 1.0 - FormatConverter::max_ratio_deviation);
}

// Real-time path (set_ratio + process) must never allocate, including inside speexdsp (C malloc is
// observed in sanitizer builds). Covers nominal ratios that sit on speex's oversampling boundaries
// (2x, 4x), where the filter table size jumps.
TEST(FormatConverter, RealTimePathDoesNotAllocate)
{
    if (!rt_guard_covers_malloc()) {
        std::cout << "[ NOTE ] non-sanitizer build: only operator new/delete are observed\n";
    }
    struct Case
    {
        AudioFormat in;
        AudioFormat out;
    };
    std::vector<Case> const cases{
        {{48000.0, 2}, {44100.0, 2}}, {{44100.0, 2}, {48000.0, 2}}, {{48000.0, 2}, {48000.0, 2}},
        {{96000.0, 2}, {48000.0, 2}}, {{192000.0, 2}, {48000.0, 2}}, {{8000.0, 1}, {48000.0, 2}},
        {{48000.0, 2}, {8000.0, 1}},  {{48000.0, 6}, {44100.0, 2}}, {{22050.0, 2}, {96000.0, 8}},
    };
    std::array<std::size_t, 5> const blocks{1, 37, 512, 4096, 100};
    std::vector<float>               out(4096 * 8);
    std::mt19937                     rng{7};
    std::uniform_real_distribution<double> ratio{0.98, 1.02};  // also exercises the clamp

    for (auto const& tc : cases) {
        SCOPED_TRACE(::testing::Message() << tc.in.sample_rate << "x" << tc.in.channels << " -> "
                                          << tc.out.sample_rate << "x" << tc.out.channels);
        FormatConverter converter;
        ASSERT_TRUE(converter.prepare(tc.in, tc.out, kMaxChunk).has_value());
        TestSignal signal{.sample_rate = tc.in.sample_rate, .channels = tc.in.channels};

        auto const before = rt_allocation_count();
        {
            RtScope rt;
            for (int i = 0; i < 400; ++i) {
                double const r = i < 4 ? (i % 2 == 0 ? 0.0 : 2.0) : ratio(rng);
                converter.set_ratio(r);
                std::size_t const n = blocks[static_cast<std::size_t>(i) % blocks.size()];
                converter.process({out.data(), n, tc.out.channels}, &TestSignal::pull, &signal);
            }
        }
        EXPECT_EQ(rt_allocation_count(), before);
    }
}
