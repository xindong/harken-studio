#include "harken_project_schema/BuildPieceIR.h"
#include "harken_project_schema/PropertyFold.h"
#include "harken_project_schema/detail/Fnv1a.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace harkenprojectschema {

namespace detail {

struct PieceIRBuilder {
    static PieceIR Build(std::vector<IrTrack> tracks,
                         std::vector<IrPiece> pieces,
                         RuntimeLineageTable lineage,
                         std::vector<CompiledEffectChain> effectChains,
                         std::uint64_t fingerprint) {
        return PieceIR(std::move(tracks), std::move(pieces),
                       std::move(lineage), std::move(effectChains),
                       fingerprint);
    }
};

}  // namespace detail

namespace {

// FNV-1a, 64-bit. A public-domain non-cryptographic hash by Fowler, Noll and Vo;
// the constants are the standard 64-bit offset basis and prime. Reference:
// http://www.isthe.com/chongo/tech/comp/fnv/ . Chosen for the lineage
// fingerprint because it is tiny, order-sensitive and fully reproducible across
// platforms — no library, no floating point, no endianness in the mixing.
// Folds a 64-bit value in little-endian byte order, so the hash of a value does
// not depend on the host's byte order.
void HashU64(std::uint64_t* state, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        detail::Fnv1a64HashByte(
            state, static_cast<std::uint8_t>(value & 0xffu));
        value >>= 8;
    }
}

std::uint64_t FingerprintLineage(const RuntimeLineageTable& lineage) {
    std::uint64_t state = detail::kFnv1a64OffsetBasis;
    for (const LineageEntry& e : lineage.entries) {
        HashU64(&state, e.hasPiece ? 1u : 0u);
        for (const auto id : {e.hasPiece ? e.pieceId : harkenstudio::ObjectId{}, e.trackId, e.clipId})
            for (const auto byte : harkenstudio::ObjectIdBytes(id))
                detail::Fnv1a64HashByte(&state, byte);
        HashU64(&state, e.occurrenceOrdinal);
    }
    return state;
}

struct BuildIndexes {
    std::map<StableId, const TrackNormalization*> trackFacts;
    std::map<StableId, const PieceNormalization*> pieceFacts;
    std::map<StableId, StableId> trackOwner;
};

// Floor of frameCount * 1e9 / sampleRate without overflowing int64. Splits the
// length into whole seconds and a leftover-frame remainder so a long resource
// does not overflow the multiply; returns false when even the whole-second
// nanoseconds would not fit, which the caller turns into a refusal. This is the
// derived nanosecond estimate only — the authoritative length stays the
// frames-at-rate pair.
bool FramesToNanosFloor(std::uint64_t frameCount, std::uint32_t sampleRate,
                        std::int64_t* outNs) {
    if (sampleRate == 0) {
        return false;
    }
    constexpr std::uint64_t kNanosPerSecond = 1000000000ull;
    constexpr std::uint64_t kMaxI64 =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
    const std::uint64_t wholeSeconds = frameCount / sampleRate;
    const std::uint64_t remainderFrames = frameCount % sampleRate;
    if (wholeSeconds > kMaxI64 / kNanosPerSecond) {
        return false;
    }
    const std::uint64_t wholeNs = wholeSeconds * kNanosPerSecond;
    // remainderFrames < sampleRate <= UINT32_MAX, so remainderFrames * 1e9 fits
    // in uint64, and the quotient is below one second.
    const std::uint64_t remainderNs =
        (remainderFrames * kNanosPerSecond) / sampleRate;
    if (wholeNs > kMaxI64 - remainderNs) {
        return false;
    }
    *outNs = static_cast<std::int64_t>(wholeNs + remainderNs);
    return true;
}

bool ClipOrderLess(const Clip& a, const Clip& b) {
    if (a.time != b.time) {
        return a.time < b.time;
    }
    if (a.semanticOrderKey != b.semanticOrderKey) {
        return a.semanticOrderKey < b.semanticOrderKey;
    }
    return a.id < b.id;
}

std::optional<BuildIndexes> ValidateBuildInputs(
    const CanonicalProject& project, const NormalizedFacts& facts) {
    std::map<StableId, const TrackDef*> modelTracks;
    std::map<StableId, const PieceDef*> modelPieces;
    for (const ContentObject& obj : project.objects) {
        if (obj.kind == ContentKind::Track) {
            const StableId id = obj.track.chassis.id;
            if (modelPieces.find(id) != modelPieces.end() ||
                !modelTracks.emplace(id, &obj.track).second) {
                return std::nullopt;
            }
        } else if (obj.kind == ContentKind::Piece) {
            const StableId id = obj.piece.chassis.id;
            if (modelTracks.find(id) != modelTracks.end() ||
                !modelPieces.emplace(id, &obj.piece).second) {
                return std::nullopt;
            }
        }
    }

    BuildIndexes indexes;
    for (const TrackNormalization& track : facts.tracks) {
        if (!indexes.trackFacts.emplace(track.track, &track).second) {
            return std::nullopt;
        }
    }
    for (const PieceNormalization& piece : facts.pieces) {
        if (!indexes.pieceFacts.emplace(piece.piece, &piece).second) {
            return std::nullopt;
        }
    }
    if (indexes.trackFacts.size() != modelTracks.size() ||
        indexes.pieceFacts.size() != modelPieces.size()) {
        return std::nullopt;
    }

    for (const auto& [trackId, trackFacts] : indexes.trackFacts) {
        const auto modelIt = modelTracks.find(trackId);
        if (modelIt == modelTracks.end()) {
            return std::nullopt;
        }
        std::set<StableId> modelClipIds;
        std::unordered_map<StableId, const Clip*> modelClipById;
        modelClipById.reserve(modelIt->second->clips.size());
        for (const Clip& clip : modelIt->second->clips) {
            if (!modelClipIds.emplace(clip.id).second ||
                !modelClipById.emplace(clip.id, &clip).second) {
                return std::nullopt;
            }
        }
        std::set<StableId> factClipIds;
        for (const StableId clipId : trackFacts->clipsByOrder) {
            if (!factClipIds.emplace(clipId).second) {
                return std::nullopt;
            }
        }
        if (factClipIds != modelClipIds) {
            return std::nullopt;
        }
        const Clip* previous = nullptr;
        for (const StableId clipId : trackFacts->clipsByOrder) {
            const auto clipIt = modelClipById.find(clipId);
            if (clipIt == modelClipById.end() ||
                (previous != nullptr &&
                 !ClipOrderLess(*previous, *clipIt->second))) {
                return std::nullopt;
            }
            previous = clipIt->second;
        }
    }

    for (const auto& [pieceId, pieceFacts] : indexes.pieceFacts) {
        const auto modelIt = modelPieces.find(pieceId);
        if (modelIt == modelPieces.end()) {
            return std::nullopt;
        }
        std::set<StableId> modelMarkerIds;
        std::unordered_map<StableId, const Marker*> modelMarkerByStableId;
        modelMarkerByStableId.reserve(modelIt->second->markers.size());
        std::unordered_map<MarkerId, const Marker*> modelMarkerByMarkerId;
        modelMarkerByMarkerId.reserve(modelIt->second->markers.size());
        for (const Marker& marker : modelIt->second->markers) {
            if (!modelMarkerIds.emplace(marker.id).second ||
                !modelMarkerByStableId.emplace(marker.id, &marker).second ||
                !modelMarkerByMarkerId.emplace(marker.markerId, &marker).second) {
                return std::nullopt;
            }
        }
        std::set<StableId> factMarkerIds;
        for (const StableId markerId : pieceFacts->markersByMarkerId) {
            if (!factMarkerIds.emplace(markerId).second) {
                return std::nullopt;
            }
        }
        if (factMarkerIds != modelMarkerIds) {
            return std::nullopt;
        }
        const Marker* previousMarker = nullptr;
        for (const StableId markerId : pieceFacts->markersByMarkerId) {
            const auto markerIt = modelMarkerByStableId.find(markerId);
            if (markerIt == modelMarkerByStableId.end() ||
                (previousMarker != nullptr &&
                 previousMarker->markerId >= markerIt->second->markerId)) {
                return std::nullopt;
            }
            previousMarker = markerIt->second;
        }
        const PieceDef& modelPiece = *modelIt->second;
        if (pieceFacts->startKind != modelPiece.startPoint.kind ||
            pieceFacts->startMarkerId != modelPiece.startPoint.markerId) {
            return std::nullopt;
        }
        std::int64_t expectedStartCoordinate = 0;
        if (modelPiece.startPoint.kind == StartPointKind::Marker) {
            const auto startMarker =
                modelMarkerByMarkerId.find(modelPiece.startPoint.markerId);
            if (startMarker == modelMarkerByMarkerId.end()) {
                return std::nullopt;
            }
            expectedStartCoordinate = startMarker->second->position;
        } else if (modelPiece.startPoint.kind != StartPointKind::Entry) {
            return std::nullopt;
        }
        if (pieceFacts->startCoordinate != expectedStartCoordinate) {
            return std::nullopt;
        }
    }

    for (const auto& [pieceId, piece] : modelPieces) {
        for (const StableId trackId : piece->tracks) {
            if (modelTracks.find(trackId) == modelTracks.end() ||
                !indexes.trackOwner.emplace(trackId, pieceId).second) {
                return std::nullopt;
            }
        }
    }
    return indexes;
}

}  // namespace

namespace {

std::optional<PieceIR> BuildPieceIRImpl(
    const CanonicalProject& project, const NormalizedFacts& facts,
    const ResourceTable& resources,
    const ReferenceResolutionFacts* resolution,
    const SendResolutionFacts* sends,
    const EffectChainSelectionFacts* effectChains) {
    // The three front-end fact bundles arrive together or not at all: a build
    // that had some of them would produce an occurrence whose routing, spatial or
    // chain facts were silently defaulted.
    if ((resolution == nullptr) != (sends == nullptr) ||
        (resolution == nullptr) != (effectChains == nullptr)) {
        return std::nullopt;
    }
    const std::optional<BuildIndexes> indexes =
        ValidateBuildInputs(project, facts);
    if (!indexes.has_value()) {
        return std::nullopt;
    }

    // Routing facts are keyed by (trackId, clipId), the key both front-end
    // products expose. The structure-only entry deliberately leaves both maps
    // empty so occurrences retain neutral routing defaults.
    std::map<std::pair<StableId, StableId>,
             const OccurrenceReferenceResolution*>
        resolutionByOccurrence;
    if (resolution != nullptr) {
        for (const OccurrenceReferenceResolution& occ :
             resolution->occurrences) {
            if (!resolutionByOccurrence
                     .emplace(std::make_pair(occ.trackId, occ.clipId), &occ)
                     .second) {
                return std::nullopt;
            }
        }
    }
    std::map<std::pair<StableId, StableId>, const OccurrenceSendResolution*>
        sendsByOccurrence;
    if (sends != nullptr) {
        for (const OccurrenceSendResolution& occ : sends->occurrences) {
            if (!sendsByOccurrence
                     .emplace(std::make_pair(occ.trackId, occ.clipId), &occ)
                     .second) {
                return std::nullopt;
            }
        }
    }
    std::map<std::pair<StableId, StableId>,
             const OccurrenceEffectChainSelection*>
        effectChainByOccurrence;
    if (effectChains != nullptr) {
        for (const OccurrenceEffectChainSelection& occ :
             effectChains->occurrences) {
            if (!effectChainByOccurrence
                     .emplace(std::make_pair(occ.trackId, occ.clipId), &occ)
                     .second) {
                return std::nullopt;
            }
        }
        // A selection that indexes outside its own chain table would leave an
        // occurrence pointing at nothing, so the table is checked here rather
        // than trusted.
        for (const OccurrenceEffectChainSelection& occ :
             effectChains->occurrences) {
            if (occ.declared &&
                occ.chainIndex >= effectChains->chains.size()) {
                return std::nullopt;
            }
        }
        // Every entry's parameter block must resolve inside its own chain. The
        // fingerprint and every later consumer read the payload through this
        // reference, so an unresolvable one is refused here rather than
        // dereferenced downstream.
        for (const CompiledEffectChain& chain : effectChains->chains) {
            for (const CompiledEffectChainEntry& entry : chain.entries) {
                if (entry.paramBlockRef >= chain.paramBlocks.size()) {
                    return std::nullopt;
                }
            }
        }
    }

    std::vector<IrTrack> tracks;
    std::vector<IrPiece> pieces;
    std::map<StableId, AuthoringGuid> objectGuidById;
    // The subject's own spatial group, looked up per occurrence. Spatial is a
    // subject property, not a chain contribution, so an occurrence carries the
    // referenced object's own values verbatim.
    std::map<StableId, const SpatialSpec*> spatialById;
    for (const ContentObject& obj : project.objects) {
        if (!objectGuidById.emplace(obj.chassis().id, obj.chassis().guid)
                 .second) {
            return std::nullopt;
        }
        spatialById.emplace(obj.chassis().id, &obj.chassis().spatial);
    }

    // The gain/pitch/loop-count fold, built once and applied per occurrence below.
    const PropertyFoldContext propertyFold(project);
    std::size_t canonicalOccurrenceCount = 0;

    // Objects come in ascending stable-id order, so the IR's track and piece
    // lists inherit that order without a further sort.
    for (const ContentObject& obj : project.objects) {
        if (obj.kind == ContentKind::Track) {
            const TrackDef& track = obj.track;
            IrTrack irTrack;
            irTrack.trackId = track.chassis.id;
            irTrack.guid = track.chassis.guid;

            // Address the Track's clips by id so the normalized order can drive
            // the occurrence sequence.
            std::map<StableId, const Clip*> clipById;
            for (const Clip& clip : track.clips) {
                clipById.emplace(clip.id, &clip);
            }
            const auto factsIt = indexes->trackFacts.find(track.chassis.id);
            if (factsIt == indexes->trackFacts.end()) {
                return std::nullopt;
            }
            std::uint32_t ordinal = 0;
            for (const StableId clipId : factsIt->second->clipsByOrder) {
                const auto clipIt = clipById.find(clipId);
                if (clipIt == clipById.end()) {
                    return std::nullopt;
                }
                const Clip& clip = *clipIt->second;
                IrOccurrence occ;
                occ.key.clipId = clip.id;
                occ.key.occurrenceOrdinal = ordinal;
                occ.clipGuid = clip.guid;
                occ.content = clip.contentRef;
                const auto targetGuid = objectGuidById.find(occ.content.target);
                if (targetGuid == objectGuidById.end()) {
                    return std::nullopt;
                }
                occ.contentTargetGuid = targetGuid->second;
                occ.time = clip.time;
                occ.authoredDuration = clip.duration;
                occ.fades = clip.fades;
                occ.semanticOrderKey = clip.semanticOrderKey;
                // A Sound target must have a valid resource fact; its natural
                // length becomes the occurrence's duration. A missing, invalid
                // or nanosecond-unrepresentable fact refuses the whole build.
                if (occ.content.kind == ContentKind::Sound) {
                    const auto resIt = resources.find(occ.content.target);
                    if (resIt == resources.end() ||
                        !resIt->second.facts.valid ||
                        resIt->second.facts.sampleRate == 0 ||
                        resIt->second.facts.channelCount == 0 ||
                        resIt->second.facts.frameCount == 0) {
                        return std::nullopt;
                    }
                    const ResourceFacts& rf = resIt->second.facts;
                    std::int64_t durationNs = 0;
                    if (!FramesToNanosFloor(rf.frameCount, rf.sampleRate,
                                            &durationNs)) {
                        return std::nullopt;
                    }
                    occ.hasDuration = true;
                    occ.durationFrames = rf.frameCount;
                    occ.resourceSampleRate = rf.sampleRate;
                    occ.durationNs = durationNs;
                    occ.seekable = resIt->second.seekable;
                }
                // A Container or Piece target keeps hasDuration false: its end is
                // decided by its own program or exit marker, not a natural length.

                // The event template: Start always, Stop only for a target
                // with a natural end. No SetStartCursor (a plain clip has no
                // source-in to seed one) and no Envelope (reserved).
                // The gain/pitch/loop-count fold for this occurrence: the subject is
                // the referenced content, its placement is this Track.
                const FoldedProperties folded =
                    propertyFold.Fold(occ.content.target, track.chassis.id, &clip);
                occ.baseVolumeDb = folded.baseVolumeDb;
                occ.outputBusVolumeDb = folded.outputBusVolumeDb;
                occ.basePitchRatio = folded.basePitchRatio;
                occ.loopCount = folded.loopCount;

                // The subject's own spatial scalars, carried verbatim (no fold).
                const auto spatialIt = spatialById.find(occ.content.target);
                if (spatialIt == spatialById.end()) {
                    return std::nullopt;
                }
                const SpatialSpec& spatial = *spatialIt->second;
                occ.panning = spatial.panning;
                occ.spread = spatial.spread;
                occ.is3DEnabled = spatial.is3DEnabled;
                occ.attenuationDistanceScaling = spatial.attenuationDistanceScaling;

                // Complete routing facts must name this occurrence and agree
                // with its normalized ordinal and referenced subject.
                const std::pair<StableId, StableId> routingKey(track.chassis.id,
                                                               clip.id);
                if (resolution != nullptr) {
                    const auto resolutionIt =
                        resolutionByOccurrence.find(routingKey);
                    const auto sendsIt = sendsByOccurrence.find(routingKey);
                    if (resolutionIt == resolutionByOccurrence.end() ||
                        sendsIt == sendsByOccurrence.end()) {
                        return std::nullopt;
                    }
                    const OccurrenceReferenceResolution& routingResolution =
                        *resolutionIt->second;
                    const OccurrenceSendResolution& routingSends =
                        *sendsIt->second;
                    if (routingResolution.occurrenceOrdinal != ordinal ||
                        routingResolution.subjectId != occ.content.target ||
                        routingResolution.subjectKind != occ.content.kind ||
                        routingSends.occurrenceOrdinal != ordinal ||
                        routingSends.subjectId != occ.content.target) {
                        return std::nullopt;
                    }
                    const harkenreferenceslots::ReferenceSlotResolution& busSlot =
                        routingResolution.resolvedSlots[static_cast<std::size_t>(
                            harkenreferenceslots::ReferenceSlot::OutputBus)];
                    occ.outputBusResolved =
                        busSlot.outcome ==
                        harkenreferenceslots::ResolutionOutcome::Resolved;
                    occ.resolvedOutputBus = occ.outputBusResolved
                                                ? busSlot.value
                                                : kReservedBusId;
                    const harkenreferenceslots::ReferenceSlotResolution&
                        attenuationSlot = routingResolution.resolvedSlots[
                            static_cast<std::size_t>(
                                harkenreferenceslots::ReferenceSlot::Attenuation)];
                    occ.attenuationResolved =
                        attenuationSlot.outcome ==
                            harkenreferenceslots::ResolutionOutcome::Resolved &&
                        attenuationSlot.value != harkenstudio::ObjectId{};
                    occ.resolvedAttenuation =
                        occ.attenuationResolved ? attenuationSlot.value : harkenstudio::ObjectId{};
                    occ.sends = routingSends.sends;

                    // The selected effect chain must name this occurrence on the
                    // same terms. Only the selection verdict and the chain index
                    // cross into the IR; the chain's content lives once in the
                    // table the index addresses.
                    const auto chainIt = effectChainByOccurrence.find(routingKey);
                    if (chainIt == effectChainByOccurrence.end()) {
                        return std::nullopt;
                    }
                    const OccurrenceEffectChainSelection& chainSelection =
                        *chainIt->second;
                    if (chainSelection.occurrenceOrdinal != ordinal ||
                        chainSelection.subjectId != occ.content.target) {
                        return std::nullopt;
                    }
                    occ.hasEffectChain = chainSelection.declared;
                    occ.effectChainIndex =
                        chainSelection.declared ? chainSelection.chainIndex : 0;
                }

                occ.frameBatch.eventIndex = ordinal;
                TemplateEvent startEvent;
                startEvent.kind = TemplateEventKind::Start;
                startEvent.phasePriority = kPhasePriorityStart;
                occ.frameBatch.events.push_back(startEvent);
                if (occ.hasDuration) {
                    TemplateEvent stopEvent;
                    stopEvent.kind = TemplateEventKind::Stop;
                    stopEvent.phasePriority = kPhasePriorityStop;
                    occ.frameBatch.events.push_back(stopEvent);
                }

                irTrack.occurrences.push_back(occ);
                ++ordinal;
                ++canonicalOccurrenceCount;
            }
            tracks.push_back(std::move(irTrack));
        } else if (obj.kind == ContentKind::Piece) {
            const PieceDef& piece = obj.piece;
            IrPiece irPiece;
            irPiece.pieceId = piece.chassis.id;
            irPiece.guid = piece.chassis.guid;
            irPiece.timeSpec = piece.timeSpec;
            irPiece.tracks = piece.tracks;

            std::map<StableId, const Marker*> markerById;
            for (const Marker& marker : piece.markers) {
                markerById.emplace(marker.id, &marker);
            }
            const auto factsIt = indexes->pieceFacts.find(piece.chassis.id);
            if (factsIt == indexes->pieceFacts.end()) {
                return std::nullopt;
            }
            const PieceNormalization& pn = *factsIt->second;
            irPiece.startKind = pn.startKind;
            irPiece.startCoordinate = pn.startCoordinate;
            irPiece.startMarkerId = pn.startMarkerId;
            for (const StableId markerStableId : pn.markersByMarkerId) {
                const auto markerIt = markerById.find(markerStableId);
                if (markerIt == markerById.end()) {
                    return std::nullopt;
                }
                const Marker& marker = *markerIt->second;
                IrMarker irMarker;
                irMarker.id = marker.id;
                irMarker.guid = marker.guid;
                irMarker.markerId = marker.markerId;
                irMarker.kind = marker.kind;
                irMarker.position = marker.position;
                irPiece.markers.push_back(irMarker);
            }
            pieces.push_back(std::move(irPiece));
        }
    }

    if (resolution != nullptr &&
        (resolutionByOccurrence.size() != canonicalOccurrenceCount ||
         sendsByOccurrence.size() != canonicalOccurrenceCount ||
         effectChainByOccurrence.size() != canonicalOccurrenceCount)) {
        return std::nullopt;
    }

    // Lineage: one entry per occurrence, tagged with the owning Piece when the
    // Track lives in one. Built from the IR tracks, so it inherits their order;
    // sorted afterward by the documented total key to be order-independent.
    RuntimeLineageTable lineage;
    for (const IrTrack& track : tracks) {
        const auto ownerIt = indexes->trackOwner.find(track.trackId);
        const bool hasPiece = ownerIt != indexes->trackOwner.end();
        for (const IrOccurrence& occ : track.occurrences) {
            LineageEntry entry;
            entry.hasPiece = hasPiece;
            entry.pieceId = hasPiece ? ownerIt->second : kUnmintedStableId;
            entry.trackId = track.trackId;
            entry.clipId = occ.key.clipId;
            entry.occurrenceOrdinal = occ.key.occurrenceOrdinal;
            lineage.entries.push_back(entry);
        }
    }
    std::sort(lineage.entries.begin(), lineage.entries.end(),
              [](const LineageEntry& a, const LineageEntry& b) {
                  if (a.trackId != b.trackId) {
                      return a.trackId < b.trackId;
                  }
                  return a.occurrenceOrdinal < b.occurrenceOrdinal;
              });

    const std::uint64_t fingerprint = FingerprintLineage(lineage);
    // The chain table crosses into the IR whole: occurrences reference it by
    // index, so the table an index was resolved against must be the one the IR
    // carries. A structure-only build has no selection and therefore no chains.
    std::vector<CompiledEffectChain> irChains;
    if (effectChains != nullptr) {
        irChains = effectChains->chains;
    }
    return detail::PieceIRBuilder::Build(
        std::move(tracks), std::move(pieces), std::move(lineage),
        std::move(irChains), fingerprint);
}

}  // namespace

std::optional<PieceIR> BuildPieceIR(
    const CanonicalProject& project, const NormalizedFacts& facts,
    const ResourceTable& resources,
    const ReferenceResolutionFacts& resolution,
    const SendResolutionFacts& sends,
    const EffectChainSelectionFacts& effectChains) {
    return BuildPieceIRImpl(project, facts, resources, &resolution, &sends,
                            &effectChains);
}

std::optional<PieceIR> BuildPieceIRForStructureOnly(
    const CanonicalProject& project, const NormalizedFacts& facts,
    const ResourceTable& resources) {
    return BuildPieceIRImpl(project, facts, resources, nullptr, nullptr,
                            nullptr);
}

}  // namespace harkenprojectschema
