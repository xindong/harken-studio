#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "Common/TestUtil.h"
#include "harken/param/ParamTargetTable.h"
#include "harken/param/VariableService.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using harkentest::EngineSession;
using harkentest::InstallDefinitionsWorld;
using harkentest::InstallSmoothDefinitions;
using harkentest::MakeNullConfig;
using harkentest::MakeVariableDefinition;

VnVariableBatchDescV1 MakeBatch(const VnVariableWriteV1* writes,
                                std::uint32_t count) {
    VnVariableBatchDescV1 desc{};
    desc.structSize = sizeof(desc);
    desc.writeCount = count;
    desc.writes = writes;
    return desc;
}

VnVariableTicketQueryResultV1 QueryTicket(const VnVariableTicketV1& ticket) {
    VnVariableTicketQueryDescV1 desc{};
    desc.structSize = sizeof(desc);
    desc.ticket = ticket;
    VnVariableTicketQueryResultV1 out{};
    out.structSize = sizeof(out);
    EXPECT_EQ(VN_OK, Vn_QueryVariableTicket(&desc, &out));
    return out;
}

TEST(VariableTicket, AcceptedIsPendingThenApplied) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallSmoothDefinitions(session.Engine(), {harkentest::ObjectId(7)}).status);
    VnVariableWriteV1 write{};
    write.variableId = harkentest::ObjectId(7);
    write.value = 0.5f;
    const auto desc = MakeBatch(&write, 1);
    VnVariableTicketV1 ticket{};
    ASSERT_EQ(VN_OK, Vn_SetValues(&desc, &ticket));
    const auto pending = QueryTicket(ticket);
    EXPECT_EQ(VN_VARIABLE_TICKET_FOUND, pending.status);
    EXPECT_EQ(VN_VARIABLE_OUTCOME_PENDING, pending.outcome);
    const auto again = QueryTicket(ticket);
    EXPECT_EQ(VN_VARIABLE_TICKET_FOUND, again.status);
    EXPECT_EQ(VN_VARIABLE_OUTCOME_PENDING, again.outcome);
    harken::VariableOutcomeRecord record{};
    EXPECT_FALSE(session.Engine()->variables.PopOutcome(&record));
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(session.Engine()));
    const auto applied = QueryTicket(ticket);
    EXPECT_EQ(VN_VARIABLE_TICKET_FOUND, applied.status);
    EXPECT_EQ(VN_VARIABLE_OUTCOME_APPLIED, applied.outcome);
}

TEST(VariableTicket, PartialOverlapFirstTicketIsSuperseded) {
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
    VnVariableTicketV1 firstTicket{};
    ASSERT_EQ(VN_OK, Vn_SetValues(&firstDesc, &firstTicket));
    VnVariableWriteV1 second{};
    second.variableId = harkentest::ObjectId(8);
    second.value = 0.75f;
    const auto secondDesc = MakeBatch(&second, 1);
    ASSERT_EQ(VN_OK, Vn_SetValues(&secondDesc, nullptr));
    const auto queried = QueryTicket(firstTicket);
    EXPECT_EQ(VN_VARIABLE_TICKET_FOUND, queried.status);
    EXPECT_EQ(VN_VARIABLE_OUTCOME_SUPERSEDED, queried.outcome);
}

TEST(VariableTicket, EmitterDestroyedMapsToTerminatedEmitter) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto definition = MakeVariableDefinition(
        harkentest::ObjectId(7), harken::VariableKindV1::Smooth, -1.0f, 1.0f, 0.0f);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallDefinitionsWorld(session.Engine(), &definition, 1).status);
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    VnVariableWriteV1 write{};
    write.variableId = harkentest::ObjectId(7);
    write.value = 0.5f;
    const auto desc = MakeBatch(&write, 1);
    VnVariableTicketV1 ticket{};
    ASSERT_EQ(VN_OK, Vn_SetEmitterValues(emitter, &desc, &ticket));
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(emitter));
    const auto queried = QueryTicket(ticket);
    EXPECT_EQ(VN_VARIABLE_TICKET_FOUND, queried.status);
    EXPECT_EQ(VN_VARIABLE_OUTCOME_TERMINATED_EMITTER, queried.outcome);
    EXPECT_NE(VN_VARIABLE_OUTCOME_TERMINATED_TRANSPORT, queried.outcome);
    EXPECT_NE(VN_VARIABLE_OUTCOME_TERMINATED_SESSION, queried.outcome);
}

TEST(VariableTicket, TransportResetMapsToTerminatedTransport) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallSmoothDefinitions(session.Engine(), {harkentest::ObjectId(7)}).status);
    VnVariableWriteV1 write{};
    write.variableId = harkentest::ObjectId(7);
    write.value = 0.5f;
    const auto desc = MakeBatch(&write, 1);
    VnVariableTicketV1 ticket{};
    ASSERT_EQ(VN_OK, Vn_SetValues(&desc, &ticket));
    (void)session.Engine()->variables.ResetTransport(0);
    const auto queried = QueryTicket(ticket);
    EXPECT_EQ(VN_VARIABLE_TICKET_FOUND, queried.status);
    EXPECT_EQ(VN_VARIABLE_OUTCOME_TERMINATED_TRANSPORT, queried.outcome);
}

TEST(VariableTicket, SessionMapsToTerminatedSession) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallSmoothDefinitions(session.Engine(), {harkentest::ObjectId(7)}).status);
    VnVariableWriteV1 write{};
    write.variableId = harkentest::ObjectId(7);
    write.value = 0.5f;
    const auto desc = MakeBatch(&write, 1);
    VnVariableTicketV1 ticket{};
    ASSERT_EQ(VN_OK, Vn_SetValues(&desc, &ticket));
    session.Engine()->variables.TerminateSession();
    const auto queried = QueryTicket(ticket);
    EXPECT_EQ(VN_VARIABLE_TICKET_FOUND, queried.status);
    EXPECT_EQ(VN_VARIABLE_OUTCOME_TERMINATED_SESSION, queried.outcome);
}

TEST(VariableTicket, OutcomeExpiredVsUnknownTicket) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallSmoothDefinitions(session.Engine(), {harkentest::ObjectId(7)}).status);
    VnVariableWriteV1 write{};
    write.variableId = harkentest::ObjectId(7);
    write.value = 0.25f;
    const auto desc = MakeBatch(&write, 1);
    VnVariableTicketV1 first{};
    ASSERT_EQ(VN_OK, Vn_SetValues(&desc, &first));
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(session.Engine()));
    for (std::uint32_t i = 0; i < harken::kVariableHistoryCapacity; ++i) {
        write.value = 0.25f;
        ASSERT_EQ(VN_OK, Vn_SetValues(&desc, nullptr));
        ASSERT_NO_FATAL_FAILURE(
            harkentest::AdvanceVariableWrite(session.Engine()));
    }
    const auto expired = QueryTicket(first);
    EXPECT_EQ(VN_VARIABLE_TICKET_OUTCOME_EXPIRED, expired.status);
    EXPECT_EQ(0u, expired.outcome);
    auto wrongWorld = first;
    ++wrongWorld.worldGeneration;
    EXPECT_EQ(VN_VARIABLE_TICKET_UNKNOWN, QueryTicket(wrongWorld).status);
    auto wrongTransport = first;
    ++wrongTransport.transportGeneration;
    EXPECT_EQ(VN_VARIABLE_TICKET_UNKNOWN, QueryTicket(wrongTransport).status);
    VnVariableTicketV1 unknown{};
    unknown.sequence = first.sequence + 100000u;
    unknown.worldGeneration = first.worldGeneration;
    unknown.transportGeneration = first.transportGeneration;
    const auto neverIssued = QueryTicket(unknown);
    EXPECT_EQ(VN_VARIABLE_TICKET_UNKNOWN, neverIssued.status);
    EXPECT_EQ(0u, neverIssued.outcome);
    VnVariableTicketV1 zero{};
    const auto malformed = QueryTicket(zero);
    EXPECT_EQ(VN_VARIABLE_TICKET_UNKNOWN, malformed.status);
    for (std::uint32_t i = 0; i < harken::kVariableHistoryCapacity; ++i) {
        ASSERT_EQ(VN_OK, Vn_SetValues(&desc, nullptr));
        ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(session.Engine()));
    }
    EXPECT_EQ(VN_VARIABLE_TICKET_UNKNOWN, QueryTicket(first).status);
}

TEST(VariableQuery, GlobalCurrentMatchesAppliedBank) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto definition = MakeVariableDefinition(
        harkentest::ObjectId(7), harken::VariableKindV1::Smooth, -1.0f, 1.0f, 0.0f);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallDefinitionsWorld(session.Engine(), &definition, 1).status);
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7), 0.5f));
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(session.Engine()));
    VnVariableQueryDescV1 desc{};
    desc.structSize = sizeof(desc);
    desc.variableId = harkentest::ObjectId(7);
    VnVariableQueryResultV1 out{};
    out.structSize = sizeof(out);
    ASSERT_EQ(VN_OK, Vn_QueryVariable(&desc, &out));
    EXPECT_EQ(static_cast<std::uint32_t>(harken::VariableKindV1::Smooth),
              out.kind);
    EXPECT_EQ(static_cast<std::uint32_t>(harken::VariableScopeV1::Global),
              out.scope);
    EXPECT_FLOAT_EQ(0.5f, out.currentValue);
    EXPECT_EQ(0u, out.hasEmitterOverride);
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, harkentest::ObjectId(7), 0.25f));
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(session.Engine()));
    desc.emitter = emitter;
    out = VnVariableQueryResultV1{};
    out.structSize = sizeof(out);
    ASSERT_EQ(VN_OK, Vn_QueryVariable(&desc, &out));
    EXPECT_FLOAT_EQ(0.25f, out.currentValue);
    EXPECT_EQ(1u, out.hasEmitterOverride);
}

void RegisterMasterBusVolumeBinding(VnBindingId binding, VnVariableId variable,
                                    float y1) {
    VnBindingDesc desc{};
    desc.structSize = sizeof(desc);
    desc.variableId = variable;
    desc.targetScopeKind = VN_TARGET_SCOPE_BUS;
    desc.targetId = harken::kBusTargetBusVolume;
    desc.curve.structSize = sizeof(desc.curve);
    desc.curve.pointCount = 2;
    desc.curve.points[0] = {0.0f, 0.0f};
    desc.curve.points[1] = {1.0f, y1};
    desc.curve.handles[0] =
        harkentest::EndpointHandles(desc.curve.points[0], desc.curve.points[1]);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(binding, &desc));
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(attach);
    attach.scopeKind = VN_TARGET_SCOPE_BUS;
    attach.objectId = VN_BUS_MASTER;
    ASSERT_EQ(VN_OK, Vn_AttachBinding(binding, &attach));
}

TEST(VariableQuery, FiniteOverflowWriteRowIsTargetRejected) {
    VnConfig config = MakeNullConfig();
    config.maxBindings = 4;
    config.maxBindingAttachments = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallSmoothDefinitions(session.Engine(), {harkentest::ObjectId(7)}, 0.0f, 1.0f)
                  .status);
    RegisterMasterBusVolumeBinding(harkentest::ObjectId(1), harkentest::ObjectId(7), 800.0f);
    ASSERT_EQ(VN_OK, Vn_Update());
    session.Engine()->nullBackend.Pump(2);

    VnVariableWriteV1 write{};
    write.variableId = harkentest::ObjectId(7);
    write.value = 1.0f;
    const auto desc = MakeBatch(&write, 1);
    VnVariableTicketV1 ticket{};
    ASSERT_EQ(VN_OK, Vn_SetValues(&desc, &ticket));
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(session.Engine()));
    const auto queried = QueryTicket(ticket);
    EXPECT_EQ(VN_VARIABLE_TICKET_FOUND, queried.status);
    EXPECT_EQ(VN_VARIABLE_OUTCOME_REJECTED_TARGET, queried.outcome);

    VnVariableQueryDescV1 query{};
    query.structSize = sizeof(query);
    query.variableId = harkentest::ObjectId(7);
    VnVariableQueryResultV1 out{};
    out.structSize = sizeof(out);
    ASSERT_EQ(VN_OK, Vn_QueryVariable(&query, &out));
    EXPECT_FLOAT_EQ(0.0f, out.currentValue);
    EXPECT_EQ(0u, out.pendingCount);
}

TEST(VariableQuery, RejectedEmitterClearsOutputAndDoesNotFallBackToGlobal) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallSmoothDefinitions(session.Engine(), {harkentest::ObjectId(7)}).status);
    VnEmitterHandle stale = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&stale));
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(stale));
    VnVariableQueryDescV1 desc{};
    desc.structSize = sizeof(desc);
    desc.variableId = harkentest::ObjectId(7);
    for (const auto emitter : {stale, session.Engine()->emitters.DefaultState()}) {
        SCOPED_TRACE(emitter);
        desc.emitter = emitter;
        VnVariableQueryResultV1 out{};
        out.structSize = sizeof(out);
        out.kind = out.scope = out.writers = 99;
        out.rangeMin = out.rangeMax = out.defaultValue = out.currentValue = 99.0f;
        out.hasEmitterOverride = out.pendingCount = 99;
        out.executionFingerprint = 99;
        EXPECT_EQ(VN_STALE_HANDLE, Vn_QueryVariable(&desc, &out));
        EXPECT_EQ(sizeof(out), out.structSize);
        EXPECT_EQ(0u, out.kind);
        EXPECT_EQ(0u, out.scope);
        EXPECT_EQ(0u, out.writers);
        EXPECT_FLOAT_EQ(0.0f, out.rangeMin);
        EXPECT_FLOAT_EQ(0.0f, out.rangeMax);
        EXPECT_FLOAT_EQ(0.0f, out.defaultValue);
        EXPECT_FLOAT_EQ(0.0f, out.currentValue);
        EXPECT_EQ(0u, out.hasEmitterOverride);
        EXPECT_EQ(0u, out.pendingCount);
        EXPECT_EQ(0u, out.executionFingerprint);
    }
}

VnVariableQueryResultV1 QueryGlobal(VnVariableId id) {
    VnVariableQueryDescV1 desc{};
    desc.structSize = sizeof(desc);
    desc.variableId = id;
    VnVariableQueryResultV1 out{};
    out.structSize = sizeof(out);
    EXPECT_EQ(VN_OK, Vn_QueryVariable(&desc, &out));
    return out;
}

TEST(VariableQuery, PreparedCurrentWorldImageAdmitsGameAndActionWriters) {
    auto definition = MakeVariableDefinition(
        harkentest::ObjectId(11), harken::VariableKindV1::Smooth, -1.0f, 1.0f, 0.25f);
    VnVariableQueryResultV1 liveQuery{};
    {
        EngineSession live(MakeNullConfig());
        ASSERT_EQ(VN_OK, live.InitResult());
        ASSERT_EQ(harken::WorldInstallStatus::Ok,
                  InstallDefinitionsWorld(live.Engine(), &definition, 1).status);
        liveQuery = QueryGlobal(harkentest::ObjectId(11));
        EXPECT_EQ(3u, liveQuery.writers);
        EXPECT_EQ(0u, liveQuery.executionFingerprint);
        EXPECT_FLOAT_EQ(0.25f, liveQuery.defaultValue);
        EXPECT_FLOAT_EQ(0.25f, liveQuery.currentValue);
    }

    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    harken::ContainerWorldInstallViewV1 world{};
    world.variables = &definition;
    world.variableCount = 1;
    world.identity.capabilityMask = harken::kWorldCapabilityVariableDefinitions;
    world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(world);

    harken::VariableServiceV1::PreparedCurrentWorldImageV1 prepared;
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              engine->variables.PrepareCurrentWorldImageV1(world, prepared));
    EXPECT_TRUE(prepared.Valid());
    EXPECT_FALSE(prepared.Idempotent());
    const harken::WorldGeneration generation =
        engine->variables.CommitPreparedCurrentWorldImageV1(prepared);
    EXPECT_NE(generation, 0u);
    engine->variables.ProjectCurrentWorld(engine->params, engine->emitters,
                                          engine->bindings, engine->busTargets);

    const auto queried = QueryGlobal(harkentest::ObjectId(11));
    EXPECT_EQ(liveQuery.writers, queried.writers);
    EXPECT_EQ(liveQuery.executionFingerprint, queried.executionFingerprint);
    EXPECT_FLOAT_EQ(liveQuery.defaultValue, queried.defaultValue);
    EXPECT_FLOAT_EQ(liveQuery.currentValue, queried.currentValue);

    const auto observation = harken::ObserveVariableCallAt(
        engine->variables.CurrentWorldGeneration(),
        engine->variables.CurrentTransportEpoch(), engine->config.blockFrames, 0,
        0);
    harken::PreparedVariableWriteToken gameWrite;
    ASSERT_EQ(VN_OK, engine->variables
                         .PrepareGlobalWrite(harkentest::ObjectId(11), 0.5f, observation, 0,
                                             &gameWrite,
                                             harken::VariableWriterIdentityV1::Game)
                         .result);
    engine->variables.CancelPreparedWrite(&gameWrite);
    harken::PreparedVariableWriteToken actionWrite;
    ASSERT_EQ(VN_OK,
              engine->variables
                  .PrepareGlobalWrite(harkentest::ObjectId(11), 0.75f, observation, 0, &actionWrite,
                                      harken::VariableWriterIdentityV1::Action)
                  .result);
    engine->variables.CancelPreparedWrite(&actionWrite);
}

} // namespace
