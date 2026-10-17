#include "harken_project_schema/Serialize.h"

#include "harken_project_schema/Diagnostics.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>

namespace harkenprojectschema {
namespace {

std::string U(std::uint64_t v) { return std::to_string(v); }
std::string U(harkenstudio::ObjectId v) { return harkenstudio::ObjectIdString(v); }
std::string I(std::int64_t v) { return std::to_string(v); }

// A float is rendered as its raw IEEE-754 bit pattern, an unsigned integer,
// rather than as a decimal. The chassis stores float values verbatim and this
// serializer's contract is byte-reproducibility, so emitting the exact bits
// keeps rendering free of any locale or rounding choice — the same reason the
// tempo map is stored as an integer microseconds-per-quarter instead of a float.
// It reads as bits rather than a friendly number, the honest trade for an exact,
// reproducible rendering of a stored value the model does not interpret.
std::string F(float v) {
    return std::to_string(std::bit_cast<std::uint32_t>(v));
}

std::string Guid(const AuthoringGuid& g) {
    return U(g.high) + ":" + U(g.low);
}

const char* KindName(ContentKind kind) {
    switch (kind) {
        case ContentKind::Layer:
            return "layer";
        case ContentKind::Sound:
            return "sound";
        case ContentKind::Container:
            return "container";
        case ContentKind::Track:
            return "track";
        case ContentKind::Piece:
            return "piece";
    }
    return "?";
}

const char* TimeKindName(PieceTimeKind kind) {
    switch (kind) {
        case PieceTimeKind::Absolute:
            return "absolute";
        case PieceTimeKind::Musical:
            return "musical";
    }
    return "?";
}

const char* StartKindName(StartPointKind kind) {
    switch (kind) {
        case StartPointKind::Entry:
            return "entry";
        case StartPointKind::Marker:
            return "marker";
    }
    return "?";
}

const char* MarkerKindName(MarkerKind kind) {
    switch (kind) {
        case MarkerKind::Custom:
            return "custom";
        case MarkerKind::Exit:
            return "exit";
    }
    return "?";
}

// Renders the effect graph in a fixed order: the declaration marker, then each
// node with its params (authored order), then the edges (authored order). Every
// value is an integer, so no float formatting enters here (param values are
// already raw bits).
//
// An undeclared object with an empty graph emits nothing, the zero-cost case. A
// declared graph always emits its marker even when it holds no nodes, because a
// declared empty graph and an absent one select differently and a renderer that
// collapsed them would prove a determinism it does not have.
void AppendSignalGraph(std::string* s, bool declared, const SignalGraph& g) {
    if (declared) {
        *s += "  fxchain declared=1\n";
    }
    for (const EffectNode& node : g.nodes) {
        *s += "  fxnode id=" + U(node.nodeId) + " object=" + U(node.effectId) + " type=" + U(node.effectType) +
              "\n";
        for (const EffectParam& param : node.params) {
            *s += "    param id=" + U(param.paramId) +
                  " bits=" + U(param.valueBits) + "\n";
        }
    }
    for (const GraphEdge& edge : g.edges) {
        *s += "  fxedge from=" + U(edge.fromNodeId) + " to=" +
              U(edge.toNodeId) + "\n";
    }
}

// The spatial group renders as one line right after the object line, in a fixed
// field order, floats as raw bits like the rest. A fully default spatial group
// emits nothing — the same zero-cost rule the empty graph follows, so a default
// object gains no spatial noise.
void AppendSpatial(std::string* s, const SpatialSpec& sp) {
    if (sp == SpatialSpec{}) {
        return;
    }
    *s += "  spatial panning=" + F(sp.panning) + " spread=" + F(sp.spread) +
          " is3D=" + U(sp.is3DEnabled ? 1 : 0) +
          " attenScale=" + F(sp.attenuationDistanceScaling) +
          " attenRef=" + U(sp.attenuationRef) + "\n";
}

void AppendReferenceSlots(std::string* s, const Chassis& c) {
    if (c.mainOutputBusRef == harkenstudio::ObjectId{}) {
        return;
    }
    *s += "  refs bus=" + U(c.mainOutputBusRef) + "\n";
}

void AppendChassis(std::string* s, const ContentObject& obj) {
    const Chassis& c = obj.chassis();
    *s += "object " + U(c.id) + " kind=" + KindName(obj.kind) +
          " guid=" + Guid(c.guid) + " editorOrder=" + I(c.editorOrder) +
          " volume=" + F(c.volume) + " pitch=" + F(c.pitch) +
          " makeUpGain=" + F(c.makeUpGain) + " loopCount=" + U(c.loopCount) +
          " streamingMode=" + U(c.streamingMode) +
          " outputBusVolume=" + F(c.outputBusVolume) + "\n";
    AppendSpatial(s, c.spatial);
    AppendReferenceSlots(s, c);
    for (const Send& send : c.sends) {
        *s += "  send target=" + U(send.targetBusId) +
              " level=" + F(send.sendLevel) + "\n";
    }
    AppendSignalGraph(s, c.presence.Has(
        harkenplayableproperties::PlayablePropertyIdV1::SignalGraph),
        c.signalGraph);
}

void AppendOwned(std::string* s, const std::vector<StableId>& ids,
                 const char* label) {
    if (ids.empty()) {
        return;
    }
    *s += std::string("  ") + label + ":";
    for (const StableId id : ids) {
        *s += " " + U(id);
    }
    *s += "\n";
}

void AppendClip(std::string* s, const Clip& clip) {
    *s += "  clip " + U(clip.id) + " guid=" + Guid(clip.guid) +
          " ref=" + KindName(clip.contentRef.kind) + ":" +
          U(clip.contentRef.target) + " time=" + I(clip.time) +
          " duration=" + I(clip.duration) +
          " fadeIn=" + I(clip.fades.inLength) +
          " fadeOut=" + I(clip.fades.outLength) +
          " order=" + U(clip.semanticOrderKey) + "\n";
}

void AppendPieceTime(std::string* s, const PieceDef& piece) {
    *s += "  time kind=" + std::string(TimeKindName(piece.timeSpec.kind)) + "\n";
    for (const TempoMapEntry& t : piece.timeSpec.tempoMap) {
        *s += "  tempo tick=" + I(t.tick) + " uspq=" + I(t.microsPerQuarter) +
              "\n";
    }
    for (const MeterMapEntry& m : piece.timeSpec.meterMap) {
        *s += "  meter tick=" + I(m.tick) + " " + U(m.numerator) + "/" +
              U(m.denominator) + "\n";
    }
    *s += "  start kind=" + std::string(StartKindName(piece.startPoint.kind)) +
          " markerId=" + U(piece.startPoint.markerId) + "\n";
    if (piece.entryMarker) {
        *s += "  entry " + U(piece.entryMarker->id) +
              " guid=" + Guid(piece.entryMarker->guid) +
              " pos=" + I(piece.entryMarker->position) + "\n";
    }
    for (const Marker& marker : piece.markers) {
        *s += "  marker " + U(marker.id) + " guid=" + Guid(marker.guid) +
              " markerId=" + U(marker.markerId) + " kind=" + MarkerKindName(marker.kind) +
              " pos=" + I(marker.position) + "\n";
    }
}

}  // namespace

const char* DiagnosticCodeName(DiagnosticCode code) {
    switch (code) {
        case DiagnosticCode::ClipsOnNonTrack:
            return "ClipsOnNonTrack";
        case DiagnosticCode::TimeSpecOnNonPiece:
            return "TimeSpecOnNonPiece";
        case DiagnosticCode::StartPointOnNonPiece:
            return "StartPointOnNonPiece";
        case DiagnosticCode::MarkersOnNonPiece:
            return "MarkersOnNonPiece";
        case DiagnosticCode::EntryMarkerOnNonPiece:
            return "EntryMarkerOnNonPiece";
        case DiagnosticCode::PieceMissingTimeSpec:
            return "PieceMissingTimeSpec";
        case DiagnosticCode::PieceMissingStartPoint:
            return "PieceMissingStartPoint";
        case DiagnosticCode::ReservedMarkerId:
            return "ReservedMarkerId";
        case DiagnosticCode::StartPointMarkerReserved:
            return "StartPointMarkerReserved";
        case DiagnosticCode::StartPointMarkerMissing:
            return "StartPointMarkerMissing";
        case DiagnosticCode::MusicalMapEmpty:
            return "MusicalMapEmpty";
        case DiagnosticCode::NonMusicalMapPresent:
            return "NonMusicalMapPresent";
        case DiagnosticCode::NullGuid:
            return "NullGuid";
        case DiagnosticCode::InvalidContentKind:
            return "InvalidContentKind";
        case DiagnosticCode::InvalidPieceTimeKind:
            return "InvalidPieceTimeKind";
        case DiagnosticCode::InvalidStartPointKind:
            return "InvalidStartPointKind";
        case DiagnosticCode::InvalidMarkerKind:
            return "InvalidMarkerKind";
        case DiagnosticCode::DuplicateMarkerId:
            return "DuplicateMarkerId";
        case DiagnosticCode::EntryStartPointMarkerPresent:
            return "EntryStartPointMarkerPresent";
        case DiagnosticCode::SendTargetBusUnset:
            return "SendTargetBusUnset";
        case DiagnosticCode::GraphNodeIdReserved:
            return "GraphNodeIdReserved";
        case DiagnosticCode::GraphNodeIdDuplicate:
            return "GraphNodeIdDuplicate";
        case DiagnosticCode::GraphEffectTypeReserved:
            return "GraphEffectTypeReserved";
        case DiagnosticCode::GraphParamIdDuplicate:
            return "GraphParamIdDuplicate";
        case DiagnosticCode::GraphEdgeEndpointMissing:
            return "GraphEdgeEndpointMissing";
        case DiagnosticCode::GraphCycle:
            return "GraphCycle";
        case DiagnosticCode::UnknownPlayablePropertyBits:
            return "UnknownPlayablePropertyBits";
        case DiagnosticCode::NonCanonicalAbsentProperty:
            return "NonCanonicalAbsentProperty";
        case DiagnosticCode::OccurrencePropertiesOnNonContainer:
            return "OccurrencePropertiesOnNonContainer";
        case DiagnosticCode::OccurrenceOrdinalOverflow:
            return "OccurrenceOrdinalOverflow";
        case DiagnosticCode::ContainerChildOccurrencesOnNonContainer:
            return "ContainerChildOccurrencesOnNonContainer";
        case DiagnosticCode::DuplicateGuid:
            return "DuplicateGuid";
        case DiagnosticCode::DuplicateStableId:
            return "DuplicateStableId";
        case DiagnosticCode::ReservedStableId:
            return "ReservedStableId";
        case DiagnosticCode::MintExhausted:
            return "MintExhausted";
        case DiagnosticCode::DanglingOwnership:
            return "DanglingOwnership";
        case DiagnosticCode::IllegalOwnership:
            return "IllegalOwnership";
        case DiagnosticCode::MultipleOwners:
            return "MultipleOwners";
        case DiagnosticCode::RootAlsoOwned:
            return "RootAlsoOwned";
        case DiagnosticCode::OwnedNotRootNorOwned:
            return "OwnedNotRootNorOwned";
        case DiagnosticCode::StructuralCycle:
            return "StructuralCycle";
        case DiagnosticCode::UnknownRoot:
            return "UnknownRoot";
        case DiagnosticCode::DuplicateRoot:
            return "DuplicateRoot";
        case DiagnosticCode::DuplicateOwnershipEdge:
            return "DuplicateOwnershipEdge";
        case DiagnosticCode::DuplicateOccurrenceOrdinal:
            return "DuplicateOccurrenceOrdinal";
        case DiagnosticCode::DanglingContentRef:
            return "DanglingContentRef";
        case DiagnosticCode::IllegalContentRefType:
            return "IllegalContentRefType";
        case DiagnosticCode::ContainerOccurrenceCycle:
            return "ContainerOccurrenceCycle";
        case DiagnosticCode::MusicalMapNotSingletonAtZero:
            return "MusicalMapNotSingletonAtZero";
        case DiagnosticCode::MultipleExitMarkers:
            return "MultipleExitMarkers";
        case DiagnosticCode::NegativeClipDuration:
            return "NegativeClipDuration";
        case DiagnosticCode::ClipEndOverflow:
            return "ClipEndOverflow";
        case DiagnosticCode::InvalidPieceTimelineBounds:
            return "InvalidPieceTimelineBounds";
        case DiagnosticCode::EntryScheduleCapabilityUnavailable:
            return "EntryScheduleCapabilityUnavailable";
        case DiagnosticCode::ReferenceChainDepthExceeded:
            return "ReferenceChainDepthExceeded";
        case DiagnosticCode::SendCountExceedsLayoutLimit:
            return "SendCountExceedsLayoutLimit";
        case DiagnosticCode::EffectChainNodeNotLinear:
            return "EffectChainNodeNotLinear";
        case DiagnosticCode::EffectChainNotConnected:
            return "EffectChainNotConnected";
        case DiagnosticCode::EffectChainExceedsSlotLimit:
            return "EffectChainExceedsSlotLimit";
        case DiagnosticCode::EffectGraphContentWithoutDeclaration:
            return "EffectGraphContentWithoutDeclaration";
    }
    return "Unknown";
}

std::string RenderProject(const CanonicalProject& project) {
    std::string s = "project\n";
    for (const ContentObject& obj : project.objects) {
        AppendChassis(&s, obj);
        switch (obj.kind) {
            case ContentKind::Layer:
                AppendOwned(&s, obj.layer.ownedChildren, "owns");
                break;
            case ContentKind::Sound:
                break;
            case ContentKind::Container:
                AppendOwned(&s, obj.container.ownedChildren, "owns");
                if (!obj.container.children.empty()) {
                    s += "  children:";
                    for (const ContainerChildOccurrence& child :
                         obj.container.children) {
                        s += " " + U(child.childPlayableRef.target);
                    }
                    s += "\n";
                }
                break;
            case ContentKind::Track:
                AppendOwned(&s, obj.track.ownedChildren, "owns");
                for (const Clip& clip : obj.track.clips) {
                    AppendClip(&s, clip);
                }
                break;
            case ContentKind::Piece:
                AppendOwned(&s, obj.piece.tracks, "tracks");
                AppendPieceTime(&s, obj.piece);
                break;
        }
    }
    s += "roots:";
    for (const StableId id : project.roots) {
        s += " " + U(id);
    }
    s += "\n";
    return s;
}

std::string RenderDiagnostics(const std::vector<Diagnostic>& diagnostics) {
    std::string s = "diagnostics " + U(diagnostics.size()) + "\n";
    for (const Diagnostic& d : diagnostics) {
        s += std::string(DiagnosticCodeName(d.code)) +
             " primary=" + Guid(d.primary) + " secondary=" + Guid(d.secondary) +
             " detail=" + U(d.detail);
        if (!d.path.empty()) {
            s += " path=[";
            for (std::size_t i = 0; i < d.path.size(); ++i) {
                if (i != 0) {
                    s += ",";
                }
                s += Guid(d.path[i]);
            }
            s += "]";
        }
        s += "\n";
    }
    return s;
}

namespace {

const char* SlotName(harkenreferenceslots::ReferenceSlot slot) {
    const harkenreferenceslots::ReferenceSlotMeta* meta =
        harkenreferenceslots::FindReferenceSlot(slot);
    return meta != nullptr ? meta->name : "?";
}

const char* LevelName(harkenreferenceslots::ResolutionLevel level) {
    switch (level) {
        case harkenreferenceslots::ResolutionLevel::ObjectOverride:
            return "objectOverride";
        case harkenreferenceslots::ResolutionLevel::PlacementAncestor:
            return "placementAncestor";
        case harkenreferenceslots::ResolutionLevel::OrganizationalAncestor:
            return "organizationalAncestor";
        case harkenreferenceslots::ResolutionLevel::SlotDefault:
            return "slotDefault";
    }
    return "?";
}

const char* OutcomeName(harkenreferenceslots::ResolutionOutcome outcome) {
    switch (outcome) {
        case harkenreferenceslots::ResolutionOutcome::Resolved:
            return "resolved";
        case harkenreferenceslots::ResolutionOutcome::Unresolved:
            return "unresolved";
        case harkenreferenceslots::ResolutionOutcome::DepthCeilingExceeded:
            return "depthCeilingExceeded";
    }
    return "?";
}

}  // namespace

std::string RenderReferenceResolution(const ReferenceResolutionFacts& facts) {
    std::string s = "resolution " + U(facts.occurrences.size()) + "\n";
    for (const OccurrenceReferenceResolution& occ : facts.occurrences) {
        s += "occ track=" + U(occ.trackId) + " clip=" + U(occ.clipId) +
             " ord=" + U(occ.occurrenceOrdinal) +
             " subject=" + KindName(occ.subjectKind) + ":" + U(occ.subjectId) +
             "\n";
        for (std::uint32_t i = 0;
             i < harkenreferenceslots::kReferenceSlotCount; ++i) {
            const harkenreferenceslots::ReferenceSlotResolution& slot =
                occ.resolvedSlots[i];
            const auto slotId = static_cast<harkenreferenceslots::ReferenceSlot>(i);
            s += "  slot=" + std::string(SlotName(slotId)) +
                 " outcome=" + OutcomeName(slot.outcome);
            if (slot.outcome ==
                harkenreferenceslots::ResolutionOutcome::Resolved) {
                s += " level=" + std::string(LevelName(slot.level)) +
                     " value=" + U(slot.value) +
                     " src=" + U(slot.sourceObjectId) +
                     " depth=" + U(slot.ancestorDepth);
            }
            s += " pw=" + U(slot.placementDepthWalked) +
                 " ow=" + U(slot.organizationalDepthWalked);
            s += "\n";
            for (const harkenreferenceslots::ReferenceSlotCandidate& cand :
                 slot.candidates) {
                s += "    cand level=" + std::string(LevelName(cand.level)) +
                     " src=" + U(cand.sourceObjectId) +
                     " depth=" + U(cand.ancestorDepth) +
                     " value=" + U(cand.value) +
                     " win=" + U(cand.winner ? 1 : 0) + "\n";
            }
        }
    }
    return s;
}

std::string RenderSendResolution(const SendResolutionFacts& facts) {
    std::string s = "sends " + U(facts.occurrences.size()) + "\n";
    for (const OccurrenceSendResolution& occ : facts.occurrences) {
        s += "occ track=" + U(occ.trackId) + " clip=" + U(occ.clipId) +
             " ord=" + U(occ.occurrenceOrdinal) +
             " subject=" + U(occ.subjectId) + " sends=" + U(occ.sends.size());
        for (const Send& send : occ.sends) {
            s += " " + U(send.targetBusId) + "," + F(send.sendLevel);
        }
        s += "\n";
    }
    return s;
}

std::string RenderEffectChainSelection(
    const EffectChainSelectionFacts& facts) {
    // The chain table first, so a reader sees each artifact once with its demand,
    // then one line per occurrence naming the chain it selected and every
    // declaration that selection overrode.
    std::string s = "chains " + U(facts.chains.size()) + "\n";
    for (const CompiledEffectChain& chain : facts.chains) {
        s += "chain owner=" + U(chain.owner) +
             " entries=" + U(chain.entries.size());
        for (const CompiledEffectChainEntry& e : chain.entries) {
            s += " " + U(e.stableNodeId) + "," + U(e.effectType) + "," +
                 U(e.flags) + ",b" + U(e.paramBlockRef);
        }
        s += " demand len=" + U(chain.resources.maxChainLength) + " caps=" +
             U(chain.resources.requiredCapabilities) + " types:";
        for (const EffectTypeDemand& d : chain.resources.perTypeDemand) {
            s += " " + U(d.effectType) + "x" + U(d.instanceCount) + "/p" +
                 U(d.requiredParameterCount);
        }
        s += "\n";
        for (std::size_t i = 0; i < chain.paramBlocks.size(); ++i) {
            s += "  block " + U(i) + ":";
            for (const EffectParam& param : chain.paramBlocks[i].params) {
                s += " " + U(param.paramId) + "=" + U(param.valueBits);
            }
            s += "\n";
        }
    }
    s += "chainsel " + U(facts.occurrences.size()) + "\n";
    for (const OccurrenceEffectChainSelection& occ : facts.occurrences) {
        s += "occ track=" + U(occ.trackId) + " clip=" + U(occ.clipId) +
             " ord=" + U(occ.occurrenceOrdinal) +
             " subject=" + U(occ.subjectId);
        if (occ.declared) {
            s += " owner=" + U(occ.owner) + " chain=" + U(occ.chainIndex);
        } else {
            s += " owner=none";
        }
        s += " shadowed:";
        for (const StableId id : occ.shadowedOwners) {
            s += " " + U(id);
        }
        s += "\n";
    }
    return s;
}

std::string RenderNormalizedFacts(const NormalizedFacts& facts) {
    std::string s = "facts\n";
    for (const PieceNormalization& p : facts.pieces) {
        s += "piece " + U(p.piece) +
             " start=" + std::string(StartKindName(p.startKind)) +
             " coord=" + I(p.startCoordinate) + " markerId=" + U(p.startMarkerId);
        if (p.timelineSemantics == PieceTimelineSemantics::Entry19)
            s += " semantics=Entry19 content=[" + I(p.bounds.contentStart) + "," +
                 I(p.bounds.contentEnd) + ") nominal=[" + I(p.bounds.nominalStart) +
                 "," + I(p.bounds.nominalEnd) + ")";
        s += " markers:";
        for (const StableId id : p.markersByMarkerId) {
            s += " " + U(id);
        }
        s += "\n";
    }
    for (const TrackNormalization& t : facts.tracks) {
        s += "track " + U(t.track) + " clips:";
        for (const StableId id : t.clipsByOrder) {
            s += " " + U(id);
        }
        s += "\n";
    }
    return s;
}

std::string RenderPieceIR(const PieceIR& ir) {
    std::string s = "ir\n";
    for (const IrTrack& t : ir.tracks()) {
        s += "track " + U(t.trackId) + " occ:";
        for (const IrOccurrence& o : t.occurrences) {
            s += " " + U(o.key.clipId) + "#" + U(o.key.occurrenceOrdinal) +
                 "@" + I(o.time) + ":" + KindName(o.content.kind) + ":" +
                 U(o.content.target) + "/f" + I(o.fades.inLength) + "," +
                 I(o.fades.outLength) + "/ord" + U(o.semanticOrderKey) +
                 "/clipdur" + I(o.authoredDuration);
            if (o.hasDuration) {
                s += "/dur" + U(o.durationFrames) + "@" +
                     U(o.resourceSampleRate) + "/ns" + I(o.durationNs) +
                     (o.seekable ? "/seek" : "/noseek");
            } else {
                s += "/nodur";
            }
            // Folded base values, floats as raw bits like the rest of the
            // determinism-facing rendering.
            s += "/fold" + F(o.baseVolumeDb) + "," + F(o.basePitchRatio) + "," +
                 U(o.loopCount) + "," + F(o.outputBusVolumeDb);
            // Routing facts: the resolved output bus (or none) then the merged
            // send list, each send as target bus and level bits.
            s += "/bus" +
                 (o.outputBusResolved ? U(o.resolvedOutputBus)
                                      : std::string("none"));
            s += "/sends" + U(o.sends.size());
            for (const Send& send : o.sends) {
                s += ":" + U(send.targetBusId) + "," + F(send.sendLevel);
            }
            s += "/spatial" + F(o.panning) + "," + F(o.spread) + "," +
                 U(o.is3DEnabled ? 1 : 0) + "," +
                 F(o.attenuationDistanceScaling);
            s += "/atten" +
                 (o.attenuationResolved ? U(o.resolvedAttenuation)
                                        : std::string("none"));
            // The selected effect chain, rendered through the reference so the
            // occurrence line shows the chain that will run rather than the index
            // it is stored under. A declared chain always renders, including the
            // empty one, because selecting an empty chain and selecting none are
            // different facts.
            if (!o.hasEffectChain) {
                s += "/chainnone";
            } else {
                const CompiledEffectChain& chain =
                    ir.effectChains()[o.effectChainIndex];
                s += "/chain" + U(chain.owner) + "#" + U(chain.entries.size());
                for (const CompiledEffectChainEntry& e : chain.entries) {
                    s += ":" + U(e.stableNodeId) + "," + U(e.effectType) + "," +
                         U(e.flags);
                    const EffectParamBlock& block =
                        chain.paramBlocks[e.paramBlockRef];
                    for (const EffectParam& param : block.params) {
                        s += "," + U(param.paramId) + "=" + U(param.valueBits);
                    }
                }
            }
            s += " ev" + U(o.frameBatch.eventIndex) + ":";
            for (const TemplateEvent& ev : o.frameBatch.events) {
                s += U(ev.phasePriority) + ".";
            }
        }
        s += "\n";
    }
    for (const IrPiece& p : ir.pieces()) {
        s += "piece " + U(p.pieceId) +
             " time=" + std::string(TimeKindName(p.timeSpec.kind)) +
             " start=" + std::string(StartKindName(p.startKind)) +
             " coord=" + I(p.startCoordinate) + " markerId=" + U(p.startMarkerId);
        if (p.timelineSemantics == PieceTimelineSemantics::Entry19)
            s += " semantics=Entry19 content=[" + I(p.bounds.contentStart) + "," +
                 I(p.bounds.contentEnd) + ") nominal=[" + I(p.bounds.nominalStart) +
                 "," + I(p.bounds.nominalEnd) + ")";
        s += " markers:";
        for (const IrMarker& c : p.markers) {
            s += " " + U(c.id) + "/" + U(c.markerId) + ":" +
                 std::string(MarkerKindName(c.kind)) + "@" + I(c.position);
        }
        s += " tracks:";
        for (const StableId id : p.tracks) {
            s += " " + U(id);
        }
        s += "\n";
        if (p.entryMarker) {
            s += "  entry " + U(p.entryMarker->id) + "@" + I(p.entryMarker->position) + "\n";
        }
        for (const TempoMapEntry& tempo : p.timeSpec.tempoMap) {
            s += "  tempo tick=" + I(tempo.tick) +
                 " uspq=" + I(tempo.microsPerQuarter) + "\n";
        }
        for (const MeterMapEntry& meter : p.timeSpec.meterMap) {
            s += "  meter tick=" + I(meter.tick) + " " +
                 U(meter.numerator) + "/" + U(meter.denominator) + "\n";
        }
    }
    s += "lineage " + U(ir.lineage().entries.size()) + "\n";
    for (const LineageEntry& e : ir.lineage().entries) {
        s += "  track=" + U(e.trackId) + " clip=" + U(e.clipId) +
             " ord=" + U(e.occurrenceOrdinal);
        if (e.hasPiece) {
            s += " piece=" + U(e.pieceId);
        }
        s += "\n";
    }
    s += "lineageFingerprint " + U(ir.runtimeLineageFingerprint()) + "\n";
    return s;
}

std::string RenderCapacityEstimate(const CapacityEstimate& estimate) {
    std::string s = "estimate active=" + U(estimate.activeUpperBound) +
                    " lowerBound=" + U(estimate.activeIsLowerBound ? 1 : 0) +
                    " unknown=" + U(estimate.unknownDurationOccurrences) + "\n";
    for (const ScopeCapacity& scope : estimate.scopes) {
        s += "scope " + U(scope.scopeId) +
             " piece=" + U(scope.isPiece ? 1 : 0) +
             " peak=" + U(scope.peakActiveOccurrences) +
             " unknown=" + U(scope.unknownDurationOccurrences) + "\n";
    }
    return s;
}

const char* CapabilityName(ProgramCapability capability) {
    switch (capability) {
        case ProgramCapability::MusicalTiming:
            return "musicalTiming";
        case ProgramCapability::AbsoluteTiming:
            return "absoluteTiming";
        case ProgramCapability::NaturalLengthPlayback:
            return "naturalLengthPlayback";
        case ProgramCapability::ContainerPlacement:
            return "containerPlacement";
        case ProgramCapability::NestedPiecePlacement:
            return "nestedPiecePlacement";
        case ProgramCapability::VoiceEffectChain:
            return "voiceEffectChain";
    }
    return "?";
}

std::string RenderPieceProgram(const PieceProgram& program) {
    std::string s = "program semanticFingerprint=" +
                    U(program.programSemanticFingerprint()) +
                    " lineageFingerprint=" +
                    U(program.runtimeLineageFingerprint()) + "\n";
    s += "capabilities:";
    for (std::uint32_t i = 0; i < kProgramCapabilityCount; ++i) {
        const ProgramCapability capability = static_cast<ProgramCapability>(i);
        if (program.capabilities().Requires(capability)) {
            s += std::string(" ") + CapabilityName(capability);
        }
    }
    s += "\n";
    s += RenderPieceIR(program.ir());
    s += RenderCapacityEstimate(program.capacityEstimate());
    return s;
}

std::string RenderResult(const FrontEndResult& result) {
    std::string s = RenderDiagnostics(result.diagnostics);
    if (result.project.has_value()) {
        s += RenderProject(*result.project);
    } else {
        s += "no-project\n";
    }
    if (result.facts.has_value()) {
        s += RenderNormalizedFacts(*result.facts);
    }
    return s;
}

}  // namespace harkenprojectschema
