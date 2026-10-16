#include "MonitorIdentityCut.h"
#include "MonitorExecutionCodec.h"
#include "MonitorIdentityCodec.h"
#include "monitor_fixture_decode.h"
#include <gtest/gtest.h>
#include <array>
#include <cstring>

namespace {

using namespace harken;
using namespace harken::monitor;

MonitorExecutionFact Execution(unsigned kind = VN_MONITOR_FACT_VOICE_LIVE) {
    MonitorExecutionFact fact{};
    fact.engine = 0xaabbcc;
    fact.sourceSequence = 37;
    fact.kind = static_cast<std::uint8_t>(kind);
    return fact;
}

MonitorExecutionFact Call(unsigned kind = VN_MONITOR_FACT_REQUEST_RETURNED) {
    auto fact = Execution(kind);
    fact.flags = kMonitorFactCallBody;
    fact.body.call = {};
    return fact;
}

struct RoundTrip {
    std::array<std::uint8_t, kExecutionRecordBytes> bytes{};
    std::array<std::uint8_t, kExecutionRecordBytes> skipped{};
    VnMonitorDecodedRecord record{};
    VnMonitorExecutionFactV1 payload{};
    bool Read(const MonitorExecutionFact& fact) {
        if (PackExecutionFact(bytes.data(), bytes.size(), fact, 71, 72) != bytes.size()) return false;
        // The unchanged legacy harness skips optional kind 24. Its envelope is
        // still readable and the new decoder consumes the exact payload bytes.
        const auto result = vn_monitor_decode_records(bytes.data(), bytes.size(), &record, 1, skipped.data(), skipped.size());
        return result.status == VN_OK && result.recordsWritten == 1 &&
            record.skippedBytesCount == kExecutionRecordBytes && record.payload == nullptr &&
            DecodeExecutionFact(record.header, bytes.data() + kPackedEnvelopeBytes, kExecutionPayloadBytes, payload);
    }
};

TEST(MonitorExecutionCodec, PreservesActionGenerationAbove32BitsAndCanonicalObjectBytes) {
    auto source = Execution(VN_MONITOR_FACT_ACTION_ACCEPTED);
    source.subjectNamespace = VN_MONITOR_SUBJECT_ACTION;
    source.contentKind = VN_MONITOR_CONTENT_ACTION;
    const auto generation = (UINT64_C(1) << 43) + 19;
    source.body.execution.identity.handle = (generation << 12) | 7;
    source.body.execution.identity.project = {UINT64_C(0x123456789abcdef0), UINT64_C(0x1020304050607080)};
    source.body.execution.identity.object = {44, 55};
    SetSourceField(source, VN_MONITOR_FACT_FIELD_SUBJECT);
    SetSourceField(source, VN_MONITOR_FACT_FIELD_PROJECT);
    SetSourceField(source, VN_MONITOR_FACT_FIELD_CONTENT_OBJECT);
    RoundTrip decoded;
    ASSERT_TRUE(decoded.Read(source));
    EXPECT_EQ(generation, decoded.record.header.instanceGeneration);
    EXPECT_EQ(source.body.execution.identity.handle, decoded.record.header.instanceHandle);
    EXPECT_EQ(source.body.execution.identity.project, decoded.payload.projectId);
    EXPECT_EQ(source.body.execution.identity.object, decoded.record.header.stableId);
    EXPECT_EQ(0x12u, decoded.bytes[kPackedEnvelopeBytes + 32]);
    EXPECT_EQ(0x80u, decoded.bytes[kPackedEnvelopeBytes + 47]);
    EXPECT_EQ(source.engine, decoded.record.header.engineMonitorInstanceId);
    EXPECT_EQ(71u, decoded.record.header.producerId);
    EXPECT_EQ(source.sourceSequence, decoded.record.header.sourceSequence);
}

TEST(MonitorExecutionCodec, KeepsPlaybackControlAndSelectionRolesAtTheSameTime) {
    auto source = Execution();
    const MonitorRequestContext playback{{source.engine, 41}, MonitorRequestOrigin::StudioPreview, VN_MONITOR_OPERATION_PLAY_PREVIEW};
    const MonitorControlCause control{{{source.engine, 42}, MonitorRequestOrigin::NativeAPI, VN_MONITOR_OPERATION_STOP_ALL}, 0x100000901, 0x100000902, 0};
    const MonitorControlCause selection{{{source.engine, 43}, MonitorRequestOrigin::NativeAPI, VN_MONITOR_OPERATION_POST_ACTION}, 0x100000903, 0x100000904, 8};
    ASSERT_TRUE(CaptureExecutionRequests(source, playback, control, selection));
    RoundTrip decoded;
    ASSERT_TRUE(decoded.Read(source));
    EXPECT_EQ(41u, decoded.payload.playbackRequest.sequence);
    EXPECT_EQ(42u, decoded.payload.controlRequest.sequence);
    EXPECT_EQ(43u, decoded.payload.selectionRequest.sequence);
    EXPECT_EQ(0x100000901u, decoded.payload.control.action);
    EXPECT_EQ(0x100000904u, decoded.payload.selection.parentPlayable);
    EXPECT_EQ(0u, decoded.payload.control.executionOrdinal);
    EXPECT_TRUE(FactHas(decoded.payload, VN_MONITOR_FACT_FIELD_CONTROL_ORDINAL));
    EXPECT_FALSE(FactHas(decoded.payload, VN_MONITOR_FACT_FIELD_REQUEST));
}

TEST(MonitorExecutionCodec, ForeignOrExhaustedRequestDoesNotHideOtherValidFields) {
    auto source = Execution();
    const MonitorRequestContext playback{{source.engine + 1, 41}, MonitorRequestOrigin::StudioPreview, VN_MONITOR_OPERATION_PLAY_PREVIEW};
    const MonitorControlCause control{{{source.engine, 0}, MonitorRequestOrigin::NativeAPI, VN_MONITOR_OPERATION_STOP_ALL}, 0x100000901, 0, 0};
    ASSERT_TRUE(CaptureExecutionRequests(source, playback, control, {}));
    source.body.execution.position.logicCommit = 8;
    SetSourceField(source, VN_MONITOR_FACT_FIELD_LOGIC_COMMIT);
    RoundTrip decoded;
    ASSERT_TRUE(decoded.Read(source));
    EXPECT_FALSE(FactHas(decoded.payload, VN_MONITOR_FACT_FIELD_PLAYBACK_REQUEST));
    EXPECT_FALSE(FactHas(decoded.payload, VN_MONITOR_FACT_FIELD_CONTROL_REQUEST));
    EXPECT_TRUE(FactHas(decoded.payload, VN_MONITOR_FACT_FIELD_PLAYBACK_ORIGIN));
    EXPECT_EQ(0x100000901u, decoded.payload.control.action);
    EXPECT_EQ(8u, decoded.record.header.telemetry.logicCommitSequence);
    EXPECT_FALSE(FactHas(decoded.payload, VN_MONITOR_FACT_FIELD_ARRANGING));
    EXPECT_NE(0u, decoded.payload.factFlags & VN_MONITOR_FACT_FLAG_IDENTITY_INCOMPLETE);
}

TEST(MonitorExecutionCodec, OutOfRangeCompressedOperationsNeverTruncateToAValidValue) {
    auto source = Execution();
    ASSERT_TRUE(CaptureExecutionRequests(source,
        {{source.engine, 10}, static_cast<MonitorRequestOrigin>(258), 259}, {}, {}));
    RoundTrip decoded;
    ASSERT_TRUE(decoded.Read(source));
    EXPECT_EQ(10u, decoded.payload.playbackRequest.sequence);
    EXPECT_FALSE(FactHas(decoded.payload, VN_MONITOR_FACT_FIELD_PLAYBACK_OPERATION));
    EXPECT_FALSE(FactHas(decoded.payload, VN_MONITOR_FACT_FIELD_PLAYBACK_ORIGIN));
    EXPECT_EQ(0u, decoded.payload.playbackRequest.operation);
    EXPECT_EQ(0u, decoded.payload.playbackRequest.origin);
    EXPECT_NE(0u, decoded.payload.factFlags & VN_MONITOR_FACT_FLAG_IDENTITY_INCOMPLETE);
}

TEST(MonitorExecutionCodec, RejectedCallRetainsRawPreviewTargetWithoutInventingSubject) {
    auto source = Call();
    ASSERT_TRUE(CaptureCallRequests(source, {{source.engine, 72}, MonitorRequestOrigin::StudioPreview, VN_MONITOR_OPERATION_STOP_PREVIEW}));
    source.result = VN_INVALID_ARG;
    SetSourceField(source, VN_MONITOR_FACT_FIELD_RESULT);
    source.body.call.target.handle = UINT64_C(0x8000000900000003);
    source.body.call.target.kind = VN_MONITOR_TARGET_PREVIEW_PLAYABLE;
    SetSourceField(source, VN_MONITOR_FACT_FIELD_TARGET_HANDLE);
    SetSourceField(source, VN_MONITOR_FACT_FIELD_TARGET_KIND);
    RoundTrip decoded;
    ASSERT_TRUE(decoded.Read(source));
    EXPECT_EQ(source.body.call.target.handle, decoded.payload.target.handle);
    EXPECT_EQ(VN_INVALID_ARG, decoded.payload.result);
    EXPECT_EQ(72u, decoded.payload.request.sequence);
    EXPECT_FALSE(FactHas(decoded.payload, VN_MONITOR_FACT_FIELD_SUBJECT));
    EXPECT_FALSE(FactHas(decoded.payload, VN_MONITOR_FACT_FIELD_TARGET_VALIDATED));
    EXPECT_EQ(0u, decoded.record.header.instanceHandle);
    EXPECT_FALSE(CaptureExecutionRequests(source, {}, {}, {}));
    EXPECT_FALSE(SetSourceField(source, VN_MONITOR_FACT_FIELD_PLAYBACK_REQUEST));
}

TEST(MonitorExecutionCodec, TargetContentCannotOverwriteTheActualLeafContent) {
    auto source = Call();
    source.body.call.identity.object = {11, 12};
    source.body.call.identity.project = {13, 14};
    source.contentKind = VN_MONITOR_CONTENT_SOUND;
    source.body.call.target.object = {21, 22};
    source.body.call.target.project = {23, 24};
    source.body.call.target.kind = VN_MONITOR_TARGET_CONTENT;
    source.body.call.target.contentKind = VN_MONITOR_CONTENT_CONTAINER;
    for (auto field : {VN_MONITOR_FACT_FIELD_CONTENT_OBJECT, VN_MONITOR_FACT_FIELD_PROJECT,
        VN_MONITOR_FACT_FIELD_TARGET_OBJECT, VN_MONITOR_FACT_FIELD_TARGET_PROJECT, VN_MONITOR_FACT_FIELD_TARGET_KIND,
        VN_MONITOR_FACT_FIELD_TARGET_CONTENT_KIND}) SetSourceField(source, field);
    RoundTrip decoded;
    ASSERT_TRUE(decoded.Read(source));
    EXPECT_EQ(VnObjectId(11, 12), decoded.record.header.stableId);
    EXPECT_EQ(VnObjectId(13, 14), decoded.payload.projectId);
    EXPECT_EQ(VnObjectId(21, 22), decoded.payload.target.objectId);
    EXPECT_EQ(VnObjectId(23, 24), decoded.payload.target.projectId);
}

TEST(MonitorExecutionCodec, ExecutionTerminalRetainsAllCausesAndProjectsOriginalRequest) {
    auto source = Execution(VN_MONITOR_FACT_REQUEST_TERMINAL);
    ASSERT_TRUE(CaptureExecutionRequests(source,
        {{source.engine, 1}, MonitorRequestOrigin::NativeAPI, VN_MONITOR_OPERATION_POST_ACTION},
        {{{source.engine, 2}, MonitorRequestOrigin::NativeAPI, VN_MONITOR_OPERATION_STOP_EMITTER}, 0, 0, UINT32_MAX},
        {{{source.engine, 3}, MonitorRequestOrigin::NativeAPI, VN_MONITOR_OPERATION_POST_ACTION}, 0, 0, UINT32_MAX}));
    RoundTrip decoded;
    ASSERT_TRUE(decoded.Read(source));
    EXPECT_EQ(1u, decoded.payload.request.sequence);
    EXPECT_EQ(1u, decoded.payload.playbackRequest.sequence);
    EXPECT_EQ(2u, decoded.payload.controlRequest.sequence);
    EXPECT_EQ(3u, decoded.payload.selectionRequest.sequence);
    decoded.payload.request.sequence = 2;
    EXPECT_FALSE(ValidateExecutionFact(decoded.record.header, decoded.payload));
}

TEST(MonitorExecutionCodec, OriginHostSurvivesDetachAndForeignHostDoesNotBecomeLocal) {
    auto source = Execution();
    MonitorEmitterOrigin origin{UINT64_C(0x900000003), {source.engine, 31, 32}, 33,
        MonitorEmitterKind::ExplicitSpatial, MonitorHostOwnership::UnityEmitter};
    CaptureFactEmitters(source, origin, {0, 0, MonitorBindingState::Detached, MonitorBindingState::Detached});
    RoundTrip decoded;
    ASSERT_TRUE(decoded.Read(source));
    EXPECT_EQ(origin.emitter, decoded.payload.originEmitter);
    EXPECT_EQ(31u, decoded.payload.hostSession);
    EXPECT_EQ(33u, decoded.payload.originHostRevision);
    EXPECT_EQ(static_cast<unsigned>(MonitorBindingState::Detached), decoded.payload.spatialBinding);
    EXPECT_FALSE(FactHas(decoded.payload, VN_MONITOR_FACT_FIELD_CURRENT_SPATIAL_EMITTER));
    origin.host.engineInstance += 1;
    CaptureFactEmitters(source, origin, {origin.emitter, 0, MonitorBindingState::Bound, MonitorBindingState::None});
    ASSERT_TRUE(decoded.Read(source));
    EXPECT_FALSE(FactHas(decoded.payload, VN_MONITOR_FACT_FIELD_HOST_KEY));
    EXPECT_FALSE(FactHas(decoded.payload, VN_MONITOR_FACT_FIELD_HOST_REVISION));
    EXPECT_EQ(origin.emitter, decoded.payload.currentStateEmitter);
    EXPECT_EQ(0u, decoded.payload.hostSession);
    EXPECT_NE(0u, decoded.payload.factFlags & VN_MONITOR_FACT_FLAG_IDENTITY_INCOMPLETE);
}

TEST(MonitorExecutionCodec, ReadCutPositionIsIndependentAndZeroOrdinalIsValid) {
    auto source = Execution(VN_MONITOR_FACT_IDENTITY_UPSERT);
    auto& position = source.body.execution.position;
    position.logicCommit = 11;
    position.readCutKey = UINT64_C(0x20000000c);
    position.readCutSequence = 13;
    position.detail.readCut = {0, 0};
    for (auto field : {VN_MONITOR_FACT_FIELD_LOGIC_COMMIT, VN_MONITOR_FACT_FIELD_READ_CUT_KEY,
        VN_MONITOR_FACT_FIELD_READ_CUT_SEQUENCE, VN_MONITOR_FACT_FIELD_READ_CUT_ORDINAL, VN_MONITOR_FACT_FIELD_EXECUTE_FRAME})
        SetSourceField(source, field);
    RoundTrip decoded;
    ASSERT_TRUE(decoded.Read(source));
    EXPECT_EQ(VN_MONITOR_STREAM_STATE, decoded.record.header.streamKind);
    EXPECT_EQ(11u, decoded.record.header.telemetry.logicCommitSequence);
    EXPECT_EQ(13u, decoded.payload.readCutSequence);
    EXPECT_TRUE(FactHas(decoded.payload, VN_MONITOR_FACT_FIELD_READ_CUT_ORDINAL));
    EXPECT_TRUE(FactHas(decoded.payload, VN_MONITOR_FACT_FIELD_EXECUTE_FRAME));
    EXPECT_EQ(0u, decoded.payload.readCutOrdinalExclusive);
    auto invalid = decoded.payload;
    invalid.readCutKey = 12;
    EXPECT_FALSE(ValidateExecutionFact(decoded.record.header, invalid));
    invalid.readCutKey = UINT64_C(0x00000001ffffffff);
    EXPECT_FALSE(ValidateExecutionFact(decoded.record.header, invalid));
    SetSourceField(source, VN_MONITOR_FACT_FIELD_OWNER_EPOCH);
    EXPECT_FALSE(decoded.Read(source));
}

TEST(MonitorExecutionCodec, OnlyOneRelationCanBeValidWithoutOtherIdentityFields) {
    auto source = Execution();
    source.body.execution.relations.parent = UINT64_C(0xa00000003);
    SetSourceField(source, VN_MONITOR_FACT_FIELD_PARENT_PLAYABLE);
    RoundTrip decoded;
    ASSERT_TRUE(decoded.Read(source));
    EXPECT_EQ(source.body.execution.relations.parent, decoded.record.header.playableInstanceHandle);
    EXPECT_FALSE(FactHas(decoded.payload, VN_MONITOR_FACT_FIELD_SUBJECT));
    EXPECT_FALSE(FactHas(decoded.payload, VN_MONITOR_FACT_FIELD_CONTENT_OBJECT));
    EXPECT_FALSE(FactHas(decoded.payload, VN_MONITOR_FACT_FIELD_ARRANGING));
}

TEST(MonitorExecutionCodec, RawZeroTargetIsPresentButValidatedTargetRequiresTypedGeneration) {
    auto source = Call();
    source.body.call.target.kind = VN_MONITOR_TARGET_PLAYABLE;
    SetSourceField(source, VN_MONITOR_FACT_FIELD_TARGET_HANDLE);
    SetSourceField(source, VN_MONITOR_FACT_FIELD_TARGET_KIND);
    RoundTrip decoded;
    ASSERT_TRUE(decoded.Read(source));
    EXPECT_TRUE(FactHas(decoded.payload, VN_MONITOR_FACT_FIELD_TARGET_HANDLE));
    EXPECT_EQ(0u, decoded.payload.target.handle);
    SetSourceField(source, VN_MONITOR_FACT_FIELD_TARGET_VALIDATED);
    EXPECT_FALSE(decoded.Read(source));
    source.body.call.target.handle = 1;
    EXPECT_FALSE(decoded.Read(source));
    source.body.call.target.handle = UINT64_C(0x8000000900000003);
    EXPECT_FALSE(decoded.Read(source));
    source.body.call.target.kind = VN_MONITOR_TARGET_PREVIEW_PLAYABLE;
    ASSERT_TRUE(decoded.Read(source));
    EXPECT_EQ(source.body.call.target.handle, decoded.payload.target.handle);
    source.body.call.target.handle = UINT64_C(0x8000000000000003);
    EXPECT_FALSE(decoded.Read(source));
}

TEST(MonitorExecutionCodec, MissingOrdinalSentinelCannotBeAdvertisedAsAnExecutionOrdinal) {
    auto source = Execution();
    source.body.execution.requests.controlOrdinal = 0;
    source.body.execution.requests.selectionOrdinal = 0;
    SetSourceField(source, VN_MONITOR_FACT_FIELD_CONTROL_ORDINAL);
    SetSourceField(source, VN_MONITOR_FACT_FIELD_SELECTION_ORDINAL);
    RoundTrip decoded;
    ASSERT_TRUE(decoded.Read(source));
    source.body.execution.requests.controlOrdinal = UINT32_MAX;
    EXPECT_FALSE(decoded.Read(source));
    source.body.execution.requests.controlOrdinal = 0;
    source.body.execution.requests.selectionOrdinal = UINT32_MAX;
    EXPECT_FALSE(decoded.Read(source));
}

TEST(MonitorExecutionCodec, RejectsMalformedVersionLengthEnumsReservedAndValidity) {
    RoundTrip decoded;
    ASSERT_TRUE(decoded.Read(Execution()));
    const auto original = decoded.payload;
    auto header = decoded.record.header;
    header.recordVersion = 2;
    EXPECT_FALSE(ValidateExecutionFact(header, original));
    for (const auto length : {UINT64_C(0), UINT64_C(431), UINT64_C(433), UINT64_C(512)})
        EXPECT_FALSE(DecodeExecutionFact(decoded.record.header, decoded.bytes.data() + kPackedEnvelopeBytes, length, decoded.payload));
    auto bad = original;
    bad.factKind = 255;
    EXPECT_FALSE(ValidateExecutionFact(decoded.record.header, bad));
    bad = original; bad.subjectNamespace = 255;
    EXPECT_FALSE(ValidateExecutionFact(decoded.record.header, bad));
    bad = original; bad.reserved1 = 1;
    EXPECT_FALSE(ValidateExecutionFact(decoded.record.header, bad));
    bad = original; bad.control.reserved0 = 1;
    EXPECT_FALSE(ValidateExecutionFact(decoded.record.header, bad));
    bad = original; bad.validityHi = UINT64_C(1) << 10;
    EXPECT_FALSE(ValidateExecutionFact(decoded.record.header, bad));
    bad = original; bad.hostSession = 1;
    EXPECT_FALSE(ValidateExecutionFact(decoded.record.header, bad));
    auto source = Execution();
    source.validity = UINT64_C(1) << 63;
    EXPECT_FALSE(decoded.Read(source));
}

TEST(MonitorExecutionCodec, DoesNotTouchOutputOnInvalidSourceOrShortBuffer) {
    auto source = Execution();
    std::array<std::uint8_t, kExecutionRecordBytes + 8> bytes;
    bytes.fill(0xa5);
    EXPECT_EQ(0u, PackExecutionFact(bytes.data(), kExecutionRecordBytes - 1, source, 1, 2));
    source.engine = 0;
    EXPECT_EQ(0u, PackExecutionFact(bytes.data(), kExecutionRecordBytes, source, 1, 2));
    for (auto byte : bytes) EXPECT_EQ(0xa5u, byte);
    source.engine = 1;
    EXPECT_EQ(kExecutionRecordBytes, PackExecutionFact(bytes.data(), kExecutionRecordBytes, source, 1, 2));
    for (unsigned i = kExecutionRecordBytes; i != bytes.size(); ++i) EXPECT_EQ(0xa5u, bytes[i]);
    for (unsigned i = kPackedEnvelopeBytes + kExecutionPayloadBytes; i != kExecutionRecordBytes; ++i) EXPECT_EQ(0u, bytes[i]);
}

} // namespace

namespace {
TEST(MonitorIdentityCodec, BaselineAndDeltaKeepNativeReadCutIndependent) {
    auto source = Execution(VN_MONITOR_FACT_IDENTITY_UPSERT);
    source.subjectNamespace = VN_MONITOR_SUBJECT_PLAYABLE;
    source.body.execution.identity.handle = UINT64_C(7) << 32;
    source.body.execution.position.readCutKey = (UINT64_C(4) << 32) | 3;
    source.body.execution.position.readCutSequence = 15;
    source.body.execution.position.detail.readCut.exclusiveOrdinal = 6;
    SetSourceField(source, VN_MONITOR_FACT_FIELD_SUBJECT);
    SetSourceField(source, VN_MONITOR_FACT_FIELD_READ_CUT_KEY);
    SetSourceField(source, VN_MONITOR_FACT_FIELD_READ_CUT_SEQUENCE);
    SetSourceField(source, VN_MONITOR_FACT_FIELD_READ_CUT_ORDINAL);
    for (const auto cut : {UINT64_C(0), UINT64_C(19)}) {
        std::array<std::uint8_t, kIdentityRecordBytes> bytes{}, skipped{};
        ASSERT_EQ(bytes.size(), PackIdentityFact(bytes.data(), bytes.size(), source, cut, 23));
        VnMonitorDecodedRecord record{};
        const auto decoded = vn_monitor_decode_records(bytes.data(), bytes.size(), &record, 1, skipped.data(), skipped.size());
        ASSERT_EQ(VN_OK, decoded.status);
        ASSERT_EQ(1u, decoded.recordsWritten);
        EXPECT_EQ(bytes.size(), record.skippedBytesCount);
        VnMonitorIdentityFactV1 identity{};
        ASSERT_TRUE(DecodeIdentityFact(record.header, bytes.data() + kPackedEnvelopeBytes, kIdentityPayloadBytes, identity));
        EXPECT_EQ(cut, identity.stateCutId);
        EXPECT_EQ(23u, identity.identityRevision);
        EXPECT_EQ((UINT64_C(4) << 32) | 3, identity.fact.readCutKey);
        EXPECT_EQ(15u, identity.fact.readCutSequence);
        EXPECT_EQ(6u, identity.fact.readCutOrdinalExclusive);
        EXPECT_EQ(cut ? kIdentityBaselineProducer : kIdentityDeltaProducer, record.header.producerId);
        EXPECT_FALSE(ValidateExecutionFact(record.header, identity.fact));
        auto bad = identity;
        bad.reserved0 = 1;
        EXPECT_FALSE(ValidateIdentityFact(record.header, bad));
        bad = identity;
        bad.identityRevision = 0;
        EXPECT_FALSE(ValidateIdentityFact(record.header, bad));
        auto wrong = record.header;
        wrong.producerId = 3;
        EXPECT_FALSE(ValidateIdentityFact(wrong, identity));
        wrong = record.header;
        wrong.streamKind = VN_MONITOR_STREAM_EVENT;
        EXPECT_FALSE(ValidateIdentityFact(wrong, identity));
        wrong = record.header;
        wrong.recordVersion = 2;
        EXPECT_FALSE(ValidateIdentityFact(wrong, identity));
        EXPECT_FALSE(DecodeIdentityFact(record.header, bytes.data() + kPackedEnvelopeBytes,
            kIdentityPayloadBytes - 1, bad));
    }
    source.kind = VN_MONITOR_FACT_IDENTITY_TOMBSTONE;
    std::array<std::uint8_t, kIdentityRecordBytes> bytes{};
    EXPECT_EQ(0u, PackIdentityFact(bytes.data(), bytes.size(), source, 19, 23));
    EXPECT_EQ(bytes.size(), PackIdentityFact(bytes.data(), bytes.size(), source, 0, 24));
    source.kind = VN_MONITOR_FACT_PLAYABLE_ACTIVE;
    EXPECT_EQ(0u, PackIdentityFact(bytes.data(), bytes.size(), source, 0, 25));
}

TEST(MonitorIdentityCodec, NamespaceCountsRejectUnsupportedAndOverflow) {
    std::uint64_t counts[9]{};
    counts[VN_MONITOR_SUBJECT_EMITTER] = 1;
    counts[VN_MONITOR_SUBJECT_ACTION] = 2;
    EXPECT_TRUE(ValidateIdentityCounts(counts, 3));
    EXPECT_FALSE(ValidateIdentityCounts(counts, 2));
    counts[VN_MONITOR_SUBJECT_NONE] = 1;
    EXPECT_FALSE(ValidateIdentityCounts(counts, 4));
    counts[VN_MONITOR_SUBJECT_NONE] = 0;
    counts[VN_MONITOR_SUBJECT_PLAYABLE] = UINT64_MAX;
    EXPECT_FALSE(ValidateIdentityCounts(counts, UINT64_MAX));
}
} // namespace

namespace {
TEST(MonitorIdentityCodec, CutControlsRoundTripAndRejectForeignEnvelopeFields) {
    VnMonitorIdentityCutBeginV1 begin{};
    begin.structSize = sizeof(begin);
    begin.stateCutId = 4;
    begin.identityRevision = 7;
    begin.deltaProducerId = kIdentityDeltaProducer;
    begin.deltaSourceSequence = 9;
    begin.capacityLimit = 3;
    begin.namespaceTrueCount[VN_MONITOR_SUBJECT_PLAYABLE] = 2;
    begin.namespaceTrueCount[VN_MONITOR_SUBJECT_EMITTER] = 1;
    auto header = IdentityCutHeader(1, 10, 2, VN_MONITOR_RECORD_IDENTITY_CUT_BEGIN, kIdentityBeginBytes);
    std::array<std::uint8_t, 832> bytes{};
    ASSERT_EQ(PackedRecordBytes(kIdentityBeginBytes), PackIdentityBegin(bytes.data(), bytes.size(), header, begin));
    VnMonitorIdentityCutBeginV1 decoded{};
    ASSERT_TRUE(DecodeIdentityBegin(header, bytes.data() + kPackedEnvelopeBytes, kIdentityBeginBytes, decoded));
    EXPECT_EQ(7u, decoded.identityRevision);
    EXPECT_EQ(9u, decoded.deltaSourceSequence);
    EXPECT_EQ(1u, decoded.namespaceTrueCount[VN_MONITOR_SUBJECT_EMITTER]);
    header.instanceHandle = UINT64_C(1) << 32;
    EXPECT_FALSE(ValidateIdentityBegin(header, begin));
    header.instanceHandle = 0;
    header.deliverySequence = 12;
    EXPECT_TRUE(ValidateIdentityBegin(header, begin));
    begin.reserved0 = 1;
    EXPECT_FALSE(ValidateIdentityBegin(header, begin));
    begin.reserved0 = 0;
    begin.capacityLimit = 2;
    EXPECT_FALSE(ValidateIdentityBegin(header, begin));
    VnMonitorIdentityCutEndV1 end{};
    end.structSize = sizeof(end);
    end.stateCutId = 4;
    end.identityRevision = 7;
    end.result = VN_MONITOR_STATE_CUT_COMPLETE;
    end.checksum = 0xfedcba;
    end.namespaceEmittedCount[VN_MONITOR_SUBJECT_PLAYABLE] = 2;
    end.namespaceEmittedCount[VN_MONITOR_SUBJECT_EMITTER] = 1;
    header = IdentityCutHeader(1, 14, 2, VN_MONITOR_RECORD_IDENTITY_CUT_END, kIdentityEndBytes);
    ASSERT_EQ(PackedRecordBytes(kIdentityEndBytes), PackIdentityEnd(bytes.data(), bytes.size(), header, end));
    VnMonitorIdentityCutEndV1 decodedEnd{};
    ASSERT_TRUE(DecodeIdentityEnd(header, bytes.data() + kPackedEnvelopeBytes, kIdentityEndBytes, decodedEnd));
    EXPECT_EQ(end.checksum, decodedEnd.checksum);
    EXPECT_EQ(1u, decodedEnd.namespaceEmittedCount[VN_MONITOR_SUBJECT_EMITTER]);
    end.result = UINT32_MAX;
    EXPECT_FALSE(ValidateIdentityEnd(header, end));
}
} // namespace

namespace {
TEST(MonitorIdentityCut, CompleteRequiresEveryRowAndExactCountsRevisionAndChecksum) {
    VnMonitorIdentityCutBeginV1 begin{};
    begin.structSize = sizeof(begin);
    begin.stateCutId = 91;
    begin.identityRevision = 8;
    begin.deltaProducerId = kIdentityDeltaProducer;
    begin.capacityLimit = 2;
    begin.namespaceTrueCount[VN_MONITOR_SUBJECT_PLAYABLE] = 2;
    const auto first = IdentityCutHeader(1, 40, 3, VN_MONITOR_RECORD_IDENTITY_CUT_BEGIN, kIdentityBeginBytes);
    VnMonitorRecordHeader headers[2]{};
    VnMonitorIdentityFactV1 rows[2]{};
    auto checksum = kIdentityChecksumSeed;
    for (unsigned i = 0; i != 2; ++i) {
        auto source = Execution(VN_MONITOR_FACT_IDENTITY_UPSERT);
        source.engine = 1;
        source.sourceSequence = 41 + i;
        source.subjectNamespace = VN_MONITOR_SUBJECT_PLAYABLE;
        source.body.execution.identity.handle = (UINT64_C(7) << 32) | i;
        SetSourceField(source, VN_MONITOR_FACT_FIELD_SUBJECT);
        std::array<std::uint8_t, kIdentityRecordBytes> bytes{};
        ASSERT_EQ(bytes.size(), PackIdentityFact(bytes.data(), bytes.size(), source, 91, 8));
        ASSERT_TRUE(ReadIdentityHeader(bytes.data(), bytes.size(), headers[i]));
        ASSERT_TRUE(DecodeIdentityFact(headers[i], bytes.data() + kPackedEnvelopeBytes, kIdentityPayloadBytes, rows[i]));
        checksum = IdentityRowChecksum(checksum, headers[i], rows[i]);
        headers[i].monitorSessionEpoch = 3;
        headers[i].deliverySequence = 500 + i;
        EXPECT_EQ(checksum, i == 0 ? IdentityRowChecksum(kIdentityChecksumSeed, headers[i], rows[i]) :
            IdentityRowChecksum(IdentityRowChecksum(kIdentityChecksumSeed, headers[0], rows[0]), headers[i], rows[i]));
    }
    VnMonitorIdentityCutEndV1 end{};
    end.structSize = sizeof(end);
    end.result = VN_MONITOR_STATE_CUT_COMPLETE;
    end.stateCutId = 91;
    end.identityRevision = 8;
    end.checksum = checksum;
    end.namespaceEmittedCount[VN_MONITOR_SUBJECT_PLAYABLE] = 2;
    const auto last = IdentityCutHeader(1, 43, 3, VN_MONITOR_RECORD_IDENTITY_CUT_END, kIdentityEndBytes);
    MonitorIdentityCutVerifier valid;
    ASSERT_TRUE(valid.Begin(first, begin));
    EXPECT_TRUE(valid.Row(headers[0], rows[0]));
    EXPECT_TRUE(valid.Row(headers[1], rows[1]));
    EXPECT_TRUE(valid.End(last, end));
    for (unsigned fault = 0; fault != 6; ++fault) {
        MonitorIdentityCutVerifier cut;
        ASSERT_TRUE(cut.Begin(first, begin));
        auto header = headers[0];
        auto row = rows[0];
        auto finish = end;
        if (fault == 0) { // Both controls arrive, but the first row is missing.
            EXPECT_FALSE(cut.Row(headers[1], rows[1]));
        } else {
            if (fault == 1) ++header.monitorSessionEpoch;
            if (fault == 2) ++row.identityRevision;
            cut.Row(header, row);
            if (fault == 3) cut.Row(header, row);
            cut.Row(headers[1], rows[1]);
            if (fault == 4) ++finish.checksum;
            if (fault == 5) --finish.namespaceEmittedCount[VN_MONITOR_SUBJECT_PLAYABLE];
        }
        EXPECT_FALSE(cut.End(last, finish)) << fault;
    }
}
} // namespace

namespace {
MonitorHostSymbolValue SymbolHost() {
    MonitorHostSymbolValue host{};
    host.origin.emitter = (UINT64_C(7) << 32) | 3;
    host.origin.host = {11, 12, 13};
    host.origin.registrationRevision = 14;
    host.description.nameBytes = 7;
    const std::uint8_t text[]{0xe4, 0xb8, 0xad, 0xf0, 0x9f, 0x8e, 0xb5};
    std::memcpy(host.description.name, text, sizeof(text));
    host.description.pathBytes = 256;
    std::memset(host.description.path, 'p', 256);
    return host;
}

TEST(MonitorHostSymbolCodec, RetainsUtf8EmptyValuesAndExactProducerProjection) {
    const auto host = SymbolHost();
    for (const auto stream : {VN_MONITOR_STREAM_EVENT, VN_MONITOR_STREAM_STATE}) {
        for (unsigned field = 1; field != 4; ++field) {
            auto source = HostSymbolField(host, field, VN_MONITOR_SYMBOL_UPSERT, 17);
            source.sourceSequence = 19;
            std::array<std::uint8_t, kHostSymbolRecordBytes> bytes{};
            ASSERT_EQ(bytes.size(), PackHostSymbol(bytes.data(), bytes.size(), source, 0, stream));
            VnMonitorRecordHeader header{};
            VnMonitorHostObjectSymbolV1 value{};
            ASSERT_TRUE(ReadIdentityHeader(bytes.data(), bytes.size(), header));
            ASSERT_TRUE(DecodeHostSymbol(header, bytes.data() + kPackedEnvelopeBytes, kHostSymbolPayloadBytes, value));
            EXPECT_EQ(kHostSymbolProducer, header.producerId);
            EXPECT_EQ(kHostSymbolStream, header.streamId);
            EXPECT_EQ(stream, header.streamKind);
            EXPECT_EQ(host.origin.emitter, header.instanceHandle);
            EXPECT_EQ(7u, header.instanceGeneration);
            EXPECT_EQ(17u, value.cutRevision);
            EXPECT_EQ(14u, value.registrationRevision);
            EXPECT_EQ(source.value.textBytes, value.textBytes);
            EXPECT_EQ(0, std::memcmp(source.value.text, value.text, sizeof(value.text)));
            if (field == VN_MONITOR_HOST_SYMBOL_SCENE) EXPECT_EQ(0u, value.textBytes);
            auto bad = value;
            bad.field = 4;
            EXPECT_FALSE(ValidateHostSymbol(header, bad));
            bad = value;
            bad.textBytes = field == 1 ? 65 : (field == 2 ? 257 : 129);
            EXPECT_FALSE(ValidateHostSymbol(header, bad));
            bad = value;
            bad.textBytes = 1;
            bad.text[0] = 0xff;
            EXPECT_FALSE(ValidateHostSymbol(header, bad));
            bad = value;
            bad.flags = 2;
            EXPECT_FALSE(ValidateHostSymbol(header, bad));
            auto wrong = header;
            wrong.actionInstanceHandle = 1;
            EXPECT_FALSE(ValidateHostSymbol(wrong, value));
            wrong = header;
            ++wrong.instanceGeneration;
            EXPECT_FALSE(ValidateHostSymbol(wrong, value));
            wrong = header;
            wrong.streamKind = VN_MONITOR_STREAM_EVENT | VN_MONITOR_STREAM_STATE;
            EXPECT_FALSE(ValidateHostSymbol(wrong, value));
        }
    }
    auto source = HostSymbolField(host, 1, VN_MONITOR_SYMBOL_TOMBSTONE, 18);
    source.sourceSequence = 20;
    std::array<std::uint8_t, kHostSymbolRecordBytes> bytes{};
    EXPECT_EQ(0u, PackHostSymbol(bytes.data(), bytes.size(), source, 1, VN_MONITOR_STREAM_STATE));
    EXPECT_EQ(bytes.size(), PackHostSymbol(bytes.data(), bytes.size(), source, 0, VN_MONITOR_STREAM_EVENT));
    source.value.factKind = VN_MONITOR_SYMBOL_UPSERT;
    EXPECT_EQ(0u, PackHostSymbol(bytes.data(), bytes.size(), source, 1, VN_MONITOR_STREAM_EVENT));
    EXPECT_EQ(bytes.size(), PackHostSymbol(bytes.data(), bytes.size(), source, 1, VN_MONITOR_STREAM_STATE));
}

TEST(MonitorHostSymbolCodec, CutRequiresAllThreeFieldsFromOneRevisionAndBothSourceWatermarks) {
    VnMonitorIdentityCutBeginV1 begin{};
    begin.structSize = sizeof(begin);
    begin.stateCutId = 8;
    begin.identityRevision = 17;
    begin.deltaProducerId = kIdentityDeltaProducer;
    begin.deltaSourceSequence = 5;
    begin.symbolProducerId = kHostSymbolProducer;
    begin.symbolSourceSequence = 6;
    begin.capacityLimit = 3;
    begin.namespaceTrueCount[VN_MONITOR_SUBJECT_HOST_SYMBOL] = 3;
    const auto first = IdentityCutHeader(11, 10, 9, VN_MONITOR_RECORD_IDENTITY_CUT_BEGIN, kIdentityBeginBytes);
    std::array<std::uint8_t, kHostSymbolRecordBytes> bytes{};
    ASSERT_NE(0u, PackIdentityBegin(bytes.data(), bytes.size(), first, begin));
    VnMonitorIdentityCutBeginV1 decoded{};
    ASSERT_TRUE(DecodeIdentityBegin(first, bytes.data() + kPackedEnvelopeBytes, kIdentityBeginBytes, decoded));
    EXPECT_EQ(7u, decoded.symbolProducerId);
    EXPECT_EQ(6u, decoded.symbolSourceSequence);
    auto badBegin = begin;
    badBegin.symbolProducerId = 0;
    EXPECT_FALSE(ValidateIdentityBegin(first, badBegin));
    VnMonitorRecordHeader headers[3]{};
    VnMonitorHostObjectSymbolV1 values[3]{};
    auto checksum = kIdentityChecksumSeed;
    for (unsigned i = 0; i != 3; ++i) {
        auto source = HostSymbolField(SymbolHost(), i + 1, VN_MONITOR_SYMBOL_UPSERT, 17);
        source.sourceSequence = 11 + i;
        ASSERT_EQ(bytes.size(), PackHostSymbol(bytes.data(), bytes.size(), source, 8, VN_MONITOR_STREAM_STATE));
        ASSERT_TRUE(ReadIdentityHeader(bytes.data(), bytes.size(), headers[i]));
        ASSERT_TRUE(DecodeHostSymbol(headers[i], bytes.data() + kPackedEnvelopeBytes, kHostSymbolPayloadBytes, values[i]));
        checksum = HostSymbolRowChecksum(checksum, headers[i], values[i]);
        headers[i].monitorSessionEpoch = 9;
        headers[i].deliverySequence = 500 + i;
    }
    VnMonitorIdentityCutEndV1 end{};
    end.structSize = sizeof(end);
    end.stateCutId = 8;
    end.identityRevision = 17;
    end.result = VN_MONITOR_STATE_CUT_COMPLETE;
    end.checksum = checksum;
    end.namespaceEmittedCount[VN_MONITOR_SUBJECT_HOST_SYMBOL] = 3;
    const auto last = IdentityCutHeader(11, 14, 9, VN_MONITOR_RECORD_IDENTITY_CUT_END, kIdentityEndBytes);
    for (unsigned fault = 0; fault != 8; ++fault) {
        MonitorIdentityCutVerifier verifier;
        ASSERT_TRUE(verifier.Begin(first, begin));
        for (unsigned i = 0; i != 3; ++i) {
            auto h = headers[i];
            auto v = values[i];
            if (fault == 1 && i == 1) continue;
            if (fault == 2 && i == 1) ++v.registrationRevision;
            if (fault == 3 && i == 1) --v.field;
            if (fault == 4 && i == 1) ++v.hostSequence;
            if (fault == 5 && i == 1) ++h.sourceSequence;
            if (fault == 6 && i == 1) ++v.cutRevision;
            if (fault == 7 && i == 1) ++h.monitorSessionEpoch;
            verifier.Row(h, v);
        }
        EXPECT_EQ(fault == 0, verifier.End(last, end)) << fault;
    }
}

TEST(MonitorHostSymbolState, SilentSeedAndImmutableCopyPreserveCaptureWindowsAndRetiredValues) {
    MonitorHostSymbolMirror mirror;
    mirror.Reset(1);
    auto current = SymbolHost();
    const auto capture = [&](auto, MonitorHostSymbolValue& out) { out = current; return current.Present(); };
    std::vector<MonitorHostSymbolSource> changes;
    const auto publish = [&](const MonitorHostSymbolValue& host, unsigned kind) {
        for (unsigned field = 1; field != 4; ++field)
            changes.push_back(HostSymbolField(host, field, kind, mirror.Revision()));
    };
    mirror.Seed(capture);
    ASSERT_TRUE(mirror.Refresh(capture, publish));
    EXPECT_EQ(1u, mirror.Revision());
    EXPECT_TRUE(changes.empty());
    std::array<MonitorHostSymbolValue, 1> frozen{};
    std::uint32_t copied = 0;
    std::uint64_t count = 0;
    ASSERT_TRUE(mirror.Copy(frozen, copied, count));
    EXPECT_EQ(3u, count);
    EXPECT_EQ(1u, copied);
    ++current.origin.registrationRevision;
    current.description.name[0] = 'X';
    ASSERT_TRUE(mirror.Refresh(capture, publish));
    ASSERT_EQ(3u, changes.size());
    EXPECT_EQ(14u, frozen[0].origin.registrationRevision);
    EXPECT_NE(current.description.name[0], frozen[0].description.name[0]);
    ++current.origin.host.sdkSession;
    ASSERT_TRUE(mirror.Refresh(capture, publish));
    ASSERT_EQ(9u, changes.size());
    EXPECT_EQ(VN_MONITOR_SYMBOL_TOMBSTONE, changes[3].value.factKind);
    EXPECT_EQ(12u, changes[3].value.sdkSession);
    EXPECT_EQ(VN_MONITOR_SYMBOL_UPSERT, changes[6].value.factKind);
    EXPECT_EQ(13u, changes[6].value.sdkSession);
    EXPECT_FALSE(mirror.Copy({}, copied, count));
    EXPECT_EQ(3u, count);
    EXPECT_EQ(0u, copied);
    EXPECT_TRUE(mirror.Refresh(capture, publish, false));
    EXPECT_EQ(0u, mirror.Unavailable());
    current = {};
    EXPECT_FALSE(mirror.Refresh(capture, publish, false));
    EXPECT_EQ(1u, mirror.Unavailable());
    EXPECT_EQ(9u, changes.size());
    ASSERT_TRUE(mirror.Refresh(capture, publish));
    ASSERT_EQ(12u, changes.size());
    EXPECT_EQ(VN_MONITOR_SYMBOL_TOMBSTONE, changes[9].value.factKind);
    EXPECT_EQ(15u, changes[9].value.registrationRevision);
}
} // namespace
