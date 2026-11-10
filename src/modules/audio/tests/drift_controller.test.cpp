#include "pipeline/drift_controller.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>

#include "pipeline/format_converter.hpp"
#include "rt_alloc_guard.hpp"

using namespace dth::audio;
using dth::audio::test::rt_allocation_count;
using dth::audio::test::RtScope;

namespace {

constexpr double kMaxDev = FormatConverter::max_ratio_deviation;
// 1000 ppm ~ 1.7 cents of pitch wobble, well below the ~5 cent pitch JND.
constexpr double kMaxSettledRatioPeakToPeak = 1000e-6;

// --- Simulation ------------------------------------------------------------------------------
//
// Producer and consumer run on independent clocks (nominal rate * (1 + ppm)), bridged by a
// bounded ring of `producer_rate` frames. The producer pushes fixed blocks; the consumer, once per
// callback, samples the fill, updates the controller and pulls consumer_block * nominal * ratio
// ring frames (fractional part carried, like the resampler). The ratio is quantized the way
// FormatConverter quantizes it. Full ring: drop + overrun. Short ring: take what is there +
// underrun.

struct SimConfig
{
    double      producer_rate  = 48000.0;
    double      consumer_rate  = 48000.0;
    double      producer_ppm   = 0.0;
    double      consumer_ppm   = 0.0;
    std::size_t producer_block = 512;  // producer frames per callback
    std::size_t consumer_block = 480;  // consumer (output) frames per callback
    double      jitter         = 0.0;  // callback time jitter, fraction of the period (uniform +-)
    bool        prefill        = true; // start with the ring at target fill (else empty)
    double      seconds        = 600.0;
    double      settle_seconds = 60.0;
    double      tail_seconds   = 60.0;  // averages are taken over the last tail_seconds
};

struct SimResult
{
    std::size_t target   = 0;
    std::size_t capacity = 0;

    std::uint64_t underruns_total  = 0;
    std::uint64_t underruns_settled = 0;
    std::uint64_t overruns_settled  = 0;

    std::size_t min_fill_settled = std::numeric_limits<std::size_t>::max();
    std::size_t max_fill_settled = 0;
    double      min_ratio        = 2.0;
    double      max_ratio        = 0.0;
    double      min_ratio_settled = 2.0;
    double      max_ratio_settled = 0.0;

    double tail_mean_ratio = 0.0;
    double tail_mean_fill  = 0.0;

    std::uint64_t ratio_changes_settled = 0;  // effective (quantized) changes = filter recomputes
};

SimResult simulate(const SimConfig& cfg)
{
    double const nominal = std::round(cfg.producer_rate) / std::round(cfg.consumer_rate);
    auto const   callback_ring_frames =
        static_cast<std::size_t>(std::lround(static_cast<double>(cfg.consumer_block) * nominal));

    DriftController controller;
    EXPECT_TRUE(controller.prepare({cfg.producer_rate, callback_ring_frames}).has_value());
    // As DeviceOutputSink does: the largest write seen so far (constant block here).
    controller.set_producer_block_frames(cfg.producer_block);

    SimResult r;
    r.target   = controller.target_fill();
    r.capacity = 4 * std::max(cfg.producer_block, callback_ring_frames) + r.target;

    double const producer_period =
        static_cast<double>(cfg.producer_block) / (cfg.producer_rate * (1.0 + cfg.producer_ppm * 1e-6));
    double const consumer_period =
        static_cast<double>(cfg.consumer_block) / (cfg.consumer_rate * (1.0 + cfg.consumer_ppm * 1e-6));

    std::mt19937_64                        rng{1234};
    std::uniform_real_distribution<double> jitter{-cfg.jitter, cfg.jitter};

    std::uint64_t producer_n = 0;
    std::uint64_t consumer_n = 0;
    auto producer_time = [&] { return (static_cast<double>(producer_n) + jitter(rng)) * producer_period; };
    auto consumer_time = [&] { return (static_cast<double>(consumer_n) + jitter(rng)) * consumer_period; };
    double next_producer = producer_time();
    double next_consumer = consumer_time();

    std::size_t   fill      = cfg.prefill ? r.target : 0;
    double        owed      = 0.0;  // fractional ring frames carried between consumer callbacks
    std::int64_t  last_num  = -1;
    double        tail_ratio_sum = 0.0;
    double        tail_fill_sum  = 0.0;
    std::uint64_t tail_count     = 0;

    while (std::min(next_producer, next_consumer) < cfg.seconds) {
        if (next_producer <= next_consumer) {
            bool const settled = next_producer >= cfg.settle_seconds;
            std::size_t const free = r.capacity - fill;
            if (cfg.producer_block > free) {
                r.overruns_settled += settled ? 1 : 0;
                fill = r.capacity;
            }
            else {
                fill += cfg.producer_block;
            }
            ++producer_n;
            next_producer = producer_time();
            continue;
        }

        double const now     = next_consumer;
        bool const   settled = now >= cfg.settle_seconds;
        if (settled) {
            r.min_fill_settled = std::min(r.min_fill_settled, fill);
            r.max_fill_settled = std::max(r.max_fill_settled, fill);
        }

        double const ratio = controller.update(fill);
        r.min_ratio        = std::min(r.min_ratio, ratio);
        r.max_ratio        = std::max(r.max_ratio, ratio);
        if (settled) {
            r.min_ratio_settled = std::min(r.min_ratio_settled, ratio);
            r.max_ratio_settled = std::max(r.max_ratio_settled, ratio);
        }

        // Quantization as in FormatConverter: num / 2^16 relative to the nominal ratio.
        std::int64_t const num = std::llround(nominal * ratio * 65536.0);
        if (num != last_num && last_num >= 0 && settled) {
            ++r.ratio_changes_settled;
        }
        last_num = num;
        double const effective = static_cast<double>(num) / 65536.0;  // ring frames per output frame

        owed += static_cast<double>(cfg.consumer_block) * effective;
        auto const need = static_cast<std::size_t>(owed);
        owed -= static_cast<double>(need);
        if (need > fill) {
            ++r.underruns_total;
            r.underruns_settled += settled ? 1 : 0;
            fill = 0;
        }
        else {
            fill -= need;
        }

        if (now >= cfg.seconds - cfg.tail_seconds) {
            tail_ratio_sum += ratio;
            tail_fill_sum += static_cast<double>(fill + need);  // fill as sampled by update()
            ++tail_count;
        }

        ++consumer_n;
        next_consumer = consumer_time();
    }

    r.tail_mean_ratio = tail_ratio_sum / static_cast<double>(tail_count);
    r.tail_mean_fill  = tail_fill_sum / static_cast<double>(tail_count);
    return r;
}

void expect_settles(const SimConfig& cfg)
{
    SimResult const r = simulate(cfg);

    double const expected_ratio = (1.0 + cfg.producer_ppm * 1e-6) / (1.0 + cfg.consumer_ppm * 1e-6);
    // P-loop steady-state offset: fill error = (ratio - 1) * rate * correction_seconds.
    double const expected_fill =
        static_cast<double>(r.target) +
        (expected_ratio - 1.0) * cfg.producer_rate * DriftController::correction_seconds;
    double const settled_minutes = (cfg.seconds - cfg.settle_seconds) / 60.0;

    std::cout << "  target " << r.target << " cap " << r.capacity << " | fill settled ["
              << r.min_fill_settled << ", " << r.max_fill_settled << "] tail mean "
              << r.tail_mean_fill << " (expect " << expected_fill << ") | ratio ["
              << r.min_ratio << ", " << r.max_ratio << "] tail mean " << r.tail_mean_ratio
              << " (expect " << expected_ratio << ") settled p-p "
              << (r.max_ratio_settled - r.min_ratio_settled) * 1e6 << " ppm | underruns total " << r.underruns_total
              << " | ratio changes/min " << static_cast<double>(r.ratio_changes_settled) / settled_minutes
              << '\n';

    EXPECT_EQ(r.underruns_settled, 0u);
    EXPECT_EQ(r.overruns_settled, 0u);
    EXPECT_GT(r.min_fill_settled, 0u);
    EXPECT_LT(r.max_fill_settled, r.capacity);
    EXPECT_GE(r.min_ratio, 1.0 - kMaxDev);
    EXPECT_LE(r.max_ratio, 1.0 + kMaxDev);
    EXPECT_NEAR(r.tail_mean_ratio, expected_ratio, 20e-6);
    // Smoothing keeps the block-write fill sawtooth out of the ratio.
    EXPECT_LT(r.max_ratio_settled - r.min_ratio_settled, kMaxSettledRatioPeakToPeak);
    EXPECT_NEAR(r.tail_mean_fill, expected_fill, 0.05 * static_cast<double>(r.target));
}

}  // namespace

// --- Unit ------------------------------------------------------------------------------------

TEST(DriftController, RejectsInvalidConfig)
{
    DriftController c;
    EXPECT_FALSE(c.prepare({0.0, 480}).has_value());
    EXPECT_FALSE(c.prepare({-48000.0, 480}).has_value());
    EXPECT_FALSE(c.prepare({std::numeric_limits<double>::quiet_NaN(), 480}).has_value());
    EXPECT_FALSE(c.prepare({48000.0, 0}).has_value());
}

TEST(DriftController, TargetIsTwoCallbacksPlusMargin)
{
    DriftController c;
    ASSERT_TRUE(c.prepare({48000.0, 480}).has_value());
    EXPECT_EQ(c.target_fill(), 2u * 480u + 480u);
    ASSERT_TRUE(c.prepare({44100.0, 512}).has_value());
    EXPECT_EQ(c.target_fill(), 2u * 512u + 441u);
    EXPECT_EQ(c.ratio(), 1.0);
}

TEST(DriftController, AtTargetStaysAtUnity)
{
    DriftController c;
    ASSERT_TRUE(c.prepare({48000.0, 480}).has_value());
    for (int i = 0; i < 1000; ++i) {
        EXPECT_EQ(c.update(c.target_fill()), 1.0);
    }
}

TEST(DriftController, DirectionAndClamp)
{
    DriftController c;
    ASSERT_TRUE(c.prepare({48000.0, 480}).has_value());

    // Too full: consume faster.
    double previous = c.ratio();
    for (int i = 0; i < 2000; ++i) {
        double const r = c.update(1'000'000);
        EXPECT_GE(r, previous);
        EXPECT_LE(r, 1.0 + kMaxDev);
        previous = r;
    }
    EXPECT_NEAR(c.ratio(), 1.0 + kMaxDev, DriftController::apply_deadband);

    // Empty: consume slower.
    for (int i = 0; i < 4000; ++i) {
        double const r = c.update(0);
        EXPECT_LE(r, previous);
        EXPECT_GE(r, 1.0 - kMaxDev);
        previous = r;
    }
    EXPECT_NEAR(c.ratio(), 1.0 - kMaxDev, DriftController::apply_deadband);
}

// Applied ratio changes are rate limited (each one rebuilds the resampler filter table).
TEST(DriftController, ChangesAreHeldAndDeadbanded)
{
    DriftController c;
    ASSERT_TRUE(c.prepare({48000.0, 480}).has_value());  // 10 ms callbacks
    auto const hold = static_cast<std::size_t>(DriftController::apply_hold_seconds / 0.010 + 0.5);

    double      previous   = c.ratio();
    std::size_t since      = hold;
    std::size_t changes    = 0;
    std::mt19937 rng{3};
    for (int i = 0; i < 20000; ++i) {
        double const r = c.update(i < 10000 ? rng() % 4000 : c.target_fill());
        ++since;
        if (r != previous) {
            EXPECT_GE(since, hold) << "at update " << i;
            EXPECT_GE(std::abs(r - previous), DriftController::apply_deadband) << "at update " << i;
            since = 0;
            ++changes;
            previous = r;
        }
    }
    EXPECT_GT(changes, 0u);
    EXPECT_LE(changes, 20000u / hold);
    // First change after prepare()/reset() is not held.
    c.reset();
    EXPECT_NE(c.update(0), 1.0);
}

TEST(DriftController, ProducerBlockRaisesTarget)
{
    DriftController c;
    ASSERT_TRUE(c.prepare({48000.0, 128}).has_value());
    EXPECT_EQ(c.target_fill(), 256u + 480u);
    c.set_producer_block_frames(256);  // below the margin: unchanged
    EXPECT_EQ(c.target_fill(), 256u + 480u);
    c.set_producer_block_frames(2048);
    EXPECT_EQ(c.target_fill(), 256u + 2048u);
    c.reset();
    EXPECT_EQ(c.target_fill(), 256u + 2048u);
    ASSERT_TRUE(c.prepare({48000.0, 128}).has_value());
    EXPECT_EQ(c.target_fill(), 256u + 480u);
}

TEST(DriftController, ResetAndPrepareRestoreUnity)
{
    DriftController c;
    ASSERT_TRUE(c.prepare({48000.0, 480}).has_value());
    c.update(0);
    EXPECT_NE(c.ratio(), 1.0);
    c.reset();
    EXPECT_EQ(c.ratio(), 1.0);
    EXPECT_EQ(c.target_fill(), 1440u);

    c.update(0);
    ASSERT_TRUE(c.prepare({48000.0, 480}).has_value());
    EXPECT_EQ(c.ratio(), 1.0);
}

TEST(DriftController, Deterministic)
{
    DriftController a;
    DriftController b;
    ASSERT_TRUE(a.prepare({48000.0, 480}).has_value());
    ASSERT_TRUE(b.prepare({48000.0, 480}).has_value());
    std::mt19937 rng{7};
    for (int i = 0; i < 10000; ++i) {
        std::size_t const fill = rng() % 4000;
        EXPECT_EQ(a.update(fill), b.update(fill));
    }
}

TEST(DriftController, UpdateDoesNotAllocate)
{
    DriftController c;
    ASSERT_TRUE(c.prepare({48000.0, 480}).has_value());
    std::size_t const before = rt_allocation_count();
    {
        RtScope const rt;
        for (std::size_t i = 0; i < 100000; ++i) {
            c.update(i % 3000);
        }
        c.reset();
    }
    EXPECT_EQ(rt_allocation_count(), before);
}

// --- Simulation: +-200 ppm clocks, 10 simulated minutes --------------------------------------

TEST(DriftControllerSim, ProducerFastConsumerSlow)
{
    expect_settles({.producer_ppm = 200.0, .consumer_ppm = -200.0});
}

TEST(DriftControllerSim, ProducerSlowConsumerFast)
{
    expect_settles({.producer_ppm = -200.0, .consumer_ppm = 200.0});
}

TEST(DriftControllerSim, SameClock)
{
    expect_settles({});
}

TEST(DriftControllerSim, Resample44100To48000)
{
    expect_settles({.producer_rate  = 44100.0,
                    .producer_ppm   = 200.0,
                    .consumer_ppm   = -200.0,
                    .consumer_block = 512});
    expect_settles({.producer_rate  = 44100.0,
                    .producer_ppm   = -200.0,
                    .consumer_ppm   = 200.0,
                    .consumer_block = 512});
}

TEST(DriftControllerSim, Resample48000To44100)
{
    expect_settles({.consumer_rate = 44100.0, .producer_ppm = 200.0, .consumer_ppm = -200.0});
    expect_settles({.consumer_rate = 44100.0, .producer_ppm = -200.0, .consumer_ppm = 200.0});
}

TEST(DriftControllerSim, SmallConsumerBlockWithJitter)
{
    expect_settles({.producer_ppm   = -200.0,
                    .consumer_ppm   = 200.0,
                    .consumer_block = 128,
                    .jitter         = 0.25});
    expect_settles({.producer_ppm   = 200.0,
                    .consumer_ppm   = -200.0,
                    .consumer_block = 128,
                    .jitter         = 0.25});
}

TEST(DriftControllerSim, StartsFromEmptyRing)
{
    expect_settles({.producer_ppm = 200.0, .consumer_ppm = -200.0, .prefill = false});
    expect_settles({.producer_ppm = -200.0, .consumer_ppm = 200.0, .prefill = false});
}

// Producer bursts larger than the margin (e.g. a 2048-frame tap block into 128-frame callbacks).
TEST(DriftControllerSim, LargeProducerBlock)
{
    expect_settles({.producer_ppm   = 200.0,
                    .consumer_ppm   = -200.0,
                    .producer_block = 2048,
                    .consumer_block = 128,
                    .jitter         = 0.02});
    expect_settles({.producer_ppm   = -200.0,
                    .consumer_ppm   = 200.0,
                    .producer_block = 2048,
                    .consumer_block = 128,
                    .jitter         = 0.02});
}
