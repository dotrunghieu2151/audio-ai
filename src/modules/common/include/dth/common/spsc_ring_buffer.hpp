#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <type_traits>

namespace dth::common {

#ifdef __cpp_lib_hardware_interference_size
inline constexpr std::size_t k_spsc_cache_line = std::hardware_destructive_interference_size;
#else
inline constexpr std::size_t k_spsc_cache_line = 128;  // Apple Silicon line size
#endif

#if defined(__SANITIZE_THREAD__)
#define DTH_SPSC_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define DTH_SPSC_TSAN 1
#endif
#endif

// Bounded, lock-free, wait-free single-producer / single-consumer ring buffer for trivially
// copyable elements, with bulk span-based read/write.
//
// Contract:
//   - At most one thread calls write() at a time (the producer) and at most one thread calls
//     read() at a time (the consumer). Producer and consumer may run concurrently.
//   - Handing the producer (or consumer) role to another thread requires a happens-before edge
//     between the old and new thread (e.g. stopping one audio callback before starting another).
//   - size(), free_space() and capacity() may be called from any thread; size()/free_space()
//     are snapshots and may be stale by the time they return.
//   - Construction allocates once; write()/read() never allocate, lock or make syscalls.
//   - Debug builds assert that write() (and read()) are never entered concurrently.
template <typename T>
class SpscRingBuffer final
{
    static_assert(std::is_trivially_copyable_v<T>, "SpscRingBuffer requires trivially copyable T");
    static_assert(std::atomic<std::size_t>::is_always_lock_free);

  public:
    // Capacity is min_capacity rounded up to a power of two (minimum 1).
    explicit SpscRingBuffer(std::size_t min_capacity)
        : _capacity(std::bit_ceil(std::max<std::size_t>(min_capacity, 1))),
          _mask(_capacity - 1),
          _buffer(std::make_unique_for_overwrite<T[]>(_capacity))
    {
        assert(min_capacity <= (std::numeric_limits<std::size_t>::max() / 2) + 1);
    }

    SpscRingBuffer(const SpscRingBuffer&)            = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;
    SpscRingBuffer(SpscRingBuffer&&)                 = delete;
    SpscRingBuffer& operator=(SpscRingBuffer&&)      = delete;
    ~SpscRingBuffer()                                = default;

    // Producer only. Writes min(src.size(), free space) elements; returns the count written.
    std::size_t write(std::span<const T> src) noexcept
    {
#ifndef NDEBUG
        const bool was_busy = _producer_busy.exchange(true, std::memory_order_acquire);
        assert(!was_busy && "SpscRingBuffer::write called concurrently");
#endif
        const std::size_t head = _head.load(std::memory_order_relaxed);
        std::size_t       free = _capacity - (head - _cached_tail);
        if (free < src.size()) {
            _cached_tail = _tail.load(std::memory_order_acquire);
            free         = _capacity - (head - _cached_tail);
        }

        const std::size_t count = std::min(src.size(), free);
        if (count != 0) {
            const std::size_t index = head & _mask;
            const std::size_t first = std::min(count, _capacity - index);
            copy(_buffer.get() + index, src.data(), first);
            if (count > first) {
                copy(_buffer.get(), src.data() + first, count - first);
            }
            _head.store(head + count, std::memory_order_release);
        }
#ifndef NDEBUG
        _producer_busy.store(false, std::memory_order_release);
#endif
        return count;
    }

    // Consumer only. Reads min(dst.size(), available) elements; returns the count read.
    std::size_t read(std::span<T> dst) noexcept
    {
#ifndef NDEBUG
        const bool was_busy = _consumer_busy.exchange(true, std::memory_order_acquire);
        assert(!was_busy && "SpscRingBuffer::read called concurrently");
#endif
        const std::size_t tail      = _tail.load(std::memory_order_relaxed);
        std::size_t       available = _cached_head - tail;
        if (available < dst.size()) {
            _cached_head = _head.load(std::memory_order_acquire);
            available    = _cached_head - tail;
        }

        const std::size_t count = std::min(dst.size(), available);
        if (count != 0) {
            const std::size_t index = tail & _mask;
            const std::size_t first = std::min(count, _capacity - index);
            copy(dst.data(), _buffer.get() + index, first);
            if (count > first) {
                copy(dst.data() + first, _buffer.get(), count - first);
            }
            _tail.store(tail + count, std::memory_order_release);
        }
#ifndef NDEBUG
        _consumer_busy.store(false, std::memory_order_release);
#endif
        return count;
    }

    // Snapshot of the number of readable elements. Exact when called from the producer or
    // consumer thread with the other side idle.
    [[nodiscard]] std::size_t size() const noexcept
    {
        // Load tail first: tail never passes head, so head - tail cannot underflow.
        const std::size_t tail = _tail.load(std::memory_order_acquire);
        const std::size_t head = _head.load(std::memory_order_acquire);
        return std::min(head - tail, _capacity);
    }

    [[nodiscard]] std::size_t free_space() const noexcept { return _capacity - size(); }

    [[nodiscard]] std::size_t capacity() const noexcept { return _capacity; }

  private:
    static void copy(T* dst, const T* src, std::size_t count) noexcept
    {
#ifdef DTH_SPSC_TSAN
        // TSan (at least Apple's) does not instrument memcpy, so buffer races would go unseen.
        for (std::size_t i = 0; i < count; ++i) {
            dst[i] = src[i];
        }
#else
        std::memcpy(dst, src, count * sizeof(T));
#endif
    }

    // Read-only after construction; on its own line so neither side's writes invalidate it.
    alignas(k_spsc_cache_line) const std::size_t _capacity;
    const std::size_t          _mask;
    const std::unique_ptr<T[]> _buffer;

    // Producer-owned line: head is written by the producer, cached_tail is producer-private.
    alignas(k_spsc_cache_line) std::atomic<std::size_t> _head{0};
    std::size_t _cached_tail{0};
#ifndef NDEBUG
    std::atomic<bool> _producer_busy{false};
#endif

    // Consumer-owned line: tail is written by the consumer, cached_head is consumer-private.
    alignas(k_spsc_cache_line) std::atomic<std::size_t> _tail{0};
    std::size_t _cached_head{0};
#ifndef NDEBUG
    std::atomic<bool> _consumer_busy{false};
#endif
};

}  // namespace dth::common
