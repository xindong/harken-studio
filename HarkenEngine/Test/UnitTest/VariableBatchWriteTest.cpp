#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "Common/TestUtil.h"
#include "Common/MonitorExecutionTestSupport.h"
#include "harken/core/VariableExecutionFingerprint.h"
#include "harken/param/VariableService.h"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstdint>
#include <vector>

namespace {

using harkentest::EngineSession;
using harkentest::InstallDefinitionsWorld;
using harkentest::InstallSmoothDefinitions;
using harkentest::MakeNullConfig;
using harkentest::MakeVariableDefinition;
using Token = harken::VariableServiceV1::PreparedPackDefinitions;
using Reason = harken::PackVariablePublicationReason;

VnVariableBatchDescV1 MakeBatch(const VnVariableWriteV1* writes,
                                std::uint32_t count) {
    VnVariableBatchDescV1 desc{};
    desc.structSize = sizeof(desc);
    desc.writeCount = count;
    desc.writes = writes;
    return desc;
}

float ReadValue(harken::Engine* engine, VnVariableId id) {
    float value = 99.0f;
    EXPECT_TRUE(engine->variables.CurrentValue(id, &value));
    return value;
}

std::vector<harkentest::monitor::ActivityRecord> ReadVariableResults(VnMonitorSessionHandle session) {
    auto records = harkentest::monitor::ReadActivityEvents(session);
    std::erase_if(records, [](const auto& row) {
        return row.fact.fact.factKind != VN_MONITOR_FACT_VARIABLE_RESULT;
    });
    return records;
}

TEST(VariableBatchWrite, MonitorReportsAppliedAndSupersededWithOriginalRequestIdentity) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
        InstallSmoothDefinitions(session.Engine(), {harkentest::ObjectId(7), harkentest::ObjectId(8)}).status);
    const auto capture = harkentest::monitor::OpenActivityEvents();
    VnVariableWriteV1 writes[]{{harkentest::ObjectId(7), 0.25f, 0},
        {harkentest::ObjectId(8), 0.5f, 0}};
    const auto desc = MakeBatch(writes, 2);
    VnVariableTicketV1 ticket{};
    ASSERT_EQ(VN_OK, Vn_SetValues(&desc, &ticket));
    const auto batchRequest = session.Engine()->monitorRequests.LastSequence();
    EXPECT_TRUE(ReadVariableResults(capture).empty());
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7), 0.75f));
    const auto replacementRequest = session.Engine()->monitorRequests.LastSequence();
    EXPECT_NE(batchRequest, replacementRequest);
    const auto superseded = ReadVariableResults(capture);
    ASSERT_EQ(2u, superseded.size());
    for (const auto& row : superseded) {
        EXPECT_EQ(batchRequest, row.fact.fact.playbackRequest.sequence);
        EXPECT_EQ(ticket.sequence, row.fact.activity.variableTicket);
        EXPECT_EQ(VN_VARIABLE_OUTCOME_SUPERSEDED, row.fact.activity.variableOutcome);
    }
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(session.Engine()));
    const auto applied = ReadVariableResults(capture);
    ASSERT_EQ(1u, applied.size());
    EXPECT_EQ(replacementRequest, applied[0].fact.fact.playbackRequest.sequence);
    EXPECT_EQ(harkentest::ObjectId(7), applied[0].header.stableId);
    EXPECT_EQ(VN_VARIABLE_OUTCOME_APPLIED, applied[0].fact.activity.variableOutcome);
    EXPECT_FLOAT_EQ(ReadValue(session.Engine(), harkentest::ObjectId(7)),
        std::bit_cast<float>(applied[0].fact.activity.variableValueBits));
    EXPECT_FLOAT_EQ(0.0f, ReadValue(session.Engine(), harkentest::ObjectId(8)));
    VnVariableWriteV1 reset{harkentest::ObjectId(7), 0.0f, 0};
    const auto resetDesc = MakeBatch(&reset, 1);
    ASSERT_EQ(VN_OK, Vn_ResetValues(&resetDesc, &ticket));
    const auto resetRequest = session.Engine()->monitorRequests.LastSequence();
    EXPECT_TRUE(ReadVariableResults(capture).empty());
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(session.Engine()));
    const auto resetRecords = ReadVariableResults(capture);
    ASSERT_EQ(1u, resetRecords.size());
    EXPECT_EQ(resetRequest, resetRecords[0].fact.fact.playbackRequest.sequence);
    EXPECT_EQ(ticket.sequence, resetRecords[0].fact.activity.variableTicket);
    EXPECT_EQ(VN_VARIABLE_OUTCOME_APPLIED, resetRecords[0].fact.activity.variableOutcome);
    EXPECT_FLOAT_EQ(0.0f, std::bit_cast<float>(resetRecords[0].fact.activity.variableValueBits));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(capture));
}

TEST(VariableBatchWrite, MonitorKeepsEmitterResultsAndRejectedRequestsDistinct) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
        InstallSmoothDefinitions(session.Engine(), {harkentest::ObjectId(7)}).status);
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    const auto capture = harkentest::monitor::OpenActivityEvents();
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, harkentest::ObjectId(7), 0.25f));
    const auto acceptedRequest = session.Engine()->monitorRequests.LastSequence();
    EXPECT_TRUE(ReadVariableResults(capture).empty());
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(session.Engine()));
    auto records = ReadVariableResults(capture);
    ASSERT_EQ(1u, records.size());
    EXPECT_EQ(acceptedRequest, records[0].fact.fact.playbackRequest.sequence);
    EXPECT_EQ(emitter, records[0].fact.fact.originEmitter);
    EXPECT_EQ(1u, records[0].fact.activity.variableScope);
    EXPECT_EQ(VN_VARIABLE_OUTCOME_APPLIED, records[0].fact.activity.variableOutcome);
    ASSERT_EQ(VN_NO_VARIABLE, Vn_SetValue(harkentest::ObjectId(99), 0.5f));
    const auto rejectedRequest = session.Engine()->monitorRequests.LastSequence();
    records = ReadVariableResults(capture);
    ASSERT_EQ(1u, records.size());
    EXPECT_NE(acceptedRequest, rejectedRequest);
    EXPECT_EQ(rejectedRequest, records[0].fact.fact.playbackRequest.sequence);
    EXPECT_EQ(VN_NO_VARIABLE, records[0].fact.fact.result);
    EXPECT_EQ(0u, records[0].fact.activity.variableTicket);
    ASSERT_EQ(VN_CAPABILITY_UNAVAILABLE, Vn_SetValueByName("Missing", 0.25f));
    records = ReadVariableResults(capture);
    ASSERT_EQ(1u, records.size());
    EXPECT_EQ(VN_CAPABILITY_UNAVAILABLE, records[0].fact.fact.result);
    EXPECT_TRUE(harken::IsNull(records[0].header.stableId));
    ASSERT_EQ(VN_NO_VARIABLE_LABEL,
        Vn_SetValueByLabelName(harkentest::ObjectId(7), "Missing", 0));
    records = ReadVariableResults(capture);
    ASSERT_EQ(1u, records.size());
    EXPECT_EQ(VN_NO_VARIABLE_LABEL, records[0].fact.fact.result);
    EXPECT_EQ(harkentest::ObjectId(7), records[0].header.stableId);
    ASSERT_EQ(VN_STALE_HANDLE, Vn_SetEmitterValue(1, harkentest::ObjectId(7), 0.5f));
    records = ReadVariableResults(capture);
    ASSERT_EQ(1u, records.size());
    EXPECT_EQ(VN_STALE_HANDLE, records[0].fact.fact.result);
    EXPECT_EQ(0u, records[0].fact.fact.originEmitter);
    EXPECT_EQ(1u, records[0].fact.activity.variableScope);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(capture));
}

TEST(VariableBatchWrite, MonitorSeparatesActionStepExecutionFromVariableApplication) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto variable = MakeVariableDefinition(harkentest::ObjectId(7),
        harken::VariableKindV1::Smooth, -1.0f, 1.0f, 0.0f);
    harken::ActionStepProgramViewV1 step{};
    step.opcode = harken::ActionOpcodeV1::SetVariable;
    step.target.kind = harken::ActionTargetKindV1::Variable;
    step.target.stableId = variable.variableId;
    step.valueBits = std::bit_cast<std::uint32_t>(0.5f);
    for (auto* time : {&step.delay, &step.fade, &step.cooldown}) {
        time->domain = harken::AuthoredTimeDomainV1::AbsoluteNanoseconds;
        time->denominator = 1;
    }
    harken::ActionProgramViewV1 action{};
    action.actionStableId = harkentest::ObjectId(9);
    action.steps = &step;
    action.stepCount = 1;
    action.expandedStepOccurrenceCount = 1;
    action.expandedFiringCount = 1;
    action.programSemanticFingerprint = harken::ComputeActionProgramFingerprintV1(action);
    harken::ContainerWorldInstallViewV1 world{};
    world.identity.projectId = harkentest::ObjectId(10);
    world.identity.capabilityMask = harken::kWorldCapabilityActionDefinitions | harken::kWorldCapabilityVariableDefinitions;
    world.variables = &variable;
    world.variableCount = 1;
    world.actions = &action;
    world.actionCount = 1;
    world.identity.runtimeSemanticFingerprint = harken::ComputeCurrentWorldFingerprintV4(world);
    ASSERT_EQ(harken::WorldInstallStatus::Ok, harken::InstallContainerWorldV1(*session.Engine(), world).status);
    const auto capture = harkentest::monitor::OpenActivityEvents();
    VnPostActionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.actionId = action.actionStableId;
    VnActionFiringHandle firing = 0;
    ASSERT_EQ(VN_OK, Vn_PostAction(&desc, &firing));
    const auto request = session.Engine()->monitorRequests.LastSequence();
    const auto immediate = harkentest::monitor::ReadActivityEvents(capture);
    bool sawAction = false, sawStep = false;
    for (const auto& row : immediate) {
        if (row.fact.fact.factKind == VN_MONITOR_FACT_ACTION_ACCEPTED) sawAction = true;
        if (row.fact.fact.factKind == VN_MONITOR_FACT_STEP_EXECUTED) {
            sawStep = true;
            EXPECT_EQ(request, row.fact.fact.playbackRequest.sequence);
        }
        EXPECT_NE(VN_MONITOR_FACT_VARIABLE_RESULT, row.fact.fact.factKind);
    }
    EXPECT_TRUE(sawAction);
    EXPECT_TRUE(sawStep);
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(session.Engine()));
    const auto applied = ReadVariableResults(capture);
    ASSERT_EQ(1u, applied.size());
    EXPECT_EQ(request, applied[0].fact.fact.playbackRequest.sequence);
    EXPECT_EQ(firing, applied[0].header.actionInstanceHandle);
    EXPECT_EQ(variable.variableId, applied[0].header.stableId);
    EXPECT_EQ(world.identity.projectId, applied[0].fact.fact.projectId);
    EXPECT_EQ(VN_VARIABLE_OUTCOME_APPLIED, applied[0].fact.activity.variableOutcome);
    EXPECT_FLOAT_EQ(0.5f, ReadValue(session.Engine(), variable.variableId));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(capture));
}

TEST(VariableBatchWrite, AbiSizes) {
    EXPECT_EQ(24u, sizeof(VnVariableWriteV1));
    EXPECT_EQ(16u + sizeof(void*), sizeof(VnVariableBatchDescV1));
    EXPECT_EQ(24u, sizeof(VnVariableTicketV1));
    EXPECT_EQ(8u, static_cast<unsigned>(VN_VARIABLE_BATCH_WRITE_CAPACITY));
    EXPECT_EQ(8u, harken::kVariableBatchWriteCapacity);
}

TEST(VariableBatchWrite, CountOneMatchesSetValueShim) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallSmoothDefinitions(session.Engine(), {harkentest::ObjectId(7)}).status);
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7), 0.5f));
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(session.Engine()));
    const float fromShim = ReadValue(session.Engine(), harkentest::ObjectId(7));

    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7), 0.0f));
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(session.Engine()));
    VnVariableWriteV1 write{};
    write.variableId = harkentest::ObjectId(7);
    write.value = 0.5f;
    const auto desc = MakeBatch(&write, 1);
    VnVariableTicketV1 ticket{};
    ASSERT_EQ(VN_OK, Vn_SetValues(&desc, &ticket));
    EXPECT_NE(0u, ticket.sequence);
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(session.Engine()));
    EXPECT_FLOAT_EQ(fromShim, ReadValue(session.Engine(), harkentest::ObjectId(7)));
}

TEST(VariableBatchWrite, BatchAppliesTogether) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallSmoothDefinitions(session.Engine(), {harkentest::ObjectId(7), harkentest::ObjectId(8)}).status);
    VnVariableWriteV1 writes[2]{};
    writes[0].variableId = harkentest::ObjectId(7);
    writes[0].value = 0.25f;
    writes[1].variableId = harkentest::ObjectId(8);
    writes[1].value = 0.5f;
    const auto desc = MakeBatch(writes, 2);
    VnVariableTicketV1 ticket{};
    ASSERT_EQ(VN_OK, Vn_SetValues(&desc, &ticket));
    EXPECT_EQ(2u, session.Engine()->variables.PendingCount());
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(session.Engine()));
    EXPECT_FLOAT_EQ(0.25f, ReadValue(session.Engine(), harkentest::ObjectId(7)));
    EXPECT_FLOAT_EQ(0.5f, ReadValue(session.Engine(), harkentest::ObjectId(8)));
}

TEST(VariableBatchWrite, DuplicateIdsAreInvalidArg) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallSmoothDefinitions(session.Engine(), {harkentest::ObjectId(7)}).status);
    VnVariableWriteV1 writes[2]{};
    writes[0].variableId = harkentest::ObjectId(7);
    writes[0].value = 0.25f;
    writes[1].variableId = harkentest::ObjectId(7);
    writes[1].value = 0.5f;
    const auto desc = MakeBatch(writes, 2);
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetValues(&desc, nullptr));
    EXPECT_EQ(0u, session.Engine()->variables.PendingCount());
    EXPECT_FLOAT_EQ(0.0f, ReadValue(session.Engine(), harkentest::ObjectId(7)));
}

TEST(VariableBatchWrite, ServiceBudgetCannotSplitBatch) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    std::vector<harken::VariableDefinitionV1> definitions;
    for (std::uint64_t idValue = 1; idValue <= 39; ++idValue) {
        const auto id = harkentest::ObjectId(idValue);
        definitions.push_back(MakeVariableDefinition(
            id, harken::VariableKindV1::Smooth, -1.0f, 1.0f, 0.0f));
    }
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallDefinitionsWorld(session.Engine(), definitions.data(),
                                      static_cast<std::uint32_t>(definitions.size())).status);
    for (std::uint64_t idValue = 1; idValue <= 31; ++idValue) {
        const auto id = harkentest::ObjectId(idValue);
        ASSERT_EQ(VN_OK, Vn_SetValue(id, 0.5f));
    }
    VnVariableWriteV1 writes[8]{};
    for (std::uint32_t i = 0; i < 8; ++i) {
        writes[i] = {harkentest::ObjectId(32 + i), 0.5f, 0};
    }
    const auto desc = MakeBatch(writes, 8);
    ASSERT_EQ(VN_OK, Vn_SetValues(&desc, nullptr));
    for (std::uint32_t step = 0; step < 32; ++step) {
        ASSERT_EQ(VN_OK, Vn_Update());
        const float first = ReadValue(session.Engine(), harkentest::ObjectId(32));
        for (std::uint64_t idValue = 33; idValue <= 39; ++idValue) {
        const auto id = harkentest::ObjectId(idValue);
            ASSERT_FLOAT_EQ(first, ReadValue(session.Engine(), id));
        }
        session.Engine()->nullBackend.Pump(1);
    }
    EXPECT_FLOAT_EQ(0.5f, ReadValue(session.Engine(), harkentest::ObjectId(32)));
}

TEST(VariableBatchWrite, BoundAndUnboundRowsBecomeVisibleTogether) {
    for (bool bindBoth : {false, true}) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallSmoothDefinitions(session.Engine(), {harkentest::ObjectId(7), harkentest::ObjectId(8)}).status);
    VnBindingDesc binding{};
    binding.structSize = sizeof(binding);
    binding.variableId = harkentest::ObjectId(8);
    binding.targetScopeKind = VN_TARGET_SCOPE_BUS;
    binding.targetId = harken::kBusTargetBusVolume;
    binding.curve.structSize = sizeof(binding.curve);
    binding.curve.pointCount = 2;
    binding.curve.points[0] = {0.0f, 0.0f};
    binding.curve.points[1] = {1.0f, -12.0f};
    binding.curve.handles[0] = harkentest::EndpointHandles(
        binding.curve.points[0], binding.curve.points[1]);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(100), &binding));
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(attach);
    attach.scopeKind = VN_TARGET_SCOPE_BUS;
    attach.objectId = VN_BUS_MASTER;
    ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(100), &attach));
    if (bindBoth) {
        binding.variableId = harkentest::ObjectId(7);
        ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(101), &binding));
        ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(101), &attach));
    }
    VnVariableWriteV1 writes[] = {{harkentest::ObjectId(7), 0.5f, 0}, {harkentest::ObjectId(8), 0.5f, 0}};
    const auto desc = MakeBatch(writes, 2);
    ASSERT_EQ(VN_OK, Vn_SetValues(&desc, nullptr));
    for (std::uint32_t step = 0; step < 32; ++step) {
        ASSERT_EQ(VN_OK, Vn_Update());
        ASSERT_FLOAT_EQ(ReadValue(session.Engine(), harkentest::ObjectId(7)), ReadValue(session.Engine(), harkentest::ObjectId(8)));
        session.Engine()->nullBackend.Pump(1);
    }
    EXPECT_FLOAT_EQ(0.5f, ReadValue(session.Engine(), harkentest::ObjectId(7)));
    EXPECT_EQ(0u, session.Engine()->variables.PendingCount());
    }
}

TEST(VariableBatchWrite, UnknownMiddleMutatesNothing) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallSmoothDefinitions(session.Engine(), {harkentest::ObjectId(7), harkentest::ObjectId(9)}).status);
    VnVariableWriteV1 writes[3]{};
    writes[0].variableId = harkentest::ObjectId(7);
    writes[0].value = 0.25f;
    writes[1].variableId = harkentest::ObjectId(8);
    writes[1].value = 0.5f;
    writes[2].variableId = harkentest::ObjectId(9);
    writes[2].value = 0.75f;
    const auto desc = MakeBatch(writes, 3);
    EXPECT_EQ(VN_NO_VARIABLE, Vn_SetValues(&desc, nullptr));
    EXPECT_EQ(0u, session.Engine()->variables.PendingCount());
    EXPECT_FLOAT_EQ(0.0f, ReadValue(session.Engine(), harkentest::ObjectId(7)));
    EXPECT_FLOAT_EQ(0.0f, ReadValue(session.Engine(), harkentest::ObjectId(9)));
}

TEST(VariableBatchWrite, CountNineIsInvalidArg) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallSmoothDefinitions(session.Engine(), {harkentest::ObjectId(7)}).status);
    std::array<VnVariableWriteV1, 9> writes{};
    for (std::uint32_t i = 0; i < writes.size(); ++i) {
        writes[i].variableId = harkentest::ObjectId(7 + i);
        writes[i].value = 0.1f * static_cast<float>(i + 1);
    }
    const auto desc = MakeBatch(writes.data(), 9);
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetValues(&desc, nullptr));
    EXPECT_EQ(0u, session.Engine()->variables.PendingCount());
    EXPECT_FLOAT_EQ(0.0f, ReadValue(session.Engine(), harkentest::ObjectId(7)));
}

TEST(VariableBatchWrite, MissingEmitterDoesNotFallBackToGlobal) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallSmoothDefinitions(session.Engine(), {harkentest::ObjectId(7)}).status);
    VnVariableWriteV1 write{};
    write.variableId = harkentest::ObjectId(7);
    write.value = 1.0f;
    const auto desc = MakeBatch(&write, 1);
    EXPECT_EQ(VN_STALE_HANDLE, Vn_SetEmitterValues(0, &desc, nullptr));
    EXPECT_EQ(0u, session.Engine()->variables.PendingCount());
    EXPECT_FLOAT_EQ(0.0f, ReadValue(session.Engine(), write.variableId));
}

TEST(VariableBatch, PartialOverlapSameScopeWhollySupersedes) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallSmoothDefinitions(session.Engine(), {harkentest::ObjectId(7), harkentest::ObjectId(8)}).status);
    VnVariableWriteV1 first[2]{};
    first[0].variableId = harkentest::ObjectId(7);
    first[0].value = 0.25f;
    first[1].variableId = harkentest::ObjectId(8);
    first[1].value = 0.5f;
    const auto firstDesc = MakeBatch(first, 2);
    ASSERT_EQ(VN_OK, Vn_SetValues(&firstDesc, nullptr));
    EXPECT_EQ(2u, session.Engine()->variables.PendingCount());
    VnVariableWriteV1 second{};
    second.variableId = harkentest::ObjectId(8);
    second.value = 0.75f;
    const auto secondDesc = MakeBatch(&second, 1);
    ASSERT_EQ(VN_OK, Vn_SetValues(&secondDesc, nullptr));
    EXPECT_EQ(1u, session.Engine()->variables.PendingCount());
    EXPECT_EQ(2u, session.Engine()->variables.SupersededCount());
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(session.Engine()));
    EXPECT_FLOAT_EQ(0.0f, ReadValue(session.Engine(), harkentest::ObjectId(7)));
    EXPECT_FLOAT_EQ(0.75f, ReadValue(session.Engine(), harkentest::ObjectId(8)));
}

TEST(VariableBatch, GlobalThenEmitterDoesNotSupersedeOtherGlobals) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    std::vector<harken::VariableDefinitionV1> definitions{
        MakeVariableDefinition(harkentest::ObjectId(7), harken::VariableKindV1::Smooth, -1.0f, 1.0f,
                               0.0f),
        MakeVariableDefinition(harkentest::ObjectId(8), harken::VariableKindV1::Smooth, -1.0f, 1.0f,
                               0.0f),
    };
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallDefinitionsWorld(session.Engine(), definitions.data(), 2)
                  .status);
    VnVariableWriteV1 globals[2]{};
    globals[0].variableId = harkentest::ObjectId(7);
    globals[0].value = 0.25f;
    globals[1].variableId = harkentest::ObjectId(8);
    globals[1].value = 0.5f;
    const auto globalDesc = MakeBatch(globals, 2);
    ASSERT_EQ(VN_OK, Vn_SetValues(&globalDesc, nullptr));
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    VnVariableWriteV1 emitterWrite{};
    emitterWrite.variableId = harkentest::ObjectId(7);
    emitterWrite.value = 0.75f;
    const auto emitterDesc = MakeBatch(&emitterWrite, 1);
    ASSERT_EQ(VN_OK, Vn_SetEmitterValues(emitter, &emitterDesc, nullptr));
    EXPECT_EQ(3u, session.Engine()->variables.PendingCount());
    EXPECT_EQ(0u, session.Engine()->variables.SupersededCount());
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(session.Engine()));
    EXPECT_FLOAT_EQ(0.25f, ReadValue(session.Engine(), harkentest::ObjectId(7)));
    EXPECT_FLOAT_EQ(0.5f, ReadValue(session.Engine(), harkentest::ObjectId(8)));
    float emitterValue = 99.0f;
    ASSERT_TRUE(session.Engine()->variables.CurrentValue(emitter, harkentest::ObjectId(7),
                                                         &emitterValue));
    EXPECT_FLOAT_EQ(0.75f, emitterValue);
}

TEST(VariableBatch, CrossScopeBarrierProtectsEveryMemberOfPriorBatch) {
    for (bool useBatch : {false, true}) {
        SCOPED_TRACE(useBatch);
        EngineSession session(MakeNullConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        const auto x = harkentest::ObjectId(7);
        const auto y = harkentest::ObjectId(8);
        ASSERT_EQ(harken::WorldInstallStatus::Ok,
                  InstallSmoothDefinitions(session.Engine(), {x, y}).status);
        VnEmitterHandle emitter = 0;
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
        VnVariableWriteV1 globals[2]{};
        globals[0].variableId = x;
        globals[0].value = 0.25f;
        globals[1].variableId = y;
        globals[1].value = 0.5f;
        const auto initial = MakeBatch(globals, 2);
        ASSERT_EQ(VN_OK, Vn_SetValues(&initial, nullptr));
        ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, x, 0.75f));
        if (useBatch) {
            globals[1].value = 0.125f;
            const auto last = MakeBatch(&globals[1], 1);
            ASSERT_EQ(VN_OK, Vn_SetValues(&last, nullptr));
        } else {
            ASSERT_EQ(VN_OK, Vn_SetValue(y, 0.125f));
        }
        EXPECT_EQ(4u, session.Engine()->variables.PendingCount());
        EXPECT_EQ(0u, session.Engine()->variables.SupersededCount());
        harkentest::AdvanceVariableWrite(session.Engine());
        EXPECT_FLOAT_EQ(0.25f, ReadValue(session.Engine(), x));
        EXPECT_FLOAT_EQ(0.125f, ReadValue(session.Engine(), y));
        float local = 0;
        ASSERT_TRUE(session.Engine()->variables.CurrentValue(emitter, x, &local));
        EXPECT_FLOAT_EQ(0.75f, local);
    }
}

TEST(VariableBatch, DisjointBatchesKeepAcceptedOrder) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallSmoothDefinitions(session.Engine(), {harkentest::ObjectId(7), harkentest::ObjectId(8), harkentest::ObjectId(9)}).status);
    VnVariableWriteV1 first[2]{};
    first[0].variableId = harkentest::ObjectId(7);
    first[0].value = 0.25f;
    first[1].variableId = harkentest::ObjectId(8);
    first[1].value = 0.5f;
    const auto firstDesc = MakeBatch(first, 2);
    ASSERT_EQ(VN_OK, Vn_SetValues(&firstDesc, nullptr));
    VnVariableWriteV1 second{};
    second.variableId = harkentest::ObjectId(9);
    second.value = 0.75f;
    const auto secondDesc = MakeBatch(&second, 1);
    ASSERT_EQ(VN_OK, Vn_SetValues(&secondDesc, nullptr));
    EXPECT_EQ(3u, session.Engine()->variables.PendingCount());
    EXPECT_EQ(0u, session.Engine()->variables.SupersededCount());
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(session.Engine()));
    EXPECT_FLOAT_EQ(0.25f, ReadValue(session.Engine(), harkentest::ObjectId(7)));
    EXPECT_FLOAT_EQ(0.5f, ReadValue(session.Engine(), harkentest::ObjectId(8)));
    EXPECT_FLOAT_EQ(0.75f, ReadValue(session.Engine(), harkentest::ObjectId(9)));
}

TEST(VariableBatch, ResetWritesCurrentInstalledDefault) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto definition = MakeVariableDefinition(
        harkentest::ObjectId(7), harken::VariableKindV1::Smooth, -1.0f, 1.0f, 0.25f);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallDefinitionsWorld(session.Engine(), &definition, 1).status);
    VnVariableWriteV1 write{};
    write.variableId = harkentest::ObjectId(7);
    write.value = 0.5f;
    const auto setDesc = MakeBatch(&write, 1);
    ASSERT_EQ(VN_OK, Vn_SetValues(&setDesc, nullptr));
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(session.Engine()));
    EXPECT_FLOAT_EQ(0.5f, ReadValue(session.Engine(), harkentest::ObjectId(7)));
    write.value = 0.0f;
    const auto resetDesc = MakeBatch(&write, 1);
    ASSERT_EQ(VN_OK, Vn_ResetValues(&resetDesc, nullptr));
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(session.Engine()));
    EXPECT_FLOAT_EQ(0.25f, ReadValue(session.Engine(), harkentest::ObjectId(7)));
}

TEST(VariableBatch, ResetNonzeroValueIsInvalidArg) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallSmoothDefinitions(session.Engine(), {harkentest::ObjectId(7)}).status);
    VnVariableWriteV1 write{};
    write.variableId = harkentest::ObjectId(7);
    write.value = 0.5f;
    const auto desc = MakeBatch(&write, 1);
    EXPECT_EQ(VN_INVALID_ARG, Vn_ResetValues(&desc, nullptr));
    EXPECT_EQ(0u, session.Engine()->variables.PendingCount());
    EXPECT_FLOAT_EQ(0.0f, ReadValue(session.Engine(), harkentest::ObjectId(7)));
}

void PublishInit(harken::Engine* engine, const harken::VariableDefinitionV1* defs,
                 std::uint32_t count,
                 const harken::VariablePolicySidecarV1* overlays) {
    Token token;
    ASSERT_EQ(VN_OK, engine->variables.PreparePackDefinitions(
                         defs, count, overlays, Reason::InitPublication, 1234,
                         &token));
    engine->variables.CommitPreparedPackDefinitions(
        &token, engine->params, engine->emitters, engine->bindings,
        engine->busTargets);
}

TEST(VariableBatch, CompatibleInitReplacePreservesApplied) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto definition = MakeVariableDefinition(
        harkentest::ObjectId(7), harken::VariableKindV1::Smooth, -1.0f, 1.0f, 0.0f);
    std::uint64_t fingerprint = 0;
    ASSERT_TRUE(harken::ComputeVariableExecutionFingerprintV1(
        harkentest::ObjectId(7), 1, 1, 1, -1.0f, 1.0f, 0.0f, nullptr, 0, &fingerprint));
    harken::VariablePolicySidecarV1 overlay{};
    overlay.writers = 1;
    overlay.executionFingerprint = fingerprint;
    ASSERT_NO_FATAL_FAILURE(
        PublishInit(session.Engine(), &definition, 1, &overlay));
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7), 0.5f));
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(session.Engine()));
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, harkentest::ObjectId(7), 0.25f));
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(session.Engine()));
    ASSERT_NO_FATAL_FAILURE(
        PublishInit(session.Engine(), &definition, 1, &overlay));
    EXPECT_FLOAT_EQ(0.5f, ReadValue(session.Engine(), harkentest::ObjectId(7)));
    float emitterValue = 99.0f;
    ASSERT_TRUE(session.Engine()->variables.CurrentValue(emitter, harkentest::ObjectId(7),
                                                         &emitterValue));
    EXPECT_FLOAT_EQ(0.25f, emitterValue);
}

TEST(VariableBatch, FingerprintChangingInitReplaceResetsDefault) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto definition = MakeVariableDefinition(
        harkentest::ObjectId(7), harken::VariableKindV1::Smooth, -1.0f, 1.0f, 0.0f);
    std::uint64_t gameFp = 0;
    std::uint64_t actionFp = 0;
    ASSERT_TRUE(harken::ComputeVariableExecutionFingerprintV1(
        harkentest::ObjectId(7), 1, 1, 1, -1.0f, 1.0f, 0.0f, nullptr, 0, &gameFp));
    ASSERT_TRUE(harken::ComputeVariableExecutionFingerprintV1(
        harkentest::ObjectId(7), 1, 1, 2, -1.0f, 1.0f, 0.0f, nullptr, 0, &actionFp));
    harken::VariablePolicySidecarV1 overlay{};
    overlay.writers = 1;
    overlay.executionFingerprint = gameFp;
    ASSERT_NO_FATAL_FAILURE(
        PublishInit(session.Engine(), &definition, 1, &overlay));
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7), 0.5f));
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(session.Engine()));
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, harkentest::ObjectId(7), 0.25f));
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(session.Engine()));
    overlay.writers = 2;
    overlay.executionFingerprint = actionFp;
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7), 0.75f));
    EXPECT_EQ(1u, session.Engine()->variables.PendingCount());
    ASSERT_NO_FATAL_FAILURE(
        PublishInit(session.Engine(), &definition, 1, &overlay));
    EXPECT_EQ(0u, session.Engine()->variables.PendingCount());
    EXPECT_FLOAT_EQ(0.0f, ReadValue(session.Engine(), harkentest::ObjectId(7)));
    float emitterValue = 99.0f;
    ASSERT_TRUE(session.Engine()->variables.CurrentValue(emitter, harkentest::ObjectId(7),
                                                         &emitterValue));
    EXPECT_FLOAT_EQ(0.0f, emitterValue);
    harken::VariableOutcomeRecord record{};
    bool sawWorldReplacement = false;
    while (session.Engine()->variables.PopOutcome(&record)) {
        if (record.outcome == harken::VariableOutcomeKind::WorldReplacement) {
            sawWorldReplacement = true;
        }
    }
    EXPECT_TRUE(sawWorldReplacement);
}

} // namespace
