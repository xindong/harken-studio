// The key that seeds a piece's randomness, and its hash.
//
// Selection randomness is keyed by what the choice belongs to -- the piece
// instance's seed, the clip, the loop iteration and the occurrence ordinal -- not
// by the order the planner happens to reach the choices in. Two planners that
// serve the same choices in different orders, or one planner run twice, must draw
// the same values, so the seed for a choice is a pure function of its key and
// nothing else. This header fixes the key and that function; the selection logic
// that consumes the stream is a later segment.
#ifndef HARKEN_CORE_KEYED_RNG_H
#define HARKEN_CORE_KEYED_RNG_H

#include <harken/ObjectId.h>
#include <cstdint>

namespace harken {

// Identifies one keyed random draw. Every field is authored/lineage data, so the
// same choice keys the same way on every run and every machine.
struct KeyedRngInput {
    std::uint64_t pieceInstanceSeed = 0;
    VnObjectId clipId{};
    std::uint32_t loopIteration = 0;
    std::uint32_t occurrenceOrdinal = 0;
};

// A deterministic seed for the draw. Depends only on the field values, so it is
// independent of when or in what order it is computed. The mix is SplitMix64's
// finalizer. The two 32-bit occurrence coordinates are packed injectively into
// one 64-bit lane before mixing.
inline std::uint64_t HashKeyedRng(const KeyedRngInput& key) {
    auto mix = [](std::uint64_t z) {
        z += 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    };
    std::uint64_t h = mix(key.pieceInstanceSeed + 0x1000000000000001ull);
    h = mix(h ^ key.clipId.high);
    h = mix(h ^ key.clipId.low);
    const std::uint64_t occurrence =
        (static_cast<std::uint64_t>(key.loopIteration) << 32) |
        key.occurrenceOrdinal;
    h = mix(h ^ occurrence);
    return h;
}

} // namespace harken

#endif // HARKEN_CORE_KEYED_RNG_H
