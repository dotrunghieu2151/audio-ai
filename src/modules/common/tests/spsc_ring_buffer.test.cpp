#include "dth/common/spsc_ring_buffer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

using dth::common::SpscRingBuffer;

namespace {

std::vector<std::uint64_t> iota_vec(std::size_t n, std::uint64_t start = 0)
{
    std::vector<std::uint64_t> v(n);
    std::iota(v.begin(), v.end(), start);
    return v;
}

}  // namespace

TEST(SpscRingBuffer, CapacityRoundsUpToPowerOfTwo)
{
    EXPECT_EQ(SpscRingBuffer<float>{0}.capacity(), 1u);
    EXPECT_EQ(SpscRingBuffer<float>{1}.capacity(), 1u);
    EXPECT_EQ(SpscRingBuffer<float>{2}.capacity(), 2u);
    EXPECT_EQ(SpscRingBuffer<float>{3}.capacity(), 4u);
    EXPECT_EQ(SpscRingBuffer<float>{1000}.capacity(), 1024u);
    EXPECT_EQ(SpscRingBuffer<float>{1024}.capacity(), 1024u);
}

TEST(SpscRingBuffer, StartsEmpty)
{
    SpscRingBuffer<float> rb{8};
    EXPECT_EQ(rb.size(), 0u);
    EXPECT_EQ(rb.free_space(), 8u);

    std::vector<float> dst(4, -1.0f);
    EXPECT_EQ(rb.read(dst), 0u);
    EXPECT_EQ(dst, std::vector<float>(4, -1.0f));
}

TEST(SpscRingBuffer, ExactCapacityFillThenFull)
{
    SpscRingBuffer<std::uint64_t> rb{8};
    const auto                    src = iota_vec(8);
    EXPECT_EQ(rb.write(src), 8u);
    EXPECT_EQ(rb.size(), 8u);
    EXPECT_EQ(rb.free_space(), 0u);

    const std::uint64_t extra[] = {99};
    EXPECT_EQ(rb.write(extra), 0u);

    std::vector<std::uint64_t> dst(8);
    EXPECT_EQ(rb.read(dst), 8u);
    EXPECT_EQ(dst, src);
    EXPECT_EQ(rb.size(), 0u);
}

TEST(SpscRingBuffer, PartialWriteWhenNotEnoughSpace)
{
    SpscRingBuffer<std::uint64_t> rb{8};
    EXPECT_EQ(rb.write(iota_vec(5)), 5u);
    EXPECT_EQ(rb.write(iota_vec(5, 5)), 3u);
    EXPECT_EQ(rb.size(), 8u);

    std::vector<std::uint64_t> dst(8);
    EXPECT_EQ(rb.read(dst), 8u);
    EXPECT_EQ(dst, iota_vec(8));
}

TEST(SpscRingBuffer, PartialReadWhenNotEnoughData)
{
    SpscRingBuffer<std::uint64_t> rb{8};
    EXPECT_EQ(rb.write(iota_vec(3)), 3u);

    std::vector<std::uint64_t> dst(8, 77);
    EXPECT_EQ(rb.read(dst), 3u);
    EXPECT_EQ(dst[0], 0u);
    EXPECT_EQ(dst[1], 1u);
    EXPECT_EQ(dst[2], 2u);
    EXPECT_EQ(dst[3], 77u);  // untouched
    EXPECT_EQ(rb.size(), 0u);
}

TEST(SpscRingBuffer, ZeroLengthSpans)
{
    SpscRingBuffer<float> rb{4};
    EXPECT_EQ(rb.write(std::span<const float>{}), 0u);
    EXPECT_EQ(rb.read(std::span<float>{}), 0u);

    const float one[] = {1.0f};
    rb.write(one);
    EXPECT_EQ(rb.write(std::span<const float>{}), 0u);
    EXPECT_EQ(rb.read(std::span<float>{}), 0u);
    EXPECT_EQ(rb.size(), 1u);
}

// For every start offset and every chunk size, data written across the wrap point comes back
// intact and in order.
TEST(SpscRingBuffer, WraparoundAtEveryOffset)
{
    constexpr std::size_t k_cap = 16;
    for (std::size_t offset = 0; offset < k_cap; ++offset) {
        for (std::size_t chunk = 1; chunk <= k_cap; ++chunk) {
            SpscRingBuffer<std::uint64_t> rb{k_cap};
            std::vector<std::uint64_t>    skip(offset);
            ASSERT_EQ(rb.write(iota_vec(offset)), offset);
            ASSERT_EQ(rb.read(skip), offset);

            const auto src = iota_vec(chunk, 1000 * offset + chunk);
            ASSERT_EQ(rb.write(src), chunk);
            ASSERT_EQ(rb.size(), chunk);
            ASSERT_EQ(rb.free_space(), k_cap - chunk);

            std::vector<std::uint64_t> dst(chunk);
            ASSERT_EQ(rb.read(dst), chunk);
            ASSERT_EQ(dst, src) << "offset=" << offset << " chunk=" << chunk;
        }
    }
}

TEST(SpscRingBuffer, SizeAndFreeSpaceStayConsistent)
{
    SpscRingBuffer<std::uint64_t> rb{32};
    std::mt19937                  rng{42};
    std::size_t                   expected = 0;
    std::uint64_t                 next_in  = 0;
    std::uint64_t                 next_out = 0;
    for (int i = 0; i < 10000; ++i) {
        const std::size_t n = std::uniform_int_distribution<std::size_t>{0, 40}(rng);
        if (rng() & 1u) {
            const auto        src     = iota_vec(n, next_in);
            const std::size_t written = rb.write(src);
            ASSERT_EQ(written, std::min(n, 32 - expected));
            next_in += written;
            expected += written;
        } else {
            std::vector<std::uint64_t> dst(n);
            const std::size_t          got = rb.read(dst);
            ASSERT_EQ(got, std::min(n, expected));
            for (std::size_t k = 0; k < got; ++k) {
                ASSERT_EQ(dst[k], next_out++);
            }
            expected -= got;
        }
        ASSERT_EQ(rb.size(), expected);
        ASSERT_EQ(rb.free_space(), 32 - expected);
    }
}

TEST(SpscRingBuffer, NonFloatElementType)
{
    struct Frame
    {
        std::uint64_t id;
        std::uint32_t tag;
        float         value;
        bool          operator==(const Frame&) const = default;
    };

    SpscRingBuffer<Frame> rb{3};
    ASSERT_EQ(rb.capacity(), 4u);
    const Frame src[] = {{1, 2, 3.0f}, {4, 5, 6.0f}, {7, 8, 9.0f}};
    Frame       dst[3]{};
    for (int round = 0; round < 5; ++round) {  // crosses the wrap point
        ASSERT_EQ(rb.write(src), 3u);
        ASSERT_EQ(rb.read(dst), 3u);
        EXPECT_TRUE(std::equal(std::begin(src), std::end(src), std::begin(dst)));
    }
}

namespace {

// Producer pushes a monotonically increasing sequence in random chunk sizes, consumer pops in
// random chunk sizes and verifies strict order (no loss, no duplication, no reordering).
void run_stress(std::size_t capacity, std::size_t max_chunk, std::uint64_t total)
{
    SpscRingBuffer<std::uint64_t> rb{capacity};

    std::thread producer([&] {
        std::mt19937                                 rng{1};
        std::uniform_int_distribution<std::size_t>   dist{1, max_chunk};
        std::vector<std::uint64_t>                   chunk(max_chunk);
        std::uint64_t                                next = 0;
        while (next < total) {
            const std::size_t n = std::min<std::uint64_t>(dist(rng), total - next);
            std::iota(chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n), next);
            std::size_t done = 0;
            while (done < n) {
                const std::size_t w = rb.write(std::span{chunk}.subspan(done, n - done));
                if (w == 0) {
                    std::this_thread::yield();
                }
                done += w;
            }
            next += n;
        }
    });

    std::mt19937                               rng{2};
    std::uniform_int_distribution<std::size_t> dist{1, max_chunk};
    std::vector<std::uint64_t>                 chunk(max_chunk);
    std::uint64_t                              expected   = 0;
    std::uint64_t                              mismatches = 0;
    while (expected < total) {
        const std::size_t n = rb.read(std::span{chunk}.first(dist(rng)));
        if (n == 0) {
            std::this_thread::yield();
            continue;
        }
        for (std::size_t k = 0; k < n; ++k) {
            mismatches += (chunk[k] != expected++) ? 1 : 0;
        }
    }
    producer.join();

    EXPECT_EQ(mismatches, 0u);
    EXPECT_EQ(expected, total);
    EXPECT_EQ(rb.size(), 0u);
}

constexpr std::uint64_t k_stress_items = 100'000'000;

}  // namespace

TEST(SpscRingBufferStress, TinyCapacity) { run_stress(4, 7, k_stress_items); }

TEST(SpscRingBufferStress, LargeCapacity) { run_stress(1 << 16, 4096, k_stress_items); }
