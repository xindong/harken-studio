#include "harken_project_schema/BuildSegmentProgram.h"

#include "harken_project_schema/CapacityEstimate.h"
#include "harken_project_schema/detail/Fnv1a.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace harkenprojectschema {

namespace detail {

struct SegmentProgramBuilder {
    static SegmentProgram Build(SegmentIR ir, CapacityEstimate estimate,
                              CapabilityPrerequisites capabilities,
                              std::uint64_t fingerprint) {
        return SegmentProgram(std::move(ir), std::move(estimate), capabilities,
                            fingerprint);
    }
};

}  // namespace detail

namespace {

void Require(std::uint32_t* mask, ProgramCapability capability) {
    *mask |= (1u << static_cast<std::uint32_t>(capability));
}

// The capabilities a program needs are exactly those its content exercises: the
// timing kinds its Segments and standalone Tracks use, and the placement kinds its
// clips take. Nothing is required that the content does not contain.
CapabilityPrerequisites DeriveCapabilities(const SegmentIR& ir) {
    std::uint32_t mask = 0;

    std::set<StableId> tracksInSegments;
    for (const IrSegment& segment : ir.segments()) {
        if (segment.timeSpec.kind == SegmentTimeKind::Musical) {
            Require(&mask, ProgramCapability::MusicalTiming);
        } else if (segment.timeSpec.kind == SegmentTimeKind::Absolute) {
            Require(&mask, ProgramCapability::AbsoluteTiming);
        }
        for (const StableId trackId : segment.tracks) {
            tracksInSegments.insert(trackId);
        }
    }

    for (const IrTrack& track : ir.tracks()) {
        const bool standalone =
            tracksInSegments.find(track.trackId) == tracksInSegments.end();
        if (standalone && !track.occurrences.empty()) {
            // A directly triggered Track plays in absolute time.
            Require(&mask, ProgramCapability::AbsoluteTiming);
        }
        for (const IrOccurrence& occ : track.occurrences) {
            switch (occ.content.kind) {
                case ContentKind::Sound:
                    Require(&mask, ProgramCapability::NaturalLengthPlayback);
                    break;
                case ContentKind::Container:
                    Require(&mask, ProgramCapability::ContainerPlacement);
                    break;
                case ContentKind::Segment:
                    Require(&mask, ProgramCapability::NestedSegmentPlacement);
                    break;
                case ContentKind::Layer:
                case ContentKind::Track:
                    break;  // not legal clip targets; the front-end refused them
            }
            // An occurrence that selected a chain with entries needs a host that
            // runs per-voice insert effects. A selected but empty chain needs
            // nothing extra: overriding an inherited chain with no effects asks
            // the runtime for no capability it would not otherwise have.
            if (occ.hasEffectChain &&
                !ir.effectChains()[occ.effectChainIndex].entries.empty()) {
                Require(&mask, ProgramCapability::VoiceEffectChain);
            }
        }
    }

    CapabilityPrerequisites prerequisites;
    prerequisites.requiredMask = mask;
    return prerequisites;
}

// A rolling FNV-1a 64 state, hashing values little-endian so the result does not
// depend on host byte order.
struct Hasher {
    std::uint64_t state = detail::kFnv1a64OffsetBasis;

    void U64(std::uint64_t value) {
        for (int i = 0; i < 8; ++i) {
            detail::Fnv1a64HashByte(&state, static_cast<std::uint8_t>(value & 0xffu));
            value >>= 8;
        }
    }
    void I64(std::int64_t value) { U64(static_cast<std::uint64_t>(value)); }
    void Guid(const AuthoringGuid& guid) {
        U64(guid.high);
        U64(guid.low);
    }
};

// The semantic fingerprint. Object identities enter as authored GUIDs and the
// identity-bearing collections are GUID-ordered, so a minted-id renumbering
// that preserves the relative ClipId order does not move this hash while it
// still moves the lineage fingerprint. Lineage, raw stable ids, editorOrder,
// seekable and derived durationNs are excluded; SegmentProgram.h states the full
// coverage contract.
std::uint64_t ComputeProgramSemanticFingerprint(const SegmentIR& ir) {
    std::map<StableId, AuthoringGuid> trackGuids;
    for (const IrTrack& track : ir.tracks()) {
        trackGuids.emplace(track.trackId, track.guid);
    }

    std::vector<const IrSegment*> orderedSegments;
    orderedSegments.reserve(ir.segments().size());
    for (const IrSegment& segment : ir.segments()) {
        orderedSegments.push_back(&segment);
    }
    std::sort(orderedSegments.begin(), orderedSegments.end(),
              [](const IrSegment* a, const IrSegment* b) {
                  return a->guid < b->guid;
              });

    std::vector<const IrTrack*> orderedTracks;
    orderedTracks.reserve(ir.tracks().size());
    for (const IrTrack& track : ir.tracks()) {
        orderedTracks.push_back(&track);
    }
    std::sort(orderedTracks.begin(), orderedTracks.end(),
              [](const IrTrack* a, const IrTrack* b) {
                  return a->guid < b->guid;
              });

    Hasher h;
    h.U64(orderedSegments.size());
    for (const IrSegment* segmentPtr : orderedSegments) {
        const IrSegment& segment = *segmentPtr;
        h.Guid(segment.guid);
        h.U64(static_cast<std::uint32_t>(segment.timeSpec.kind));
        h.U64(segment.timeSpec.tempoMap.size());
        for (const TempoMapEntry& tempo : segment.timeSpec.tempoMap) {
            h.I64(tempo.tick);
            h.I64(tempo.microsPerQuarter);
        }
        h.U64(segment.timeSpec.meterMap.size());
        for (const MeterMapEntry& meter : segment.timeSpec.meterMap) {
            h.I64(meter.tick);
            h.U64(meter.numerator);
            h.U64(meter.denominator);
        }
        h.U64(static_cast<std::uint32_t>(segment.startKind));
        h.I64(segment.startCoordinate);
        h.U64(segment.startMarkerId);
        h.U64(segment.markers.size());
        for (const IrMarker& marker : segment.markers) {
            h.Guid(marker.guid);
            h.U64(marker.markerId);
            h.U64(static_cast<std::uint32_t>(marker.kind));
            h.I64(marker.position);
        }
        h.U64(segment.tracks.size());
        std::vector<AuthoringGuid> segmentTrackGuids;
        segmentTrackGuids.reserve(segment.tracks.size());
        for (const StableId trackId : segment.tracks) {
            segmentTrackGuids.push_back(trackGuids.at(trackId));
        }
        std::sort(segmentTrackGuids.begin(), segmentTrackGuids.end());
        for (const AuthoringGuid& guid : segmentTrackGuids) {
            h.Guid(guid);
        }
    }
    h.U64(orderedTracks.size());
    for (const IrTrack* trackPtr : orderedTracks) {
        const IrTrack& track = *trackPtr;
        h.Guid(track.guid);
        h.U64(track.occurrences.size());
        for (const IrOccurrence& occ : track.occurrences) {
            h.Guid(occ.clipGuid);
            h.U64(static_cast<std::uint32_t>(occ.content.kind));
            h.Guid(occ.contentTargetGuid);
            h.I64(occ.time);
            h.I64(occ.authoredDuration);
            h.I64(occ.fades.inLength);
            h.I64(occ.fades.outLength);
            h.U64(occ.semanticOrderKey);
            h.U64(occ.hasDuration ? 1u : 0u);
            h.U64(occ.durationFrames);
            h.U64(occ.resourceSampleRate);
            h.U64(occ.frameBatch.eventIndex);
            h.U64(occ.frameBatch.events.size());
            for (const TemplateEvent& ev : occ.frameBatch.events) {
                h.U64(ev.phasePriority);
                h.U64(static_cast<std::uint32_t>(ev.kind));
            }
            // The folded start-time base values are audible: two programs that
            // differ only in a folded gain, pitch or loop count render differently, so
            // they belong in the audible fingerprint. Floats hash as their raw
            // bits, never through a decimal or a double conversion.
            h.U64(std::bit_cast<std::uint32_t>(occ.baseVolumeDb));
            h.U64(std::bit_cast<std::uint32_t>(occ.outputBusVolumeDb));
            h.U64(std::bit_cast<std::uint32_t>(occ.basePitchRatio));
            h.U64(occ.loopCount);
            // The routing facts are program semantics: where a sound goes — its
            // merged sends and its resolved output bus — is part of what the
            // program means, so a change to it moves this hash. The target bus
            // ids are authoring-side external identities (this compiler mints no
            // bus id), so hashing them as raw values is not the excluded raw
            // stable id; send levels hash as their raw float bits.
            h.U64(occ.sends.size());
            for (const Send& send : occ.sends) {
                h.Guid(send.targetBusId);
                h.U64(std::bit_cast<std::uint32_t>(send.sendLevel));
            }
            h.U64(occ.outputBusResolved ? 1u : 0u);
            h.Guid(occ.resolvedOutputBus);
            h.U64(std::bit_cast<std::uint32_t>(occ.panning));
            h.U64(std::bit_cast<std::uint32_t>(occ.spread));
            h.U64(occ.is3DEnabled ? 1u : 0u);
            h.U64(std::bit_cast<std::uint32_t>(occ.attenuationDistanceScaling));
            h.U64(occ.attenuationResolved ? 1u : 0u);
            h.Guid(occ.resolvedAttenuation);
            // The selected effect chain is audible: which effects run, in what
            // order, with what parameter values is exactly what a voice sounds
            // like. What is hashed is the chain the occurrence dereferences, not
            // how it is addressed — the chain index and each entry's parameter
            // block index are storage layout, so two programs that differ only in
            // how identical chains are numbered are the same program. The authored
            // node id is excluded for the same reason it is excluded elsewhere:
            // renumbering authored nodes changes lineage, not sound. Physical slot
            // numbers cannot appear here at all — this stage does not know them,
            // and a chain lowered onto different slot layouts is one chain.
            h.U64(occ.hasEffectChain ? 1u : 0u);
            if (occ.hasEffectChain) {
                const CompiledEffectChain& chain =
                    ir.effectChains()[occ.effectChainIndex];
                h.U64(chain.entries.size());
                for (const CompiledEffectChainEntry& entry : chain.entries) {
                    h.U64(entry.effectType);
                    h.U64(entry.flags);
                    // The parameter payload in declared order: raw bits only, so
                    // no decimal or double conversion can enter a value that the
                    // model already holds as bits.
                    const EffectParamBlock& block =
                        chain.paramBlocks[entry.paramBlockRef];
                    h.U64(block.params.size());
                    for (const EffectParam& param : block.params) {
                        h.U64(param.paramId);
                        h.U64(param.valueBits);
                    }
                }
            }
        }
    }
    return h.state;
}

}  // namespace

std::optional<SegmentProgram> BuildSegmentProgram(const SegmentIR& ir) {
    std::optional<CapacityEstimate> estimate = EstimateCapacity(ir);
    if (!estimate.has_value()) {
        return std::nullopt;  // an unestimable IR forms no program
    }
    const CapabilityPrerequisites capabilities = DeriveCapabilities(ir);
    const std::uint64_t fingerprint = ComputeProgramSemanticFingerprint(ir);
    return detail::SegmentProgramBuilder::Build(ir, std::move(*estimate),
                                              capabilities, fingerprint);
}

}  // namespace harkenprojectschema
