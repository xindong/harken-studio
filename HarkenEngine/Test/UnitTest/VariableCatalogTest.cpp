#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include <harken/HarkenVariables.h>
#include "harken/core/VariableExecutionFingerprint.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using harkentest::EngineSession;
using harkentest::InstallSmoothDefinitions;
using harkentest::MakeNullConfig;

constexpr std::uint32_t kBlock = 64;
constexpr std::uint32_t kRate = 48000;

VnVariableBatchDescV1 OneWrite(VnVariableWriteV1* write, VnVariableId id,
                               float value) {
    *write = {};
    write->variableId = id;
    write->value = value;
    VnVariableBatchDescV1 desc{};
    desc.structSize = sizeof(desc);
    desc.writeCount = 1;
    desc.writes = write;
    return desc;
}

std::uint64_t VehicleRpmExecutionFingerprint() {
    std::uint64_t fingerprint = 0;
    EXPECT_TRUE(harken::ComputeVariableExecutionFingerprintV1(
        harken::generated::kVariable_VehicleRPM, 1, 1, 3, 0.0f, 1.0f, 0.0f,
        nullptr, 0, &fingerprint));
    return fingerprint;
}

} // namespace

TEST(VariableCatalog, GeneratedConstantsMatchArtifact) {
    const std::uint64_t execution = VehicleRpmExecutionFingerprint();
    harken::VariableCatalogFingerprintRowV1 row{};
    row.variableId = harken::generated::kVariable_VehicleRPM;
    row.fingerprint = execution;
    EXPECT_EQ((VnObjectId{0x67e0c65bf36e40d9, 0x80ee75af00ecf712}),
              harken::generated::kVariable_VehicleRPM);
    EXPECT_NE(0u, execution);
    EXPECT_EQ(harken::generated::kVariableExecutionCatalogFingerprint,
              harken::ComputeVariableExecutionCatalogFingerprintV1(&row, 1));
    EXPECT_NE(0u, harken::generated::kVariableCatalogFingerprint);
}

TEST(VariableCatalog, ExecutionMismatchRefusesBeforeWrite) {
    EngineSession session(MakeNullConfig(8, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallSmoothDefinitions(
                  engine, {harken::generated::kVariable_VehicleRPM}, 0.0f, 1.0f)
                  .status);

    VnVariableWriteV1 write{};
    const VnVariableBatchDescV1 desc =
        OneWrite(&write, harken::generated::kVariable_VehicleRPM, 1.0f);
    VnVariableTicketV1 ticket{};
    ASSERT_EQ(VN_VARIABLE_CATALOG_MISMATCH,
              harken::variables::SetValues(&desc, &ticket));
    EXPECT_EQ(0u, ticket.sequence);
    EXPECT_EQ(0u, engine->variables.AcceptedCount());

    VnVariableQueryDescV1 query{};
    query.structSize = sizeof(query);
    query.variableId = harken::generated::kVariable_VehicleRPM;
    VnVariableQueryResultV1 current{};
    current.structSize = sizeof(current);
    ASSERT_EQ(VN_OK, Vn_QueryVariable(&query, &current));
    EXPECT_FLOAT_EQ(0.0f, current.currentValue);
}

TEST(VariableCatalog, DisplayMismatchWarnsAndStillWrites) {
    EngineSession session(MakeNullConfig(8, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallSmoothDefinitions(
                  engine, {harken::generated::kVariable_VehicleRPM}, 0.0f, 1.0f)
                  .status);
    ASSERT_TRUE(harken::test::SetDefinitionExecutionFingerprint(
        engine->variables, harken::generated::kVariable_VehicleRPM,
        VehicleRpmExecutionFingerprint()));
    harken::test::SetSdkCatalogFingerprint(engine->variables, 1);

    VnVariableWriteV1 write{};
    const VnVariableBatchDescV1 desc =
        OneWrite(&write, harken::generated::kVariable_VehicleRPM, 1.0f);
    VnVariableTicketV1 ticket{};
    ASSERT_EQ(VN_OK, harken::variables::SetValues(&desc, &ticket));
    EXPECT_NE(0u, ticket.sequence);
    static int warnings = 0;
    warnings = 0;
    EXPECT_EQ(VN_OK, harken::GuardTypedVariableCatalog(
        harken::generated::kVariableExecutionCatalogFingerprint,
        harken::generated::kVariableCatalogFingerprint,
        [](std::uint64_t, std::uint64_t) { ++warnings; }));
    EXPECT_EQ(1, warnings);
    EXPECT_EQ(1u, engine->variables.AcceptedCount());
}

TEST(VariableCatalog, QueryMatchesInstalledRows) {
    EngineSession session(MakeNullConfig(8, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallSmoothDefinitions(
                  engine, {harken::generated::kVariable_VehicleRPM}, 0.0f, 1.0f)
                  .status);
    ASSERT_TRUE(harken::test::SetDefinitionExecutionFingerprint(
        engine->variables, harken::generated::kVariable_VehicleRPM,
        VehicleRpmExecutionFingerprint()));
    harken::test::SetSdkCatalogFingerprint(engine->variables, 99);

    VnVariableCatalogInfoV1 info{};
    info.structSize = sizeof(info);
    ASSERT_EQ(VN_OK, Vn_QueryVariableCatalog(&info));
    EXPECT_EQ(harken::generated::kVariableExecutionCatalogFingerprint,
              info.executionCatalogFingerprint);
    EXPECT_EQ(99u, info.sdkCatalogFingerprint);
    EXPECT_EQ(engine->variables.SdkCatalogFingerprint(),
              info.sdkCatalogFingerprint);
}

TEST(VariableCatalog, UntypedSetValuesIgnoresGeneratedCatalog) {
    EngineSession session(MakeNullConfig(8, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallSmoothDefinitions(
                  engine, {harken::generated::kVariable_VehicleRPM}, 0.0f, 1.0f)
                  .status);

    VnVariableWriteV1 write{};
    const VnVariableBatchDescV1 desc =
        OneWrite(&write, harken::generated::kVariable_VehicleRPM, 1.0f);
    VnVariableTicketV1 ticket{};
    ASSERT_EQ(VN_OK, Vn_SetValues(&desc, &ticket));
    EXPECT_NE(0u, ticket.sequence);
    EXPECT_EQ(1u, engine->variables.AcceptedCount());
}

TEST(VariableCatalog, TypedWriteUsesPublishedExecutionContract) {
    EngineSession session(MakeNullConfig(8, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const auto definition = harkentest::MakeVariableDefinition(
        harken::generated::kVariable_VehicleRPM, harken::VariableKindV1::Smooth,
        0.0f, 1.0f, 0.0f);
    const harken::VariablePolicySidecarV1 policy{3, VehicleRpmExecutionFingerprint()};
    harken::VariableServiceV1::PreparedPackDefinitions prepared;
    ASSERT_EQ(VN_OK, engine->variables.PreparePackDefinitions(
        &definition, 1, &policy, harken::PackVariablePublicationReason::InitPublication,
        engine->serviceFrameClock, &prepared));
    engine->variables.CommitPreparedPackDefinitions(
        &prepared, engine->params, engine->emitters, engine->bindings, engine->busTargets);
    ASSERT_EQ(VN_VARIABLE_CATALOG_MISMATCH, harken::GuardTypedVariableCatalog(
        harken::generated::kVariableExecutionCatalogFingerprint ^ 1));
    EXPECT_EQ(0u, engine->variables.AcceptedCount());
    VnVariableWriteV1 write{};
    const auto batch = OneWrite(&write, definition.variableId, 1.0f);
    VnVariableTicketV1 ticket{};
    ASSERT_EQ(VN_OK, harken::variables::SetValues(&batch, &ticket));
    ASSERT_NE(0u, ticket.sequence);
    harkentest::AdvanceVariableWrite(engine);
    VnVariableTicketQueryDescV1 ticketQuery{};
    ticketQuery.structSize = sizeof(ticketQuery);
    ticketQuery.ticket = ticket;
    VnVariableTicketQueryResultV1 outcome{};
    outcome.structSize = sizeof(outcome);
    ASSERT_EQ(VN_OK, harken::variables::QueryVariableTicket(&ticketQuery, &outcome));
    ASSERT_EQ(VN_VARIABLE_TICKET_FOUND, outcome.status);
    ASSERT_EQ(VN_VARIABLE_OUTCOME_APPLIED, outcome.outcome);
    VnVariableQueryDescV1 query{};
    query.structSize = sizeof(query);
    query.variableId = definition.variableId;
    VnVariableQueryResultV1 result{};
    result.structSize = sizeof(result);
    ASSERT_EQ(VN_OK, harken::variables::QueryVariable(&query, &result));
    EXPECT_FLOAT_EQ(1.0f, result.currentValue);
}

TEST(VariableCatalog, TypedLabelWritesGuardGlobalAndEmitterMutations) {
    EngineSession session(MakeNullConfig(8, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    const auto variable = harken::generated::kVariable_VehicleRPM;
    const auto labelNone = harkentest::ObjectId(721);
    const auto labelActive = harkentest::ObjectId(722);
    const auto definition = harkentest::MakeVariableDefinition(
        variable, harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.0f);
    const harken::VariableLabelDefinitionV1 labels[] = {
        {0, "None", labelNone}, {1, "Active", labelActive}};
    const auto execution = VehicleRpmExecutionFingerprint();
    harken::VariablePolicySidecarV1 policy{3, execution};
    policy.labels = labels;
    policy.labelCount = 2;
    harken::VariableServiceV1::PreparedPackDefinitions prepared;
    ASSERT_EQ(VN_OK, engine->variables.PreparePackDefinitions(
        &definition, 1, &policy, harken::PackVariablePublicationReason::InitPublication,
        engine->serviceFrameClock, &prepared));
    engine->variables.CommitPreparedPackDefinitions(
        &prepared, engine->params, engine->emitters, engine->bindings, engine->busTargets);
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_EQ(VN_OK, harken::variables::SetValue(variable, labelActive));
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(engine));
    ASSERT_EQ(VN_OK, harken::variables::SetValue(variable, "None", emitter));
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(engine));
    ASSERT_TRUE(harken::test::SetDefinitionExecutionFingerprint(
        engine->variables, variable, execution ^ 1));
    const auto accepted = engine->variables.AcceptedCount();
    VnVariableQueryDescV1 query{};
    query.structSize = sizeof(query);
    query.variableId = variable;
    VnVariableQueryResultV1 current{};
    current.structSize = sizeof(current);
    for (const VnEmitterHandle target : {VnEmitterHandle{0}, emitter}) {
        EXPECT_EQ(VN_VARIABLE_CATALOG_MISMATCH,
                  harken::variables::SetValue(variable, labelNone, target));
        EXPECT_EQ(VN_VARIABLE_CATALOG_MISMATCH,
                  harken::variables::SetValue(variable, "Active", target));
        query.emitter = target;
        ASSERT_EQ(VN_OK, Vn_QueryVariable(&query, &current));
        EXPECT_FLOAT_EQ(target == 0 ? 1.0f : 0.0f, current.currentValue);
    }
    EXPECT_EQ(accepted, engine->variables.AcceptedCount());
    ASSERT_TRUE(harken::test::SetDefinitionExecutionFingerprint(
        engine->variables, variable, execution));
    ASSERT_EQ(VN_OK, harken::variables::SetValue(variable, "None"));
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(engine));
    ASSERT_EQ(VN_OK, harken::variables::SetValue(variable, labelActive, emitter));
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(engine));
    for (const VnEmitterHandle target : {VnEmitterHandle{0}, emitter}) {
        query.emitter = target;
        ASSERT_EQ(VN_OK, Vn_QueryVariable(&query, &current));
        EXPECT_FLOAT_EQ(target == 0 ? 0.0f : 1.0f, current.currentValue);
    }
}

TEST(VariableCatalog, LabelWritesResolveCurrentIdentityAndRetainGlobalEmitterSemantics) {
    EngineSession session(MakeNullConfig(8, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    const auto variable = harkentest::ObjectId(720);
    const auto labelA = harkentest::ObjectId(721);
    const auto labelB = harkentest::ObjectId(722);
    const auto definition = harkentest::MakeVariableDefinition(
        variable, harken::VariableKindV1::Smooth, 0.0f, 3.0f, 0.0f);
    std::vector<harken::VariableLabelDefinitionV1> labels{{1, "AAA", labelA}, {2, "BBB", labelB}};
    const auto publish = [&] {
        harken::VariablePolicySidecarV1 policy;
        policy.writers = 3;
        EXPECT_TRUE(harken::ComputeVariableExecutionFingerprintV1(variable, 1, 1, 3,
            0.0f, 3.0f, 0.0f, nullptr, 0, &policy.executionFingerprint));
        policy.catalogFingerprint = 77;
        policy.labels = labels.data();
        policy.labelCount = static_cast<std::uint32_t>(labels.size());
        harken::VariableServiceV1::PreparedPackDefinitions prepared;
        ASSERT_EQ(VN_OK, engine->variables.PreparePackDefinitions(&definition, 1, &policy,
            harken::PackVariablePublicationReason::ContentMutation, engine->serviceFrameClock, &prepared));
        engine->variables.CommitPreparedPackDefinitions(&prepared, engine->params, engine->emitters,
            engine->bindings, engine->busTargets);
    };
    ASSERT_NO_FATAL_FAILURE(publish());
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_EQ(VN_OK, Vn_SetValueByLabelName(variable, "AAA", 0));
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(engine));
    ASSERT_EQ(VN_OK, Vn_SetValueByLabelId(variable, labelB, emitter));
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(engine));
    ASSERT_EQ(VN_OK, Vn_SetValueByLabelId(variable, labelA, 0));
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(engine));
    VnVariableQueryDescV1 query{};
    query.structSize = sizeof(query);
    query.variableId = variable;
    VnVariableQueryResultV1 current{};
    current.structSize = sizeof(current);
    ASSERT_EQ(VN_OK, Vn_QueryVariable(&query, &current));
    EXPECT_FLOAT_EQ(current.currentValue, 1.0f);
    query.emitter = emitter;
    ASSERT_EQ(VN_OK, Vn_QueryVariable(&query, &current));
    EXPECT_FLOAT_EQ(current.currentValue, 1.0f);
    std::swap(labels[0].anchor, labels[1].anchor);
    labels[0].name = "Renamed";
    ASSERT_NO_FATAL_FAILURE(publish());
    ASSERT_EQ(VN_OK, Vn_QueryVariable(&query, &current));
    EXPECT_FLOAT_EQ(current.currentValue, 1.0f);
    EXPECT_EQ(VN_NO_VARIABLE_LABEL, Vn_SetValueByLabelName(variable, "AAA", 0));
    EXPECT_EQ(VN_NO_VARIABLE_LABEL, Vn_SetValueByLabelName(variable, "2", 0));
    EXPECT_EQ(VN_NO_VARIABLE_LABEL, Vn_SetValueByLabelName(variable, "renamed", 0));
    ASSERT_EQ(VN_OK, Vn_SetValueByLabelId(variable, labelA, 0));
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(engine));
    ASSERT_EQ(VN_OK, Vn_QueryVariable(&query, &current));
    EXPECT_FLOAT_EQ(current.currentValue, 2.0f);
    labels.erase(labels.begin());
    ASSERT_NO_FATAL_FAILURE(publish());
    EXPECT_EQ(VN_NO_VARIABLE_LABEL, Vn_SetValueByLabelId(variable, labelA, 0));
    EXPECT_EQ(VN_NO_VARIABLE_LABEL, Vn_SetValueByLabelName(variable, "Renamed", 0));
}
