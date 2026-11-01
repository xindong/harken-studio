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

std::size_t StablePageReservation::SystemPageBytes() noexcept {
#if defined(_WIN32)
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    return info.dwPageSize;
#else
    const auto size = sysconf(_SC_PAGESIZE);
    return size > 0 ? static_cast<std::size_t>(size) : 0;
#endif
}

bool StablePageReservation::Reserve(std::size_t bytes, std::size_t alignment, bool sparse) noexcept {
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
    std::unique_ptr<std::uint64_t[]> committedPages;
    std::size_t committedPageWords = 0;
    if (sparse) {
        try {
            const auto count = reservedBytes / pageBytes;
            committedPageWords = count / 64u + (count % 64u != 0);
            committedPages = std::make_unique<std::uint64_t[]>(committedPageWords);
        } catch (...) { return false; }
    }
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
    committedPages_.swap(committedPages);
    committedPageWords_ = committedPageWords;
    return true;
}

void StablePageReservation::FailNextReservationForTests() noexcept {
    failNextReservationForTests = true;
}

bool StablePageReservation::CommitThrough(std::size_t bytes) noexcept {
    if (memguard::IsAudioTagged() || bytes > reservedBytes_) return false;
    if (committedPages_ != nullptr) return CommitRange(0, bytes);
    if (bytes <= committedBytes_) return true;
    std::size_t target = 0;
    if (!RoundUp(bytes, pageBytes_, &target) || target > reservedBytes_) return false;
    const auto requested = (target - committedBytes_) / pageBytes_;
    const auto committed = CommitPages(committedBytes_, requested);
    // Each successful page stays charged, including an unconstructed failure prefix.
    committedBytes_ += committed * pageBytes_;
    return committed == requested;
}

std::size_t StablePageReservation::CommitPages(std::size_t offset, std::size_t count) noexcept {
    const auto permitted = std::min(count, remainingPagesForTests_);
    if (permitted == 0) return 0;
#if defined(_WIN32)
    auto* first = static_cast<std::byte*>(address_) + offset;
    if (permitted > 1 &&
        VirtualAlloc(first, permitted * pageBytes_, MEM_COMMIT, PAGE_READWRITE) != nullptr) {
        // Complete physical preparation before any caller publishes a row or backing hold.
        for (std::size_t index = 0; index < permitted; ++index)
            *reinterpret_cast<volatile unsigned char*>(first + index * pageBytes_) = 0;
        if (remainingPagesForTests_ != SIZE_MAX) remainingPagesForTests_ -= permitted;
        return permitted;
    }
#endif
    // Preserve the original successful prefix if a larger Windows commitment is refused.
    std::size_t committed = 0;
    while (committed < permitted && CommitPage(offset + committed * pageBytes_)) ++committed;
    return committed;
}

bool StablePageReservation::CommitPage(std::size_t offset) noexcept {
    if (remainingPagesForTests_ == 0) return false;
    auto* page = static_cast<std::byte*>(address_) + offset;
#if defined(_WIN32)
    if (VirtualAlloc(page, pageBytes_, MEM_COMMIT, PAGE_READWRITE) == nullptr) return false;
#else
    if (mprotect(page, pageBytes_, PROT_READ | PROT_WRITE) != 0) return false;
#endif
    *reinterpret_cast<volatile unsigned char*>(page) = 0;
    if (remainingPagesForTests_ != SIZE_MAX) --remainingPagesForTests_;
    return true;
}

bool StablePageReservation::CommitRange(std::size_t offset, std::size_t bytes) noexcept {
    if (memguard::IsAudioTagged() || offset > reservedBytes_ || bytes > reservedBytes_ - offset)
        return false;
    if (bytes == 0) return true;
    if (committedPages_ == nullptr) return false;
    const auto first = offset / pageBytes_;
    const auto last = (offset + bytes - 1u) / pageBytes_;
    const auto isCommitted = [&](std::size_t index) noexcept {
        return (committedPages_[index / 64u] & (std::uint64_t{1} << (index % 64u))) != 0;
    };
    for (auto index = first; index <= last;) {
        if (isCommitted(index)) { ++index; continue; }
        auto end = index + 1u;
        while (end <= last && !isCommitted(end)) ++end;
        const auto requested = end - index;
        const auto committed = CommitPages(index * pageBytes_, requested);
        for (std::size_t page = index; page < index + committed; ++page)
            committedPages_[page / 64u] |= std::uint64_t{1} << (page % 64u);
        committedBytes_ += committed * pageBytes_;
        if (committed != requested) return false;
        index = end;
    }
    return true;
}

void StablePageReservation::Swap(StablePageReservation& other) noexcept {
    std::swap(address_, other.address_);
    std::swap(reservedBytes_, other.reservedBytes_);
    std::swap(committedBytes_, other.committedBytes_);
    std::swap(pageBytes_, other.pageBytes_);
    std::swap(remainingPagesForTests_, other.remainingPagesForTests_);
    committedPages_.swap(other.committedPages_);
    std::swap(committedPageWords_, other.committedPageWords_);
}

} // namespace harken::detail
