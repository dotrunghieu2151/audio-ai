#pragma once

#include <atomic>
// #include <immintrin.h>
#include <dth_common/export_dth_common.hpp>
#include <new>
#include <thread>

namespace dth::common {

template <typename Element, std::size_t Size>
class DTH_COMMON_EXPORT McmpQueue
{
  public:
    static constexpr size_t Capacity{Size + 1};
    McmpQueue()
        : _wCommitted{0},
          _wPending{0},
          _rCommitted{0},
          _rPending{0},
          _array{static_cast<Element*>(
              ::operator new[](sizeof(Element) * Capacity,
                               std::align_val_t(std::hardware_destructive_interference_size)))}
    {
    }

    // not thread-safe make sure destructor is called when no thread is working
    // on it
    virtual ~McmpQueue()
    {
        auto       currentReadIndex  = ToIndex(_rCommitted.load(std::memory_order_relaxed));
        const auto currentWriteIndex = ToIndex(_wCommitted.load(std::memory_order_relaxed));
        while (currentReadIndex != currentWriteIndex) {
            _array[currentReadIndex].~Element();
            currentReadIndex = ToIndex(Increment(currentReadIndex));
        }
        ::operator delete[](_array, std::align_val_t(std::hardware_destructive_interference_size));
    };

    McmpQueue(const McmpQueue&)            = delete;
    McmpQueue& operator=(const McmpQueue&) = delete;

    bool Push(const Element& item)
    {
        auto pendingW     = _wPending.load(std::memory_order_relaxed);
        auto nextPendingW = pendingW;
        // basically trying to grab the write slot by advancing the pendingW pointer
        do {
            const auto currentCommittedR = _rCommitted.load(std::memory_order_acquire);
            nextPendingW                 = pendingW + 1;
            if (ToIndex(nextPendingW) == ToIndex(currentCommittedR)) {
                return false;
            }
        } while (!_wPending.compare_exchange_weak(pendingW, nextPendingW, std::memory_order_release,
                                                  std::memory_order_relaxed));

        // Got hold of the slot, write to slot and try to advance the committedW
        // pointer
        _array[ToIndex(pendingW)] = item;
        // since _wCommitted must be advanced in order, we have to use compare-swap
        // instead of directly incrementing the atomics
        // _wCommitted will be compare against the current thread pendingW
        // so _wCommitted value will increment in-order
        // ex: thread A has pendingW of 1, thread C may finish first and set
        // committedW to 3 this will break the read sequence, so we must increment
        // _wCommitted in-order (A done -> C done)
        auto temp = pendingW;
        while (true) {
            // since compare_exchange_weak auto reassign newest value, we don't want
            // that here we have to advance only if the _wCommitted has reach our
            // pendingW
            temp = pendingW;
            if (_wCommitted.compare_exchange_weak(temp, nextPendingW, std::memory_order_release,
                                                  std::memory_order_relaxed)) {
                return true;
            }
            // cpu pause instruction
            // better performance than sleep(0)
            // _mm_pause();
            std::this_thread::yield();
        };
    }

    bool Push(Element&& item)
    {
        auto pendingW     = _wPending.load(std::memory_order_relaxed);
        auto nextPendingW = pendingW;
        // basically trying to grab the write slot by advancing the pendingW pointer
        do {
            const auto currentCommittedR = _rCommitted.load(std::memory_order_acquire);
            nextPendingW                 = pendingW + 1;
            if (ToIndex(nextPendingW) == ToIndex(currentCommittedR)) {
                return false;
            }
        } while (!_wPending.compare_exchange_weak(pendingW, nextPendingW, std::memory_order_release,
                                                  std::memory_order_relaxed));

        // Got hold of the slot, write to slot and try to advance the committedW
        // pointer
        _array[ToIndex(pendingW)] = std::move(item);
        // since _wCommitted must be advanced in order, we have to use compare-swap
        // instead of directly incrementing the atomics
        // _wCommitted will be compare against the current thread pendingW
        // so _wCommitted value will increment in-order
        // ex: thread A has pendingW of 1, thread C may finish first and set
        // committedW to 3 this will break the read sequence, so we must increment
        // _wCommitted in-order (A done -> C done)
        auto temp = pendingW;
        while (true) {
            // since compare_exchange_weak auto reassign newest value, we don't want
            // that here. We have to advance only if the _wCommitted has reach our
            // pendingW
            temp = pendingW;
            if (_wCommitted.compare_exchange_weak(temp, nextPendingW, std::memory_order_release,
                                                  std::memory_order_relaxed)) {
                return true;
            }
            // cpu pause instruction
            // better performance than sleep(0)
            // _mm_pause()
            std::this_thread::yield();
        };
    }

    bool Pop(Element& item)
    {
        auto pendingR    = _rPending.load(std::memory_order_relaxed);
        auto newPendingR = pendingR;
        do {
            const auto currentCommittedW = _wCommitted.load(std::memory_order_acquire);
            if (pendingR == currentCommittedW) {
                return false;
            }
            newPendingR = pendingR + 1;
        } while (!_rPending.compare_exchange_weak(pendingR, newPendingR, std::memory_order_release,
                                                  std::memory_order_relaxed));

        item = std::move(_array[ToIndex(pendingR)]);
        _array[ToIndex(pendingR)].~Element();
        auto temp = pendingR;
        while (true) {
            temp = pendingR;
            if (_rCommitted.compare_exchange_weak(temp, newPendingR, std::memory_order_release,
                                                  std::memory_order_relaxed)) {
                return true;
            }
            std::this_thread::yield();
        }
    }

    inline size_t GetSize() const
    {
        return _wCommitted.load(std::memory_order_relaxed) -
               _rCommitted.load(std::memory_order_relaxed);
    }

    inline bool IsEmpty() const { return GetSize() == 0; }

    inline bool IsFull() const { return GetSize() == Size; }

  private:
    inline size_t Increment(size_t idx) const { return idx + 1; }
    inline size_t ToIndex(size_t readOrWriteSequence) const
    {
        return readOrWriteSequence % Capacity;
    }

    Element* _array;
    alignas(std::hardware_destructive_interference_size) std::atomic<size_t> _wCommitted;
    alignas(std::hardware_destructive_interference_size) std::atomic<size_t> _wPending;

    alignas(std::hardware_destructive_interference_size)
        std::atomic<size_t> _rCommitted;  // head(output) index
    alignas(std::hardware_destructive_interference_size)
        std::atomic<size_t> _rPending;  // head(output) index
};
}  // namespace dth::common
