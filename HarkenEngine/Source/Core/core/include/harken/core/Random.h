// Engine pseudo-random generator: one deterministic stream for every logic-side
// selection the engine makes (container weighted draws now; Action probability
// and jitter later). Determinism is a hard requirement — the same seed and the
// same sequence of draws must reproduce identical results on every platform and
// toolchain — so the generator is pure 64-bit integer arithmetic with no
// floating point and no dependency on the C library's RNG.
//
// Algorithm: SplitMix64 by Sebastiano Vigna, a public-domain reference
// generator (http://prng.di.unimi.it/splitmix64.c, released to the public
// domain under CC0). It is a single-word-state mixing generator: the state
// advances by a fixed odd increment and the output is a fixed avalanche of that
// state. Being one 64-bit word, its whole state saves and restores trivially —
// the container resolver snapshots it so a failed resolution advances nothing.
#ifndef HARKEN_CORE_RANDOM_H
#define HARKEN_CORE_RANDOM_H

#include <cstdint>

namespace harken {

class SplitMix64 {
public:
    // Seed used when the config leaves randomSeed at 0: a fixed nonzero
    // constant so an unset seed is still a well-defined, reproducible stream.
    static constexpr std::uint64_t kDefaultSeed = 0x9E3779B97F4A7C15ull;

    explicit SplitMix64(std::uint64_t seed = kDefaultSeed) : m_state(seed) {}

    // Whole-state access for the snapshot/restore the resolver uses to make a
    // failed resolution a no-op on the stream.
    std::uint64_t State() const { return m_state; }
    void SetState(std::uint64_t s) { m_state = s; }

    // One 64-bit draw. The constants are the published SplitMix64 mixing
    // constants; every operation is defined wraparound arithmetic on u64, so
    // the sequence is identical across compilers and CPUs.
    std::uint64_t Next() {
        std::uint64_t z = (m_state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    // Uniform draw in [0, bound) with no modulo bias, via rejection sampling
    // (the OpenBSD arc4random_uniform technique): reject the low residue window
    // so every value in range is equally likely regardless of bound. bound must
    // be > 0. Deterministic — rejections consume draws in a fixed pattern for a
    // given stream, so the result is reproducible across platforms.
    std::uint64_t Bounded(std::uint64_t bound) {
        // threshold = 2^64 mod bound, computed as (-bound) mod bound in u64.
        const std::uint64_t threshold = (0ull - bound) % bound;
        std::uint64_t r;
        do {
            r = Next();
        } while (r < threshold);
        return r % bound;
    }

private:
    std::uint64_t m_state;
};

} // namespace harken

#endif // HARKEN_CORE_RANDOM_H
