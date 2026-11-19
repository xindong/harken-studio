// The plain value an audio resource reduces to for a compiler or a load-time
// check: how long the material is, at what rate, over how many channels.
//
// This header is deliberately free of engine and platform dependencies — it
// includes only <cstdint> — so a tooling-side compiler can consume the very same
// value type the runtime reads, without pulling in Harken.h or the media
// registries. The concept is owned by the runtime media layer, which produces
// this value from live registries in harken/media/ResourceFacts.h; a tooling
// consumer receives it as a declarative input and never reads a registry.
//
// It is a value, valid at the moment it was read. There is no ownership and no
// second registry: a caller that needs the numbers to stay true across a hot
// reload pins the generation rather than caching this struct.
#ifndef HARKEN_MEDIA_RESOURCE_FACTS_VALUE_H
#define HARKEN_MEDIA_RESOURCE_FACTS_VALUE_H

#include <cstdint>

namespace harken {

// Immutable facts about one audio resource. valid is false when the id names
// nothing, in which case every other field reads 0 rather than a stale number.
struct ResourceFacts {
    bool valid = false;
    std::uint32_t sampleRate = 0;
    std::uint32_t channelCount = 0;
    std::uint64_t frameCount = 0;
    std::uint64_t contentRevision = 0;
};

}  // namespace harken

#endif  // HARKEN_MEDIA_RESOURCE_FACTS_VALUE_H
