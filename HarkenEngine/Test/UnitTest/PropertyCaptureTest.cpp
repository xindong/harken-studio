// Attribution capture: the two
// caller-buffer verbs explain the property system's current state as
// data. Coverage here:
//   - the acceptance attribution golden: a -18 dB bus decomposes into
//     base 0 + API-direct -6 + Variable-slot -12, with the binding row
//     explaining where the -12 comes from;
//   - voice-scope rows (API-direct + apply-born instance) and the
//     neutralizing flag after a clear;
//   - the meter-owned-source flag on sidechain rows;
//   - PENDING rows for instance identities held by parked voices;
//   - the pending-publication window between a Variable write and the
//     update that realizes it;
//   - the truncation contract: requiredCount always written, VN_OK with
//     the prefix written, counted once, logged with the named code, and
//     the capacity-0 probe not counted;
//   - the refusal matrix (NULL requiredCount, NULL buffer under nonzero
//     capacity, unknown row vintage).
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include "harken/core/LogRing.h"
#include "harken/param/BindingTable.h"
#include "harken/param/ParamTargetTable.h"
#include "harken/param/ParamTargets.h"

#include <cstddef>
#include <cstdint>
#include <vector>

// Row shapes are frozen in the public header (section 12); the pins
// mirror the binding-desc discipline so a drift is a compile error here
// as well as in the generated binding pin.
static_assert(sizeof(VnPropertyTargetInfo) == 64 &&
                  offsetof(VnPropertyTargetInfo, structSize) == 0 &&
                  offsetof(VnPropertyTargetInfo, objectId) == 8 &&
                  offsetof(VnPropertyTargetInfo, base) == 40 &&
                  offsetof(VnPropertyTargetInfo, finalValue) == 52 &&
                  offsetof(VnPropertyTargetInfo, lastPublishedRampMs) == 60,
              "VnPropertyTargetInfo row shape is frozen");
static_assert(sizeof(VnPropertyBindingInfo) == 88 &&
                  offsetof(VnPropertyBindingInfo, stateFlags) == 4 &&
                  offsetof(VnPropertyBindingInfo, bindingId) == 8 &&
                  offsetof(VnPropertyBindingInfo, variableId) == 24 &&
                  offsetof(VnPropertyBindingInfo, objectId) == 48 &&
                  offsetof(VnPropertyBindingInfo, currentContribution) == 84,
              "VnPropertyBindingInfo row shape is frozen");

namespace {

using harkentest::EngineSession;
using harkentest::FindLogicLog;
using harkentest::MakeDC;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::RenderHandle;

constexpr std::uint32_t kBlock = 256;
constexpr std::uint32_t kRate = 48000;

void RegisterLeaf(VnSoundId id) {
    static const std::vector<float> dc = MakeDC(kRate, 0.5f);
    const VnPCMDesc desc = harkentest::MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(id, &desc));
}

// Two-point linear definition through the engine seam: variable in
// [0, 1] maps to [y0, y1] on the scoped target.
VnResult RegisterLinear(harken::Engine* engine, harken::BindingId id,
                        VnVariableId variableId, std::uint32_t scopeKind,
                        std::uint32_t targetId, float y0, float y1,
                        std::uint32_t transitionMs = 0) {
    const harken::CurvePoint pts[] = {{0.0f, y0}, {1.0f, y1}};
    const harken::CurveHandlePair handles[] = {{pts[0].x, pts[0].y, pts[1].x, pts[1].y}};
    return harken::RegisterBinding(*engine, id, variableId, scopeKind,
                                  targetId, pts, handles, 2, transitionMs);
}

// Fetches every target row into a vector sized by the probe (the
// documented two-call idiom).
std::vector<VnPropertyTargetInfo> CaptureTargets(std::uint32_t* required) {
    *required = 0;
    EXPECT_EQ(VN_OK, Vn_CapturePropertyTargets(nullptr, 0, required));
    std::vector<VnPropertyTargetInfo> rows(*required > 0 ? *required : 1);
    rows[0].structSize = sizeof(VnPropertyTargetInfo);
    std::uint32_t got = 0;
    EXPECT_EQ(VN_OK, Vn_CapturePropertyTargets(
                         rows.data(),
                         static_cast<std::uint32_t>(rows.size()), &got));
    EXPECT_EQ(*required, got);
    rows.resize(*required);
    return rows;
}

std::vector<VnPropertyBindingInfo> CaptureBindings(std::uint32_t* required) {
    *required = 0;
    EXPECT_EQ(VN_OK, Vn_CapturePropertyBindings(nullptr, 0, required));
    std::vector<VnPropertyBindingInfo> rows(*required > 0 ? *required : 1);
    rows[0].structSize = sizeof(VnPropertyBindingInfo);
    std::uint32_t got = 0;
    EXPECT_EQ(VN_OK, Vn_CapturePropertyBindings(
                         rows.data(),
                         static_cast<std::uint32_t>(rows.size()), &got));
    EXPECT_EQ(*required, got);
    rows.resize(*required);
    return rows;
}

const VnPropertyTargetInfo* FindTargetRow(
    const std::vector<VnPropertyTargetInfo>& rows, std::uint32_t scopeKind,
    VnObjectId objectId, std::uint32_t targetId, std::uint64_t instanceHandle = 0) {
    for (const VnPropertyTargetInfo& row : rows) {
        if (row.scopeKind == scopeKind && row.objectId == objectId && row.instanceHandle == instanceHandle &&
            row.targetId == targetId) {
            return &row;
        }
    }
    return nullptr;
}

const VnPropertyBindingInfo* FindBindingRow(
    const std::vector<VnPropertyBindingInfo>& rows, VnObjectId bindingId,
    VnObjectId objectId, std::uint64_t instanceHandle = 0) {
    for (const VnPropertyBindingInfo& row : rows) {
        if (row.bindingId == bindingId && row.objectId == objectId && row.instanceHandle == instanceHandle) {
            return &row;
        }
    }
    return nullptr;
}

double Cubic(double p0, double p1, double p2, double p3, double t) {
    const double u = 1.0 - t;
    return u * u * u * p0 + 3.0 * u * u * t * p1 +
           3.0 * u * t * t * p2 + t * t * t * p3;
}

double BindingOracle(double x, const VnCurvePoint& a,
                     const VnCurveHandlePair& handle,
                     const VnCurvePoint& b) {
    // Independent 200-iteration double bisection derives the expected
    // contribution from the authored absolute Bezier controls.
    double lo = 0.0;
    double hi = 1.0;
    for (int i = 0; i < 200; ++i) {
        const double mid = (lo + hi) * 0.5;
        if (Cubic(a.x, handle.h1x, handle.h2x, b.x, mid) < x) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return Cubic(a.y, handle.h1y, handle.h2y, b.y, (lo + hi) * 0.5);
}

// --- acceptance item 10: "why is Music at -18 dB?" ---

TEST(PropertyCapture, ExplainsMinusEighteenAsBasePlusApiPlusVariable) {
    EngineSession session(MakeNullConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(
                  engine, {harkentest::ObjectId(7)}, 0.0f, 1.0f).status);
    constexpr VnBusId kMusic = harkentest::ObjectId(100);
    constexpr VnVariableId kSidechainVar = harkentest::ObjectId(7);
    constexpr harken::BindingId kSidechain = harkentest::ObjectId(10);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kMusic, VN_BUS_MASTER));
    // API-direct -6 dB (its own ramp), plus a Variable-driven -12 dB
    // (transition 250 ms) through one BusVolume binding at input 1.0.
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(kMusic, -6.0f, 50));
    ASSERT_EQ(VN_OK, RegisterLinear(engine, kSidechain, kSidechainVar,
                                    harken::kTargetScopeBus,
                                    harken::kBusTargetBusVolume, 0.0f,
                                    -12.0f, 250));
    ASSERT_EQ(VN_OK, harken::AttachBinding(*engine, kSidechain,
                                          harken::kTargetScopeBus, kMusic));
    ASSERT_EQ(VN_OK, Vn_SetValue(kSidechainVar, 1.0f));
    harkentest::AdvanceVariableWrite(engine);

    // Target row: the -18 decomposes exactly as base + API + Variable.
    std::uint32_t required = 0;
    const std::vector<VnPropertyTargetInfo> targets =
        CaptureTargets(&required);
    ASSERT_EQ(1u, required); // the untouched master publishes no row
    const VnPropertyTargetInfo* music = FindTargetRow(
        targets, VN_TARGET_SCOPE_BUS, kMusic, harken::kBusTargetBusVolume);
    ASSERT_NE(nullptr, music);
    EXPECT_EQ(sizeof(VnPropertyTargetInfo), music->structSize);
    EXPECT_FLOAT_EQ(0.0f, music->base);
    EXPECT_FLOAT_EQ(-6.0f, music->apiDirectValue);
    EXPECT_FLOAT_EQ(-12.0f, music->variableValue);
    EXPECT_FLOAT_EQ(-18.0f, music->finalValue);
    EXPECT_EQ(0u, music->pendingPublication); // published, quiescent
    EXPECT_EQ(1u, music->activeBindingCount);
    // The tick's single command carried the section-9.2 aggregate:
    // max(binding transition 250, same-tick API ramp 50).
    EXPECT_EQ(250u, music->lastPublishedRampMs);

    // Binding row: the -12 is this binding, this bus, input 1.0.
    const std::vector<VnPropertyBindingInfo> bindings =
        CaptureBindings(&required);
    ASSERT_EQ(1u, required);
    const VnPropertyBindingInfo* sidechain =
        FindBindingRow(bindings, kSidechain, kMusic);
    ASSERT_NE(nullptr, sidechain);
    EXPECT_EQ(sizeof(VnPropertyBindingInfo), sidechain->structSize);
    EXPECT_EQ(kSidechainVar, sidechain->variableId);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_TARGET_SCOPE_BUS),
              sidechain->scopeKind);
    EXPECT_EQ(harken::kBusTargetBusVolume, sidechain->targetId);
    EXPECT_EQ(1u, sidechain->definitionRevision);
    EXPECT_EQ(250u, sidechain->transitionMs);
    EXPECT_FLOAT_EQ(1.0f, sidechain->currentInput);
    EXPECT_FLOAT_EQ(-12.0f, sidechain->currentContribution);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_PROPERTY_BINDING_ACTIVE),
              sidechain->stateFlags);
}

TEST(PropertyCapture, BindingRowReportsBezierContribution) {
    EngineSession session(MakeNullConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    constexpr VnBusId kMusic = harkentest::ObjectId(100);
    constexpr VnVariableId kVariable = harkentest::ObjectId(7);
    constexpr VnBindingId kBinding = harkentest::ObjectId(10);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(
                  engine, {kVariable}, 0.0f, 1.0f).status);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kMusic, VN_BUS_MASTER));

    VnBindingDesc desc{};
    desc.structSize = sizeof(VnBindingDesc);
    desc.variableId = kVariable;
    desc.targetScopeKind = VN_TARGET_SCOPE_BUS;
    desc.targetId = harken::kBusTargetBusVolume;
    desc.curve.structSize = sizeof(VnCurveDesc);
    desc.curve.pointCount = 2;
    desc.curve.points[0] = {0.0f, 0.0f};
    desc.curve.points[1] = {1.0f, -24.0f};
    desc.curve.handles[0] = {0.1f, -4.0f, 0.8f, -20.0f};
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(kBinding, &desc));
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(VnBindingAttachDesc);
    attach.scopeKind = VN_TARGET_SCOPE_BUS;
    attach.objectId = kMusic;
    ASSERT_EQ(VN_OK, Vn_AttachBinding(kBinding, &attach));
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 0.25f));
    harkentest::AdvanceVariableWrite(engine);

    std::uint32_t required = 0;
    const std::vector<VnPropertyBindingInfo> bindings =
        CaptureBindings(&required);
    const VnPropertyBindingInfo* row =
        FindBindingRow(bindings, kBinding, kMusic);
    ASSERT_NE(nullptr, row);
    EXPECT_FLOAT_EQ(0.25f, row->currentInput);
    const auto amplitude = [](float db) {
        return static_cast<float>(std::pow(10.0, static_cast<double>(db) / 20.0));
    };
    const double expected = 20.0 * std::log10(BindingOracle(0.25,
        {0.0f, amplitude(0.0f)}, {0.1f, amplitude(-4.0f), 0.8f, amplitude(-20.0f)},
        {1.0f, amplitude(-24.0f)}));
    EXPECT_NEAR(static_cast<float>(expected), row->currentContribution,
                3.0e-5f);
}

TEST(PropertyCapture, PendingPublicationMarksTheUnrealizedWriteWindow) {
    EngineSession session(MakeNullConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(
                  engine, {harkentest::ObjectId(7)}, 0.0f, 1.0f).status);
    constexpr VnBusId kMusic = harkentest::ObjectId(100);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kMusic, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, RegisterLinear(engine, harkentest::ObjectId(10), harkentest::ObjectId(7), harken::kTargetScopeBus,
                                    harken::kBusTargetBusVolume, 0.0f,
                                    -12.0f));
    ASSERT_EQ(VN_OK, harken::AttachBinding(*engine, harkentest::ObjectId(10),
                                          harken::kTargetScopeBus, kMusic));
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7), 1.0f));
    harkentest::AdvanceVariableWrite(engine);

    // A fresh accepted write has not crossed its actual block yet. Capture
    // still reports the last realized target and binding values.
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7), 0.5f));
    std::uint32_t required = 0;
    std::vector<VnPropertyTargetInfo> targets = CaptureTargets(&required);
    const VnPropertyTargetInfo* music = FindTargetRow(
        targets, VN_TARGET_SCOPE_BUS, kMusic, harken::kBusTargetBusVolume);
    ASSERT_NE(nullptr, music);
    EXPECT_EQ(1u, music->pendingPublication);
    EXPECT_FLOAT_EQ(-12.0f, music->variableValue); // last realized
    std::vector<VnPropertyBindingInfo> bindings = CaptureBindings(&required);
    const VnPropertyBindingInfo* sidechain = FindBindingRow(bindings, harkentest::ObjectId(10), kMusic);
    ASSERT_NE(nullptr, sidechain);
    EXPECT_FLOAT_EQ(1.0f, sidechain->currentInput);
    EXPECT_FLOAT_EQ(-12.0f, sidechain->currentContribution);

    // A logic update may consume a conservative input mark, but the durable
    // write is still known incoming until its effective audio block crosses.
    ASSERT_EQ(VN_OK, Vn_Update());
    targets = CaptureTargets(&required);
    music = FindTargetRow(targets, VN_TARGET_SCOPE_BUS, kMusic,
                          harken::kBusTargetBusVolume);
    ASSERT_NE(nullptr, music);
    EXPECT_EQ(1u, music->pendingPublication);
    EXPECT_FLOAT_EQ(-12.0f, music->variableValue);
    bindings = CaptureBindings(&required);
    sidechain = FindBindingRow(bindings, harkentest::ObjectId(10), kMusic);
    ASSERT_NE(nullptr, sidechain);
    EXPECT_FLOAT_EQ(1.0f, sidechain->currentInput);
    EXPECT_FLOAT_EQ(-12.0f, sidechain->currentContribution);

    // The actual block advances the accepted write and its publication.
    harkentest::AdvanceVariableWrite(engine);
    targets = CaptureTargets(&required);
    music = FindTargetRow(targets, VN_TARGET_SCOPE_BUS, kMusic,
                          harken::kBusTargetBusVolume);
    ASSERT_NE(nullptr, music);
    EXPECT_EQ(0u, music->pendingPublication);
    EXPECT_NEAR(-4.074144f, music->variableValue, 1.0e-5f);
    EXPECT_NEAR(-4.074144f, music->finalValue, 1.0e-5f);
}

TEST(PropertyCapture,
     VoicePendingPublicationRespectsEmitterScopeAndAppliedOverrides) {
    VnConfig config = MakeNullConfig(4, kBlock, kRate);
    config.maxEmitters = 3;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(
                  engine, {harkentest::ObjectId(7)}, 0.0f, 1.0f, 0.0f).status);
    RegisterLeaf(harkentest::ObjectId(31));
    RegisterLeaf(harkentest::ObjectId(32));
    ASSERT_EQ(VN_OK, RegisterLinear(engine, harkentest::ObjectId(20), harkentest::ObjectId(7),
                                    harken::kTargetScopeVoice,
                                    harken::kTargetVoiceVolume, 0.0f,
                                    -12.0f));
    VnEmitterHandle firstEmitter = 0;
    VnEmitterHandle secondEmitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&firstEmitter));
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&secondEmitter));
    VnPlayDesc firstPlay = MakePlayDesc(harkentest::ObjectId(31));
    firstPlay.emitterHandle = firstEmitter;
    VnPlayDesc secondPlay = MakePlayDesc(harkentest::ObjectId(32));
    secondPlay.emitterHandle = secondEmitter;
    VnVoiceHandle firstVoice = 0;
    VnVoiceHandle secondVoice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&firstPlay, &firstVoice));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&secondPlay, &secondVoice));
    ASSERT_EQ(VN_OK, harken::ApplyVoiceBinding(*engine, firstVoice, harkentest::ObjectId(20)));
    ASSERT_EQ(VN_OK, harken::ApplyVoiceBinding(*engine, secondVoice, harkentest::ObjectId(20)));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());

    const auto expectPending = [&](std::uint32_t first,
                                   std::uint32_t second) {
        std::uint32_t required = 0;
        const std::vector<VnPropertyTargetInfo> targets =
            CaptureTargets(&required);
        const VnPropertyTargetInfo* firstVolume = FindTargetRow(targets, VN_TARGET_SCOPE_VOICE, {}, harken::kTargetVoiceVolume, firstVoice);
        const VnPropertyTargetInfo* secondVolume = FindTargetRow(targets, VN_TARGET_SCOPE_VOICE, {}, harken::kTargetVoiceVolume, secondVoice);
        ASSERT_NE(nullptr, firstVolume);
        ASSERT_NE(nullptr, secondVolume);
        EXPECT_EQ(first, firstVolume->pendingPublication);
        EXPECT_EQ(second, secondVolume->pendingPublication);
    };

    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(firstEmitter, harkentest::ObjectId(7), 1.0f));
    expectPending(1, 0);
    ASSERT_EQ(VN_OK, Vn_Update());
    expectPending(1, 0);
    harkentest::AdvanceVariableWrite(engine);
    expectPending(0, 0);

    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7), 0.5f));
    expectPending(1, 1);
    ASSERT_EQ(VN_OK, Vn_Update());
    expectPending(1, 1);
    float localValue = 0.0f;
    ASSERT_TRUE(engine->emitters.LocalValue(firstEmitter, harkentest::ObjectId(7), &localValue));
    EXPECT_FLOAT_EQ(1.0f, localValue);
    harkentest::AdvanceVariableWrite(engine);
    expectPending(0, 0);
    EXPECT_FALSE(engine->emitters.LocalValue(firstEmitter, harkentest::ObjectId(7), &localValue));

    std::uint32_t required = 0;
    const std::vector<VnPropertyBindingInfo> bindings = CaptureBindings(&required);
    for (const VnVoiceHandle voice : {firstVoice, secondVoice}) {
        const VnPropertyBindingInfo* binding =
            FindBindingRow(bindings, harkentest::ObjectId(20), {}, voice);
        ASSERT_NE(nullptr, binding);
        EXPECT_FLOAT_EQ(0.5f, binding->currentInput);
    }
}

// --- voice-scope rows: API-direct + apply-born instance, then clear ---

TEST(PropertyCapture, VoiceTargetAndInstanceRowsExplainAndNeutralize) {
    EngineSession session(MakeNullConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(
                  engine, {harkentest::ObjectId(7)}, 0.0f, 1.0f).status);
    RegisterLeaf(harkentest::ObjectId(1));
    ASSERT_EQ(VN_OK, RegisterLinear(engine, harkentest::ObjectId(20), harkentest::ObjectId(7),
                                    harken::kTargetScopeVoice,
                                    harken::kTargetVoiceVolume, 0.0f,
                                    -9.0f));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(handle, -3.0f, 0));
    ASSERT_EQ(VN_OK, harken::ApplyVoiceBinding(*engine, handle, harkentest::ObjectId(20)));
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7), 1.0f));
    harkentest::AdvanceVariableWrite(engine);

    std::uint32_t required = 0;
    std::vector<VnPropertyTargetInfo> targets = CaptureTargets(&required);
    const VnPropertyTargetInfo* volume = FindTargetRow(targets, VN_TARGET_SCOPE_VOICE, {}, harken::kTargetVoiceVolume, handle);
    ASSERT_NE(nullptr, volume);
    EXPECT_FLOAT_EQ(0.0f, volume->base);
    EXPECT_FLOAT_EQ(-3.0f, volume->apiDirectValue);
    EXPECT_FLOAT_EQ(-9.0f, volume->variableValue);
    EXPECT_FLOAT_EQ(-12.0f, volume->finalValue);
    EXPECT_EQ(1u, volume->activeBindingCount);

    std::vector<VnPropertyBindingInfo> bindings = CaptureBindings(&required);
    const VnPropertyBindingInfo* instance =
        FindBindingRow(bindings, harkentest::ObjectId(20), {}, handle);
    ASSERT_NE(nullptr, instance);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_TARGET_SCOPE_VOICE),
              instance->scopeKind);
    EXPECT_EQ(harken::kTargetVoiceVolume, instance->targetId);
    EXPECT_FLOAT_EQ(1.0f, instance->currentInput);
    EXPECT_FLOAT_EQ(-9.0f, instance->currentContribution);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_PROPERTY_BINDING_ACTIVE),
              instance->stateFlags);

    // Clearing neutralizes the instance. INSIDE the window — after the
    // clear, before the exit evaluation — the row flips to NEUTRALIZING
    // but keeps reporting its in-force contribution, because the target
    // row's Variable slot still folds it: binding rows must sum to
    // target rows in every phase (the section-12 attribution
    // invariant), and the target row flags the pending exit.
    ASSERT_EQ(VN_OK, harken::ClearVoiceBinding(*engine, handle, harkentest::ObjectId(20)));
    bindings = CaptureBindings(&required);
    instance = FindBindingRow(bindings, harkentest::ObjectId(20), {}, handle);
    ASSERT_NE(nullptr, instance);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_PROPERTY_BINDING_NEUTRALIZING),
              instance->stateFlags);
    EXPECT_FLOAT_EQ(-9.0f, instance->currentContribution);
    targets = CaptureTargets(&required);
    volume = FindTargetRow(targets, VN_TARGET_SCOPE_VOICE, {}, harken::kTargetVoiceVolume, handle);
    ASSERT_NE(nullptr, volume);
    EXPECT_FLOAT_EQ(-9.0f, volume->variableValue); // still folded
    EXPECT_FLOAT_EQ(-12.0f, volume->finalValue);   // = -3 API + -9 row
    EXPECT_EQ(1u, volume->pendingPublication);     // exit is coming
    // The update runs the exit evaluation: contribution and target land
    // together — the row reports 0 exactly when the slot stops folding
    // it, never earlier.
    ASSERT_EQ(VN_OK, Vn_Update());
    bindings = CaptureBindings(&required);
    instance = FindBindingRow(bindings, harkentest::ObjectId(20), {}, handle);
    ASSERT_NE(nullptr, instance);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_PROPERTY_BINDING_NEUTRALIZING),
              instance->stateFlags);
    EXPECT_FLOAT_EQ(0.0f, instance->currentContribution);
    targets = CaptureTargets(&required);
    volume = FindTargetRow(targets, VN_TARGET_SCOPE_VOICE, {}, harken::kTargetVoiceVolume, handle);
    ASSERT_NE(nullptr, volume);
    EXPECT_FLOAT_EQ(-3.0f, volume->finalValue);
    EXPECT_EQ(0u, volume->activeBindingCount);
}

// --- an armed, never-driven instance is still a lifecycle state ---

TEST(PropertyCapture, NeverDrivenLiveInstanceReportsActiveWithZeroTerms) {
    EngineSession session(MakeNullConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(
                  engine, {harkentest::ObjectId(7)}, 0.0f, 1.0f).status);
    RegisterLeaf(harkentest::ObjectId(1));
    // Variable 7 is NEVER written: the instance occupies its slot and
    // arms the first write, so it must read as a live row — ACTIVE, with
    // currentInput 0 (the documented no-value-at-any-scope report) and
    // contribution 0 (nothing realized) — never as a flagless ghost.
    ASSERT_EQ(VN_OK, RegisterLinear(engine, harkentest::ObjectId(20), harkentest::ObjectId(7),
                                    harken::kTargetScopeVoice,
                                    harken::kTargetVoiceVolume, 0.0f,
                                    -9.0f));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, harken::ApplyVoiceBinding(*engine, handle, harkentest::ObjectId(20)));
    ASSERT_EQ(VN_OK, Vn_Update());
    std::uint32_t required = 0;
    const std::vector<VnPropertyBindingInfo> bindings =
        CaptureBindings(&required);
    const VnPropertyBindingInfo* instance =
        FindBindingRow(bindings, harkentest::ObjectId(20), {}, handle);
    ASSERT_NE(nullptr, instance);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_PROPERTY_BINDING_ACTIVE),
              instance->stateFlags);
    EXPECT_FLOAT_EQ(0.0f, instance->currentInput);
    EXPECT_FLOAT_EQ(0.0f, instance->currentContribution);
    // The first write arms it for real: the realized contribution and
    // the ACTIVE flag now agree with the target row.
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7), 1.0f));
    harkentest::AdvanceVariableWrite(engine);
    const std::vector<VnPropertyBindingInfo> after =
        CaptureBindings(&required);
    instance = FindBindingRow(after, harkentest::ObjectId(20), {}, handle);
    ASSERT_NE(nullptr, instance);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_PROPERTY_BINDING_ACTIVE),
              instance->stateFlags);
    EXPECT_FLOAT_EQ(1.0f, instance->currentInput);
    EXPECT_FLOAT_EQ(-9.0f, instance->currentContribution);
}

// --- PENDING rows: parked voices hold live instance identities ---

TEST(PropertyCapture, ParkedInstanceIdentityReportsAsPendingRow) {
    VnConfig config = MakeNullConfig(4, kBlock, kRate);
    config.maxParkedVoices = 4;
    config.audibilityParkEnabled = 1;
    config.parkThresholdDb = -40.0f;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(
                  engine, {harkentest::ObjectId(7)}, 0.0f, 1.0f).status);
    RegisterLeaf(harkentest::ObjectId(1));
    // The nontrivial Bezier drives the looping voice below the park
    // threshold from its first block (the binding-liveness park recipe).
    VnBindingDesc desc{};
    desc.structSize = sizeof(VnBindingDesc);
    desc.variableId = harkentest::ObjectId(7);
    desc.targetScopeKind = VN_TARGET_SCOPE_VOICE;
    desc.targetId = harken::kTargetVoiceVolume;
    desc.curve.structSize = sizeof(VnCurveDesc);
    desc.curve.pointCount = 2;
    desc.curve.points[0] = {0.0f, 0.0f};
    desc.curve.points[1] = {1.0f, -80.0f};
    desc.curve.handles[0] = {0.0f, -80.0f, 1.0f, -80.0f};
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(10), &desc));
    ASSERT_EQ(VN_OK, harken::AttachBinding(*engine, harkentest::ObjectId(10),
                                          harken::kTargetScopeVoice, harkentest::ObjectId(1)));
    constexpr float kInput = 0.98f;
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7), kInput));
    harkentest::AdvanceVariableWrite(engine);
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    play.loopCount = 0; // infinite: park-eligible while inaudible
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    const std::uint32_t ledgerSlot = static_cast<std::uint32_t>(handle);
    ASSERT_TRUE(engine->parkRecords[ledgerSlot].active);

    // The held identity is a PENDING row against the parked voice's
    // public handle, with the would-be contribution at the current input.
    std::uint32_t required = 0;
    const std::vector<VnPropertyBindingInfo> bindings =
        CaptureBindings(&required);
    const VnPropertyBindingInfo* held = nullptr;
    for (const VnPropertyBindingInfo& row : bindings) {
        if (row.bindingId == harkentest::ObjectId(10) && row.instanceHandle == handle &&
            (row.stateFlags & VN_PROPERTY_BINDING_PENDING) != 0) {
            held = &row;
        }
    }
    ASSERT_NE(nullptr, held);
    EXPECT_FLOAT_EQ(kInput, held->currentInput);
    const double expected = 20.0 * std::log10(BindingOracle(kInput,
        {0.0f, 1.0f}, {0.0f, 0.0001f, 1.0f, 0.0001f}, {1.0f, 0.0001f}));
    EXPECT_NEAR(static_cast<float>(expected), held->currentContribution,
                3.0e-5f);
    // The parked voice holds no render slot, so no VOICE target row
    // reports its handle.
    const std::vector<VnPropertyTargetInfo> targets =
        CaptureTargets(&required);
    EXPECT_EQ(nullptr, FindTargetRow(targets, VN_TARGET_SCOPE_VOICE, {}, harken::kTargetVoiceVolume, handle));
}

// --- truncation and refusal contracts ---

TEST(PropertyCapture, TruncationReportsCountsAndLogsHonestly) {
    EngineSession session(MakeNullConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(
                  engine, {harkentest::ObjectId(7)}, 0.0f, 1.0f).status);
    constexpr VnBusId kBusA = harkentest::ObjectId(100);
    constexpr VnBusId kBusB = harkentest::ObjectId(101);
    constexpr VnBusId kBusC = harkentest::ObjectId(102);
    for (VnBusId bus : {kBusA, kBusB, kBusC}) {
        ASSERT_EQ(VN_OK, Vn_CreateBus(bus, VN_BUS_MASTER));
        ASSERT_EQ(VN_OK, Vn_SetBusVolume(bus, -6.0f, 0));
    }
    // Three active bus rows; a capacity-1 fetch truncates: VN_OK, the
    // first row written, requiredCount reporting the full need, counted
    // once, logged with the named code.
    VnPropertyTargetInfo rows[2] = {};
    rows[0].structSize = sizeof(VnPropertyTargetInfo);
    rows[1].structSize = 0xEEEEEEEEu; // sentinel: must stay untouched
    std::uint32_t required = 0;
    EXPECT_EQ(VN_OK, Vn_CapturePropertyTargets(rows, 1, &required));
    EXPECT_EQ(3u, required);
    EXPECT_EQ(sizeof(VnPropertyTargetInfo), rows[0].structSize);
    EXPECT_EQ(0xEEEEEEEEu, rows[1].structSize);
    EXPECT_EQ(1u, engine->captureTruncationCount);
    harken::LogRecord record{};
    ASSERT_TRUE(FindLogicLog(engine, harken::kLogCodeCaptureTruncated,
                             &record));
    EXPECT_EQ(harken::kCaptureSurfaceTargets, record.arg0);
    EXPECT_EQ(3u, record.arg1);
    // The capacity-0 probe is the sizing idiom, never a truncation.
    EXPECT_EQ(VN_OK, Vn_CapturePropertyTargets(nullptr, 0, &required));
    EXPECT_EQ(3u, required);
    EXPECT_EQ(1u, engine->captureTruncationCount);
    EXPECT_FALSE(FindLogicLog(engine, harken::kLogCodeCaptureTruncated,
                              &record));
    // An exact-fit fetch is not a truncation either.
    std::vector<VnPropertyTargetInfo> fit(3);
    fit[0].structSize = sizeof(VnPropertyTargetInfo);
    EXPECT_EQ(VN_OK, Vn_CapturePropertyTargets(fit.data(), 3, &required));
    EXPECT_EQ(3u, required);
    EXPECT_EQ(1u, engine->captureTruncationCount);
    // The bindings surface truncates independently, named by its own
    // surface tag.
    ASSERT_EQ(VN_OK, RegisterLinear(engine, harkentest::ObjectId(10), harkentest::ObjectId(7), harken::kTargetScopeBus,
                                    harken::kBusTargetBusVolume, 0.0f,
                                    -12.0f));
    for (VnBusId bus : {kBusA, kBusB}) {
        ASSERT_EQ(VN_OK, harken::AttachBinding(*engine, harkentest::ObjectId(10),
                                              harken::kTargetScopeBus, bus));
    }
    VnPropertyBindingInfo bindingRow{};
    bindingRow.structSize = sizeof(VnPropertyBindingInfo);
    EXPECT_EQ(VN_OK, Vn_CapturePropertyBindings(&bindingRow, 1, &required));
    EXPECT_EQ(2u, required);
    EXPECT_EQ(2u, engine->captureTruncationCount);
    ASSERT_TRUE(FindLogicLog(engine, harken::kLogCodeCaptureTruncated,
                             &record));
    EXPECT_EQ(harken::kCaptureSurfaceBindings, record.arg0);
    EXPECT_EQ(2u, record.arg1);
}

TEST(PropertyCapture, RefusalMatrixLeavesNothingAmbiguous) {
    {
        VnPropertyTargetInfo row{};
        row.structSize = sizeof(VnPropertyTargetInfo);
        std::uint32_t required = 0;
        EXPECT_EQ(VN_NOT_INITIALIZED,
                  Vn_CapturePropertyTargets(&row, 1, &required));
    }
    EngineSession session(MakeNullConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(
                  engine, {harkentest::ObjectId(7)}, 0.0f, 1.0f).status);
    std::uint32_t required = 0;
    // requiredCount is mandatory: truncation reports through it.
    VnPropertyTargetInfo row{};
    row.structSize = sizeof(VnPropertyTargetInfo);
    EXPECT_EQ(VN_INVALID_ARG, Vn_CapturePropertyTargets(&row, 1, nullptr));
    // A sized fetch needs a buffer with a known row vintage.
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_CapturePropertyTargets(nullptr, 1, &required));
    row.structSize = sizeof(VnPropertyTargetInfo) - 4;
    EXPECT_EQ(VN_INVALID_ARG, Vn_CapturePropertyTargets(&row, 1, &required));
    VnPropertyBindingInfo bindingRow{};
    bindingRow.structSize = 0;
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_CapturePropertyBindings(&bindingRow, 1, &required));
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_CapturePropertyBindings(&bindingRow, 1, nullptr));
    // Refusals never count as truncations.
    EXPECT_EQ(0u, engine->captureTruncationCount);
    // An idle engine captures zero rows through the probe idiom, with
    // requiredCount always written.
    required = 99;
    EXPECT_EQ(VN_OK, Vn_CapturePropertyTargets(nullptr, 0, &required));
    EXPECT_EQ(0u, required);
    required = 99;
    EXPECT_EQ(VN_OK, Vn_CapturePropertyBindings(nullptr, 0, &required));
    EXPECT_EQ(0u, required);
}

} // namespace
