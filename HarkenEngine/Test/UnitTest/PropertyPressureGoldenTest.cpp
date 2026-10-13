// Pressure goldens for transactional property publication, driven
// through a real engine
// session on the null backend: a 20-target mix switch under forced
// command-queue pressure must apply all-or-nothing at the rendered level
// (never a half-applied mix across blocks), the deferral counter must
// fire, and the deferred batch must land whole on the very next tick; a
// sustained single-command-traffic soak proves the admission reservation
// bounds the deferral at one tick (no starvation).
//
// Evidence is rendered output: every voice plays the same DC source, so
// the mix amplitude is the sum of the per-voice gains and any strict
// subset of applied commits lands measurably between the old and new
// sums. The per-voice deltas are distinct, so no partial subset can alias
// either endpoint. Deterministic: explicit pumps, no sleeps, no
// wall-clock.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::MonoDesc;

constexpr std::uint32_t kBlockFrames = 256;
constexpr VnVariableId kMixVariable = harkentest::ObjectId(7);
constexpr float kDcAmplitude = 0.02f;

void InstallPressureVariables(harken::Engine* engine) {
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(
                  engine, {harkentest::ObjectId(7), harkentest::ObjectId(8), harkentest::ObjectId(19)}, 0.0f, 1.0f, 0.0f).status);
}

float DbToGain(float db) {
    return std::pow(10.0f, db / 20.0f);
}

// Steady-state amplitude of one captured block: the mean magnitude of its
// last quarter, past any block-boundary settling. The DC source makes the
// settled value exactly the summed per-voice gain image.
float BlockTail(const Capture& capture, std::uint32_t blockIndex) {
    const std::size_t begin =
        static_cast<std::size_t>(blockIndex) * kBlockFrames +
        (3 * kBlockFrames) / 4;
    const std::size_t end =
        static_cast<std::size_t>(blockIndex + 1) * kBlockFrames;
    double sum = 0.0;
    for (std::size_t i = begin; i < end; ++i) {
        sum += std::fabs(static_cast<double>(capture.left[i]));
    }
    return static_cast<float>(sum / static_cast<double>(end - begin));
}

// True when every frame of the block sits within `tolerance` of `value` —
// the whole block rendered one mix state, with no mid-block move.
bool BlockIsFlatAt(const Capture& capture, std::uint32_t blockIndex,
                   float value, float tolerance) {
    const std::size_t begin =
        static_cast<std::size_t>(blockIndex) * kBlockFrames;
    const std::size_t end =
        static_cast<std::size_t>(blockIndex + 1) * kBlockFrames;
    for (std::size_t i = begin; i < end; ++i) {
        if (std::fabs(std::fabs(capture.left[i]) - value) > tolerance) {
            return false;
        }
    }
    return true;
}

// Issues harmless master-volume singles until admission refuses, and
// returns how many were admitted. Same-value sets, so the drained
// commands change nothing audible; what matters is that each occupies one
// command slot through the single-producer admission gate.
std::uint32_t SpamSinglesUntilRefused() {
    for (std::uint32_t n = 0; n < 64; ++n) {
        if (Vn_SetBusVolume(VN_BUS_MASTER, 0.0f, 0) != VN_OK) {
            return n;
        }
    }
    return 64;
}

TEST(PropertyPressureGolden, MixSwitchUnderPressureAppliesAllOrNothing) {
    constexpr std::uint32_t kVoices = 20;
    constexpr std::uint32_t kQueueCapacity = 32; // power of two: exact

    VnConfig config = MakeNullConfig(24, kBlockFrames, 48000);
    config.commandQueueDepth = kQueueCapacity;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallPressureVariables(engine);

    const std::vector<float> dc = harkentest::MakeDC(48000, kDcAmplitude);
    const VnPCMDesc desc = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    // The Variable exists (at the neutral input) before the bindings
    // evaluate, so their first evaluation resolves to the base and
    // publishes nothing.
    ASSERT_EQ(VN_OK, Vn_SetValue(kMixVariable, 0.0f));

    // 20 voices, one volume binding each, with DISTINCT full-scale deltas
    // (-6 dB .. -15.5 dB) so no partial subset of commits can alias the
    // old or the new mix sum.
    float gainSumOld = 0.0f;
    float gainSumNew = 0.0f;
    for (std::uint32_t i = 0; i < kVoices; ++i) {
        VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
        VnVoiceHandle handle = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        const float deltaDb = -(6.0f + 0.5f * static_cast<float>(i));
        harken::Curve curve;
        const harken::CurvePoint pts[] = {{0.0f, 0.0f}, {1.0f, deltaDb}};
        const harken::CurveHandlePair handles[] = {{pts[0].x, pts[0].y, pts[1].x, pts[1].y}};
        ASSERT_TRUE(curve.SetPoints(pts, handles, 2));
        ASSERT_TRUE(engine->params.BindVoiceTarget(
            kMixVariable, harkentest::RenderHandle(engine, handle),
            harken::kTargetVoiceVolume, curve));
        gainSumOld += 1.0f;
        gainSumNew += DbToGain(deltaDb);
    }
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(0u, engine->propertyPublisher.DeferredCount());

    // Settle and measure the old mix. The DC image scales linearly with
    // the summed voice gains, so the new mix is the measured old one
    // scaled by the gain-sum ratio — no pan-law constant needed.
    Capture settle;
    harkentest::PumpAndCapture(engine, 2, settle);
    const float ampOld = BlockTail(settle, 1);
    ASSERT_GT(ampOld, harkentest::kSilenceFloor);
    const float ampNew = ampOld * (gainSumNew / gainSumOld);
    // The nearest partial subset (19 of 20 applied) sits at least
    // (1 - 10^(-6/20)) / 20 of the old mix away from the new sum; half a
    // percent of the old mix cleanly separates every subset.
    const float tolerance = 0.005f * ampOld;

    // Reach the write's actual block before creating queue pressure.
    ASSERT_EQ(VN_OK, Vn_SetValue(kMixVariable, 1.0f));
    harkentest::ReachVariableWriteEffectiveBlock(engine);
    EXPECT_EQ(kQueueCapacity, SpamSinglesUntilRefused());

    // The 20-target mix switch under a full queue applies as ONE audio
    // slice: every rendered block is flat at the old mix, flat at the new
    // mix, or the single one-block gain ramp between them — never a torn
    // subset held across blocks.
    bool rampSeen = false;
    bool sawNew = false;
    for (std::uint32_t block = 0; block < 32 && !sawNew; ++block) {
        ASSERT_EQ(VN_OK, Vn_Update());
        Capture step;
        harkentest::PumpAndCapture(engine, 1, step);
        if (BlockIsFlatAt(step, 0, ampOld, tolerance)) {
            ASSERT_FALSE(rampSeen)
                << "old mix returned after the transition began";
            continue;
        }
        if (BlockIsFlatAt(step, 0, ampNew, tolerance)) {
            sawNew = true;
            continue;
        }
        ASSERT_FALSE(rampSeen)
            << "second partial block: tail=" << BlockTail(step, 0)
            << " old=" << ampOld << " new=" << ampNew;
        rampSeen = true;
    }
    ASSERT_TRUE(sawNew);
    harkentest::AdvanceVariableWrite(engine);

    // Settled: the complete NEW mix, all 20 targets together.
    Capture appliedBlock;
    harkentest::PumpAndCapture(engine, 1, appliedBlock);
    ASSERT_GT(BlockTail(appliedBlock, 0), harkentest::kSilenceFloor);
    EXPECT_NEAR(ampNew, BlockTail(appliedBlock, 0), tolerance);
}

TEST(PropertyPressureGolden, SustainedSingleTrafficMeetsOneTickBound) {
    constexpr std::uint32_t kVoices = 4;
    constexpr std::uint32_t kQueueCapacity = 8; // power of two: exact

    VnConfig config = MakeNullConfig(6, kBlockFrames, 48000);
    config.commandQueueDepth = kQueueCapacity;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallPressureVariables(engine);

    const std::vector<float> dc = harkentest::MakeDC(96000, kDcAmplitude);
    const VnPCMDesc desc = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    ASSERT_EQ(VN_OK, Vn_SetValue(kMixVariable, 0.0f));

    for (std::uint32_t i = 0; i < kVoices; ++i) {
        VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
        VnVoiceHandle handle = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        harken::Curve curve;
        const harken::CurvePoint pts[] = {{0.0f, 0.0f}, {1.0f, -12.0f}};
        const harken::CurveHandlePair handles[] = {{pts[0].x, pts[0].y, pts[1].x, pts[1].y}};
        ASSERT_TRUE(curve.SetPoints(pts, handles, 2));
        ASSERT_TRUE(engine->params.BindVoiceTarget(
            kMixVariable, harkentest::RenderHandle(engine, handle),
            harken::kTargetVoiceVolume, curve));
    }
    ASSERT_EQ(VN_OK, Vn_Update());

    Capture settle;
    harkentest::PumpAndCapture(engine, 2, settle);
    const float ampBase = BlockTail(settle, 1); // all bindings at 0 dB
    ASSERT_GT(ampBase, harkentest::kSilenceFloor);
    const auto ampAt = [ampBase](float value) {
        return ampBase * (1.0f - value + value * DbToGain(-12.0f));
    };
    const float tolerance = 0.005f * ampBase;

    // Soak: every iteration flips the mix while a full burst of
    // single-command traffic saturates the queue at the write's effective
    // block. The Variable rides its own audio slice, so sustained
    // unrelated traffic can neither tear a flip across blocks nor starve
    // it: every rendered block is the previous mix, the new mix, or the
    // single transition ramp, and every flip settles within the horizon.
    float previousValue = 0.0f;
    for (int iteration = 1; iteration <= 12; ++iteration) {
        const float value = (iteration % 2 != 0) ? 0.75f : 0.25f;

        ASSERT_EQ(VN_OK, Vn_SetValue(kMixVariable, value));
        harkentest::ReachVariableWriteEffectiveBlock(engine);
        ASSERT_EQ(kQueueCapacity, SpamSinglesUntilRefused())
            << "iteration " << iteration;

        bool rampSeen = false;
        bool sawNew = false;
        for (std::uint32_t block = 0; block < 32 && !sawNew; ++block) {
            ASSERT_EQ(VN_OK, Vn_Update());
            (void)SpamSinglesUntilRefused();
            Capture step;
            harkentest::PumpAndCapture(engine, 1, step);
            if (BlockIsFlatAt(step, 0, ampAt(previousValue), tolerance)) {
                ASSERT_FALSE(rampSeen)
                    << "iteration " << iteration
                    << ": previous mix returned after the transition began";
                continue;
            }
            if (BlockIsFlatAt(step, 0, ampAt(value), tolerance)) {
                sawNew = true;
                continue;
            }
            ASSERT_FALSE(rampSeen)
                << "iteration " << iteration
                << " second partial block: tail=" << BlockTail(step, 0);
            rampSeen = true;
        }
        ASSERT_TRUE(sawNew) << "iteration " << iteration;
        harkentest::AdvanceVariableWrite(engine);

        previousValue = value;
    }
}

TEST(PropertyPressureGolden, DirectSetterDuringDeferralIsNotOverwritten) {
    // While a Variable write's target is claimed for a prepared bank cut,
    // a direct setter on the same target refuses VN_BACKPRESSURE with zero
    // side effects. After the cut applies, the setter lands and composes
    // with the applied binding contribution; the applied cut never
    // overwrites the later setter.
    constexpr std::uint32_t kQueueCapacity = 8;

    VnConfig config = MakeNullConfig(4, kBlockFrames, 48000);
    config.commandQueueDepth = kQueueCapacity;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallPressureVariables(engine);

    const std::vector<float> dc = harkentest::MakeDC(96000, kDcAmplitude);
    const VnPCMDesc desc = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    ASSERT_EQ(VN_OK, Vn_SetValue(kMixVariable, 0.0f));

    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    harken::Curve curve;
    const harken::CurvePoint pts[] = {{0.0f, 0.0f}, {1.0f, -12.0f}};
    const harken::CurveHandlePair handles[] = {{pts[0].x, pts[0].y, pts[1].x, pts[1].y}};
    ASSERT_TRUE(curve.SetPoints(pts, handles, 2));
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        kMixVariable, harkentest::RenderHandle(engine, handle),
        harken::kTargetVoiceVolume, curve));
    ASSERT_EQ(VN_OK, Vn_Update());

    Capture settle;
    harkentest::PumpAndCapture(engine, 2, settle);
    const float ampBase = BlockTail(settle, 1);
    ASSERT_GT(ampBase, harkentest::kSilenceFloor);

    // Reach the write's effective block and prepare its claim.
    ASSERT_EQ(VN_OK, Vn_SetValue(kMixVariable, 1.0f));
    harkentest::ReachVariableWriteEffectiveBlock(engine);
    ASSERT_EQ(VN_OK, Vn_Update());

    // The claimed window: the direct setter refuses without mutating.
    VnResult refused = VN_OK;
    for (std::uint32_t attempt = 0; attempt < 4; ++attempt) {
        refused = Vn_SetVoiceVolume(handle, 6.0f, 10);
        if (refused == VN_BACKPRESSURE) {
            break;
        }
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    EXPECT_EQ(VN_BACKPRESSURE, refused);

    // Drain the cut, then the setter lands and composes with the applied
    // binding contribution: base + 6 dB - 12 dB = -6 dB.
    harkentest::AdvanceVariableWrite(engine);
    ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(handle, 6.0f, 10));
    ASSERT_EQ(VN_OK, Vn_Update());
    Capture settled;
    harkentest::PumpAndCapture(engine, 4, settled);
    const float tail = BlockTail(settled, 3);
    const float ampSetter = ampBase * DbToGain(-6.0f);
    const float ampStale = ampBase * DbToGain(-12.0f);
    EXPECT_NEAR(ampSetter, tail, 0.005f * ampBase);
    EXPECT_GT(std::fabs(tail - ampStale), 0.05f * ampBase);
}

// Captures every drained log record (level, code, text) so tests can
// assert overflow and config-estimate records by code.
struct LogCodeSink {
    struct Rec {
        std::uint32_t level;
        std::uint32_t code;
        std::string text;
    };
    std::vector<Rec> records;

    static void Callback(void* user, std::uint32_t level, std::uint32_t code,
                         const char* text) {
        static_cast<LogCodeSink*>(user)->records.push_back(
            {level, code, text != nullptr ? text : ""});
    }
    const Rec* Find(std::uint32_t code) const {
        for (const Rec& r : records) {
            if (r.code == code) {
                return &r;
            }
        }
        return nullptr;
    }
};

TEST(PropertyPressureGolden,
     SustainedSpatialPressureBoundsLaterVoiceAndBusTargets) {
    constexpr std::uint32_t kQueueCapacity = 4;
    constexpr int kLatencyBoundTicks = 6;
    constexpr VnVariableId kLateVariable = harkentest::ObjectId(19);
    constexpr VnBusId kLateBus = harkentest::ObjectId(45);
    constexpr VnBindingId kLateBusBinding = harkentest::ObjectId(91);

    VnConfig config = MakeNullConfig(6, kBlockFrames, 48000);
    config.commandQueueDepth = kQueueCapacity;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallPressureVariables(engine);

    const std::vector<float> silent = harkentest::MakeDC(96000, 0.0f);
    const std::vector<float> audible = harkentest::MakeDC(96000, 0.25f);
    const VnPCMDesc silentDesc = MonoDesc(silent, 48000);
    const VnPCMDesc audibleDesc = MonoDesc(audible, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &silentDesc));
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(2), &audibleDesc));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kLateBus, VN_BUS_MASTER));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK,
              Vn_SetListener(harkentest::Vec3(0.0f, 0.0f, 10.0f),
                             harkentest::Vec3(0.0f, 0.0f, -1.0f),
                             harkentest::Vec3(0.0f, 1.0f, 0.0f)));

    // Five independent early slots each contribute one changing pan target.
    // A listener move changes all five, so every deliberately saturated tick
    // presents more early demand than the four-entry batch can hold. A drained
    // retry may catch up; the next saturated tick renews producer pressure.
    for (int i = 0; i < 5; ++i) {
        VnVoiceHandle spatialVoice = 0;
        VnPlayDesc spatial =
            harkentest::MakePlayDesc3D(harkentest::ObjectId(1), 0.0f, 0.0f, 0.0f);
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&spatial, &spatialVoice));
        engine->nullBackend.Pump(1);
    }

    VnVoiceHandle lateVoice = 0;
    VnPlayDesc late = MakePlayDesc(harkentest::ObjectId(2));
    late.busId = kLateBus;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&late, &lateVoice));
    harken::Curve lateCurve;
    const harken::CurvePoint latePoints[] = {
        {0.0f, 0.0f}, {1.0f, -24.0f}};
    const harken::CurveHandlePair lateHandles[] = {{latePoints[0].x, latePoints[0].y, latePoints[1].x, latePoints[1].y}};
    ASSERT_TRUE(lateCurve.SetPoints(latePoints, lateHandles, 2));
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        kLateVariable, harkentest::RenderHandle(engine, lateVoice),
        harken::kTargetVoiceVolume, lateCurve));

    VnBindingDesc busBinding{};
    busBinding.structSize = sizeof(VnBindingDesc);
    busBinding.variableId = kLateVariable;
    busBinding.targetScopeKind = VN_TARGET_SCOPE_BUS;
    busBinding.targetId = harken::kBusTargetBusVolume;
    busBinding.curve.structSize = sizeof(VnCurveDesc);
    busBinding.curve.pointCount = 2;
    busBinding.curve.points[0].x = 0.0f;
    busBinding.curve.points[0].y = 0.0f;
    busBinding.curve.points[1].x = 1.0f;
    busBinding.curve.points[1].y = -24.0f;
    busBinding.curve.handles[0] = harkentest::EndpointHandles(
        busBinding.curve.points[0], busBinding.curve.points[1]);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(kLateBusBinding, &busBinding));
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(VnBindingAttachDesc);
    attach.scopeKind = VN_TARGET_SCOPE_BUS;
    attach.objectId = kLateBus;
    ASSERT_EQ(VN_OK, Vn_AttachBinding(kLateBusBinding, &attach));
    engine->nullBackend.Pump(1);

    // Let the initial spatial image drain while its geometry is stable.
    for (int tick = 0; tick < 8; ++tick) {
        ASSERT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(1);
    }
    ASSERT_EQ(0u, engine->params.DirtyTargetCount());
    ASSERT_EQ(0u, engine->busTargets.DemandCount());
    const std::uint32_t lateBusIndex = engine->buses.FindIndex(kLateBus);
    ASSERT_FLOAT_EQ(0.0f, engine->busTargets.LastCommitted(
                              lateBusIndex, harken::kBusTargetBusVolume));
    Capture baselineBlock;
    harkentest::PumpAndCapture(engine, 1, baselineBlock);
    const float ampBaseline = BlockTail(baselineBlock, 0);
    ASSERT_GT(ampBaseline, harkentest::kSilenceFloor);
    const float ampPartial = ampBaseline * DbToGain(-24.0f);
    const float ampMutated = ampBaseline * DbToGain(-48.0f);
    const float renderTolerance = 0.005f * ampBaseline;

    const VnVoiceHandle lateRender =
        harkentest::RenderHandle(engine, lateVoice);
    const auto lateVoiceValue = [&]() {
        return engine->params.ResolvedValue(lateRender,
                                            harken::kTargetVoiceVolume);
    };
    ASSERT_FLOAT_EQ(0.0f, lateVoiceValue());

    int pressurePhase = 0;
    const auto pumpDeferredPressure = [&](float oldValue, float newValue,
                                          float oldAmplitude,
                                          float partialAmplitude,
                                          float newAmplitude) {
        int voicePublishedTick = 0;
        int busPublishedTick = 0;
        int renderedPublishedTick = 0;
        bool finalBlockAtNewAmplitude = false;
        for (int tick = 0; tick < kLatencyBoundTicks; ++tick) {
            // Every other tick starts with a completely full queue. The
            // gathered chunk must defer whole, then retry while geometry
            // keeps changing after the filler commands drain.
            const bool deferringTick = (tick & 1) == 0;
            if (deferringTick) {
                ASSERT_EQ(kQueueCapacity, SpamSinglesUntilRefused());
            }
            // A two-point flip is not sustained pressure: after a deferred
            // move, flipping back can reduce exactly to lastCommitted and
            // clean the target without publishing. This monotonic trajectory
            // gives every tick a new azimuth, distinct from both the previous
            // slot value and every earlier committed pan.
            const float listenerX =
                -static_cast<float>(++pressurePhase);
            ASSERT_EQ(VN_OK,
                      Vn_SetListener(
                          harkentest::Vec3(listenerX, 0.0f, 10.0f),
                          harkentest::Vec3(0.0f, 0.0f, -1.0f),
                          harkentest::Vec3(0.0f, 1.0f, 0.0f)));
            const std::uint64_t deferredBefore =
                engine->propertyPublisher.DeferredCount();
            const std::uint64_t overflowBefore =
                engine->paramBatchOverflowCount;
            ASSERT_EQ(VN_OK, Vn_Update());

            if (deferringTick) {
                // Producer pressure is explicit on the deliberately
                // saturated ticks: the batch overflows, remains dirty, and
                // defers whole. The drained retry is allowed to consume all
                // remaining demand; bounded later publication is asserted
                // independently across the complete active window.
                EXPECT_GT(engine->paramBatchOverflowCount, overflowBefore)
                    << "tick " << tick;
                EXPECT_GT(engine->params.DirtyTargetCount(), 0u)
                    << "tick " << tick;
                EXPECT_EQ(deferredBefore + 1u,
                          engine->propertyPublisher.DeferredCount());
                EXPECT_EQ(kQueueCapacity,
                          engine->propertyPublisher.PendingReservation());
                if (voicePublishedTick == 0) {
                    EXPECT_FLOAT_EQ(oldValue, lateVoiceValue());
                }
                if (busPublishedTick == 0) {
                    EXPECT_FLOAT_EQ(
                        oldValue,
                        engine->busTargets.LastCommitted(
                            lateBusIndex, harken::kBusTargetBusVolume));
                }
            } else {
                EXPECT_EQ(deferredBefore,
                          engine->propertyPublisher.DeferredCount());
                EXPECT_EQ(0u,
                          engine->propertyPublisher.PendingReservation());
            }

            if (voicePublishedTick == 0 &&
                lateVoiceValue() == newValue) {
                voicePublishedTick = tick + 1;
            }
            if (busPublishedTick == 0 &&
                engine->busTargets.LastCommitted(
                    lateBusIndex, harken::kBusTargetBusVolume) == newValue) {
                busPublishedTick = tick + 1;
            }
            Capture renderedBlock;
            harkentest::PumpAndCapture(engine, 1, renderedBlock);
            const bool atOldAmplitude = BlockIsFlatAt(
                renderedBlock, 0, oldAmplitude, renderTolerance);
            const bool atNewAmplitude = BlockIsFlatAt(
                renderedBlock, 0, newAmplitude, renderTolerance);
            const bool atPartialAmplitude = BlockIsFlatAt(
                renderedBlock, 0, partialAmplitude, renderTolerance);
            EXPECT_TRUE(atOldAmplitude || atPartialAmplitude ||
                        atNewAmplitude)
                << "tick " << tick
                << " rendered tail=" << BlockTail(renderedBlock, 0);
            if (deferringTick && tick == 0) {
                EXPECT_TRUE(atOldAmplitude)
                    << "deferred tick " << tick
                    << " rendered tail=" << BlockTail(renderedBlock, 0);
            }
            if (renderedPublishedTick == 0 && atNewAmplitude) {
                renderedPublishedTick = tick + 1;
            }
            finalBlockAtNewAmplitude = atNewAmplitude;
        }
        EXPECT_GT(voicePublishedTick, 0);
        EXPECT_LE(voicePublishedTick, kLatencyBoundTicks);
        EXPECT_GT(busPublishedTick, 0);
        EXPECT_LE(busPublishedTick, kLatencyBoundTicks);
        EXPECT_GT(renderedPublishedTick, 0);
        EXPECT_LE(renderedPublishedTick, kLatencyBoundTicks);
        EXPECT_TRUE(finalBlockAtNewAmplitude);
    };

    // Every geometry flip changes five early spatial targets, more than the
    // four-slot batch. A fixed-prefix gather mutant continuously selects the
    // same early four and leaves both later targets and the audible DC output
    // at their old values past this bound.
    ASSERT_EQ(VN_OK, Vn_SetValue(kLateVariable, 1.0f));
    harkentest::ReachVariableWriteEffectiveBlock(engine);
    pumpDeferredPressure(0.0f, -24.0f, ampBaseline, ampPartial,
                         ampMutated);
    EXPECT_FLOAT_EQ(-24.0f, lateVoiceValue());
    EXPECT_FLOAT_EQ(-24.0f, engine->busTargets.LastCommitted(
                                 lateBusIndex,
                                 harken::kBusTargetBusVolume));

    // Exact rollback is equally discriminating: persistent spatial pressure
    // cannot leave either later target stuck at the strong mutation.
    ASSERT_EQ(VN_OK, Vn_SetValue(kLateVariable, 0.0f));
    harkentest::ReachVariableWriteEffectiveBlock(engine);
    pumpDeferredPressure(-24.0f, 0.0f, ampMutated, ampPartial,
                         ampBaseline);
    EXPECT_FLOAT_EQ(0.0f, lateVoiceValue());
    EXPECT_FLOAT_EQ(0.0f, engine->busTargets.LastCommitted(
                               lateBusIndex,
                               harken::kBusTargetBusVolume));
    EXPECT_GT(engine->paramBatchOverflowCount, 0u);
}

// Drives one spatial scene whose single-tick dirty set (three moving
// spatialized voices, volume + pan each) exceeds a tiny command queue:
// two geometry waves, each followed by enough update/pump cycles to
// converge. Reports the final converged block, the overflow counter, and
// whether every tick's staged chunk published whole or deferred whole
// (no chunk-internal tear).
void RunSpatialScene(std::uint32_t queueDepth, LogCodeSink* sink,
                     Capture* finalBlock, std::uint64_t* overflowCount,
                     bool* chunkInvariantHeld) {
    VnConfig config = MakeNullConfig(4, kBlockFrames, 48000);
    config.commandQueueDepth = queueDepth;
    if (sink != nullptr) {
        config.logCallback = &LogCodeSink::Callback;
        config.logUserData = sink;
    }
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallPressureVariables(engine);

    const std::vector<float> dc = harkentest::MakeDC(96000, 0.25f);
    const VnPCMDesc desc = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    const VnAttenuationDesc profile =
        harkentest::MakeAttenuationDesc(1.0f, 100.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(9), &profile));
    ASSERT_EQ(VN_OK, Vn_SetListener(harkentest::Vec3(0.0f, 0.0f, 0.0f),
                                    harkentest::Vec3(0.0f, 0.0f, -1.0f),
                                    harkentest::Vec3(0.0f, 1.0f, 0.0f)));

    const float positions[3][3] = {
        {10.0f, 0.0f, 0.0f}, {-20.0f, 0.0f, 0.0f}, {7.0f, 0.0f, 7.0f}};
    for (int i = 0; i < 3; ++i) {
        VnPlayDesc play = harkentest::MakePlayDesc3D(
            harkentest::ObjectId(1), positions[i][0], positions[i][1], positions[i][2]);
        VnVoiceHandle handle = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, harkentest::ObjectId(9)));
    }
    engine->nullBackend.Pump(1); // drain the plays

    bool invariant = true;
    for (int wave = 0; wave < 2; ++wave) {
        if (wave == 1) {
            ASSERT_EQ(VN_OK,
                      Vn_SetListener(harkentest::Vec3(3.0f, 0.0f, 1.0f),
                                     harkentest::Vec3(0.0f, 0.0f, -1.0f),
                                     harkentest::Vec3(0.0f, 1.0f, 0.0f)));
        }
        for (int tick = 0; tick < 6; ++tick) {
            ASSERT_EQ(VN_OK, Vn_Update());
            // Chunk atomicity: each tick either published its staged
            // chunk whole or deferred it whole — never a partial
            // consumption of the chunk.
            const std::uint32_t staged =
                engine->propertyPublisher.StagedCount();
            if (staged != 0 &&
                engine->propertyPublisher.PendingReservation() != staged) {
                invariant = false;
            }
            engine->nullBackend.Pump(1);
        }
    }
    // Converged: a further update stages nothing and no mark is dirty.
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(0u, engine->propertyPublisher.StagedCount());
    EXPECT_EQ(0u, engine->params.DirtyTargetCount());
    if (overflowCount != nullptr) {
        *overflowCount = engine->paramBatchOverflowCount;
    }
    if (chunkInvariantHeld != nullptr) {
        *chunkInvariantHeld = invariant;
    }
    harkentest::PumpAndCapture(engine, 1, *finalBlock);
}

TEST(PropertyPressureGolden, SpatialOverflowDegradesToChunksAndConverges) {
    // Spatializer-owned contributions sit outside the binding-instance
    // bound, so a valid config can dirty more targets in one tick than a
    // tiny queue holds. Publication must degrade to capacity-sized chunks
    // — counted, logged, no crash in any build — and converge to the same
    // final state an unconstrained queue reaches.
    Capture reference;
    std::uint64_t referenceOverflow = 0;
    bool referenceInvariant = true;
    ASSERT_NO_FATAL_FAILURE(RunSpatialScene(256, nullptr, &reference,
                                            &referenceOverflow,
                                            &referenceInvariant));
    EXPECT_EQ(0u, referenceOverflow);
    HARKENTEST_ASSERT_NON_SILENT(reference.left);

    LogCodeSink sink;
    Capture constrained;
    std::uint64_t overflow = 0;
    bool invariant = true;
    ASSERT_NO_FATAL_FAILURE(
        RunSpatialScene(4, &sink, &constrained, &overflow, &invariant));
    EXPECT_GE(overflow, 1u); // the degradation fired (and did not crash)
    EXPECT_NE(nullptr, sink.Find(harken::kLogCodePropertyBatchOverflow));
    EXPECT_TRUE(invariant);

    // Convergence: once traffic settles the constrained session's render
    // is the unconstrained session's render.
    ASSERT_EQ(reference.left.size(), constrained.left.size());
    float maxDiff = 0.0f;
    for (std::size_t i = 0; i < reference.left.size(); ++i) {
        const float diff = std::fabs(reference.left[i] - constrained.left[i]);
        if (diff > maxDiff) {
            maxDiff = diff;
        }
    }
    EXPECT_LT(maxDiff, 1e-6f);
}

TEST(PropertyPressureGolden, ConfigWorstCaseEstimateWarnsAtInit) {
    // A config whose simultaneous full-load tick cannot fit the queue
    // warns once at init with both numbers; a config with headroom warns
    // not at all. Observability only — nothing is refused.
    {
        LogCodeSink sink;
        VnConfig config = MakeNullConfig(8, kBlockFrames, 48000);
        config.commandQueueDepth = 4; // 8 * 5 spatial + 32 * 2 bus rows = 104 > 4
        config.logCallback = &LogCodeSink::Callback;
        config.logUserData = &sink;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        ASSERT_EQ(VN_OK, Vn_Update()); // drains the init-time record
        const LogCodeSink::Rec* rec =
            sink.Find(harken::kLogCodePropertyCapacityEstimate);
        ASSERT_NE(nullptr, rec);
        EXPECT_EQ(static_cast<std::uint32_t>(VN_LOG_WARN), rec->level);
        EXPECT_EQ(
            "property worst-case estimate 104 exceeds command queue "
            "capacity 4: publication will chunk under full load",
            rec->text);
    }
    {
        LogCodeSink sink;
        VnConfig config = MakeNullConfig(2, kBlockFrames, 48000);
        // 2 * 5 spatial + 64 bus rows = 74 <= 256: within the queue.
        config.logCallback = &LogCodeSink::Callback;
        config.logUserData = &sink;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        ASSERT_EQ(VN_OK, Vn_Update());
        EXPECT_EQ(nullptr,
                  sink.Find(harken::kLogCodePropertyCapacityEstimate));
    }
}

TEST(PropertyPressureGolden, DeferredGrowthIsReservedBeforeTargetsJoin) {
    // From the moment of deferral, the structural worst case reserves room
    // for targets that may become dirty before the retry. Waiting for the
    // write itself is too late: singles admitted beyond the old batch cannot
    // be reclaimed when the rebuilt batch grows.
    constexpr std::uint32_t kQueueCapacity = 8;
    constexpr VnVariableId kSecondVariable = harkentest::ObjectId(8);

    VnConfig config = MakeNullConfig(10, kBlockFrames, 48000);
    config.commandQueueDepth = kQueueCapacity;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallPressureVariables(engine);

    const std::vector<float> dc = harkentest::MakeDC(96000, kDcAmplitude);
    const VnPCMDesc desc = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    ASSERT_EQ(VN_OK, Vn_SetValue(kMixVariable, 0.0f));
    ASSERT_EQ(VN_OK, Vn_SetValue(kSecondVariable, 0.0f));

    // Eight bound voices: four on each variable, one volume target each.
    for (std::uint32_t i = 0; i < 8; ++i) {
        VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
        VnVoiceHandle handle = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        harken::Curve curve;
        const harken::CurvePoint pts[] = {{0.0f, 0.0f}, {1.0f, -12.0f}};
        const harken::CurveHandlePair handles[] = {{pts[0].x, pts[0].y, pts[1].x, pts[1].y}};
        ASSERT_TRUE(curve.SetPoints(pts, handles, 2));
        ASSERT_TRUE(engine->params.BindVoiceTarget(
            (i < 4) ? kMixVariable : kSecondVariable,
            harkentest::RenderHandle(engine, handle),
            harken::kTargetVoiceVolume, curve));
    }
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);

    // Two keys write under a saturated queue: each bank cut lands whole
    // through its own audio slice — a growing dirty census can never split
    // a cut or strand the other key's cut behind admitted traffic.
    ASSERT_EQ(VN_OK, Vn_SetValue(kMixVariable, 1.0f));
    harkentest::ReachVariableWriteEffectiveBlock(engine);
    ASSERT_EQ(kQueueCapacity, SpamSinglesUntilRefused());
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);

    ASSERT_EQ(VN_OK, Vn_SetValue(kSecondVariable, 1.0f));
    harkentest::AdvanceVariableWrite(engine);
    EXPECT_EQ(0u, engine->variables.PendingCount());
    float value = 0.0f;
    ASSERT_TRUE(engine->variables.CurrentValue(kMixVariable, &value));
    EXPECT_FLOAT_EQ(1.0f, value);
    ASSERT_TRUE(engine->variables.CurrentValue(kSecondVariable, &value));
    EXPECT_FLOAT_EQ(1.0f, value);
    EXPECT_EQ(0u, engine->params.DirtyTargetCount());
}

TEST(PropertyPressureGolden,
     DeferredBatchGrowthStillLandsOnTheFirstRetryWhole) {
    constexpr std::uint32_t kQueueCapacity = 8;
    constexpr std::uint32_t kInitialBatch = 4;
    constexpr VnVariableId kGrowthVariable = harkentest::ObjectId(8);

    VnConfig config = MakeNullConfig(10, kBlockFrames, 48000);
    config.commandQueueDepth = kQueueCapacity;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallPressureVariables(engine);

    const std::vector<float> dc = harkentest::MakeDC(96000, kDcAmplitude);
    const VnPCMDesc desc = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    ASSERT_EQ(VN_OK, Vn_SetValue(kMixVariable, 0.0f));
    ASSERT_EQ(VN_OK, Vn_SetValue(kGrowthVariable, 0.0f));

    float gainSumNew = 0.0f;
    for (std::uint32_t i = 0; i < kQueueCapacity; ++i) {
        VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
        VnVoiceHandle handle = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        const float deltaDb = -(3.0f + static_cast<float>(i));
        harken::Curve curve;
        const harken::CurvePoint pts[] = {{0.0f, 0.0f}, {1.0f, deltaDb}};
        const harken::CurveHandlePair handles[] = {{pts[0].x, pts[0].y, pts[1].x, pts[1].y}};
        ASSERT_TRUE(curve.SetPoints(pts, handles, 2));
        ASSERT_TRUE(engine->params.BindVoiceTarget(
            (i < kInitialBatch) ? kMixVariable : kGrowthVariable,
            harkentest::RenderHandle(engine, handle),
            harken::kTargetVoiceVolume, curve));
        const float input = (i < kInitialBatch) ? 1.0f : 0.75f;
        gainSumNew += 1.0f - input + input * DbToGain(deltaDb);
    }
    ASSERT_EQ(kQueueCapacity,
              engine->propertyPublisher.WorstCaseCommits());
    ASSERT_EQ(VN_OK, Vn_Update());

    Capture settle;
    harkentest::PumpAndCapture(engine, 2, settle);
    const float ampOld = BlockTail(settle, 1);
    ASSERT_GT(ampOld, harkentest::kSilenceFloor);
    const float ampNew = ampOld * (gainSumNew / kQueueCapacity);
    const float tolerance = 0.005f * ampOld;

    // Two keys write back to back under a saturated queue. Each bank cut
    // lands whole through its own audio slice; after both settle, all
    // eight distinct gains are exactly at the two-key mix — no partial
    // subset, no starved key.
    ASSERT_EQ(VN_OK, Vn_SetValue(kMixVariable, 1.0f));
    harkentest::ReachVariableWriteEffectiveBlock(engine);
    ASSERT_EQ(kQueueCapacity, SpamSinglesUntilRefused());
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_SetValue(kGrowthVariable, 0.75f));
    harkentest::AdvanceVariableWrite(engine);

    Capture appliedBlock;
    harkentest::PumpAndCapture(engine, 1, appliedBlock);
    EXPECT_TRUE(BlockIsFlatAt(appliedBlock, 0, ampNew, tolerance))
        << "tail=" << BlockTail(appliedBlock, 0) << " expected=" << ampNew;

    // Exact rollback proves the value-level mutation rather than relying on
    // counters alone: all eight distinct gains return to the original mix.
    ASSERT_EQ(VN_OK, Vn_SetValue(kMixVariable, 0.0f));
    ASSERT_EQ(VN_OK, Vn_SetValue(kGrowthVariable, 0.0f));
    harkentest::AdvanceVariableWrite(engine);
    Capture rolledBackBlock;
    harkentest::PumpAndCapture(engine, 1, rolledBackBlock);
    EXPECT_TRUE(BlockIsFlatAt(rolledBackBlock, 0, ampOld, tolerance))
        << "tail=" << BlockTail(rolledBackBlock, 0) << " expected=" << ampOld;
}

TEST(PropertyPressureGolden, BusDemandExtendsTheReservationLive) {
    // The bus-side mirror of the live-reservation rule: while a deferred
    // batch exists, a Variable write that moves BUS bindings joins the
    // re-derived demand the moment it lands — bus targets and voice
    // targets share one batch, so the reservation is their union, and
    // later singles can never consume room the grown batch is already
    // known to need. The union publishes as ONE batch at the earliest
    // tick with room.
    constexpr std::uint32_t kQueueCapacity = 8;
    constexpr VnVariableId kSidechainVariable = harkentest::ObjectId(8);
    constexpr VnBusId kBusOne = harkentest::ObjectId(45);
    constexpr VnBusId kBusTwo = harkentest::ObjectId(46);
    constexpr VnBindingId kSidechainBinding = harkentest::ObjectId(90);

    VnConfig config = MakeNullConfig(10, kBlockFrames, 48000);
    config.commandQueueDepth = kQueueCapacity;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallPressureVariables(engine);

    const std::vector<float> dc = harkentest::MakeDC(96000, kDcAmplitude);
    const VnPCMDesc desc = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    ASSERT_EQ(VN_OK, Vn_SetValue(kMixVariable, 0.0f));
    ASSERT_EQ(VN_OK, Vn_SetValue(kSidechainVariable, 0.0f));
    harkentest::AdvanceVariableWrite(engine);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusOne, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusTwo, VN_BUS_MASTER));

    // The deferring batch: four voice-volume bindings on the first
    // variable (the established pattern).
    for (std::uint32_t i = 0; i < 4; ++i) {
        VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
        VnVoiceHandle handle = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        harken::Curve curve;
        const harken::CurvePoint pts[] = {{0.0f, 0.0f}, {1.0f, -12.0f}};
        const harken::CurveHandlePair handles[] = {{pts[0].x, pts[0].y, pts[1].x, pts[1].y}};
        ASSERT_TRUE(curve.SetPoints(pts, handles, 2));
        ASSERT_TRUE(engine->params.BindVoiceTarget(
            kMixVariable, harkentest::RenderHandle(engine, handle),
            harken::kTargetVoiceVolume, curve));
    }

    // The bus-side growth source: ONE public definition on the second
    // variable fanned out to two buses — two unique bus targets.
    VnBindingDesc sidechain{};
    sidechain.structSize = sizeof(VnBindingDesc);
    sidechain.variableId = kSidechainVariable;
    sidechain.targetScopeKind = VN_TARGET_SCOPE_BUS;
    sidechain.targetId = 0; // BusVolume
    sidechain.curve.structSize = sizeof(VnCurveDesc);
    sidechain.curve.pointCount = 2;
    sidechain.curve.points[0].x = 0.0f;
    sidechain.curve.points[0].y = 0.0f;
    sidechain.curve.points[1].x = 1.0f;
    sidechain.curve.points[1].y = -12.0f;
    sidechain.curve.handles[0] = harkentest::EndpointHandles(
        sidechain.curve.points[0], sidechain.curve.points[1]);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(kSidechainBinding, &sidechain));
    for (const VnBusId busId : {kBusOne, kBusTwo}) {
        VnBindingAttachDesc attach{};
        attach.structSize = sizeof(VnBindingAttachDesc);
        attach.scopeKind = VN_TARGET_SCOPE_BUS;
        attach.objectId = busId;
        ASSERT_EQ(VN_OK, Vn_AttachBinding(kSidechainBinding, &attach));
    }
    // Settle: the attachments evaluate to a zero contribution on a zero
    // input, so nothing publishes and every mark is clean.
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    ASSERT_EQ(0u, engine->busTargets.DemandCount());

    // Two keys — one driving four voice targets, one fanned to two bus
    // targets — write under a saturated queue. Each bank cut carries its
    // whole exact target set through its own audio slice; after both
    // settle, the voice mix and both bus references landed complete.
    ASSERT_EQ(VN_OK, Vn_SetValue(kMixVariable, 1.0f));
    harkentest::ReachVariableWriteEffectiveBlock(engine);
    ASSERT_EQ(kQueueCapacity, SpamSinglesUntilRefused());
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);

    ASSERT_EQ(VN_OK, Vn_SetValue(kSidechainVariable, 1.0f));
    harkentest::AdvanceVariableWrite(engine);
    EXPECT_EQ(0u, engine->variables.PendingCount());
    EXPECT_EQ(0u, engine->params.DirtyTargetCount());
    EXPECT_EQ(0u, engine->busTargets.DemandCount());
    const std::uint32_t busOneIndex = engine->buses.FindIndex(kBusOne);
    const std::uint32_t busTwoIndex = engine->buses.FindIndex(kBusTwo);
    EXPECT_FLOAT_EQ(-12.0f, engine->busTargets.LastCommitted(
                                busOneIndex, harken::kBusTargetBusVolume));
    EXPECT_FLOAT_EQ(-12.0f, engine->busTargets.LastCommitted(
                                busTwoIndex, harken::kBusTargetBusVolume));
}

TEST(PropertyPressureGolden, SetterCannotEraseKnownDemandDuringDeferral) {
    // A direct setter admitted during a deferral may clean the dirty
    // state it owns, but demand marked by a Variable write it has not
    // yet been evaluated (pending input) survives the setter: the census
    // keeps counting the target, later singles stay blocked at the grown
    // boundary, and the next gather rebuilds the FULL union — the
    // original deferred commits land at their tick, never re-deferred by
    // admitted traffic.
    constexpr std::uint32_t kQueueCapacity = 8;
    constexpr VnVariableId kSecondVariable = harkentest::ObjectId(8);

    VnConfig config = MakeNullConfig(8, kBlockFrames, 48000);
    config.commandQueueDepth = kQueueCapacity;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallPressureVariables(engine);

    const std::vector<float> dc = harkentest::MakeDC(96000, kDcAmplitude);
    const VnPCMDesc desc = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    ASSERT_EQ(VN_OK, Vn_SetValue(kMixVariable, 0.0f));
    ASSERT_EQ(VN_OK, Vn_SetValue(kSecondVariable, 0.0f));

    // Four voices on the first variable, two on the second; the setter
    // below hits one of the second pair.
    VnVoiceHandle setterTarget = 0;
    for (std::uint32_t i = 0; i < 6; ++i) {
        VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
        VnVoiceHandle handle = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        harken::Curve curve;
        const harken::CurvePoint pts[] = {{0.0f, 0.0f}, {1.0f, -12.0f}};
        const harken::CurveHandlePair handles[] = {{pts[0].x, pts[0].y, pts[1].x, pts[1].y}};
        ASSERT_TRUE(curve.SetPoints(pts, handles, 2));
        ASSERT_TRUE(engine->params.BindVoiceTarget(
            (i < 4) ? kMixVariable : kSecondVariable,
            harkentest::RenderHandle(engine, handle),
            harken::kTargetVoiceVolume, curve));
        if (i == 4) {
            setterTarget = handle;
        }
    }
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);

    // Two keys write under a saturated queue while a direct setter lands
    // on one of the second key's targets mid-flight. The setter cannot
    // erase the accepted write: both bank cuts land whole, and the setter
    // composes with the applied contribution instead of replacing it.
    ASSERT_EQ(VN_OK, Vn_SetValue(kMixVariable, 1.0f));
    harkentest::ReachVariableWriteEffectiveBlock(engine);
    ASSERT_EQ(kQueueCapacity, SpamSinglesUntilRefused());
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);

    ASSERT_EQ(VN_OK, Vn_SetValue(kSecondVariable, 1.0f));
    for (std::uint32_t attempt = 0; attempt < 32; ++attempt) {
        if (Vn_SetVoiceVolume(setterTarget, 3.0f, 0) != VN_BACKPRESSURE) {
            break;
        }
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    harkentest::AdvanceVariableWrite(engine);
    EXPECT_EQ(0u, engine->variables.PendingCount());
    float value = 0.0f;
    ASSERT_TRUE(engine->variables.CurrentValue(kSecondVariable, &value));
    EXPECT_FLOAT_EQ(1.0f, value);
    // The setter's +3 dB composed onto the applied -12 dB contribution.
    EXPECT_FLOAT_EQ(-9.0f,
                    engine->params.ResolvedValue(
                        harkentest::RenderHandle(engine, setterTarget),
                        harken::kTargetVoiceVolume));
}

TEST(PropertyPressureGolden, EmitterDestroyMarksFutureDemand) {
    // Destroying an emitter flips its voices' scoped Variable reads to
    // the global fallback — a latent input change no Variable write
    // announces. The census must grow at the destroy, and the next
    // evaluation realizes it (here: the local override disappears, so
    // the contribution returns to base and publishes).
    EngineSession session(MakeNullConfig(4, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallPressureVariables(engine);

    const std::vector<float> dc = harkentest::MakeDC(96000, kDcAmplitude);
    const VnPCMDesc desc = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    play.emitterHandle = emitter;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    harken::Curve curve;
    const harken::CurvePoint pts[] = {{0.0f, 0.0f}, {1.0f, -12.0f}};
    const harken::CurveHandlePair handles[] = {{pts[0].x, pts[0].y, pts[1].x, pts[1].y}};
    ASSERT_TRUE(curve.SetPoints(pts, handles, 2));
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        kMixVariable, harkentest::RenderHandle(engine, handle),
        harken::kTargetVoiceVolume, curve));

    // The emitter-local override alone drives the binding to -12 dB (the
    // global scope never holds a value).
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, kMixVariable, 1.0f));
    harkentest::AdvanceVariableWrite(engine);
    ASSERT_EQ(0u, engine->params.DirtyTargetCount());

    const std::uint64_t commitsBefore = engine->params.CommitCount();
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(emitter));
    EXPECT_EQ(1u, engine->params.DirtyTargetCount());

    // Evaluation realizes the fallback: no value at either scope, the
    // contribution retires, the target publishes back to base — and the
    // census returns to zero.
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(commitsBefore + 1u, engine->params.CommitCount());
    EXPECT_EQ(0u, engine->params.DirtyTargetCount());
}

TEST(PropertyPressureGolden,
     EmitterClearImmediatelyExtendsActiveDeferralDemand) {
    constexpr std::uint32_t kQueueCapacity = 8;
    constexpr VnVariableId kOtherVariable = harkentest::ObjectId(8);
    VnConfig config = MakeNullConfig(8, kBlockFrames, 48000);
    config.commandQueueDepth = kQueueCapacity;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallPressureVariables(engine);

    const std::vector<float> dc =
        harkentest::MakeDC(96000, kDcAmplitude);
    const VnPCMDesc desc = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));

    for (std::uint32_t i = 0; i < 6; ++i) {
        VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
        if (i >= 4) {
            play.emitterHandle = emitter;
        }
        VnVoiceHandle handle = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        harken::Curve curve;
        const harken::CurvePoint points[] = {
            {0.0f, 0.0f}, {1.0f, -12.0f}};
        const harken::CurveHandlePair handles[] = {
            {points[0].x, points[0].y, points[1].x, points[1].y}};
        ASSERT_TRUE(curve.SetPoints(points, handles, 2));
        ASSERT_TRUE(engine->params.BindVoiceTarget(
            i < 4 ? kMixVariable : kOtherVariable,
            harkentest::RenderHandle(engine, handle),
            harken::kTargetVoiceVolume, curve));
    }
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, kOtherVariable, 1.0f));
    harkentest::AdvanceVariableWrite(engine);

    // A Global write on one key while a saturated queue holds unrelated
    // traffic, then a scoped Clear on the other key: both are Variable
    // bank cuts and land through their own audio slices, so neither the
    // spam nor each other can tear or starve them.
    ASSERT_EQ(VN_OK, Vn_SetValue(kMixVariable, 1.0f));
    harkentest::ReachVariableWriteEffectiveBlock(engine);
    ASSERT_EQ(kQueueCapacity, SpamSinglesUntilRefused());
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);

    ASSERT_EQ(VN_OK, Vn_SetValue(kOtherVariable, 0.0f));
    harkentest::AdvanceVariableWrite(engine);
    EXPECT_EQ(0u, engine->variables.PendingCount());
    EXPECT_EQ(0u, engine->propertyPublisher.PendingReservation());
    float value = 0.0f;
    ASSERT_TRUE(engine->variables.CurrentValue(
        emitter, kOtherVariable, &value));
    EXPECT_FLOAT_EQ(0.0f, value);
}

TEST(PropertyPressureGolden, CommandFreeStopAdmitsUnderFullReservation) {
    // Command-free work is never gated by the admission reservation:
    // stopping an emitter whose only voice is PARKED pushes zero commands,
    // so it succeeds under a full queue with an active deferral, ends the
    // parked voice, and counts no backpressure.
    constexpr std::uint32_t kQueueCapacity = 8;

    VnConfig config = MakeNullConfig(4, kBlockFrames, 48000);
    config.commandQueueDepth = kQueueCapacity;
    config.audibilityParkEnabled = 1;
    config.parkThresholdDb = -40.0f;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallPressureVariables(engine);

    const std::vector<float> dc = harkentest::MakeDC(96000, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    const VnAttenuationDesc farProfile =
        harkentest::MakeAttenuationDesc(1.0f, 100.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(9), &farProfile));

    // An attached spatialized voice at distance 100 parks as inaudible
    // (its render slot frees; the emitter keeps only the parked voice).
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_EQ(VN_OK,
              Vn_SetEmitterPosition(emitter, harkentest::Vec3(100.0f, 0.0f,
                                                             0.0f)));
    VnPlayDesc attached = harkentest::MakePlayDesc3D(harkentest::ObjectId(1), 100.0f, 0.0f, 0.0f);
    attached.emitterHandle = emitter;
    VnVoiceHandle parked = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&attached, &parked));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(parked, harkentest::ObjectId(9)));
    ASSERT_EQ(VN_OK, Vn_SetListener(harkentest::Vec3(0.0f, 0.0f, 0.0f),
                                    harkentest::Vec3(0.0f, 0.0f, -1.0f),
                                    harkentest::Vec3(0.0f, 1.0f, 0.0f)));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(parked).kind);

    // An unrelated bound voice supplies the deferral batch.
    ASSERT_EQ(VN_OK, Vn_SetValue(kMixVariable, 0.0f));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle bound = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &bound));
    harken::Curve curve;
    const harken::CurvePoint pts[] = {{0.0f, 0.0f}, {1.0f, -12.0f}};
    const harken::CurveHandlePair handles[] = {{pts[0].x, pts[0].y, pts[1].x, pts[1].y}};
    ASSERT_TRUE(curve.SetPoints(pts, handles, 2));
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        kMixVariable, harkentest::RenderHandle(engine, bound),
        harken::kTargetVoiceVolume, curve));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);

    // Saturate the queue with the Variable's bank cut in flight: the cut
    // occupies its slice claim while spam holds every command slot.
    ASSERT_EQ(VN_OK, Vn_SetValue(kMixVariable, 1.0f));
    harkentest::ReachVariableWriteEffectiveBlock(engine);
    ASSERT_EQ(kQueueCapacity, SpamSinglesUntilRefused());
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_GE(engine->variables.PendingCount(), 1u);

    // The stop needs zero commands (no live attached voices): it admits
    // trivially, ends the parked voice, and counts no backpressure.
    const std::uint64_t backpressureBefore =
        engine->commands.BackpressureCount();
    EXPECT_EQ(VN_OK, Vn_StopEmitter(emitter, 0));
    EXPECT_EQ(backpressureBefore, engine->commands.BackpressureCount());
    EXPECT_EQ(0u, engine->ledger.ParkedCount());
    EXPECT_NE(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(parked).kind);
    // The in-flight bank cut is untouched by the stop and still completes.
    harkentest::AdvanceVariableWrite(engine);
    EXPECT_EQ(0u, engine->variables.PendingCount());
}

} // namespace
