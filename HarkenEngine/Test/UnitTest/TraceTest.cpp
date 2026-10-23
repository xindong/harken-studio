// Session-trace capture surface: the Vn_TraceStart origin gate (stateful
// calls close it, queries do not), the refusal codes and their pinned
// values, the bad-path and mid-session write-failure containment (engine
// keeps running, sticky traceFailed + log record), the implicit finalize on
// Vn_Term, and the journal file's header identity. Replay determinism is
// the E2E suite's job (TraceReplayTest.cpp); this file owns the recorder
// side.
//
// Audio-thread contact (code-review-level assertion): tracing is a logic
// thread API journal. The recorder is owned by the Engine, written only
// from public API entry hooks, Vn_Update, and Vn_TraceStart/Stop — all
// externally synchronized logic-thread calls. Nothing under
// Source/Runtime/render includes TraceRecorder.h, and the mixer's render
// path carries no trace branch; the recorder OBSERVES the audio thread only
// through the snapshot double buffer's reader side, which the audio thread
// never waits on. Tracing OFF is one null-pointer test per API call.
#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "Common/TestUtil.h"
#include "TraceFormat.h"
#include "TraceCodec.h"
#include "TraceRecorder.h"
#include "harken/core/EngineConfig.h"
#include "harken/param/MeterProbe.h"
#include "harken/param/VariableService.h"
#include "harken/render/DynamicRoute.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

using harkentest::EngineSession;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::MonoDesc;

// The new error codes are ABI: pin the numeric values (the S0/S1 pin
// discipline for every appended enum value).
static_assert(VN_TRACE_TOO_LATE == -318,
              "trace gate refusal sits in the -3xx state band");
static_assert(VN_BAD_TRACE_FILE == -403,
              "trace file refusal sits in the -4xx system band");
static_assert(harken::kTraceFormatVersion == 7,
              "the portable semantic trace format is VTRC v6");
static_assert(harken::kTraceCaptureResultFeature == 0x52504356u &&
                  harken::kTraceCaptureResultVintage == 1u &&
                  harken::kTraceCaptureResultTailBytes == 16u &&
                  harken::kTraceRequiredCountUnavailable == 0xFFFFFFFFu,
              "VTRC v2 capture-result tail boundary is pinned");
static_assert(harken::kTraceCallSetBusEffectDynamicRoute == 79 &&
                  harken::kTraceCallClearBusEffectDynamicRoute == 80 &&
                  harken::kTraceCallResetVoiceEffect == 81 &&
                  harken::kTraceCallResetBusEffect == 82 &&
                  harken::kTraceCallCaptureEffectDynamicCapabilities == 83 &&
                  harken::kTraceCallCaptureEffectDynamicParameterCapabilities ==
                      84 &&
                  harken::kTraceCallCaptureEffectDynamicRoutes == 85 &&
                  harken::kTraceCallCaptureCompressorRuntimeInfo == 86,
              "M7 Dynamic Route and Compressor VTRC call ids are pinned");
static_assert(harken::kTraceCallReservedLegacyActionTriggerEx == 87 &&
                  harken::kTraceCallReservedLegacyActionStop == 88 &&
                  harken::kTraceCallReservedLegacyActionInstanceState == 89 &&
                  harken::kTraceCallStopAll == 90 &&
                  harken::kTraceCallApplyBusTopology == 91,
              "legacy Action tombstones and current VTRC call ids are pinned");
static_assert(harken::kTraceCallSetVoicePan == 92 &&
                  harken::kTraceCallSetVoiceWidth == 93,
              "voice pan and width VTRC call ids are pinned");
static_assert(harken::kTraceCallCommitVoiceStart == 94,
              "staged-install commit VTRC call id is pinned");
static_assert(harken::kTraceCallPostAction == 99 &&
                  harken::kTraceCallStopActionFiring == 100 &&
                  harken::kTraceCallQueryActionFiring == 101 &&
                  harken::kTraceCallQueryActionStep == 102 &&
                  harken::kTraceCallReleaseActionFiring == 103,
              "Action-firing VTRC call ids are pinned");
static_assert(harken::kTraceCallUnloadPackEx == 104,
              "Pack unload policy VTRC call id is pinned");
static_assert(harken::kTraceCallPreviewResetPlayable == 105,
              "Preview reset VTRC call id is pinned");

namespace {

constexpr std::uint32_t kBlockFrames = 256;

struct TraceRecorderFaultGuard {
    explicit TraceRecorderFaultGuard(harken::TraceRecorderFault fault) {
        harken::SetTraceRecorderFaultForTests(fault);
    }
    ~TraceRecorderFaultGuard() {
        harken::SetTraceRecorderFaultForTests(
            harken::TraceRecorderFault::None);
    }
    TraceRecorderFaultGuard(const TraceRecorderFaultGuard&) = delete;
    TraceRecorderFaultGuard& operator=(const TraceRecorderFaultGuard&) =
        delete;
};

std::string TracePath(const std::string& name) {
    std::filesystem::create_directories(HARKEN_TEMP_DIR);
    return std::string(HARKEN_TEMP_DIR "/") + name;
}

VnSnapshot Capture() {
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    EXPECT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    return snapshot;
}

void RegisterToneSound(VnSoundId id) {
    const std::vector<float> tone =
        harkentest::MakeSine(4096, 440.0, 48000.0, 0.5f);
    VnPCMDesc pcm = MonoDesc(tone, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(id, &pcm));
}

// Proves the engine still renders after a trace-layer failure: register,
// play, pump, and observe a non-silent master peak.
void AssertEngineStillSounds(harken::Engine* engine, VnSoundId id) {
    ASSERT_NO_FATAL_FAILURE(RegisterToneSound(id));
    VnPlayDesc play = MakePlayDesc(id);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    engine->nullBackend.Pump(2);
    const VnSnapshot snapshot = Capture();
    EXPECT_GT(snapshot.busPeaks[0], harkentest::kSilenceFloor);
}

// Minimal VTRC structure check: magic, version, and a walk over the
// length-prefixed records; reports the last record's type (0 = none).
struct JournalShape {
    bool magicOk = false;
    std::uint32_t version = 0;
    std::uint64_t recordCount = 0;
    std::uint16_t lastRecordType = 0;
    bool wellFormed = false;
};

JournalShape ReadJournalShape(const std::string& path) {
    JournalShape shape;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return shape;
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    if (bytes.size() < harken::kTraceFixedHeaderBytes) {
        return shape;
    }
    shape.magicOk = std::memcmp(bytes.data(), harken::kTraceMagic, 4) == 0;
    std::memcpy(&shape.version, bytes.data() + 4, 4);
    std::uint32_t headerBytes = 0;
    std::memcpy(&headerBytes, bytes.data() + 8, 4);
    if (headerBytes > bytes.size()) {
        return shape;
    }
    std::size_t offset = headerBytes;
    while (offset + harken::kTraceRecordHeaderBytes <= bytes.size()) {
        std::uint32_t payloadBytes = 0;
        std::uint16_t type = 0;
        std::memcpy(&payloadBytes, bytes.data() + offset, 4);
        std::memcpy(&type, bytes.data() + offset + 4, 2);
        offset += harken::kTraceRecordHeaderBytes;
        if (bytes.size() - offset < payloadBytes) {
            return shape; // truncated payload: not well formed
        }
        offset += payloadBytes;
        ++shape.recordCount;
        shape.lastRecordType = type;
    }
    shape.wellFormed = offset == bytes.size();
    return shape;
}

struct JournalRecord {
    std::uint16_t type = 0;
    std::vector<std::uint8_t> payload;
};

std::vector<JournalRecord> ReadJournalRecords(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    std::vector<JournalRecord> records;
    if (bytes.size() < harken::kTraceFixedHeaderBytes) {
        return records;
    }
    std::uint32_t headerBytes = 0;
    std::memcpy(&headerBytes, bytes.data() + 8, sizeof(headerBytes));
    std::size_t offset = headerBytes;
    while (offset + harken::kTraceRecordHeaderBytes <= bytes.size()) {
        std::uint32_t payloadBytes = 0;
        std::uint16_t type = 0;
        std::memcpy(&payloadBytes, bytes.data() + offset,
                    sizeof(payloadBytes));
        std::memcpy(&type, bytes.data() + offset + 4, sizeof(type));
        offset += harken::kTraceRecordHeaderBytes;
        if (payloadBytes > bytes.size() - offset) {
            records.clear();
            return records;
        }
        JournalRecord record;
        record.type = type;
        record.payload.assign(bytes.begin() + offset,
                              bytes.begin() + offset + payloadBytes);
        records.push_back(std::move(record));
        offset += payloadBytes;
    }
    if (offset != bytes.size()) {
        records.clear();
    }
    return records;
}

std::uint16_t PayloadCallId(const JournalRecord& record) {
    std::uint16_t callId = 0;
    if (record.type == harken::kTraceRecordApiCall &&
        record.payload.size() >= sizeof(callId)) {
        std::memcpy(&callId, record.payload.data(), sizeof(callId));
    }
    return callId;
}

void InstallImmediateTraceActionWorld(harken::Engine* engine,
                                      VnObjectId actionId) {
    harken::VariableDefinitionV1 variable = harkentest::MakeVariableDefinition(
        harkentest::ObjectId(1), harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.0f);
    harken::ActionStepProgramViewV1 step{};
    step.opcode = harken::ActionOpcodeV1::SetVariable;
    step.target.kind = harken::ActionTargetKindV1::Variable;
    step.target.stableId = variable.variableId;
    step.delay.domain = harken::AuthoredTimeDomainV1::AbsoluteNanoseconds;
    step.delay.denominator = 1;
    step.fade.domain = harken::AuthoredTimeDomainV1::AbsoluteNanoseconds;
    step.fade.denominator = 1;
    step.cooldown.domain = harken::AuthoredTimeDomainV1::AbsoluteNanoseconds;
    step.cooldown.denominator = 1;
    step.valueBits = std::bit_cast<std::uint32_t>(0.5f);

    harken::ActionProgramViewV1 action{};
    action.actionStableId = actionId;
    action.steps = &step;
    action.stepCount = 1;
    action.expandedStepOccurrenceCount = 1;
    action.expandedFiringCount = 1;
    action.programSemanticFingerprint =
        harken::ComputeActionProgramFingerprintV1(action);

    harken::ContainerWorldInstallViewV1 world{};
    world.variables = &variable;
    world.variableCount = 1;
    world.actions = &action;
    world.actionCount = 1;
    world.capacity.maxActionDefinitions = 1;
    world.capacity.maxActionStepsPerDefinition = 1;
    world.capacity.maxActionStepsTotal = 1;
    world.capacity.maxActionExpandedWork = 1;
    world.identity.capabilityMask =
        harken::kWorldCapabilityVariableDefinitions |
        harken::kWorldCapabilityActionDefinitions;
    world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(world);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harken::InstallContainerWorldV1(*engine, world).status);
}

} // namespace

TEST(TraceV4, VariableWorldProbeCallsAndOutcomesUsePinnedRecords) {
    const std::string path = TracePath("unit1_variable_records.vtrc");
    std::filesystem::remove(path);
    VnConfig config = MakeNullConfig(4, kBlockFrames, 48000);
    config.maxEmitters = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const harken::VariableDefinitionV1 definitions[] = {
        harkentest::MakeVariableDefinition(harkentest::ObjectId(8101), harken::VariableKindV1::Step,
                                         0.0f, 4.0f, 0.0f),
        harkentest::MakeVariableDefinition(harkentest::ObjectId(8102), harken::VariableKindV1::Smooth,
                                         -24.0f, 0.0f, 0.0f)};
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallDefinitionsWorld(engine, definitions, 2).status);
    std::uint32_t probeGeneration = 0;
    ASSERT_EQ(VN_OK, harken::InstallMeterProbe(
                         *engine, harkentest::ObjectId(8201), VN_BUS_MASTER,
                         VN_BUS_METER_TAP_POST_EFFECTS, 5, 50,
                         &probeGeneration));
    const harken::CurvePoint points[2] = {{-96.0f, 0.0f}, {0.0f, -6.0f}};
    const harken::CurveHandlePair handles[1] = {
        {-96.0f, 0.0f, 0.0f, -6.0f}};
    ASSERT_EQ(VN_OK, harken::DefineMeterProbeBinding(
                         *engine, harkentest::ObjectId(8202), harkentest::ObjectId(8201), probeGeneration,
                         VN_TARGET_SCOPE_BUS, 0, points, handles, 2, 0));

    ASSERT_EQ(VN_OK, Vn_TraceStart(path.c_str()));
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(8101), 3.0f));
    harkentest::AdvanceVariableWrite(engine);
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, harkentest::ObjectId(8102), -6.0f));
    harkentest::AdvanceVariableWrite(engine);
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(8102), 0.0f));
    harkentest::AdvanceVariableWrite(engine);
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(8102), 0.0f));
    harkentest::AdvanceVariableWrite(engine);
    ASSERT_EQ(VN_OK, Vn_TraceStop());

    const std::vector<JournalRecord> records = ReadJournalRecords(path);
    ASSERT_FALSE(records.empty());
    std::uint32_t worldRecords = 0;
    std::uint32_t probeInstallRecords = 0;
    std::uint32_t probeDefineRecords = 0;
    std::uint32_t outcomeRecords = 0;
    std::uint32_t globalSetCalls = 0;
    std::uint32_t emitterSetCalls = 0;
    for (const JournalRecord& record : records) {
        if (record.type == harken::kTraceRecordCurrentWorldInstall) {
            ASSERT_GE(record.payload.size(), 32u);
            const auto readU32 = [&](std::size_t offset) {
                std::uint32_t value = 0;
                std::memcpy(&value, record.payload.data() + offset,
                            sizeof(value));
                return value;
            };
            const auto readU64 = [&](std::size_t offset) {
                std::uint64_t value = 0;
                std::memcpy(&value, record.payload.data() + offset,
                            sizeof(value));
                return value;
            };
            EXPECT_EQ(harken::kCurrentWorldShippingBodyVintage, readU32(0));
            EXPECT_EQ(0u, readU32(4));
            EXPECT_EQ(static_cast<std::uint32_t>(
                          harken::WorldInstallStatus::Ok),
                      readU32(8));
            EXPECT_EQ(0u, readU32(12));
            EXPECT_EQ(0u, readU64(16));
            EXPECT_EQ(engine->variables.CurrentWorldGeneration(),
                      readU64(24));
            harken::TraceCurrentWorldImage image;
            ASSERT_TRUE(image.Decode(
                record.payload.data() + 32, record.payload.size() - 32,
                harken::kCurrentWorldShippingBodyVintage,
                harken::CurrentWorldDecodeMode::StrictArtifact));
            EXPECT_EQ(2u, image.ProductionView()->variableCount);
            EXPECT_EQ(0u, image.ProductionView()->playablePropertyCount);
            ++worldRecords;
        } else if (record.type == harken::kTraceRecordMeterProbeInstall) {
            ++probeInstallRecords;
        } else if (record.type ==
                   harken::kTraceRecordMeterProbeBindingDefine) {
            ++probeDefineRecords;
        } else if (record.type == harken::kTraceRecordVariableOutcome) {
            EXPECT_EQ(80u, record.payload.size());
            ++outcomeRecords;
        } else if (PayloadCallId(record) == harken::kTraceCallSetValue) {
            EXPECT_EQ(72u, record.payload.size());
            ++globalSetCalls;
        } else if (PayloadCallId(record) ==
                   harken::kTraceCallSetEmitterValue) {
            EXPECT_EQ(84u, record.payload.size());
            ++emitterSetCalls;
        }
        EXPECT_NE(harken::kTraceCallClearEmitterValue, PayloadCallId(record));
        EXPECT_NE(harken::kTraceCallClearEmitterValues, PayloadCallId(record));
    }
    EXPECT_EQ(1u, worldRecords);
    EXPECT_EQ(1u, probeInstallRecords);
    EXPECT_EQ(1u, probeDefineRecords);
    EXPECT_GE(outcomeRecords, 4u);
    EXPECT_EQ(3u, globalSetCalls);
    EXPECT_EQ(1u, emitterSetCalls);
}

TEST(TraceV5, PostActionRecordsPinnedFieldWisePayload) {
    constexpr VnObjectId kActionId = harkentest::ObjectId(0xA099u);
    constexpr std::uint64_t kCallerSeed = UINT64_C(0x1020304050607080);
    constexpr std::uint16_t kPostActionCallId = 99;

    EngineSession session(MakeNullConfig(4, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_NO_FATAL_FAILURE(
        InstallImmediateTraceActionWorld(session.Engine(), kActionId));

    const std::string path = TracePath("post_action_call_99.vtrc");
    std::filesystem::remove(path);
    ASSERT_EQ(VN_OK, Vn_TraceStart(path.c_str()));

    VnPostActionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.actionId = harkentest::ObjectId(kActionId);
    desc.callerSeed = kCallerSeed;
    VnActionFiringHandle firing = 0;
    ASSERT_EQ(VN_OK, Vn_PostAction(&desc, &firing));
    ASSERT_NE(0u, firing);
    ASSERT_EQ(VN_OK, Vn_TraceStop());

    const std::vector<JournalRecord> records = ReadJournalRecords(path);
    const auto found = std::find_if(
        records.begin(), records.end(), [](const JournalRecord& record) {
            return PayloadCallId(record) == kPostActionCallId;
        });
    ASSERT_NE(records.end(), found);
    ASSERT_EQ(84u + harken::kTracePostActionPairedTailBytes, found->payload.size());

    std::size_t offset = 0;
    const auto readU16 = [&](std::uint16_t* value) {
        ASSERT_LE(offset + sizeof(*value), found->payload.size());
        std::memcpy(value, found->payload.data() + offset, sizeof(*value));
        offset += sizeof(*value);
    };
    const auto readU32 = [&](std::uint32_t* value) {
        ASSERT_LE(offset + sizeof(*value), found->payload.size());
        std::memcpy(value, found->payload.data() + offset, sizeof(*value));
        offset += sizeof(*value);
    };
    const auto readU64 = [&](std::uint64_t* value) {
        ASSERT_LE(offset + sizeof(*value), found->payload.size());
        std::memcpy(value, found->payload.data() + offset, sizeof(*value));
        offset += sizeof(*value);
    };
    const auto readObjectId = [&](VnObjectId* value) {
        ASSERT_LE(offset + 16, found->payload.size());
        *value = harken::ObjectIdFromBytes(std::span<const std::uint8_t, 16>(found->payload.data() + offset, 16));
        offset += 16;
    };

    std::uint16_t callId = 0;
    std::uint16_t reserved = 0;
    std::uint32_t schema = 0;
    std::uint32_t structSize = 0;
    std::uint32_t flags = UINT32_MAX;
    VnObjectId actionId = harkentest::ObjectId(0);
    std::uint64_t emitter = UINT64_MAX;
    std::uint64_t callerSeed = 0;
    std::uint64_t reserved0 = UINT64_MAX;
    std::uint32_t result = UINT32_MAX;
    std::uint64_t recordedFiring = 0;
    std::uint64_t recordedTicket = 0;
    std::uint64_t recordedWorld = 0;
    readU16(&callId);
    readU16(&reserved);
    readU32(&schema);
    readU32(&structSize);
    readU32(&flags);
    readObjectId(&actionId);
    readU64(&emitter);
    readU64(&callerSeed);
    readU64(&reserved0);
    readU32(&result);
    readU64(&recordedFiring);
    readU64(&recordedTicket);
    readU64(&recordedWorld);

    EXPECT_EQ(kPostActionCallId, callId);
    EXPECT_EQ(0u, reserved);
    EXPECT_EQ(2u, schema);
    EXPECT_EQ(sizeof(VnPostActionDesc), structSize);
    EXPECT_EQ(desc.flags, flags);
    EXPECT_EQ(desc.actionId, actionId);
    EXPECT_EQ(desc.emitter, emitter);
    EXPECT_EQ(desc.callerSeed, callerSeed);
    EXPECT_EQ(desc.reserved0, reserved0);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_OK), result);
    EXPECT_EQ(firing, recordedFiring);
    EXPECT_EQ(session.Engine()->actionCommitBundle.Sequence(), recordedTicket);
    EXPECT_EQ(session.Engine()->variables.CurrentWorldGeneration(), recordedWorld);
    EXPECT_EQ(84u, offset);

    std::uint64_t projectSessionId = UINT64_MAX;
    std::uint64_t runtimeEpoch = UINT64_MAX;
    std::uint64_t authoringRevision = UINT64_MAX;
    std::uint64_t submissionSerial = UINT64_MAX;
    std::uint64_t publishedBusFingerprint = UINT64_MAX;
    std::uint64_t compileTargetProfileFingerprint = UINT64_MAX;
    std::uint32_t admission = UINT32_MAX;
    readU64(&projectSessionId);
    readU64(&runtimeEpoch);
    readU64(&authoringRevision);
    readU64(&submissionSerial);
    readU64(&publishedBusFingerprint);
    readU64(&compileTargetProfileFingerprint);
    readU32(&admission);
    EXPECT_EQ(0u, projectSessionId);
    EXPECT_EQ(0u, runtimeEpoch);
    EXPECT_EQ(0u, authoringRevision);
    EXPECT_EQ(0u, submissionSerial);
    EXPECT_EQ(0u, publishedBusFingerprint);
    EXPECT_EQ(0u, compileTargetProfileFingerprint);
    EXPECT_EQ(3u, admission);
    EXPECT_EQ(found->payload.size(), offset);

    EXPECT_EQ(VN_OK, Vn_ReleaseActionFiring(firing));
}

TEST(TraceV5, StopActionFiringRecordsPinnedFieldWisePayload) {
    constexpr VnObjectId kActionId = harkentest::ObjectId(0xA100u);
    constexpr std::uint16_t kStopActionFiringCallId = 100;
    constexpr std::uint32_t kFadeMs = 37;

    EngineSession session(MakeNullConfig(4, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_NO_FATAL_FAILURE(
        InstallImmediateTraceActionWorld(session.Engine(), kActionId));

    const std::string path = TracePath("stop_action_firing_call_100.vtrc");
    std::filesystem::remove(path);
    ASSERT_EQ(VN_OK, Vn_TraceStart(path.c_str()));
    VnPostActionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.actionId = harkentest::ObjectId(kActionId);
    VnActionFiringHandle firing = 0;
    ASSERT_EQ(VN_OK, Vn_PostAction(&desc, &firing));
    ASSERT_NE(0u, firing);
    ASSERT_EQ(VN_OK, Vn_StopActionFiring(firing, kFadeMs));
    ASSERT_EQ(VN_OK, Vn_TraceStop());

    const std::vector<JournalRecord> records = ReadJournalRecords(path);
    const auto found = std::find_if(
        records.begin(), records.end(), [](const JournalRecord& record) {
            return PayloadCallId(record) == kStopActionFiringCallId;
        });
    ASSERT_NE(records.end(), found);
    ASSERT_EQ(24u, found->payload.size());

    std::uint16_t reserved = UINT16_MAX;
    std::uint32_t schema = 0;
    std::uint64_t recordedFiring = 0;
    std::uint32_t fadeMs = 0;
    std::uint32_t result = UINT32_MAX;
    std::memcpy(&reserved, found->payload.data() + 2, sizeof reserved);
    std::memcpy(&schema, found->payload.data() + 4, sizeof schema);
    std::memcpy(&recordedFiring, found->payload.data() + 8,
                sizeof recordedFiring);
    std::memcpy(&fadeMs, found->payload.data() + 16, sizeof fadeMs);
    std::memcpy(&result, found->payload.data() + 20, sizeof result);
    EXPECT_EQ(0u, reserved);
    EXPECT_EQ(1u, schema);
    EXPECT_EQ(firing, recordedFiring);
    EXPECT_EQ(kFadeMs, fadeMs);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_OK), result);

    EXPECT_EQ(VN_OK, Vn_ReleaseActionFiring(firing));
}

TEST(TraceV5, QueryActionFiringRecordsCompleteFieldWiseOutput) {
    constexpr VnObjectId kActionId = harkentest::ObjectId(0xA101u);
    constexpr std::uint16_t kQueryActionFiringCallId = 101;

    EngineSession session(MakeNullConfig(4, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_NO_FATAL_FAILURE(
        InstallImmediateTraceActionWorld(session.Engine(), kActionId));

    const std::string path = TracePath("query_action_firing_call_101.vtrc");
    std::filesystem::remove(path);
    ASSERT_EQ(VN_OK, Vn_TraceStart(path.c_str()));
    VnPostActionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.actionId = harkentest::ObjectId(kActionId);
    VnActionFiringHandle firing = 0;
    ASSERT_EQ(VN_OK, Vn_PostAction(&desc, &firing));
    VnActionFiringInfo info{};
    info.structSize = sizeof(info);
    ASSERT_EQ(VN_OK, Vn_QueryActionFiring(firing, &info));
    ASSERT_EQ(VN_OK, Vn_TraceStop());

    const std::vector<JournalRecord> records = ReadJournalRecords(path);
    const auto found = std::find_if(
        records.begin(), records.end(), [](const JournalRecord& record) {
            return PayloadCallId(record) == kQueryActionFiringCallId;
        });
    ASSERT_NE(records.end(), found);
    ASSERT_EQ(96u, found->payload.size());

    std::size_t offset = 4;
    const auto readU32 = [&](std::uint32_t* value) {
        ASSERT_LE(offset + sizeof(*value), found->payload.size());
        std::memcpy(value, found->payload.data() + offset, sizeof(*value));
        offset += sizeof(*value);
    };
    const auto readU64 = [&](std::uint64_t* value) {
        ASSERT_LE(offset + sizeof(*value), found->payload.size());
        std::memcpy(value, found->payload.data() + offset, sizeof(*value));
        offset += sizeof(*value);
    };
    const auto readObjectId = [&](VnObjectId* value) {
        ASSERT_LE(offset + 16, found->payload.size());
        *value = harken::ObjectIdFromBytes(std::span<const std::uint8_t, 16>(found->payload.data() + offset, 16));
        offset += 16;
    };

    std::uint32_t schema = 0;
    std::uint64_t recordedFiring = 0;
    std::uint32_t outputStructSize = 0;
    std::uint32_t result = UINT32_MAX;
    VnActionFiringInfo recorded{};
    readU32(&schema);
    readU64(&recordedFiring);
    readU32(&outputStructSize);
    readU32(&result);
    readU32(&recorded.structSize);
    readU32(&recorded.state);
    readU32(&recorded.outcome);
    readU32(&recorded.terminalReason);
    readObjectId(&recorded.actionId);
    readU64(&recorded.worldGeneration);
    readU64(&recorded.acceptedLogicSequence);
    readU64(&recorded.parentFiring);
    readU32(&recorded.directStepCount);
    readU32(&recorded.directStepTerminalCount);
    readU32(&recorded.childFiringCount);
    readU32(&recorded.livePlayableCount);

    EXPECT_EQ(1u, schema);
    EXPECT_EQ(firing, recordedFiring);
    EXPECT_EQ(sizeof(VnActionFiringInfo), outputStructSize);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_OK), result);
    EXPECT_EQ(0, std::memcmp(&info, &recorded, sizeof(info)));
    EXPECT_EQ(found->payload.size(), offset);

    EXPECT_EQ(VN_OK, Vn_ReleaseActionFiring(firing));
}

TEST(TraceV5, QueryActionStepRecordsCompleteFieldWiseOutput) {
    constexpr VnObjectId kActionId = harkentest::ObjectId(0xA102u);
    constexpr std::uint16_t kQueryActionStepCallId = 102;

    EngineSession session(MakeNullConfig(4, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_NO_FATAL_FAILURE(
        InstallImmediateTraceActionWorld(session.Engine(), kActionId));

    const std::string path = TracePath("query_action_step_call_102.vtrc");
    std::filesystem::remove(path);
    ASSERT_EQ(VN_OK, Vn_TraceStart(path.c_str()));
    VnPostActionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.actionId = harkentest::ObjectId(kActionId);
    VnActionFiringHandle firing = 0;
    ASSERT_EQ(VN_OK, Vn_PostAction(&desc, &firing));
    VnActionStepResult step{};
    step.structSize = sizeof(step);
    ASSERT_EQ(VN_OK, Vn_QueryActionStep(firing, 0, &step));
    ASSERT_EQ(VN_OK, Vn_TraceStop());

    const std::vector<JournalRecord> records = ReadJournalRecords(path);
    const auto found = std::find_if(
        records.begin(), records.end(), [](const JournalRecord& record) {
            return PayloadCallId(record) == kQueryActionStepCallId;
        });
    ASSERT_NE(records.end(), found);
    ASSERT_EQ(124u, found->payload.size());

    std::size_t offset = 4;
    const auto readU32 = [&](std::uint32_t* value) {
        ASSERT_LE(offset + sizeof(*value), found->payload.size());
        std::memcpy(value, found->payload.data() + offset, sizeof(*value));
        offset += sizeof(*value);
    };
    const auto readU64 = [&](std::uint64_t* value) {
        ASSERT_LE(offset + sizeof(*value), found->payload.size());
        std::memcpy(value, found->payload.data() + offset, sizeof(*value));
        offset += sizeof(*value);
    };
    const auto readObjectId = [&](VnObjectId* value) {
        ASSERT_LE(offset + 16, found->payload.size());
        *value = harken::ObjectIdFromBytes(std::span<const std::uint8_t, 16>(found->payload.data() + offset, 16));
        offset += 16;
    };

    std::uint32_t schema = 0;
    std::uint64_t recordedFiring = 0;
    std::uint32_t authoredEventIndex = UINT32_MAX;
    std::uint32_t outputStructSize = 0;
    std::uint32_t result = UINT32_MAX;
    std::uint32_t stepResultBits = UINT32_MAX;
    VnActionStepResult recorded{};
    readU32(&schema);
    readU64(&recordedFiring);
    readU32(&authoredEventIndex);
    readU32(&outputStructSize);
    readU32(&result);
    readU32(&recorded.structSize);
    readU32(&recorded.state);
    readU32(&stepResultBits);
    recorded.result = static_cast<VnResult>(
        static_cast<std::int32_t>(stepResultBits));
    readU32(&recorded.opcode);
    readObjectId(&recorded.ownerActionId);
    readObjectId(&recorded.targetStableId);
    readU64(&recorded.logicCommitSequence);
    readU64(&recorded.childFiring);
    readU64(&recorded.producedPlayableInstance);
    readU32(&recorded.authoredEventIndex);
    readU32(&recorded.executionOrdinal);
    readU32(&recorded.depth);
    readU32(&recorded.targetKind);
    readU32(&recorded.capturedPlayableCount);
    readU32(&recorded.remainingPlayableCount);

    EXPECT_EQ(1u, schema);
    EXPECT_EQ(firing, recordedFiring);
    EXPECT_EQ(0u, authoredEventIndex);
    EXPECT_EQ(sizeof(VnActionStepResult), outputStructSize);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_OK), result);
    EXPECT_EQ(0, std::memcmp(&step, &recorded, sizeof(step)));
    EXPECT_EQ(found->payload.size(), offset);

    EXPECT_EQ(VN_OK, Vn_ReleaseActionFiring(firing));
}

TEST(TraceV5, ReleaseActionFiringRecordsPinnedFieldWisePayload) {
    constexpr VnObjectId kActionId = harkentest::ObjectId(0xA103u);
    constexpr std::uint16_t kReleaseActionFiringCallId = 103;

    EngineSession session(MakeNullConfig(4, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_NO_FATAL_FAILURE(
        InstallImmediateTraceActionWorld(session.Engine(), kActionId));

    const std::string path = TracePath("release_action_firing_call_103.vtrc");
    std::filesystem::remove(path);
    ASSERT_EQ(VN_OK, Vn_TraceStart(path.c_str()));
    VnPostActionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.actionId = harkentest::ObjectId(kActionId);
    VnActionFiringHandle firing = 0;
    ASSERT_EQ(VN_OK, Vn_PostAction(&desc, &firing));
    ASSERT_EQ(VN_OK, Vn_ReleaseActionFiring(firing));
    ASSERT_EQ(VN_OK, Vn_TraceStop());

    const std::vector<JournalRecord> records = ReadJournalRecords(path);
    const auto found = std::find_if(
        records.begin(), records.end(), [](const JournalRecord& record) {
            return PayloadCallId(record) == kReleaseActionFiringCallId;
        });
    ASSERT_NE(records.end(), found);
    ASSERT_EQ(20u, found->payload.size());

    std::uint32_t schema = 0;
    std::uint64_t recordedFiring = 0;
    std::uint32_t result = UINT32_MAX;
    std::memcpy(&schema, found->payload.data() + 4, sizeof schema);
    std::memcpy(&recordedFiring, found->payload.data() + 8,
                sizeof recordedFiring);
    std::memcpy(&result, found->payload.data() + 16, sizeof result);
    EXPECT_EQ(1u, schema);
    EXPECT_EQ(firing, recordedFiring);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_OK), result);
}

TEST(TraceV5, UnloadPackExRecordsHandleAndFlags) {
    EngineSession session(MakeNullConfig(4, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::string path = TracePath("unload_pack_ex_call_104.vtrc");
    std::filesystem::remove(path);
    ASSERT_EQ(VN_OK, Vn_TraceStart(path.c_str()));
    EXPECT_EQ(VN_STALE_HANDLE,
              Vn_UnloadPackEx(UINT64_MAX, VN_PACK_UNLOAD_REFUSE_IF_USED));
    ASSERT_EQ(VN_OK, Vn_TraceStop());

    const std::vector<JournalRecord> records = ReadJournalRecords(path);
    const auto found = std::find_if(
        records.begin(), records.end(), [](const JournalRecord& record) {
            return PayloadCallId(record) == harken::kTraceCallUnloadPackEx;
        });
    ASSERT_NE(records.end(), found);
    ASSERT_EQ(16u, found->payload.size());
    std::uint64_t recordedHandle = 0;
    std::uint32_t recordedFlags = 0;
    std::memcpy(&recordedHandle, found->payload.data() + 4,
                sizeof recordedHandle);
    std::memcpy(&recordedFlags, found->payload.data() + 12,
                sizeof recordedFlags);
    EXPECT_EQ(UINT64_MAX, recordedHandle);
    EXPECT_EQ(VN_PACK_UNLOAD_REFUSE_IF_USED, recordedFlags);
}

TEST(TraceV5, PostActionFailureRecordsNonOkResultAndZeroFiring) {
    constexpr VnObjectId kActionId = harkentest::ObjectId(0xA104u);

    EngineSession session(MakeNullConfig(4, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_NO_FATAL_FAILURE(
        InstallImmediateTraceActionWorld(session.Engine(), kActionId));

    const std::string path = TracePath("post_action_refusal_call_99.vtrc");
    std::filesystem::remove(path);
    ASSERT_EQ(VN_OK, Vn_TraceStart(path.c_str()));

    VnPostActionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.actionId = harkentest::OffsetObjectId(kActionId, 1);
    VnActionFiringHandle firing = UINT64_MAX;
    ASSERT_EQ(VN_UNKNOWN_ID, Vn_PostAction(&desc, &firing));
    EXPECT_EQ(0u, firing);
    ASSERT_EQ(VN_OK, Vn_TraceStop());

    const std::vector<JournalRecord> records = ReadJournalRecords(path);
    const auto found = std::find_if(
        records.begin(), records.end(), [](const JournalRecord& record) {
            return PayloadCallId(record) == harken::kTraceCallPostAction;
        });
    ASSERT_NE(records.end(), found);
    ASSERT_EQ(68u + harken::kTracePostActionPairedTailBytes, found->payload.size());
    std::uint32_t result = 0;
    std::uint64_t recordedFiring = UINT64_MAX;
    std::memcpy(&result, found->payload.data() + 56, sizeof result);
    std::memcpy(&recordedFiring, found->payload.data() + 60,
                sizeof recordedFiring);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_UNKNOWN_ID), result);
    EXPECT_EQ(0u, recordedFiring);
}

TEST(TraceV5, InvalidActionFiringArgumentShapesAreNotRecorded) {
    constexpr VnObjectId kActionId = harkentest::ObjectId(0xA105u);

    EngineSession session(MakeNullConfig(4, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_NO_FATAL_FAILURE(
        InstallImmediateTraceActionWorld(session.Engine(), kActionId));

    const std::string path = TracePath("action_firing_invalid_shapes.vtrc");
    std::filesystem::remove(path);
    ASSERT_EQ(VN_OK, Vn_TraceStart(path.c_str()));

    VnPostActionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.actionId = harkentest::ObjectId(kActionId);
    VnActionFiringHandle firing = 0;
    EXPECT_EQ(VN_INVALID_ARG, Vn_PostAction(nullptr, &firing));
    EXPECT_EQ(VN_INVALID_ARG, Vn_PostAction(&desc, nullptr));
    desc.structSize = sizeof(desc) - 1;
    EXPECT_EQ(VN_INVALID_ARG, Vn_PostAction(&desc, &firing));

    VnActionFiringInfo info{};
    info.structSize = sizeof(info) - 1;
    EXPECT_EQ(VN_INVALID_ARG, Vn_QueryActionFiring(1, &info));
    EXPECT_EQ(VN_INVALID_ARG, Vn_QueryActionFiring(1, nullptr));
    VnActionStepResult step{};
    step.structSize = sizeof(step) - 1;
    EXPECT_EQ(VN_INVALID_ARG, Vn_QueryActionStep(1, 0, &step));
    EXPECT_EQ(VN_INVALID_ARG, Vn_QueryActionStep(1, 0, nullptr));
    ASSERT_EQ(VN_OK, Vn_TraceStop());

    const std::vector<JournalRecord> records = ReadJournalRecords(path);
    const std::size_t actionCallCount = std::count_if(
        records.begin(), records.end(), [](const JournalRecord& record) {
            const std::uint16_t callId = PayloadCallId(record);
            return callId >= harken::kTraceCallPostAction &&
                   callId <= harken::kTraceCallReleaseActionFiring;
        });
    EXPECT_EQ(0u, actionCallCount);
}

TEST(TraceV5, FailedActionQueriesRecordCompleteZeroOutputImages) {
    constexpr VnObjectId kActionId = harkentest::ObjectId(0xA106u);

    EngineSession session(MakeNullConfig(4, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_NO_FATAL_FAILURE(
        InstallImmediateTraceActionWorld(session.Engine(), kActionId));

    const std::string path = TracePath("action_firing_zero_outputs.vtrc");
    std::filesystem::remove(path);
    ASSERT_EQ(VN_OK, Vn_TraceStart(path.c_str()));

    VnPostActionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.actionId = harkentest::ObjectId(kActionId);
    VnActionFiringHandle firing = 0;
    ASSERT_EQ(VN_OK, Vn_PostAction(&desc, &firing));
    ASSERT_NE(0u, firing);

    VnActionFiringInfo info;
    std::memset(&info, 0xA5, sizeof info);
    info.structSize = sizeof(info);
    ASSERT_EQ(VN_STALE_HANDLE,
              Vn_QueryActionFiring(UINT64_MAX, &info));
    VnActionFiringInfo expectedInfo{};
    expectedInfo.structSize = sizeof(expectedInfo);
    EXPECT_EQ(0, std::memcmp(&expectedInfo, &info, sizeof info));

    VnActionStepResult step;
    std::memset(&step, 0xA5, sizeof step);
    step.structSize = sizeof(step);
    ASSERT_EQ(VN_INVALID_ARG,
              Vn_QueryActionStep(firing, 9, &step));
    VnActionStepResult expectedStep{};
    expectedStep.structSize = sizeof(expectedStep);
    EXPECT_EQ(0, std::memcmp(&expectedStep, &step, sizeof step));
    ASSERT_EQ(VN_OK, Vn_TraceStop());

    const std::vector<JournalRecord> records = ReadJournalRecords(path);
    const auto queryFiring = std::find_if(
        records.begin(), records.end(), [](const JournalRecord& record) {
            return PayloadCallId(record) ==
                   harken::kTraceCallQueryActionFiring;
        });
    ASSERT_NE(records.end(), queryFiring);
    ASSERT_EQ(96u, queryFiring->payload.size());
    std::uint32_t queryFiringResult = 0;
    std::uint32_t infoStructSize = 0;
    std::memcpy(&queryFiringResult, queryFiring->payload.data() + 20,
                sizeof queryFiringResult);
    std::memcpy(&infoStructSize, queryFiring->payload.data() + 24,
                sizeof infoStructSize);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_STALE_HANDLE),
              queryFiringResult);
    EXPECT_EQ(sizeof(VnActionFiringInfo), infoStructSize);
    for (std::size_t index = 28; index < queryFiring->payload.size();
         ++index) {
        EXPECT_EQ(0u, queryFiring->payload[index]);
    }

    const auto queryStep = std::find_if(
        records.begin(), records.end(), [](const JournalRecord& record) {
            return PayloadCallId(record) ==
                   harken::kTraceCallQueryActionStep;
        });
    ASSERT_NE(records.end(), queryStep);
    ASSERT_EQ(124u, queryStep->payload.size());
    std::uint32_t queryStepResult = 0;
    std::uint32_t stepStructSize = 0;
    std::memcpy(&queryStepResult, queryStep->payload.data() + 24,
                sizeof queryStepResult);
    std::memcpy(&stepStructSize, queryStep->payload.data() + 28,
                sizeof stepStructSize);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_INVALID_ARG),
              queryStepResult);
    EXPECT_EQ(sizeof(VnActionStepResult), stepStructSize);
    for (std::size_t index = 32; index < queryStep->payload.size();
         ++index) {
        EXPECT_EQ(0u, queryStep->payload[index]);
    }

    EXPECT_EQ(VN_OK, Vn_ReleaseActionFiring(firing));
}

TEST(TraceV4, OutcomesProducedOutsideUpdateReachTheJournal) {
    const std::string path = TracePath("unit1_outside_update_outcomes.vtrc");
    std::filesystem::remove(path);
    VnConfig config = MakeNullConfig(4, kBlockFrames, 48000);
    config.maxEmitters = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    harken::VariableDefinitionV1 definitions[] = {
        harkentest::MakeVariableDefinition(
            harkentest::ObjectId(8301), harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.0f),
        harkentest::MakeVariableDefinition(
            harkentest::ObjectId(8302), harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.0f)};
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallDefinitionsWorld(engine, definitions, 2).status);
    ASSERT_EQ(VN_OK, Vn_TraceStart(path.c_str()));

    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(8301), 0.25f));
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(8301), 0.75f));
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, harkentest::ObjectId(8302), 0.5f));
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(emitter));
    definitions[1].rangeMax = 2.0f;
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallDefinitionsWorld(
                  engine, definitions, 2, 0x8302u).status);
    ASSERT_EQ(VN_OK, Vn_Update());

    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(8301), 0.5f));
    engine->variables.TerminateSession();
    ASSERT_EQ(VN_OK, Vn_TraceStop());

    const std::vector<JournalRecord> records = ReadJournalRecords(path);
    std::uint32_t superseded = 0;
    std::uint32_t emitterDestroyed = 0;
    std::uint32_t worldReplacement = 0;
    std::uint32_t sessionOutcomes = 0;
    for (const JournalRecord& record : records) {
        if (record.type != harken::kTraceRecordVariableOutcome ||
            record.payload.size() < sizeof(std::uint32_t)) {
            continue;
        }
        std::uint32_t kind = 0;
        std::memcpy(&kind, record.payload.data() + 4, sizeof(kind));
        superseded += kind == static_cast<std::uint32_t>(
                                   harken::VariableOutcomeKind::Superseded);
        emitterDestroyed += kind == static_cast<std::uint32_t>(
                                      harken::VariableOutcomeKind::EmitterDestroyed);
        worldReplacement += kind == static_cast<std::uint32_t>(
                                      harken::VariableOutcomeKind::WorldReplacement);
        sessionOutcomes += kind == static_cast<std::uint32_t>(
                              harken::VariableOutcomeKind::Session);
    }
    EXPECT_GE(superseded, 1u);
    EXPECT_GE(emitterDestroyed, 1u);
    EXPECT_GE(worldReplacement, 1u);
    EXPECT_GE(sessionOutcomes, 1u);
}

TEST(TraceV4, RefusedArgumentsNeverReadUnboundedCallerArrays) {
    const std::string path = TracePath("unit1_refused_attempts.vtrc");
    std::filesystem::remove(path);
    EngineSession session(MakeNullConfig(4, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(VN_OK, Vn_TraceStart(path.c_str()));

    const harken::VariableDefinitionV1 one =
        harkentest::MakeVariableDefinition(
            harkentest::ObjectId(8401), harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.0f);
    harken::ContainerWorldInstallViewV1 oversized{};
    oversized.variables = &one;
    oversized.variableCount = UINT32_MAX;
    EXPECT_EQ(harken::WorldInstallStatus::Capacity,
              harken::InstallContainerWorldV1(*engine, oversized).status);

    std::uint32_t probeGeneration = 0;
    ASSERT_EQ(VN_OK, harken::InstallMeterProbe(
                         *engine, harkentest::ObjectId(8403), VN_BUS_MASTER,
                         VN_BUS_METER_TAP_POST_EFFECTS, 0, 0,
                         &probeGeneration));
    const harken::CurvePoint point = {-96.0f, 0.0f};
    EXPECT_EQ(VN_INVALID_ARG,
              harken::DefineMeterProbeBinding(
                  *engine, harkentest::ObjectId(8402), harkentest::ObjectId(8403), probeGeneration,
                  VN_TARGET_SCOPE_BUS, 0, &point, nullptr, UINT32_MAX, 0));
    EXPECT_EQ(VN_STALE_HANDLE,
              harken::DefineMeterProbeBinding(
                  *engine, harkentest::ObjectId(8404), harkentest::ObjectId(8403), probeGeneration + 1,
                  VN_TARGET_SCOPE_BUS, 0, &point, nullptr, 1, 0));
    ASSERT_EQ(VN_OK, Vn_TraceStop());

    const std::vector<JournalRecord> records = ReadJournalRecords(path);
    std::uint32_t worldAttempts = 0;
    std::uint32_t probeAttempts = 0;
    for (const JournalRecord& record : records) {
        worldAttempts +=
            record.type == harken::kTraceRecordCurrentWorldInstall;
        probeAttempts +=
            record.type == harken::kTraceRecordMeterProbeBindingDefine;
    }
    EXPECT_EQ(1u, worldAttempts);
    EXPECT_EQ(2u, probeAttempts);
}

TEST(TraceCodec, ConfigVintageIsSemanticRatherThanByteSized) {
    for (std::uint32_t vintage = 1;
         vintage <= harken::kTraceConfigVintageCount; ++vintage) {
        const std::uint32_t nativeSize =
            harken::NativeConfigSizeForVintage(vintage);
        EXPECT_NE(0u, nativeSize) << "vintage " << vintage;
        EXPECT_EQ(vintage, harken::ConfigVintageFromNativeSize(nativeSize));
    }
    EXPECT_EQ(0u, harken::ConfigVintageFromNativeSize(1));
    EXPECT_EQ(0u, harken::ConfigVintageFromNativeSize(
                      static_cast<std::uint32_t>(sizeof(VnConfig) + 8)));
}

TEST(TraceCodec, CurrentWorldRoundTripsEveryUnitTwoRowFamily) {
    harken::VariableDefinitionV1 variable = harkentest::MakeVariableDefinition(
        harkentest::ObjectId(9101), harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.0f);
    const harken::CurvePoint points[2] = {{0.0f, 0.0f}, {1.0f, 1.0f}};
    const harken::CurveHandlePair handles[1] = {
        {0.0f, 0.0f, 1.0f, 1.0f}};
    harken::CurveV1 curve{};
    curve.curveLocalId = 17;
    ASSERT_TRUE(curve.curve.SetPoints(points, handles, 2));
    const harken::SendV1 send{3, 0, VN_BUS_MASTER, -6.0f, 0};
    const harken::EffectParamV1 effectParam{0, std::bit_cast<std::uint32_t>(-3.0f)};
    const harken::EffectNodeV1 effectNode{5, 1, 1, &effectParam};
    const harken::EffectEdgeV1 effectEdge{5, 5};
    harken::PlayablePropertyPayloadViewV1 payload{};
    payload.presentMask = UINT64_C(0x0000000000008088);
    payload.values[3].bits = 1000;
    payload.values[11].bits = std::bit_cast<std::uint32_t>(1.0f);
    payload.sendCount = 1;
    payload.sends = &send;
    payload.effectNodeCount = 1;
    payload.effectNodes = &effectNode;
    payload.effectEdgeCount = 1;
    payload.effectEdges = &effectEdge;
    const harken::LayerAncestorRefV1 ancestor{harkentest::ObjectId(99), 100};
    harken::PlayablePropertyProgramViewV1 property{};
    property.contentPropertyKind =
        harken::playableproperties::ContentPropertyKindV1::Sound;
    property.authoredStableOrder = 4;
    property.stableId = harkentest::ObjectId(9201);
    property.programSemanticFingerprint = 101;
    property.contentRevision = 102;
    property.routeStableId = VN_BUS_MASTER;
    property.compatibilityFingerprint = 103;
    property.organizationLayerAncestorCount = 1;
    property.organizationLayerAncestors = &ancestor;
    property.payload = payload;
    property.payload.presentMask |= harken::kResolvedSectionGroupPropertyMaskV1;
    property.payload.values[6].objectId = harkentest::ObjectId(2);

    harken::PlayableOccurrencePropertyViewV1 occurrence{};
    occurrence.parentKind =
        harken::playableproperties::ContentPropertyKindV1::Container;
    occurrence.stableOccurrenceOrdinal = 7;
    occurrence.parentStableId = harkentest::ObjectId(9301);
    occurrence.parentProgramSemanticFingerprint = 104;
    occurrence.childPlayable.kind = harken::PlayableKindV1::Sound;
    occurrence.childPlayable.stableId = property.stableId;
    occurrence.childPlayable.expectedProgramSemanticFingerprint =
        property.programSemanticFingerprint;
    occurrence.childPlayable.contentRevision = property.contentRevision;
    occurrence.childPlayable.routeStableId = property.routeStableId;
    occurrence.childPlayable.compatibilityFingerprint =
        property.compatibilityFingerprint;
    occurrence.occurrenceSemanticFingerprint = 105;
    occurrence.payload = payload;
    occurrence.payload.presentMask &= ~harken::kResolvedSectionGroupPropertyMaskV1;
    occurrence.payload.sendCount = 0;
    occurrence.payload.sends = nullptr;

    const harken::StableAuthoredPathStepV1 pathStep{
        occurrence.childPlayable, 7, 0, harkentest::ObjectId(31), harkentest::ObjectId(32), 33, 34};
    harken::AuthoredCompletionLeafV1 leaf{};
    leaf.stableLeafOrdinal = 1;
    leaf.content = occurrence.childPlayable;
    leaf.startOffset = {harken::AuthoredTimeDomainV1::SourceFrames, 0, 1};
    leaf.duration = {harken::AuthoredTimeDomainV1::SourceFrames, 64, 1};
    leaf.pathStepCount = 1;
    leaf.pathSteps = &pathStep;
    harken::ContainerChildProgramViewV2 child{};
    child.playable = occurrence.childPlayable;
    child.stableChildOrdinal = 7;
    child.authoredStableOrder = 8;
    child.frequencyWeight = 1;
    harkentest::CopyLeafTimingToHint(child, leaf);
    harken::ContainerProgramViewV3 container{};
    container.containerStableId = occurrence.parentStableId;
    container.programSemanticFingerprint =
        occurrence.parentProgramSemanticFingerprint;
    container.memoryScope = harken::MemoryScopeV1::Global;
    container.childCount = 1;
    container.children = &child;
    container.curveCount = 1;
    container.curves = &curve;

    harken::ContainerWorldInstallViewV1 world{};
    world.identity.runtimeSemanticFingerprint = 106;
    world.identity.capabilityMask =
        harken::kWorldCapabilityVariableDefinitions |
        harken::kWorldCapabilityPlayableProperties |
        harken::kWorldCapabilityRandomDraw;
    world.variables = &variable;
    world.variableCount = 1;
    world.playableProperties = &property;
    world.playablePropertyCount = 1;
    world.playableOccurrences = &occurrence;
    world.playableOccurrenceCount = 1;
    world.containers = &container;
    world.containerCount = 1;

    std::vector<std::uint8_t> encoded;
    ASSERT_TRUE(harken::EncodeTraceCurrentWorldBody(world, encoded));
    std::vector<std::uint8_t> shippingBody;
    ASSERT_TRUE(harken::EncodeCurrentWorldShippingBody(world, shippingBody));
    EXPECT_EQ(encoded, shippingBody);
    ASSERT_GE(encoded.size(), 88u);
    const auto readU32 = [&](std::size_t offset) {
        std::uint32_t value = 0;
        std::memcpy(&value, encoded.data() + offset, sizeof(value));
        return value;
    };
    EXPECT_EQ(1u, readU32(40));
    EXPECT_EQ(1u, readU32(44));
    EXPECT_EQ(1u, readU32(48));
    EXPECT_EQ(1u, readU32(52));
    EXPECT_EQ(0u, readU32(56));
    EXPECT_EQ(0u, readU32(60));
    EXPECT_EQ(1u, readU32(64));
    EXPECT_EQ(2u, readU32(68));
    EXPECT_EQ(2u, readU32(72));
    EXPECT_EQ(2u, readU32(76));
    EXPECT_EQ(0u, readU32(80));
    EXPECT_EQ(0u, readU32(84));
    EXPECT_EQ(0u, readU32(88));
    EXPECT_EQ(0u, readU32(92));
    EXPECT_EQ(0u, readU32(96));
    EXPECT_EQ(1u, readU32(100));
    EXPECT_EQ(0u, readU32(104));
    harken::TraceCurrentWorldImage decoded;
    ASSERT_TRUE(decoded.Decode(
        encoded.data(), encoded.size(),
        harken::kCurrentWorldShippingBodyVintage,
        harken::CurrentWorldDecodeMode::StrictArtifact));
    const auto& roundTrip = *decoded.ProductionView();
    ASSERT_EQ(roundTrip.playablePropertyCount, 1u);
    ASSERT_EQ(roundTrip.playableOccurrenceCount, 1u);
    ASSERT_EQ(roundTrip.containerCount, 1u);
    EXPECT_EQ(roundTrip.playableProperties[0]
                  .organizationLayerAncestors[0]
                  .layerStableId,
              ancestor.layerStableId);
    EXPECT_EQ(roundTrip.playableProperties[0].payload.values[3].bits, 1000u);
    EXPECT_EQ(roundTrip.playableOccurrences[0].payload.values[3].bits, 1000u);
    EXPECT_EQ(roundTrip.playableOccurrences[0].payload.effectNodes[0]
                  .params[0].valueBits,
              effectParam.valueBits);
    std::vector<std::uint8_t> reencoded;
    ASSERT_TRUE(harken::EncodeTraceCurrentWorldBody(roundTrip, reencoded));
    EXPECT_EQ(reencoded, encoded);
}

TEST(TraceCodec, CurrentWorldRoundTripsVariableControlGrids) {
    const harken::GridAxisV1 discreteAxis{2, 4, harkentest::ObjectId(101)};
    const harken::DiscreteGridCellV1 discreteCells[2] = {
        {harken::DiscreteCellKindV1::Exact, 17},
        {harken::DiscreteCellKindV1::Wildcard, 0}};
    const harken::DiscreteGridRowViewV1 discreteRow{
        3, 5, 7, 2, discreteCells};
    const harken::GridAxisV1 continuousAxis{6, 8, harkentest::ObjectId(102)};
    const harken::ContinuousGridCellV1 continuousCell{
        harken::ContinuousCellKindV1::Interval,
        -0.25f,
        0.75f,
        1,
        0,
        0,
        11,
        12};
    const harken::ContinuousGridRowViewV1 continuousRow{
        9, 10, 7, 1, &continuousCell};
    harken::ContainerProgramViewV3 container{};
    container.containerStableId = harkentest::ObjectId(1001);
    container.programSemanticFingerprint = 1002;
    container.mode = harken::ContainerModeV1::VariableControl;
    container.discreteAxisCount = 1;
    container.discreteAxes = &discreteAxis;
    container.discreteRowCount = 1;
    container.discreteRows = &discreteRow;
    container.continuousAxisCount = 1;
    container.continuousAxes = &continuousAxis;
    container.continuousRowCount = 1;
    container.continuousRows = &continuousRow;
    harken::ContainerWorldInstallViewV1 world{};
    world.containers = &container;
    world.containerCount = 1;

    std::vector<std::uint8_t> encoded;
    ASSERT_TRUE(harken::EncodeTraceCurrentWorldBody(world, encoded));
    harken::TraceCurrentWorldImage decoded;
    ASSERT_TRUE(decoded.Decode(
        encoded.data(), encoded.size(),
        harken::kCurrentWorldShippingBodyVintage,
        harken::CurrentWorldDecodeMode::StrictArtifact));
    const auto& roundTrip = *decoded.ProductionView();
    ASSERT_EQ(1u, roundTrip.containerCount);
    const auto& decodedContainer = roundTrip.containers[0];
    ASSERT_EQ(1u, decodedContainer.discreteAxisCount);
    ASSERT_NE(nullptr, decodedContainer.discreteAxes);
    EXPECT_EQ(discreteAxis.variableId,
              decodedContainer.discreteAxes[0].variableId);
    ASSERT_EQ(1u, decodedContainer.discreteRowCount);
    ASSERT_EQ(2u, decodedContainer.discreteRows[0].cellCount);
    EXPECT_EQ(discreteCells[0].exactValue,
              decodedContainer.discreteRows[0].cells[0].exactValue);
    ASSERT_EQ(1u, decodedContainer.continuousAxisCount);
    ASSERT_EQ(1u, decodedContainer.continuousRowCount);
    EXPECT_EQ(continuousCell.lower,
              decodedContainer.continuousRows[0].cells[0].lower);
    EXPECT_EQ(continuousCell.rightCurveLocalId,
              decodedContainer.continuousRows[0]
                  .cells[0]
                  .rightCurveLocalId);

    std::vector<std::uint8_t> reencoded;
    ASSERT_TRUE(harken::EncodeTraceCurrentWorldBody(roundTrip, reencoded));
    EXPECT_EQ(encoded, reencoded);
}

TEST(TraceCodec, CurrentWorldVintageFourOneByteLoopLayoutIsRejected) {
    harken::PlayablePropertyProgramViewV1 property{};
    property.payload.values[3].bits = 1;
    property.payload.values[11].bits = std::bit_cast<std::uint32_t>(1.0f);
    harken::ContainerWorldInstallViewV1 world{};
    world.playableProperties = &property;
    world.playablePropertyCount = 1;

    std::vector<std::uint8_t> encoded;
    ASSERT_TRUE(harken::EncodeCurrentWorldBodyV5(harkentest::AsHistoricalPrefix(world), encoded));
    ASSERT_GT(encoded.size(), 287u);
    encoded.erase(encoded.begin() + 285, encoded.begin() + 288);

    harken::TraceCurrentWorldImage decoded;
    EXPECT_FALSE(decoded.Decode(
        encoded.data(), encoded.size(), 4,
        harken::CurrentWorldDecodeMode::ReplayRawAttempt));
    EXPECT_EQ(nullptr, decoded.ReplayView());

    harken::TraceCurrentWorldImage wrongVintage;
    EXPECT_FALSE(wrongVintage.Decode(
        encoded.data(), encoded.size(), 5,
        harken::CurrentWorldDecodeMode::ReplayRawAttempt));
}

TEST(TraceCodec, HistoricalV4V5ZeroActionWorldsAreRejected) {
    harken::VariableDefinitionV1 variable{};
    variable.variableId = harkentest::ObjectId(1001);
    variable.kind = harken::VariableKindV1::Smooth;
    variable.rangeMin = -1.0f;
    variable.rangeMax = 1.0f;
    harken::ContainerWorldInstallViewV1 world{};
    world.variables = &variable;
    world.variableCount = 1;
    world.identity.capabilityMask =
        harken::kWorldCapabilityVariableDefinitions;
    world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV3(harkentest::AsHistoricalPrefix(world));

    std::vector<std::uint8_t> encoded;
    ASSERT_TRUE(harken::EncodeCurrentWorldBodyV5(harkentest::AsHistoricalPrefix(world), encoded));
    for (const std::uint32_t vintage : {4u, 5u}) {
        harken::TraceCurrentWorldImage decoded;
        EXPECT_FALSE(decoded.Decode(
            encoded.data(), encoded.size(), vintage,
            harken::CurrentWorldDecodeMode::ReplayRawAttempt));
        EXPECT_EQ(nullptr, decoded.ReplayView());
        EXPECT_EQ(nullptr, decoded.ProductionView());
    }
}

TEST(TraceCodec, CurrentWorldRoundTripsTypedTransitionsExactly) {
    harken::StableAuthoredPathStepV1 path{};
    path.playable.kind = harken::PlayableKindV1::Container;
    path.playable.stableId = harkentest::ObjectId(1001);
    path.playable.expectedProgramSemanticFingerprint = 1002;
    path.playable.contentRevision = 1003;
    path.playable.routeStableId = harkentest::ObjectId(1004);
    path.playable.compatibilityFingerprint = 1005;
    path.stableChildOrdinal = 7;
    path.trackId = harkentest::ObjectId(8);
    path.clipId = harkentest::ObjectId(9);
    path.clipGuidHigh = 10;
    path.clipGuidLow = 11;

    harken::TransitionRuleViewV1 rule{};
    rule.stableRuleOrdinal = 3;
    rule.sourceAny = 1;
    rule.targetAny = 1;
    rule.sourceExit = harken::TransitionSourceExitV1::NextBeat;
    rule.targetEntry = harken::TransitionTargetEntryV1::FromEntryMarker;
    rule.sourceFade.active = 1;
    rule.sourceFade.durationFrames = 480;
    rule.sourceFade.curveLocalId = 17;
    rule.bridgeFadeIn.curveLocalId = UINT32_MAX;
    rule.bridgeFadeOut.curveLocalId = UINT32_MAX;
    rule.targetFade.active = 1;
    rule.targetFade.durationFrames = 960;
    rule.targetFade.curveLocalId = 18;
    rule.requiredClockFingerprint = 0x12345678;
    rule.authoredStableOrder = 4;

    harken::TransitionProgramViewV1 transition{};
    transition.ownerContainerStableId = harkentest::ObjectId(1001);
    transition.ruleCount = 1;
    transition.downwardPathStepCount = 1;
    transition.maxContainerDescent = 6;
    transition.maxOwnerAscent = 5;
    transition.downwardPathSteps = &path;
    transition.rules = &rule;

    harken::ContainerWorldInstallViewV1 world{};
    world.transitions = &transition;
    world.transitionCount = 1;
    world.identity.capabilityMask = harken::kWorldCapabilityTransition;
    world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV3(harkentest::AsHistoricalPrefix(world));

    std::vector<std::uint8_t> encoded;
    ASSERT_TRUE(harken::EncodeCurrentWorldShippingBody(world, encoded));
    harken::TraceCurrentWorldImage decoded;
    ASSERT_TRUE(decoded.Decode(
        encoded.data(), encoded.size(), harken::kCurrentWorldShippingBodyVintage,
        harken::CurrentWorldDecodeMode::StrictArtifact));
    const auto& roundTrip = *decoded.ProductionView();
    ASSERT_EQ(1u, roundTrip.transitionCount);
    ASSERT_NE(nullptr, roundTrip.transitions);
    ASSERT_EQ(1u, roundTrip.transitions[0].downwardPathStepCount);
    ASSERT_EQ(1u, roundTrip.transitions[0].ruleCount);
    EXPECT_EQ(path.clipGuidLow,
              roundTrip.transitions[0].downwardPathSteps[0].clipGuidLow);
    EXPECT_EQ(rule.sourceExit,
              roundTrip.transitions[0].rules[0].sourceExit);
    EXPECT_EQ(rule.requiredClockFingerprint,
              roundTrip.transitions[0].rules[0].requiredClockFingerprint);
    EXPECT_EQ(rule.targetFade.durationFrames,
              roundTrip.transitions[0].rules[0].targetFade.durationFrames);

    path.clipGuidLow = 99;
    rule.requiredClockFingerprint = 99;
    EXPECT_EQ(11u,
              roundTrip.transitions[0].downwardPathSteps[0].clipGuidLow);
    EXPECT_EQ(UINT64_C(0x12345678),
              roundTrip.transitions[0].rules[0].requiredClockFingerprint);

    std::vector<std::uint8_t> reencoded;
    ASSERT_TRUE(harken::EncodeCurrentWorldShippingBody(roundTrip, reencoded));
    EXPECT_EQ(encoded, reencoded);
}

TEST(TraceCodec,
     CurrentWorldVintageThreeOpaqueTransitionIsRejected) {
    std::array<harken::TransitionProgramViewV1, 2> encodedPlaceholders{};
    harken::ContainerWorldInstallViewV1 world{};
    world.transitions = encodedPlaceholders.data();
    world.transitionCount = encodedPlaceholders.size();
    world.identity.capabilityMask = harken::kWorldCapabilityTransition;
    world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV3Replay(harkentest::AsHistoricalPrefix(world));

    std::vector<std::uint8_t> encoded;
    ASSERT_TRUE(harken::EncodeCurrentWorldBodyV5(harkentest::AsHistoricalPrefix(world), encoded));
    constexpr std::size_t kVintageFourTransitionHeaderBytes =
        sizeof(std::uint64_t) + 6u * sizeof(std::uint32_t);
    constexpr std::size_t kTransitionBytes =
        encodedPlaceholders.size() * kVintageFourTransitionHeaderBytes;
    ASSERT_GE(encoded.size(), kTransitionBytes);
    encoded.resize(encoded.size() - kTransitionBytes);

    harken::TraceCurrentWorldImage decoded;
    EXPECT_FALSE(decoded.Decode(
        encoded.data(), encoded.size(), 3,
        harken::CurrentWorldDecodeMode::ReplayRawAttempt));
    EXPECT_EQ(nullptr, decoded.ReplayView());
    EXPECT_EQ(nullptr, decoded.ProductionView());
}

TEST(TraceCodec, CurrentWorldVintageThreeKeepsZeroGridContainersCompatible) {
    harken::ContainerProgramViewV3 container{};
    container.containerStableId = harkentest::ObjectId(2001);
    container.programSemanticFingerprint = 2002;
    harken::ContainerWorldInstallViewV1 world{};
    world.containers = &container;
    world.containerCount = 1;

    std::vector<std::uint8_t> encoded;
    ASSERT_TRUE(harken::EncodeTraceCurrentWorldBody(world, encoded));
    harken::TraceCurrentWorldImage decoded;
    ASSERT_TRUE(decoded.Decode(
        encoded.data(), encoded.size(),
        harken::kCurrentWorldShippingBodyVintage,
        harken::CurrentWorldDecodeMode::StrictArtifact));
    const auto& roundTrip = *decoded.ProductionView();
    ASSERT_EQ(1u, roundTrip.containerCount);
    EXPECT_EQ(0u, roundTrip.containers[0].discreteAxisCount);
    EXPECT_EQ(nullptr, roundTrip.containers[0].discreteAxes);
    EXPECT_EQ(0u, roundTrip.containers[0].discreteRowCount);
    EXPECT_EQ(nullptr, roundTrip.containers[0].discreteRows);
    EXPECT_EQ(0u, roundTrip.containers[0].continuousAxisCount);
    EXPECT_EQ(nullptr, roundTrip.containers[0].continuousAxes);
    EXPECT_EQ(0u, roundTrip.containers[0].continuousRowCount);
    EXPECT_EQ(nullptr, roundTrip.containers[0].continuousRows);

    std::vector<std::uint8_t> reencoded;
    ASSERT_TRUE(harken::EncodeTraceCurrentWorldBody(roundTrip, reencoded));
    EXPECT_EQ(encoded, reencoded);
}

TEST(TraceCodec,
     CurrentWorldRoundTripsTrackPieceAndScheduleReferenceExactly) {
    harken::PlayablePropertyProgramViewV1 properties[4]{};
    properties[0].contentPropertyKind =
        harken::playableproperties::ContentPropertyKindV1::Sound;
    properties[0].stableId = harkentest::ObjectId(11);
    properties[0].contentRevision = 101;
    properties[0].payload.values[11].bits =
        std::bit_cast<std::uint32_t>(1.0f);
    properties[0].payload.values[3].bits = 1;
    properties[0].payload.presentMask = harken::kResolvedSectionGroupPropertyMaskV1;
    properties[0].payload.values[6].objectId = VN_BUS_MASTER;
    properties[0].programSemanticFingerprint =
        harken::ComputePlayablePropertyFingerprintV1(properties[0]);
    properties[1].contentPropertyKind =
        harken::playableproperties::ContentPropertyKindV1::Container;
    properties[1].authoredStableOrder = 1;
    properties[1].stableId = harkentest::ObjectId(21);
    properties[1].contentRevision = 102;
    properties[1].payload.values[11].bits =
        std::bit_cast<std::uint32_t>(1.0f);
    properties[1].payload.values[3].bits = 1;
    properties[1].payload.presentMask = harken::kResolvedSectionGroupPropertyMaskV1;
    properties[1].payload.values[6].objectId = VN_BUS_MASTER;
    properties[1].programSemanticFingerprint =
        harken::ComputePlayablePropertyFingerprintV1(properties[1]);
    properties[2].contentPropertyKind =
        harken::playableproperties::ContentPropertyKindV1::Track;
    properties[2].authoredStableOrder = 2;
    properties[2].stableId = harkentest::ObjectId(22);
    properties[2].contentRevision = 103;
    properties[2].payload.values[11].bits =
        std::bit_cast<std::uint32_t>(1.0f);
    properties[2].payload.values[3].bits = 1;
    properties[2].payload.presentMask = harken::kResolvedSectionGroupPropertyMaskV1;
    properties[2].payload.values[6].objectId = VN_BUS_MASTER;
    properties[2].programSemanticFingerprint =
        harken::ComputePlayablePropertyFingerprintV1(properties[2]);
    properties[3].contentPropertyKind =
        harken::playableproperties::ContentPropertyKindV1::Piece;
    properties[3].authoredStableOrder = 3;
    properties[3].stableId = harkentest::ObjectId(23);
    properties[3].contentRevision = 104;
    properties[3].payload.values[11].bits =
        std::bit_cast<std::uint32_t>(1.0f);
    properties[3].payload.values[3].bits = 1;
    properties[3].payload.presentMask = harken::kResolvedSectionGroupPropertyMaskV1;
    properties[3].payload.values[6].objectId = VN_BUS_MASTER;
    properties[3].programSemanticFingerprint =
        harken::ComputePlayablePropertyFingerprintV1(properties[3]);

    harken::ScheduleClipPlacementV1 placement{};
    placement.stableOccurrenceOrdinal = 3;
    placement.content.kind = harken::PlayableKindV1::Sound;
    placement.content.stableId = properties[0].stableId;
    placement.content.expectedProgramSemanticFingerprint =
        properties[0].programSemanticFingerprint;
    placement.content.contentRevision = properties[0].contentRevision;
    placement.authoredStart = 1000000;
    placement.authoredFadeIn = 1000;
    placement.authoredFadeOut = 2000;
    placement.clipId = harkentest::ObjectId(31);
    placement.clipGuidHigh = 32;
    placement.clipGuidLow = 33;

    harken::PlayableOccurrencePropertyViewV1 occurrence{};
    occurrence.parentKind =
        harken::playableproperties::ContentPropertyKindV1::Track;
    occurrence.stableOccurrenceOrdinal = placement.stableOccurrenceOrdinal;
    occurrence.parentStableId = properties[2].stableId;
    occurrence.parentProgramSemanticFingerprint =
        properties[2].programSemanticFingerprint;
    occurrence.childPlayable = placement.content;
    occurrence.payload.values[11].bits =
        std::bit_cast<std::uint32_t>(1.0f);
    occurrence.payload.values[3].bits = 1;
    occurrence.occurrenceSemanticFingerprint =
        harken::ComputePlayableOccurrenceFingerprintV1(occurrence);

    harken::TrackScheduleProgramViewV1 track{};
    track.rootDefinition.kind = harken::PlayableKindV1::Track;
    track.rootDefinition.stableId = properties[2].stableId;
    track.rootDefinition.expectedProgramSemanticFingerprint =
        properties[2].programSemanticFingerprint;
    track.rootDefinition.contentRevision = properties[2].contentRevision;
    track.placementCount = 1;
    track.placements = &placement;
    track.scheduleSemanticFingerprint =
        harken::ComputeTrackScheduleFingerprintV1(track);

    harken::ScheduleTrackRefV1 trackRef{};
    trackRef.trackDefinition = track.rootDefinition;
    trackRef.trackScheduleSemanticFingerprint =
        track.scheduleSemanticFingerprint;
    harken::ScheduleMarkerV1 marker{harkentest::ObjectId(41), 7, 1, 5000000};
    harken::PieceScheduleProgramViewV1 piece{};
    piece.rootDefinition.kind = harken::PlayableKindV1::Piece;
    piece.rootDefinition.stableId = properties[3].stableId;
    piece.rootDefinition.expectedProgramSemanticFingerprint =
        properties[3].programSemanticFingerprint;
    piece.rootDefinition.contentRevision = properties[3].contentRevision;
    piece.startPointKind = 1;
    piece.startMarkerId = marker.markerId;
    piece.trackRefCount = 1;
    piece.trackRefs = &trackRef;
    piece.markerCount = 1;
    piece.markers = &marker;
    piece.scheduleSemanticFingerprint =
        harken::ComputePieceScheduleFingerprintV1(piece);

    harken::ContainerChildProgramViewV2 child{};
    child.playable = piece.rootDefinition;
    child.stableChildOrdinal = 8;
    child.authoredStableOrder = 9;
    child.orderWeight = 1;
    child.timingHint = harken::MakeScheduleReferenceTimingHint(
        harken::PlayableKindV1::Piece, piece.rootDefinition.stableId,
        piece.scheduleSemanticFingerprint);
    harken::ContainerProgramViewV3 container{};
    container.containerStableId = properties[1].stableId;
    container.programSemanticFingerprint =
        properties[1].programSemanticFingerprint;
    container.mode = harken::ContainerModeV1::RandomQueue;
    container.childCount = 1;
    container.children = &child;

    harken::ContainerWorldInstallViewV1 world{};
    world.playableProperties = properties;
    world.playablePropertyCount = 4;
    world.playableOccurrences = &occurrence;
    world.playableOccurrenceCount = 1;
    world.trackSchedules = &track;
    world.trackScheduleCount = 1;
    world.pieceSchedules = &piece;
    world.pieceScheduleCount = 1;
    world.containers = &container;
    world.containerCount = 1;
    world.identity.capabilityMask = harken::kWorldCapabilityPlayableProperties;
    world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(world);

    std::vector<std::uint8_t> encoded;
    ASSERT_TRUE(harken::EncodeTraceCurrentWorldBody(world, encoded));
    harken::TraceCurrentWorldImage decoded;
    ASSERT_TRUE(decoded.Decode(
        encoded.data(), encoded.size(),
        harken::kCurrentWorldShippingBodyVintage,
        harken::CurrentWorldDecodeMode::StrictArtifact));
    const auto& roundTrip = *decoded.ProductionView();
    ASSERT_EQ(1u, roundTrip.trackScheduleCount);
    ASSERT_EQ(1u, roundTrip.trackSchedules[0].placementCount);
    ASSERT_EQ(1u, roundTrip.pieceScheduleCount);
    ASSERT_EQ(1u, roundTrip.pieceSchedules[0].trackRefCount);
    ASSERT_EQ(1u, roundTrip.pieceSchedules[0].markerCount);
    ASSERT_EQ(1u, roundTrip.containerCount);
    EXPECT_EQ(placement.authoredStart,
              roundTrip.trackSchedules[0].placements[0].authoredStart);
    EXPECT_EQ(track.scheduleSemanticFingerprint,
              roundTrip.trackSchedules[0].scheduleSemanticFingerprint);
    EXPECT_EQ(piece.scheduleSemanticFingerprint,
              roundTrip.pieceSchedules[0].scheduleSemanticFingerprint);
    EXPECT_EQ(marker.position, roundTrip.pieceSchedules[0].markers[0].position);
    EXPECT_EQ(harken::ChildTimingHintKindV2::ScheduleReference,
              roundTrip.containers[0].children[0].timingHint.kind);
    EXPECT_EQ(piece.scheduleSemanticFingerprint,
              roundTrip.containers[0].children[0]
                  .timingHint.payload.schedule.scheduleSemanticFingerprint);
    std::vector<std::uint8_t> reencoded;
    ASSERT_TRUE(harken::EncodeTraceCurrentWorldBody(roundTrip, reencoded));
    EXPECT_EQ(encoded, reencoded);

    const auto overwriteU32 = [](std::vector<std::uint8_t>& bytes,
                                 std::size_t offset,
                                 std::uint32_t value) {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    };
    auto corruptAggregate = encoded;
    overwriteU32(corruptAggregate, 92, 2);
    harken::TraceCurrentWorldImage aggregateDecode;
    EXPECT_FALSE(aggregateDecode.Decode(
        corruptAggregate.data(), corruptAggregate.size(), 3,
        harken::CurrentWorldDecodeMode::ReplayRawAttempt));
    auto corruptCapacity = encoded;
    overwriteU32(corruptCapacity, 164,
                 harken::kHardTrackScheduleProgramCapacity + 1);
    harken::TraceCurrentWorldImage capacityDecode;
    EXPECT_FALSE(capacityDecode.Decode(
        corruptCapacity.data(), corruptCapacity.size(), 3,
        harken::CurrentWorldDecodeMode::ReplayRawAttempt));
    ASSERT_GT(encoded.size(), 200u);
    harken::TraceCurrentWorldImage truncatedPrefixDecode;
    EXPECT_FALSE(truncatedPrefixDecode.Decode(
        encoded.data(), 199, 3,
        harken::CurrentWorldDecodeMode::ReplayRawAttempt));

    auto unknownCompletion = encoded;
    ASSERT_GE(unknownCompletion.size(), 32u);
    overwriteU32(unknownCompletion, unknownCompletion.size() - 32, 99);
    harken::TraceCurrentWorldImage unknownDecode;
    EXPECT_FALSE(unknownDecode.Decode(
        unknownCompletion.data(), unknownCompletion.size(), 3,
        harken::CurrentWorldDecodeMode::ReplayRawAttempt));

    child.timingHint.kind = static_cast<harken::ChildTimingHintKindV2>(99);
    std::vector<std::uint8_t> unknownEncoded = {1, 2, 3};
    EXPECT_FALSE(harken::EncodeTraceCurrentWorldBody(world, unknownEncoded));
    EXPECT_TRUE(unknownEncoded.empty());
}

TEST(TraceCodec, FrozenVintageTwoEmptyFixtureUsesOnlyVintageTwoGrammar) {
    std::vector<std::uint8_t> bytes;
    const auto appendU32 = [&](std::uint32_t value) {
        for (std::uint32_t shift = 0; shift < 32; shift += 8) {
            bytes.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    };
    const auto appendU64 = [&](std::uint64_t value) {
        appendU32(static_cast<std::uint32_t>(value));
        appendU32(static_cast<std::uint32_t>(value >> 32));
    };
    for (int i = 0; i < 6; ++i) appendU32(1);
    appendU64(UINT64_C(0x8b272f6b42796eba));
    appendU64(0);
    for (int i = 0; i < 12; ++i) appendU32(0);
    const harken::WorldCapacitySummaryV1 capacity{};
    appendU32(capacity.maxContainerDepth);
    appendU32(capacity.maxChildrenPerContainer);
    appendU32(capacity.maxAvoidLastN);
    appendU32(capacity.maxDiscreteAxes);
    appendU32(capacity.maxDiscreteRows);
    appendU32(capacity.maxDiscreteTrieNodes);
    appendU32(capacity.maxContinuousAxes);
    appendU32(capacity.maxContinuousRows);
    appendU32(capacity.maxContinuousCandidates);
    appendU32(capacity.maxContinuousActiveRows);
    appendU32(capacity.maxContinuousLiveRows);
    appendU32(capacity.maxTransitionRules);
    appendU32(capacity.maxCurvePoints);
    appendU32(capacity.maxKnownScheduleLeaves);

    harken::TraceCurrentWorldImage decoded;
    EXPECT_FALSE(decoded.Decode(
        bytes.data(), bytes.size(), 2,
        harken::CurrentWorldDecodeMode::ReplayRawAttempt));
    EXPECT_EQ(nullptr, decoded.ReplayView());
    harken::TraceCurrentWorldImage wrongVintage;
    EXPECT_FALSE(wrongVintage.Decode(
        bytes.data(), bytes.size(), 3,
        harken::CurrentWorldDecodeMode::ReplayRawAttempt));
}

TEST(TraceCodec,
     FrozenVintageTwoOpaqueTransitionIsRejected) {
    std::array<harken::TransitionProgramViewV1, 1> placeholders{};
    harken::ContainerWorldInstallViewV1 world{};
    world.transitions = placeholders.data();
    world.transitionCount = placeholders.size();
    world.identity.capabilityMask = harken::kWorldCapabilityTransition;
    world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV2(harkentest::AsHistoricalPrefix(world));

    std::vector<std::uint8_t> bytes;
    const auto appendU32 = [&](std::uint32_t value) {
        for (std::uint32_t shift = 0; shift < 32; shift += 8) {
            bytes.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    };
    const auto appendU64 = [&](std::uint64_t value) {
        appendU32(static_cast<std::uint32_t>(value));
        appendU32(static_cast<std::uint32_t>(value >> 32));
    };
    for (int i = 0; i < 6; ++i) appendU32(1);
    appendU64(world.identity.runtimeSemanticFingerprint);
    appendU64(world.identity.capabilityMask);
    for (int i = 0; i < 11; ++i) appendU32(0);
    appendU32(world.transitionCount);
    appendU32(world.capacity.maxContainerDepth);
    appendU32(world.capacity.maxChildrenPerContainer);
    appendU32(world.capacity.maxAvoidLastN);
    appendU32(world.capacity.maxDiscreteAxes);
    appendU32(world.capacity.maxDiscreteRows);
    appendU32(world.capacity.maxDiscreteTrieNodes);
    appendU32(world.capacity.maxContinuousAxes);
    appendU32(world.capacity.maxContinuousRows);
    appendU32(world.capacity.maxContinuousCandidates);
    appendU32(world.capacity.maxContinuousActiveRows);
    appendU32(world.capacity.maxContinuousLiveRows);
    appendU32(world.capacity.maxTransitionRules);
    appendU32(world.capacity.maxCurvePoints);
    appendU32(world.capacity.maxKnownScheduleLeaves);

    harken::TraceCurrentWorldImage decoded;
    EXPECT_FALSE(decoded.Decode(
        bytes.data(), bytes.size(), 2,
        harken::CurrentWorldDecodeMode::ReplayRawAttempt));
    EXPECT_EQ(nullptr, decoded.ReplayView());
    EXPECT_EQ(nullptr, decoded.ProductionView());
}

TEST(TraceCodec, FrozenVintageTwoTrackAndPieceAreRejected) {
    harken::PlayablePropertyProgramViewV1 properties[2]{};
    properties[0].contentPropertyKind =
        harken::playableproperties::ContentPropertyKindV1::Track;
    properties[0].stableId = harkentest::ObjectId(11);
    properties[0].contentRevision = 101;
    properties[0].payload.values[11].bits =
        std::bit_cast<std::uint32_t>(1.0f);
    properties[0].payload.values[3].bits = 1;
    properties[0].programSemanticFingerprint =
        harken::ComputePlayablePropertyFingerprintV1(properties[0]);
    properties[1].contentPropertyKind =
        harken::playableproperties::ContentPropertyKindV1::Piece;
    properties[1].authoredStableOrder = 1;
    properties[1].stableId = harkentest::ObjectId(12);
    properties[1].contentRevision = 102;
    properties[1].payload.values[11].bits =
        std::bit_cast<std::uint32_t>(1.0f);
    properties[1].payload.values[3].bits = 1;
    properties[1].programSemanticFingerprint =
        harken::ComputePlayablePropertyFingerprintV1(properties[1]);
    harken::ContainerWorldInstallViewV1 source{};
    source.playableProperties = properties;
    source.playablePropertyCount = 2;
    source.identity.capabilityMask =
        harken::kWorldCapabilityPlayableProperties;
    source.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV2(harkentest::AsHistoricalPrefix(source));

    std::vector<std::uint8_t> bytes;
    const auto appendU8 = [&](std::uint8_t value) { bytes.push_back(value); };
    const auto appendU16 = [&](std::uint16_t value) {
        appendU8(static_cast<std::uint8_t>(value));
        appendU8(static_cast<std::uint8_t>(value >> 8));
    };
    const auto appendU32 = [&](std::uint32_t value) {
        appendU16(static_cast<std::uint16_t>(value));
        appendU16(static_cast<std::uint16_t>(value >> 16));
    };
    const auto appendU64 = [&](std::uint64_t value) {
        appendU32(static_cast<std::uint32_t>(value));
        appendU32(static_cast<std::uint32_t>(value >> 32));
    };
    const auto appendPayload =
        [&](const harken::PlayablePropertyPayloadViewV1& payload) {
            appendU64(payload.presentMask);
            appendU32(static_cast<std::uint32_t>(payload.values[0].bits));
            appendU32(static_cast<std::uint32_t>(payload.values[1].bits));
            appendU32(static_cast<std::uint32_t>(payload.values[2].bits));
            appendU32(static_cast<std::uint32_t>(payload.values[5].bits));
            appendU32(static_cast<std::uint32_t>(payload.values[4].bits));
            appendU8(static_cast<std::uint8_t>(payload.values[3].bits));
            appendU8(static_cast<std::uint8_t>(payload.values[10].bits));
            appendU16(0);
            appendU32(static_cast<std::uint32_t>(payload.values[8].bits));
            appendU32(static_cast<std::uint32_t>(payload.values[9].bits));
            appendU32(static_cast<std::uint32_t>(payload.values[11].bits));
            appendU32(static_cast<std::uint32_t>(payload.values[12].bits));
            appendU64(payload.values[6].bits);
            appendU64(payload.values[13].bits);
            appendU64(payload.values[14].bits);
            for (int i = 0; i < 5; ++i) appendU32(0);
        };
    for (int i = 0; i < 6; ++i) appendU32(1);
    appendU64(source.identity.runtimeSemanticFingerprint);
    appendU64(source.identity.capabilityMask);
    appendU32(0);
    appendU32(2);
    appendU32(0);
    for (int i = 0; i < 7; ++i) appendU32(0);
    appendU32(0);
    appendU32(0);
    const auto capacity = source.capacity;
    appendU32(capacity.maxContainerDepth);
    appendU32(capacity.maxChildrenPerContainer);
    appendU32(capacity.maxAvoidLastN);
    appendU32(capacity.maxDiscreteAxes);
    appendU32(capacity.maxDiscreteRows);
    appendU32(capacity.maxDiscreteTrieNodes);
    appendU32(capacity.maxContinuousAxes);
    appendU32(capacity.maxContinuousRows);
    appendU32(capacity.maxContinuousCandidates);
    appendU32(capacity.maxContinuousActiveRows);
    appendU32(capacity.maxContinuousLiveRows);
    appendU32(capacity.maxTransitionRules);
    appendU32(capacity.maxCurvePoints);
    appendU32(capacity.maxKnownScheduleLeaves);
    for (const auto& property : properties) {
        appendU32(static_cast<std::uint32_t>(property.contentPropertyKind));
        appendU32(property.authoredStableOrder);
        for (const auto byte : harken::ObjectIdBytes(property.stableId)) appendU8(byte);
        appendU64(property.programSemanticFingerprint);
        appendU64(property.contentRevision);
        for (const auto byte : harken::ObjectIdBytes(property.routeStableId)) appendU8(byte);
        appendU64(property.compatibilityFingerprint);
        appendU32(0);
        appendU32(0);
        appendPayload(property.payload);
    }

    harken::TraceCurrentWorldImage decoded;
    EXPECT_FALSE(decoded.Decode(
        bytes.data(), bytes.size(), 2,
        harken::CurrentWorldDecodeMode::ReplayRawAttempt));
    EXPECT_EQ(nullptr, decoded.ReplayView());
    EXPECT_EQ(nullptr, decoded.ProductionView());
}

TEST(TraceCodec, ConfigWireImageRoundTripsWithoutNativePointers) {
    VnConfig config = MakeNullConfig(7, kBlockFrames, 44100);
    config.maxBuses = 47;
    config.randomSeed = 0x12345678u;
    config.maxTopologyPlanBytes = 17u * 1024u * 1024u;
    config.maxActionFirings = 777;
    config.maxActionCooldownRows = 333;
    config.onNotification = reinterpret_cast<VnNotificationCallback>(
        static_cast<std::uintptr_t>(0x1234u));
    config.notificationUserData = reinterpret_cast<void*>(
        static_cast<std::uintptr_t>(0x5678u));
    config.logCallback = reinterpret_cast<VnLogFn>(
        static_cast<std::uintptr_t>(0x9ABCu));
    config.logUserData = reinterpret_cast<void*>(
        static_cast<std::uintptr_t>(0xDEF0u));

    std::vector<std::uint8_t> wire;
    ASSERT_TRUE(harken::EncodeTraceConfig(config, wire));
    EXPECT_EQ(harken::kTraceConfigWireBytes, wire.size());
    EXPECT_NE(sizeof(VnConfig), wire.size());

    VnConfig decoded{};
    std::uint32_t vintage = 0;
    ASSERT_TRUE(harken::DecodeTraceConfig(wire.data(), wire.size(),
                                         &decoded, &vintage));
    EXPECT_EQ(harken::kTraceConfigVintageCount, vintage);
    EXPECT_EQ(sizeof(VnConfig), decoded.structSize);
    EXPECT_EQ(config.sampleRate, decoded.sampleRate);
    EXPECT_EQ(config.maxBuses, decoded.maxBuses);
    EXPECT_EQ(config.randomSeed, decoded.randomSeed);
    EXPECT_EQ(config.maxTopologyPlanBytes, decoded.maxTopologyPlanBytes);
    EXPECT_EQ(config.maxActionFirings, decoded.maxActionFirings);
    EXPECT_EQ(config.maxActionCooldownRows,
              decoded.maxActionCooldownRows);
    EXPECT_EQ(nullptr, decoded.onNotification);
    EXPECT_EQ(nullptr, decoded.notificationUserData);
    EXPECT_EQ(nullptr, decoded.logCallback);
    EXPECT_EQ(nullptr, decoded.logUserData);
}

TEST(TraceCodec, VintageFifteenOmitsCooldownCapacity) {
    VnConfig config = MakeNullConfig(7, kBlockFrames, 44100);
    config.structSize =
        static_cast<std::uint32_t>(offsetof(VnConfig, maxActionCooldownRows));
    config.maxActionCooldownRows = 333;
    config.reserved12 = 1;

    std::vector<std::uint8_t> wire;
    ASSERT_TRUE(harken::EncodeTraceConfig(config, wire));
    EXPECT_EQ(harken::kTraceConfigWireBytesV15, wire.size());
    EXPECT_EQ(15u, harken::ConfigVintageFromNativeSize(config.structSize));

    VnConfig decoded{};
    std::uint32_t vintage = 0;
    ASSERT_TRUE(harken::DecodeTraceConfig(wire.data(), wire.size(),
                                         &decoded, &vintage));
    EXPECT_EQ(15u, vintage);
    EXPECT_EQ(0u, decoded.maxActionCooldownRows);
    EXPECT_EQ(0u, decoded.reserved12);
}

TEST(TraceCodec, VintageSixteenRejectsNonZeroReserved12) {
    VnConfig config = MakeNullConfig(7, kBlockFrames, 44100);
    config.reserved12 = 1;
    std::vector<std::uint8_t> wire;
    EXPECT_FALSE(harken::EncodeTraceConfig(config, wire));
}

TEST(TraceCodec, HistoricalPendingActionCapacityIsConsumedAndNormalized) {
    VnConfig config = MakeNullConfig(7, kBlockFrames, 44100);
    config.structSize = 280;
    config.pendingActionCapacity = 37;

    std::vector<std::uint8_t> wire;
    ASSERT_TRUE(harken::EncodeTraceConfig(config, wire));
    EXPECT_EQ(harken::kTraceConfigWireBytesV13, wire.size());

    VnConfig decoded{};
    std::uint32_t vintage = 0;
    ASSERT_TRUE(harken::DecodeTraceConfig(wire.data(), wire.size(),
                                         &decoded, &vintage));
    EXPECT_EQ(13u, vintage);
    EXPECT_EQ(0u, decoded.pendingActionCapacity);
    EXPECT_EQ(0u, decoded.maxActionFirings);
}

TEST(TraceCodec, CurrentVintageRejectsNonZeroActionCapacityTombstone) {
    VnConfig config = MakeNullConfig(7, kBlockFrames, 44100);
    config.pendingActionCapacity = 1;
    std::vector<std::uint8_t> wire;
    EXPECT_FALSE(harken::EncodeTraceConfig(config, wire));

    config.pendingActionCapacity = 0;
    ASSERT_TRUE(harken::EncodeTraceConfig(config, wire));
    constexpr std::size_t kPendingCapacityWireOffset = 16 + 20 * 4;
    ASSERT_GE(wire.size(), kPendingCapacityWireOffset + sizeof(std::uint32_t));
    const std::uint32_t nonZero = 1;
    std::memcpy(wire.data() + kPendingCapacityWireOffset, &nonZero,
                sizeof nonZero);

    VnConfig decoded{};
    std::uint32_t vintage = 0;
    EXPECT_FALSE(harken::DecodeTraceConfig(wire.data(), wire.size(),
                                          &decoded, &vintage));
}

TEST(TraceCodec, RequestedDefaultsAndResolvedDeviceFormatStayDistinct) {
    harken::TraceSessionOrigin origin;
    origin.requested = MakeNullConfig(4, kBlockFrames, 0);
    origin.requested.deviceMode = VN_DEVICE_REAL;
    origin.requested.deviceChannelLayout = VN_CHANNEL_LAYOUT_DEFAULT;
    origin.resolvedSampleRate = 96000;
    origin.resolvedDeviceLayout = VN_CHANNEL_LAYOUT_7_1;

    std::vector<std::uint8_t> wire;
    ASSERT_TRUE(harken::EncodeTraceConfig(origin, wire));
    VnConfig requested{};
    std::uint32_t vintage = 0;
    std::uint32_t resolvedRate = 0;
    std::uint32_t resolvedLayout = VN_CHANNEL_LAYOUT_DEFAULT;
    ASSERT_TRUE(harken::DecodeTraceConfig(
        wire.data(), wire.size(), &requested, &vintage,
        &resolvedRate, &resolvedLayout));
    EXPECT_EQ(0u, requested.sampleRate);
    EXPECT_EQ(VN_CHANNEL_LAYOUT_DEFAULT,
              requested.deviceChannelLayout);
    EXPECT_EQ(96000u, resolvedRate);
    EXPECT_EQ(VN_CHANNEL_LAYOUT_7_1, resolvedLayout);
}

TEST(Trace, StartBeforeAnyStatefulCallCapturesAndStops) {
    EngineSession session(MakeNullConfig(4, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::string path = TracePath("trace_basic.vtrc");

    EXPECT_EQ(VN_OK, Vn_TraceStart(path.c_str()));
    EXPECT_EQ(1u, Capture().traceActive);

    ASSERT_NO_FATAL_FAILURE(RegisterToneSound(harkentest::ObjectId(1)));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    session.Engine()->nullBackend.Pump(3);
    EXPECT_EQ(VN_OK, Vn_Update());

    EXPECT_EQ(VN_OK, Vn_TraceStop());
    EXPECT_EQ(0u, Capture().traceActive);
    // Stop with nothing active stays a benign no-op.
    EXPECT_EQ(VN_OK, Vn_TraceStop());

    const JournalShape shape = ReadJournalShape(path);
    EXPECT_TRUE(shape.magicOk);
    EXPECT_EQ(harken::kTraceFormatVersion, shape.version);
    EXPECT_TRUE(shape.wellFormed);
    EXPECT_GT(shape.recordCount, 4u); // calls + block barriers + update pair
    EXPECT_EQ(harken::kTraceRecordEnd, shape.lastRecordType);
}

TEST(Trace, QueriesAndUpdatesDoNotCloseTheStartGate) {
    EngineSession session(MakeNullConfig(4, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());

    // Everything here is non-stateful by the header's definition: pumps,
    // updates, and snapshot captures mutate no content.
    session.Engine()->nullBackend.Pump(2);
    EXPECT_EQ(VN_OK, Vn_Update());
    (void)Capture();
    const std::string path = TracePath("trace_after_queries.vtrc");
    EXPECT_EQ(VN_OK, Vn_TraceStart(path.c_str()));
    EXPECT_EQ(VN_OK, Vn_TraceStop());
}

TEST(Trace, DynamicRouteCaptureOrdersByTargetDenseIndexThenSlots) {
    VnConfig config = MakeNullConfig(4, kBlockFrames);
    config.fxSlotsPerBus = 2;
    config.compressorEffectPoolCapacity = 2;
    config.dynamicRouteCapacity = 4;
    config.dynamicBusTapCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    constexpr VnBusId kSource = harkentest::ObjectId(100);
    constexpr VnBusId kFirstTarget = harkentest::ObjectId(101);
    constexpr VnBusId kSecondTarget = harkentest::ObjectId(102);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSource, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kFirstTarget, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSecondTarget, VN_BUS_MASTER));

    VnCompressorDesc compressor{};
    compressor.structSize = sizeof compressor;
    compressor.thresholdDb = -24.0f;
    compressor.ratio = 4.0f;
    compressor.attackMs = 5.0f;
    compressor.releaseMs = 80.0f;
    compressor.kneeDb = 3.0f;
    ASSERT_EQ(
        VN_OK,
        Vn_SetBusEffect(
            kFirstTarget, 1, VN_EFFECT_COMPRESSOR,
            reinterpret_cast<const VnEffectDesc*>(&compressor)));
    ASSERT_EQ(
        VN_OK,
        Vn_SetBusEffect(
            kSecondTarget, 0, VN_EFFECT_COMPRESSOR,
            reinterpret_cast<const VnEffectDesc*>(&compressor)));
    session.Engine()->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());

    VnEffectDynamicRouteDesc route{};
    route.structSize = sizeof route;
    route.signalForm = VN_DYNAMIC_SIGNAL_RAW_AUDIO;
    route.sourceKind = VN_DYNAMIC_SOURCE_BUS;
    route.tap = VN_DYNAMIC_TAP_BUS_PRE_FADER;
    route.sourceId = kSource;
    route.expectedEffectType = VN_EFFECT_COMPRESSOR;

    // Allocate the later target first. Route-table order is therefore the
    // reverse of the frozen public capture order.
    ASSERT_EQ(VN_OK, Vn_SetBusEffectDynamicRoute(
                         kSecondTarget, 0, 1, &route));
    ASSERT_EQ(VN_OK, Vn_SetBusEffectDynamicRoute(
                         kFirstTarget, 1, 0, &route));

    const VnEffectDynamicRouteInfo* const scratchData =
        session.Engine()->dynamicRouteCaptureScratch.data();
    const std::size_t scratchCapacity =
        session.Engine()->dynamicRouteCaptureScratch.capacity();
    VnEffectDynamicRouteInfo rows[2] = {};
    rows[0].structSize = sizeof(VnEffectDynamicRouteInfo);
    std::uint32_t required = 0;
    ASSERT_EQ(VN_OK,
              Vn_CaptureEffectDynamicRoutes(rows, 2, &required));
    ASSERT_EQ(2u, required);
    EXPECT_EQ(kFirstTarget, rows[0].targetBusId);
    EXPECT_EQ(1u, rows[0].effectSlot);
    EXPECT_EQ(0u, rows[0].routeSlot);
    EXPECT_EQ(kSecondTarget, rows[1].targetBusId);
    EXPECT_EQ(0u, rows[1].effectSlot);
    EXPECT_EQ(1u, rows[1].routeSlot);

    EXPECT_EQ(0u, session.Engine()->captureTruncationCount);
    VnEffectDynamicRouteInfo one{};
    one.structSize = sizeof one;
    ASSERT_EQ(VN_OK,
              Vn_CaptureEffectDynamicRoutes(&one, 1, &required));
    EXPECT_EQ(2u, required);
    EXPECT_EQ(scratchData,
              session.Engine()->dynamicRouteCaptureScratch.data());
    EXPECT_EQ(scratchCapacity,
              session.Engine()->dynamicRouteCaptureScratch.capacity());
    EXPECT_EQ(1u, session.Engine()->captureTruncationCount);
    harken::LogRecord record{};
    ASSERT_TRUE(harkentest::FindLogicLog(
        session.Engine(), harken::kLogCodeCaptureTruncated, &record));
    EXPECT_EQ(harken::kCaptureSurfaceEffectDynamicRoutes, record.arg0);
    EXPECT_EQ(2u, record.arg1);
}

TEST(Trace, DynamicRouteDetectorCountsOnlyValuesAboveFloatMaxAsSaturated) {
    struct Result {
        float detector = 0.0f;
        harken::DynamicDetectorStatus status{};
        std::uint64_t totalNonFinite = 0;
        std::uint64_t totalSaturation = 0;
    };
    const auto evaluate = [](float sample, float levelLinear) {
        harken::EngineConfig config;
        config.dynamicRouteCapacity = 1;
        config.dynamicRoutePlanPoolCapacity = 2;
        config.dynamicBusTapCapacity = 1;
        harken::DynamicRouteSystem routes(config);
        EXPECT_EQ(0u, routes.TapSampleStorageBytes());

        std::uint32_t tableIndex = 0;
        EXPECT_TRUE(routes.AllocateRouteSlot(&tableIndex));
        harken::DynamicRouteDefinition route{};
        route.active = true;
        route.tableIndex = tableIndex;
        route.generation = routes.NextRouteGeneration(tableIndex);
        route.revision = 1;
        route.targetBusIndex = 2;
        route.effectSlot = 0;
        route.sourceKind = VN_DYNAMIC_SOURCE_BUS;
        route.sourceIndex = 1;
        route.tap = VN_DYNAMIC_TAP_BUS_PRE_FADER;
        route.levelLinear = levelLinear;
        routes.SetTapSampleCommitPageLimitsForTests(0, SIZE_MAX);
        EXPECT_FALSE(routes.AcquireRouteTap(route));
        EXPECT_EQ(0u, routes.BusTapInUse());
        EXPECT_EQ(0u, routes.TapGeneration(VN_DYNAMIC_SOURCE_BUS, 0));
        routes.SetTapSampleCommitPageLimitsForTests(SIZE_MAX, SIZE_MAX);
        EXPECT_TRUE(routes.AcquireRouteTap(route));
        const auto preparedBytes = routes.TapSampleStorageBytes();
        EXPECT_GT(preparedBytes, 0u);
        routes.CommitSet(route);

        const std::uint32_t plan = routes.AcquirePlan();
        EXPECT_NE(harken::kInvalidDynamicRoutePlanIndex, plan);
        EXPECT_TRUE(routes.BuildPlan(plan, nullptr, false));
        routes.InstallPlan(plan);
        routes.BeginBlock();
        float source[1] = {sample};
        float* channels[1] = {source};
        routes.CaptureBus(1, VN_DYNAMIC_TAP_BUS_PRE_FADER, channels, 1, 1);
        Result result{};
        const float* detector =
            routes.BuildDetector(2, 0, 1, &result.status);
        EXPECT_NE(nullptr, detector);
        result.detector = detector != nullptr ? detector[0] : 0.0f;
        result.totalNonFinite = routes.DetectorNonFiniteCount();
        result.totalSaturation = routes.DetectorSaturationCount();
        EXPECT_EQ(preparedBytes, routes.TapSampleStorageBytes());
        return result;
    };

    const float max = (std::numeric_limits<float>::max)();
    const Result exact = evaluate(max, 1.0f);
    EXPECT_EQ(max, exact.detector);
    EXPECT_EQ(0u, exact.status.saturationCount);
    EXPECT_EQ(0u, exact.totalSaturation);

    const float plus24 =
        static_cast<float>(std::pow(10.0, 24.0 / 20.0));
    const Result over = evaluate(max, plus24);
    EXPECT_EQ(max, over.detector);
    EXPECT_EQ(1u, over.status.saturationCount);
    EXPECT_EQ(1u, over.totalSaturation);

    const Result nan =
        evaluate((std::numeric_limits<float>::quiet_NaN)(), 1.0f);
    EXPECT_EQ(0.0f, nan.detector);
    EXPECT_EQ(1u, nan.status.nonFiniteCount);
    EXPECT_EQ(1u, nan.totalNonFinite);
    EXPECT_EQ(0u, nan.status.saturationCount);

    const Result infinity =
        evaluate((std::numeric_limits<float>::infinity)(), 1.0f);
    EXPECT_EQ(0.0f, infinity.detector);
    EXPECT_EQ(1u, infinity.status.nonFiniteCount);
    EXPECT_EQ(1u, infinity.totalNonFinite);
    EXPECT_EQ(0u, infinity.status.saturationCount);
}

TEST(Trace, VoiceTapReturnRearmsEverySharedRouteBeforeSilentPublication) {
    VnConfig config = MakeNullConfig(1, 4);
    config.fxSlotsPerBus = 1;
    config.compressorEffectPoolCapacity = 1;
    config.dynamicRouteCapacity = 2;
    config.dynamicRoutePlanPoolCapacity = 2;
    config.dynamicVoiceTapCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::DynamicRouteSystem& routes = session.Engine()->dynamicRoutes;
    EXPECT_EQ(0u, routes.TapSampleStorageBytes());

    constexpr VnVoiceHandle kIdentity =
        (static_cast<VnVoiceHandle>(9) << 32) | 7u;
    for (std::uint32_t routeSlot = 0; routeSlot < 2; ++routeSlot) {
        std::uint32_t tableIndex = 0;
        ASSERT_TRUE(routes.AllocateRouteSlot(&tableIndex));
        harken::DynamicRouteDefinition route{};
        route.active = true;
        route.tableIndex = tableIndex;
        route.generation = routes.NextRouteGeneration(tableIndex);
        route.revision = 1;
        route.targetBusIndex = 2;
        route.effectSlot = 0;
        route.routeSlot = routeSlot;
        route.sourceKind = VN_DYNAMIC_SOURCE_VOICE;
        route.sourceIndex = static_cast<std::uint32_t>(kIdentity);
        route.sourceGeneration =
            static_cast<std::uint32_t>(kIdentity >> 32);
        route.tap = VN_DYNAMIC_TAP_VOICE_POST_GAIN_PRE_SPATIAL;
        route.armFirstBlock = true;
        if (routeSlot == 0) {
            routes.SetTapSampleCommitPageLimitsForTests(SIZE_MAX, 0);
            EXPECT_FALSE(routes.AcquireRouteTap(route));
            EXPECT_EQ(0u, routes.VoiceTapInUse());
            EXPECT_EQ(0u, routes.TapGeneration(VN_DYNAMIC_SOURCE_VOICE, 0));
            routes.SetTapSampleCommitPageLimitsForTests(SIZE_MAX, SIZE_MAX);
        }
        ASSERT_TRUE(routes.AcquireRouteTap(route));
        routes.CommitSet(route);
    }

    const std::uint32_t plan = routes.AcquirePlan();
    ASSERT_NE(harken::kInvalidDynamicRoutePlanIndex, plan);
    ASSERT_TRUE(routes.BuildPlan(plan, nullptr, false));
    routes.InstallPlan(plan);

    float source[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float* channels[1] = {source};
    harken::DynamicDetectorStatus detectorStatus{};

    routes.BeginBlock();
    routes.PublishVoiceTap(kIdentity, channels, 1, 0, 4);
    const float* detector =
        routes.BuildDetector(2, 0, 4, &detectorStatus);
    ASSERT_NE(nullptr, detector);
    EXPECT_EQ(2u, detectorStatus.staleRouteCount);
    routes.EndBlock(1);

    routes.BeginBlock();
    routes.PublishVoiceTap(kIdentity, channels, 1, 0, 4);
    detector = routes.BuildDetector(2, 0, 4, &detectorStatus);
    ASSERT_NE(nullptr, detector);
    EXPECT_TRUE(detectorStatus.active);
    EXPECT_FLOAT_EQ(1.0f, detector[0]);
    routes.EndBlock(2);

    routes.InvalidateVoiceTap(kIdentity);
    routes.BeginBlock();
    routes.PublishVoiceTap(kIdentity, nullptr, 0, 0, 0);
    detector = routes.BuildDetector(2, 0, 4, &detectorStatus);
    ASSERT_NE(nullptr, detector);
    EXPECT_EQ(2u, detectorStatus.staleRouteCount);
    EXPECT_FLOAT_EQ(0.0f, detector[0]);
    routes.EndBlock(3);

    std::uint32_t required = 0;
    VnEffectDynamicRouteInfo captured[2]{};
    for (auto& row : captured) {
        row.structSize = sizeof row;
    }
    ASSERT_EQ(VN_OK, routes.CaptureRoutes(captured, 2, &required));
    ASSERT_EQ(2u, required);
    EXPECT_NE(0u, captured[0].stateFlags & VN_DYNAMIC_ROUTE_FIRST_BLOCK);
    EXPECT_NE(0u, captured[1].stateFlags & VN_DYNAMIC_ROUTE_FIRST_BLOCK);

    routes.BeginBlock();
    routes.PublishVoiceTap(kIdentity, channels, 1, 0, 4);
    detector = routes.BuildDetector(2, 0, 4, &detectorStatus);
    ASSERT_NE(nullptr, detector);
    EXPECT_TRUE(detectorStatus.active);
    EXPECT_FLOAT_EQ(0.0f, detector[0])
        << "the alive-but-unrunnable return block publishes valid silence";
    routes.EndBlock(4);
}

TEST(Trace, StatefulCallClosesTheGateWithTooLate) {
    EngineSession session(MakeNullConfig(4, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());

    ASSERT_NO_FATAL_FAILURE(RegisterToneSound(harkentest::ObjectId(1)));

    const std::string path = TracePath("trace_too_late.vtrc");
    std::filesystem::remove(path); // a prior run's leftover must not alias
    EXPECT_EQ(VN_TRACE_TOO_LATE, Vn_TraceStart(path.c_str()));
    // The refusal wrote nothing: no journal file appears.
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(Trace, SecondStartWhileCapturingIsInUse) {
    EngineSession session(MakeNullConfig(4, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::string path = TracePath("trace_first.vtrc");
    ASSERT_EQ(VN_OK, Vn_TraceStart(path.c_str()));
    const std::string second = TracePath("trace_second.vtrc");
    EXPECT_EQ(VN_IN_USE, Vn_TraceStart(second.c_str()));
    EXPECT_EQ(VN_OK, Vn_TraceStop());
}

TEST(Trace, NullPathIsInvalidArg) {
    EngineSession session(MakeNullConfig(4, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    EXPECT_EQ(VN_INVALID_ARG, Vn_TraceStart(nullptr));
}

TEST(Trace, BadPathRefusedAndEngineStaysHealthy) {
    EngineSession session(MakeNullConfig(4, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::string path =
        TracePath("no_such_directory/deeper/trace.vtrc");
    EXPECT_EQ(VN_BAD_TRACE_FILE, Vn_TraceStart(path.c_str()));

    const VnSnapshot snapshot = Capture();
    EXPECT_EQ(0u, snapshot.traceActive);
    EXPECT_EQ(0u, snapshot.traceFailed); // open failure is not a write failure
    ASSERT_NO_FATAL_FAILURE(AssertEngineStillSounds(session.Engine(), harkentest::ObjectId(7)));
}

TEST(Trace, MidSessionWriteFailureStopsTraceKeepsEngineAndLogs) {
    // The log sink collects codes so the failure's log-ring record is
    // observable alongside the sticky snapshot flag.
    static std::vector<std::uint32_t> logCodes;
    logCodes.clear();
    VnConfig config = MakeNullConfig(4, kBlockFrames);
    config.logCallback = [](void*, std::uint32_t, std::uint32_t code,
                            const char*) { logCodes.push_back(code); };
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::string path = TracePath("trace_write_failure.vtrc");
    ASSERT_EQ(VN_OK, Vn_TraceStart(path.c_str()));

    // Injected failure at the next buffered flush (the deterministic stand-
    // in for a full disk); a PCM registration large enough to cross the
    // flush threshold forces that flush inside the recording call.
    session.Engine()->trace->InjectWriteFailure();
    const std::vector<float> big(96000, 0.25f);
    VnPCMDesc pcm = MonoDesc(big, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));

    VnSnapshot snapshot = Capture();
    EXPECT_EQ(0u, snapshot.traceActive); // the trace stopped...
    EXPECT_EQ(1u, snapshot.traceFailed); // ...and says so, stickily

    // The engine runs on: the registered sound plays and renders.
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    session.Engine()->nullBackend.Pump(2);
    EXPECT_EQ(VN_OK, Vn_Update()); // drains the failure's log record
    snapshot = Capture();
    EXPECT_GT(snapshot.busPeaks[0], harkentest::kSilenceFloor);
    EXPECT_EQ(1u, snapshot.traceFailed); // sticky across later captures

    bool sawFailureLog = false;
    for (std::uint32_t code : logCodes) {
        if (code == harken::kLogCodeTraceWriteFailed) {
            sawFailureLog = true;
        }
    }
    EXPECT_TRUE(sawFailureLog);
}

TEST(Trace, AppendAllocationFailureStopsTraceKeepsStatefulCallRunning) {
    EngineSession session(MakeNullConfig(4, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::string path = TracePath("trace_append_failure.vtrc");
    ASSERT_EQ(VN_OK, Vn_TraceStart(path.c_str()));

    {
        TraceRecorderFaultGuard fault(harken::TraceRecorderFault::Append);
        EXPECT_EQ(VN_OK,
                  Vn_SetBusVolume(VN_BUS_MASTER, -3.0f, 0));
    }

    const VnSnapshot snapshot = Capture();
    EXPECT_EQ(0u, snapshot.traceActive);
    EXPECT_EQ(1u, snapshot.traceFailed);

    AssertEngineStillSounds(session.Engine(), harkentest::ObjectId(1));
}

TEST(Trace, NextAcceptedStartClearsTheStickyFailureFlag) {
    EngineSession session(MakeNullConfig(4, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());

    // Fail a trace without any stateful call, so the gate stays open for
    // the second start: the injected failure fires at the stop-time flush.
    const std::string first = TracePath("trace_sticky_first.vtrc");
    ASSERT_EQ(VN_OK, Vn_TraceStart(first.c_str()));
    session.Engine()->nullBackend.Pump(1); // a barrier record to flush
    EXPECT_EQ(VN_OK, Vn_Update());
    session.Engine()->trace->InjectWriteFailure();
    EXPECT_EQ(VN_OK, Vn_TraceStop()); // failure surfaces via the flag
    EXPECT_EQ(1u, Capture().traceFailed);

    const std::string second = TracePath("trace_sticky_second.vtrc");
    ASSERT_EQ(VN_OK, Vn_TraceStart(second.c_str()));
    const VnSnapshot snapshot = Capture();
    EXPECT_EQ(1u, snapshot.traceActive);
    EXPECT_EQ(0u, snapshot.traceFailed); // the accepted start cleared it
    EXPECT_EQ(VN_OK, Vn_TraceStop());
}

TEST(Trace, TermWithActiveTraceFinalizesTheJournal) {
    const std::string path = TracePath("trace_term_finalize.vtrc");
    {
        EngineSession session(MakeNullConfig(4, kBlockFrames));
        ASSERT_EQ(VN_OK, session.InitResult());
        ASSERT_EQ(VN_OK, Vn_TraceStart(path.c_str()));
        ASSERT_NO_FATAL_FAILURE(RegisterToneSound(harkentest::ObjectId(1)));
        session.Engine()->nullBackend.Pump(2);
        // Vn_Term runs at scope exit with the trace still active: the
        // implicit stop must finalize the file.
    }
    const JournalShape shape = ReadJournalShape(path);
    EXPECT_TRUE(shape.magicOk);
    EXPECT_TRUE(shape.wellFormed);
    EXPECT_EQ(harken::kTraceRecordEnd, shape.lastRecordType);
}

// Illegal UTF-8 in the trace path is refused with VN_BAD_TRACE_FILE, never
// an escaping path conversion out of the C ABI. Each literal terminates its
// hex escape at the quote so the following ASCII is a separate token.
constexpr const char* kIllegalUtf8Paths[] = {
    "\xFF\xFE\x80" "trace",   // bytes that are never UTF-8 lead bytes
    "\x80" "orphan",          // an isolated continuation byte, no lead
    "\xC0\xAF" "overlong",    // overlong two-byte encoding of '/'
};

TEST(Trace, IllegalUtf8PathRefusedAndEngineStaysHealthy) {
    EngineSession session(MakeNullConfig(4, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());

    for (const char* illegal : kIllegalUtf8Paths) {
        EXPECT_EQ(VN_BAD_TRACE_FILE, Vn_TraceStart(illegal)) << illegal;
    }

    const VnSnapshot snapshot = Capture();
    EXPECT_EQ(0u, snapshot.traceActive);
    EXPECT_EQ(0u, snapshot.traceFailed); // a rejected open is not a write failure
    ASSERT_NO_FATAL_FAILURE(AssertEngineStillSounds(session.Engine(), harkentest::ObjectId(7)));
}

TEST(Trace, IllegalUtf8StreamPathDuringActiveTraceRefusedAndSurvives) {
    EngineSession session(MakeNullConfig(4, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::string path = TracePath("trace_illegal_cross.vtrc");
    ASSERT_EQ(VN_OK, Vn_TraceStart(path.c_str()));

    // With a trace active, the recorder hashes each referenced content file
    // on the logic thread (the file-reference record). A stream
    // registration whose path is illegal UTF-8 must refuse with
    // VN_BAD_STREAM_FILE without the content-hash conversion escaping the
    // C ABI, on the same logic-thread path that would otherwise throw.
    EXPECT_EQ(VN_BAD_STREAM_FILE,
              Vn_RegisterStreamFile(harkentest::ObjectId(1), "\xFF\xFE\x80" "stream"));

    // The trace layer itself did not fail — a content-file hash failure is
    // not a journal write failure — and the engine keeps rendering.
    const VnSnapshot snapshot = Capture();
    EXPECT_EQ(1u, snapshot.traceActive);
    EXPECT_EQ(0u, snapshot.traceFailed);
    ASSERT_NO_FATAL_FAILURE(AssertEngineStillSounds(session.Engine(), harkentest::ObjectId(7)));

    // The trace finalizes normally after the refusal.
    EXPECT_EQ(VN_OK, Vn_TraceStop());
    EXPECT_EQ(0u, Capture().traceActive);
}

TEST(Trace, ProjectGenerationInstallAckRecordIsFortyByteLayout) {
    EXPECT_EQ(harken::kTraceRecordProjectGenerationInstallAck, 14);
    EXPECT_EQ(harken::kTraceRecordPreparedTopologyPlan, 15);
    EXPECT_EQ(harken::kTracePreparedTopologyPlanVintage, 1u);
    EXPECT_EQ(harken::kTraceCurrentWorldInstallPairedTailBytes, 52u);
    EXPECT_EQ(harken::kTracePreviewPlayPairedTailBytes, 52u);
    EXPECT_EQ(harken::kTracePostActionPairedTailBytes, 52u);
    EXPECT_EQ(sizeof(harken::ProjectGenerationInstallAckV1), 40u);

    EngineSession session(MakeNullConfig(4, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    const std::string path = TracePath("plan09_u2_type14_layout.vtrc");
    std::filesystem::remove(path);
    harken::TraceRecorder recorder(*engine);
    ASSERT_EQ(VN_OK, recorder.Start(path.c_str()));
    harken::ProjectGenerationInstallAckV1 ack{};
    ack.planIndex = 3;
    ack.zero = 0;
    ack.slotGeneration = 11;
    ack.planSequence = 22;
    ack.planFingerprint = 33;
    ack.claimSetFingerprint = 44;
    recorder.RecordProjectGenerationInstallAck(ack);
    recorder.Stop();

    const std::vector<JournalRecord> records = ReadJournalRecords(path);
    const auto found = std::find_if(
        records.begin(), records.end(), [](const JournalRecord& record) {
            return record.type == harken::kTraceRecordProjectGenerationInstallAck;
        });
    ASSERT_NE(records.end(), found);
    ASSERT_EQ(40u, found->payload.size());
    std::uint32_t planIndex = 0;
    std::uint32_t zero = 1;
    std::uint64_t slotGeneration = 0;
    std::uint64_t planSequence = 0;
    std::uint64_t planFingerprint = 0;
    std::uint64_t claimSetFingerprint = 0;
    std::memcpy(&planIndex, found->payload.data(), 4);
    std::memcpy(&zero, found->payload.data() + 4, 4);
    std::memcpy(&slotGeneration, found->payload.data() + 8, 8);
    std::memcpy(&planSequence, found->payload.data() + 16, 8);
    std::memcpy(&planFingerprint, found->payload.data() + 24, 8);
    std::memcpy(&claimSetFingerprint, found->payload.data() + 32, 8);
    EXPECT_EQ(3u, planIndex);
    EXPECT_EQ(0u, zero);
    EXPECT_EQ(11u, slotGeneration);
    EXPECT_EQ(22u, planSequence);
    EXPECT_EQ(33u, planFingerprint);
    EXPECT_EQ(44u, claimSetFingerprint);
}

TEST(Trace, PairedCurrentWorldInstallTailIsBoundToThatRecord) {
    EngineSession session(MakeNullConfig(4, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);

    const std::string path = TracePath("plan09_u2_paired_current_world.vtrc");
    std::filesystem::remove(path);
    harken::TraceRecorder recorder(*engine);
    ASSERT_EQ(VN_OK, recorder.Start(path.c_str()));

    harken::WorldInstallResult result{};
    result.status = harken::WorldInstallStatus::Ok;
    result.priorGeneration = 1;
    result.resultGeneration = 2;
    harken::RuntimeProjectGenerationTokenV1 token{};
    token.projectSessionId = 11;
    token.runtimeEpoch = 3;
    token.authoringRevision = 2;
    token.submissionSerial = 4;
    harken::ContainerWorldInstallViewV1 world{};
    harken::VariableDefinitionV1 variable = harkentest::MakeVariableDefinition(
        harkentest::ObjectId(1), harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.0f);
    world.variables = &variable;
    world.variableCount = 1;
    world.identity.capabilityMask = harken::kWorldCapabilityVariableDefinitions;
    world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(world);
    recorder.RecordCurrentWorldInstall(world, result, 0, &token, 7, 8, 1);
    recorder.RecordCurrentWorldInstall(world, result, 0);
    recorder.Stop();

    const std::vector<JournalRecord> records = ReadJournalRecords(path);
    std::vector<JournalRecord> worlds;
    for (const auto& record : records) {
        if (record.type == harken::kTraceRecordCurrentWorldInstall)
            worlds.push_back(record);
    }
    ASSERT_GE(worlds.size(), 2u);
    const JournalRecord* paired = nullptr;
    const JournalRecord* unpaired = nullptr;
    for (const auto& record : worlds) {
        if (record.payload.size() < 32u + 52u)
            continue;
        std::uint64_t sessionId = 0;
        std::uint64_t runtimeEpoch = 0;
        std::memcpy(&sessionId, record.payload.data() + record.payload.size() - 52u, 8);
        std::memcpy(&runtimeEpoch, record.payload.data() + record.payload.size() - 44u, 8);
        if (sessionId == 11u && runtimeEpoch == 3u) {
            paired = &record;
            break;
        }
    }
    ASSERT_NE(paired, nullptr);
    for (const auto& record : worlds) {
        if (&record == paired)
            continue;
        if (record.payload.size() + 52u == paired->payload.size()) {
            unpaired = &record;
            break;
        }
    }
    ASSERT_NE(paired, nullptr);
    ASSERT_NE(unpaired, nullptr);
    ASSERT_GE(paired->payload.size(), 32u + 52u);
    std::uint64_t sessionId = 0;
    std::uint64_t runtimeEpoch = 0;
    std::uint64_t authoringRevision = 0;
    std::uint64_t submissionSerial = 0;
    std::uint64_t published = 0;
    std::uint64_t compileProfile = 0;
    std::uint32_t vintage = 0;
    const std::size_t tailAt = paired->payload.size() - 52u;
    std::memcpy(&sessionId, paired->payload.data() + tailAt, 8);
    std::memcpy(&runtimeEpoch, paired->payload.data() + tailAt + 8, 8);
    std::memcpy(&authoringRevision, paired->payload.data() + tailAt + 16, 8);
    std::memcpy(&submissionSerial, paired->payload.data() + tailAt + 24, 8);
    std::memcpy(&published, paired->payload.data() + tailAt + 32, 8);
    std::memcpy(&compileProfile, paired->payload.data() + tailAt + 40, 8);
    std::memcpy(&vintage, paired->payload.data() + tailAt + 48, 4);
    EXPECT_EQ(11u, sessionId);
    EXPECT_EQ(3u, runtimeEpoch);
    EXPECT_EQ(2u, authoringRevision);
    EXPECT_EQ(4u, submissionSerial);
    EXPECT_EQ(7u, published);
    EXPECT_EQ(8u, compileProfile);
    EXPECT_EQ(1u, vintage);
    EXPECT_LT(unpaired->payload.size(), paired->payload.size());
    EXPECT_EQ(unpaired->payload.size() + 52u, paired->payload.size());
}

TEST(Trace, TracingOffLeavesSnapshotTraceFieldsZeroUnderLoad) {
    // The OFF-cost contract in observable terms: a full load session that
    // never starts a trace reports both trace fields zero throughout (the
    // structural audio-thread argument lives in this file's header
    // comment).
    EngineSession session(MakeNullConfig(8, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_NO_FATAL_FAILURE(RegisterToneSound(harkentest::ObjectId(1)));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    for (int i = 0; i < 10; ++i) {
        session.Engine()->nullBackend.Pump(1);
        EXPECT_EQ(VN_OK, Vn_Update());
        const VnSnapshot snapshot = Capture();
        EXPECT_EQ(0u, snapshot.traceActive);
        EXPECT_EQ(0u, snapshot.traceFailed);
    }
}
