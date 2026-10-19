#include "harken/core/StableRowStorage.h"

#include <exception>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace harken::detail {
namespace {

thread_local bool failNextReservationForTests = false;

bool RoundUp(std::size_t bytes, std::size_t alignment, std::size_t* out) noexcept {
    if (alignment == 0 || bytes > SIZE_MAX - (alignment - 1)) return false;
    *out = ((bytes + alignment - 1) / alignment) * alignment;
    return true;
}

} // namespace

StablePageReservation::~StablePageReservation() {
    if (address_ == nullptr) return;
    if (memguard::IsAudioTagged()) std::terminate();
#if defined(_WIN32)
    const bool released = VirtualFree(address_, 0, MEM_RELEASE) != 0;
#else
    const bool released = munmap(address_, reservedBytes_) == 0;
#endif
    if (!released) std::terminate();
}

bool StablePageReservation::Reserve(std::size_t bytes, std::size_t alignment) noexcept {
    if (memguard::IsAudioTagged() || address_ != nullptr) return false;
    if (bytes == 0) return true;
    std::size_t pageBytes = 0, granularity = 0, reservedBytes = 0;
#if defined(_WIN32)
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    pageBytes = info.dwPageSize;
    granularity = info.dwAllocationGranularity;
#else
    const auto size = sysconf(_SC_PAGESIZE);
    if (size <= 0) return false;
    pageBytes = static_cast<std::size_t>(size);
    granularity = pageBytes;
#endif
    if (alignment == 0 || pageBytes % alignment != 0 ||
        !RoundUp(bytes, granularity, &reservedBytes))
        return false;
    if (std::exchange(failNextReservationForTests, false)) return false;
#if defined(_WIN32)
    void* address = VirtualAlloc(nullptr, reservedBytes, MEM_RESERVE, PAGE_NOACCESS);
    if (address == nullptr) return false;
#else
    void* address = mmap(nullptr, reservedBytes, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (address == MAP_FAILED) return false;
#endif
    address_ = address;
    pageBytes_ = pageBytes;
    reservedBytes_ = reservedBytes;
    return true;
}

void StablePageReservation::FailNextReservationForTests() noexcept {
    failNextReservationForTests = true;
}

bool StablePageReservation::CommitThrough(std::size_t bytes) noexcept {
    if (memguard::IsAudioTagged() || bytes > reservedBytes_) return false;
    if (bytes <= committedBytes_) return true;
    std::size_t target = 0;
    if (!RoundUp(bytes, pageBytes_, &target) || target > reservedBytes_) return false;
    // Each successful page stays charged, including an unconstructed failure prefix.
    while (committedBytes_ < target) {
        if (remainingPagesForTests_ == 0) return false;
        auto* page = static_cast<std::byte*>(address_) + committedBytes_;
#if defined(_WIN32)
        if (VirtualAlloc(page, pageBytes_, MEM_COMMIT, PAGE_READWRITE) == nullptr)
            return false;
#else
        if (mprotect(page, pageBytes_, PROT_READ | PROT_WRITE) != 0) return false;
#endif
        *reinterpret_cast<volatile unsigned char*>(page) = 0;
        committedBytes_ += pageBytes_;
        if (remainingPagesForTests_ != SIZE_MAX) --remainingPagesForTests_;
    }
    return true;
}

void StablePageReservation::Swap(StablePageReservation& other) noexcept {
    std::swap(address_, other.address_);
    std::swap(reservedBytes_, other.reservedBytes_);
    std::swap(committedBytes_, other.committedBytes_);
    std::swap(pageBytes_, other.pageBytes_);
    std::swap(remainingPagesForTests_, other.remainingPagesForTests_);
}

} // namespace harken::detail
