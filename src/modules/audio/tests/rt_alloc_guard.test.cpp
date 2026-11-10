#include "rt_alloc_guard.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <memory>
#include <new>
#include <thread>
#include <vector>

using dth::audio::test::rt_allocation_count;
using dth::audio::test::rt_guard_covers_malloc;
using dth::audio::test::RtScope;

// Direct calls to the replaceable functions cannot be elided by the optimizer (new-expressions can).
TEST(RtAllocGuard, IgnoresAllocationOutsideScope)
{
    auto const before = rt_allocation_count();
    ::operator delete(::operator new(16));
    EXPECT_EQ(rt_allocation_count(), before);
}

TEST(RtAllocGuard, CatchesAllocationAndDeallocationInsideScope)
{
    auto const before = rt_allocation_count();
    void*      p      = nullptr;
    {
        RtScope rt;
        p = ::operator new(16);
    }
    EXPECT_EQ(rt_allocation_count(), before + 1);
    {
        RtScope rt;
        ::operator delete(p);
    }
    EXPECT_EQ(rt_allocation_count(), before + 2);
}

TEST(RtAllocGuard, CatchesEveryAllocationVariant)
{
    auto expect_counted = [](auto&& alloc_and_free) {
        auto const before = rt_allocation_count();
        {
            RtScope rt;
            alloc_and_free();
        }
        EXPECT_EQ(rt_allocation_count(), before + 2);
    };
    expect_counted([] { ::operator delete[](::operator new[](8)); });
    expect_counted([] { ::operator delete(::operator new(8, std::nothrow), std::nothrow); });
    expect_counted([] {
        ::operator delete(::operator new(8, std::align_val_t{64}), std::align_val_t{64});
    });
    expect_counted([] {
        ::operator delete[](::operator new[](8, std::align_val_t{64}, std::nothrow),
                            std::align_val_t{64}, std::nothrow);
    });
}

TEST(RtAllocGuard, CatchesStandardContainerAllocation)
{
    auto const before = rt_allocation_count();
    {
        RtScope rt;
        auto    v = std::make_unique<std::vector<int>>(1000, 1);
        EXPECT_EQ(v->size(), 1000u);
    }
    EXPECT_GT(rt_allocation_count(), before);
}

TEST(RtAllocGuard, CatchesAllocationOnAnotherThread)
{
    auto const before = rt_allocation_count();
    std::thread{[] {
        RtScope rt;
        ::operator delete(::operator new(16));
    }}.join();
    EXPECT_EQ(rt_allocation_count(), before + 2);
}

TEST(RtAllocGuard, ScopeIsPerThread)
{
    std::atomic<int> phase{0};
    std::thread      other{[&phase] {
        while (phase.load() != 1) {
            std::this_thread::yield();
        }
        ::operator delete(::operator new(16));
        phase.store(2);
    }};

    auto const before = rt_allocation_count();
    {
        RtScope rt;
        phase.store(1);
        while (phase.load() != 2) {
            std::this_thread::yield();
        }
    }
    EXPECT_EQ(rt_allocation_count(), before);
    other.join();
}

TEST(RtAllocGuard, NestedScopeRestoresOuter)
{
    auto const before = rt_allocation_count();
    {
        RtScope outer;
        {
            RtScope inner;
        }
        ::operator delete(::operator new(16));
    }
    EXPECT_EQ(rt_allocation_count(), before + 2);
    ::operator delete(::operator new(16));
    EXPECT_EQ(rt_allocation_count(), before + 2);
}

TEST(RtAllocGuard, CatchesCMallocInSanitizerBuilds)
{
    if (!rt_guard_covers_malloc()) {
        GTEST_SKIP() << "C malloc is only observed in sanitizer builds";
    }
    auto const before = rt_allocation_count();
    void*      p      = std::malloc(16);
    {
        RtScope rt;
        p = std::realloc(p, 4096);  // counts as allocate + free
        std::free(std::calloc(4, 4));
    }
    std::free(p);
    EXPECT_GE(rt_allocation_count(), before + 3);
}
