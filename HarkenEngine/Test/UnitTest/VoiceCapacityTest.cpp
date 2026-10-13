// Render, Virtual, and Logical Voice capacity contracts. These tests keep the
// legacy public config fields while pinning the resolved internal terminology,
// allocation boundaries, refusal accounting, and public identity behavior.

#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "Common/TestUtil.h"
#include "harken/core/EngineConfig.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

constexpr std::uint32_t kBlockFrames = 256;
constexpr std::uint32_t kSampleRate = 48000;
constexpr VnSoundId kSoundId = harkentest::ObjectId(81001);
constexpr VnAttenuationId kFarProfileId = harkentest::ObjectId(81002);

TEST(VoiceCapacity, ResolvesDefaultExplicitAndLegacyVintageRelationships) {
    harken::EngineConfig resolved;

    VnConfig defaults = harkentest::MakeNullConfig();
    ASSERT_EQ(VN_OK, harken::ResolveConfig(&defaults, &resolved));
    EXPECT_EQ(256u, resolved.voiceCapacity.Render());
    EXPECT_EQ(256u, resolved.voiceCapacity.Virtual());
    EXPECT_EQ(512u, resolved.voiceCapacity.Logical());
    EXPECT_EQ(2u << 20, resolved.arenaBytes);

    VnConfig explicitConfig =
        harkentest::MakeNullConfig(/*maxVoices=*/3, kBlockFrames, kSampleRate);
    explicitConfig.maxParkedVoices = 5;
    ASSERT_EQ(VN_OK, harken::ResolveConfig(&explicitConfig, &resolved));
    EXPECT_EQ(3u, resolved.voiceCapacity.Render());
    EXPECT_EQ(5u, resolved.voiceCapacity.Virtual());
    EXPECT_EQ(8u, resolved.voiceCapacity.Logical());

    VnConfig legacy =
        harkentest::MakeNullConfig(/*maxVoices=*/7, kBlockFrames, kSampleRate);
    legacy.maxParkedVoices = 9;
    legacy.structSize =
        static_cast<std::uint32_t>(offsetof(VnConfig, maxActions));
    ASSERT_EQ(VN_OK, harken::ResolveConfig(&legacy, &resolved));
    EXPECT_EQ(7u, resolved.voiceCapacity.Render());
    EXPECT_EQ(256u, resolved.voiceCapacity.Virtual());
    EXPECT_EQ(263u, resolved.voiceCapacity.Logical());

    harken::VoiceCapacityModel overflow;
    ASSERT_TRUE(harken::VoiceCapacityModel::TryCreate(2, 3, &overflow));
    EXPECT_FALSE(harken::VoiceCapacityModel::TryCreate(
        std::numeric_limits<std::uint32_t>::max(), 1, &overflow));
    EXPECT_EQ(2u, overflow.Render());
    EXPECT_EQ(3u, overflow.Virtual());
    EXPECT_EQ(5u, overflow.Logical());
}

TEST(VoiceCapacity, DefaultRenderCapacityHasStereoScratchForEveryVoice) {
    VnConfig config = harkentest::MakeNullConfig();
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(256u, engine->config.voiceCapacity.Render());
    ASSERT_EQ(2u << 20, engine->config.arenaBytes);

    const std::vector<float> source(kBlockFrames * 8u * 2u, 0.001f);
    const VnPCMDesc pcm =
        harkentest::InterleavedDesc(source, 2u, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(kSoundId, &pcm));
    const VnPlayDesc play = harkentest::MakePlayDesc(kSoundId);

    std::array<VnVoiceHandle, 256> handles{};
    for (std::size_t i = 0; i < handles.size(); ++i) {
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handles[i])) << "voice=" << i;
    }
    engine->nullBackend.Pump(1);

    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    EXPECT_EQ(256u, snapshot.activeVoiceCount);
    EXPECT_EQ(0u, snapshot.arenaOverflowCount);
    EXPECT_GE(snapshot.arenaHighWaterBytes,
              static_cast<std::uint64_t>(256u * 2u * kBlockFrames *
                                         sizeof(float)));
    for (const VnVoiceHandle handle : handles) {
        EXPECT_NE(0u, handle);
        EXPECT_NE(0u, harkentest::RenderHandle(engine, handle));
    }
}

TEST(VoiceCapacity, RenderRefusalLeavesIdentityAndPcmResourcesUntouched) {
    VnConfig config =
        harkentest::MakeNullConfig(/*maxVoices=*/2, kBlockFrames, kSampleRate);
    config.maxParkedVoices = 1;
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(2u, engine->config.voiceCapacity.Render());
    ASSERT_EQ(1u, engine->config.voiceCapacity.Virtual());
    ASSERT_EQ(3u, engine->ledger.Capacity());

    const std::vector<float> source =
        harkentest::MakeDC(kSampleRate, 0.25f);
    const VnPCMDesc pcm = harkentest::MonoDesc(source, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(kSoundId, &pcm));
    const harken::PCMView view = engine->registry.Lookup(kSoundId);
    ASSERT_NE(nullptr, view.useCount);

    const VnPlayDesc play = harkentest::MakePlayDesc(kSoundId);
    VnVoiceHandle first = 0;
    VnVoiceHandle second = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &first));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &second));
    ASSERT_EQ(2u, engine->voices.ActiveCount());
    ASSERT_EQ(2u, view.useCount->load(std::memory_order_relaxed));

    const std::uint64_t refusalsBefore = engine->voices.NoVoiceCount();
    VnVoiceHandle refused = std::numeric_limits<VnVoiceHandle>::max();
    EXPECT_EQ(VN_NO_VOICE, harkentest::PlayRaw(&play, &refused));
    EXPECT_EQ(std::numeric_limits<VnVoiceHandle>::max(), refused);
    EXPECT_EQ(refusalsBefore + 1, engine->voices.NoVoiceCount());
    EXPECT_EQ(2u, engine->voices.ActiveCount());
    EXPECT_EQ(2u, view.useCount->load(std::memory_order_relaxed));

    // The Render-full refusal must not claim a Logical Voice entry. With R=2,
    // V=1, and L=3, stopping without reaping leaves one ended identity.
    // Recovery can use the only remaining entry; a hidden claim makes this
    // play fail before it can claim the free Render Voice.
    ASSERT_EQ(VN_OK, Vn_Stop(first, 0));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(1u, engine->voices.ActiveCount());
    ASSERT_EQ(1u, view.useCount->load(std::memory_order_relaxed));
    VnVoiceHandle recovered = 0;
    EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &recovered));
    EXPECT_NE(0u, recovered);
    EXPECT_EQ(2u, engine->voices.ActiveCount());
    EXPECT_EQ(2u, view.useCount->load(std::memory_order_relaxed));
}

TEST(VoiceCapacity, VirtualBoundaryKeepsOverflowEligibleVoiceRendering) {
    VnConfig config =
        harkentest::MakeNullConfig(/*maxVoices=*/2, kBlockFrames, kSampleRate);
    config.maxParkedVoices = 1;
    config.audibilityParkEnabled = 1;
    config.parkThresholdDb = -40.0f;
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(2u, engine->config.voiceCapacity.Render());
    ASSERT_EQ(1u, engine->config.voiceCapacity.Virtual());

    const std::vector<float> source =
        harkentest::MakeDC(kSampleRate, 0.25f);
    const VnPCMDesc pcm = harkentest::MonoDesc(source, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(kSoundId, &pcm));
    const VnAttenuationDesc attenuation =
        harkentest::MakeAttenuationDesc(1.0f, 100.0f);
    ASSERT_EQ(VN_OK,
              Vn_RegisterAttenuation(kFarProfileId, &attenuation));

    const VnPlayDesc play =
        harkentest::MakePlayDesc3D(kSoundId, 100.0f, 0.0f, 0.0f);
    VnVoiceHandle handles[2] = {};
    for (VnVoiceHandle& handle : handles) {
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        ASSERT_EQ(VN_OK,
                  Vn_BindVoiceAttenuation(handle, kFarProfileId));
    }
    ASSERT_EQ(
        VN_OK,
        Vn_SetListener(harkentest::Vec3(0.0f, 0.0f, 0.0f),
                       harkentest::Vec3(0.0f, 0.0f, -1.0f),
                       harkentest::Vec3(0.0f, 1.0f, 0.0f)));

    // Both voices are eligible. The park pass admits one Virtual Voice, then
    // the V=1 boundary keeps the other Logical Voice bound to its Render Voice.
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1); // applies the parked voice's internal stop

    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    EXPECT_EQ(1u, snapshot.activeVoiceCount);
    EXPECT_EQ(1u, snapshot.parkedVoiceCount);
    EXPECT_EQ(1u, engine->voices.ActiveCount());

    std::uint32_t live = 0;
    std::uint32_t parked = 0;
    for (const VnVoiceHandle handle : handles) {
        const harken::VoiceLedger::Resolved resolved =
            engine->ledger.Classify(handle);
        EXPECT_NE(harken::VoiceLedger::HandleKind::Stale, resolved.kind);
        if (resolved.kind == harken::VoiceLedger::HandleKind::Live) {
            ++live;
            ASSERT_NE(0u, resolved.renderHandle);
            EXPECT_NE(nullptr, engine->voices.Resolve(resolved.renderHandle));
        } else if (resolved.kind ==
                   harken::VoiceLedger::HandleKind::Parked) {
            ++parked;
            EXPECT_EQ(0u, resolved.renderHandle);
        } else {
            ADD_FAILURE() << "voice remained in an unexpected lifecycle state";
        }
    }
    EXPECT_EQ(1u, live);
    EXPECT_EQ(1u, parked);
}


TEST(VoiceCapacity,
     LogicalRefusalIsCountedDoesNotClaimRenderAndRecoversAfterReap) {
    VnConfig config =
        harkentest::MakeNullConfig(/*maxVoices=*/1, kBlockFrames, kSampleRate);
    config.maxParkedVoices = 1;
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(2u, engine->config.voiceCapacity.Logical());
    ASSERT_EQ(2u, engine->ledger.Capacity());

    const std::vector<float> source = harkentest::MakeDC(64, 0.25f);
    const VnPCMDesc pcm = harkentest::MonoDesc(source, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(kSoundId, &pcm));
    const harken::PCMView view = engine->registry.Lookup(kSoundId);
    ASSERT_NE(nullptr, view.useCount);
    const VnPlayDesc play = harkentest::MakePlayDesc(kSoundId);

    VnVoiceHandle first = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &first));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(0u, engine->voices.ActiveCount());

    VnVoiceHandle second = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &second));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(0u, engine->voices.ActiveCount());
    ASSERT_EQ(0u, view.useCount->load(std::memory_order_relaxed));

    const std::uint64_t refusalsBefore = engine->voices.NoVoiceCount();
    VnVoiceHandle refused = std::numeric_limits<VnVoiceHandle>::max();
    EXPECT_EQ(VN_NO_VOICE, harkentest::PlayRaw(&play, &refused));
    EXPECT_EQ(std::numeric_limits<VnVoiceHandle>::max(), refused);
    EXPECT_EQ(refusalsBefore + 1, engine->voices.NoVoiceCount());
    EXPECT_EQ(0u, engine->voices.ActiveCount());
    EXPECT_EQ(0u, view.useCount->load(std::memory_order_relaxed));

    ASSERT_EQ(VN_OK, Vn_Update());
    VnVoiceHandle recovered = 0;
    EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &recovered));
    EXPECT_NE(0u, recovered);
    EXPECT_EQ(refusalsBefore + 1, engine->voices.NoVoiceCount());
}

TEST(VoiceCapacity, LegacyConfigKeepsPublicAndRenderIdentitiesDecoupled) {
    VnConfig config =
        harkentest::MakeNullConfig(/*maxVoices=*/1, kBlockFrames, kSampleRate);
    config.structSize =
        static_cast<std::uint32_t>(offsetof(VnConfig, maxActions));
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(1u, engine->config.voiceCapacity.Render());
    ASSERT_EQ(256u, engine->config.voiceCapacity.Virtual());
    ASSERT_EQ(257u, engine->config.voiceCapacity.Logical());

    const std::vector<float> source = harkentest::MakeDC(64, 0.25f);
    const VnPCMDesc pcm = harkentest::MonoDesc(source, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(kSoundId, &pcm));
    const VnPlayDesc play = harkentest::MakePlayDesc(kSoundId);

    VnVoiceHandle first = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &first));
    engine->nullBackend.Pump(1);

    VnVoiceHandle second = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &second));
    const VnVoiceHandle secondRender =
        harkentest::RenderHandle(engine, second);
    ASSERT_NE(0u, secondRender);
    EXPECT_NE(static_cast<std::uint32_t>(second),
              static_cast<std::uint32_t>(secondRender));
    EXPECT_EQ(second, engine->ledger.ToPublic(secondRender));

    EXPECT_EQ(VN_STALE_HANDLE, Vn_Stop(first, 0));
    EXPECT_NE(nullptr, engine->voices.Resolve(secondRender));
}

TEST(VoiceCapacity, ScaleTiersKeepIndependentBoundariesAtomic) {
    constexpr std::array<std::uint32_t, 4> kTiers = {64, 128, 256, 512};
    constexpr VnSoundId kScaleSound = harkentest::ObjectId(81901);
    constexpr VnAttenuationId kScaleAttenuation = harkentest::ObjectId(81902);

    for (const std::uint32_t tier : kTiers) {
        SCOPED_TRACE(testing::Message() << "tier=" << tier);

        // Render-full: Logical capacity remains available, so the refusal is
        // owned by the Render boundary and must not claim identity or PCM.
        {
            VnConfig config = harkentest::MakeNullConfig(
                tier, kBlockFrames, kSampleRate);
            config.maxParkedVoices = tier;
            config.arenaBytes = 16u << 20;
            config.commandQueueDepth = 2048;
            config.notificationQueueDepth = 2048;
            harkentest::EngineSession session(config);
            ASSERT_EQ(VN_OK, session.InitResult());
            harken::Engine* engine = session.Engine();
            engine->mixer.SetMaxCommandsPerQuantumForTest(
                config.commandQueueDepth);
            ASSERT_EQ(tier, engine->config.voiceCapacity.Render());
            ASSERT_EQ(tier, engine->config.voiceCapacity.Virtual());
            ASSERT_EQ(tier * 2u, engine->config.voiceCapacity.Logical());
            ASSERT_EQ(tier * 2u, engine->ledger.Capacity());

            const std::vector<float> source =
                harkentest::MakeDC(kSampleRate, 0.1f);
            const VnPCMDesc pcm = harkentest::MonoDesc(
                source, kSampleRate, VN_PCM_POLICY_REFERENCE);
            ASSERT_EQ(VN_OK, Vn_RegisterPCM(kScaleSound, &pcm));
            const harken::PCMView view = engine->registry.Lookup(kScaleSound);
            ASSERT_NE(nullptr, view.useCount);

            VnPlayDesc play = harkentest::MakePlayDesc(kScaleSound);
            play.loopCount = 0;
            std::vector<VnVoiceHandle> handles(tier);
            for (VnVoiceHandle& handle : handles) {
                ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
                ASSERT_NE(0u, handle);
            }
            std::vector<VnVoiceHandle> ordered = handles;
            std::sort(ordered.begin(), ordered.end());
            EXPECT_EQ(ordered.end(),
                      std::adjacent_find(ordered.begin(), ordered.end()));
            ASSERT_EQ(tier, engine->voices.ActiveCount());
            ASSERT_EQ(tier,
                      view.useCount->load(std::memory_order_relaxed));

            const std::uint64_t refusals = engine->voices.NoVoiceCount();
            VnVoiceHandle refused =
                std::numeric_limits<VnVoiceHandle>::max();
            EXPECT_EQ(VN_NO_VOICE, harkentest::PlayRaw(&play, &refused));
            EXPECT_EQ(std::numeric_limits<VnVoiceHandle>::max(), refused);
            EXPECT_EQ(refusals + 1u, engine->voices.NoVoiceCount());
            EXPECT_EQ(tier, engine->voices.ActiveCount());
            EXPECT_EQ(tier,
                      view.useCount->load(std::memory_order_relaxed));

            const std::uint64_t allocationHits =
                harken::memguard::HitCount();
#ifdef _DEBUG
            ASSERT_TRUE(harken::memguard::IsArmed());
#endif
            engine->nullBackend.Pump(1);
            EXPECT_EQ(allocationHits, harken::memguard::HitCount());
            EXPECT_GT(harkentest::Peak(
                          engine->nullBackend.LastBlock(),
                          static_cast<std::size_t>(kBlockFrames) * 2u),
                      harkentest::kSilenceFloor);
        }

        // Virtual-full: another inaudible Voice remains Render-bound instead
        // of crossing the Virtual boundary. Releasing it lets a parked public
        // identity return without changing that identity.
        {
            VnConfig config = harkentest::MakeNullConfig(
                tier, kBlockFrames, kSampleRate);
            config.maxParkedVoices = tier;
            config.arenaBytes = 16u << 20;
            config.audibilityParkEnabled = 1;
            config.parkThresholdDb = -40.0f;
            config.commandQueueDepth = 2048;
            config.notificationQueueDepth = 2048;
            harkentest::EngineSession session(config);
            ASSERT_EQ(VN_OK, session.InitResult());
            harken::Engine* engine = session.Engine();
            // A tier's worth of plays is more commands than one quantum
            // applies, and the park cadence asserted below counts blocks.
            // This case is about capacity tiers, not the command ceiling, so
            // lift the ceiling and keep the original per-block sequence.
            engine->mixer.SetMaxCommandsPerQuantumForTest(
                config.commandQueueDepth);

            const std::vector<float> source =
                harkentest::MakeDC(kSampleRate, 0.1f);
            const VnPCMDesc pcm = harkentest::MonoDesc(
                source, kSampleRate, VN_PCM_POLICY_REFERENCE);
            ASSERT_EQ(VN_OK, Vn_RegisterPCM(kScaleSound, &pcm));
            const VnAttenuationDesc attenuation =
                harkentest::MakeAttenuationDesc(1.0f, 100.0f);
            ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(
                                 kScaleAttenuation, &attenuation));
            ASSERT_EQ(VN_OK, Vn_SetListener(
                                 harkentest::Vec3(0.0f, 0.0f, 0.0f),
                                 harkentest::Vec3(0.0f, 0.0f, -1.0f),
                                 harkentest::Vec3(0.0f, 1.0f, 0.0f)));

            VnPlayDesc far = harkentest::MakePlayDesc3D(
                kScaleSound, 100.0f, 0.0f, 0.0f);
            far.loopCount = 0;
            std::vector<VnVoiceHandle> parkedHandles(tier);
            for (VnVoiceHandle& handle : parkedHandles) {
                ASSERT_EQ(VN_OK, harkentest::PlayRaw(&far, &handle));
                ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(
                                     handle, kScaleAttenuation));
            }
            engine->nullBackend.Pump(1);
            ASSERT_EQ(VN_OK, Vn_Update());
            engine->nullBackend.Pump(1);
            ASSERT_EQ(tier, engine->ledger.ParkedCount());
            ASSERT_EQ(0u, engine->voices.ActiveCount());

            VnVoiceHandle overflow = 0;
            ASSERT_EQ(VN_OK, harkentest::PlayRaw(&far, &overflow));
            ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(
                                 overflow, kScaleAttenuation));
            engine->nullBackend.Pump(1);
            ASSERT_EQ(VN_OK, Vn_Update());
            engine->nullBackend.Pump(1);
            EXPECT_EQ(tier, engine->ledger.ParkedCount());
            EXPECT_EQ(1u, engine->voices.ActiveCount());
            EXPECT_EQ(harken::VoiceLedger::HandleKind::Live,
                      engine->ledger.Classify(overflow).kind);

            ASSERT_EQ(VN_OK, Vn_Stop(overflow, 0));
            engine->nullBackend.Pump(1);
            ASSERT_EQ(VN_OK, Vn_SetListener(
                                 harkentest::Vec3(100.0f, 0.0f, 0.0f),
                                 harkentest::Vec3(0.0f, 0.0f, -1.0f),
                                 harkentest::Vec3(0.0f, 1.0f, 0.0f)));
            ASSERT_EQ(VN_OK, Vn_Update());
            engine->nullBackend.Pump(1);
            bool returned = false;
            for (const VnVoiceHandle handle : parkedHandles) {
                returned = returned ||
                    engine->ledger.Classify(handle).kind ==
                        harken::VoiceLedger::HandleKind::Live;
            }
            EXPECT_TRUE(returned);
            EXPECT_EQ(tier, engine->voices.ActiveCount());
            EXPECT_EQ(0u, engine->ledger.ParkedCount());
        }

        // Scale-full: Render, Virtual, and Logical occupancy reach the exact
        // (C,C,2C) tuple at the same time. A parked identity returns unchanged
        // after one Render slot is released, and cleanup restores both pools.
        {
            VnConfig config = harkentest::MakeNullConfig(
                tier, kBlockFrames, kSampleRate);
            config.maxParkedVoices = tier;
            config.arenaBytes = 16u << 20;
            config.audibilityParkEnabled = 1;
            config.parkThresholdDb = -40.0f;
            config.commandQueueDepth = 2048;
            config.notificationQueueDepth = 2048;
            harkentest::EngineSession session(config);
            ASSERT_EQ(VN_OK, session.InitResult());
            harken::Engine* engine = session.Engine();
            engine->mixer.SetMaxCommandsPerQuantumForTest(
                config.commandQueueDepth);

            const std::vector<float> source =
                harkentest::MakeDC(kSampleRate, 0.1f);
            const VnPCMDesc pcm = harkentest::MonoDesc(
                source, kSampleRate, VN_PCM_POLICY_REFERENCE);
            ASSERT_EQ(VN_OK, Vn_RegisterPCM(kScaleSound, &pcm));
            const VnAttenuationDesc attenuation =
                harkentest::MakeAttenuationDesc(1.0f, 100.0f);
            ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(
                                 kScaleAttenuation, &attenuation));
            ASSERT_EQ(VN_OK, Vn_SetListener(
                                 harkentest::Vec3(0.0f, 0.0f, 0.0f),
                                 harkentest::Vec3(0.0f, 0.0f, -1.0f),
                                 harkentest::Vec3(0.0f, 1.0f, 0.0f)));

            VnPlayDesc far = harkentest::MakePlayDesc3D(
                kScaleSound, 100.0f, 0.0f, 0.0f);
            far.loopCount = 0;
            std::vector<VnVoiceHandle> parkedHandles(tier);
            for (VnVoiceHandle& handle : parkedHandles) {
                ASSERT_EQ(VN_OK, harkentest::PlayRaw(&far, &handle));
                ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(
                                     handle, kScaleAttenuation));
            }
            engine->nullBackend.Pump(1);
            ASSERT_EQ(VN_OK, Vn_Update());
            engine->nullBackend.Pump(1);
            ASSERT_EQ(tier, engine->ledger.ParkedCount());

            VnPlayDesc near = harkentest::MakePlayDesc3D(
                kScaleSound, 0.0f, 0.0f, 0.0f);
            near.loopCount = 0;
            std::vector<VnVoiceHandle> renderHandles(tier);
            for (VnVoiceHandle& handle : renderHandles) {
                ASSERT_EQ(VN_OK, harkentest::PlayRaw(&near, &handle));
                ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(
                                     handle, kScaleAttenuation));
            }
            engine->nullBackend.Pump(1);
            ASSERT_EQ(VN_OK, Vn_Update());
            ASSERT_EQ(tier, engine->voices.ActiveCount());
            ASSERT_EQ(tier, engine->ledger.ParkedCount());
            ASSERT_EQ(tier * 2u, engine->ledger.Capacity());

            std::vector<VnVoiceHandle> identities = parkedHandles;
            identities.insert(identities.end(), renderHandles.begin(),
                              renderHandles.end());
            std::sort(identities.begin(), identities.end());
            EXPECT_EQ(identities.end(), std::adjacent_find(
                                                identities.begin(),
                                                identities.end()));

            const VnVoiceHandle sentinel = parkedHandles.front();
            ASSERT_EQ(VN_OK, Vn_Stop(renderHandles.front(), 0));
            engine->nullBackend.Pump(1);
            ASSERT_EQ(VN_OK, Vn_SetListener(
                                 harkentest::Vec3(100.0f, 0.0f, 0.0f),
                                 harkentest::Vec3(0.0f, 0.0f, -1.0f),
                                 harkentest::Vec3(0.0f, 1.0f, 0.0f)));
            ASSERT_EQ(VN_OK, Vn_Update());
            engine->nullBackend.Pump(1);
            EXPECT_EQ(harken::VoiceLedger::HandleKind::Live,
                      engine->ledger.Classify(sentinel).kind);

            for (const VnVoiceHandle handle : identities) {
                const VnResult stopped = Vn_Stop(handle, 0);
                EXPECT_TRUE(stopped == VN_OK || stopped == VN_STALE_HANDLE);
            }
            engine->nullBackend.Pump(1);
            ASSERT_EQ(VN_OK, Vn_Update());
            engine->nullBackend.Pump(1);
            EXPECT_EQ(0u, engine->voices.ActiveCount());
            EXPECT_EQ(0u, engine->ledger.ParkedCount());
            for (const VnVoiceHandle handle : identities) {
                EXPECT_EQ(harken::VoiceLedger::HandleKind::Stale,
                          engine->ledger.Classify(handle).kind);
            }
        }

        // Logical-full: both Render batches have ended but remain unreaped,
        // leaving Render slots free while the Ledger owns all 2C identities.
        {
            VnConfig config = harkentest::MakeNullConfig(
                tier, kBlockFrames, kSampleRate);
            config.maxParkedVoices = tier;
            config.arenaBytes = 16u << 20;
            config.commandQueueDepth = 2048;
            config.notificationQueueDepth = 2048;
            harkentest::EngineSession session(config);
            ASSERT_EQ(VN_OK, session.InitResult());
            harken::Engine* engine = session.Engine();
            engine->mixer.SetMaxCommandsPerQuantumForTest(
                config.commandQueueDepth);

            const std::vector<float> source =
                harkentest::MakeDC(64, 0.1f);
            const VnPCMDesc pcm = harkentest::MonoDesc(
                source, kSampleRate, VN_PCM_POLICY_REFERENCE);
            ASSERT_EQ(VN_OK, Vn_RegisterPCM(kScaleSound, &pcm));
            VnPlayDesc play = harkentest::MakePlayDesc(kScaleSound);

            for (std::uint32_t batch = 0; batch < 2; ++batch) {
                for (std::uint32_t voice = 0; voice < tier; ++voice) {
                    VnVoiceHandle handle = 0;
                    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
                }
                engine->nullBackend.Pump(1);
                ASSERT_EQ(0u, engine->voices.ActiveCount());
            }

            const std::uint64_t refusals = engine->voices.NoVoiceCount();
            VnVoiceHandle refused =
                std::numeric_limits<VnVoiceHandle>::max();
            EXPECT_EQ(VN_NO_VOICE, harkentest::PlayRaw(&play, &refused));
            EXPECT_EQ(std::numeric_limits<VnVoiceHandle>::max(), refused);
            EXPECT_EQ(refusals + 1u, engine->voices.NoVoiceCount());
            EXPECT_EQ(0u, engine->voices.ActiveCount());

            ASSERT_EQ(VN_OK, Vn_Update());
            VnVoiceHandle recovered = 0;
            EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &recovered));
            EXPECT_NE(0u, recovered);
        }
    }
}

} // namespace
