#ifndef HARKEN_CORE_FLOAT_MODE_H
#define HARKEN_CORE_FLOAT_MODE_H

#include <cstdint>

#if defined(_M_ARM64) || defined(_M_ARM64EC) || defined(__aarch64__) ||       \
    defined(__arm64__)
#define HARKEN_FLOAT_MODE_ARM64 1
#if defined(_MSC_VER)
#include <arm64intr.h>
#include <intrin.h>
#endif
#elif defined(__arm__)
#define HARKEN_FLOAT_MODE_ARM32 1
#elif defined(_M_IX86) || defined(_M_X64) || defined(__i386__) ||            \
    defined(__x86_64__)
#define HARKEN_FLOAT_MODE_X86 1
#include <pmmintrin.h>
#include <xmmintrin.h>
#endif

namespace harken::floatmode {

#if defined(HARKEN_FLOAT_MODE_ARM64)
namespace detail {

inline std::uint64_t ReadFpcr() noexcept {
#if defined(_MSC_VER)
    return static_cast<std::uint64_t>(_ReadStatusReg(ARM64_FPCR));
#else
    std::uint64_t value = 0;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(value));
    return value;
#endif
}

inline void WriteFpcr(std::uint64_t value) noexcept {
#if defined(_MSC_VER)
    _WriteStatusReg(ARM64_FPCR, static_cast<__int64>(value));
#else
    __asm__ __volatile__("msr fpcr, %0" : : "r"(value) : "memory");
#endif
}

} // namespace detail
#endif

// Applies the engine's denormal mode for one render boundary and restores the
// incoming thread state on exit. This keeps device-owned callback threads from
// retaining engine policy after control returns to the backend.
class ScopedFlushDenormals final {
public:
    ScopedFlushDenormals() noexcept {
#if defined(HARKEN_FLOAT_MODE_X86)
        m_previous = _mm_getcsr();
        _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
        _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#elif defined(HARKEN_FLOAT_MODE_ARM64)
        m_previous = detail::ReadFpcr();
        detail::WriteFpcr(m_previous | kFlushToZeroMask);
#elif defined(HARKEN_FLOAT_MODE_ARM32)
        m_previous = __builtin_arm_get_fpscr();
        __builtin_arm_set_fpscr(m_previous | kFlushToZeroMask);
#endif
    }

    ~ScopedFlushDenormals() {
#if defined(HARKEN_FLOAT_MODE_X86)
        _mm_setcsr(m_previous);
#elif defined(HARKEN_FLOAT_MODE_ARM64)
        detail::WriteFpcr(m_previous);
#elif defined(HARKEN_FLOAT_MODE_ARM32)
        __builtin_arm_set_fpscr(m_previous);
#endif
    }

    ScopedFlushDenormals(const ScopedFlushDenormals&) = delete;
    ScopedFlushDenormals& operator=(const ScopedFlushDenormals&) = delete;

private:
#if defined(HARKEN_FLOAT_MODE_X86)
    unsigned int m_previous = 0;
#elif defined(HARKEN_FLOAT_MODE_ARM64)
    static constexpr std::uint64_t kFlushToZeroMask =
        std::uint64_t{1} << 24;
    std::uint64_t m_previous = 0;
#elif defined(HARKEN_FLOAT_MODE_ARM32)
    static constexpr std::uint32_t kFlushToZeroMask =
        std::uint32_t{1} << 24;
    std::uint32_t m_previous = 0;
#endif
};

} // namespace harken::floatmode

#undef HARKEN_FLOAT_MODE_ARM32
#undef HARKEN_FLOAT_MODE_ARM64
#undef HARKEN_FLOAT_MODE_X86

#endif // HARKEN_CORE_FLOAT_MODE_H
