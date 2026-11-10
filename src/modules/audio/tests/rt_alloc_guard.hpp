#pragma once

#include <cstddef>

namespace dth::audio::test {

// Marks the current thread as real-time for the scope's lifetime. Heap operations made on a thread
// inside an RtScope are recorded:
// - always: global operator new/delete (rt_alloc_guard.cpp replaces them in the linking binary);
// - sanitizer builds (ASan/TSan): also C malloc/calloc/realloc/free, via sanitizer malloc hooks.
class RtScope
{
  public:
    RtScope() noexcept;
    ~RtScope();

    RtScope(const RtScope&)            = delete;
    RtScope& operator=(const RtScope&) = delete;

  private:
    bool _previous;
};

// Allocations + deallocations made inside any RtScope, on any thread, since process start.
// Tests snapshot it before the real-time work and expect it unchanged afterwards.
[[nodiscard]]
std::size_t rt_allocation_count() noexcept;

// True when C malloc/free are recorded too (sanitizer builds), not only operator new/delete.
[[nodiscard]]
bool rt_guard_covers_malloc() noexcept;

}  // namespace dth::audio::test
