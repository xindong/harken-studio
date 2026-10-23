// Voice priority and the three steal policies, plus the pending-start
// lifecycle a steal creates: deterministic victim selection (quietest and
// oldest, with the render-slot tie-break), never stealing a higher-priority
// voice, the refuse policy's exact prior behavior, the victim surfacing as
// VN_NOTIFY_VOICE_STOLEN, the one-block declick's bounded step, the deferred
// start's block latency, pending-start snapshot exclusion and counting,
// cancelling a pending start with Vn_Stop, staged per-voice state applied at
// the deferred start, and the old-PlayDesc-vintage default priority.
#include "Common/ObjectIdTestSupport.h"
#include "Common/RecipePackTestUtil.h"
#include "Common/TestUtil.h"
#include "VoicePoolReleaseTestAccess.h"

#include "harken/param/EffectParamLocator.h"
#include "harken/render/BuiltinEffects.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <vector>

namespace {

using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::MonoDesc;

constexpr std::uint32_t kBlock = 256;
constexpr std::uint32_t kRate = 48000;

VnConfig StealConfig(std::uint32_t maxVoices, std::uint32_t policy) {
    VnConfig config = MakeNullConfig(maxVoices, kBlock, kRate);
    config.voiceStealPolicy = policy;
    return config;
}

VnPlayDesc PlayWith(VnSoundId sound, std::uint32_t priority,
                    float volumeDb = 0.0f) {
    VnPlayDesc play = MakePlayDesc(sound, volumeDb);
    play.priority = priority;
    return play;
}

struct NotificationLog {
    std::vector<VnNotification> notifications;
    std::size_t CountOfType(std::uint32_t type) const {
        std::size_t n = 0;
        for (const VnNotification& e : notifications) {
            if (e.type == type) {
                ++n;
            }
        }
        return n;
    }
};

void RecordNotification(const VnNotification* notification, void* userData) {
    static_cast<NotificationLog*>(userData)->notifications.push_back(*notification);
}

VnSnapshot Snap() {
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    EXPECT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    return snapshot;
}

// A long steady source so the tested voices never end on their own within the
// handful of blocks each drill pumps.
std::vector<float> LongDc(float value) {
    return harkentest::MakeDC(kRate, value);
}

VnThreeBandToneDesc FullParamTone() {
    VnThreeBandToneDesc tone{};
    tone.structSize = sizeof(VnThreeBandToneDesc);
    tone.lowShelfGainDb = 0.0f;
    tone.lowShelfFreqHz = 200.0f;
    tone.peakGainDb = 0.0f;
    tone.peakFreqHz = 1000.0f;
    tone.peakQ = 1.0f;
    tone.highShelfGainDb = 0.0f;
    tone.highShelfFreqHz = 8000.0f;
    return tone;
}

TEST(Steal, RefusePolicyFullPoolReturnsNoVoice) {
    EngineSession session(StealConfig(2, 0));
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::vector<float> dc = LongDc(0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnVoiceHandle a = 0;
    VnVoiceHandle b = 0;
    VnPlayDesc play = PlayWith(harkentest::ObjectId(1), 128);
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &a));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &b));

    VnVoiceHandle overflow = 0;
    EXPECT_EQ(VN_NO_VOICE, harkentest::PlayRaw(&play, &overflow));
    EXPECT_EQ(1u, session.Engine()->voices.NoVoiceCount());
    EXPECT_EQ(0u, session.Engine()->ledger.PendingStartCount());
}

TEST(Steal, QuietestPicksLowestAudibilityWithSlotTieBreak) {
    NotificationLog log;
    VnConfig config = StealConfig(2, 1); // steal-quietest
    config.onNotification = &RecordNotification;
    config.notificationUserData = &log;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::vector<float> dc = LongDc(0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    // A loud voice (0 dB) and a quiet one (-40 dB), same priority.
    VnVoiceHandle loud = 0;
    VnVoiceHandle quiet = 0;
    VnPlayDesc loudPlay = PlayWith(harkentest::ObjectId(1), 128, 0.0f);
    VnPlayDesc quietPlay = PlayWith(harkentest::ObjectId(1), 128, -40.0f);
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&loudPlay, &loud));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&quietPlay, &quiet));

    // A third play steals: the quiet voice is the computed victim.
    VnVoiceHandle incoming = 0;
    VnPlayDesc incomingPlay = PlayWith(harkentest::ObjectId(1), 128, 0.0f);
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&incomingPlay, &incoming));
    EXPECT_EQ(1u, Snap().voiceStolenCount);

    // Pump the declick block, then deliver: the quiet voice surfaces STOLEN.
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(1u, log.CountOfType(VN_NOTIFY_VOICE_STOLEN));
    EXPECT_EQ(quiet, log.notifications.front().voiceHandle);
    EXPECT_EQ(0u, log.CountOfType(VN_NOTIFY_VOICE_ENDED));

    // The loud voice was never a victim: it stays resolvable.
    EXPECT_NE(nullptr,
              engine->voices.Resolve(harkentest::RenderHandle(engine, loud)));
}

TEST(Steal, WiderSuccessorBackingFailurePreservesPlayingVictimAndRetry) {
    EngineSession session(StealConfig(1, 2));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const auto samples = LongDc(0.25f);
    const auto mono = MonoDesc(samples, kRate);
    auto stereo = mono;
    stereo.channels = 2;
    stereo.frames /= 2;
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &mono));
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(2), &stereo));
    auto play = PlayWith(harkentest::ObjectId(1), 128);
    VnVoiceHandle incumbent = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &incumbent));
    engine.nullBackend.Pump(1);
    const auto original = harkentest::RenderHandle(&engine, incumbent);
    ASSERT_NE(nullptr, engine.voices.Resolve(original));
    harken::detail::VoicePoolReleaseTestAccess::SetRingCommitPageLimit(engine.voices, 1, 0);
    play.soundId = harkentest::ObjectId(2);
    VnVoiceHandle successor = 0;
    EXPECT_EQ(VN_NO_VOICE, harkentest::PlayRaw(&play, &successor));
    EXPECT_EQ(0u, successor);
    EXPECT_EQ(0u, engine.ledger.PendingStartCount());
    EXPECT_EQ(0u, Snap().voiceStolenCount);
    engine.nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_NE(nullptr, engine.voices.Resolve(original));
    harken::detail::VoicePoolReleaseTestAccess::SetRingCommitPageLimit(engine.voices, 1, SIZE_MAX);
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &successor));
    engine.nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine.nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto* context = engine.voices.Resolve(harkentest::RenderHandle(&engine, successor));
    ASSERT_NE(nullptr, context);
    EXPECT_EQ(2u, context->pcm.channelCount);
    EXPECT_EQ(1u, Snap().voiceStolenCount);
}

TEST(Steal, OldestPicksLowestStartOrder) {
    NotificationLog log;
    VnConfig config = StealConfig(2, 2); // steal-oldest
    config.onNotification = &RecordNotification;
    config.notificationUserData = &log;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::vector<float> dc = LongDc(0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    // Same volume and priority: only start order distinguishes them.
    VnVoiceHandle older = 0;
    VnVoiceHandle newer = 0;
    VnPlayDesc play = PlayWith(harkentest::ObjectId(1), 128, 0.0f);
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &older));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &newer));

    VnVoiceHandle incoming = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &incoming));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(1u, log.CountOfType(VN_NOTIFY_VOICE_STOLEN));
    EXPECT_EQ(older, log.notifications.front().voiceHandle);
    EXPECT_NE(nullptr,
              engine->voices.Resolve(harkentest::RenderHandle(engine, newer)));
}

TEST(Steal, NeverStealsHigherPriorityAndEqualIsEligible) {
    const std::vector<float> dc = LongDc(0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);

    {
        // A lower-priority incoming cannot steal a higher-priority voice.
        EngineSession session(StealConfig(1, 1));
        ASSERT_EQ(VN_OK, session.InitResult());
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
        VnVoiceHandle high = 0;
        VnPlayDesc highPlay = PlayWith(harkentest::ObjectId(1), 200);
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&highPlay, &high));
        VnVoiceHandle incoming = 0;
        VnPlayDesc lowPlay = PlayWith(harkentest::ObjectId(1), 100);
        EXPECT_EQ(VN_NO_VOICE, harkentest::PlayRaw(&lowPlay, &incoming));
        EXPECT_EQ(0u, session.Engine()->ledger.PendingStartCount());
        EXPECT_EQ(1u, session.Engine()->voices.NoVoiceCount());
    }
    {
        // Equal priority is an eligible victim (priority <= incoming).
        EngineSession session(StealConfig(1, 2));
        ASSERT_EQ(VN_OK, session.InitResult());
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
        VnVoiceHandle a = 0;
        VnPlayDesc play = PlayWith(harkentest::ObjectId(1), 150);
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &a));
        VnVoiceHandle incoming = 0;
        EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &incoming));
        EXPECT_EQ(1u, session.Engine()->ledger.PendingStartCount());
    }
}

TEST(Steal, UserStopInFlightIsNotRelabeledOrSelectedForSteal) {
    NotificationLog log;
    VnConfig config = StealConfig(1, 2);
    config.onNotification = &RecordNotification;
    config.notificationUserData = &log;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::vector<float> dc = LongDc(0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnVoiceHandle stopping = 0;
    VnPlayDesc play = PlayWith(harkentest::ObjectId(1), 128);
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &stopping));
    ASSERT_EQ(VN_OK, Vn_Stop(stopping, 20));

    VnVoiceHandle incoming = 0;
    EXPECT_EQ(VN_NO_VOICE, harkentest::PlayRaw(&play, &incoming));
    EXPECT_EQ(0u, Snap().voiceStolenCount);
    EXPECT_EQ(0u, session.Engine()->ledger.PendingStartCount());
    session.Engine()->nullBackend.Pump(8);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(0u, Snap().voiceStolenCount);
    EXPECT_EQ(0u, log.CountOfType(VN_NOTIFY_VOICE_STOLEN));
}

TEST(Steal, ExactClaimBlocksUnrelatedFreshPlayAfterVictimRelease) {
    EngineSession session(StealConfig(1, 2));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::vector<float> dc = LongDc(0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    VnPlayDesc play = PlayWith(harkentest::ObjectId(1), 128);

    VnVoiceHandle victim = 0;
    VnVoiceHandle claimant = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &victim));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &claimant));
    engine->nullBackend.Pump(1); // victim releases; exact claim remains

    VnVoiceHandle unrelated = 0;
    EXPECT_EQ(VN_NO_VOICE, harkentest::PlayRaw(&play, &unrelated));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Live,
              engine->ledger.Classify(claimant).kind);
}

TEST(Steal, StaleLedgerEntryCannotDonateReusedRenderGeneration) {
    NotificationLog log;
    VnConfig config = StealConfig(1, 2);
    config.maxParkedVoices = 2;
    config.onNotification = &RecordNotification;
    config.notificationUserData = &log;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::vector<float> shortDc(kBlock, 0.5f);
    const std::vector<float> longDc = LongDc(0.5f);
    const VnPCMDesc shortDesc = MonoDesc(shortDc, kRate);
    const VnPCMDesc longDesc = MonoDesc(longDc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &shortDesc));
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(2), &longDesc));

    VnVoiceHandle ended = 0;
    VnPlayDesc shortPlay = PlayWith(harkentest::ObjectId(1), 128);
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&shortPlay, &ended));
    engine->nullBackend.Pump(2);

    VnVoiceHandle reused = 0;
    VnPlayDesc longPlay = PlayWith(harkentest::ObjectId(2), 128);
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&longPlay, &reused));
    engine->nullBackend.Pump(1);
    ASSERT_NE(0u, harkentest::RenderHandle(engine, reused));

    VnVoiceHandle claimant = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&longPlay, &claimant));
    EXPECT_EQ(1u, Snap().voiceStolenCount);
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Pending,
              engine->ledger.Classify(claimant).kind);

    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(1u, log.CountOfType(VN_NOTIFY_VOICE_STOLEN));
    EXPECT_EQ(0u, engine->ledger.ToRender(reused));
}

TEST(Steal, DeclickIsBoundedAndCompletesWithinOneBlock) {
    EngineSession session(StealConfig(1, 2));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::vector<float> dc = LongDc(0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnVoiceHandle a = 0;
    VnPlayDesc play = PlayWith(harkentest::ObjectId(1), 128, 0.0f);
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &a));
    Capture warm;
    harkentest::PumpAndCapture(engine, 2, warm); // A steady at full level

    // Steal: the incoming voice is pending; only the victim renders this block.
    VnVoiceHandle incoming = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &incoming));

    Capture declick;
    harkentest::PumpAndCapture(engine, 1, declick); // the declick block

    // The declick starts from the audible level (a real ramp, not silence)...
    ASSERT_GT(harkentest::Peak(declick.left), harkentest::kSilenceFloor);
    // ...its largest sample-to-sample step is small (documented bound 0.02,
    // well above the ~level/240 a one-block linear ramp of a 0.5 DC produces)...
    float maxStep = 0.0f;
    for (std::size_t i = 1; i < declick.left.size(); ++i) {
        maxStep = std::max(
            maxStep, std::fabs(declick.left[i] - declick.left[i - 1]));
    }
    EXPECT_LT(maxStep, 0.02f);
    // ...and it has fully faded out by the end of the block.
    EXPECT_LT(std::fabs(declick.left.back()), harkentest::kSilenceFloor);
}

TEST(Steal, DeferredStartLandsWithinTwoBlocks) {
    EngineSession session(StealConfig(1, 2));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::vector<float> dc = LongDc(0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnVoiceHandle a = 0;
    VnPlayDesc play = PlayWith(harkentest::ObjectId(1), 128);
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &a));
    engine->nullBackend.Pump(1);

    VnVoiceHandle incoming = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &incoming));

    // Immediately after the steal the incoming voice is pending: not in the
    // snapshot voice rows, but counted.
    VnSnapshot before = Snap();
    EXPECT_EQ(1u, before.pendingStartCount);
    EXPECT_LT(harkentest::VoiceRowForHandle(before, incoming), 0);

    // Victim declick block, then the deferred start issues, then it renders.
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);

    VnSnapshot after = Snap();
    EXPECT_EQ(0u, after.pendingStartCount);
    const int row = harkentest::VoiceRowForHandle(after, incoming);
    ASSERT_GE(row, 0);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_VOICE_STATE_ACTIVE),
              after.voices[row].state);
}

TEST(Steal, PendingStartCancelledByStop) {
    NotificationLog log;
    VnConfig config = StealConfig(1, 2);
    config.onNotification = &RecordNotification;
    config.notificationUserData = &log;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::vector<float> dc = LongDc(0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnVoiceHandle a = 0;
    VnPlayDesc play = PlayWith(harkentest::ObjectId(1), 128);
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &a));
    VnVoiceHandle incoming = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &incoming));
    ASSERT_EQ(1u, engine->ledger.PendingStartCount());

    // Stopping the pending voice cancels the deferred start: it emits the
    // ended completion (a logic-originated notification) and reclaims the entry.
    ASSERT_EQ(VN_OK, Vn_Stop(incoming, 0));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(0u, engine->ledger.PendingStartCount());
    EXPECT_GE(log.CountOfType(VN_NOTIFY_VOICE_ENDED), 1u);
    // The cancelled handle is now stale.
    EXPECT_EQ(VN_STALE_HANDLE, Vn_SetVoiceVolume(incoming, -3.0f, 0));
}

TEST(Steal, StagedVolumeIsAppliedAtDeferredStart) {
    EngineSession session(StealConfig(1, 2));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::vector<float> dc = LongDc(0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnVoiceHandle a = 0;
    VnPlayDesc play = PlayWith(harkentest::ObjectId(1), 128, 0.0f);
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &a));
    engine->nullBackend.Pump(1);
    VnVoiceHandle incoming = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &incoming));

    // Set volume on the pending voice: staged, applied when it starts.
    ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(incoming, -18.0f, 0));

    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);

    // The started voice carries the staged API-direct volume on top of its 0 dB
    // base.
    EXPECT_FLOAT_EQ(-18.0f,
                    engine->params.ResolvedValue(
                        harkentest::RenderHandle(engine, incoming),
                        harken::kTargetVoiceVolume));
}

TEST(Steal, StagedEffectInstalledAtStartAndReleasedOnCancel) {
    VnConfig config = StealConfig(1, 2);
    config.fxSlotsPerVoice = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::vector<float> dc = LongDc(0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnThreeBandToneDesc tone{};
    tone.structSize = sizeof(VnThreeBandToneDesc);
    tone.lowShelfGainDb = 6.0f;
    tone.lowShelfFreqHz = 200.0f;
    tone.peakGainDb = 0.0f;
    tone.peakFreqHz = 1000.0f;
    tone.peakQ = 1.0f;
    tone.highShelfGainDb = 0.0f;
    tone.highShelfFreqHz = 8000.0f;

    // Cancel path: staging an effect acquires a pool instance; cancelling the
    // pending start returns it.
    {
        VnVoiceHandle a = 0;
        VnPlayDesc play = PlayWith(harkentest::ObjectId(1), 128);
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &a));
        VnVoiceHandle incoming = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &incoming));
        ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(incoming, 0, VN_EFFECT_THREE_BAND_TONE,
                                           reinterpret_cast<const VnEffectDesc*>(
                                               &tone)));
        EXPECT_EQ(1u, engine->effectBank.ToneInUse());
        ASSERT_EQ(VN_OK, Vn_Stop(incoming, 0));
        EXPECT_EQ(0u, engine->effectBank.ToneInUse());
        // Let the victim's declick free the slot so the next drill starts clean.
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    // Install path: a staged effect installs on the render slot at start.
    {
        VnVoiceHandle a = 0;
        VnPlayDesc play = PlayWith(harkentest::ObjectId(1), 128);
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &a));
        engine->nullBackend.Pump(1);
        VnVoiceHandle incoming = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &incoming));
        ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(incoming, 0, VN_EFFECT_THREE_BAND_TONE,
                                           reinterpret_cast<const VnEffectDesc*>(
                                               &tone)));
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update()); // deferred start issues play + install
        engine->nullBackend.Pump(1);   // audio applies both
        harken::VoiceRenderContext* ctx =
            engine->voices.Resolve(harkentest::RenderHandle(engine, incoming));
        ASSERT_NE(nullptr, ctx);
        EXPECT_NE(nullptr, ctx->fxSlots[0].process);
    }
}

TEST(Steal, FreshFreeRecipeSeedFailureIsAtomicAndRetryable) {
    VnConfig config = StealConfig(1, 2);
    config.fxSlotsPerVoice = 1;
    config.gainEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    constexpr VnBindingId kBinding = harkentest::ObjectId(81);
    VnBindingDesc binding{};
    binding.structSize = sizeof(VnBindingDesc);
    binding.variableId = harkentest::ObjectId(82);
    binding.targetScopeKind = VN_TARGET_SCOPE_VOICE_EFFECT_PARAM;
    binding.targetId = harken::PackEffectParamLocator(
        VN_EFFECT_GAIN, 0, harken::kGainParamGainDb);
    binding.curve.structSize = sizeof(VnCurveDesc);
    binding.curve.pointCount = 2;
    binding.curve.points[0] = {0.0f, 0.0f};
    binding.curve.points[1] = {1.0f, 6.0f};
    binding.curve.handles[0] = harkentest::EndpointHandles(
        binding.curve.points[0], binding.curve.points[1]);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(kBinding, &binding));

    const harkentest::RecipePackArtifact artifact =
        harkentest::BuildGainRecipePack(
            "steal_fresh_free_recipe_seed_atomic", {kBinding});
    ASSERT_TRUE(artifact.built);
    VnPackDesc packDesc{};
    packDesc.structSize = sizeof(VnPackDesc);
    packDesc.utf8Path = artifact.path.c_str();
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, Vn_LoadPack(&packDesc, &pack));
    const harken::PCMView view = engine->registry.Lookup(harkentest::ObjectId(9101));
    ASSERT_TRUE(view.Valid());
    ASSERT_NE(nullptr, view.useCount);
    ASSERT_EQ(0u, view.useCount->load(std::memory_order_acquire));

    const std::uint32_t commandRoom = engine->commands.FreeSlots();
    VnPlayDesc play = PlayWith(harkentest::ObjectId(9101), 128);
    play.loopCount = 0;
    engine->effectParamTargets.SetPrepareSeedFailureAfterForTests(0);
    VnVoiceHandle refused = 0;
    EXPECT_EQ(VN_DEVICE_ERROR, harkentest::PlayRaw(&play, &refused));
    engine->effectParamTargets.SetPrepareSeedFailureAfterForTests(-1);

    EXPECT_EQ(0u, refused);
    EXPECT_EQ(0u, engine->voices.ActiveCount());
    EXPECT_EQ(0u, engine->ledger.PendingStartCount());
    EXPECT_EQ(0u, engine->ledger.ParkedCount());
    EXPECT_EQ(commandRoom, engine->commands.FreeSlots());
    EXPECT_EQ(0u, engine->effectBank.InUseOf(VN_EFFECT_GAIN));
    EXPECT_EQ(
        0u,
        engine->effectParamTargets.ReservedRecordCountForTests());
    EXPECT_EQ(0u, view.useCount->load(std::memory_order_acquire));
    std::uint32_t failedBindingInstances = 0;
    engine->bindings.ForEachVoiceEffectInstanceOf(
        kBinding, [&](VnVoiceHandle, bool) {
            ++failedBindingInstances;
        });
    EXPECT_EQ(0u, failedBindingInstances);

    VnVoiceHandle started = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &started));
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Live,
              engine->ledger.Classify(started).kind);
    EXPECT_EQ(1u, engine->voices.ActiveCount());
    EXPECT_EQ(1u, engine->effectBank.InUseOf(VN_EFFECT_GAIN));
    EXPECT_EQ(
        0u,
        engine->effectParamTargets.ReservedRecordCountForTests());
    EXPECT_EQ(1u, view.useCount->load(std::memory_order_acquire));
    std::uint32_t activeBindingInstances = 0;
    engine->bindings.ForEachVoiceEffectInstanceOf(
        kBinding, [&](VnVoiceHandle render, bool held) {
            ++activeBindingInstances;
            EXPECT_EQ(harkentest::RenderHandle(engine, started), render);
            EXPECT_FALSE(held);
        });
    EXPECT_EQ(1u, activeBindingInstances);
}

TEST(Steal, FreshRecipeStealSeedFailureLeavesIncumbentUntouched) {
    VnConfig config = StealConfig(1, 2);
    config.fxSlotsPerVoice = 1;
    config.gainEffectPoolCapacity = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const harkentest::RecipePackArtifact artifact =
        harkentest::BuildGainRecipePack(
            "steal_fresh_recipe_seed_atomic");
    ASSERT_TRUE(artifact.built);
    VnPackDesc packDesc{};
    packDesc.structSize = sizeof(VnPackDesc);
    packDesc.utf8Path = artifact.path.c_str();
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, Vn_LoadPack(&packDesc, &pack));

    VnPlayDesc play = PlayWith(harkentest::ObjectId(9101), 128);
    play.loopCount = 0;
    VnVoiceHandle incumbent = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &incumbent));
    ASSERT_EQ(1u, engine->effectBank.InUseOf(VN_EFFECT_GAIN));

    engine->effectParamTargets.SetPrepareSeedFailureAfterForTests(0);
    VnVoiceHandle refused = 0;
    EXPECT_EQ(VN_DEVICE_ERROR, harkentest::PlayRaw(&play, &refused));
    engine->effectParamTargets.SetPrepareSeedFailureAfterForTests(-1);

    EXPECT_EQ(0u, refused);
    EXPECT_EQ(0u, engine->ledger.PendingStartCount());
    EXPECT_EQ(0u, Snap().voiceStolenCount);
    EXPECT_NE(nullptr,
              engine->voices.Resolve(
                  harkentest::RenderHandle(engine, incumbent)));
    EXPECT_EQ(1u, engine->effectBank.InUseOf(VN_EFFECT_GAIN));
}

TEST(Steal, ExactClaimSeedsPastFullyInstalledBaseBudget) {
    VnConfig config = StealConfig(1, 2);
    config.maxBuses = 1;
    config.fxSlotsPerVoice = 1;
    config.fxSlotsPerBus = 1;
    config.toneEffectPoolCapacity = 3;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::vector<float> dc = LongDc(0.5f);
    const VnPCMDesc pcm = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    const VnThreeBandToneDesc tone = FullParamTone();
    ASSERT_EQ(
        VN_OK,
        Vn_SetBusEffect(
            VN_BUS_MASTER, 0, VN_EFFECT_THREE_BAND_TONE,
            reinterpret_cast<const VnEffectDesc*>(&tone)));

    const harkentest::RecipePackArtifact artifact =
        harkentest::BuildRecipePack(
            "steal_exact_claim_full_seed_budget", "threeBandTone",
            "0.0, 200.0, 0.0, 1000.0, 1.0, 0.0, 8000.0");
    ASSERT_TRUE(artifact.built);
    VnPackDesc packDesc{};
    packDesc.structSize = sizeof(VnPackDesc);
    packDesc.utf8Path = artifact.path.c_str();
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, Vn_LoadPack(&packDesc, &pack));

    VnVoiceHandle victim = 0;
    VnPlayDesc victimPlay = PlayWith(harkentest::ObjectId(1), 128);
    victimPlay.loopCount = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&victimPlay, &victim));
    ASSERT_EQ(
        VN_OK,
        Vn_SetVoiceEffect(
            victim, 0, VN_EFFECT_THREE_BAND_TONE,
            reinterpret_cast<const VnEffectDesc*>(&tone)));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(2u, engine->effectBank.ToneInUse());

    VnVoiceHandle incoming = 0;
    VnPlayDesc incomingPlay = PlayWith(harkentest::ObjectId(9101), 128);
    incomingPlay.loopCount = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&incomingPlay, &incoming));
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Pending,
              engine->ledger.Classify(incoming).kind);
    VnVoiceHandle claimedVictim = 0;
    ASSERT_TRUE(
        engine->voiceManager.ClaimFor(incoming, &claimedVictim));
    EXPECT_EQ(harkentest::RenderHandle(engine, victim), claimedVictim);
    EXPECT_EQ(harken::kToneParamCount,
              engine->effectParamTargets.ReservedRecordCountForTests());
    EXPECT_EQ(3u, engine->effectBank.ToneInUse());

    bool started = false;
    for (int i = 0; i < 8 && !started; ++i) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
        started =
            engine->ledger.Classify(incoming).kind ==
            harken::VoiceLedger::HandleKind::Live;
    }
    ASSERT_TRUE(started);
    EXPECT_EQ(
        0u,
        engine->effectParamTargets.ReservedRecordCountForTests());
    VnVoiceHandle leakedClaim = 0;
    EXPECT_FALSE(
        engine->voiceManager.ClaimFor(incoming, &leakedClaim));
    EXPECT_EQ(1u, Snap().voiceStolenCount);
    EXPECT_EQ(2u, engine->effectBank.ToneInUse());
}

TEST(Steal, FreshFreeThirdGenerationUsesLogicalRecordBudget) {
    VnConfig config = StealConfig(1, 2);
    config.maxParkedVoices = 2;
    config.maxBuses = 1;
    config.fxSlotsPerVoice = 1;
    config.fxSlotsPerBus = 1;
    config.toneEffectPoolCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::vector<float> shortDc(kBlock, 0.5f);
    const VnPCMDesc shortPcm = MonoDesc(shortDc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &shortPcm));
    const VnThreeBandToneDesc tone = FullParamTone();
    ASSERT_EQ(
        VN_OK,
        Vn_SetBusEffect(
            VN_BUS_MASTER, 0, VN_EFFECT_THREE_BAND_TONE,
            reinterpret_cast<const VnEffectDesc*>(&tone)));
    EXPECT_EQ(harken::kToneParamCount,
              engine->effectParamTargets.LiveRecordCount());

    const harkentest::RecipePackArtifact artifact =
        harkentest::BuildRecipePack(
            "steal_fresh_free_third_generation", "threeBandTone",
            "0.0, 200.0, 0.0, 1000.0, 1.0, 0.0, 8000.0",
            {}, kBlock);
    ASSERT_TRUE(artifact.built);
    VnPackDesc packDesc{};
    packDesc.structSize = sizeof(VnPackDesc);
    packDesc.utf8Path = artifact.path.c_str();
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, Vn_LoadPack(&packDesc, &pack));

    VnPlayDesc shortPlay = PlayWith(harkentest::ObjectId(1), 128);
    shortPlay.loopCount = 1;
    VnVoiceHandle generation1 = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&shortPlay, &generation1));
    ASSERT_EQ(
        VN_OK,
        Vn_SetVoiceEffect(
            generation1, 0, VN_EFFECT_THREE_BAND_TONE,
            reinterpret_cast<const VnEffectDesc*>(&tone)));
    const VnVoiceHandle render1 =
        harkentest::RenderHandle(engine, generation1);
    EXPECT_EQ(2u * harken::kToneParamCount,
              engine->effectParamTargets.LiveRecordCount());
    engine->nullBackend.Pump(2);
    ASSERT_EQ(nullptr, engine->voices.Resolve(render1));
    EXPECT_EQ(2u * harken::kToneParamCount,
              engine->effectParamTargets.LiveRecordCount());

    VnVoiceHandle generation2 = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&shortPlay, &generation2));
    ASSERT_EQ(
        VN_OK,
        Vn_SetVoiceEffect(
            generation2, 0, VN_EFFECT_THREE_BAND_TONE,
            reinterpret_cast<const VnEffectDesc*>(&tone)));
    const VnVoiceHandle render2 =
        harkentest::RenderHandle(engine, generation2);
    EXPECT_EQ(3u * harken::kToneParamCount,
              engine->effectParamTargets.LiveRecordCount());
    engine->nullBackend.Pump(2);
    ASSERT_EQ(nullptr, engine->voices.Resolve(render2));
    EXPECT_EQ(3u * harken::kToneParamCount,
              engine->effectParamTargets.LiveRecordCount());

    VnPlayDesc recipePlay = PlayWith(harkentest::ObjectId(9101), 128);
    recipePlay.loopCount = 1;
    VnVoiceHandle generation3 = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&recipePlay, &generation3));
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Live,
              engine->ledger.Classify(generation1).kind);
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Live,
              engine->ledger.Classify(generation2).kind);
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Live,
              engine->ledger.Classify(generation3).kind);
    EXPECT_NE(generation1, generation2);
    EXPECT_NE(generation2, generation3);
    EXPECT_EQ(4u * harken::kToneParamCount,
              engine->effectParamTargets.LiveRecordCount());
    EXPECT_EQ(
        0u,
        engine->effectParamTargets.ReservedRecordCountForTests());
    EXPECT_EQ(0u, Snap().voiceStolenCount);

    const VnVoiceHandle render3 =
        harkentest::RenderHandle(engine, generation3);
    engine->nullBackend.Pump(2);
    ASSERT_EQ(nullptr, engine->voices.Resolve(render3));
    engine->effectParamTargets.SetPrepareSeedFailureAfterForTests(0);
    VnVoiceHandle ledgerFull = 0;
    EXPECT_EQ(VN_NO_VOICE, harkentest::PlayRaw(&recipePlay, &ledgerFull));
    EXPECT_EQ(0u, ledgerFull);
    EXPECT_EQ(
        0u,
        engine->effectParamTargets.ReservedRecordCountForTests());

    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(harken::kToneParamCount,
              engine->effectParamTargets.LiveRecordCount());
    VnVoiceHandle seedFailure = 0;
    EXPECT_EQ(VN_DEVICE_ERROR, harkentest::PlayRaw(&recipePlay, &seedFailure));
    engine->effectParamTargets.SetPrepareSeedFailureAfterForTests(-1);
    EXPECT_EQ(0u, seedFailure);
    EXPECT_EQ(0u, Snap().voiceStolenCount);
}

TEST(Steal, ExactClaimThirdGenerationUsesLogicalRecordBudget) {
    VnConfig config = StealConfig(1, 2);
    config.maxParkedVoices = 2;
    config.maxBuses = 1;
    config.fxSlotsPerVoice = 1;
    config.fxSlotsPerBus = 1;
    config.toneEffectPoolCapacity = 3;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::vector<float> shortDc(kBlock, 0.5f);
    const std::vector<float> longDc = LongDc(0.5f);
    const VnPCMDesc shortPcm = MonoDesc(shortDc, kRate);
    const VnPCMDesc longPcm = MonoDesc(longDc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &shortPcm));
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(2), &longPcm));
    const VnThreeBandToneDesc tone = FullParamTone();
    ASSERT_EQ(
        VN_OK,
        Vn_SetBusEffect(
            VN_BUS_MASTER, 0, VN_EFFECT_THREE_BAND_TONE,
            reinterpret_cast<const VnEffectDesc*>(&tone)));

    const harkentest::RecipePackArtifact artifact =
        harkentest::BuildRecipePack(
            "steal_exact_claim_third_generation", "threeBandTone",
            "0.0, 200.0, 0.0, 1000.0, 1.0, 0.0, 8000.0");
    ASSERT_TRUE(artifact.built);
    VnPackDesc packDesc{};
    packDesc.structSize = sizeof(VnPackDesc);
    packDesc.utf8Path = artifact.path.c_str();
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, Vn_LoadPack(&packDesc, &pack));

    VnPlayDesc firstPlay = PlayWith(harkentest::ObjectId(1), 128);
    firstPlay.loopCount = 1;
    VnVoiceHandle generation1 = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&firstPlay, &generation1));
    ASSERT_EQ(
        VN_OK,
        Vn_SetVoiceEffect(
            generation1, 0, VN_EFFECT_THREE_BAND_TONE,
            reinterpret_cast<const VnEffectDesc*>(&tone)));
    const VnVoiceHandle render1 =
        harkentest::RenderHandle(engine, generation1);
    engine->nullBackend.Pump(2);
    ASSERT_EQ(nullptr, engine->voices.Resolve(render1));

    VnPlayDesc secondPlay = PlayWith(harkentest::ObjectId(2), 128);
    secondPlay.loopCount = 0;
    VnVoiceHandle generation2 = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&secondPlay, &generation2));
    ASSERT_EQ(
        VN_OK,
        Vn_SetVoiceEffect(
            generation2, 0, VN_EFFECT_THREE_BAND_TONE,
            reinterpret_cast<const VnEffectDesc*>(&tone)));
    engine->nullBackend.Pump(1);
    EXPECT_EQ(3u * harken::kToneParamCount,
              engine->effectParamTargets.LiveRecordCount());

    VnPlayDesc incomingPlay = PlayWith(harkentest::ObjectId(9101), 128);
    incomingPlay.loopCount = 0;
    VnVoiceHandle generation3 = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&incomingPlay, &generation3));
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Pending,
              engine->ledger.Classify(generation3).kind);
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Live,
              engine->ledger.Classify(generation1).kind);
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Live,
              engine->ledger.Classify(generation2).kind);
    EXPECT_NE(generation1, generation2);
    EXPECT_NE(generation2, generation3);
    VnVoiceHandle claimedVictim = 0;
    ASSERT_TRUE(
        engine->voiceManager.ClaimFor(generation3, &claimedVictim));
    EXPECT_EQ(harkentest::RenderHandle(engine, generation2),
              claimedVictim);
    EXPECT_EQ(harken::kToneParamCount,
              engine->effectParamTargets.ReservedRecordCountForTests());
    EXPECT_EQ(1u, Snap().voiceStolenCount);

    bool started = false;
    for (int i = 0; i < 8 && !started; ++i) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
        started =
            engine->ledger.Classify(generation3).kind ==
            harken::VoiceLedger::HandleKind::Live;
    }
    ASSERT_TRUE(started);
    EXPECT_EQ(
        0u,
        engine->effectParamTargets.ReservedRecordCountForTests());
    VnVoiceHandle leakedClaim = 0;
    EXPECT_FALSE(
        engine->voiceManager.ClaimFor(generation3, &leakedClaim));
    EXPECT_EQ(1u, Snap().voiceStolenCount);
    EXPECT_EQ(2u * harken::kToneParamCount,
              engine->effectParamTargets.LiveRecordCount());
}


TEST(Steal, OldPlayDescVintageDefaultsPriorityTo128) {
    EngineSession session(StealConfig(1, 1)); // steal-quietest
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::vector<float> dc = LongDc(0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    // Play with a struct that predates the priority field: it must resolve to
    // priority 128, not 0. A priority-100 incoming then finds no eligible
    // victim (128 > 100), proving the default.
    VnPlayDesc oldDesc = MakePlayDesc(harkentest::ObjectId(1));
    oldDesc.structSize =
        static_cast<std::uint32_t>(offsetof(VnPlayDesc, emitterHandle));
    VnVoiceHandle a = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&oldDesc, &a));

    VnVoiceHandle incoming = 0;
    VnPlayDesc lowPlay = PlayWith(harkentest::ObjectId(1), 100);
    EXPECT_EQ(VN_NO_VOICE, harkentest::PlayRaw(&lowPlay, &incoming));
}

TEST(Steal, DeclickCompletesWithinOneBlockOnSmallBlocks) {
    // A sub-millisecond block: 32 frames at 48 kHz is 0.667 ms. A declick
    // rounded through milliseconds (a 1 ms floor rounds to 48 frames) would
    // run longer than one 32-frame block; the frames-direct declick must
    // complete inside this single block, so the victim is silent by block end.
    constexpr std::uint32_t kSmallBlock = 32;
    VnConfig config = MakeNullConfig(1, kSmallBlock, kRate);
    config.voiceStealPolicy = 2; // steal-oldest
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::vector<float> dc = LongDc(0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnVoiceHandle a = 0;
    VnPlayDesc play = PlayWith(harkentest::ObjectId(1), 128, 0.0f);
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &a));
    Capture warm;
    harkentest::PumpAndCapture(engine, 2, warm);

    VnVoiceHandle incoming = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &incoming));

    Capture declick;
    harkentest::PumpAndCapture(engine, 1, declick); // the single declick block
    ASSERT_EQ(kSmallBlock, declick.left.size());
    // Audible at the block's start, fully faded out by its end: the fade did
    // not spill past this one short block (the bug this guards against).
    ASSERT_GT(harkentest::Peak(declick.left), harkentest::kSilenceFloor);
    EXPECT_LT(std::fabs(declick.left.back()), harkentest::kSilenceFloor);
}

TEST(Steal, FullCommandQueueRefusesStealWithoutSideEffects) {
    // The victim-stop command is admitted before the steal commits any side
    // effect, so a full command queue refuses cleanly: no victim marked, no
    // pending entry, no counter movement — nothing half-commits.
    VnConfig config = StealConfig(1, 2); // steal-oldest
    config.commandQueueDepth = 1;        // one-deep: a single play fills it
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::vector<float> dc = LongDc(0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnVoiceHandle a = 0;
    VnPlayDesc play = PlayWith(harkentest::ObjectId(1), 128, 0.0f);
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &a));
    // The play command sits undrained in the one-deep queue (no pump), so the
    // queue is full while the pool is also full — the steal preconditions.
    VnVoiceHandle incoming = 0;
    EXPECT_EQ(VN_BACKPRESSURE, harkentest::PlayRaw(&play, &incoming));
    EXPECT_EQ(0u, engine->ledger.PendingStartCount());
    EXPECT_EQ(0u, Snap().voiceStolenCount);
    EXPECT_NE(nullptr,
              engine->voices.Resolve(harkentest::RenderHandle(engine, a)));
}

TEST(Steal, DroppedStolenNotificationStillCountsExactly) {
    // voiceStolenCount is committed at the steal, not at notification
    // delivery, so a dropped VN_NOTIFY_VOICE_STOLEN never desynchronizes it.
    // Two voices are stolen in one block; their STOLEN terminals collide in
    // a one-deep notification queue and one is dropped, yet the count stays
    // exact.
    NotificationLog log;
    VnConfig config = StealConfig(2, 2); // steal-oldest
    config.notificationQueueDepth = 1;
    config.onNotification = &RecordNotification;
    config.notificationUserData = &log;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::vector<float> dc = LongDc(0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnVoiceHandle a = 0;
    VnVoiceHandle b = 0;
    VnPlayDesc play = PlayWith(harkentest::ObjectId(1), 128, 0.0f);
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &a));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &b));
    engine->nullBackend.Pump(2); // both rendering

    // Two steals with the pool full: the first steals the oldest (a); the
    // second skips the now-stolen a and steals b. Both incoming are pending.
    VnVoiceHandle c = 0;
    VnVoiceHandle d = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &c));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &d));
    EXPECT_EQ(2u, Snap().voiceStolenCount);

    // One declick block completes both victims' fades; both STOLEN terminals
    // are pushed into the one-deep queue, so one overwrites the other.
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());

    const VnSnapshot snapshot = Snap();
    EXPECT_EQ(2u, snapshot.voiceStolenCount);   // committed count is exact
    EXPECT_GE(snapshot.notificationDropCount, 1u);      // a terminal was dropped
    EXPECT_EQ(1u, log.CountOfType(VN_NOTIFY_VOICE_STOLEN)); // one survived
}

TEST(Steal, PendingStartStagesEveryPerVoiceOp) {
    // Every per-voice API staged on a pending voice lands in the ledger's
    // staging record and is replayed at the deferred start.
    VnConfig config = StealConfig(1, 2); // steal-oldest
    config.fxSlotsPerVoice = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::vector<float> dc = LongDc(0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    const VnAttenuationDesc atten =
        harkentest::MakeAttenuationDesc(1.0f, 100.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(55), &atten));

    VnVoiceHandle a = 0;
    VnPlayDesc play = PlayWith(harkentest::ObjectId(1), 128, 0.0f);
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &a));
    engine->nullBackend.Pump(1);

    // The incoming is a 3D voice so position/orientation/attenuation are
    // meaningful; it steals the incumbent and becomes pending.
    VnVoiceHandle incoming = 0;
    VnPlayDesc play3d = harkentest::MakePlayDesc3D(harkentest::ObjectId(1), 0.0f, 0.0f, 0.0f, 0.0f);
    play3d.priority = 128;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play3d, &incoming));
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Pending,
              engine->ledger.Classify(incoming).kind);
    const std::uint32_t slot = engine->ledger.Classify(incoming).ledgerSlot;

    ASSERT_EQ(VN_OK, Vn_SetVoicePitch(incoming, 2.0f));
    ASSERT_EQ(VN_OK, Vn_SetVoicePosition(incoming, VnVec3{4.0f, 5.0f, 6.0f}));
    ASSERT_EQ(VN_OK, Vn_SetVoiceDistanceScale(incoming, 2.5f));
    ASSERT_EQ(VN_OK, Vn_SetVoiceSpatializationMix(incoming, 0.25f));
    ASSERT_EQ(VN_OK,
              Vn_SetVoiceOrientation(incoming, VnVec3{0.0f, 0.0f, 1.0f}));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(incoming, harkentest::ObjectId(55)));

    VnThreeBandToneDesc tone{};
    tone.structSize = sizeof(VnThreeBandToneDesc);
    tone.lowShelfGainDb = 6.0f;
    tone.lowShelfFreqHz = 200.0f;
    tone.peakGainDb = 0.0f;
    tone.peakFreqHz = 1000.0f;
    tone.peakQ = 1.0f;
    tone.highShelfGainDb = 0.0f;
    tone.highShelfFreqHz = 8000.0f;
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(incoming, 0, VN_EFFECT_THREE_BAND_TONE,
                                       reinterpret_cast<const VnEffectDesc*>(
                                           &tone)));
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffectParam(incoming, 0,
                                            harken::kToneParamPeakGainDb, 3.0f));
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffectBypass(incoming, 0, 1));

    // Each staging branch fired.
    const harken::PendingStartRecord& rec = engine->pendingStarts[slot];
    EXPECT_EQ(harken::ResampleProfile::LegacyCompatibility,
              rec.resampleProfile);
    EXPECT_TRUE(rec.hasPitch);
    EXPECT_TRUE(rec.hasPosition);
    EXPECT_FLOAT_EQ(4.0f, rec.setPosX);
    EXPECT_TRUE(rec.hasDistanceScale);
    EXPECT_FLOAT_EQ(2.5f, rec.distanceScale);
    EXPECT_TRUE(rec.hasSpatializationMix);
    EXPECT_FLOAT_EQ(0.25f, rec.spatializationMix);
    EXPECT_TRUE(rec.hasOrientation);
    EXPECT_TRUE(rec.hasAttenuation);
    EXPECT_EQ(harkentest::ObjectId(55u), rec.attenuationId);
    EXPECT_EQ(0u, rec.attenuationGeneration);
    EXPECT_TRUE(rec.effects[0].set);
    EXPECT_TRUE(rec.effectSeedsPrepared);
    EXPECT_TRUE(rec.preparedSeedTokens[0].IsPrepared());
    EXPECT_TRUE(rec.effectBypass[0]); // per-slot bypass, independent of the effect

    // The deferred Play resets to its base ratio, then the staged pitch setter
    // replays as a public snap (rampMs=0).
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    harken::VoiceRenderContext* ctx =
        engine->voices.Resolve(harkentest::RenderHandle(engine, incoming));
    ASSERT_NE(nullptr, ctx);
    EXPECT_EQ(harken::ResampleProfile::LegacyCompatibility,
              ctx->resampler.Profile());
    EXPECT_DOUBLE_EQ(2.0, ctx->resampler.TargetRatio());
    EXPECT_EQ(0u, ctx->resampler.RampFramesRemaining());
    EXPECT_DOUBLE_EQ(2.0, ctx->resampler.CurrentRatio());
    harken::Spatializer::SpatialVoiceState spatial;
    ASSERT_TRUE(engine->spatial.CaptureVoice(
        harkentest::RenderHandle(engine, incoming), &spatial));
    EXPECT_FLOAT_EQ(2.5f, spatial.distanceScale);
    EXPECT_FLOAT_EQ(0.25f, spatial.spatializationMix);
    EXPECT_FLOAT_EQ(0.25f, ctx->spatializationMix);
    EXPECT_FLOAT_EQ(4.0f, ctx->positionX);
    EXPECT_FLOAT_EQ(5.0f, ctx->positionY);
    EXPECT_FLOAT_EQ(6.0f, ctx->positionZ);
    EXPECT_NE(nullptr, ctx->fxSlots[0].process);
}

TEST(Steal, PendingSetVoiceEffectRefusesOnPoolExhaustion) {
    // Effect staging keeps the live-voice contract on a pending voice: an
    // exhausted instance pool refuses VN_NO_EFFECT.
    VnConfig config = StealConfig(2, 2); // steal-oldest
    config.fxSlotsPerVoice = 1;
    config.toneEffectPoolCapacity = 1; // a single tone instance exists
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::vector<float> dc = LongDc(0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnThreeBandToneDesc tone{};
    tone.structSize = sizeof(VnThreeBandToneDesc);
    tone.lowShelfGainDb = 6.0f;
    tone.lowShelfFreqHz = 200.0f;
    tone.peakGainDb = 0.0f;
    tone.peakFreqHz = 1000.0f;
    tone.peakQ = 1.0f;
    tone.highShelfGainDb = 0.0f;
    tone.highShelfFreqHz = 8000.0f;

    VnVoiceHandle a = 0;
    VnVoiceHandle b = 0;
    VnPlayDesc play = PlayWith(harkentest::ObjectId(1), 128, 0.0f);
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &a));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &b));
    engine->nullBackend.Pump(1);

    // Consume the only tone instance on a live voice.
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(a, 0, VN_EFFECT_THREE_BAND_TONE,
                                       reinterpret_cast<const VnEffectDesc*>(
                                           &tone)));
    ASSERT_EQ(1u, engine->effectBank.ToneInUse());

    // Steal to create a pending voice; without a pump the victim's slot has
    // not freed, so its tone instance is still in use and the pool is empty.
    VnVoiceHandle incoming = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &incoming));
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Pending,
              engine->ledger.Classify(incoming).kind);
    EXPECT_EQ(VN_NO_EFFECT,
              Vn_SetVoiceEffect(incoming, 0, VN_EFFECT_THREE_BAND_TONE,
                                reinterpret_cast<const VnEffectDesc*>(&tone)));
}

TEST(Steal, PendingBypassPersistsAcrossClearAndInstall) {
    // A pending voice's per-slot bypass is independent of the staged effect: it
    // survives a staged clear and applies at the deferred start — bypass before
    // install, so the deferred voice's FIRST rendered block is dry with no wet
    // leak. Pinned by first-block audio: render the deferred voice's first block
    // with the bypass-then-clear-then-set effect staged, and — identical timing —
    // with no effect; a bypassed effect contributes nothing, so the blocks are
    // sample-identical. (Post-start bypass state settles by the block's end
    // whichever order the commands ran, so only the audio catches a leak.)
    VnThreeBandToneDesc tone{};
    tone.structSize = sizeof(VnThreeBandToneDesc);
    tone.lowShelfGainDb = 6.0f; // a boost that would be audible if it leaked
    tone.lowShelfFreqHz = 200.0f;
    tone.peakGainDb = 0.0f;
    tone.peakFreqHz = 1000.0f;
    tone.peakQ = 1.0f;
    tone.highShelfGainDb = 0.0f;
    tone.highShelfFreqHz = 8000.0f;
    const std::vector<float> dc = LongDc(0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);

    harkentest::Capture withBypassedEffect;
    {
        VnConfig config = StealConfig(1, 2); // steal-oldest
        config.fxSlotsPerVoice = 1;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
        VnVoiceHandle a = 0;
        VnPlayDesc play = PlayWith(harkentest::ObjectId(1), 128);
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &a));
        engine->nullBackend.Pump(1);
        VnVoiceHandle incoming = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &incoming));
        ASSERT_EQ(harken::VoiceLedger::HandleKind::Pending,
                  engine->ledger.Classify(incoming).kind);
        const std::uint32_t slot = engine->ledger.Classify(incoming).ledgerSlot;
        // Bypass the empty slot, clear it, then install the effect — all pending.
        ASSERT_EQ(VN_OK, Vn_SetVoiceEffectBypass(incoming, 0, 1));
        ASSERT_EQ(VN_OK, Vn_ClearVoiceEffect(incoming, 0));
        ASSERT_EQ(VN_OK,
                  Vn_SetVoiceEffect(incoming, 0, VN_EFFECT_THREE_BAND_TONE,
                                    reinterpret_cast<const VnEffectDesc*>(&tone)));
        EXPECT_TRUE(engine->pendingStarts[slot].effectBypass[0]); // survived clear
        engine->nullBackend.Pump(1);   // victim declick frees the slot
        ASSERT_EQ(VN_OK, Vn_Update()); // deferred start (bypass before install)
        harkentest::PumpAndCapture(engine, 1, withBypassedEffect); // first block
        harken::VoiceRenderContext* ctx =
            engine->voices.Resolve(harkentest::RenderHandle(engine, incoming));
        ASSERT_NE(nullptr, ctx);
        ASSERT_NE(nullptr, ctx->fxSlots[0].process); // effect installed
    }
    harkentest::Capture noEffect;
    {
        VnConfig config = StealConfig(1, 2);
        config.fxSlotsPerVoice = 1;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
        VnVoiceHandle a = 0;
        VnPlayDesc play = PlayWith(harkentest::ObjectId(1), 128);
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &a));
        engine->nullBackend.Pump(1);
        VnVoiceHandle incoming = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &incoming));
        engine->nullBackend.Pump(1);   // victim declick frees the slot
        ASSERT_EQ(VN_OK, Vn_Update()); // deferred start, no effect
        harkentest::PumpAndCapture(engine, 1, noEffect); // first block
    }
    HARKENTEST_ASSERT_NON_SILENT(noEffect.left);
    ASSERT_EQ(noEffect.left.size(), withBypassedEffect.left.size());
    for (std::size_t i = 0; i < noEffect.left.size(); ++i) {
        EXPECT_FLOAT_EQ(noEffect.left[i], withBypassedEffect.left[i])
            << "left frame " << i << " (wet effect leaked on the first block)";
        EXPECT_FLOAT_EQ(noEffect.right[i], withBypassedEffect.right[i])
            << "right frame " << i;
    }
}

} // namespace
