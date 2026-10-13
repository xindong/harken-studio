// Resource facts: the handful of numbers a compiler or a load-time check needs
// about an audio resource, in one shape regardless of where the resource lives.
//
// The stream side already published facts in exactly this spirit. The resident
// PCM side published a view instead — the same numbers wrapped around sample
// pointers and a live-voice counter. Handing a compiler that view would hand it
// playback machinery it has no business touching, so this reader answers the
// question it actually has: how long is the material, at what rate, over how
// many channels.
//
// It reads through the existing registries and copies nothing but the numbers.
// There is no second registry here, and no ownership: the facts are a value,
// valid at the moment they were read, and a caller that needs them to stay true
// across a hot reload pins the generation rather than caching this struct.
//
// The ResourceFacts value itself lives in the engine-free ResourceFactsValue.h
// so a tooling-side compiler can consume the same type without this header's
// engine dependencies; this header adds the registry readers that produce it.
#ifndef HARKEN_MEDIA_RESOURCE_FACTS_H
#define HARKEN_MEDIA_RESOURCE_FACTS_H

#include <harken/Harken.h>

#include "harken/media/PcmRegistry.h"
#include "harken/media/ResourceFactsValue.h"
#include "harken/media/StreamManager.h"

namespace harken {

// Facts for a resident PCM sound. Returns an invalid value for an id the
// registry does not hold, and for a registration whose view is not usable —
// a zero-channel or zero-frame entry would otherwise report as a real resource
// of length nothing.
ResourceFacts ResidentPcmFacts(const PCMRegistry& registry, VnSoundId soundId);

// Facts for an already-read stream description, so both resource classes reach
// a caller in the same shape. Takes the facts rather than the manager because
// the stream lookup and the generation pinning that goes with it belong to the
// caller, not here.
ResourceFacts StreamResourceFacts(const StreamManager::StreamFacts& facts);

} // namespace harken

#endif // HARKEN_MEDIA_RESOURCE_FACTS_H
