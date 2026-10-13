// Public binding system pools and validation: registration validates
// exhaustively before touching pool
// state (curve rules, known scoped target, the section-9.1 transition
// refusal on snap-only targets), duplicate ids refuse VN_IN_USE, full
// pools refuse the no-binding code with a log record naming which pool,
// attachment validation (scope match, unknown ids, duplicates), the
// section-5.4 unregister reference rule, the per-voice instance slot
// ceiling, the section-5.5 frozen structural identity on Replace, the
// no-audible-side-effect rule for registration/attachment, bus-scope
// attachment evaluation into the two-slot store at global scope, and the
// section-9.2 target-level ramp aggregation at the instance seam.
//
// The engine-session entries (RegisterBinding and friends) carry the
// exact semantics the public verbs surface later; the aggregation golden
// drives the ParamSystem seam directly, like the publication seam tests.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include "harken/core/LogRing.h"
#include "harken/dsp/GainRamp.h"
#include "harken/param/BindingTable.h"
#include "harken/param/ParamTargetTable.h"
#include "harken/param/ParamTargets.h"

#include <cmath>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace {

using harkentest::EngineSession;
using harkentest::FindLogicLog;
using harkentest::MakeDC;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::RenderHandle;

constexpr std::uint32_t kBlock = 256;
constexpr std::uint32_t kRate = 48000;

void InstallBindingVariables(harken::Engine* engine) {
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(
                  engine, {harkentest::ObjectId(7), harkentest::ObjectId(8)}, 0.0f, 1.0f).status);
}

static_assert(noexcept(
    std::declval<harken::BindingTable&>().CommitPreparedDefinition(
        static_cast<harken::BindingTable::PreparedDefinitionToken*>(
            nullptr))));
static_assert(noexcept(
    std::declval<harken::BindingTable&>().CommitPreparedAttachment(
        static_cast<harken::BindingTable::PreparedAttachmentToken*>(
            nullptr),
        static_cast<harken::BusTargetStore*>(nullptr))));
static_assert(noexcept(
    std::declval<harken::ParamSystem&>().CommitPreparedPurgeInstances(
        static_cast<harken::ParamSystem::PreparedPurgeToken*>(
            nullptr))));
static_assert(noexcept(
    std::declval<const harken::BindingTable&>().CanCommitPreparedAttachment(
        std::declval<const harken::BindingTable::PreparedAttachmentToken&>())));

TEST(BindingPool, PreparedDetachRejectsZeroPublishWhenLiveAttachmentChanges) {
    harken::BindingTable table(2, 2, 2);
    harken::BusTargetStore store(2);
    harken::ParamSystem params(harken::EngineConfig{});
    const harken::CurvePoint points[] = {{0.0f, 0.0f}, {1.0f, -6.0f}};
    const harken::CurveHandlePair handles[] = {{0.0f, 0.0f, 1.0f, -6.0f}};
    ASSERT_EQ(VN_OK, table.RegisterBinding(harkentest::ObjectId(41), harkentest::ObjectId(71), harken::kTargetScopeBus,
        harken::kBusTargetBusVolume, points, handles, 2, 0, nullptr));
    ASSERT_EQ(VN_OK, table.Attach(harkentest::ObjectId(41), harken::kTargetScopeBus, harkentest::ObjectId(81), 1, nullptr, 0));
    ASSERT_TRUE(table.HasAttachment(harkentest::ObjectId(41), harken::kTargetScopeBus, harkentest::ObjectId(81)));

    harken::BindingTable::PreparedAttachmentToken token;
    ASSERT_EQ(VN_OK, table.PrepareDetach(harkentest::ObjectId(41), harken::kTargetScopeBus, harkentest::ObjectId(81), &token));
    ASSERT_TRUE(token.IsPrepared());
    EXPECT_TRUE(table.CanCommitPreparedAttachment(token));

    ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(71), 1.0f));
    table.EvaluateBusAttachments(params, store);
    EXPECT_FALSE(table.CanCommitPreparedAttachment(token));
    EXPECT_TRUE(table.HasAttachment(harkentest::ObjectId(41), harken::kTargetScopeBus, harkentest::ObjectId(81)));
    table.CancelPreparedAttachment(&token);
    EXPECT_FALSE(token.IsPrepared());
    EXPECT_TRUE(table.HasAttachment(harkentest::ObjectId(41), harken::kTargetScopeBus, harkentest::ObjectId(81)));
}

TEST(BindingPool, TransitionRangeRefusalPreservesRegisteredAndPreparedState) {
    harken::BindingTable table(2, 2, 2);
    const harken::CurvePoint points[] = {{0.0f, 0.0f}, {1.0f, -6.0f}};
    const harken::CurveHandlePair handles[] = {{0.0f, 0.0f, 1.0f, -6.0f}};
    ASSERT_EQ(VN_OK, table.RegisterBinding(harkentest::ObjectId(41), harkentest::ObjectId(71), harken::kTargetScopeBus,
        harken::kBusTargetBusVolume, points, handles, 2, 60000, nullptr));
    ASSERT_EQ(1u, table.DefinitionCount());
    for (std::uint32_t invalid : {60001u, UINT32_MAX}) {
        EXPECT_EQ(VN_INVALID_ARG, table.CanRegister(harkentest::ObjectId(42), harkentest::ObjectId(71), harken::kTargetScopeBus,
            harken::kBusTargetBusVolume, points, handles, 2, invalid));
        EXPECT_EQ(VN_INVALID_ARG, table.RegisterBinding(harkentest::ObjectId(42), harkentest::ObjectId(71), harken::kTargetScopeBus,
            harken::kBusTargetBusVolume, points, handles, 2, invalid, nullptr));
        EXPECT_EQ(VN_INVALID_ARG, table.ReplaceBinding(harkentest::ObjectId(41), harkentest::ObjectId(71), harken::kTargetScopeBus,
            harken::kBusTargetBusVolume, points, handles, 2, invalid));
        harken::BindingTable::PreparedDefinitionToken prepared;
        EXPECT_EQ(VN_INVALID_ARG, table.PrepareRegisterDefinition(harkentest::ObjectId(42), harkentest::ObjectId(71),
            harken::kTargetScopeBus, harken::kBusTargetBusVolume,
            points, handles, 2, invalid, 1, 9001, &prepared));
        EXPECT_FALSE(prepared.IsPrepared());
        EXPECT_EQ(VN_INVALID_ARG, table.PrepareReplaceDefinition(harkentest::ObjectId(41), harkentest::ObjectId(71),
            harken::kTargetScopeBus, harken::kBusTargetBusVolume,
            points, handles, 2, invalid, &prepared));
        EXPECT_FALSE(prepared.IsPrepared());
        table.InstallPendingRevisions([](harken::BindingId) {});
        EXPECT_EQ(1u, table.DefinitionCount());
        EXPECT_EQ(nullptr, table.Find(harkentest::ObjectId(42)));
        ASSERT_NE(nullptr, table.Find(harkentest::ObjectId(41)));
        EXPECT_EQ(60000u, table.Find(harkentest::ObjectId(41))->transitionMs);
        EXPECT_FLOAT_EQ(-6.0f, table.Find(harkentest::ObjectId(41))->curve.points[1].y);
    }
    harken::BindingTable::PreparedDefinitionToken prepared;
    ASSERT_EQ(VN_OK, table.PrepareRegisterDefinition(harkentest::ObjectId(42), harkentest::ObjectId(71),
        harken::kTargetScopeBus, harken::kBusTargetBusVolume,
        points, handles, 2, 60000, 1, 9001, &prepared));
    table.CommitPreparedDefinition(&prepared);
    EXPECT_EQ(2u, table.DefinitionCount());
}

TEST(BindingPool,
     PreparedDefinitionAndAttachmentCommitsUseExactSlots) {
    harken::BindingTable bindings(2, 2, 2);
    harken::BindingTable::PreparedDefinitionToken definition;
    const harken::CurvePoint invalidPoints[] = {
        {0.5f, 0.0f}, {0.5f, -6.0f}};
    const harken::CurveHandlePair invalidHandles[] = {
        {0.5f, 0.0f, 0.5f, -6.0f}};

    EXPECT_EQ(
        VN_INVALID_ARG,
        bindings.PrepareRegisterDefinition(
            harkentest::ObjectId(41), harkentest::ObjectId(71), harken::kTargetScopeBus,
            harken::kBusTargetBusVolume, invalidPoints, invalidHandles,
            2, 0, 1, 9001, &definition));
    EXPECT_FALSE(definition.IsPrepared());
    EXPECT_EQ(0u, bindings.DefinitionCount());

    const harken::CurvePoint points[] = {
        {0.0f, 0.0f}, {1.0f, -6.0f}};
    const harken::CurveHandlePair handles[] = {
        {0.0f, 0.0f, 1.0f, -6.0f}};
    EXPECT_EQ(
        VN_OK,
        bindings.PrepareRegisterDefinition(
            harkentest::ObjectId(41), harkentest::ObjectId(71), harken::kTargetScopeBus,
            harken::kBusTargetBusVolume, points, handles, 2, 0,
            1, 9001, &definition));
    ASSERT_TRUE(definition.IsPrepared());
    EXPECT_EQ(1u, definition.SlotIndex());
    bindings.CommitPreparedDefinition(&definition);
    ASSERT_EQ(1u, bindings.DefinitionCount());
    ASSERT_NE(nullptr, bindings.Find(harkentest::ObjectId(41)));
    EXPECT_EQ(9001u, bindings.DefinitionOwnerOf(harkentest::ObjectId(41)));

    const harken::CurvePoint revisedPoints[] = {
        {0.0f, 0.0f}, {1.0f, -12.0f}};
    const harken::CurveHandlePair revisedHandles[] = {
        {0.0f, 0.0f, 1.0f, -12.0f}};
    EXPECT_EQ(
        VN_OK,
        bindings.PrepareReplaceDefinition(
            harkentest::ObjectId(41), harkentest::ObjectId(71), harken::kTargetScopeBus,
            harken::kBusTargetBusVolume, revisedPoints, revisedHandles,
            2, 0, &definition));
    EXPECT_EQ(1u, definition.SlotIndex());
    bindings.CommitPreparedDefinition(&definition);
    bindings.InstallPendingRevisions([](harken::BindingId) {});
    EXPECT_FLOAT_EQ(-12.0f, bindings.Find(harkentest::ObjectId(41))->curve.points[1].y);

    harken::BindingTable::PreparedAttachmentToken attachment;
    EXPECT_EQ(
        VN_OK,
        bindings.PrepareAttach(
            harkentest::ObjectId(41), harken::kTargetScopeBus, harkentest::ObjectId(81), 1, 1, 9001,
            &attachment));
    ASSERT_TRUE(attachment.IsPrepared());
    EXPECT_EQ(1u, attachment.SlotIndex());
    bindings.CommitPreparedAttachment(&attachment, nullptr);
    EXPECT_EQ(1u, bindings.AttachmentCount());
    EXPECT_TRUE(bindings.HasOwnedAttachment(
        harkentest::ObjectId(41), harken::kTargetScopeBus, harkentest::ObjectId(81), 9001));

    EXPECT_EQ(
        VN_OK,
        bindings.PrepareDetach(
            harkentest::ObjectId(41), harken::kTargetScopeBus, harkentest::ObjectId(81), &attachment));
    EXPECT_EQ(1u, attachment.SlotIndex());
    bindings.CommitPreparedAttachment(&attachment, nullptr);
    EXPECT_EQ(0u, bindings.AttachmentCount());

    EXPECT_EQ(VN_OK,
              bindings.PrepareRemoveDefinition(harkentest::ObjectId(41), &definition));
    EXPECT_EQ(1u, definition.SlotIndex());
    bindings.CommitPreparedDefinition(&definition);
    EXPECT_EQ(0u, bindings.DefinitionCount());
    EXPECT_EQ(nullptr, bindings.Find(harkentest::ObjectId(41)));
}

void RegisterLeaf(VnSoundId id) {
    static const std::vector<float> dc = MakeDC(kRate, 0.5f);
    const VnPCMDesc desc = harkentest::MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(id, &desc));
}

// Registers a two-point linear binding definition through the engine
// seam: variable in [0, 1] maps to [y0, y1] on the scoped target.
VnResult RegisterLinear(harken::Engine* engine, harken::BindingId id,
                        VnVariableId variableId, std::uint32_t scopeKind,
                        std::uint32_t targetId, float y0, float y1,
                        std::uint32_t transitionMs = 0) {
    const harken::CurvePoint pts[] = {{0.0f, y0}, {1.0f, y1}};
    const harken::CurveHandlePair handles[] = {{pts[0].x, pts[0].y, pts[1].x, pts[1].y}};
    return harken::RegisterBinding(*engine, id, variableId, scopeKind,
                                  targetId, pts, handles, 2, transitionMs);
}

VnResult ReplaceLinear(harken::Engine* engine, harken::BindingId id,
                       VnVariableId variableId, std::uint32_t scopeKind,
                       std::uint32_t targetId, float y0, float y1,
                       std::uint32_t transitionMs = 0) {
    const harken::CurvePoint pts[] = {{0.0f, y0}, {1.0f, y1}};
    const harken::CurveHandlePair handles[] = {{pts[0].x, pts[0].y, pts[1].x, pts[1].y}};
    return harken::ReplaceBinding(*engine, id, variableId, scopeKind,
                                 targetId, pts, handles, 2, transitionMs);
}

// --- section 5.2: registration validates exhaustively, then mutates ---

TEST(BindingPool, RegisterValidatesExhaustivelyBeforeMutating) {
    EngineSession session(MakeNullConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallBindingVariables(engine);

    // Curve rules are Curve::SetPoints' own: non-increasing x refuses.
    {
        const harken::CurvePoint pts[] = {{0.5f, 0.0f}, {0.5f, -6.0f}};
        const harken::CurveHandlePair handles[] = {{pts[0].x, pts[0].y, pts[1].x, pts[1].y}};
        EXPECT_EQ(VN_INVALID_ARG,
                  harken::RegisterBinding(*engine, harkentest::ObjectId(1), harkentest::ObjectId(7),
                                         harken::kTargetScopeVoice,
                                         harken::kTargetVoiceVolume, pts,
                                         handles, 2, 0));
    }
    // Non-finite coordinate refuses.
    {
        const harken::CurvePoint pts[] = {{0.0f, 0.0f},
                                         {1.0f, std::nanf("")}};
        const harken::CurveHandlePair handles[] = {{pts[0].x, pts[0].y, pts[1].x, pts[1].y}};
        EXPECT_EQ(VN_INVALID_ARG,
                  harken::RegisterBinding(*engine, harkentest::ObjectId(1), harkentest::ObjectId(7),
                                         harken::kTargetScopeVoice,
                                         harken::kTargetVoiceVolume, pts,
                                         handles, 2, 0));
    }
    // Out-of-range handle refuses.
    {
        const harken::CurvePoint pts[] = {{0.0f, 0.0f}, {1.0f, -6.0f}};
        const harken::CurveHandlePair handles[] = {
            {-1.0f, 0.0f, 1.0f, -6.0f}};
        EXPECT_EQ(VN_INVALID_ARG,
                  harken::RegisterBinding(*engine, harkentest::ObjectId(1), harkentest::ObjectId(7),
                                         harken::kTargetScopeVoice,
                                         harken::kTargetVoiceVolume, pts,
                                         handles, 2, 0));
    }
    // Unknown scoped target — bad target id, and a scope kind that does
    // not exist yet — refuses, never guesses.
    EXPECT_EQ(VN_INVALID_ARG,
              RegisterLinear(engine, harkentest::ObjectId(1), harkentest::ObjectId(7), harken::kTargetScopeVoice, 99,
                             0.0f, -6.0f));
    EXPECT_EQ(VN_INVALID_ARG,
              RegisterLinear(engine, harkentest::ObjectId(1), harkentest::ObjectId(7), /*scopeKind=*/7,
                             harken::kTargetVoiceVolume, 0.0f, -6.0f));
    // Section 9.1: nonzero transition on a snap-only target refuses
    // loudly (pan does not support transitions)...
    EXPECT_EQ(VN_INVALID_ARG,
              RegisterLinear(engine, harkentest::ObjectId(1), harkentest::ObjectId(7), harken::kTargetScopeVoice,
                             harken::kTargetVoicePan, 0.0f, 1.0f,
                             /*transitionMs=*/100));
    // Every refusal above left the pool untouched.
    EXPECT_EQ(nullptr, engine->bindings.Find(harkentest::ObjectId(1)));
    EXPECT_EQ(0u, engine->bindings.DefinitionCount());

    // ...while transition-capable targets accept one.
    EXPECT_EQ(VN_OK, RegisterLinear(engine, harkentest::ObjectId(1), harkentest::ObjectId(7), harken::kTargetScopeVoice,
                                    harken::kTargetVoiceVolume, 0.0f, -6.0f,
                                    /*transitionMs=*/100));
    EXPECT_EQ(VN_OK, RegisterLinear(engine, harkentest::ObjectId(2), harkentest::ObjectId(7), harken::kTargetScopeBus,
                                    harken::kBusTargetBusVolume, 0.0f, -6.0f,
                                    /*transitionMs=*/250));
    EXPECT_EQ(VN_OK, RegisterLinear(
                         engine, harkentest::ObjectId(3), harkentest::ObjectId(7), harken::kTargetScopeVoice,
                         harken::kTargetVoiceHighShelfQ, 0.0f, 1.0f,
                         /*transitionMs=*/500));
    // Duplicate id refuses VN_IN_USE and keeps the registered row.
    EXPECT_EQ(VN_IN_USE,
              RegisterLinear(engine, harkentest::ObjectId(1), harkentest::ObjectId(8), harken::kTargetScopeVoice,
                             harken::kTargetVoicePitch, 0.0f, 100.0f));
    const harken::BindingTable::Definition* def = engine->bindings.Find(harkentest::ObjectId(1));
    ASSERT_NE(nullptr, def);
    EXPECT_EQ(harkentest::ObjectId(7u), def->variableId);
    EXPECT_EQ(harken::kTargetVoiceVolume, def->targetId);
}

TEST(BindingPool, DefinitionPoolFullRefusesNoBindingAndNamesThePool) {
    EngineSession session(MakeNullConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallBindingVariables(engine);

    // The internal default pool holds 128 definitions (section 16).
    for (std::uint32_t i = 0; i < 128; ++i) {
        ASSERT_EQ(VN_OK, RegisterLinear(engine, harkentest::ObjectId(1000 + i), harkentest::ObjectId(7),
                                        harken::kTargetScopeVoice,
                                        harken::kTargetVoiceVolume, 0.0f,
                                        -6.0f));
    }
    EXPECT_EQ(harken::kResultNoBinding,
              RegisterLinear(engine, harkentest::ObjectId(5000), harkentest::ObjectId(7), harken::kTargetScopeVoice,
                             harken::kTargetVoiceVolume, 0.0f, -6.0f));
    harken::LogRecord record{};
    ASSERT_TRUE(FindLogicLog(engine, harken::kLogCodeBindingPoolExhausted,
                             &record));
    EXPECT_EQ(harken::kBindingPoolDefinitions, record.arg0);
    EXPECT_EQ(128u, record.arg1);
    // Section 12: the definition pool's high water and refusal counter
    // surface in the snapshot tail, 1:1 with the log record.
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    EXPECT_EQ(128u, snapshot.bindingDefinitionHighWater);
    EXPECT_EQ(1u, snapshot.bindingDefinitionRefusedCount);
    EXPECT_EQ(0u, snapshot.bindingAttachmentRefusedCount);
    EXPECT_EQ(0u, snapshot.bindingInstanceRefusedCount);
}

// --- section 5.3: attachment validation ---

TEST(BindingPool, AttachValidationMatrix) {
    EngineSession session(MakeNullConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallBindingVariables(engine);
    RegisterLeaf(harkentest::ObjectId(1));
    ASSERT_EQ(VN_OK, RegisterLinear(engine, harkentest::ObjectId(10), harkentest::ObjectId(7), harken::kTargetScopeVoice,
                                    harken::kTargetVoiceVolume, 0.0f, -6.0f));
    ASSERT_EQ(VN_OK, RegisterLinear(engine, harkentest::ObjectId(11), harkentest::ObjectId(7), harken::kTargetScopeBus,
                                    harken::kBusTargetBusVolume, 0.0f, -6.0f));

    // Unknown binding id.
    EXPECT_EQ(VN_UNKNOWN_ID, harken::AttachBinding(
                                 *engine, harkentest::ObjectId(99), harken::kTargetScopeVoice, harkentest::ObjectId(1)));
    // Scope mismatch against the definition's targetScopeKind.
    EXPECT_EQ(VN_INVALID_ARG,
              harken::AttachBinding(*engine, harkentest::ObjectId(10), harken::kTargetScopeBus,
                                   VN_BUS_MASTER));
    EXPECT_EQ(VN_INVALID_ARG, harken::AttachBinding(
                                  *engine, harkentest::ObjectId(11), harken::kTargetScopeVoice, harkentest::ObjectId(1)));
    // Unknown object ids: an unregistered playable, an unknown bus.
    EXPECT_EQ(VN_UNKNOWN_ID, harken::AttachBinding(
                                 *engine, harkentest::ObjectId(10), harken::kTargetScopeVoice, harkentest::ObjectId(42)));
    EXPECT_EQ(VN_UNKNOWN_ID,
              harken::AttachBinding(*engine, harkentest::ObjectId(11), harken::kTargetScopeBus, harkentest::ObjectId(42)));
    // A valid pair attaches once; the duplicate refuses VN_IN_USE.
    EXPECT_EQ(VN_OK, harken::AttachBinding(*engine, harkentest::ObjectId(10),
                                          harken::kTargetScopeVoice, harkentest::ObjectId(1)));
    EXPECT_EQ(VN_IN_USE, harken::AttachBinding(*engine, harkentest::ObjectId(10),
                                              harken::kTargetScopeVoice, harkentest::ObjectId(1)));
    // Detach of a relation that does not exist refuses VN_UNKNOWN_ID.
    EXPECT_EQ(VN_UNKNOWN_ID, harken::DetachBinding(
                                 *engine, harkentest::ObjectId(10), harken::kTargetScopeVoice, harkentest::ObjectId(2)));
    EXPECT_EQ(VN_OK, harken::DetachBinding(*engine, harkentest::ObjectId(10),
                                          harken::kTargetScopeVoice, harkentest::ObjectId(1)));
}

TEST(BindingPool, AttachmentPoolFullRefusesNoBindingAndNamesThePool) {
    VnConfig config = MakeNullConfig(4, kBlock, kRate);
    // Headroom for 256 attachment worst-case commits (section 10.4): the
    // capacity contract must not fire before the pool bound under test.
    config.commandQueueDepth = 1024;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallBindingVariables(engine);
    RegisterLeaf(harkentest::ObjectId(1));
    RegisterLeaf(harkentest::ObjectId(2));
    RegisterLeaf(harkentest::ObjectId(3));
    // 128 definitions x 2 playables = 256 attachments — the exact
    // internal default pool (section 16).
    for (std::uint32_t i = 0; i < 128; ++i) {
        ASSERT_EQ(VN_OK, RegisterLinear(engine, harkentest::ObjectId(1000 + i), harkentest::ObjectId(7),
                                        harken::kTargetScopeVoice,
                                        harken::kTargetVoiceVolume, 0.0f,
                                        -6.0f));
        ASSERT_EQ(VN_OK, harken::AttachBinding(*engine, harkentest::ObjectId(1000 + i),
                                              harken::kTargetScopeVoice, harkentest::ObjectId(1)));
        ASSERT_EQ(VN_OK, harken::AttachBinding(*engine, harkentest::ObjectId(1000 + i),
                                              harken::kTargetScopeVoice, harkentest::ObjectId(2)));
    }
    EXPECT_EQ(harken::kResultNoBinding,
              harken::AttachBinding(*engine, harkentest::ObjectId(1000), harken::kTargetScopeVoice,
                                   harkentest::ObjectId(3)));
    harken::LogRecord record{};
    ASSERT_TRUE(FindLogicLog(engine, harken::kLogCodeBindingPoolExhausted,
                             &record));
    EXPECT_EQ(harken::kBindingPoolAttachments, record.arg0);
    EXPECT_EQ(256u, record.arg1);
    // Section 12: attachment pool high water and refusal counter in the
    // snapshot tail, 1:1 with the log record.
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    EXPECT_EQ(256u, snapshot.bindingAttachmentHighWater);
    EXPECT_EQ(1u, snapshot.bindingAttachmentRefusedCount);
}

TEST(BindingPool, AttachCapacityContractRefusesInvalidConfig) {
    EngineSession session(MakeNullConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallBindingVariables(engine);
    RegisterLeaf(harkentest::ObjectId(1));
    ASSERT_EQ(VN_OK, RegisterLinear(engine, harkentest::ObjectId(10), harkentest::ObjectId(7), harken::kTargetScopeVoice,
                                    harken::kTargetVoiceVolume, 0.0f, -6.0f));
    // Squeeze the section-10.4 worst-case bound to its ceiling: the next
    // attach's +1 cannot fit and refuses structurally, with zero pool
    // state change.
    const std::uint32_t capacity = engine->propertyPublisher.BatchCapacity();
    ASSERT_TRUE(engine->propertyPublisher.TryGrowWorstCase(capacity,
                                                           nullptr));
    EXPECT_EQ(VN_INVALID_CONFIG,
              harken::AttachBinding(*engine, harkentest::ObjectId(10), harken::kTargetScopeVoice, harkentest::ObjectId(1)));
    EXPECT_EQ(0u, engine->bindings.AttachmentCount());
    // Releasing the pressure re-opens admission.
    engine->propertyPublisher.ShrinkWorstCase(capacity);
    EXPECT_EQ(VN_OK, harken::AttachBinding(*engine, harkentest::ObjectId(10),
                                          harken::kTargetScopeVoice, harkentest::ObjectId(1)));
}

// --- section 5.4: unregister refuses while referenced ---

TEST(BindingPool, UnregisterRefusesWhileAttachedOrInstanced) {
    EngineSession session(MakeNullConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallBindingVariables(engine);
    RegisterLeaf(harkentest::ObjectId(1));
    ASSERT_EQ(VN_OK, RegisterLinear(engine, harkentest::ObjectId(10), harkentest::ObjectId(7), harken::kTargetScopeVoice,
                                    harken::kTargetVoiceVolume, 0.0f, -6.0f));

    EXPECT_EQ(VN_UNKNOWN_ID, harken::UnregisterBinding(*engine, harkentest::ObjectId(99)));

    // Attachment reference.
    ASSERT_EQ(VN_OK, harken::AttachBinding(*engine, harkentest::ObjectId(10),
                                          harken::kTargetScopeVoice, harkentest::ObjectId(1)));
    EXPECT_EQ(VN_IN_USE, harken::UnregisterBinding(*engine, harkentest::ObjectId(10)));
    ASSERT_EQ(VN_OK, harken::DetachBinding(*engine, harkentest::ObjectId(10),
                                          harken::kTargetScopeVoice, harkentest::ObjectId(1)));

    // Live instance reference (the per-play apply special case).
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, harken::ApplyVoiceBinding(*engine, handle, harkentest::ObjectId(10)));
    EXPECT_EQ(VN_IN_USE, harken::UnregisterBinding(*engine, harkentest::ObjectId(10)));

    // The voice's end releases the reference: no hidden cascade, just the
    // honest static rule.
    ASSERT_EQ(VN_OK, Vn_Stop(handle, 0));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(VN_OK, harken::UnregisterBinding(*engine, harkentest::ObjectId(10)));
    EXPECT_EQ(nullptr, engine->bindings.Find(harkentest::ObjectId(10)));
}

// --- section 16: the per-voice instance slot ceiling ---

TEST(BindingPool, InstanceSlotCeilingRefusesNoBinding) {
    EngineSession session(MakeNullConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallBindingVariables(engine);
    RegisterLeaf(harkentest::ObjectId(1));
    for (std::uint32_t i = 0; i < 9; ++i) {
        ASSERT_EQ(VN_OK, RegisterLinear(engine, harkentest::ObjectId(100 + i), harkentest::ObjectId(7),
                                        harken::kTargetScopeVoice,
                                        harken::kTargetVoiceVolume, 0.0f,
                                        -1.0f * static_cast<float>(i)));
    }
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    // Eight instances fill the voice's slots (voiceBindingInstanceSlots
    // default, section 16); the ninth refuses the no-binding code and
    // the log record names the instance pool.
    for (std::uint32_t i = 0; i < 8; ++i) {
        ASSERT_EQ(VN_OK, harken::ApplyVoiceBinding(*engine, handle, harkentest::ObjectId(100 + i)));
    }
    EXPECT_EQ(harken::kResultNoBinding,
              harken::ApplyVoiceBinding(*engine, handle, harkentest::ObjectId(108)));
    harken::LogRecord record{};
    ASSERT_TRUE(FindLogicLog(engine, harken::kLogCodeBindingPoolExhausted,
                             &record));
    EXPECT_EQ(harken::kBindingPoolVoiceInstances, record.arg0);
    // A second instance of an already-applied definition is a duplicate,
    // not a fresh slot.
    EXPECT_EQ(VN_IN_USE, harken::ApplyVoiceBinding(*engine, handle, harkentest::ObjectId(100)));
    // Section 12: instance-table high water and the ceiling refusal in
    // the snapshot tail (the duplicate refusal above is VN_IN_USE, not a
    // pool refusal, and must not count).
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    EXPECT_EQ(8u, snapshot.bindingInstanceHighWater);
    EXPECT_EQ(1u, snapshot.bindingInstanceRefusedCount);
}

// --- section 5.5: frozen structural identity ---

TEST(BindingPool, ReplaceStructuralIdentityIsFrozen) {
    EngineSession session(MakeNullConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallBindingVariables(engine);
    ASSERT_EQ(VN_OK, RegisterLinear(engine, harkentest::ObjectId(10), harkentest::ObjectId(7), harken::kTargetScopeVoice,
                                    harken::kTargetVoiceVolume, 0.0f, -6.0f,
                                    /*transitionMs=*/100));

    EXPECT_EQ(VN_UNKNOWN_ID,
              ReplaceLinear(engine, harkentest::ObjectId(99), harkentest::ObjectId(7), harken::kTargetScopeVoice,
                            harken::kTargetVoiceVolume, 0.0f, -12.0f));
    // variableId, targetScopeKind, targetId must equal the registered
    // definition's — re-pointing is detach + unregister + register.
    EXPECT_EQ(VN_INVALID_ARG,
              ReplaceLinear(engine, harkentest::ObjectId(10), harkentest::ObjectId(8), harken::kTargetScopeVoice,
                            harken::kTargetVoiceVolume, 0.0f, -12.0f));
    EXPECT_EQ(VN_INVALID_ARG,
              ReplaceLinear(engine, harkentest::ObjectId(10), harkentest::ObjectId(7), harken::kTargetScopeBus,
                            harken::kBusTargetBusVolume, 0.0f, -12.0f));
    EXPECT_EQ(VN_INVALID_ARG,
              ReplaceLinear(engine, harkentest::ObjectId(10), harkentest::ObjectId(7), harken::kTargetScopeVoice,
                            harken::kTargetVoicePitch, 0.0f, -12.0f));
    // The replacement itself validates fully (a bad curve refuses)...
    {
        const harken::CurvePoint pts[] = {{0.5f, 0.0f}, {0.5f, -6.0f}};
        const harken::CurveHandlePair handles[] = {{pts[0].x, pts[0].y, pts[1].x, pts[1].y}};
        EXPECT_EQ(VN_INVALID_ARG,
                  harken::ReplaceBinding(*engine, harkentest::ObjectId(10), harkentest::ObjectId(7),
                                        harken::kTargetScopeVoice,
                                        harken::kTargetVoiceVolume, pts,
                                        handles, 2, 100));
    }
    // ...and every refusal above left the current revision untouched.
    const harken::BindingTable::Definition* def = engine->bindings.Find(harkentest::ObjectId(10));
    ASSERT_NE(nullptr, def);
    EXPECT_EQ(1u, def->revision);
    EXPECT_FALSE(def->pendingInstall);

    // Only curve and transitionMs may change: the legal replace stages a
    // pending revision that installs at the NEXT update, not at the call.
    EXPECT_EQ(VN_OK, ReplaceLinear(engine, harkentest::ObjectId(10), harkentest::ObjectId(7), harken::kTargetScopeVoice,
                                   harken::kTargetVoiceVolume, 0.0f, -12.0f,
                                   /*transitionMs=*/250));
    def = engine->bindings.Find(harkentest::ObjectId(10));
    ASSERT_NE(nullptr, def);
    EXPECT_TRUE(def->pendingInstall);
    EXPECT_EQ(1u, def->revision);
    EXPECT_EQ(100u, def->transitionMs); // the live revision is unchanged
    ASSERT_EQ(VN_OK, Vn_Update());
    def = engine->bindings.Find(harkentest::ObjectId(10));
    ASSERT_NE(nullptr, def);
    EXPECT_FALSE(def->pendingInstall);
    EXPECT_EQ(2u, def->revision);
    EXPECT_EQ(250u, def->transitionMs);
    EXPECT_FLOAT_EQ(-12.0f, def->curve.Eval(1.0f));
}

// --- section 5.1: registration/attachment have no audible side effect ---

TEST(BindingPool, RegistrationAndAttachmentAreInaudible) {
    EngineSession session(MakeNullConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallBindingVariables(engine);
    RegisterLeaf(harkentest::ObjectId(1));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7), 1.0f));
    harkentest::AdvanceVariableWrite(engine);
    const std::uint64_t commitsBefore = engine->params.CommitCount();

    // Register + attach while the voice plays: a VOICE attachment
    // activates FUTURE plays only, so the live voice's resolved volume
    // and the commit stream stay untouched.
    ASSERT_EQ(VN_OK, RegisterLinear(engine, harkentest::ObjectId(10), harkentest::ObjectId(7), harken::kTargetScopeVoice,
                                    harken::kTargetVoiceVolume, 0.0f,
                                    -24.0f));
    ASSERT_EQ(VN_OK, harken::AttachBinding(*engine, harkentest::ObjectId(10),
                                          harken::kTargetScopeVoice, harkentest::ObjectId(1)));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    EXPECT_FLOAT_EQ(0.0f, engine->params.ResolvedValue(
                              RenderHandle(engine, handle),
                              harken::kTargetVoiceVolume));
    EXPECT_EQ(commitsBefore, engine->params.CommitCount());

    // A future play instantiates the attachment: its resolved volume
    // carries the curve at the current Variable value from the start.
    VnVoiceHandle second = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &second));
    EXPECT_FLOAT_EQ(-24.0f, engine->params.ResolvedValue(
                                RenderHandle(engine, second),
                                harken::kTargetVoiceVolume));
}

// --- section 8 / 5.3: bus attachments evaluate into the two-slot store ---

TEST(BindingPool, BusAttachmentEvaluatesIntoStoreAtGlobalScope) {
    EngineSession session(MakeNullConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallBindingVariables(engine);
    ASSERT_EQ(VN_OK, RegisterLinear(engine, harkentest::ObjectId(20), harkentest::ObjectId(7), harken::kTargetScopeBus,
                                    harken::kBusTargetBusVolume, 0.0f,
                                    -12.0f, /*transitionMs=*/100));
    ASSERT_EQ(VN_OK, RegisterLinear(engine, harkentest::ObjectId(21), harkentest::ObjectId(8), harken::kTargetScopeBus,
                                    harken::kBusTargetBusVolume, 0.0f,
                                    -6.0f));
    ASSERT_EQ(VN_OK, harken::AttachBinding(*engine, harkentest::ObjectId(20),
                                          harken::kTargetScopeBus,
                                          VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, harken::AttachBinding(*engine, harkentest::ObjectId(21),
                                          harken::kTargetScopeBus,
                                          VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7), 1.0f));
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(8), 1.0f));
    harkentest::AdvanceVariableWrite(engine);
    // Multiple bindings on one bus target accumulate in-domain inside the
    // Variable slot: -12 dB + -6 dB sum, on the neutral base 0.
    EXPECT_FLOAT_EQ(-18.0f,
                    engine->busTargets.Reduce(0, harken::kBusTargetBusVolume));

    // Detach removes the contribution at the next update.
    ASSERT_EQ(VN_OK, harken::DetachBinding(*engine, harkentest::ObjectId(21),
                                          harken::kTargetScopeBus,
                                          VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_FLOAT_EQ(-12.0f,
                    engine->busTargets.Reduce(0, harken::kBusTargetBusVolume));
    ASSERT_EQ(VN_OK, harken::DetachBinding(*engine, harkentest::ObjectId(20),
                                          harken::kTargetScopeBus,
                                          VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_Update());
    // With no attachments the slot never writes: the bus target reduces
    // to the neutral base and the flush publishes nothing for it — the
    // M5 no-binding guarantee's logic-side face.
    EXPECT_FALSE(engine->busTargets.SlotActive(
        0, harken::kBusTargetBusVolume, harken::BusTargetStore::kSlotVariableIndex));
    EXPECT_FLOAT_EQ(0.0f,
                    engine->busTargets.Reduce(0, harken::kBusTargetBusVolume));
}

TEST(BindingPool, BusOutputVolumeSetValueDrivesOutputGainNotBusGain) {
    EngineSession session(MakeNullConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallBindingVariables(engine);
    ASSERT_EQ(2u, harken::kBusTargetCount);
    ASSERT_EQ(VN_OK, RegisterLinear(engine, harkentest::ObjectId(20), harkentest::ObjectId(7), harken::kTargetScopeBus,
                                    harken::kBusTargetBusOutputVolume, 0.0f,
                                    -6.0f, /*transitionMs=*/20));
    ASSERT_EQ(VN_OK, harken::AttachBinding(*engine, harkentest::ObjectId(20),
                                          harken::kTargetScopeBus,
                                          VN_BUS_MASTER));
    const std::uint32_t master = engine->buses.FindIndex(VN_BUS_MASTER);
    ASSERT_NE(harken::kInvalidBusIndex, master);
    auto* bus = engine->buses.BusState(master);
    ASSERT_NE(bus, nullptr);
    const float gainBefore = bus->gain.target;
    const float outputBefore = bus->outputGain.target;

    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7), 1.0f));
    harkentest::AdvanceVariableWrite(engine);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());

    EXPECT_FLOAT_EQ(-6.0f, engine->busTargets.Reduce(
                               master, harken::kBusTargetBusOutputVolume));
    bus = engine->buses.BusState(master);
    ASSERT_NE(bus, nullptr);
    float expected = 0.0f;
    ASSERT_TRUE(harken::TryVolumeGainFromDb(-6.0f, &expected));
    EXPECT_NEAR(bus->outputGain.target, expected, 1e-5f);
    EXPECT_FLOAT_EQ(bus->gain.target, gainBefore);
    EXPECT_NE(bus->outputGain.target, outputBefore);
}

TEST(BindingPool, BusOutputVolumeBindingAddsToEngineeredValue) {
    EngineSession session(MakeNullConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallBindingVariables(engine);
    const std::uint32_t master = engine->buses.FindIndex(VN_BUS_MASTER);
    ASSERT_NE(harken::kInvalidBusIndex, master);
    auto* bus = engine->buses.BusState(master);
    ASSERT_NE(bus, nullptr);
    const float gainBefore = bus->gain.target;

    ASSERT_EQ(VN_OK, Vn_SetBusOutputVolume(VN_BUS_MASTER, -6.0f, 0));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    bus = engine->buses.BusState(master);
    ASSERT_NE(bus, nullptr);
    float engineered = 0.0f;
    ASSERT_TRUE(harken::TryVolumeGainFromDb(-6.0f, &engineered));
    EXPECT_NEAR(bus->outputGain.target, engineered, 1e-5f);

    ASSERT_EQ(VN_OK, RegisterLinear(engine, harkentest::ObjectId(20), harkentest::ObjectId(7), harken::kTargetScopeBus,
                                    harken::kBusTargetBusOutputVolume, 0.0f,
                                    -3.0f, /*transitionMs=*/20));
    ASSERT_EQ(VN_OK, harken::AttachBinding(*engine, harkentest::ObjectId(20),
                                          harken::kTargetScopeBus,
                                          VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7), 1.0f));
    harkentest::AdvanceVariableWrite(engine);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());

    EXPECT_FLOAT_EQ(-9.0f, engine->busTargets.Reduce(
                               master, harken::kBusTargetBusOutputVolume));
    bus = engine->buses.BusState(master);
    ASSERT_NE(bus, nullptr);
    float expected = 0.0f;
    ASSERT_TRUE(harken::TryVolumeGainFromDb(-9.0f, &expected));
    EXPECT_NEAR(bus->outputGain.target, expected, 1e-5f);
    EXPECT_FLOAT_EQ(bus->gain.target, gainBefore);
}

// --- section 9.2: target-level ramp aggregation at the instance seam ---

// Local definition storage standing in for the definition pool: the seam
// reads curve/transition/revision through pointers exactly as it reads
// the real pool's rows.
struct LocalDefinition {
    harken::Curve curve;
    std::uint32_t transitionMs = 0;
    std::uint32_t revision = 1;
};

harken::BindingDefinitionView ViewOf(const LocalDefinition& def,
                                    std::uint64_t id, VnVariableId variable,
                                    std::uint32_t targetId) {
    harken::BindingDefinitionView view;
    view.definitionId = harkentest::ObjectId(id);
    view.variableId = variable;
    view.targetId = targetId;
    view.curve = &def.curve;
    view.transitionMs = &def.transitionMs;
    view.revision = &def.revision;
    return view;
}

void SetLinear(harken::Curve* curve, float y0, float y1) {
    const harken::CurvePoint pts[] = {{0.0f, y0}, {1.0f, y1}};
    const harken::CurveHandlePair handles[] = {{pts[0].x, pts[0].y, pts[1].x, pts[1].y}};
    ASSERT_TRUE(curve->SetPoints(pts, handles, 2));
}

constexpr VnVoiceHandle kSeamVoice = (1ULL << 32) | 0u;

TEST(BindingPool, AggregationPublishesOneCommandWithMaxRamp) {
    harken::ParamSystem params(harken::EngineConfig{});
    params.OnVoiceStarted(kSeamVoice, 0.0f, 1.0);

    LocalDefinition slow;
    SetLinear(&slow.curve, 0.0f, -6.0f);
    slow.transitionMs = 40;
    LocalDefinition fast;
    SetLinear(&fast.curve, 0.0f, -3.0f);
    fast.transitionMs = 100;
    ASSERT_EQ(harken::ParamSystem::InstanceBindResult::Ok,
              params.BindVoiceInstance(
                  ViewOf(slow, 1, harkentest::ObjectId(7), harken::kTargetVoiceVolume), kSeamVoice,
                  harkentest::ObjectId(0), true));
    ASSERT_EQ(harken::ParamSystem::InstanceBindResult::Ok,
              params.BindVoiceInstance(
                  ViewOf(fast, 2, harkentest::ObjectId(7), harken::kTargetVoiceVolume), kSeamVoice,
                  harkentest::ObjectId(0), true));

    std::vector<harken::ParamCommit> commits;
    auto capture = [&commits](const harken::ParamCommit& commit) {
        commits.push_back(commit);
        return harken::FlushOutcome::Committed;
    };

    // Tick 1: both bindings' contributions change (first evaluation at a
    // fresh value) — per tick, per target, ONE command, rampMs = max of
    // the changed contributors' transitions.
    ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(7), 1.0f));
    params.FlushCommits(capture);
    ASSERT_EQ(1u, commits.size());
    EXPECT_EQ(harken::kTargetVoiceVolume, commits[0].targetId);
    EXPECT_FLOAT_EQ(-9.0f, commits[0].resolved);
    EXPECT_EQ(100u, commits[0].rampMs);

    // Tick 2: nothing changed — a tick that reduces to the last committed
    // value publishes nothing and starts no ramp.
    commits.clear();
    params.FlushCommits(capture);
    EXPECT_TRUE(commits.empty());

    // Tick 3: both bindings change again AND an API-direct change lands
    // the same tick: the single command's rampMs is the max over the
    // bindings' transitions and the API-direct rampMs.
    (void)params.SetApiDirect(kSeamVoice, harken::kTargetVoiceVolume, -1.0f,
                              /*rampMs=*/250);
    ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(7), 0.5f));
    params.FlushCommits(capture);
    ASSERT_EQ(1u, commits.size());
    EXPECT_NEAR(-4.863021f, commits[0].resolved, 0.0001f);
    EXPECT_EQ(250u, commits[0].rampMs);

    // Tick 4: only the bindings move — the API-direct stamp expired with
    // its tick, so the max falls back to the bindings' 100.
    commits.clear();
    ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(7), 1.0f));
    params.FlushCommits(capture);
    ASSERT_EQ(1u, commits.size());
    EXPECT_EQ(100u, commits[0].rampMs);
}

TEST(BindingPool, PrimedInitialInstantiationSnapsAndFlushIsQuiet) {
    harken::ParamSystem params(harken::EngineConfig{});
    ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(7), 1.0f));
    params.OnVoiceStarted(kSeamVoice, 0.0f, 1.0);

    LocalDefinition def;
    SetLinear(&def.curve, 0.0f, -6.0f);
    def.transitionMs = 500;
    ASSERT_EQ(harken::ParamSystem::InstanceBindResult::Ok,
              params.BindVoiceInstance(
                  ViewOf(def, 1, harkentest::ObjectId(7), harken::kTargetVoiceVolume), kSeamVoice,
                  harkentest::ObjectId(0), false));

    // Priming resolves the initial value for the start batch (the caller
    // publishes it with rampMs 0 — initial instantiation snaps)...
    harken::ParamSystem::PrimedValue primed[harken::kBuiltinTargetCount];
    const std::uint32_t count = params.PrimeVoiceBindings(
        kSeamVoice, nullptr, primed, harken::kBuiltinTargetCount);
    ASSERT_EQ(1u, count);
    EXPECT_EQ(harken::kTargetVoiceVolume, primed[0].targetId);
    EXPECT_FLOAT_EQ(-6.0f, primed[0].resolved);

    // ...and the tick's ordinary flush sees no spurious change: nothing
    // re-publishes and no activation ramp is folded.
    std::vector<harken::ParamCommit> commits;
    params.FlushCommits([&commits](const harken::ParamCommit& commit) {
        commits.push_back(commit);
        return harken::FlushOutcome::Committed;
    });
    EXPECT_TRUE(commits.empty());

    // A later input change rides the ordinary path with the definition's
    // transition.
    ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(7), 0.0f));
    params.FlushCommits([&commits](const harken::ParamCommit& commit) {
        commits.push_back(commit);
        return harken::FlushOutcome::Committed;
    });
    ASSERT_EQ(1u, commits.size());
    EXPECT_FLOAT_EQ(0.0f, commits[0].resolved);
    EXPECT_EQ(500u, commits[0].rampMs);
}

// --- section 9.2: a deferred commit keeps its aggregated ramp ---

TEST(BindingPool, RetryKeepsAggregatedRampForTheDeferredCommit) {
    harken::ParamSystem params(harken::EngineConfig{});
    ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(7), 0.2f));
    params.OnVoiceStarted(kSeamVoice, 0.0f, 1.0);
    LocalDefinition def;
    SetLinear(&def.curve, 0.0f, -6.0f);
    def.transitionMs = 100;
    ASSERT_EQ(harken::ParamSystem::InstanceBindResult::Ok,
              params.BindVoiceInstance(
                  ViewOf(def, 1, harkentest::ObjectId(7), harken::kTargetVoiceVolume), kSeamVoice,
                  harkentest::ObjectId(0), true));
    // Same tick: the binding's activation folds 100, an API-direct
    // change contributes 250 — the aggregate is 250.
    (void)params.SetApiDirect(kSeamVoice, harken::kTargetVoiceVolume, -1.0f,
                              /*rampMs=*/250);

    // Tick 1 defers (a full queue / a staged publication batch): the
    // commit is handed over with the full aggregate but not consumed.
    std::vector<harken::ParamCommit> commits;
    params.FlushCommits([&commits](const harken::ParamCommit& commit) {
        commits.push_back(commit);
        return harken::FlushOutcome::Retry;
    });
    ASSERT_EQ(1u, commits.size());
    EXPECT_EQ(250u, commits[0].rampMs);

    // Tick 2 retries the SAME publication: the API-direct term's tick
    // stamp has expired, but the deferred commit's aggregate survived
    // with the dirty mark — the retried command still carries 250, not
    // a degraded 100.
    commits.clear();
    params.FlushCommits([&commits](const harken::ParamCommit& commit) {
        commits.push_back(commit);
        return harken::FlushOutcome::Committed;
    });
    ASSERT_EQ(1u, commits.size());
    EXPECT_EQ(250u, commits[0].rampMs);
}

// --- section 9.2: only a CHANGED contribution folds its transition ---

TEST(BindingPool, FlatCurveSegmentDoesNotFoldItsTransition) {
    harken::ParamSystem params(harken::EngineConfig{});
    ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(7), 0.2f));
    params.OnVoiceStarted(kSeamVoice, 0.0f, 1.0);
    // A flat curve with a LONG transition and a sloped curve with a
    // short one, on the same target and Variable.
    LocalDefinition flat;
    SetLinear(&flat.curve, -6.0f, -6.0f);
    flat.transitionMs = 500;
    LocalDefinition sloped;
    SetLinear(&sloped.curve, 0.0f, -6.0f);
    sloped.transitionMs = 100;
    ASSERT_EQ(harken::ParamSystem::InstanceBindResult::Ok,
              params.BindVoiceInstance(
                  ViewOf(flat, 1, harkentest::ObjectId(7), harken::kTargetVoiceVolume), kSeamVoice,
                  harkentest::ObjectId(0), true));
    ASSERT_EQ(harken::ParamSystem::InstanceBindResult::Ok,
              params.BindVoiceInstance(
                  ViewOf(sloped, 2, harkentest::ObjectId(7), harken::kTargetVoiceVolume),
                  kSeamVoice, harkentest::ObjectId(0), true));

    // Activation tick: both contributions appear — the max legitimately
    // includes the flat curve's 500.
    std::vector<harken::ParamCommit> commits;
    auto capture = [&commits](const harken::ParamCommit& commit) {
        commits.push_back(commit);
        return harken::FlushOutcome::Committed;
    };
    params.FlushCommits(capture);
    ASSERT_EQ(1u, commits.size());
    EXPECT_EQ(500u, commits[0].rampMs);

    // The input moves ACROSS the flat span: the flat binding
    // re-evaluates but its OUTPUT is unchanged, so section 9.2 folds
    // only the sloped binding's 100 — "every binding whose contribution
    // changed", not every binding whose input moved.
    commits.clear();
    ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(7), 0.8f));
    params.FlushCommits(capture);
    ASSERT_EQ(1u, commits.size());
    EXPECT_NEAR(-10.423237f, commits[0].resolved, 0.0001f);
    EXPECT_EQ(100u, commits[0].rampMs);
}

// --- section 10.4: purge releases once per unique (voice, target) ---

TEST(BindingPool, PurgeReleasesWorstCaseOncePerUniquePair) {
    harken::ParamSystem params(harken::EngineConfig{});
    harken::PropertyPublisher publisher(/*batchCapacity=*/8);
    harken::LogRing log;
    params.AttachPublicationCapacity(&publisher, &log);
    ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(7), 1.0f));
    params.OnVoiceStarted(kSeamVoice, 0.0f, 1.0);

    LocalDefinition def;
    SetLinear(&def.curve, 0.0f, -6.0f);
    // Apply -> clear -> apply: the neutralized leftover and the live
    // re-apply are TWO entries on ONE (voice, target) pair whose first
    // bind grew the worst case exactly once.
    ASSERT_EQ(harken::ParamSystem::InstanceBindResult::Ok,
              params.BindVoiceInstance(
                  ViewOf(def, 1, harkentest::ObjectId(7), harken::kTargetVoiceVolume), kSeamVoice,
                  harkentest::ObjectId(0), true));
    EXPECT_EQ(1u, publisher.WorstCaseCommits());
    LocalDefinition secondDef;
    SetLinear(&secondDef.curve, 0.0f, -3.0f);
    ASSERT_EQ(harken::ParamSystem::InstanceBindResult::Ok,
              params.BindVoiceInstance(
                  ViewOf(secondDef, 2, harkentest::ObjectId(7),
                         harken::kTargetVoiceVolume),
                  kSeamVoice, harkentest::ObjectId(0), true));
    EXPECT_EQ(1u, publisher.WorstCaseCommits());
    ASSERT_TRUE(params.NeutralizeVoiceInstance(kSeamVoice, harkentest::ObjectId(1)));
    ASSERT_EQ(harken::ParamSystem::InstanceBindResult::Ok,
              params.BindVoiceInstance(
                  ViewOf(def, 1, harkentest::ObjectId(7), harken::kTargetVoiceVolume), kSeamVoice,
                  harkentest::ObjectId(0), true));
    EXPECT_EQ(1u, publisher.WorstCaseCommits());
    // A second unique pair whose demand must survive the purge.
    harken::Curve curve;
    SetLinear(&curve, 0.0f, -12.0f);
    ASSERT_TRUE(params.BindVoiceTarget(harkentest::ObjectId(9), kSeamVoice,
                                       harken::kTargetVoicePitch, curve));
    EXPECT_EQ(2u, publisher.WorstCaseCommits());

    harken::ParamSystem::PreparedPurgeToken purge;
    const VnObjectId purgeIds[] = {harkentest::ObjectId(1), harkentest::ObjectId(2)};
    EXPECT_EQ(VN_NO_BINDING,
              params.PreparePurgeInstancesOf(
                  purgeIds, 2, nullptr, 0, &purge));
    EXPECT_FALSE(purge.IsPrepared());
    EXPECT_EQ(2u, publisher.WorstCaseCommits());

    harken::ParamSystem::PreparedPurgeExit exits[4];
    ASSERT_EQ(VN_OK,
              params.PreparePurgeInstancesOf(
                  purgeIds, 2, exits, 4, &purge));
    ASSERT_TRUE(purge.IsPrepared());
    EXPECT_EQ(1u, purge.ExitCount());
    // The exact batch purge removes every selected-definition volume entry
    // but releases their shared pair's seat once; the pitch seat survives.
    params.CommitPreparedPurgeInstances(&purge);
    EXPECT_EQ(1u, publisher.WorstCaseCommits());
}

// --- section 5.3: detach then unregister in one tick still exits ---

TEST(BindingPool, DetachThenUnregisterSameTickStillPublishesExit) {
    EngineSession session(MakeNullConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallBindingVariables(engine);
    RegisterLeaf(harkentest::ObjectId(1));
    ASSERT_EQ(VN_OK, RegisterLinear(engine, harkentest::ObjectId(10), harkentest::ObjectId(7), harken::kTargetScopeVoice,
                                    harken::kTargetVoiceVolume, -12.0f,
                                    -12.0f, /*transitionMs=*/300));
    ASSERT_EQ(VN_OK, harken::AttachBinding(*engine, harkentest::ObjectId(10),
                                          harken::kTargetScopeVoice, harkentest::ObjectId(1)));
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7), 1.0f));
    harkentest::AdvanceVariableWrite(engine);
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    EXPECT_FLOAT_EQ(-12.0f, engine->params.ResolvedValue(
                                RenderHandle(engine, handle),
                                harken::kTargetVoiceVolume));

    // Detach neutralizes the instance; the same-tick unregister succeeds
    // (a neutralized instance no longer references the curve) and purges
    // the entry BEFORE its exit evaluation ever ran. The departed
    // contribution must still leave: the next update rebuilds the slot
    // from the (empty) survivor set and publishes the exit — no -12 dB
    // residue haunting the voice forever.
    ASSERT_EQ(VN_OK, harken::DetachBinding(*engine, harkentest::ObjectId(10),
                                          harken::kTargetScopeVoice, harkentest::ObjectId(1)));
    ASSERT_EQ(VN_OK, harken::UnregisterBinding(*engine, harkentest::ObjectId(10)));
    EXPECT_EQ(nullptr, engine->bindings.Find(harkentest::ObjectId(10)));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_FLOAT_EQ(0.0f, engine->params.ResolvedValue(
                              RenderHandle(engine, handle),
                              harken::kTargetVoiceVolume));
}

// --- section 5.3: played-id and leaf-id attachments both instantiate ---

TEST(ParamCommand, PitchBindingCopiesRampMs) {
    EngineSession session(MakeNullConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::ParamCommit commit{};
    commit.handle = 1;
    commit.targetId = harken::kTargetVoicePitch;
    commit.resolved = 0.0f;
    commit.rampMs = 20;
    harken::CommandRecord command{};
    ASSERT_TRUE(harken::MakeVariableParamCommand(*session.Engine(), commit,
                                                &command));
    EXPECT_EQ(harken::kOpcodeSetVoicePitch, command.opcode);
    EXPECT_EQ(20u, command.payload.setVoicePitch.rampMs);
}

} // namespace
