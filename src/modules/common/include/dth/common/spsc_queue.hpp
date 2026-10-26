#pragma once

#include <atomic>
#include <dth_common/export_dth_common.hpp>
#include <new>
#include <thread>

namespace dth::common {

template <typename Element, std::size_t Size>
class DTH_COMMON_EXPORT SpscQueue
{
  public:
    static constexpr size_t Capacity{Size + 1};
    SpscQueue()
        : _tail(0),
          _head(0),
          _array{static_cast<Element*>(
              ::operator new[](sizeof(Element) * Capacity,
                               std::align_val_t(std::hardware_destructive_interference_size)))}
    {
    }

    // not thread-safe make sure destructor is called when no thread is working on
    // it
    virtual ~SpscQueue()
    {
        auto currentHead = ToIndex(_head.load(std::memory_order_relaxed));
        auto currentTail = ToIndex(_tail.load(std::memory_order_relaxed));
        while (currentHead != currentTail) {
            _array[currentHead].~Element();
            currentHead = ToIndex(Increment(currentHead));
        }
        ::operator delete[](_array, std::align_val_t(std::hardware_destructive_interference_size));
    };

    SpscQueue(const SpscQueue&)            = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    bool Push(const Element& item)
    {
        const auto currentTail = _tail.load(std::memory_order_relaxed);
        const auto nextTail    = Increment(currentTail);
        if (ToIndex(nextTail) != ToIndex(_head.load(std::memory_order_acquire))) {
            new (_array + ToIndex(currentTail)) Element{item};
            _tail.store(nextTail, std::memory_order_release);
            return true;
        }

        return false;
    }

    bool Push(Element&& item)
    {
        const auto currentTail = _tail.load(std::memory_order_relaxed);
        const auto nextTail    = Increment(currentTail);
        if (ToIndex(nextTail) != ToIndex(_head.load(std::memory_order_acquire))) {
            new (_array + ToIndex(currentTail)) Element{std::move(item)};
            _tail.store(nextTail, std::memory_order_release);
            return true;
        }

        return false;
    }
    bool Pop(Element& item)
    {
        const auto   currentHead = _head.load(std::memory_order_relaxed);
        const auto   nextHead    = Increment(currentHead);
        const size_t index{ToIndex(currentHead)};
        if (index == ToIndex(_tail.load(std::memory_order_acquire))) {
            return false;  // empty queue
        }

        item = std::move(_array[index]);
        _array[index].~Element();

        _head.store(nextHead, std::memory_order_release);
        return true;
    }

    inline size_t GetSize() const
    {
        return _tail.load(std::memory_order_relaxed) - _head.load(std::memory_order_relaxed);
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
    alignas(std::hardware_destructive_interference_size) std::atomic<size_t> _tail;
    alignas(std::hardware_destructive_interference_size)
        std::atomic<size_t> _head;  // head(output) index
};
}  // namespace dth::common
