#include "harken/core/AllocGuard.h"

#include <atomic>

// The guard rides the MSVC debug CRT allocation hook, which observes both
// malloc and operator new without overriding global operators, and lets us
// skip CRT-internal blocks. Both supported toolchains (cl, clang-cl) use the
// MSVC debug CRT in Debug configurations.
#if defined(_DEBUG) && defined(_MSC_VER)
#define HARKEN_MEMGUARD_ACTIVE 1
#else
#define HARKEN_MEMGUARD_ACTIVE 0
#endif

#if HARKEN_MEMGUARD_ACTIVE
#include <crtdbg.h>
#include <cstdio>
#include <cstdlib>
#include <windows.h>
#endif

namespace harken::memguard {

namespace {

std::atomic<std::uint64_t> g_hits{0};
std::atomic<bool> g_armed{false};
// Depth count rather than a flag: scopes nest (device callback wrapping
// the mixer's per-block scope), and an inner scope's exit must not
// untag the thread while an outer scope is still live.
thread_local std::uint32_t t_audioDepth = 0;
thread_local AllocationObservation* t_observation = nullptr;

#if HARKEN_MEMGUARD_ACTIVE

thread_local bool t_inHook = false;
_CRT_ALLOC_HOOK g_previousHook = nullptr;

void ReportViolation(std::size_t size) {
    // Formatting to a stack buffer and writing via the OS handle avoids
    // re-entering the allocator from inside the hook.
    char message[256];
    int length = _snprintf_s(message, _TRUNCATE,
        "harken memguard: heap allocation of %zu bytes on an audio-tagged thread\n",
        size);
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    DWORD written = 0;
    if (err != INVALID_HANDLE_VALUE && length > 0) {
        WriteFile(err, message, static_cast<DWORD>(length), &written, nullptr);
    }

    void* frames[32];
    USHORT captured = CaptureStackBackTrace(1, 32, frames, nullptr);
    for (USHORT i = 0; i < captured; ++i) {
        length = _snprintf_s(message, _TRUNCATE, "  frame %02u: %p\n",
                             static_cast<unsigned>(i), frames[i]);
        if (err != INVALID_HANDLE_VALUE && length > 0) {
            WriteFile(err, message, static_cast<DWORD>(length), &written, nullptr);
        }
    }

    if (IsDebuggerPresent()) {
        __debugbreak();
    }
    std::abort();
}

int AllocHook(int allocType, void* /*userData*/, std::size_t size,
              int blockType, long /*requestNumber*/,
              const unsigned char* /*fileName*/, int /*line*/) {
    // CRT bookkeeping blocks are not user allocations; re-entrancy from the
    // reporting path must not recurse into the hook.
    if (t_inHook || blockType == _CRT_BLOCK) {
        return TRUE;
    }
    if (t_observation != nullptr &&
        (allocType == _HOOK_ALLOC || allocType == _HOOK_REALLOC)) {
        auto& events = t_audioDepth > 0 ? t_observation->audio : t_observation->logic;
        if (allocType == _HOOK_ALLOC) {
            ++events.allocations;
            events.allocationBytes += size;
        } else {
            ++events.reallocations;
            events.reallocationBytes += size;
        }
    }
    if (t_audioDepth > 0 &&
        (allocType == _HOOK_ALLOC || allocType == _HOOK_REALLOC)) {
        t_inHook = true;
        g_hits.fetch_add(1, std::memory_order_relaxed);
        ReportViolation(size);
        t_inHook = false; // unreachable after abort; kept for clarity
    }
    return TRUE;
}

#endif // HARKEN_MEMGUARD_ACTIVE

} // namespace

void Install() {
#if HARKEN_MEMGUARD_ACTIVE
    if (!g_armed.exchange(true)) {
        g_previousHook = _CrtSetAllocHook(AllocHook);
    }
#endif
}

void Uninstall() {
#if HARKEN_MEMGUARD_ACTIVE
    if (g_armed.exchange(false)) {
        _CrtSetAllocHook(g_previousHook);
        g_previousHook = nullptr;
    }
#endif
}

bool IsArmed() {
    return g_armed.load(std::memory_order_relaxed);
}

std::uint64_t HitCount() {
    return g_hits.load(std::memory_order_relaxed);
}

bool IsAudioTagged() {
    return t_audioDepth > 0;
}

AllocationObservationScope::AllocationObservationScope(AllocationObservation& result) noexcept {
    result = {};
    if (IsArmed() && t_observation == nullptr) {
        result_ = &result;
        result.available = true;
        t_observation = &result;
    }
}

AllocationObservationScope::~AllocationObservationScope() {
    if (result_ != nullptr) {
        result_->available = result_->available && IsArmed();
        t_observation = nullptr;
    }
}

AudioThreadScope::AudioThreadScope() {
    ++t_audioDepth;
}

AudioThreadScope::~AudioThreadScope() {
    --t_audioDepth;
}

} // namespace harken::memguard
