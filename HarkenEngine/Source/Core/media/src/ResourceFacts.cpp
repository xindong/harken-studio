#include "harken/media/ResourceFacts.h"

namespace harken {

ResourceFacts ResidentPcmFacts(const PCMRegistry& registry,
                               VnSoundId soundId) {
    ResourceFacts facts;
    const PCMView view = registry.Lookup(soundId);
    // Valid() is the registry's own usability test. Reporting an unusable
    // registration as a resource of length nothing would let a compiler plan
    // events against material that cannot be read.
    if (!view.Valid()) {
        return facts;
    }
    facts.valid = true;
    facts.sampleRate = view.sampleRate;
    facts.channelCount = view.channelCount;
    facts.frameCount = view.frames;
    facts.contentRevision = view.contentRevision;
    return facts;
}

ResourceFacts StreamResourceFacts(const StreamManager::StreamFacts& facts) {
    ResourceFacts out;
    if (facts.channelCount == 0 || facts.frameCount == 0) {
        return out;
    }
    out.valid = true;
    out.sampleRate = facts.sampleRate;
    out.channelCount = facts.channelCount;
    out.frameCount = facts.frameCount;
    out.contentRevision = facts.contentRevision;
    return out;
}

} // namespace harken
