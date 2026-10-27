#include "dth/common/mpmc_queue.hpp"

#include <gtest/gtest.h>

#include <set>

// TestType tracks correct usage of constructors and destructors
struct TestType
{
    static inline std::set<const TestType*> constructed{};
    TestType() noexcept
    {
        EXPECT_EQ(constructed.count(this), 0);
        constructed.insert(this);
    };
    TestType(const TestType& other) noexcept
    {
        EXPECT_EQ(constructed.count(this), 0);
        EXPECT_EQ(constructed.count(&other), 1);
        constructed.insert(this);
    };
    TestType(TestType&& other) noexcept
    {
        EXPECT_EQ(constructed.count(this), 0);
        EXPECT_EQ(constructed.count(&other), 1);
        constructed.insert(this);
    };
    TestType& operator=(const TestType& other) noexcept
    {
        EXPECT_EQ(constructed.count(&other), 1);
        constructed.insert(this);
        return *this;
    };
    TestType& operator=(TestType&& other) noexcept
    {
        EXPECT_EQ(constructed.count(&other), 1);
        constructed.insert(this);
        return *this;
    }
    ~TestType() noexcept
    {
        EXPECT_EQ(constructed.count(this), 1);
        constructed.erase(this);
    };
    // To verify that alignment and padding calculations are handled correctly
    char data[129];
};

class McmpQueueTest : public ::testing::Test
{
  public:
  protected:
    void SetUp() override {};
};

TEST_F(McmpQueueTest, SingleThread)
{
    dth::common::McmpQueue<TestType, 11> q{};
    EXPECT_EQ(q.GetSize(), 0);
    EXPECT_EQ(q.IsEmpty(), true);
    for (int i = 0; i < 10; i++) {
        q.Push(TestType{});
    }
    EXPECT_EQ(q.GetSize(), 10);
    EXPECT_EQ(!q.IsEmpty(), true);
    EXPECT_EQ(TestType::constructed.size(), 10);

    TestType t;
    q.Pop(t);
    EXPECT_EQ(q.GetSize(), 9);
    EXPECT_EQ(!q.IsEmpty(), true);
    EXPECT_EQ(TestType::constructed.size(), 10);

    q.Pop(t);
    q.Push(TestType{});
    EXPECT_EQ(q.GetSize(), 9);
    EXPECT_EQ(!q.IsEmpty(), true);

    EXPECT_EQ(TestType::constructed.size(), 10);
}

TEST_F(McmpQueueTest, MultithreadFuzzTest)
{
    const uint64_t                               numOps     = 5000;
    const uint64_t                               numThreads = 8;
    dth::common::McmpQueue<uint64_t, numThreads> q{};
    std::atomic<bool>                            flag(false);
    std::vector<std::thread>                     threads;
    std::atomic<uint64_t>                        sum(0);

    // write
    threads.reserve(numThreads);
    for (uint64_t i = 0; i < numThreads; ++i) {
        threads.emplace_back([&, i] {
            while (!flag) {
            }
            for (auto j = i; j < numOps; j += numThreads) {
                while (!q.Push(j)) {
                    std::this_thread::yield();
                }
            }
        });
    }

    // read
    for (uint64_t i = 0; i < numThreads; ++i) {
        threads.emplace_back([&, i] {
            while (!flag) {
            }
            uint64_t threadSum = 0;
            for (auto j = i; j < numOps; j += numThreads) {
                uint64_t v;
                while (!q.Pop(v)) {
                    std::this_thread::yield();
                }
                threadSum += v;
            }
            sum += threadSum;
        });
    }
    flag = true;
    for (auto& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(sum, numOps * (numOps - 1) / 2);
}
