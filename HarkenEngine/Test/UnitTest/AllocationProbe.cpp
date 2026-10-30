#include "AllocationProbe.h"

#include <cstdlib>
#include <new>
#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace allocation_probe {
thread_local bool enabled = false;
thread_local std::size_t count = 0;
thread_local std::size_t bytes = 0;
thread_local std::size_t failAt = 0;
thread_local bool (*countIf)(void*) noexcept = nullptr;
thread_local void* countContext = nullptr;

void* Allocate(std::size_t size, std::size_t alignment = 0) {
    if (enabled && (countIf == nullptr || countIf(countContext))) {
        ++count;
        bytes += size;
        if (failAt != 0 && count == failAt) {
            failAt = 0;
            throw std::bad_alloc{};
        }
    }
    size = size == 0 ? 1 : size;
    for (;;) {
        void* p = nullptr;
        if (alignment == 0) {
            p = std::malloc(size);
        } else {
#if defined(_MSC_VER)
            p = _aligned_malloc(size, alignment);
#else
            if (posix_memalign(&p, alignment, size) != 0) p = nullptr;
#endif
        }
        if (p != nullptr) return p;
        const auto handler = std::get_new_handler();
        if (handler == nullptr) throw std::bad_alloc{};
        handler();
    }
}

void FreeAligned(void* p) noexcept {
#if defined(_MSC_VER)
    _aligned_free(p);
#else
    std::free(p);
#endif
}
}

void* operator new(std::size_t n) { return allocation_probe::Allocate(n); }
void* operator new[](std::size_t n) { return allocation_probe::Allocate(n); }
void* operator new(std::size_t n, std::align_val_t a) { return allocation_probe::Allocate(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a) { return allocation_probe::Allocate(n, static_cast<std::size_t>(a)); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { try { return ::operator new(n); } catch (...) { return nullptr; } }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { try { return ::operator new[](n); } catch (...) { return nullptr; } }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { try { return ::operator new(n, a); } catch (...) { return nullptr; } }
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { try { return ::operator new[](n, a); } catch (...) { return nullptr; } }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { allocation_probe::FreeAligned(p); }
void operator delete[](void* p, std::align_val_t) noexcept { allocation_probe::FreeAligned(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { allocation_probe::FreeAligned(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { allocation_probe::FreeAligned(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { allocation_probe::FreeAligned(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { allocation_probe::FreeAligned(p); }
