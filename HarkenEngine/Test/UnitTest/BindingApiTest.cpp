// Public C surface of the binding system: the eight verbs at the Harken.h
// tail, driven through the C ABI rather than the engine seam. Covers the
// descriptor vintage gate (structSize of VnBindingDesc, its embedded
// VnCurveDesc, and VnBindingAttachDesc), the refusal matrix and success
// path of every verb, the VnConfig pool routing (custom sizes take
// effect; out-of-range sizes refuse Vn_Init), and the compile-time ABI
// pins for the appended enums, result code, descriptor layouts, and the
// VnConfig tail fields.
#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "Common/TestUtil.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

using harkentest::EngineSession;
using harkentest::MakeDC;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;

constexpr std::uint32_t kBlock = 256;
constexpr std::uint32_t kRate = 48000;

// Voice-scope target ids of the built-in table (volume 0 ... low-shelf 6);
// pan remains snap-only; bus targets are BusVolume then BusOutputVolume.
constexpr std::uint32_t kTargetVoiceVolume = 0;
constexpr std::uint32_t kTargetVoicePitch = 1;
constexpr std::uint32_t kTargetVoicePan = 4;
constexpr std::uint32_t kBusTargetVolume = 0;

// --- compile-time ABI pins: result/scope values and descriptor layouts ---

static_assert(VN_NO_BINDING == -320, "new result code must keep its value");
static_assert(VN_TARGET_SCOPE_VOICE == 0, "scope value must not renumber");
static_assert(VN_TARGET_SCOPE_BUS == 1, "scope value must not renumber");
static_assert(VN_CURVE_MAX_POINTS == 8, "curve capacity is ABI-fixed");

static_assert(sizeof(VnCurvePoint) == 8, "curve point is two packed floats");
static_assert(offsetof(VnCurvePoint, x) == 0 && offsetof(VnCurvePoint, y) == 4,
              "curve point packing pinned");
static_assert(offsetof(VnCurveDesc, structSize) == 0 &&
                  offsetof(VnCurveDesc, pointCount) == 4 &&
                  offsetof(VnCurveDesc, points) == 8 &&
                  offsetof(VnCurveDesc, handles) == 72 &&
                  sizeof(VnCurveDesc) == 184,
              "VnCurveDesc layout pinned");
static_assert(offsetof(VnBindingDesc, structSize) == 0 &&
                  offsetof(VnBindingDesc, variableId) == 8 &&
                  offsetof(VnBindingDesc, targetScopeKind) == 24 &&
                  offsetof(VnBindingDesc, targetId) == 28 &&
                  offsetof(VnBindingDesc, curve) == 32 &&
                  offsetof(VnBindingDesc, transitionMs) == 216 &&
                  offsetof(VnBindingDesc, reserved0) == 220 &&
                  sizeof(VnBindingDesc) == 224,
              "VnBindingDesc layout pinned");
static_assert(offsetof(VnBindingAttachDesc, structSize) == 0 &&
                  offsetof(VnBindingAttachDesc, scopeKind) == 4 &&
                  offsetof(VnBindingAttachDesc, objectId) == 8 &&
                  sizeof(VnBindingAttachDesc) == 24,
              "VnBindingAttachDesc layout pinned");

// VnConfig: binding-pool fields append after the multichannel tail
// (reserved6), followed by the Gain, Limiter, Compressor, and Dynamic
// Modulation capacity vintages, contiguous and in order, with no tail padding
// beyond reserved9. A mid-struct insertion or a reorder fails here.
static_assert(offsetof(VnConfig, maxBindings) ==
                  offsetof(VnConfig, reserved6) + sizeof(std::uint32_t),
              "the binding-pool fields must append right after reserved6");
static_assert(offsetof(VnConfig, maxBindingAttachments) ==
                  offsetof(VnConfig, maxBindings) + sizeof(std::uint32_t),
              "maxBindingAttachments follows maxBindings");
static_assert(offsetof(VnConfig, voiceBindingInstanceSlots) ==
                  offsetof(VnConfig, maxBindings) + 2 * sizeof(std::uint32_t),
              "voiceBindingInstanceSlots follows maxBindingAttachments");
static_assert(offsetof(VnConfig, meterRingBlocks) ==
                  offsetof(VnConfig, maxBindings) + 3 * sizeof(std::uint32_t),
              "meterRingBlocks is the last binding-pool field");
static_assert(offsetof(VnConfig, gainEffectPoolCapacity) ==
                  offsetof(VnConfig, meterRingBlocks) + sizeof(std::uint32_t),
              "the gain-pool field appends right after the binding-pool tail");
static_assert(offsetof(VnConfig, reserved7) ==
                  offsetof(VnConfig, gainEffectPoolCapacity) +
                      sizeof(std::uint32_t),
              "reserved7 follows gainEffectPoolCapacity");
static_assert(offsetof(VnConfig, limiterEffectPoolCapacity) ==
                  offsetof(VnConfig, reserved7) + sizeof(std::uint32_t),
              "limiter pool capacity appends after reserved7");
static_assert(offsetof(VnConfig, reserved8) ==
                  offsetof(VnConfig, limiterEffectPoolCapacity) +
                      sizeof(std::uint32_t),
              "reserved8 follows limiterEffectPoolCapacity");
static_assert(offsetof(VnConfig, compressorEffectPoolCapacity) ==
                  offsetof(VnConfig, reserved8) + sizeof(std::uint32_t),
              "compressor capacity appends after the Phase 1 tail");
static_assert(offsetof(VnConfig, dynamicRouteCapacity) ==
                  offsetof(VnConfig, compressorEffectPoolCapacity) +
                      sizeof(std::uint32_t),
              "dynamic route capacity follows compressor capacity");
static_assert(offsetof(VnConfig, dynamicRoutePlanPoolCapacity) ==
                  offsetof(VnConfig, dynamicRouteCapacity) +
                      sizeof(std::uint32_t),
              "dynamic route-plan capacity follows route capacity");
static_assert(offsetof(VnConfig, dynamicBusTapCapacity) ==
                  offsetof(VnConfig, dynamicRoutePlanPoolCapacity) +
                      sizeof(std::uint32_t),
              "dynamic bus-tap capacity follows route-plan capacity");
static_assert(offsetof(VnConfig, dynamicVoiceTapCapacity) ==
                  offsetof(VnConfig, dynamicBusTapCapacity) +
                      sizeof(std::uint32_t),
              "dynamic voice-tap capacity follows bus-tap capacity");
static_assert(offsetof(VnConfig, reserved9) ==
                  offsetof(VnConfig, dynamicVoiceTapCapacity) +
                      sizeof(std::uint32_t),
              "reserved9 follows the dynamic capacity fields");
static_assert(offsetof(VnConfig, maxActionFirings) ==
                  offsetof(VnConfig, maxTopologyPlanBytes) +
                      sizeof(std::uint64_t),
              "maxActionFirings appends after maxTopologyPlanBytes");
static_assert(offsetof(VnConfig, reserved10) ==
                  offsetof(VnConfig, maxActionFirings) +
                      sizeof(std::uint32_t),
              "reserved10 follows maxActionFirings");
static_assert(sizeof(VnConfig) ==
                  offsetof(VnConfig, reserved12) + sizeof(std::uint32_t),
              "no VnConfig tail padding beyond Action cooldown fields");

// --- helpers ---

VnBindingDesc MakeBindingDesc(VnVariableId variableId, std::uint32_t scopeKind,
                              std::uint32_t targetId, float y0, float y1,
                              std::uint32_t transitionMs = 0) {
    VnBindingDesc desc{};
    desc.structSize = sizeof(VnBindingDesc);
    desc.variableId = variableId;
    desc.targetScopeKind = scopeKind;
    desc.targetId = targetId;
    desc.transitionMs = transitionMs;
    desc.curve.structSize = sizeof(VnCurveDesc);
    desc.curve.pointCount = 2;
    desc.curve.points[0] = {0.0f, y0};
    desc.curve.points[1] = {1.0f, y1};
    desc.curve.handles[0] = harkentest::EndpointHandles(
        desc.curve.points[0], desc.curve.points[1]);
    return desc;
}

VnBindingAttachDesc MakeAttachDesc(std::uint32_t scopeKind,
                                   VnObjectId objectId) {
    VnBindingAttachDesc desc{};
    desc.structSize = sizeof(VnBindingAttachDesc);
    desc.scopeKind = scopeKind;
    desc.objectId = objectId;
    return desc;
}

void RegisterLeaf(VnSoundId id) {
    static const std::vector<float> dc = MakeDC(kRate, 0.5f);
    const VnPCMDesc desc = harkentest::MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(id, &desc));
}

// A handle value no session ever minted: structurally stale.
constexpr VnVoiceHandle kBogusHandle = 0xDEADBEEFull;

// --- every verb refuses without an engine ---

TEST(BindingApi, EveryVerbRefusesBeforeInit) {
    const VnBindingDesc desc =
        MakeBindingDesc(harkentest::ObjectId(7), VN_TARGET_SCOPE_VOICE, kTargetVoiceVolume, 0.0f,
                        -6.0f);
    const VnBindingAttachDesc attach =
        MakeAttachDesc(VN_TARGET_SCOPE_BUS, VN_BUS_MASTER);
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_RegisterBinding(harkentest::ObjectId(1), &desc));
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_UnregisterBinding(harkentest::ObjectId(1)));
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_ReplaceBinding(harkentest::ObjectId(1), &desc));
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_AttachBinding(harkentest::ObjectId(1), &attach));
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_DetachBinding(harkentest::ObjectId(1), &attach));
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_ApplyVoiceBinding(kBogusHandle, harkentest::ObjectId(1)));
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_ClearVoiceBinding(kBogusHandle, harkentest::ObjectId(1)));
}

// --- Vn_RegisterBinding: vintage gate, refusal matrix, success ---

TEST(BindingApi, RegisterVintageGateAndRefusalMatrix) {
    EngineSession session(MakeNullConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());

    // Descriptor shape gate: null, wrong outer vintage, wrong curve
    // vintage — the engine knows exactly one vintage of each struct.
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterBinding(harkentest::ObjectId(1), nullptr));
    {
        VnBindingDesc desc = MakeBindingDesc(harkentest::ObjectId(7), VN_TARGET_SCOPE_VOICE,
                                             kTargetVoiceVolume, 0.0f, -6.0f);
        desc.structSize = sizeof(VnBindingDesc) - 4;
        EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterBinding(harkentest::ObjectId(1), &desc));
    }
    {
        VnBindingDesc desc = MakeBindingDesc(harkentest::ObjectId(7), VN_TARGET_SCOPE_VOICE,
                                             kTargetVoiceVolume, 0.0f, -6.0f);
        desc.curve.structSize = sizeof(VnCurveDesc) + 4;
        EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterBinding(harkentest::ObjectId(1), &desc));
    }
    // Point-count bounds: zero and beyond the fixed capacity.
    {
        VnBindingDesc desc = MakeBindingDesc(harkentest::ObjectId(7), VN_TARGET_SCOPE_VOICE,
                                             kTargetVoiceVolume, 0.0f, -6.0f);
        desc.curve.pointCount = 0;
        EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterBinding(harkentest::ObjectId(1), &desc));
        desc.curve.pointCount = VN_CURVE_MAX_POINTS + 1;
        EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterBinding(harkentest::ObjectId(1), &desc));
    }
    // Curve rules ride the seam: non-increasing x refuses.
    {
        VnBindingDesc desc = MakeBindingDesc(harkentest::ObjectId(7), VN_TARGET_SCOPE_VOICE,
                                             kTargetVoiceVolume, 0.0f, -6.0f);
        desc.curve.points[1].x = 0.0f;
        EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterBinding(harkentest::ObjectId(1), &desc));
    }
    // Active handles must stay inside their segment and remain finite.
    {
        VnBindingDesc desc = MakeBindingDesc(harkentest::ObjectId(7), VN_TARGET_SCOPE_VOICE,
                                             kTargetVoiceVolume, 0.0f, -6.0f);
        desc.curve.handles[0].h1x = -0.01f;
        EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterBinding(harkentest::ObjectId(1), &desc));
        desc.curve.handles[0] = harkentest::EndpointHandles(
            desc.curve.points[0], desc.curve.points[1]);
        desc.curve.handles[0].h2y = std::numeric_limits<float>::infinity();
        EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterBinding(harkentest::ObjectId(1), &desc));
    }
    // Every fixed-capacity tail slot must be bitwise +0.0f.
    {
        VnBindingDesc desc = MakeBindingDesc(harkentest::ObjectId(7), VN_TARGET_SCOPE_VOICE,
                                             kTargetVoiceVolume, 0.0f, -6.0f);
        desc.curve.handles[1].h1x = 1.0f;
        EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterBinding(harkentest::ObjectId(1), &desc));
        desc.curve.handles[1] = {};
        desc.curve.handles[1].h1x = -0.0f;
        EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterBinding(harkentest::ObjectId(1), &desc));
    }
    // Unknown scoped target: bad target id, and a scope kind that does
    // not exist yet — refused, never guessed.
    {
        const VnBindingDesc desc =
            MakeBindingDesc(harkentest::ObjectId(7), VN_TARGET_SCOPE_VOICE, 99, 0.0f, -6.0f);
        EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterBinding(harkentest::ObjectId(1), &desc));
    }
    {
        const VnBindingDesc desc =
            MakeBindingDesc(harkentest::ObjectId(7), /*scopeKind=*/7, kTargetVoiceVolume, 0.0f,
                            -6.0f);
        EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterBinding(harkentest::ObjectId(1), &desc));
    }
    // Nonzero transition on a snap-only target refuses loudly.
    {
        const VnBindingDesc desc =
            MakeBindingDesc(harkentest::ObjectId(7), VN_TARGET_SCOPE_VOICE, kTargetVoicePan, 0.0f,
                            1.0f, /*transitionMs=*/100);
        EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterBinding(harkentest::ObjectId(1), &desc));
    }

    // Success: a transition-capable target accepts one; a duplicate id
    // refuses.
    {
        const VnBindingDesc desc =
            MakeBindingDesc(harkentest::ObjectId(7), VN_TARGET_SCOPE_VOICE, kTargetVoiceVolume, 0.0f,
                            -6.0f, /*transitionMs=*/100);
        EXPECT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &desc));
        EXPECT_EQ(VN_IN_USE, Vn_RegisterBinding(harkentest::ObjectId(1), &desc));
    }
    {
        const VnBindingDesc desc = MakeBindingDesc(
            harkentest::ObjectId(8), VN_TARGET_SCOPE_BUS, kBusTargetVolume, 0.0f, -12.0f);
        EXPECT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(2), &desc));
    }
}

TEST(BindingTable, PitchNonzeroTransitionAccepted) {
    EngineSession session(MakeNullConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    const VnBindingDesc pitch =
        MakeBindingDesc(harkentest::ObjectId(7), VN_TARGET_SCOPE_VOICE, kTargetVoicePitch, 0.0f,
                        1200.0f, /*transitionMs=*/20);
    EXPECT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &pitch));
    const VnBindingDesc pan =
        MakeBindingDesc(harkentest::ObjectId(8), VN_TARGET_SCOPE_VOICE, kTargetVoicePan, 0.0f, 1.0f,
                        /*transitionMs=*/20);
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterBinding(harkentest::ObjectId(2), &pan));
}

TEST(BindingApi, DefinitionPoolSizeRoutesFromConfig) {
    VnConfig config = MakeNullConfig(4, kBlock, kRate);
    config.maxBindings = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    const VnBindingDesc desc = MakeBindingDesc(harkentest::ObjectId(7), VN_TARGET_SCOPE_VOICE,
                                               kTargetVoiceVolume, 0.0f,
                                               -6.0f);
    EXPECT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &desc));
    EXPECT_EQ(VN_NO_BINDING, Vn_RegisterBinding(harkentest::ObjectId(2), &desc));
    // Freeing the single slot re-admits.
    EXPECT_EQ(VN_OK, Vn_UnregisterBinding(harkentest::ObjectId(1)));
    EXPECT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(2), &desc));
}

// --- attach / detach / unregister ---

TEST(BindingApi, AttachDetachMatrixAndUnregisterReferenceRule) {
    EngineSession session(MakeNullConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    RegisterLeaf(harkentest::ObjectId(21));

    const VnBindingDesc busDef = MakeBindingDesc(harkentest::ObjectId(7), VN_TARGET_SCOPE_BUS,
                                                 kBusTargetVolume, 0.0f,
                                                 -12.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &busDef));
    const VnBindingDesc voiceDef = MakeBindingDesc(harkentest::ObjectId(8), VN_TARGET_SCOPE_VOICE,
                                                   kTargetVoiceVolume, 0.0f,
                                                   -6.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(2), &voiceDef));

    const VnBindingAttachDesc busAttach =
        MakeAttachDesc(VN_TARGET_SCOPE_BUS, VN_BUS_MASTER);
    const VnBindingAttachDesc voiceAttach =
        MakeAttachDesc(VN_TARGET_SCOPE_VOICE, harkentest::ObjectId(21));

    // Descriptor gate.
    EXPECT_EQ(VN_INVALID_ARG, Vn_AttachBinding(harkentest::ObjectId(1), nullptr));
    {
        VnBindingAttachDesc bad = busAttach;
        bad.structSize = sizeof(VnBindingAttachDesc) + 8;
        EXPECT_EQ(VN_INVALID_ARG, Vn_AttachBinding(harkentest::ObjectId(1), &bad));
    }
    // Unknown definition; scope mismatch against the definition; unknown
    // objects on both scopes.
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_AttachBinding(harkentest::ObjectId(99), &busAttach));
    EXPECT_EQ(VN_INVALID_ARG, Vn_AttachBinding(harkentest::ObjectId(1), &voiceAttach));
    {
        const VnBindingAttachDesc unknownBus =
            MakeAttachDesc(VN_TARGET_SCOPE_BUS, harkentest::ObjectId(777));
        EXPECT_EQ(VN_UNKNOWN_ID, Vn_AttachBinding(harkentest::ObjectId(1), &unknownBus));
    }
    {
        const VnBindingAttachDesc unknownPlayable =
            MakeAttachDesc(VN_TARGET_SCOPE_VOICE, harkentest::ObjectId(777));
        EXPECT_EQ(VN_UNKNOWN_ID, Vn_AttachBinding(harkentest::ObjectId(2), &unknownPlayable));
    }

    // Success both scopes; duplicates refuse.
    EXPECT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(1), &busAttach));
    EXPECT_EQ(VN_IN_USE, Vn_AttachBinding(harkentest::ObjectId(1), &busAttach));
    EXPECT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(2), &voiceAttach));

    // Unregister refuses while attached (the honest static rule).
    EXPECT_EQ(VN_IN_USE, Vn_UnregisterBinding(harkentest::ObjectId(1)));
    EXPECT_EQ(VN_IN_USE, Vn_UnregisterBinding(harkentest::ObjectId(2)));

    // Detach: descriptor gate, unknown attachment, then success frees the
    // reference and unregister admits.
    EXPECT_EQ(VN_INVALID_ARG, Vn_DetachBinding(harkentest::ObjectId(1), nullptr));
    {
        const VnBindingAttachDesc never =
            MakeAttachDesc(VN_TARGET_SCOPE_BUS, harkentest::ObjectId(777));
        EXPECT_EQ(VN_UNKNOWN_ID, Vn_DetachBinding(harkentest::ObjectId(1), &never));
    }
    EXPECT_EQ(VN_OK, Vn_DetachBinding(harkentest::ObjectId(1), &busAttach));
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_DetachBinding(harkentest::ObjectId(1), &busAttach));
    EXPECT_EQ(VN_OK, Vn_UnregisterBinding(harkentest::ObjectId(1)));
    EXPECT_EQ(VN_OK, Vn_DetachBinding(harkentest::ObjectId(2), &voiceAttach));
    EXPECT_EQ(VN_OK, Vn_UnregisterBinding(harkentest::ObjectId(2)));
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_UnregisterBinding(harkentest::ObjectId(2)));
}

TEST(BindingApi, AttachmentPoolSizeRoutesFromConfig) {
    VnConfig config = MakeNullConfig(4, kBlock, kRate);
    config.maxBindingAttachments = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(5), VN_BUS_MASTER));

    const VnBindingDesc busDef = MakeBindingDesc(harkentest::ObjectId(7), VN_TARGET_SCOPE_BUS,
                                                 kBusTargetVolume, 0.0f,
                                                 -12.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &busDef));
    const VnBindingAttachDesc first =
        MakeAttachDesc(VN_TARGET_SCOPE_BUS, VN_BUS_MASTER);
    const VnBindingAttachDesc second = MakeAttachDesc(VN_TARGET_SCOPE_BUS, harkentest::ObjectId(5));
    EXPECT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(1), &first));
    EXPECT_EQ(VN_NO_BINDING, Vn_AttachBinding(harkentest::ObjectId(1), &second));
    EXPECT_EQ(VN_OK, Vn_DetachBinding(harkentest::ObjectId(1), &first));
    EXPECT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(1), &second));
}

// --- Vn_ReplaceBinding: frozen structural identity, live success ---

TEST(BindingApi, ReplaceFreezesStructuralIdentity) {
    EngineSession session(MakeNullConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());

    const VnBindingDesc original = MakeBindingDesc(harkentest::ObjectId(7), VN_TARGET_SCOPE_VOICE,
                                                   kTargetVoiceVolume, 0.0f,
                                                   -6.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &original));

    EXPECT_EQ(VN_UNKNOWN_ID, Vn_ReplaceBinding(harkentest::ObjectId(99), &original));
    EXPECT_EQ(VN_INVALID_ARG, Vn_ReplaceBinding(harkentest::ObjectId(1), nullptr));
    {
        VnBindingDesc moved = original;
        moved.variableId = harkentest::ObjectId(8);
        EXPECT_EQ(VN_INVALID_ARG, Vn_ReplaceBinding(harkentest::ObjectId(1), &moved));
        moved = original;
        moved.targetId = kTargetVoicePitch;
        EXPECT_EQ(VN_INVALID_ARG, Vn_ReplaceBinding(harkentest::ObjectId(1), &moved));
    }
    // Only curve and transitionMs may change; the replacement re-runs the
    // full validation (a bad curve refuses with the old content intact).
    {
        VnBindingDesc recurve = original;
        recurve.curve.points[1].y = -24.0f;
        recurve.curve.handles[0] = harkentest::EndpointHandles(
            recurve.curve.points[0], recurve.curve.points[1]);
        recurve.transitionMs = 250;
        EXPECT_EQ(VN_OK, Vn_ReplaceBinding(harkentest::ObjectId(1), &recurve));
        recurve.curve.points[1].x = 0.0f;
        EXPECT_EQ(VN_INVALID_ARG, Vn_ReplaceBinding(harkentest::ObjectId(1), &recurve));
    }
}

// --- Vn_ApplyVoiceBinding / Vn_ClearVoiceBinding ---

TEST(BindingApi, ApplyAndClearVoiceBindingMatrix) {
    EngineSession session(MakeNullConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    RegisterLeaf(harkentest::ObjectId(21));

    const VnBindingDesc voiceDef = MakeBindingDesc(harkentest::ObjectId(7), VN_TARGET_SCOPE_VOICE,
                                                   kTargetVoiceVolume, 0.0f,
                                                   -6.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &voiceDef));
    const VnBindingDesc busDef = MakeBindingDesc(harkentest::ObjectId(8), VN_TARGET_SCOPE_BUS,
                                                 kBusTargetVolume, 0.0f,
                                                 -12.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(2), &busDef));

    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(21));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    EXPECT_EQ(VN_UNKNOWN_ID, Vn_ApplyVoiceBinding(handle, harkentest::ObjectId(99)));
    EXPECT_EQ(VN_INVALID_ARG, Vn_ApplyVoiceBinding(handle, harkentest::ObjectId(2)));
    EXPECT_EQ(VN_STALE_HANDLE, Vn_ApplyVoiceBinding(kBogusHandle, harkentest::ObjectId(1)));

    EXPECT_EQ(VN_OK, Vn_ApplyVoiceBinding(handle, harkentest::ObjectId(1)));
    EXPECT_EQ(VN_IN_USE, Vn_ApplyVoiceBinding(handle, harkentest::ObjectId(1)));

    EXPECT_EQ(VN_STALE_HANDLE, Vn_ClearVoiceBinding(kBogusHandle, harkentest::ObjectId(1)));
    EXPECT_EQ(VN_OK, Vn_ClearVoiceBinding(handle, harkentest::ObjectId(1)));
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_ClearVoiceBinding(handle, harkentest::ObjectId(1)));
}

TEST(BindingApi, VoiceInstanceSlotCountRoutesFromConfig) {
    VnConfig config = MakeNullConfig(4, kBlock, kRate);
    config.voiceBindingInstanceSlots = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    RegisterLeaf(harkentest::ObjectId(21));

    const VnBindingDesc first = MakeBindingDesc(harkentest::ObjectId(7), VN_TARGET_SCOPE_VOICE,
                                                kTargetVoiceVolume, 0.0f,
                                                -6.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &first));
    const VnBindingDesc second = MakeBindingDesc(harkentest::ObjectId(8), VN_TARGET_SCOPE_VOICE,
                                                 kTargetVoiceVolume, 0.0f,
                                                 -3.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(2), &second));

    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(21));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    EXPECT_EQ(VN_OK, Vn_ApplyVoiceBinding(handle, harkentest::ObjectId(1)));
    EXPECT_EQ(VN_NO_BINDING, Vn_ApplyVoiceBinding(handle, harkentest::ObjectId(2)));
}

// --- Vn_Init validation of the appended config fields ---

TEST(BindingApi, InitRefusesOutOfRangePoolFields) {
    {
        VnConfig config = MakeNullConfig(4, kBlock, kRate);
        config.voiceBindingInstanceSlots = 9; // above the slot-array capacity
        EXPECT_EQ(VN_INVALID_CONFIG, Vn_Init(&config));
    }
    {
        VnConfig config = MakeNullConfig(4, kBlock, kRate);
        config.maxBindings = (1u << 16) + 1u;
        EXPECT_EQ(VN_INVALID_CONFIG, Vn_Init(&config));
    }
    {
        VnConfig config = MakeNullConfig(4, kBlock, kRate);
        config.maxBindingAttachments = (1u << 16) + 1u;
        EXPECT_EQ(VN_INVALID_CONFIG, Vn_Init(&config));
    }
}

TEST(BindingApi, CombinedBindingAndApiDirectVolumeOverflowIsRefused) {
    EngineSession session(MakeNullConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(engine, {harkentest::ObjectId(7), harkentest::ObjectId(8)}, 0.0f, 1.0f)
                  .status);
    RegisterLeaf(harkentest::ObjectId(21));

    const VnBindingDesc voiceDef = MakeBindingDesc(
        harkentest::ObjectId(7), VN_TARGET_SCOPE_VOICE, kTargetVoiceVolume, 400.0f, 400.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &voiceDef));
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(21));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_ApplyVoiceBinding(handle, harkentest::ObjectId(1)));
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7), 1.0f));
    harkentest::AdvanceVariableWrite(engine);
    const VnVoiceHandle render = harkentest::RenderHandle(engine, handle);
    const float resolvedBefore =
        engine->params.ResolvedValue(render, harken::kTargetVoiceVolume);
    ASSERT_NEAR(400.0f, resolvedBefore, 1e-3f);
    const std::uint64_t commandHead = engine->commands.PublishedHead();
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceVolume(handle, 400.0f, 0));
    EXPECT_EQ(commandHead, engine->commands.PublishedHead());
    EXPECT_FLOAT_EQ(resolvedBefore, engine->params.ResolvedValue(
                                        render, harken::kTargetVoiceVolume));

    const VnBindingDesc busDef = MakeBindingDesc(
        harkentest::ObjectId(8), VN_TARGET_SCOPE_BUS, kBusTargetVolume, 400.0f, 400.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(2), &busDef));
    const VnBindingAttachDesc busAttach =
        MakeAttachDesc(VN_TARGET_SCOPE_BUS, VN_BUS_MASTER);
    ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(2), &busAttach));
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(8), 1.0f));
    harkentest::AdvanceVariableWrite(engine);
    const std::uint32_t master = engine->buses.FindIndex(VN_BUS_MASTER);
    ASSERT_NE(harken::kInvalidBusIndex, master);
    const float busBefore =
        engine->busTargets.Reduce(master, harken::kBusTargetBusVolume);
    ASSERT_NEAR(400.0f, busBefore, 1e-3f);
    const std::uint64_t busHead = engine->commands.PublishedHead();
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetBusVolume(VN_BUS_MASTER, 400.0f, 0));
    EXPECT_EQ(busHead, engine->commands.PublishedHead());
    EXPECT_FLOAT_EQ(busBefore, engine->busTargets.Reduce(
                                   master, harken::kBusTargetBusVolume));
    EXPECT_FALSE(engine->busTargets.SlotActive(
        master, harken::kBusTargetBusVolume,
        harken::BusTargetStore::kSlotApiDirectIndex));
}

TEST(BindingApi, OlderConfigVintageReadsDefaultsForBindingFields) {
    // A caller built before the binding-pool vintage: its struct ends at
    // the old tail, so the appended fields must read as defaults even when
    // the memory past the declared size carries garbage.
    VnConfig config = MakeNullConfig(4, kBlock, kRate);
    config.structSize = static_cast<std::uint32_t>(
        offsetof(VnConfig, maxBindings));
    config.maxBindings = 9999999;              // past the declared vintage:
    config.maxBindingAttachments = 9999999;    // never read
    config.voiceBindingInstanceSlots = 9999999;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    // The default pools (128 definitions) admit ordinary registrations.
    const VnBindingDesc desc = MakeBindingDesc(harkentest::ObjectId(7), VN_TARGET_SCOPE_VOICE,
                                               kTargetVoiceVolume, 0.0f,
                                               -6.0f);
    EXPECT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &desc));
    EXPECT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(2), &desc));
}

} // namespace
