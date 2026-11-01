#include "harken/dsp/Resampler.h"
#include <bit>
#include <limits>

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#if defined(__clang__) || defined(__GNUC__)
#define HARKEN_AVX2 __attribute__((target("avx2,no-fma"), noinline))
#else
#define HARKEN_AVX2 __declspec(noinline)
#endif

namespace harken {
HARKEN_AVX2 void ExpandExactInt16Avx2(float* destination,
    const std::int16_t* source, std::uint32_t frames) noexcept {
    const auto scale = _mm256_set1_ps(0x1p-15f);
    std::uint32_t i = 0;
    for (; frames - i >= 16; i += 16) {
        const auto first = _mm256_cvtepi16_epi32(_mm_loadu_si128(
            reinterpret_cast<const __m128i*>(source + i)));
        const auto second = _mm256_cvtepi16_epi32(_mm_loadu_si128(
            reinterpret_cast<const __m128i*>(source + i + 8)));
        _mm256_storeu_ps(destination + i,
            _mm256_mul_ps(_mm256_cvtepi32_ps(first), scale));
        _mm256_storeu_ps(destination + i + 8,
            _mm256_mul_ps(_mm256_cvtepi32_ps(second), scale));
    }
    if (frames - i >= 8) {
        const auto samples = _mm256_cvtepi16_epi32(_mm_loadu_si128(
            reinterpret_cast<const __m128i*>(source + i)));
        _mm256_storeu_ps(destination + i,
            _mm256_mul_ps(_mm256_cvtepi32_ps(samples), scale));
        i += 8;
    }
    for (; i < frames; ++i)
        destination[i] = static_cast<float>(source[i]) * 0x1p-15f;
}

#if defined(__clang__) || defined(__GNUC__)
__attribute__((aligned(64)))
#endif
HARKEN_AVX2 std::uint32_t ProduceFixedMonoAvx2(const ResamplerView& source,
    float* destination, std::uint32_t maxFrames, std::uint64_t& index,
    std::uint64_t& phaseHigh, std::uint64_t stepHigh) noexcept {
    constexpr auto fractionMask = (UINT64_C(1) << 53) - 1;
    constexpr auto ringMask = kResampleFifoFrames - 1;
    const auto step = stepHigh >> 11;
    const auto offsets0 = _mm256_setr_epi64x(0, step, 2 * step, 3 * step);
    const auto offsets1 = _mm256_setr_epi64x(4 * step, 5 * step, 6 * step, 7 * step);
    std::uint32_t produced = 0;
    while (maxFrames - produced >= 8 && index <= UINT64_MAX - 8 &&
           index + 8 < source.writeIndex &&
           (!source.inputEnded || index + 7 < source.inputEndIndex)) {
        const auto ringOffset = static_cast<std::uint32_t>(index & ringMask);
        if (ringOffset > ringMask - 8) break;
        const auto phase = phaseHigh >> 11;
        const auto base = _mm256_set1_epi64x(phase);
        const auto positions0 = _mm256_add_epi64(base, offsets0);
        const auto positions1 = _mm256_add_epi64(base, offsets1);
        const auto whole0 = _mm256_srli_epi64(positions0, 53);
        const auto whole1 = _mm256_srli_epi64(positions1, 53);
        const auto whole = _mm256_permute4x64_epi64(_mm256_castps_si256(
            _mm256_shuffle_ps(_mm256_castsi256_ps(whole0),
                _mm256_castsi256_ps(whole1), _MM_SHUFFLE(2, 0, 2, 0))),
            _MM_SHUFFLE(3, 1, 2, 0));
        const auto words0 = _mm256_srli_epi64(positions0, 22);
        const auto words1 = _mm256_srli_epi64(positions1, 22);
        const auto words = _mm256_and_si256(_mm256_permute4x64_epi64(
            _mm256_castps_si256(_mm256_shuffle_ps(_mm256_castsi256_ps(words0),
                _mm256_castsi256_ps(words1), _MM_SHUFFLE(2, 0, 2, 0))),
            _MM_SHUFFLE(3, 1, 2, 0)), _mm256_set1_epi32(INT32_MAX));
        auto fraction = _mm256_mul_ps(_mm256_cvtepi32_ps(words), _mm256_set1_ps(0x1p-31f));
        const auto small = _mm256_cmpgt_epi32(_mm256_set1_epi32(1 << 25), words);
        if (_mm256_movemask_ps(_mm256_castsi256_ps(small)) != 0) {
            // Small Q53 fractions fit in 47 bits, so the double
            // bias conversion is exact before the existing float rounding.
            const auto mantissa = _mm256_set1_epi64x((UINT64_C(1) << 52) - 1);
            const auto exponent = _mm256_set1_epi64x(UINT64_C(0x4330000000000000));
            const auto bias = _mm256_set1_pd(0x1p52);
            const auto exact0 = _mm256_cvtpd_ps(_mm256_sub_pd(
                _mm256_castsi256_pd(_mm256_or_si256(exponent,
                    _mm256_and_si256(positions0, mantissa))), bias));
            const auto exact1 = _mm256_cvtpd_ps(_mm256_sub_pd(
                _mm256_castsi256_pd(_mm256_or_si256(exponent,
                    _mm256_and_si256(positions1, mantissa))), bias));
            const auto exact = _mm256_insertf128_ps(_mm256_castps128_ps256(exact0),
                                                    exact1, 1);
            fraction = _mm256_blendv_ps(fraction,
                _mm256_mul_ps(exact, _mm256_set1_ps(0x1p-53f)),
                _mm256_castsi256_ps(small));
        }
        const auto initial = _mm256_permutevar8x32_ps(
            _mm256_loadu_ps(source.ring[0] + ringOffset), whole);
        const auto next = _mm256_permutevar8x32_ps(
            _mm256_loadu_ps(source.ring[0] + ringOffset + 1), whole);
        const auto endpoint = _mm256_cmp_ps(fraction, _mm256_setzero_ps(), _CMP_EQ_OQ);
        const auto first = _mm256_andnot_ps(endpoint, initial);
        const auto following = _mm256_andnot_ps(endpoint, next);
        const auto interpolated = _mm256_add_ps(first,
            _mm256_mul_ps(fraction, _mm256_sub_ps(following, first)));
        _mm256_storeu_ps(destination + produced,
            _mm256_blendv_ps(interpolated, initial, endpoint));
        const auto nextPosition = phase + 8 * step;
        index += nextPosition >> 53;
        phaseHigh = (nextPosition & fractionMask) << 11;
        produced += 8;
    }
    return produced;
}
} // namespace harken
#undef HARKEN_AVX2
#endif
