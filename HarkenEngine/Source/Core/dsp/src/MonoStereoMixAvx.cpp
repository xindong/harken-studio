// Keep this translation unit independent of the baseline inline DSP kernels.
#include <cstring>
#include <cstdint>

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>

#if defined(__clang__) || defined(__GNUC__)
#define HARKEN_MONO_MIX_AVX __attribute__((target("avx,no-fma"), noinline))
#else
#define HARKEN_MONO_MIX_AVX __declspec(noinline)
#endif

namespace harken::output_gain_detail {
namespace {

template<bool ApplyVoiceGain, bool EqualOutputScale>
HARKEN_MONO_MIX_AVX void MixAvx(const float* src, float* left, float* right,
    std::uint32_t frames, float voiceGain, float leftScale, float rightScale) noexcept {
    const __m256 voice = _mm256_set1_ps(voiceGain);
    const __m256 leftGain = _mm256_set1_ps(leftScale);
    const __m256 rightGain = _mm256_set1_ps(rightScale);
    const auto vectorFrames = frames & ~std::uint32_t{7};
    std::uint32_t i = 0;
    for (; i < vectorFrames; i += 8) {
        __m256 value = _mm256_loadu_ps(src + i);
        if constexpr (ApplyVoiceGain) value = _mm256_mul_ps(value, voice);
        const __m256 leftContribution = _mm256_mul_ps(value, leftGain);
        _mm256_storeu_ps(left + i,
            _mm256_add_ps(_mm256_loadu_ps(left + i), leftContribution));
        const __m256 rightContribution = EqualOutputScale
            ? leftContribution : _mm256_mul_ps(value, rightGain);
        _mm256_storeu_ps(right + i,
            _mm256_add_ps(_mm256_loadu_ps(right + i), rightContribution));
    }
    // Preserve the two separate roundings and ordered accumulation in the tail.
    for (; i < frames; ++i) {
        float value = src[i];
        if constexpr (ApplyVoiceGain) value *= voiceGain;
        const float leftContribution = value * leftScale;
        left[i] += leftContribution;
        const float rightContribution = EqualOutputScale
            ? leftContribution : value * rightScale;
        right[i] += rightContribution;
    }
}

} // namespace

HARKEN_MONO_MIX_AVX void MixMonoToStereoAvx(const float* src, float* left, float* right,
    std::uint32_t frames, float voiceGain, float leftScale, float rightScale) noexcept {
    std::uint32_t leftBits = 0, rightBits = 0;
    std::memcpy(&leftBits, &leftScale, sizeof(leftBits));
    std::memcpy(&rightBits, &rightScale, sizeof(rightBits));
    if (leftBits == rightBits) {
        if (voiceGain == 1.0f)
            MixAvx<false, true>(src, left, right, frames, voiceGain, leftScale, rightScale);
        else
            MixAvx<true, true>(src, left, right, frames, voiceGain, leftScale, rightScale);
    } else {
        if (voiceGain == 1.0f)
            MixAvx<false, false>(src, left, right, frames, voiceGain, leftScale, rightScale);
        else
            MixAvx<true, false>(src, left, right, frames, voiceGain, leftScale, rightScale);
    }
}

} // namespace harken::output_gain_detail

#undef HARKEN_MONO_MIX_AVX
#endif
