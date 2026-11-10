#include "rt_alloc_guard.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <new>

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define DTH_RT_GUARD_MALLOC_HOOKS 1
#endif
#endif
#if !defined(DTH_RT_GUARD_MALLOC_HOOKS) && \
    (defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__))
#define DTH_RT_GUARD_MALLOC_HOOKS 1
#endif

#if defined(DTH_RT_GUARD_MALLOC_HOOKS)
#include <pthread.h>
#include <sanitizer/allocator_interface.h>
#endif

namespace dth::audio::test {
namespace {

constinit std::atomic<std::size_t> g_rt_allocations{0};

#if defined(DTH_RT_GUARD_MALLOC_HOOKS)
// Sanitizer builds: the sanitizer runtime reports every heap operation (malloc, calloc, realloc,
// free, operator new/delete) through these hooks, so C libraries (e.g. speexdsp) are covered too.
//
// The per-thread flag lives in a pthread key, not `thread_local`: on macOS the first access to
// a thread_local calls malloc, which would re-enter the hook and recurse.
pthread_key_t     g_rt_key;
std::atomic<bool> g_rt_key_ready{false};

bool in_rt_scope() noexcept
{
    return g_rt_key_ready.load(std::memory_order_acquire) &&
           pthread_getspecific(g_rt_key) != nullptr;
}

void set_in_rt_scope(bool value) noexcept
{
    pthread_setspecific(g_rt_key, value ? &g_rt_key : nullptr);
}

void on_malloc(const volatile void* /*ptr*/, std::size_t /*size*/)
{
    if (in_rt_scope()) {
        g_rt_allocations.fetch_add(1, std::memory_order_relaxed);
    }
}

void on_free(const volatile void* ptr)
{
    if (ptr != nullptr && in_rt_scope()) {
        g_rt_allocations.fetch_add(1, std::memory_order_relaxed);
    }
}

const bool g_hooks_installed = [] {
    pthread_key_create(&g_rt_key, nullptr);
    g_rt_key_ready.store(true, std::memory_order_release);
    return __sanitizer_install_malloc_and_free_hooks(on_malloc, on_free) != 0;
}();

// The hooks already see the malloc/free that operator new/delete forward to.
void record() noexcept {}
#else
// Non-sanitizer builds: only global operator new/delete are observed.
constinit thread_local bool t_in_rt_scope = false;

bool in_rt_scope() noexcept
{
    return t_in_rt_scope;
}

void set_in_rt_scope(bool value) noexcept
{
    t_in_rt_scope = value;
}

void record() noexcept
{
    if (t_in_rt_scope) {
        g_rt_allocations.fetch_add(1, std::memory_order_relaxed);
    }
}
#endif

void* allocate(std::size_t size)
{
    record();
    if (void* p = std::malloc(std::max<std::size_t>(size, 1))) {
        return p;
    }
    throw std::bad_alloc{};
}

// Built on plain malloc (original pointer stored just before the aligned block) instead of
// aligned_alloc/posix_memalign: TSan does not report those to malloc hooks.
void* allocate_aligned(std::size_t size, std::align_val_t alignment)
{
    record();
    auto const align = std::max(static_cast<std::size_t>(alignment), sizeof(void*));
    void* const raw  = std::malloc(size + align + sizeof(void*));
    if (raw == nullptr) {
        throw std::bad_alloc{};
    }
    auto const address = (reinterpret_cast<std::uintptr_t>(raw) + sizeof(void*) + align - 1) &
                         ~(static_cast<std::uintptr_t>(align) - 1);
    auto* const aligned = reinterpret_cast<void**>(address);
    aligned[-1]         = raw;
    return aligned;
}

void deallocate(void* p) noexcept
{
    if (p != nullptr) {
        record();
        std::free(p);
    }
}

void deallocate_aligned(void* p) noexcept
{
    if (p != nullptr) {
        record();
        std::free(static_cast<void**>(p)[-1]);
    }
}

}  // namespace

RtScope::RtScope() noexcept : _previous{in_rt_scope()}
{
    set_in_rt_scope(true);
}

RtScope::~RtScope()
{
    set_in_rt_scope(_previous);
}

bool rt_guard_covers_malloc() noexcept
{
#if defined(DTH_RT_GUARD_MALLOC_HOOKS)
    return g_hooks_installed;
#else
    return false;
#endif
}

std::size_t rt_allocation_count() noexcept
{
    return g_rt_allocations.load(std::memory_order_relaxed);
}

}  // namespace dth::audio::test

// Replacements for every global allocation function of the test binary.
using dth::audio::test::allocate;
using dth::audio::test::allocate_aligned;
using dth::audio::test::deallocate;
using dth::audio::test::deallocate_aligned;

void* operator new(std::size_t size)
{
    return allocate(size);
}
void* operator new[](std::size_t size)
{
    return allocate(size);
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    try {
        return allocate(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    try {
        return allocate(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new(std::size_t size, std::align_val_t alignment)
{
    return allocate_aligned(size, alignment);
}
void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return allocate_aligned(size, alignment);
}
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    try {
        return allocate_aligned(size, alignment);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    try {
        return allocate_aligned(size, alignment);
    } catch (...) {
        return nullptr;
    }
}

void operator delete(void* p) noexcept
{
    deallocate(p);
}
void operator delete[](void* p) noexcept
{
    deallocate(p);
}
void operator delete(void* p, const std::nothrow_t&) noexcept
{
    deallocate(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept
{
    deallocate(p);
}
void operator delete(void* p, std::size_t) noexcept
{
    deallocate(p);
}
void operator delete[](void* p, std::size_t) noexcept
{
    deallocate(p);
}
void operator delete(void* p, std::align_val_t) noexcept
{
    deallocate_aligned(p);
}
void operator delete[](void* p, std::align_val_t) noexcept
{
    deallocate_aligned(p);
}
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept
{
    deallocate_aligned(p);
}
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept
{
    deallocate_aligned(p);
}
void operator delete(void* p, std::size_t, std::align_val_t) noexcept
{
    deallocate_aligned(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept
{
    deallocate_aligned(p);
}
