#include "harken/dsp/GainRamp.h"

#if defined(_M_X64) || defined(__x86_64__)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

namespace harken::output_gain_detail {

#if defined(_M_X64) || defined(__x86_64__)
void MixMonoToStereoAvx(const float* src, float* left, float* right,
    std::uint32_t frames, float voiceGain, float leftScale, float rightScale) noexcept;

namespace {

bool AvxStateAvailable() noexcept {
    constexpr unsigned required = (1u << 26) | (1u << 27) | (1u << 28);
#if defined(_MSC_VER)
    int registers[4]{};
    __cpuid(registers, 1);
    if ((static_cast<unsigned>(registers[2]) & required) != required) return false;
    return (_xgetbv(0) & 6u) == 6u;
#else
    unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx) || (ecx & required) != required)
        return false;
    unsigned low = 0, high = 0;
    __asm__ __volatile__("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
    return (low & 6u) == 6u;
#endif
}

void MixMonoToStereoSelected(const float* src, float* left, float* right,
    std::uint32_t frames, float voiceGain, float leftScale, float rightScale) noexcept {
    if (frames < 64) {
        MixMonoToStereoScaled(src, left, right, frames, voiceGain, leftScale, rightScale);
        return;
    }
    MixMonoToStereoAvx(src, left, right, frames, voiceGain, leftScale, rightScale);
}

} // namespace
#endif

MonoStereoMixKernel SelectMonoStereoMixKernel() noexcept {
#if defined(_M_X64) || defined(__x86_64__)
    if (AvxStateAvailable()) return &MixMonoToStereoSelected;
#endif
    return &MixMonoToStereoScaled;
}

} // namespace harken::output_gain_detail
