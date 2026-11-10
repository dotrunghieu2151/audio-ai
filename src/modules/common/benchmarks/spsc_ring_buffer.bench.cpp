#include "dth/common/spsc_ring_buffer.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <deque>
#include <mutex>
#include <span>
#include <vector>

namespace {

constexpr std::size_t k_capacity = 16384;

// Baseline: same bounded read/write API, backed by std::deque under a std::mutex.
class MutexDequeBuffer
{
  public:
    explicit MutexDequeBuffer(std::size_t capacity)
        : _capacity(capacity)
    {
    }

    std::size_t write(std::span<const float> src)
    {
        std::scoped_lock  lock{_mutex};
        const std::size_t n = std::min(src.size(), _capacity - _queue.size());
        _queue.insert(_queue.end(), src.begin(), src.begin() + static_cast<std::ptrdiff_t>(n));
        return n;
    }

    std::size_t read(std::span<float> dst)
    {
        std::scoped_lock  lock{_mutex};
        const std::size_t n = std::min(dst.size(), _queue.size());
        std::copy_n(_queue.begin(), n, dst.begin());
        _queue.erase(_queue.begin(), _queue.begin() + static_cast<std::ptrdiff_t>(n));
        return n;
    }

  private:
    std::size_t       _capacity;
    std::mutex        _mutex;
    std::deque<float> _queue;
};

// One thread writes a chunk then reads it back.
template <typename Buffer>
void bm_round_trip(benchmark::State& state)
{
    const auto         chunk = static_cast<std::size_t>(state.range(0));
    Buffer             buffer{k_capacity};
    std::vector<float> src(chunk, 1.0f);
    std::vector<float> dst(chunk);
    for (auto _ : state) {
        benchmark::DoNotOptimize(buffer.write(src));
        benchmark::DoNotOptimize(buffer.read(dst));
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
    state.SetBytesProcessed(state.iterations() * state.range(0) *
                            static_cast<std::int64_t>(sizeof(float)));
}

// Thread 0 streams chunks in, thread 1 streams chunks out; both spin until each chunk is done.
// Every thread runs the same iteration count, so the buffer is drained when a run ends.
template <typename Buffer>
void bm_two_thread(benchmark::State& state)
{
    static Buffer      buffer{k_capacity};
    const auto         chunk = static_cast<std::size_t>(state.range(0));
    std::vector<float> data(chunk, 1.0f);
    const bool         producer = state.thread_index() == 0;
    for (auto _ : state) {
        std::size_t done = 0;
        while (done < chunk) {
            done += producer ? buffer.write(std::span<const float>{data}.subspan(done))
                             : buffer.read(std::span<float>{data}.subspan(done));
        }
    }
    if (producer) {
        state.SetItemsProcessed(state.iterations() * state.range(0));
        state.SetBytesProcessed(state.iterations() * state.range(0) *
                                static_cast<std::int64_t>(sizeof(float)));
    }
}

using RingBuffer = dth::common::SpscRingBuffer<float>;

}  // namespace

BENCHMARK(bm_round_trip<RingBuffer>)->Arg(1)->Arg(64)->Arg(512)->Arg(4096);
BENCHMARK(bm_round_trip<MutexDequeBuffer>)->Arg(1)->Arg(64)->Arg(512)->Arg(4096);
BENCHMARK(bm_two_thread<RingBuffer>)->Arg(1)->Arg(64)->Arg(512)->Arg(4096)->Threads(2)->UseRealTime();
BENCHMARK(bm_two_thread<MutexDequeBuffer>)
    ->Arg(1)
    ->Arg(64)
    ->Arg(512)
    ->Arg(4096)
    ->Threads(2)
    ->UseRealTime();

BENCHMARK_MAIN();
