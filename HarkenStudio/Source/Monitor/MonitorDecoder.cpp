#include "MonitorDecoder.h"
#include "Monitor/MonitorTypes.h"

#include <algorithm>
#include <bit>
#include <type_traits>
#include <cstring>
#include <limits>
#include <new>

namespace VNX {
namespace Monitor {
namespace {

constexpr uint32_t kPacked = kMonitorRecordEnvelopePackedBytes;

uint32_t readU32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t readU64(const uint8_t* p)
{
    return static_cast<uint64_t>(readU32(p)) |
           (static_cast<uint64_t>(readU32(p + 4)) << 32);
}

bool addOverflow(uint64_t a, uint64_t b, uint64_t* out)
{
    if (a > std::numeric_limits<uint64_t>::max() - b)
        return true;
    *out = a + b;
    return false;
}

bool paddingZero(const uint8_t* p, uint64_t from, uint64_t to)
{
    for (uint64_t i = from; i < to; ++i)
    {
        if (p[i] != 0)
            return false;
    }
    return true;
}

MonitorResult readPackedHeader(const uint8_t* p, MonitorRecordHeader* out)
{
    std::memset(out, 0, sizeof(*out));
    out->structSize = sizeof(MonitorRecordHeader);
    size_t o = 0;
    out->recordKind = readU32(p + o); o += 4;
    out->recordVersion = readU32(p + o); o += 4;
    out->flags = readU32(p + o); o += 4;
    out->streamKind = readU32(p + o); o += 4;
    out->recordBytes = readU64(p + o); o += 8;
    out->streamId = readU64(p + o); o += 8;
    out->producerId = readU64(p + o); o += 8;
    out->sourceSequence = readU64(p + o); o += 8;
    out->deliverySequence = readU64(p + o); o += 8;
    out->engineMonitorInstanceId = readU64(p + o); o += 8;
    out->monitorSessionEpoch = readU64(p + o); o += 8;
    out->worldGeneration = readU64(p + o); o += 8;
    out->capturePlanGeneration = readU64(p + o); o += 8;
    out->renderPlanGeneration = readU64(p + o); o += 8;
    out->transportEpoch = readU64(p + o); o += 8;
    out->deviceEpoch = readU64(p + o); o += 8;
    out->clockAnchorGeneration = readU64(p + o); o += 8;
    out->measurementSegmentGeneration = readU64(p + o); o += 8;
    out->executeFrame = readU64(p + o); o += 8;
    out->graphFrame = readU64(p + o); o += 8;
    out->devicePresentationEstimate = readU64(p + o); o += 8;
    out->deviceUncertaintyFrames = readU32(p + o); o += 4;
    out->entityKind = readU32(p + o); o += 4;
    out->programSemanticFingerprint = readU64(p + o); o += 8;
    out->runtimeLineageFingerprint = readU64(p + o); o += 8;
    out->optionalSymbolFingerprint = readU64(p + o); o += 8;
    out->stableId = harkenstudio::ObjectIdFromBytes(std::span<const uint8_t, 16>(p + o, 16)); o += 16;
    out->instanceHandle = readU64(p + o); o += 8;
    out->instanceGeneration = readU64(p + o); o += 8;
    out->actionInstanceHandle = readU64(p + o); o += 8;
    out->playableInstanceHandle = readU64(p + o); o += 8;
    out->parentActionInstanceHandle = readU64(p + o); o += 8;
    out->traceCorrelationId = readU64(p + o); o += 8;
    out->reason = readU32(p + o); o += 4;
    out->validityFlags = readU32(p + o); o += 4;
    out->payloadBytes = readU64(p + o); o += 8;
    out->telemetry.structSize = sizeof(out->telemetry);
    out->telemetry.reserved0 = readU32(p + o); o += 4;
    out->telemetry.arrangingInstance = readU64(p + o); o += 8;
    out->telemetry.clipId = harkenstudio::ObjectIdFromBytes(std::span<const uint8_t, 16>(p + o, 16)); o += 16;
    out->telemetry.loopIteration = readU32(p + o); o += 4;
    out->telemetry.occurrenceOrdinal = readU32(p + o); o += 4;
    out->telemetry.logicCommitSequence = readU64(p + o); o += 8;
    out->telemetry.executeFrame = readU64(p + o); o += 8;
    if (o != kPacked)
        return kMonitorIncompatible;
    return kOk;
}

MonitorDecodeBatch fail(MonitorResult status, uint64_t nextBytes = kPacked)
{
    MonitorDecodeBatch out;
    out.status = status;
    out.nextRecordBytes = nextBytes;
    return out;
}

bool permit(const MonitorDecodeAllocator& allocator, std::size_t bytes)
{
    if (!allocator)
        return true;
    return allocator(bytes);
}

template <typename Work>
bool tryPermittedAlloc(const MonitorDecodeAllocator& allocator,
                       std::size_t bytes,
                       Work&& work)
{
    if (!permit(allocator, bytes))
        return false;
    try
    {
        work();
        return true;
    }
    catch (const std::bad_alloc&)
    {
        return false;
    }
}

constexpr bool isCallFactKind(unsigned kind) noexcept {
    return kind == kMonitorFactRequestSubmitted || kind == kMonitorFactRequestReturned ||
        kind == kMonitorFactRequestTerminal;
}

constexpr std::uint64_t subjectGeneration(unsigned subject, std::uint64_t handle) noexcept {
    return subject == kMonitorSubjectAction ? handle >> 12 : handle >> 32;
}

constexpr bool validSubjectHandle(unsigned subject, std::uint64_t handle) noexcept {
    if (subject == kMonitorSubjectNone || subject > kMonitorSubjectTransition ||
        handle == 0 || subjectGeneration(subject, handle) == 0) return false;
    return (subject != kMonitorSubjectPlayable && subject != kMonitorSubjectLogicalVoice) ||
        (handle & (UINT64_C(1) << 63)) == 0;
}

bool validRequestOperation(uint32_t value) { return value >= kMonitorOperationPostAction && value <= kMonitorOperationPlayRawPreview; }
bool validRequestOrigin(uint32_t value) { return value >= kMonitorRequestOriginNativeApi && value <= kMonitorRequestOriginStudioPreview; }
std::uint32_t subjectEntityKind(unsigned subject) noexcept {
    switch (subject) {
    case kMonitorSubjectPlayable: return kMonitorEntityPlayable;
    case kMonitorSubjectAction: return kMonitorEntityAction;
    case kMonitorSubjectLogicalVoice:
    case kMonitorSubjectRenderActivation: return kMonitorEntityVoice;
    case kMonitorSubjectEmitter: return kMonitorEntityNone;
    case kMonitorSubjectQueue: return kMonitorEntityQueue;
    case kMonitorSubjectTransition: return kMonitorEntityTransition;
    default: return kMonitorEntityNone;
    }
}

bool validateFactFields(const MonitorRecordHeader& header, const MonitorExecutionFactV1& fact) noexcept {
    const auto has = [&](unsigned field) { return monitorFactHas(fact, field); };
    if (header.engineMonitorInstanceId == 0 ||
        header.producerId == 0 || header.sourceSequence == 0 || header.streamId == 0 ||
        header.reserved0 != 0 || header.telemetry.reserved0 != 0 ||
        fact.structSize != 432u || fact.factKind == 0 || fact.factKind > kMonitorFactPlayableStarted ||
        fact.subjectNamespace > kMonitorSubjectTransition || fact.contentKind > kMonitorContentMedia ||
        fact.reserved0 != 0 || fact.reserved1 != 0 || fact.control.reserved0 != 0 || fact.selection.reserved0 != 0 ||
        fact.target.reserved0 != 0 || (fact.factFlags & ~(kMonitorFactFlagIdentityIncomplete | kMonitorFactFlagControlIncomplete)) != 0 ||
        (fact.validityHi >> (kMonitorFactFieldTargetScope - 63)) != 0) return false;
    const auto expectedStream = fact.factKind == kMonitorFactIdentityUpsert || fact.factKind == kMonitorFactIdentityTombstone
        ? kMonitorStreamState : kMonitorStreamEvent;
    if (header.streamKind != expectedStream) return false;
    bool executionFields = false;
    for (unsigned i = kMonitorFactFieldPlaybackRequest; i <= kMonitorFactFieldParentAction; ++i)
        executionFields |= has(i);
    for (unsigned i = kMonitorFactFieldServiceLeaf; i <= kMonitorFactFieldRenderActivation; ++i)
        executionFields |= has(i);
    const bool callFields = fact.validityHi != 0 || has(kMonitorFactFieldCausedByRequest) ||
        has(kMonitorFactFieldCausedByOperation) || has(kMonitorFactFieldCausedByOrigin);
    if ((executionFields && callFields) ||
        (executionFields && (fact.factKind == kMonitorFactRequestSubmitted || fact.factKind == kMonitorFactRequestReturned)))
        return false;
    if (fact.factKind == kMonitorFactRequestTerminal && executionFields) {
        for (unsigned i = 0; i != 3; ++i)
            if (has(kMonitorFactFieldRequest + i) != has(kMonitorFactFieldPlaybackRequest + i)) return false;
        if (fact.request.sequence != fact.playbackRequest.sequence || fact.request.operation != fact.playbackRequest.operation ||
            fact.request.origin != fact.playbackRequest.origin) return false;
    }
    const auto optional = [&](unsigned field, auto value, bool validValue = true) {
        return has(field) ? validValue : value == decltype(value){};
    };
    const auto nonzero = [&](unsigned field, auto value) { return optional(field, value, value != decltype(value){}); };
    if (has(kMonitorFactFieldSubject)) {
        if (!validSubjectHandle(fact.subjectNamespace, header.instanceHandle) ||
            header.instanceGeneration == 0 || header.instanceGeneration != subjectGeneration(fact.subjectNamespace, header.instanceHandle) ||
            header.entityKind != subjectEntityKind(fact.subjectNamespace)) return false;
    } else if (header.instanceHandle != 0 || header.instanceGeneration != 0 || header.entityKind != 0) return false;
    const auto handle = [&](unsigned field, std::uint64_t value, unsigned subject) {
        return optional(field, value, validSubjectHandle(subject, value));
    };
    if (!nonzero(kMonitorFactFieldProject, fact.projectId) || !nonzero(kMonitorFactFieldContentObject, header.stableId) ||
        !nonzero(kMonitorFactFieldContentWorld, header.worldGeneration) ||
        !nonzero(kMonitorFactFieldContentSemantic, header.programSemanticFingerprint) ||
        !nonzero(kMonitorFactFieldContentLineage, header.runtimeLineageFingerprint) ||
        !nonzero(kMonitorFactFieldContentRevision, fact.contentRevision) ||
        !optional(kMonitorFactFieldTimestamp, fact.timestampNs) ||
        !optional(kMonitorFactFieldResult, fact.result) || !optional(kMonitorFactFieldReason, header.reason) ||
        !optional(kMonitorFactFieldTerminalKind, fact.terminalKind,
            fact.subjectNamespace == kMonitorSubjectPlayable &&
            fact.terminalKind >= kMonitorTerminalNaturalCompletion &&
            fact.terminalKind <= kMonitorTerminalFailure)) return false;
    const auto request = [&](unsigned field, const MonitorFactRequest& value) {
        return nonzero(field, value.sequence) && optional(field + 1, value.operation, validRequestOperation(value.operation)) &&
            optional(field + 2, value.origin, validRequestOrigin(value.origin));
    };
    if (!request(kMonitorFactFieldRequest, fact.request) || !request(kMonitorFactFieldPlaybackRequest, fact.playbackRequest) ||
        !request(kMonitorFactFieldCausedByRequest, fact.causedByRequest) ||
        !request(kMonitorFactFieldControlRequest, fact.controlRequest) ||
        !request(kMonitorFactFieldSelectionRequest, fact.selectionRequest)) return false;
    const auto cause = [&](unsigned field, const MonitorFactCause& value) {
        return handle(field, value.action, kMonitorSubjectAction) &&
            handle(field + 1, value.parentPlayable, kMonitorSubjectPlayable) &&
            optional(field + 2, value.executionOrdinal, value.executionOrdinal != UINT32_MAX);
    };
    if (!cause(kMonitorFactFieldControlAction, fact.control) || !cause(kMonitorFactFieldSelectionAction, fact.selection) ||
        !handle(kMonitorFactFieldParentPlayable, header.playableInstanceHandle, kMonitorSubjectPlayable) ||
        !handle(kMonitorFactFieldParentAction, header.parentActionInstanceHandle, kMonitorSubjectAction) ||
        !handle(kMonitorFactFieldRoot, fact.root, fact.subjectNamespace == kMonitorSubjectAction ? kMonitorSubjectAction : kMonitorSubjectPlayable) ||
        !handle(kMonitorFactFieldRelatedAction, header.actionInstanceHandle, kMonitorSubjectAction) ||
        !handle(kMonitorFactFieldServiceLeaf, fact.serviceLeaf, kMonitorSubjectPlayable) ||
        !handle(kMonitorFactFieldResourceOwner, fact.resourceOwner, kMonitorSubjectPlayable) ||
        !handle(kMonitorFactFieldLogicalVoice, fact.logicalVoice, kMonitorSubjectLogicalVoice) ||
        !handle(kMonitorFactFieldRenderActivation, fact.renderActivation, kMonitorSubjectRenderActivation) ||
        !handle(kMonitorFactFieldOriginEmitter, fact.originEmitter, kMonitorSubjectEmitter) || !nonzero(kMonitorFactFieldHostKey, fact.hostSession) ||
        !nonzero(kMonitorFactFieldHostKey, fact.hostLocalSequence) || !nonzero(kMonitorFactFieldHostRevision, fact.originHostRevision) ||
        (has(kMonitorFactFieldHostRevision) && !has(kMonitorFactFieldHostKey)) ||
        !optional(kMonitorFactFieldOriginKind, fact.originKind, fact.originKind > 0 && fact.originKind <= 4) ||
        !optional(kMonitorFactFieldHostOwnership, fact.hostOwnership,
            fact.hostOwnership > 0 && fact.hostOwnership <= kMonitorHostStudioDefaultEmitter) ||
        !handle(kMonitorFactFieldCurrentStateEmitter, fact.currentStateEmitter, kMonitorSubjectEmitter) ||
        !handle(kMonitorFactFieldCurrentSpatialEmitter, fact.currentSpatialEmitter, kMonitorSubjectEmitter) ||
        !optional(kMonitorFactFieldStateBinding, fact.stateBinding, fact.stateBinding <= 2) ||
        !optional(kMonitorFactFieldSpatialBinding, fact.spatialBinding, fact.spatialBinding <= 2)) return false;
    if ((has(kMonitorFactFieldStateBinding) && (fact.stateBinding == 1) != has(kMonitorFactFieldCurrentStateEmitter)) ||
        (has(kMonitorFactFieldSpatialBinding) && (fact.spatialBinding == 1) != has(kMonitorFactFieldCurrentSpatialEmitter))) return false;
    if (!nonzero(kMonitorFactFieldLogicCommit, header.telemetry.logicCommitSequence) ||
        !optional(kMonitorFactFieldReadCutKey, fact.readCutKey,
            (fact.readCutKey >> 32) != 0 && static_cast<std::uint32_t>(fact.readCutKey) != UINT32_MAX) ||
        !nonzero(kMonitorFactFieldReadCutSequence, fact.readCutSequence) ||
        !optional(kMonitorFactFieldReadCutOrdinal, fact.readCutOrdinalExclusive) ||
        !optional(kMonitorFactFieldExecuteFrame, header.executeFrame) || !optional(kMonitorFactFieldGraphFrame, header.graphFrame) ||
        !nonzero(kMonitorFactFieldTransport, header.transportEpoch) || !optional(kMonitorFactFieldAudioBlock, fact.audioBlock) ||
        !optional(kMonitorFactFieldFrameOffset, fact.frameOffset) || !optional(kMonitorFactFieldFrameSpan, fact.frameSpan) ||
        !nonzero(kMonitorFactFieldArranging, header.telemetry.arrangingInstance) || !nonzero(kMonitorFactFieldClip, header.telemetry.clipId) ||
        !optional(kMonitorFactFieldLoop, header.telemetry.loopIteration) || !optional(kMonitorFactFieldOccurrence, header.telemetry.occurrenceOrdinal) ||
        !nonzero(kMonitorFactFieldOwnerEpoch, fact.ownerEpoch)) return false;
    if (!optional(kMonitorFactFieldTargetHandle, fact.target.handle) ||
        !nonzero(kMonitorFactFieldTargetProject, fact.target.projectId) || !nonzero(kMonitorFactFieldTargetObject, fact.target.objectId) ||
        !nonzero(kMonitorFactFieldTargetWorld, fact.target.worldGeneration) ||
        !nonzero(kMonitorFactFieldTargetSemantic, fact.target.programSemanticFingerprint) ||
        !nonzero(kMonitorFactFieldTargetRevision, fact.target.contentRevision) ||
        !optional(kMonitorFactFieldTargetKind, fact.target.kind, fact.target.kind > 0 && fact.target.kind <= kMonitorTargetContent) ||
        !optional(kMonitorFactFieldTargetContentKind, fact.target.contentKind, fact.target.contentKind > 0 && fact.target.contentKind <= kMonitorContentMedia) ||
        !optional(kMonitorFactFieldTargetScope, fact.target.scope, fact.target.scope == 1) ||
        (has(kMonitorFactFieldTargetValidated) && !has(kMonitorFactFieldTargetHandle) && !has(kMonitorFactFieldTargetObject))) return false;
    if (has(kMonitorFactFieldTargetValidated)) {
        if (!has(kMonitorFactFieldTargetKind)) return false;
        if (fact.target.kind == kMonitorTargetContent) {
            if (!has(kMonitorFactFieldTargetObject) || !has(kMonitorFactFieldTargetContentKind)) return false;
        } else {
            if (!has(kMonitorFactFieldTargetHandle)) return false;
            auto targetHandle = fact.target.handle;
            unsigned targetSubject = 0;
            switch (fact.target.kind) {
            case kMonitorTargetPlayable: targetSubject = kMonitorSubjectPlayable; break;
            case kMonitorTargetAction: targetSubject = kMonitorSubjectAction; break;
            case kMonitorTargetLogicalVoice: targetSubject = kMonitorSubjectLogicalVoice; break;
            case kMonitorTargetRenderActivation: targetSubject = kMonitorSubjectRenderActivation; break;
            case kMonitorTargetEmitter: targetSubject = kMonitorSubjectEmitter; break;
            case kMonitorTargetPreviewPlayable:
                if ((targetHandle & (UINT64_C(1) << 63)) == 0) return false;
                targetHandle &= ~(UINT64_C(1) << 63);
                targetSubject = kMonitorSubjectPlayable;
                break;
            default: return false;
            }
            if (!validSubjectHandle(targetSubject, targetHandle)) return false;
        }
    }
    if (!isCallFactKind(fact.factKind) && (fact.validityHi != 0 || has(kMonitorFactFieldRequest) ||
        has(kMonitorFactFieldRequestOperation) || has(kMonitorFactFieldRequestOrigin) ||
        has(kMonitorFactFieldCausedByRequest) || has(kMonitorFactFieldCausedByOperation) || has(kMonitorFactFieldCausedByOrigin))) return false;
    return true;
}


} // namespace

MonitorDecodeBatch decodePackedRecords(const uint8_t* packedBytes,
                                       uint64_t bytesWritten,
                                       uint32_t expectedRecords,
                                       MonitorDecodeAllocator allocator)
{
    if (kPacked != 316u)
        return fail(kMonitorIncompatible);
    if (bytesWritten > 0 && packedBytes == nullptr)
        return fail(kInvalidArg, 0);
    if ((expectedRecords == 0) != (bytesWritten == 0))
        return fail(kMonitorIncompatible, 0);
    if (expectedRecords > 0 && bytesWritten / kPacked < expectedRecords)
        return fail(kMonitorIncompatible, 0);
    if (bytesWritten == 0)
    {
        MonitorDecodeBatch empty;
        empty.status = kOk;
        return empty;
    }
    if (bytesWritten > std::numeric_limits<std::size_t>::max())
        return fail(kMonitorIncompatible, 0);
    // Upper bound on owned copies: every successful batch copies at most
    // bytesWritten. Deny this before any reserve/assign.
    if (!permit(allocator, static_cast<std::size_t>(bytesWritten)))
        return fail(kMonitorIncompatible, 0);

    std::vector<MonitorDecodedRecord> staging;
    const std::size_t stagingBytes =
        static_cast<std::size_t>(expectedRecords) * sizeof(MonitorDecodedRecord);
    if (!tryPermittedAlloc(allocator, stagingBytes, [&] { staging.reserve(expectedRecords); }))
        return fail(kMonitorIncompatible, 0);

    uint64_t offset = 0;
    try
    {
        for (uint32_t i = 0; i < expectedRecords; ++i)
        {
            if ((offset & 7ull) != 0)
                return fail(kMonitorIncompatible);
            if (offset >= bytesWritten)
                return fail(kMonitorIncompatible);
            const uint64_t remaining = bytesWritten - offset;
            if (remaining < kPacked)
                return fail(kMonitorIncompatible, kPacked);

            MonitorRecordHeader header {};
            const MonitorResult headerStatus = readPackedHeader(packedBytes + offset, &header);
            if (headerStatus != kOk)
                return fail(headerStatus, kPacked);
            if ((header.recordBytes & 7ull) != 0 ||
                header.recordBytes < kPacked ||
                header.payloadBytes > header.recordBytes - kPacked)
                return fail(kMonitorIncompatible, header.recordBytes);
            if (header.recordBytes > remaining)
                return fail(kMonitorIncompatible, header.recordBytes);

            const uint64_t padFrom = kPacked + header.payloadBytes;
            if (padFrom > header.recordBytes)
                return fail(kMonitorIncompatible, header.recordBytes);
            if (!paddingZero(packedBytes + offset, padFrom, header.recordBytes))
                return fail(kMonitorIncompatible, header.recordBytes);

            const uint32_t required = header.flags & kMonitorRecordFlagRequired;
            const uint32_t optional = header.flags & kMonitorRecordFlagOptional;
            if ((required == 0 && optional == 0) || (required != 0 && optional != 0))
                return fail(kMonitorIncompatible, header.recordBytes);
            const uint32_t reservedFlags =
                header.flags & ~(kMonitorRecordFlagRequired | kMonitorRecordFlagOptional);
            const bool unknownKind =
                header.recordKind == kMonitorRecordUnknown ||
                (header.recordKind >= kMonitorRecordInvalid &&
                 header.recordKind != kMonitorRecordEventDetail &&
                 header.recordKind != kMonitorRecordExecutionFact);
            const bool unknownVersion = header.recordVersion != 1u &&
                !(header.recordKind == kMonitorRecordEventDetail && header.recordVersion == 2u);
            const bool unknown = unknownKind || unknownVersion || reservedFlags != 0;
            if (unknown && required)
                return fail(kMonitorIncompatible, header.recordBytes);

            const std::size_t copyBytes = unknown && optional
                ? static_cast<std::size_t>(header.recordBytes)
                : static_cast<std::size_t>(header.payloadBytes);

            MonitorDecodedRecord rec;
            rec.header = header;
            rec.unknownOptional = unknown && optional;
            if (copyBytes > 0)
            {
                if (!tryPermittedAlloc(allocator, copyBytes, [&] {
                        if (unknown && optional)
                        {
                            rec.skippedBytes.assign(
                                packedBytes + offset,
                                packedBytes + offset + static_cast<size_t>(header.recordBytes));
                        }
                        else
                        {
                            rec.payload.assign(
                                packedBytes + offset + kPacked,
                                packedBytes + offset + kPacked +
                                    static_cast<size_t>(header.payloadBytes));
                        }
                    }))
                    return fail(kMonitorIncompatible, 0);
            }
            staging.push_back(std::move(rec));

            uint64_t next = 0;
            if (addOverflow(offset, header.recordBytes, &next))
                return fail(kMonitorIncompatible, header.recordBytes);
            offset = next;
        }
        if (offset != bytesWritten || staging.size() != expectedRecords)
            return fail(kMonitorIncompatible);

        MonitorDecodeBatch batch;
        batch.status = kOk;
        batch.records = std::move(staging);
        batch.recordsWritten = static_cast<uint32_t>(batch.records.size());
        batch.bytesConsumed = offset;
        return batch;
    }
    catch (const std::bad_alloc&)
    {
        return fail(kMonitorIncompatible, 0);
    }
}

bool decodeExecutionFact(const MonitorDecodedRecord& record, MonitorExecutionFactV1* out)
{
    const auto& header = record.header;
    if (!out || record.unknownOptional || header.recordKind != kMonitorRecordExecutionFact ||
        header.recordVersion != 1 || header.flags != kMonitorRecordFlagOptional ||
        header.streamKind != kMonitorStreamEvent || header.payloadBytes != 432 ||
        header.recordBytes != ((kMonitorRecordEnvelopePackedBytes + 432u + 7u) & ~7u) ||
        record.payload.size() != 432) return false;
    MonitorExecutionFactV1 fact{};
    size_t offset = 0;
    const auto* bytes = record.payload.data();
    const auto visit = [&]<class T>(T& value) {
        if constexpr (std::is_same_v<T, harkenstudio::ObjectId>) {
            value = harkenstudio::ObjectIdFromBytes(std::span<const uint8_t, 16>(bytes + offset, 16));
            offset += 16;
        } else {
            std::make_unsigned_t<T> bits = 0;
            for (size_t i = 0; i < sizeof(T); ++i)
                bits |= static_cast<std::make_unsigned_t<T>>(bytes[offset++]) << (8 * i);
            value = std::bit_cast<T>(bits);
        }
    };
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
    if (offset != 432 || !validateFactFields(header, fact)) return false;
    *out = fact;
    return true;
}

bool decodeEventDetail(const MonitorDecodedRecord& record, MonitorEventDetail* out,
                        MonitorEventDetailV2* identity)
{
    const bool version2 = record.header.recordVersion == 2;
    if (!out || record.unknownOptional || record.header.recordKind != kMonitorRecordEventDetail ||
        (record.header.recordVersion != 1 && !version2) || record.header.streamKind != kMonitorStreamEvent ||
        record.payload.size() != (version2 ? sizeof(MonitorEventDetailV2) : sizeof(MonitorEventDetail))) return false;
    const auto* p = record.payload.data();
    MonitorEventDetail value{};
    value.structSize = readU32(p);
    value.category = readU32(p + 4);
    value.code = readU32(p + 8);
    value.level = readU32(p + 12);
    value.result = static_cast<int32_t>(readU32(p + 16));
    value.reserved0 = readU32(p + 20);
    value.timestampNs = readU64(p + 24);
    value.arg0 = readU64(p + 32);
    value.arg1 = readU64(p + 40);
    value.valueBits = readU32(p + 48);
    value.textBytes = readU32(p + 52);
    if (value.structSize != record.payload.size() || value.reserved0 != 0 || value.timestampNs == 0 ||
        value.category < kMonitorEventCategoryOperation ||
        value.category > kMonitorEventCategoryDiagnostic || value.level > kLogError ||
        value.textBytes > sizeof(value.text) ||
        !paddingZero(p, 56 + value.textBytes, sizeof(value))) return false;
    std::copy_n(p + 56, value.textBytes, value.text);
    *out = value;
    if (identity) {
        *identity = {};
        identity->structSize = value.structSize;
        identity->category = value.category;
        identity->code = value.code;
        identity->level = value.level;
        identity->result = value.result;
        identity->timestampNs = value.timestampNs;
        identity->arg0 = value.arg0;
        identity->arg1 = value.arg1;
        identity->valueBits = value.valueBits;
        identity->textBytes = value.textBytes;
        std::copy_n(value.text, value.textBytes, identity->text);
        if (version2) {
            std::copy_n(p + sizeof(value), 16, identity->projectGuid);
        }
    }
    return true;
}

} // namespace Monitor
} // namespace VNX
