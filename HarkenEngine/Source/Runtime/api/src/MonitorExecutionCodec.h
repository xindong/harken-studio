#ifndef HARKEN_API_MONITOR_EXECUTION_CODEC_H
#define HARKEN_API_MONITOR_EXECUTION_CODEC_H

#include "MonitorExecutionBuilder.h"
#include "MonitorRecordPack.h"
#include <bit>
#include <span>
#include <type_traits>

namespace harken::monitor {

inline constexpr std::uint32_t kExecutionPayloadBytes = 432;
inline constexpr std::uint32_t kExecutionRecordBytes = PackedRecordBytes(kExecutionPayloadBytes);
static_assert(sizeof(MonitorExecutionFact) <= 384);
static_assert(sizeof(VnMonitorExecutionFactV1) == kExecutionPayloadBytes);
static_assert(kExecutionPayloadBytes <= 512 && kExecutionRecordBytes <= 832);

inline bool FactHas(const VnMonitorExecutionFactV1& fact, unsigned field) noexcept {
    if (field >= 128) return false;
    return ((field < 64 ? fact.validityLo : fact.validityHi) & (UINT64_C(1) << (field % 64))) != 0;
}

inline void SetFactField(VnMonitorExecutionFactV1& fact, unsigned field) noexcept {
    if (field < 64) fact.validityLo |= UINT64_C(1) << field;
    else if (field < 128) fact.validityHi |= UINT64_C(1) << (field - 64);
}

// Payload fields have a single explicit wire order. Integers are little endian;
// ObjectIds retain their canonical byte order, independent of native padding.
template<class Visitor, class Fact>
inline void VisitExecutionPayload(Visitor& visit, Fact& fact) noexcept {
    visit(fact.structSize); visit(fact.factKind); visit(fact.subjectNamespace); visit(fact.contentKind);
    visit(fact.validityLo); visit(fact.validityHi); visit(fact.timestampNs);
    visit(fact.projectId); visit(fact.contentRevision); visit(fact.factFlags); visit(fact.reserved0);
    const auto request = [&](auto& value) { visit(value.sequence); visit(value.operation); visit(value.origin); };
    request(fact.request); request(fact.playbackRequest); request(fact.causedByRequest);
    request(fact.controlRequest); request(fact.selectionRequest);
    const auto cause = [&](auto& value) {
        visit(value.action); visit(value.parentPlayable); visit(value.executionOrdinal); visit(value.reserved0);
    };
    cause(fact.control); cause(fact.selection);
    visit(fact.root); visit(fact.serviceLeaf); visit(fact.resourceOwner); visit(fact.logicalVoice); visit(fact.renderActivation);
    visit(fact.originEmitter); visit(fact.hostSession); visit(fact.hostLocalSequence); visit(fact.originHostRevision);
    visit(fact.currentStateEmitter); visit(fact.currentSpatialEmitter);
    visit(fact.originKind); visit(fact.hostOwnership); visit(fact.stateBinding); visit(fact.spatialBinding);
    visit(fact.terminalKind); visit(fact.result); visit(fact.readCutKey); visit(fact.readCutSequence);
    visit(fact.audioBlock); visit(fact.ownerEpoch); visit(fact.frameOffset); visit(fact.frameSpan);
    visit(fact.readCutOrdinalExclusive); visit(fact.reserved1);
    visit(fact.target.handle); visit(fact.target.projectId); visit(fact.target.objectId);
    visit(fact.target.worldGeneration); visit(fact.target.programSemanticFingerprint); visit(fact.target.contentRevision);
    visit(fact.target.kind); visit(fact.target.contentKind); visit(fact.target.scope); visit(fact.target.reserved0);
}

struct ExecutionPayloadWriter {
    std::uint8_t* bytes;
    std::size_t offset = 0;
    template<class T> void operator()(T value) noexcept {
        static_assert(std::is_integral_v<T>);
        const auto bits = static_cast<std::make_unsigned_t<T>>(value);
        for (unsigned i = 0; i < sizeof(T); ++i) bytes[offset++] = static_cast<std::uint8_t>(bits >> (8 * i));
    }
    void operator()(VnObjectId value) noexcept {
        const auto encoded = ObjectIdBytes(value);
        std::memcpy(bytes + offset, encoded.data(), encoded.size());
        offset += encoded.size();
    }
};

struct ExecutionPayloadReader {
    const std::uint8_t* bytes;
    std::size_t offset = 0;
    template<class T> void operator()(T& value) noexcept {
        static_assert(std::is_integral_v<T>);
        std::make_unsigned_t<T> bits = 0;
        for (unsigned i = 0; i < sizeof(T); ++i)
            bits |= static_cast<std::make_unsigned_t<T>>(bytes[offset++]) << (8 * i);
        value = std::bit_cast<T>(bits);
    }
    void operator()(VnObjectId& value) noexcept {
        value = ObjectIdFromBytes(std::span<const std::uint8_t, 16>(bytes + offset, 16));
        offset += 16;
    }
};

inline std::uint32_t SubjectEntityKind(unsigned subject) noexcept {
    switch (subject) {
    case VN_MONITOR_SUBJECT_PLAYABLE: return VN_MONITOR_ENTITY_PLAYABLE;
    case VN_MONITOR_SUBJECT_ACTION: return VN_MONITOR_ENTITY_ACTION;
    case VN_MONITOR_SUBJECT_LOGICAL_VOICE:
    case VN_MONITOR_SUBJECT_RENDER_ACTIVATION: return VN_MONITOR_ENTITY_VOICE;
    // Legacy entity kinds have no Emitter. The typed namespace is authoritative.
    case VN_MONITOR_SUBJECT_EMITTER: return VN_MONITOR_ENTITY_NONE;
    case VN_MONITOR_SUBJECT_QUEUE: return VN_MONITOR_ENTITY_QUEUE;
    case VN_MONITOR_SUBJECT_TRANSITION: return VN_MONITOR_ENTITY_TRANSITION;
    default: return VN_MONITOR_ENTITY_NONE;
    }
}

inline bool ProjectFactFields(const MonitorExecutionFact& source, std::uint64_t producer,
    std::uint64_t stream, VnMonitorRecordHeader& header, VnMonitorExecutionFactV1& fact) noexcept {
    header = {};
    fact = {};
    const bool call = source.BodyKind() == MonitorFactBody::Call;
    if (source.engine == 0 || source.sourceSequence == 0 || producer == 0 || stream == 0 ||
        source.kind == 0 || source.kind > VN_MONITOR_FACT_PLAYABLE_STARTED ||
        (source.flags & ~(kMonitorFactCallBody | kMonitorFactIdentityIncomplete | kMonitorFactControlIncomplete)) != 0 ||
        (source.validity & ~SourceAllowedFields(source.BodyKind())) != 0 ||
        (call && !IsCallFactKind(source.kind)) ||
        (!call && (source.kind == VN_MONITOR_FACT_REQUEST_SUBMITTED || source.kind == VN_MONITOR_FACT_REQUEST_RETURNED)))
        return false;
    const auto has = [&](unsigned field) { return SourceHas(source, field); };
    // The shared frame and position-tail slots cannot encode simultaneous arms.
    const bool audioDetail = has(VN_MONITOR_FACT_FIELD_FRAME_OFFSET) || has(VN_MONITOR_FACT_FIELD_FRAME_SPAN);
    const bool cutDetail = has(VN_MONITOR_FACT_FIELD_READ_CUT_ORDINAL);
    const bool ownerDetail = has(VN_MONITOR_FACT_FIELD_OWNER_EPOCH);
    if (unsigned(audioDetail) + unsigned(cutDetail) + unsigned(ownerDetail) > 1 ||
        (has(VN_MONITOR_FACT_FIELD_EXECUTE_FRAME) && has(VN_MONITOR_FACT_FIELD_GRAPH_FRAME)) ||
        (has(VN_MONITOR_FACT_FIELD_PARENT_PLAYABLE) && has(VN_MONITOR_FACT_FIELD_PARENT_ACTION))) return false;
    header.structSize = sizeof(header);
    header.streamKind = source.kind == VN_MONITOR_FACT_IDENTITY_UPSERT || source.kind == VN_MONITOR_FACT_IDENTITY_TOMBSTONE
        ? VN_MONITOR_STREAM_STATE : VN_MONITOR_STREAM_EVENT;
    header.streamId = stream;
    header.producerId = producer;
    header.engineMonitorInstanceId = source.engine;
    header.sourceSequence = source.sourceSequence;
    header.telemetry.structSize = sizeof(header.telemetry);
    fact.structSize = sizeof(fact);
    fact.factKind = source.kind;
    fact.subjectNamespace = source.subjectNamespace;
    fact.contentKind = source.contentKind;
    fact.factFlags = source.flags & (kMonitorFactIdentityIncomplete | kMonitorFactControlIncomplete);
    const auto copy = [&](unsigned field, auto& destination, const auto& value) {
        if (has(field)) { destination = value; SetFactField(fact, field); }
    };
    const auto& identity = call ? source.body.call.identity : source.body.execution.identity;
    copy(VN_MONITOR_FACT_FIELD_SUBJECT, header.instanceHandle, identity.handle);
    if (has(VN_MONITOR_FACT_FIELD_SUBJECT)) {
        header.instanceGeneration = SubjectGeneration(source.subjectNamespace, identity.handle);
        header.entityKind = SubjectEntityKind(source.subjectNamespace);
    }
    copy(VN_MONITOR_FACT_FIELD_PROJECT, fact.projectId, identity.project);
    copy(VN_MONITOR_FACT_FIELD_CONTENT_OBJECT, header.stableId, identity.object);
    copy(VN_MONITOR_FACT_FIELD_CONTENT_WORLD, header.worldGeneration, identity.world);
    copy(VN_MONITOR_FACT_FIELD_CONTENT_SEMANTIC, header.programSemanticFingerprint, identity.semantic);
    copy(VN_MONITOR_FACT_FIELD_CONTENT_LINEAGE, header.runtimeLineageFingerprint, identity.lineage);
    copy(VN_MONITOR_FACT_FIELD_CONTENT_REVISION, fact.contentRevision, identity.revision);
    copy(VN_MONITOR_FACT_FIELD_TIMESTAMP, fact.timestampNs, source.timestamp);
    copy(VN_MONITOR_FACT_FIELD_RESULT, fact.result, source.result);
    copy(VN_MONITOR_FACT_FIELD_REASON, header.reason, source.reason);
    copy(VN_MONITOR_FACT_FIELD_TERMINAL_KIND, fact.terminalKind, source.terminalKind);
    const auto request = [&](unsigned field, VnMonitorFactRequest& destination, const auto& value) {
        copy(field, destination.sequence, value.sequence);
        copy(field + 1, destination.operation, value.operation);
        copy(field + 2, destination.origin, value.origin);
    };
    if (call) {
        const auto& body = source.body.call;
        request(VN_MONITOR_FACT_FIELD_REQUEST, fact.request, body.request);
        request(VN_MONITOR_FACT_FIELD_CAUSED_BY_REQUEST, fact.causedByRequest, body.causedBy);
        copy(VN_MONITOR_FACT_FIELD_ROOT, fact.root, body.root);
        copy(VN_MONITOR_FACT_FIELD_RELATED_ACTION, header.actionInstanceHandle, body.action);
        const auto& target = body.target;
        if (target.reserved != 0) return false;
        copy(VN_MONITOR_FACT_FIELD_TARGET_HANDLE, fact.target.handle, target.handle);
        if (has(VN_MONITOR_FACT_FIELD_TARGET_VALIDATED)) SetFactField(fact, VN_MONITOR_FACT_FIELD_TARGET_VALIDATED);
        copy(VN_MONITOR_FACT_FIELD_TARGET_PROJECT, fact.target.projectId, target.project);
        copy(VN_MONITOR_FACT_FIELD_TARGET_OBJECT, fact.target.objectId, target.object);
        copy(VN_MONITOR_FACT_FIELD_TARGET_WORLD, fact.target.worldGeneration, target.world);
        copy(VN_MONITOR_FACT_FIELD_TARGET_SEMANTIC, fact.target.programSemanticFingerprint, target.semantic);
        copy(VN_MONITOR_FACT_FIELD_TARGET_REVISION, fact.target.contentRevision, target.revision);
        copy(VN_MONITOR_FACT_FIELD_TARGET_KIND, fact.target.kind, target.kind);
        copy(VN_MONITOR_FACT_FIELD_TARGET_CONTENT_KIND, fact.target.contentKind, target.contentKind);
        copy(VN_MONITOR_FACT_FIELD_TARGET_SCOPE, fact.target.scope, target.scope);
    } else {
        const auto& body = source.body.execution;
        const auto& requests = body.requests;
        if (requests.reserved != 0) return false;
        request(VN_MONITOR_FACT_FIELD_PLAYBACK_REQUEST, fact.playbackRequest,
            MonitorFactRequest{requests.playback, requests.playbackOperation, requests.playbackOrigin});
        request(VN_MONITOR_FACT_FIELD_CONTROL_REQUEST, fact.controlRequest,
            MonitorFactRequest{requests.control, requests.controlOperation, requests.controlOrigin});
        request(VN_MONITOR_FACT_FIELD_SELECTION_REQUEST, fact.selectionRequest,
            MonitorFactRequest{requests.selection, requests.selectionOperation, requests.selectionOrigin});
        copy(VN_MONITOR_FACT_FIELD_CONTROL_ACTION, fact.control.action, requests.controlAction);
        copy(VN_MONITOR_FACT_FIELD_CONTROL_PARENT, fact.control.parentPlayable, requests.controlParent);
        copy(VN_MONITOR_FACT_FIELD_CONTROL_ORDINAL, fact.control.executionOrdinal, requests.controlOrdinal);
        copy(VN_MONITOR_FACT_FIELD_SELECTION_ACTION, fact.selection.action, requests.selectionAction);
        copy(VN_MONITOR_FACT_FIELD_SELECTION_PARENT, fact.selection.parentPlayable, requests.selectionParent);
        copy(VN_MONITOR_FACT_FIELD_SELECTION_ORDINAL, fact.selection.executionOrdinal, requests.selectionOrdinal);
        copy(VN_MONITOR_FACT_FIELD_PARENT_PLAYABLE, header.playableInstanceHandle, body.relations.parent);
        copy(VN_MONITOR_FACT_FIELD_PARENT_ACTION, header.parentActionInstanceHandle, body.relations.parent);
        copy(VN_MONITOR_FACT_FIELD_ROOT, fact.root, body.relations.root);
        copy(VN_MONITOR_FACT_FIELD_RELATED_ACTION, header.actionInstanceHandle, body.relations.action);
        copy(VN_MONITOR_FACT_FIELD_SERVICE_LEAF, fact.serviceLeaf, body.relations.leaf);
        copy(VN_MONITOR_FACT_FIELD_RESOURCE_OWNER, fact.resourceOwner, body.relations.owner);
        copy(VN_MONITOR_FACT_FIELD_LOGICAL_VOICE, fact.logicalVoice, body.relations.logicalVoice);
        copy(VN_MONITOR_FACT_FIELD_RENDER_ACTIVATION, fact.renderActivation, body.relations.renderActivation);
        if (source.kind == VN_MONITOR_FACT_REQUEST_TERMINAL) {
            fact.request = fact.playbackRequest;
            for (unsigned i = 0; i != 3; ++i)
                if (FactHas(fact, VN_MONITOR_FACT_FIELD_PLAYBACK_REQUEST + i)) SetFactField(fact, VN_MONITOR_FACT_FIELD_REQUEST + i);
        }
    }
    const auto& emitters = call ? source.body.call.emitters : source.body.execution.emitters;
    copy(VN_MONITOR_FACT_FIELD_ORIGIN_EMITTER, fact.originEmitter, emitters.origin);
    copy(VN_MONITOR_FACT_FIELD_HOST_KEY, fact.hostSession, emitters.hostSession);
    copy(VN_MONITOR_FACT_FIELD_HOST_KEY, fact.hostLocalSequence, emitters.hostSequence);
    copy(VN_MONITOR_FACT_FIELD_HOST_REVISION, fact.originHostRevision, emitters.hostRevision);
    copy(VN_MONITOR_FACT_FIELD_CURRENT_STATE_EMITTER, fact.currentStateEmitter, emitters.currentState);
    copy(VN_MONITOR_FACT_FIELD_CURRENT_SPATIAL_EMITTER, fact.currentSpatialEmitter, emitters.currentSpatial);
    copy(VN_MONITOR_FACT_FIELD_ORIGIN_KIND, fact.originKind, source.originKind);
    copy(VN_MONITOR_FACT_FIELD_HOST_OWNERSHIP, fact.hostOwnership, source.hostOwnership);
    copy(VN_MONITOR_FACT_FIELD_STATE_BINDING, fact.stateBinding, source.bindingStates & 15u);
    copy(VN_MONITOR_FACT_FIELD_SPATIAL_BINDING, fact.spatialBinding, source.bindingStates >> 4);
    const auto& position = call ? source.body.call.position : source.body.execution.position;
    copy(VN_MONITOR_FACT_FIELD_LOGIC_COMMIT, header.telemetry.logicCommitSequence, position.logicCommit);
    copy(VN_MONITOR_FACT_FIELD_READ_CUT_KEY, fact.readCutKey, position.readCutKey);
    copy(VN_MONITOR_FACT_FIELD_READ_CUT_SEQUENCE, fact.readCutSequence, position.readCutSequence);
    copy(VN_MONITOR_FACT_FIELD_TRANSPORT, header.transportEpoch, position.transport);
    copy(VN_MONITOR_FACT_FIELD_EXECUTE_FRAME, header.executeFrame, position.frame);
    copy(VN_MONITOR_FACT_FIELD_GRAPH_FRAME, header.graphFrame, position.frame);
    copy(VN_MONITOR_FACT_FIELD_AUDIO_BLOCK, fact.audioBlock, position.audioBlock);
    copy(VN_MONITOR_FACT_FIELD_ARRANGING, header.telemetry.arrangingInstance, position.arranging);
    copy(VN_MONITOR_FACT_FIELD_CLIP, header.telemetry.clipId, position.clip);
    copy(VN_MONITOR_FACT_FIELD_LOOP, header.telemetry.loopIteration, position.loop);
    copy(VN_MONITOR_FACT_FIELD_OCCURRENCE, header.telemetry.occurrenceOrdinal, position.occurrence);
    if (audioDetail) {
        copy(VN_MONITOR_FACT_FIELD_FRAME_OFFSET, fact.frameOffset, position.detail.audio.offset);
        copy(VN_MONITOR_FACT_FIELD_FRAME_SPAN, fact.frameSpan, position.detail.audio.span);
    } else if (cutDetail) {
        if (position.detail.readCut.reserved != 0) return false;
        copy(VN_MONITOR_FACT_FIELD_READ_CUT_ORDINAL, fact.readCutOrdinalExclusive, position.detail.readCut.exclusiveOrdinal);
    } else if (ownerDetail) copy(VN_MONITOR_FACT_FIELD_OWNER_EPOCH, fact.ownerEpoch, position.detail.ownerEpoch);
    return true;
}

inline bool ProjectExecutionFact(const MonitorExecutionFact& source, std::uint64_t producer,
    std::uint64_t stream, VnMonitorRecordHeader& header, VnMonitorExecutionFactV1& fact) noexcept {
    if (!ProjectFactFields(source, producer, stream, header, fact)) return false;
    header.recordKind = VN_MONITOR_RECORD_EXECUTION_FACT;
    header.recordVersion = 1;
    header.flags = VN_MONITOR_RECORD_FLAG_OPTIONAL;
    header.recordBytes = kExecutionRecordBytes;
    header.payloadBytes = kExecutionPayloadBytes;
    return true;
}

// Check every optional value, including those carried by the envelope. Invalid
// fields must be zero, so stale source storage cannot leak through projection.
inline bool ValidateFactFields(const VnMonitorRecordHeader& header, const VnMonitorExecutionFactV1& fact) noexcept {
    const auto has = [&](unsigned field) { return FactHas(fact, field); };
    if (header.engineMonitorInstanceId == 0 ||
        header.producerId == 0 || header.sourceSequence == 0 || header.streamId == 0 ||
        header.reserved0 != 0 || header.telemetry.reserved0 != 0 ||
        fact.structSize != kExecutionPayloadBytes || fact.factKind == 0 || fact.factKind > VN_MONITOR_FACT_PLAYABLE_STARTED ||
        fact.subjectNamespace > VN_MONITOR_SUBJECT_TRANSITION || fact.contentKind > VN_MONITOR_CONTENT_MEDIA ||
        fact.reserved0 != 0 || fact.reserved1 != 0 || fact.control.reserved0 != 0 || fact.selection.reserved0 != 0 ||
        fact.target.reserved0 != 0 || (fact.factFlags & ~(VN_MONITOR_FACT_FLAG_IDENTITY_INCOMPLETE | VN_MONITOR_FACT_FLAG_CONTROL_INCOMPLETE)) != 0 ||
        (fact.validityHi >> (VN_MONITOR_FACT_FIELD_TARGET_SCOPE - 63)) != 0) return false;
    const auto expectedStream = fact.factKind == VN_MONITOR_FACT_IDENTITY_UPSERT || fact.factKind == VN_MONITOR_FACT_IDENTITY_TOMBSTONE
        ? VN_MONITOR_STREAM_STATE : VN_MONITOR_STREAM_EVENT;
    if (header.streamKind != expectedStream) return false;
    bool executionFields = false;
    for (unsigned i = VN_MONITOR_FACT_FIELD_PLAYBACK_REQUEST; i <= VN_MONITOR_FACT_FIELD_PARENT_ACTION; ++i)
        executionFields |= has(i);
    for (unsigned i = VN_MONITOR_FACT_FIELD_SERVICE_LEAF; i <= VN_MONITOR_FACT_FIELD_RENDER_ACTIVATION; ++i)
        executionFields |= has(i);
    const bool callFields = fact.validityHi != 0 || has(VN_MONITOR_FACT_FIELD_CAUSED_BY_REQUEST) ||
        has(VN_MONITOR_FACT_FIELD_CAUSED_BY_OPERATION) || has(VN_MONITOR_FACT_FIELD_CAUSED_BY_ORIGIN);
    if ((executionFields && callFields) ||
        (executionFields && (fact.factKind == VN_MONITOR_FACT_REQUEST_SUBMITTED || fact.factKind == VN_MONITOR_FACT_REQUEST_RETURNED)))
        return false;
    if (fact.factKind == VN_MONITOR_FACT_REQUEST_TERMINAL && executionFields) {
        for (unsigned i = 0; i != 3; ++i)
            if (has(VN_MONITOR_FACT_FIELD_REQUEST + i) != has(VN_MONITOR_FACT_FIELD_PLAYBACK_REQUEST + i)) return false;
        if (fact.request.sequence != fact.playbackRequest.sequence || fact.request.operation != fact.playbackRequest.operation ||
            fact.request.origin != fact.playbackRequest.origin) return false;
    }
    const auto optional = [&](unsigned field, auto value, bool validValue = true) {
        return has(field) ? validValue : value == decltype(value){};
    };
    const auto nonzero = [&](unsigned field, auto value) { return optional(field, value, value != decltype(value){}); };
    if (has(VN_MONITOR_FACT_FIELD_SUBJECT)) {
        if (!ValidSubjectHandle(fact.subjectNamespace, header.instanceHandle) ||
            header.instanceGeneration == 0 || header.instanceGeneration != SubjectGeneration(fact.subjectNamespace, header.instanceHandle) ||
            header.entityKind != SubjectEntityKind(fact.subjectNamespace)) return false;
    } else if (header.instanceHandle != 0 || header.instanceGeneration != 0 || header.entityKind != 0) return false;
    const auto handle = [&](unsigned field, std::uint64_t value, unsigned subject) {
        return optional(field, value, ValidSubjectHandle(subject, value));
    };
    if (!nonzero(VN_MONITOR_FACT_FIELD_PROJECT, fact.projectId) || !nonzero(VN_MONITOR_FACT_FIELD_CONTENT_OBJECT, header.stableId) ||
        !nonzero(VN_MONITOR_FACT_FIELD_CONTENT_WORLD, header.worldGeneration) ||
        !nonzero(VN_MONITOR_FACT_FIELD_CONTENT_SEMANTIC, header.programSemanticFingerprint) ||
        !nonzero(VN_MONITOR_FACT_FIELD_CONTENT_LINEAGE, header.runtimeLineageFingerprint) ||
        !nonzero(VN_MONITOR_FACT_FIELD_CONTENT_REVISION, fact.contentRevision) ||
        !optional(VN_MONITOR_FACT_FIELD_TIMESTAMP, fact.timestampNs) ||
        !optional(VN_MONITOR_FACT_FIELD_RESULT, fact.result) || !optional(VN_MONITOR_FACT_FIELD_REASON, header.reason) ||
        !optional(VN_MONITOR_FACT_FIELD_TERMINAL_KIND, fact.terminalKind,
            fact.subjectNamespace == VN_MONITOR_SUBJECT_PLAYABLE &&
            fact.terminalKind >= VN_MONITOR_TERMINAL_NATURAL_COMPLETION &&
            fact.terminalKind <= VN_MONITOR_TERMINAL_FAILURE)) return false;
    const auto request = [&](unsigned field, const VnMonitorFactRequest& value) {
        return nonzero(field, value.sequence) && optional(field + 1, value.operation, ValidRequestOperation(value.operation)) &&
            optional(field + 2, value.origin, ValidRequestOrigin(value.origin));
    };
    if (!request(VN_MONITOR_FACT_FIELD_REQUEST, fact.request) || !request(VN_MONITOR_FACT_FIELD_PLAYBACK_REQUEST, fact.playbackRequest) ||
        !request(VN_MONITOR_FACT_FIELD_CAUSED_BY_REQUEST, fact.causedByRequest) ||
        !request(VN_MONITOR_FACT_FIELD_CONTROL_REQUEST, fact.controlRequest) ||
        !request(VN_MONITOR_FACT_FIELD_SELECTION_REQUEST, fact.selectionRequest)) return false;
    const auto cause = [&](unsigned field, const VnMonitorFactCause& value) {
        return handle(field, value.action, VN_MONITOR_SUBJECT_ACTION) &&
            handle(field + 1, value.parentPlayable, VN_MONITOR_SUBJECT_PLAYABLE) &&
            optional(field + 2, value.executionOrdinal, value.executionOrdinal != UINT32_MAX);
    };
    if (!cause(VN_MONITOR_FACT_FIELD_CONTROL_ACTION, fact.control) || !cause(VN_MONITOR_FACT_FIELD_SELECTION_ACTION, fact.selection) ||
        !handle(VN_MONITOR_FACT_FIELD_PARENT_PLAYABLE, header.playableInstanceHandle, VN_MONITOR_SUBJECT_PLAYABLE) ||
        !handle(VN_MONITOR_FACT_FIELD_PARENT_ACTION, header.parentActionInstanceHandle, VN_MONITOR_SUBJECT_ACTION) ||
        !handle(VN_MONITOR_FACT_FIELD_ROOT, fact.root, fact.subjectNamespace == VN_MONITOR_SUBJECT_ACTION ? VN_MONITOR_SUBJECT_ACTION : VN_MONITOR_SUBJECT_PLAYABLE) ||
        !handle(VN_MONITOR_FACT_FIELD_RELATED_ACTION, header.actionInstanceHandle, VN_MONITOR_SUBJECT_ACTION) ||
        !handle(VN_MONITOR_FACT_FIELD_SERVICE_LEAF, fact.serviceLeaf, VN_MONITOR_SUBJECT_PLAYABLE) ||
        !handle(VN_MONITOR_FACT_FIELD_RESOURCE_OWNER, fact.resourceOwner, VN_MONITOR_SUBJECT_PLAYABLE) ||
        !handle(VN_MONITOR_FACT_FIELD_LOGICAL_VOICE, fact.logicalVoice, VN_MONITOR_SUBJECT_LOGICAL_VOICE) ||
        !handle(VN_MONITOR_FACT_FIELD_RENDER_ACTIVATION, fact.renderActivation, VN_MONITOR_SUBJECT_RENDER_ACTIVATION) ||
        !handle(VN_MONITOR_FACT_FIELD_ORIGIN_EMITTER, fact.originEmitter, VN_MONITOR_SUBJECT_EMITTER) || !nonzero(VN_MONITOR_FACT_FIELD_HOST_KEY, fact.hostSession) ||
        !nonzero(VN_MONITOR_FACT_FIELD_HOST_KEY, fact.hostLocalSequence) || !nonzero(VN_MONITOR_FACT_FIELD_HOST_REVISION, fact.originHostRevision) ||
        (has(VN_MONITOR_FACT_FIELD_HOST_REVISION) && !has(VN_MONITOR_FACT_FIELD_HOST_KEY)) ||
        !optional(VN_MONITOR_FACT_FIELD_ORIGIN_KIND, fact.originKind, fact.originKind > 0 && fact.originKind <= 4) ||
        !optional(VN_MONITOR_FACT_FIELD_HOST_OWNERSHIP, fact.hostOwnership,
            fact.hostOwnership > 0 && fact.hostOwnership <= VN_MONITOR_HOST_STUDIO_DEFAULT_EMITTER) ||
        !handle(VN_MONITOR_FACT_FIELD_CURRENT_STATE_EMITTER, fact.currentStateEmitter, VN_MONITOR_SUBJECT_EMITTER) ||
        !handle(VN_MONITOR_FACT_FIELD_CURRENT_SPATIAL_EMITTER, fact.currentSpatialEmitter, VN_MONITOR_SUBJECT_EMITTER) ||
        !optional(VN_MONITOR_FACT_FIELD_STATE_BINDING, fact.stateBinding, fact.stateBinding <= 2) ||
        !optional(VN_MONITOR_FACT_FIELD_SPATIAL_BINDING, fact.spatialBinding, fact.spatialBinding <= 2)) return false;
    if ((has(VN_MONITOR_FACT_FIELD_STATE_BINDING) && (fact.stateBinding == 1) != has(VN_MONITOR_FACT_FIELD_CURRENT_STATE_EMITTER)) ||
        (has(VN_MONITOR_FACT_FIELD_SPATIAL_BINDING) && (fact.spatialBinding == 1) != has(VN_MONITOR_FACT_FIELD_CURRENT_SPATIAL_EMITTER))) return false;
    if (!nonzero(VN_MONITOR_FACT_FIELD_LOGIC_COMMIT, header.telemetry.logicCommitSequence) ||
        !optional(VN_MONITOR_FACT_FIELD_READ_CUT_KEY, fact.readCutKey,
            (fact.readCutKey >> 32) != 0 && static_cast<std::uint32_t>(fact.readCutKey) != UINT32_MAX) ||
        !nonzero(VN_MONITOR_FACT_FIELD_READ_CUT_SEQUENCE, fact.readCutSequence) ||
        !optional(VN_MONITOR_FACT_FIELD_READ_CUT_ORDINAL, fact.readCutOrdinalExclusive) ||
        !optional(VN_MONITOR_FACT_FIELD_EXECUTE_FRAME, header.executeFrame) || !optional(VN_MONITOR_FACT_FIELD_GRAPH_FRAME, header.graphFrame) ||
        !nonzero(VN_MONITOR_FACT_FIELD_TRANSPORT, header.transportEpoch) || !optional(VN_MONITOR_FACT_FIELD_AUDIO_BLOCK, fact.audioBlock) ||
        !optional(VN_MONITOR_FACT_FIELD_FRAME_OFFSET, fact.frameOffset) || !optional(VN_MONITOR_FACT_FIELD_FRAME_SPAN, fact.frameSpan) ||
        !nonzero(VN_MONITOR_FACT_FIELD_ARRANGING, header.telemetry.arrangingInstance) || !nonzero(VN_MONITOR_FACT_FIELD_CLIP, header.telemetry.clipId) ||
        !optional(VN_MONITOR_FACT_FIELD_LOOP, header.telemetry.loopIteration) || !optional(VN_MONITOR_FACT_FIELD_OCCURRENCE, header.telemetry.occurrenceOrdinal) ||
        !nonzero(VN_MONITOR_FACT_FIELD_OWNER_EPOCH, fact.ownerEpoch)) return false;
    if (!optional(VN_MONITOR_FACT_FIELD_TARGET_HANDLE, fact.target.handle) ||
        !nonzero(VN_MONITOR_FACT_FIELD_TARGET_PROJECT, fact.target.projectId) || !nonzero(VN_MONITOR_FACT_FIELD_TARGET_OBJECT, fact.target.objectId) ||
        !nonzero(VN_MONITOR_FACT_FIELD_TARGET_WORLD, fact.target.worldGeneration) ||
        !nonzero(VN_MONITOR_FACT_FIELD_TARGET_SEMANTIC, fact.target.programSemanticFingerprint) ||
        !nonzero(VN_MONITOR_FACT_FIELD_TARGET_REVISION, fact.target.contentRevision) ||
        !optional(VN_MONITOR_FACT_FIELD_TARGET_KIND, fact.target.kind, fact.target.kind > 0 && fact.target.kind <= VN_MONITOR_TARGET_CONTENT) ||
        !optional(VN_MONITOR_FACT_FIELD_TARGET_CONTENT_KIND, fact.target.contentKind, fact.target.contentKind > 0 && fact.target.contentKind <= VN_MONITOR_CONTENT_MEDIA) ||
        !optional(VN_MONITOR_FACT_FIELD_TARGET_SCOPE, fact.target.scope, fact.target.scope == 1) ||
        (has(VN_MONITOR_FACT_FIELD_TARGET_VALIDATED) && !has(VN_MONITOR_FACT_FIELD_TARGET_HANDLE) && !has(VN_MONITOR_FACT_FIELD_TARGET_OBJECT))) return false;
    if (has(VN_MONITOR_FACT_FIELD_TARGET_VALIDATED)) {
        if (!has(VN_MONITOR_FACT_FIELD_TARGET_KIND)) return false;
        if (fact.target.kind == VN_MONITOR_TARGET_CONTENT) {
            if (!has(VN_MONITOR_FACT_FIELD_TARGET_OBJECT) || !has(VN_MONITOR_FACT_FIELD_TARGET_CONTENT_KIND)) return false;
        } else {
            if (!has(VN_MONITOR_FACT_FIELD_TARGET_HANDLE)) return false;
            auto targetHandle = fact.target.handle;
            unsigned targetSubject = 0;
            switch (fact.target.kind) {
            case VN_MONITOR_TARGET_PLAYABLE: targetSubject = VN_MONITOR_SUBJECT_PLAYABLE; break;
            case VN_MONITOR_TARGET_ACTION: targetSubject = VN_MONITOR_SUBJECT_ACTION; break;
            case VN_MONITOR_TARGET_LOGICAL_VOICE: targetSubject = VN_MONITOR_SUBJECT_LOGICAL_VOICE; break;
            case VN_MONITOR_TARGET_RENDER_ACTIVATION: targetSubject = VN_MONITOR_SUBJECT_RENDER_ACTIVATION; break;
            case VN_MONITOR_TARGET_EMITTER: targetSubject = VN_MONITOR_SUBJECT_EMITTER; break;
            case VN_MONITOR_TARGET_PREVIEW_PLAYABLE:
                // Only this explicit target namespace permits the Preview tag.
                if ((targetHandle & (UINT64_C(1) << 63)) == 0) return false;
                targetHandle &= ~(UINT64_C(1) << 63);
                targetSubject = VN_MONITOR_SUBJECT_PLAYABLE;
                break;
            default: return false;
            }
            if (!ValidSubjectHandle(targetSubject, targetHandle)) return false;
        }
    }
    if (!IsCallFactKind(fact.factKind) && (fact.validityHi != 0 || has(VN_MONITOR_FACT_FIELD_REQUEST) ||
        has(VN_MONITOR_FACT_FIELD_REQUEST_OPERATION) || has(VN_MONITOR_FACT_FIELD_REQUEST_ORIGIN) ||
        has(VN_MONITOR_FACT_FIELD_CAUSED_BY_REQUEST) || has(VN_MONITOR_FACT_FIELD_CAUSED_BY_OPERATION) || has(VN_MONITOR_FACT_FIELD_CAUSED_BY_ORIGIN))) return false;
    return true;
}

inline bool ValidateExecutionFact(const VnMonitorRecordHeader& header,
    const VnMonitorExecutionFactV1& fact) noexcept {
    return header.recordKind == VN_MONITOR_RECORD_EXECUTION_FACT && header.recordVersion == 1 &&
        header.flags == VN_MONITOR_RECORD_FLAG_OPTIONAL && header.payloadBytes == kExecutionPayloadBytes &&
        header.recordBytes == kExecutionRecordBytes && ValidateFactFields(header, fact);
}

inline std::uint32_t PackExecutionFact(std::uint8_t* out, std::uint32_t capacity,
    const MonitorExecutionFact& source, std::uint64_t producer, std::uint64_t stream) noexcept {
    if (out == nullptr || capacity < kExecutionRecordBytes) return 0;
    VnMonitorRecordHeader header{};
    VnMonitorExecutionFactV1 fact{};
    if (!ProjectExecutionFact(source, producer, stream, header, fact) || !ValidateExecutionFact(header, fact)) return 0;
    std::memset(out, 0, kExecutionRecordBytes);
    PackHeader(out, header);
    ExecutionPayloadWriter writer{out + kPackedEnvelopeBytes};
    VisitExecutionPayload(writer, fact);
    return writer.offset == kExecutionPayloadBytes ? kExecutionRecordBytes : 0;
}

inline bool DecodeExecutionFact(const VnMonitorRecordHeader& header, const std::uint8_t* bytes,
    std::uint64_t size, VnMonitorExecutionFactV1& out) noexcept {
    if (bytes == nullptr || size != kExecutionPayloadBytes) return false;
    VnMonitorExecutionFactV1 fact{};
    ExecutionPayloadReader reader{bytes};
    VisitExecutionPayload(reader, fact);
    if (reader.offset != kExecutionPayloadBytes || !ValidateExecutionFact(header, fact)) return false;
    out = fact;
    return true;
}

} // namespace harken::monitor

#endif
