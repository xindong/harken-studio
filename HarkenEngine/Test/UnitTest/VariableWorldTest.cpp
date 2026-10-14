// Current definitions-only Variable world authority: atomic install with
// identity and idempotence, typed refusals with zero side effects,
// installed-definition-only Step/Smooth write validation, Global default
// banks with exact emitter overrides, idempotent Clear, durable accepted
// rows reaching Applied/Superseded/termination outcomes, and capability
// rejection of any Container program.
#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "Common/TestUtil.h"
#include "harken/param/VariableService.h"
#include "harken/core/LogicAllocCheckpoint.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <vector>

namespace {

using harkentest::EngineSession;
using harkentest::InstallDefinitionsWorld;
using harkentest::MakeDC;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::MakeVariableDefinition;

constexpr VnVariableId kStepVar = harkentest::ObjectId(501);
constexpr VnVariableId kSmoothVar = harkentest::ObjectId(502);

std::vector<harken::VariableDefinitionV1> DefaultDefinitions() {
    return {MakeVariableDefinition(kStepVar, harken::VariableKindV1::Step,
                                   0.0f, 8.0f, 2.0f),
            MakeVariableDefinition(kSmoothVar,
                                   harken::VariableKindV1::Smooth, -24.0f,
                                   6.0f, 0.0f)};
}

class VariablePublication : public ::testing::Test {
protected:
    EngineSession session{MakeNullConfig()};
    std::vector<harken::VariableDefinitionV1> definitions = DefaultDefinitions();
    using Token = harken::VariableServiceV1::PreparedPackDefinitions;
    using Reason = harken::PackVariablePublicationReason;

    void SetUp() override {
        ASSERT_EQ(VN_OK, session.InitResult());
        Token initial;
        ASSERT_EQ(VN_OK, Prepare(Reason::InitPublication, &initial));
        Commit(&initial);
        EXPECT_EQ(1u, session.Engine()->variables.CurrentWorldGeneration());
    }
    VnResult Prepare(Reason reason, Token* out) {
        return session.Engine()->variables.PreparePackDefinitions(
            definitions.data(), static_cast<std::uint32_t>(definitions.size()), reason, 1234, out);
    }
    void Commit(Token* token) {
        auto& e = *session.Engine();
        e.variables.CommitPreparedPackDefinitions(token, e.params, e.emitters, e.bindings, e.busTargets);
    }
    void ExpectValue(VnEmitterHandle emitter, VnVariableId id, float expected) {
        float value = 99;
        ASSERT_TRUE(session.Engine()->variables.CurrentValue(emitter, id, &value));
        EXPECT_FLOAT_EQ(expected, value);
    }
};

TEST_F(VariablePublication, CompatibleInitTerminatesCompleteOldWriteSet) {
    auto& e = *session.Engine();
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_EQ(VN_OK, Vn_SetValue(kSmoothVar, -3));
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(&e));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, kSmoothVar, -6));
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(&e));
    const auto world = e.variables.CurrentWorldGeneration();
    const auto transport = e.variables.CurrentTransportEpoch();
    ASSERT_EQ(VN_OK, Vn_SetValue(kSmoothVar, -9));
    const auto firstTicket = e.logicCommits.IssuedCount();
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, kSmoothVar, -12));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, kStepVar, 3));
    Token token;
    ASSERT_EQ(VN_OK, Prepare(Reason::InitPublication, &token));
    EXPECT_EQ(3u, e.variables.PendingCount());
    EXPECT_EQ(world, e.variables.CurrentWorldGeneration());
    Token moved(std::move(token));
    EXPECT_FALSE(token.IsPrepared());
    Commit(&moved);
    EXPECT_FALSE(moved.IsPrepared());
    EXPECT_EQ(world + 1, e.variables.CurrentWorldGeneration());
    EXPECT_EQ(0u, e.variables.PendingCount());
    ExpectValue(0, kSmoothVar, -3);
    ExpectValue(emitter, kSmoothVar, -6);
    ExpectValue(emitter, kStepVar, 2);
    std::uint32_t seen = 0;
    harken::VariableOutcomeRecord record{};
    while (e.variables.PopOutcome(&record)) {
        if (record.acceptedTicket.sequence < firstTicket) continue;
        EXPECT_EQ(firstTicket + seen, record.acceptedTicket.sequence);
        EXPECT_EQ(harken::VariableOutcomeKind::WorldReplacement, record.outcome);
        EXPECT_EQ(world, record.world);
        EXPECT_EQ(transport, record.transport);
        EXPECT_EQ(1234u, record.actualFrame);
        ++seen;
    }
    EXPECT_EQ(3u, seen);
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(&e));
    ExpectValue(0, kSmoothVar, -3);
    ExpectValue(emitter, kSmoothVar, -6);
    EXPECT_EQ(3u, e.variables.TerminationCount());
}

TEST_F(VariablePublication, CancelAndExclusiveMoveLeavePendingWritesUntouched) {
    auto& e = *session.Engine();
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_EQ(VN_OK, Vn_SetValue(kSmoothVar, -9));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, kSmoothVar, -12));
    const auto world = e.variables.CurrentWorldGeneration();
    {
        Token first;
        ASSERT_EQ(VN_OK, Prepare(Reason::InitPublication, &first));
        Token second(std::move(first));
        EXPECT_FALSE(first.IsPrepared());
        EXPECT_TRUE(second.IsPrepared());
        Token third;
        ASSERT_EQ(VN_OK, Prepare(Reason::InitPublication, &third));
        third = std::move(second);
        EXPECT_FALSE(second.IsPrepared());
        EXPECT_TRUE(third.IsPrepared());
        third = Token{};
        EXPECT_FALSE(third.IsPrepared());
    }
    EXPECT_EQ(world, e.variables.CurrentWorldGeneration());
    EXPECT_EQ(2u, e.variables.PendingCount());
    EXPECT_EQ(0u, e.variables.TerminationCount());
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(&e));
    ExpectValue(0, kSmoothVar, -9);
    ExpectValue(emitter, kSmoothVar, -12);
}

TEST_F(VariablePublication, ContentMutationRetainsWorldAndPendingWrites) {
    auto& e = *session.Engine();
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_EQ(VN_OK, Vn_SetValue(kSmoothVar, -9));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, kSmoothVar, -12));
    const auto world = e.variables.CurrentWorldGeneration();
    Token token;
    ASSERT_EQ(VN_OK, Prepare(Reason::ContentMutation, &token));
    Commit(&token);
    EXPECT_EQ(world, e.variables.CurrentWorldGeneration());
    EXPECT_EQ(2u, e.variables.PendingCount());
    EXPECT_EQ(0u, e.variables.TerminationCount());
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(&e));
    ExpectValue(0, kSmoothVar, -9);
    ExpectValue(emitter, kSmoothVar, -12);
}

TEST_F(VariablePublication, ChangedDefinitionsAndUnloadAdvanceOnlyOnCommit) {
    auto& e = *session.Engine();
    ASSERT_EQ(VN_OK, Vn_SetValue(kSmoothVar, -3));
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(&e));
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, kSmoothVar, -6));
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(&e));
    definitions.erase(definitions.begin());
    definitions[0].rangeMin = -48;
    definitions[0].defaultValue = -1;
    Token token;
    ASSERT_EQ(VN_OK, Prepare(Reason::InitPublication, &token));
    ExpectValue(emitter, kSmoothVar, -6);
    Commit(&token);
    ExpectValue(0, kSmoothVar, -1);
    ExpectValue(emitter, kSmoothVar, -1);
    EXPECT_EQ(nullptr, e.variables.Definition(kStepVar));
    EXPECT_EQ(2u, e.variables.CurrentWorldGeneration());
    ASSERT_EQ(VN_OK, Vn_SetValue(kSmoothVar, -9));
    definitions.clear();
    ASSERT_EQ(VN_OK, Prepare(Reason::InitUnload, &token));
    EXPECT_EQ(2u, e.variables.CurrentWorldGeneration());
    Commit(&token);
    EXPECT_EQ(3u, e.variables.CurrentWorldGeneration());
    EXPECT_EQ(0u, e.variables.DefinitionCount());
    EXPECT_EQ(0u, e.variables.PendingCount());
    EXPECT_EQ(VN_NO_VARIABLE, Vn_SetValue(kSmoothVar, -1));
    definitions = DefaultDefinitions();
    ASSERT_EQ(VN_OK, Prepare(Reason::InitPublication, &token));
    Commit(&token);
    EXPECT_EQ(4u, e.variables.CurrentWorldGeneration());
    ExpectValue(0, kSmoothVar, 0);
}

TEST_F(VariablePublication, InvalidInputsAndClosedAdmissionPreserveLiveState) {
    auto& service = session.Engine()->variables;
    ASSERT_EQ(VN_OK, Vn_SetValue(kSmoothVar, -9));
    Token token;
    EXPECT_EQ(VN_INVALID_ARG, Prepare(Reason::InitPublication, nullptr));
    EXPECT_EQ(VN_INVALID_ARG, service.PreparePackDefinitions(nullptr, 1, Reason::InitPublication, 0, &token));
    EXPECT_EQ(VN_INVALID_ARG, Prepare(static_cast<Reason>(255), &token));
    EXPECT_EQ(VN_INVALID_ARG, Prepare(Reason::InitUnload, &token));
    EXPECT_EQ(VN_PACK_RESIDENT_CAPACITY, service.PreparePackDefinitions(
        definitions.data(), harken::kVariableDefinitionCapacity + 1, Reason::InitPublication, 0, &token));
    auto invalid = definitions;
    invalid[1].variableId = invalid[0].variableId;
    EXPECT_EQ(VN_BAD_PACK, service.PreparePackDefinitions(invalid.data(), 2, Reason::InitPublication, 0, &token));
    invalid = definitions;
    invalid[1].defaultValue = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(VN_BAD_PACK, service.PreparePackDefinitions(invalid.data(), 2, Reason::InitPublication, 0, &token));
    service.SetEngineAdmissionClosed(true);
    EXPECT_EQ(VN_BACKPRESSURE, Prepare(Reason::InitPublication, &token));
    service.SetEngineAdmissionClosed(false);
    EXPECT_FALSE(token.IsPrepared());
    EXPECT_EQ(1u, service.CurrentWorldGeneration());
    EXPECT_EQ(1u, service.PendingCount());
    EXPECT_EQ(0u, service.TerminationCount());
    ExpectValue(0, kSmoothVar, 0);
}

TEST_F(VariablePublication, AllocationFailureDiscardsOnlyCandidateStorage) {
    auto& e = *session.Engine();
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_EQ(VN_OK, Vn_SetValue(kSmoothVar, -9));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, kSmoothVar, -12));
    for (std::size_t nth = 1; nth <= 5; ++nth) {
        SCOPED_TRACE(nth);
        Token token;
        harken::detail::ArmLogicAllocFaultForTests(nth, harken::detail::LogicAllocSite::PackVariablePublication);
        EXPECT_THROW(Prepare(Reason::InitPublication, &token), std::bad_alloc);
        harken::detail::DisarmLogicAllocFaultForTests();
        EXPECT_FALSE(token.IsPrepared());
        EXPECT_EQ(1u, e.variables.CurrentWorldGeneration());
        EXPECT_EQ(2u, e.variables.PendingCount());
        EXPECT_EQ(0u, e.variables.TerminationCount());
        ExpectValue(0, kSmoothVar, 0);
        ExpectValue(emitter, kSmoothVar, 0);
    }
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(&e));
    ExpectValue(0, kSmoothVar, -9);
    ExpectValue(emitter, kSmoothVar, -12);
}

TEST_F(VariablePublication, PendingGlobalWriteIsCancelledWithoutErasingAppliedOverride) {
    auto& e = *session.Engine();
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, kSmoothVar, -6));
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(&e));
    ASSERT_EQ(VN_OK, Vn_SetValue(kSmoothVar, 0));
    Token token;
    ASSERT_EQ(VN_OK, Prepare(Reason::InitPublication, &token));
    Commit(&token);
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(&e));
    ExpectValue(emitter, kSmoothVar, -6);
    EXPECT_EQ(1u, e.variables.TerminationCount());
}

TEST(VariableWorld, InstallIsAtomicIdempotentAndBehaviorReplacing) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const auto definitions = DefaultDefinitions();
    const harken::WorldInstallResult first = InstallDefinitionsWorld(
        engine, definitions.data(),
        static_cast<std::uint32_t>(definitions.size()));
    ASSERT_EQ(harken::WorldInstallStatus::Ok, first.status);
    EXPECT_EQ(0u, first.priorGeneration);
    EXPECT_EQ(1u, first.resultGeneration);
    EXPECT_FALSE(first.idempotent);

    // Same semantic body: idempotent success keeps the generation and every
    // live bank untouched.
    ASSERT_EQ(VN_OK, Vn_SetValue(kSmoothVar, -3.0f));
    harkentest::AdvanceVariableWrite(engine);
    const harken::WorldInstallResult again = InstallDefinitionsWorld(
        engine, definitions.data(),
        static_cast<std::uint32_t>(definitions.size()));
    ASSERT_EQ(harken::WorldInstallStatus::Ok, again.status);
    EXPECT_TRUE(again.idempotent);
    EXPECT_EQ(first.resultGeneration, again.priorGeneration);
    EXPECT_EQ(first.resultGeneration, again.resultGeneration);
    float value = 0.0f;
    ASSERT_TRUE(engine->variables.CurrentValue(kSmoothVar, &value));
    EXPECT_FLOAT_EQ(-3.0f, value);

    // A behavior change (range move) mints a new generation and restarts the
    // changed definition at its new default while keeping the unchanged one.
    auto changed = definitions;
    changed[1].rangeMin = -48.0f;
    const harken::WorldInstallResult replaced = InstallDefinitionsWorld(
        engine, changed.data(),
        static_cast<std::uint32_t>(changed.size()), 0x5445535458u);
    ASSERT_EQ(harken::WorldInstallStatus::Ok, replaced.status);
    EXPECT_FALSE(replaced.idempotent);
    EXPECT_EQ(first.resultGeneration, replaced.priorGeneration);
    EXPECT_EQ(first.resultGeneration + 1u, replaced.resultGeneration);
    ASSERT_TRUE(engine->variables.CurrentValue(kSmoothVar, &value));
    EXPECT_FLOAT_EQ(changed[1].defaultValue, value);
    ASSERT_TRUE(engine->variables.CurrentValue(kStepVar, &value));
    EXPECT_FLOAT_EQ(2.0f, value);
}

TEST(VariableWorld, StaticImpactIsDerivedAndExcludedFromSemanticIdentity) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    harken::VariableDefinitionV1 definition = MakeVariableDefinition(
        harkentest::ObjectId(503), harken::VariableKindV1::Smooth, -1.0f, 1.0f, 0.0f);
    definition.impact.propertiesPerConsumer =
        std::numeric_limits<std::uint32_t>::max();
    definition.impact.discreteSideReferences = 1;
    definition.impact.continuousSideReferences = 2;
    definition.impact.transactionsPerConsumer = 3;
    definition.impact.eventsPerConsumer = 4;
    definition.impact.participantsPerConsumer = 5;
    definition.impact.targetRowsPerConsumer = 6;
    definition.impact.referencedFlags = 7;

    const harken::WorldInstallResult first =
        InstallDefinitionsWorld(engine, &definition, 1);
    ASSERT_EQ(harken::WorldInstallStatus::Ok, first.status);
    ASSERT_EQ(1u, engine->variables.DefinitionCount());
    const harken::StaticImpactSummary stored =
        engine->variables.DefinitionAt(0).impact;
    EXPECT_EQ(0u, stored.propertiesPerConsumer);
    EXPECT_EQ(0u, stored.discreteSideReferences);
    EXPECT_EQ(0u, stored.continuousSideReferences);
    EXPECT_EQ(0u, stored.transactionsPerConsumer);
    EXPECT_EQ(0u, stored.eventsPerConsumer);
    EXPECT_EQ(0u, stored.participantsPerConsumer);
    EXPECT_EQ(0u, stored.targetRowsPerConsumer);
    EXPECT_EQ(0u, stored.referencedFlags);

    definition.impact = harken::StaticImpactSummary{};
    definition.impact.propertiesPerConsumer = 13;
    definition.impact.referencedFlags = 0xFFFFFFFFu;
    const harken::WorldInstallResult again =
        InstallDefinitionsWorld(engine, &definition, 1);
    EXPECT_EQ(harken::WorldInstallStatus::Ok, again.status);
    EXPECT_TRUE(again.idempotent);
    EXPECT_EQ(first.resultGeneration, again.resultGeneration);
    EXPECT_EQ(0u,
              engine->variables.DefinitionAt(0)
                  .impact.propertiesPerConsumer);
    EXPECT_EQ(0u,
              engine->variables.DefinitionAt(0).impact.referencedFlags);
}

TEST(VariableWorld, InvalidWorldsRefuseWithZeroSideEffects) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    // Unknown format vocabulary refuses as InvalidFormat.
    {
        auto definitions = DefaultDefinitions();
        harken::ContainerWorldInstallViewV1 world{};
        world.identity.format.schema = 2;
        world.variables = definitions.data();
        world.variableCount =
            static_cast<std::uint32_t>(definitions.size());
        EXPECT_EQ(harken::WorldInstallStatus::InvalidFormat,
                  harken::InstallContainerWorldV1(*engine, world).status);
    }
    // A nonzero typed row count with no row storage is an invalid world shape.
    {
        auto definitions = DefaultDefinitions();
        harken::ContainerWorldInstallViewV1 world{};
        world.variables = definitions.data();
        world.variableCount =
            static_cast<std::uint32_t>(definitions.size());
        world.containerCount = 1;
        EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
                  harken::InstallContainerWorldV1(*engine, world).status);
    }
    // Invalid definitions refuse the whole world: reversed range, non-finite
    // default, out-of-range default, non-integer Step default, duplicate id,
    // zero id.
    const auto refuse = [&](harken::VariableDefinitionV1 bad) {
        auto definitions = DefaultDefinitions();
        definitions.push_back(bad);
        EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
                  InstallDefinitionsWorld(
                      engine, definitions.data(),
                      static_cast<std::uint32_t>(definitions.size()))
                      .status);
    };
    refuse(MakeVariableDefinition(harkentest::ObjectId(600), harken::VariableKindV1::Smooth, 4.0f,
                                  -4.0f, 0.0f));
    refuse(MakeVariableDefinition(
        harkentest::ObjectId(601), harken::VariableKindV1::Smooth, 0.0f, 1.0f,
        std::numeric_limits<float>::quiet_NaN()));
    refuse(MakeVariableDefinition(harkentest::ObjectId(602), harken::VariableKindV1::Smooth, 0.0f,
                                  1.0f, 2.0f));
    refuse(MakeVariableDefinition(harkentest::ObjectId(603), harken::VariableKindV1::Step, 0.0f,
                                  4.0f, 1.5f));
    refuse(MakeVariableDefinition(kStepVar, harken::VariableKindV1::Smooth,
                                  0.0f, 1.0f, 0.0f));
    refuse(MakeVariableDefinition(harkentest::ObjectId(0), harken::VariableKindV1::Smooth, 0.0f,
                                  1.0f, 0.0f));
    // Every refusal above left no world installed.
    EXPECT_EQ(0u, engine->variables.CurrentWorldGeneration());
    EXPECT_EQ(VN_NO_VARIABLE, Vn_SetValue(kStepVar, 1.0f));
}

TEST(VariableWorld, SetValidatesAgainstInstalledDefinitionsOnly) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const auto definitions = DefaultDefinitions();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallDefinitionsWorld(
                  engine, definitions.data(),
                  static_cast<std::uint32_t>(definitions.size()))
                  .status);

    // Unknown ids are never implicitly created.
    EXPECT_EQ(VN_NO_VARIABLE, Vn_SetValue(harkentest::ObjectId(999), 1.0f));
    // Step: fraction, out of range, non-finite all refuse; exact in-range
    // integers (bounds included) accept.
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetValue(kStepVar, 2.5f));
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetValue(kStepVar, -1.0f));
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetValue(kStepVar, 9.0f));
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetValue(kStepVar,
                          std::numeric_limits<float>::infinity()));
    EXPECT_EQ(VN_OK, Vn_SetValue(kStepVar, 0.0f));
    EXPECT_EQ(VN_OK, Vn_SetValue(kStepVar, 8.0f));
    // Smooth: finite in-range values accept; outside range and NaN refuse.
    EXPECT_EQ(VN_OK, Vn_SetValue(kSmoothVar, -24.0f));
    EXPECT_EQ(VN_OK, Vn_SetValue(kSmoothVar, 6.0f));
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetValue(kSmoothVar, 6.5f));
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetValue(kSmoothVar,
                          std::numeric_limits<float>::quiet_NaN()));

    // The refused writes accepted no row.
    const std::uint64_t accepted = engine->variables.AcceptedCount();
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetValue(kSmoothVar, 100.0f));
    EXPECT_EQ(accepted, engine->variables.AcceptedCount());
}

TEST(VariableWorld, FractionalStepRangeAcceptsOnlyIntegerValues) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const harken::VariableDefinitionV1 definition = MakeVariableDefinition(
        harkentest::ObjectId(701), harken::VariableKindV1::Step, 0.2f, 2.8f, 1.0f);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallDefinitionsWorld(engine, &definition, 1).status);
    EXPECT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(701), 1.0f));
    EXPECT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(701), 2.0f));
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetValue(harkentest::ObjectId(701), 0.0f));
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetValue(harkentest::ObjectId(701), 3.0f));
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetValue(harkentest::ObjectId(701), 1.5f));
}

TEST(VariableWorld, ReorderedWorldRefusesWithoutMutation) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const auto definitions = DefaultDefinitions();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallDefinitionsWorld(engine, definitions.data(),
                                      definitions.size()).status);
    const std::uint64_t generation =
        engine->variables.CurrentWorldGeneration();
    auto reordered = definitions;
    std::swap(reordered[0], reordered[1]);
    const harken::WorldInstallResult result = InstallDefinitionsWorld(
        engine, reordered.data(), reordered.size(), 0x52454F52444552u);
    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld, result.status);
    EXPECT_EQ(generation, engine->variables.CurrentWorldGeneration());
    EXPECT_EQ(definitions.size(), engine->variables.DefinitionCount());
    EXPECT_EQ(kStepVar, engine->variables.DefinitionAt(0).variableId);
    EXPECT_EQ(kSmoothVar, engine->variables.DefinitionAt(1).variableId);
}

TEST(VariableWorld, EqualGlobalWriteRemovesEmitterOverride) {
    VnConfig config = MakeNullConfig();
    config.maxEmitters = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const auto definitions = DefaultDefinitions();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallDefinitionsWorld(
                  engine, definitions.data(),
                  static_cast<std::uint32_t>(definitions.size()))
                  .status);
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));

    // Emitter writes validate against the same installed definition.
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetEmitterValue(emitter, kStepVar, 2.5f));
    EXPECT_EQ(VN_NO_VARIABLE, Vn_SetEmitterValue(emitter, harkentest::ObjectId(999), 1.0f));
    ASSERT_EQ(VN_OK, Vn_SetValue(kSmoothVar, -12.0f));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, kSmoothVar, -3.0f));
    harkentest::AdvanceVariableWrite(engine);

    // Resolution order: exact emitter override, then Global.
    float value = 0.0f;
    ASSERT_TRUE(engine->variables.CurrentValue(emitter, kSmoothVar, &value));
    EXPECT_FLOAT_EQ(-3.0f, value);
    ASSERT_TRUE(engine->variables.CurrentValue(kSmoothVar, &value));
    EXPECT_FLOAT_EQ(-12.0f, value);

    const std::uint64_t appliedBefore = engine->variables.AppliedCount();
    ASSERT_EQ(VN_OK, Vn_SetValue(kSmoothVar, -12.0f));
    harkentest::AdvanceVariableWrite(engine);
    ASSERT_EQ(VN_OK, Vn_SetValue(kSmoothVar, -12.0f));
    harkentest::AdvanceVariableWrite(engine);
    ASSERT_TRUE(engine->variables.CurrentValue(emitter, kSmoothVar, &value));
    EXPECT_FLOAT_EQ(-12.0f, value);
    EXPECT_GE(engine->variables.AppliedCount(), appliedBefore + 2u);
    // Stale emitter handles refuse.
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(emitter));
    EXPECT_EQ(VN_STALE_HANDLE,
              Vn_SetEmitterValue(emitter, kSmoothVar, -1.0f));
}

TEST(VariableWorld, EmitterBanksAndOverrideSlotsHonorResolvedCapacity) {
    {
        VnConfig config = MakeNullConfig();
        config.maxEmitters = 129;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        const harken::VariableDefinitionV1 definition = MakeVariableDefinition(
            harkentest::ObjectId(9001), harken::VariableKindV1::Smooth, 0.0f, 1.0f, 0.0f);
        ASSERT_EQ(harken::WorldInstallStatus::Ok,
                  InstallDefinitionsWorld(engine, &definition, 1).status);
        VnEmitterHandle emitter = 0;
        for (std::uint32_t i = 0; i < 129; ++i) {
            ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
        }
        EXPECT_EQ(VN_OK, Vn_SetEmitterValue(emitter, harkentest::ObjectId(9001), 0.5f));
    }
    {
        VnConfig config = MakeNullConfig();
        config.maxEmitters = 1;
        config.emitterVariableSlots = 17;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        std::vector<harken::VariableDefinitionV1> definitions;
        for (std::uint32_t i = 0; i < 17; ++i) {
            definitions.push_back(MakeVariableDefinition(
                harkentest::ObjectId(9100 + i), harken::VariableKindV1::Smooth,
                0.0f, 1.0f, 0.0f));
        }
        ASSERT_EQ(harken::WorldInstallStatus::Ok,
                  InstallDefinitionsWorld(
                      engine, definitions.data(), definitions.size()).status);
        VnEmitterHandle emitter = 0;
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
        for (std::uint32_t i = 0; i < 17; ++i) {
            ASSERT_EQ(VN_OK,
                      Vn_SetEmitterValue(emitter, harkentest::ObjectId(9100 + i), 0.5f));
        }
    }
}

TEST(VariableWorld,
     PendingEmitterSetReservesOverrideCapacityAndGlobalWriteReleasesIt) {
    VnConfig config = MakeNullConfig();
    config.maxEmitters = 1;
    config.emitterVariableSlots = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const harken::VariableDefinitionV1 definitions[] = {
        MakeVariableDefinition(harkentest::ObjectId(9201), harken::VariableKindV1::Smooth,
                               0.0f, 1.0f, 0.1f),
        MakeVariableDefinition(harkentest::ObjectId(9202), harken::VariableKindV1::Smooth,
                               0.0f, 1.0f, 0.2f)};
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallDefinitionsWorld(engine, definitions, 2).status);
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    const std::uint64_t issuedBefore = engine->logicCommits.IssuedCount();
    const std::uint64_t acceptedBefore = engine->variables.AcceptedCount();
    const std::uint64_t appliedBefore = engine->variables.AppliedCount();
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, harkentest::ObjectId(9201), 0.25f));
    ASSERT_EQ(issuedBefore + 1u, engine->logicCommits.IssuedCount());
    ASSERT_EQ(acceptedBefore + 1u, engine->variables.AcceptedCount());
    ASSERT_EQ(1u, engine->variables.PendingCount());
    float value = 0.0f;
    ASSERT_TRUE(engine->variables.CurrentValue(emitter, harkentest::ObjectId(9201), &value));
    EXPECT_FLOAT_EQ(0.1f, value);
    const std::uint64_t issuedAfterFirst =
        engine->logicCommits.IssuedCount();
    const std::uint64_t acceptedAfterFirst =
        engine->variables.AcceptedCount();
    EXPECT_EQ(VN_VARIABLE_EMITTER_SLOT_EXHAUSTED,
              Vn_SetEmitterValue(emitter, harkentest::ObjectId(9202), 0.75f));
    EXPECT_EQ(issuedAfterFirst, engine->logicCommits.IssuedCount());
    EXPECT_EQ(acceptedAfterFirst, engine->variables.AcceptedCount());
    EXPECT_EQ(1u, engine->variables.PendingCount());

    harkentest::AdvanceVariableWrite(engine);
    EXPECT_EQ(appliedBefore + 1u, engine->variables.AppliedCount());
    ASSERT_TRUE(engine->variables.CurrentValue(emitter, harkentest::ObjectId(9201), &value));
    EXPECT_FLOAT_EQ(0.25f, value);

    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(9201), 0.1f));
    harkentest::AdvanceVariableWrite(engine);
    EXPECT_EQ(appliedBefore + 2u, engine->variables.AppliedCount());
    ASSERT_TRUE(engine->variables.CurrentValue(emitter, harkentest::ObjectId(9201), &value));
    EXPECT_FLOAT_EQ(0.1f, value);

    const std::uint64_t issuedBeforeReuse =
        engine->logicCommits.IssuedCount();
    const std::uint64_t acceptedBeforeReuse =
        engine->variables.AcceptedCount();
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, harkentest::ObjectId(9202), 0.75f));
    EXPECT_EQ(issuedBeforeReuse + 1u, engine->logicCommits.IssuedCount());
    EXPECT_EQ(acceptedBeforeReuse + 1u, engine->variables.AcceptedCount());
    harkentest::AdvanceVariableWrite(engine);
    EXPECT_EQ(appliedBefore + 3u, engine->variables.AppliedCount());
    ASSERT_TRUE(engine->variables.CurrentValue(emitter, harkentest::ObjectId(9202), &value));
    EXPECT_FLOAT_EQ(0.75f, value);

    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(9202), 0.2f));
    harkentest::AdvanceVariableWrite(engine);
    const std::uint64_t issuedBeforeSupersede =
        engine->logicCommits.IssuedCount();
    const std::uint64_t acceptedBeforeSupersede =
        engine->variables.AcceptedCount();
    const std::uint64_t supersededBefore =
        engine->variables.SupersededCount();
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, harkentest::ObjectId(9201), 0.5f));
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(9201), 0.1f));
    EXPECT_EQ(VN_VARIABLE_EMITTER_SLOT_EXHAUSTED,
              Vn_SetEmitterValue(emitter, harkentest::ObjectId(9202), 0.25f));
    harkentest::AdvanceVariableWrite(engine);
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, harkentest::ObjectId(9202), 0.25f));
    EXPECT_EQ(issuedBeforeSupersede + 3u,
              engine->logicCommits.IssuedCount());
    EXPECT_EQ(acceptedBeforeSupersede + 3u,
              engine->variables.AcceptedCount());
    EXPECT_EQ(supersededBefore,
              engine->variables.SupersededCount());
    EXPECT_EQ(1u, engine->variables.PendingCount());
    harkentest::AdvanceVariableWrite(engine);
    ASSERT_TRUE(engine->variables.CurrentValue(emitter, harkentest::ObjectId(9201), &value));
    EXPECT_FLOAT_EQ(0.1f, value);
    ASSERT_TRUE(engine->variables.CurrentValue(emitter, harkentest::ObjectId(9202), &value));
    EXPECT_FLOAT_EQ(0.25f, value);
}

TEST(VariableWorld,
     EarlierGlobalWriteKeepsPreviouslyAcceptedFutureEmitterReservation) {
    VnConfig config = MakeNullConfig();
    config.maxEmitters = 1;
    config.emitterVariableSlots = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const harken::VariableDefinitionV1 definitions[] = {
        MakeVariableDefinition(harkentest::ObjectId(9211), harken::VariableKindV1::Smooth,
                               0.0f, 1.0f, 0.1f),
        MakeVariableDefinition(harkentest::ObjectId(9212), harken::VariableKindV1::Smooth,
                               0.0f, 1.0f, 0.2f)};
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallDefinitionsWorld(engine, definitions, 2).status);
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, harkentest::ObjectId(9211), 0.25f));
    harkentest::AdvanceVariableWrite(engine);

    harken::VariableCallObservation clear = harken::ObserveVariableCallAt(
        engine->variables.CurrentWorldGeneration(),
        engine->variables.CurrentTransportEpoch(),
        engine->timelinePool.QuantumFrames(),
        engine->mixer.BlocksRendered() * engine->timelinePool.QuantumFrames(),
        engine->timelinePool.PublishCutoffFrame());
    // Keep the future override beyond the clear's actual publication lead.
    clear.requestedEffectiveFrame = std::max(engine->timelinePool.PublishCutoffFrame(),
        engine->mixer.BlocksRendered() * engine->timelinePool.QuantumFrames()) +
        static_cast<std::uint64_t>(engine->timelinePool.PublishLeadQuanta() + 2u) *
            engine->timelinePool.QuantumFrames();
    harken::VariableCallObservation laterSet = clear;
    laterSet.requestedEffectiveFrame +=
        static_cast<std::uint64_t>(2u * engine->timelinePool.PublishLeadQuanta() + 4u) *
            engine->timelinePool.QuantumFrames();
    const auto futureAccepted = engine->variables.AcceptEmitter(
        emitter, harkentest::ObjectId(9211), 0.75f, laterSet, engine->emitters, engine->params);
    ASSERT_EQ(VN_OK, futureAccepted.result);
    const auto clearAccepted = engine->variables.AcceptGlobal(
        harkentest::ObjectId(9211), 0.1f, clear, engine->params, engine->bindings, engine->busTargets);
    ASSERT_EQ(VN_OK, clearAccepted.result);

    float current = 0.0f;
    bool observedClearWindow = false;
    std::vector<std::pair<float, std::uint32_t>> observedBanks;
    std::uint64_t clearFrame = 0, futureFrame = 0;
    for (std::uint32_t block = 0; block < 32; ++block) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
        ASSERT_TRUE(engine->variables.CurrentValue(emitter, harkentest::ObjectId(9211),
                                                   &current));
        observedBanks.emplace_back(current, engine->variables.PendingCount());
        const auto clearSlice = engine->variables.SliceForTicketForTests(clearAccepted.ticket.sequence);
        const auto futureSlice = engine->variables.SliceForTicketForTests(futureAccepted.ticket.sequence);
        if (clearSlice.Valid()) clearFrame = engine->variableSlices.EffectiveFrame(clearSlice);
        if (futureSlice.Valid()) futureFrame = engine->variableSlices.EffectiveFrame(futureSlice);
        if (current == 0.1f && engine->variables.PendingCount() == 1) {
            observedClearWindow = true;
            break;
        }
    }
    ASSERT_TRUE(observedClearWindow) << "clear frame " << clearFrame << ", future frame " << futureFrame
        << ", values/pending " << ::testing::PrintToString(observedBanks);
    const std::uint64_t issuedBeforeRefusal =
        engine->logicCommits.IssuedCount();
    const std::uint64_t acceptedBeforeRefusal =
        engine->variables.AcceptedCount();
    EXPECT_EQ(VN_VARIABLE_EMITTER_SLOT_EXHAUSTED,
              Vn_SetEmitterValue(emitter, harkentest::ObjectId(9212), 0.5f));
    EXPECT_EQ(issuedBeforeRefusal, engine->logicCommits.IssuedCount());
    EXPECT_EQ(acceptedBeforeRefusal, engine->variables.AcceptedCount());
    EXPECT_EQ(1u, engine->variables.PendingCount());

    harkentest::AdvanceVariableWrite(engine);
    ASSERT_TRUE(engine->variables.CurrentValue(emitter, harkentest::ObjectId(9211), &current));
    EXPECT_FLOAT_EQ(0.75f, current);
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(9211), 0.1f));
    harkentest::AdvanceVariableWrite(engine);
    EXPECT_EQ(VN_OK, Vn_SetEmitterValue(emitter, harkentest::ObjectId(9212), 0.5f));
    harkentest::AdvanceVariableWrite(engine);
    ASSERT_TRUE(engine->variables.CurrentValue(emitter, harkentest::ObjectId(9212), &current));
    EXPECT_FLOAT_EQ(0.5f, current);
}

TEST(VariableWorld, DurableRowsCoalesceSameFrameByHighestTicket) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const auto definitions = DefaultDefinitions();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallDefinitionsWorld(
                  engine, definitions.data(),
                  static_cast<std::uint32_t>(definitions.size()))
                  .status);

    // Same call-entry observation window: three writes to one key coalesce
    // to the highest LogicCommitTicket, older rows become Superseded.
    ASSERT_EQ(VN_OK, Vn_SetValue(kSmoothVar, -2.0f));
    ASSERT_EQ(VN_OK, Vn_SetValue(kSmoothVar, -4.0f));
    ASSERT_EQ(VN_OK, Vn_SetValue(kSmoothVar, -6.0f));
    harkentest::AdvanceVariableWrite(engine);

    float value = 0.0f;
    ASSERT_TRUE(engine->variables.CurrentValue(kSmoothVar, &value));
    EXPECT_FLOAT_EQ(-6.0f, value);
    EXPECT_EQ(2u, engine->variables.SupersededCount());
    EXPECT_EQ(1u, engine->variables.AppliedCount());

    // The service drains terminal outcome rows after updating the counters;
    // exact record payloads are pinned by the VTRC tests.
    EXPECT_EQ(0u, engine->variables.PendingCount());
}

TEST(VariableWorld, EmitterDestroyTerminatesPendingScopedRows) {
    VnConfig config = MakeNullConfig();
    config.maxEmitters = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const auto definitions = DefaultDefinitions();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallDefinitionsWorld(
                  engine, definitions.data(),
                  static_cast<std::uint32_t>(definitions.size()))
                  .status);
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));

    // A pending emitter-scoped row whose emitter dies before Applied ends as
    // TerminatedEmitterDestroyed, never Applied.
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, kSmoothVar, -9.0f));
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(emitter));
    ASSERT_EQ(VN_OK, Vn_Update());

    EXPECT_EQ(1u, engine->variables.TerminationCount());
    EXPECT_EQ(0u, engine->variables.AppliedCount());
    EXPECT_EQ(0u, engine->variables.PendingCount());
}

TEST(VariableWorld, WorldReplacementTerminatesOldPendingRows) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    auto definitions = DefaultDefinitions();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallDefinitionsWorld(
                  engine, definitions.data(),
                  static_cast<std::uint32_t>(definitions.size()))
                  .status);

    ASSERT_EQ(VN_OK, Vn_SetValue(kSmoothVar, -5.0f));
    definitions[0].rangeMax = 16.0f; // behavior change forces replacement
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallDefinitionsWorld(
                  engine, definitions.data(),
                  static_cast<std::uint32_t>(definitions.size()),
                  0x5445535459u)
                  .status);
    ASSERT_EQ(VN_OK, Vn_Update());

    EXPECT_EQ(1u, engine->variables.TerminationCount());
    EXPECT_EQ(0u, engine->variables.PendingCount());
    // Same-behavior Smooth definition carried its bank across the replace.
    float value = 0.0f;
    ASSERT_TRUE(engine->variables.CurrentValue(kSmoothVar, &value));
    EXPECT_FLOAT_EQ(0.0f, value);
}

TEST(VariableWorld, AppliedValueDrivesBusBindingAudibly) {
    VnConfig config = MakeNullConfig(4, 256, 48000);
    config.maxBuses = 4;
    config.maxBindings = 4;
    config.maxBindingAttachments = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const auto definitions = DefaultDefinitions();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallDefinitionsWorld(
                  engine, definitions.data(),
                  static_cast<std::uint32_t>(definitions.size()))
                  .status);

    constexpr VnBusId kBus = harkentest::ObjectId(9);
    constexpr VnSoundId kSound = harkentest::ObjectId(901);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBus, VN_BUS_MASTER));
    const std::vector<float> dc = MakeDC(48000, 0.5f);
    const VnPCMDesc pcm = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(kSound, &pcm));

    VnBindingDesc binding{};
    binding.structSize = sizeof(VnBindingDesc);
    binding.variableId = kSmoothVar;
    binding.targetScopeKind = VN_TARGET_SCOPE_BUS;
    binding.targetId = 0;
    binding.curve.structSize = sizeof(VnCurveDesc);
    binding.curve.pointCount = 2;
    binding.curve.points[0] = {-24.0f, -24.0f};
    binding.curve.points[1] = {0.0f, 0.0f};
    binding.curve.handles[0] = harkentest::EndpointHandles(
        binding.curve.points[0], binding.curve.points[1]);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(70), &binding));
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(VnBindingAttachDesc);
    attach.scopeKind = VN_TARGET_SCOPE_BUS;
    attach.objectId = kBus;
    ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(70), &attach));

    VnPlayDesc play = MakePlayDesc(kSound);
    play.busId = kBus;
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    ASSERT_EQ(VN_OK, Vn_Update());
    harkentest::Capture loud;
    harkentest::PumpAndCapture(engine, 2, loud);
    const float loudPeak = harkentest::Peak(loud.left);
    ASSERT_GT(loudPeak, harkentest::kSilenceFloor);

    // An applied -24 dB write must audibly attenuate the bound bus.
    ASSERT_EQ(VN_OK, Vn_SetValue(kSmoothVar, -24.0f));
    harkentest::AdvanceVariableWrite(engine);
    engine->nullBackend.Pump(4);
    harkentest::Capture quiet;
    harkentest::PumpAndCapture(engine, 2, quiet);
    const float quietPeak = harkentest::Peak(quiet.left);
    ASSERT_GT(quietPeak, 0.0f);
    EXPECT_LT(quietPeak, loudPeak * 0.25f);
}

TEST(VariableWorld, HistorySlotsRecycleAcrossTheFullCapacity) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const auto definitions = DefaultDefinitions();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallDefinitionsWorld(
                  engine, definitions.data(),
                  static_cast<std::uint32_t>(definitions.size()))
                  .status);

    // Delivered Terminal rows must recycle their slots: accept-and-apply
    // cycled past the full history capacity still accepts. Without
    // recycling the 4097th write refuses VN_NO_VARIABLE forever.
    const std::uint32_t cycles =
        engine->variables.HistoryCapacity() + 8u;
    for (std::uint32_t i = 0; i < cycles; ++i) {
        ASSERT_EQ(VN_OK, Vn_SetValue(kSmoothVar, -1.0f - (i % 3)))
            << "cycle " << i;
        ASSERT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    // Drain the tail: a write's effective frame sits ahead of the audio
    // clock, so the last few writes are still in flight when the loop
    // ends.
    harkentest::AdvanceVariableWrite(engine);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(0u, engine->variables.PendingCount());
    // Every accepted write reached a terminal outcome; none was refused.
    EXPECT_EQ(static_cast<std::uint64_t>(cycles),
              engine->variables.AcceptedCount());
    EXPECT_EQ(static_cast<std::uint64_t>(cycles),
              engine->variables.AppliedCount() +
                  engine->variables.SupersededCount());
    EXPECT_EQ(0u, engine->variables.RefusalCount());
    // The pool never grew: the high-water mark stays far under capacity
    // The live page lead and the strict return fence keep a bounded pipeline,
    // not one fully completed write per cycle.
    EXPECT_LE(engine->variables.HistoryHighWater(),
              static_cast<std::uint64_t>(2u * engine->timelinePool.PublishLeadQuanta() + 4u));
    float value = 0.0f;
    ASSERT_TRUE(engine->variables.CurrentValue(kSmoothVar, &value));
    EXPECT_FLOAT_EQ(-1.0f - ((cycles - 1u) % 3u), value);
}

} // namespace
