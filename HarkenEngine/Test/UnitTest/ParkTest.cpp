// Audibility parking / return (spec §6, acceptance §9.6): an inaudible voice
// releases its render slot but keeps its handle and timeline, and returns when
// audible again. Parking is driven here by distance attenuation — a spatialized
// voice bound to a linear rolloff profile, with the listener moved to change the
// distance (and thus the combine-resolved per-voice volume that is the
// audibility metric). All tests run on the null backend, single-threaded, so
// the source-cursor mirror and the published block clock are read coherently.
//
// Return tolerance: the parked timeline authority is the published source
// cursor, which is the source frame PUSHED to the resampler input — it leads the
// audible output by at most the resampler FIFO depth plus history pad, and the
// double-buffer read is at most one block stale. So a returned voice resumes
// within (1 block + FIFO + history) source frames of its virtual position; the
// resume assertions use that bound.
#include "Common/ObjectIdTestSupport.h"
#include "Common/RecipePackTestUtil.h"
#include "Common/TestUtil.h"

#include "harken/core/LogicAllocCheckpoint.h"
#include "harken/param/EffectParamLocator.h"
#include "harken/param/ParamTargets.h"
#include "harken/render/BuiltinEffects.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

using harkentest::EngineSession;
using harkentest::MakeAttenuationDesc;
using harkentest::MakeDC;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::MakePlayDesc3D;
using harkentest::MonoDesc;
using harkentest::RenderHandle;
using harkentest::Vec3;

constexpr std::uint32_t kBlock = 256;
constexpr std::uint32_t kRate = 48000;
// Bound on how far a returned voice's cursor may differ from its virtual offset
// (one block of staleness + one resampler FIFO + the history pad; kResampleFifo
// 256 + pad 8 rounded up generously).
constexpr std::uint64_t kResumeToleranceFrames = kBlock + 512;

VnConfig ParkConfig(std::uint32_t maxVoices = 4,
                    std::uint32_t maxParkedVoices = 4,
                    float thresholdDb = -40.0f, std::uint32_t enabled = 1) {
    VnConfig config = MakeNullConfig(maxVoices, kBlock, kRate);
    // Parking coverage does not require the default nested Action scratch.
    config.maxTriggerDepth = 2;
    config.maxParkedVoices = maxParkedVoices;
    config.audibilityParkEnabled = enabled;
    config.parkThresholdDb = thresholdDb;
    return config;
}

// A long DC source so the tested voices never end on their own within the few
// blocks each drill pumps (except the one-shot-end test, which uses a short one).
void RegisterLeaf(VnSoundId id, std::size_t frames = kRate) {
    static std::vector<float> longDc = MakeDC(kRate, 0.5f);
    static std::vector<float> shortDc = MakeDC(kBlock, 0.5f);
    std::vector<float>& src = (frames <= kBlock) ? shortDc : longDc;
    VnPCMDesc desc = MonoDesc(src, kRate);
    desc.frames = frames;
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(id, &desc));
}

void RegisterLeafAtRate(VnSoundId id, std::size_t frames,
                        std::uint32_t sampleRate) {
    const std::vector<float> source = MakeDC(frames, 0.5f);
    const VnPCMDesc desc = MonoDesc(source, sampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(id, &desc));
}

// A far/near listener sets the voice's distance and thus its metric. The voice
// sits at (100,0,0); a linear profile over [1,100] spans 0 dB (near) to the
// attenuation floor (far).
void RegisterFarProfile(VnAttenuationId id) {
    const VnAttenuationDesc desc = MakeAttenuationDesc(1.0f, 100.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(id, &desc));
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

harkentest::RecipePackArtifact BuildSpatialRecipePack(
    const std::string& stem) {
    std::filesystem::create_directories(HARKEN_TEMP_DIR);
    const std::string unique =
        stem + "_" + std::to_string(_getpid());
    const std::filesystem::path base =
        std::filesystem::path(HARKEN_TEMP_DIR);
    const std::string wav =
        (base / (unique + ".wav")).generic_string();
    const std::string pack =
        (base / (unique + ".pack")).generic_string();
    const std::string manifest =
        (base / (unique + ".json")).generic_string();
    harkentest::RecipePackArtifact artifact;
    artifact.path = pack;
    if (!harkentest::WriteRecipeWav(wav, 2048)) {
        return artifact;
    }
    const std::string json =
        "{\n"
        "  \"manifestVersion\": 3,\n"
        "  \"output\": \"" + pack + "\",\n"
        "  \"packKind\": \"content\",\n"
        "  \"entries\": [{\"path\": \"" + wav +
        "\", \"id\": " + harkentest::JsonObjectId(harkentest::ObjectId(9201)) + "}],\n"
        "  \"voiceRecipes\": [{\n"
        "    \"playable\": " + harkentest::JsonObjectId(harkentest::ObjectId(9201)) + ",\n"
        "    \"spatial\": {\"attenuation\": " + harkentest::JsonObjectId(harkentest::ObjectId(9)) + ", "
        "\"distanceScale\": 1.0},\n"
        "    \"effects\": [],\n"
        "    \"bindings\": []\n"
        "  }]\n"
        "}\n";
    {
        std::ofstream out(manifest,
                          std::ios::binary | std::ios::trunc);
        if (!out.good()) {
            return artifact;
        }
        out.write(json.data(), static_cast<std::streamsize>(json.size()));
        if (!out.good()) {
            return artifact;
        }
    }
    artifact.built = harkentest::CompileResources({manifest}) == 0;
    return artifact;
}

void SetListenerX(float x) {
    ASSERT_EQ(VN_OK, Vn_SetListener(Vec3(x, 0.0f, 0.0f), Vec3(0.0f, 0.0f, -1.0f),
                                    Vec3(0.0f, 1.0f, 0.0f)));
}

VnSnapshot Snap() {
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    EXPECT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    return snapshot;
}

struct NotificationSink {
    std::vector<VnNotification> notifications;
    std::size_t Count(std::uint32_t type, VnVoiceHandle handle) const {
        std::size_t n = 0;
        for (const VnNotification& e : notifications) {
            if (e.type == type && e.voiceHandle == handle) {
                ++n;
            }
        }
        return n;
    }
};
void SinkCb(const VnNotification* e, void* ud) {
    static_cast<NotificationSink*>(ud)->notifications.push_back(*e);
}

// Plays a spatialized voice at (100,0,0) bound to the far profile, with the
// listener already placed so the voice is inaudible; pumps and updates until it
// parks. Returns the public handle.
VnVoiceHandle PlayAndPark(harken::Engine* engine, VnSoundId sound,
                          VnAttenuationId profile) {
    VnPlayDesc play = MakePlayDesc3D(sound, 100.0f, 0.0f, 0.0f);
    VnVoiceHandle handle = 0;
    EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    EXPECT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, profile));
    // Listener at origin -> distance 100 -> attenuation floor -> inaudible.
    SetListenerX(0.0f);
    engine->nullBackend.Pump(1); // render + publish the cursor
    EXPECT_EQ(VN_OK, Vn_Update()); // spatial + flush + park pass parks it
    engine->nullBackend.Pump(1);   // audio applies the internal stop, frees slot
    return handle;
}

// --- config-off invariance ---

TEST(Park, DisabledLeavesInaudibleVoicesLive) {
    EngineSession session(ParkConfig(/*maxVoices=*/4, /*maxParkedVoices=*/4,
                                     /*thresholdDb=*/-40.0f, /*enabled=*/0));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterLeaf(harkentest::ObjectId(1));
    RegisterFarProfile(harkentest::ObjectId(9));
    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 100.0f, 0.0f, 0.0f);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, harkentest::ObjectId(9)));
    SetListenerX(0.0f);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    // Parking off: the inaudible voice stays live, holding its slot.
    EXPECT_NE(nullptr, engine->voices.Resolve(RenderHandle(engine, handle)));
    EXPECT_EQ(0u, Snap().parkedVoiceCount);
}

TEST(Park, ImmediateInaudiblePcmStartsVirtualWithoutRenderOrCommand) {
    NotificationSink sink;
    VnConfig config = ParkConfig(/*maxVoices=*/1, /*maxParkedVoices=*/1);
    config.onNotification = &SinkCb;
    config.notificationUserData = &sink;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterLeaf(harkentest::ObjectId(1));

    const std::uint32_t freeCommands = engine->commands.FreeSlots();
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1), -80.0f);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    EXPECT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(handle).kind);
    const harken::VoiceLedger::Resolved parked =
        engine->ledger.Classify(handle);
    ASSERT_LT(parked.ledgerSlot, engine->parkRecords.size());
    EXPECT_EQ(harken::ResampleProfile::LegacyCompatibility,
              engine->parkRecords[parked.ledgerSlot]
                  .facts.resampleProfile);
    EXPECT_EQ(0u, engine->voices.ActiveCount());
    EXPECT_EQ(freeCommands, engine->commands.FreeSlots());
    EXPECT_EQ(0u, Snap().voiceStolenCount);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(1u, sink.Count(VN_NOTIFY_VOICE_PARKED, handle));
}

TEST(Park, ImmediateVirtualIgnoresFullCommandQueue) {
    VnConfig config = ParkConfig(/*maxVoices=*/2, /*maxParkedVoices=*/1);
    config.commandQueueDepth = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterLeaf(harkentest::ObjectId(1));

    VnPlayDesc loud = MakePlayDesc(harkentest::ObjectId(1), 0.0f);
    VnVoiceHandle incumbent = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&loud, &incumbent));
    ASSERT_EQ(0u, engine->commands.FreeSlots());
    const std::uint64_t backpressureBefore =
        engine->commands.BackpressureCount();

    VnPlayDesc quiet = MakePlayDesc(harkentest::ObjectId(1), -80.0f);
    VnVoiceHandle virtualized = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&quiet, &virtualized));
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(virtualized).kind);
    EXPECT_EQ(0u, engine->commands.FreeSlots());
    EXPECT_EQ(backpressureBefore, engine->commands.BackpressureCount());
    EXPECT_EQ(1u, engine->voices.ActiveCount());
    EXPECT_NE(nullptr,
              engine->voices.Resolve(RenderHandle(engine, incumbent)));
    EXPECT_EQ(1u, engine->voiceParkedCount);
}

TEST(Park, ImmediateVirtualRecipeBindingActivatesOnReturn) {
    VnConfig config = ParkConfig(/*maxVoices=*/1, /*maxParkedVoices=*/1);
    config.fxSlotsPerVoice = 1;
    config.gainEffectPoolCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    constexpr VnBindingId kBinding = harkentest::ObjectId(71);
    constexpr VnVariableId kVariable = harkentest::ObjectId(72);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(
                  engine, {kVariable}, 0.0f, 1.0f).status);
    VnBindingDesc binding{};
    binding.structSize = sizeof(VnBindingDesc);
    binding.variableId = kVariable;
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
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1.0f));
    harkentest::AdvanceVariableWrite(engine);

    const harkentest::RecipePackArtifact artifact =
        harkentest::BuildGainRecipePack(
            "park_direct_virtual_recipe_binding", {kBinding});
    ASSERT_TRUE(artifact.built);
    VnPackDesc packDesc{};
    packDesc.structSize = sizeof(VnPackDesc);
    packDesc.utf8Path = artifact.path.c_str();
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, Vn_LoadPack(&packDesc, &pack));

    const std::uint32_t commandRoom = engine->commands.FreeSlots();
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(9101), -80.0f);
    play.loopCount = 0;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    const std::uint32_t ledgerSlot =
        static_cast<std::uint32_t>(handle);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(handle).kind);
    EXPECT_EQ(commandRoom, engine->commands.FreeSlots());
    ASSERT_EQ(1u, engine->parkRecords[ledgerSlot].recipeBindingCount);
    ASSERT_EQ(1u, engine->parkRecords[ledgerSlot].effects.size());

    std::uint32_t reservedInstances = 0;
    engine->bindings.ForEachVoiceEffectInstanceOf(
        kBinding, [&](VnVoiceHandle render, bool held) {
            ++reservedInstances;
            EXPECT_EQ(0u, render);
            EXPECT_TRUE(held);
        });
    EXPECT_EQ(1u, reservedInstances);

    ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(handle, 80.0f, 0));
    ASSERT_EQ(VN_OK, Vn_Update());
    const VnVoiceHandle returnedRender = RenderHandle(engine, handle);
    ASSERT_NE(0u, returnedRender);
    std::uint32_t activeInstances = 0;
    engine->bindings.ForEachVoiceEffectInstanceOf(
        kBinding, [&](VnVoiceHandle render, bool held) {
            ++activeInstances;
            EXPECT_EQ(returnedRender, render);
            EXPECT_FALSE(held);
        });
    EXPECT_EQ(1u, activeInstances);

    engine->nullBackend.Pump(1);
    harken::VoiceRenderContext* context =
        engine->voices.Resolve(returnedRender);
    ASSERT_NE(nullptr, context);
    ASSERT_EQ(static_cast<std::uint32_t>(VN_EFFECT_GAIN),
              context->fxSlotEffectRef[0].type);
    auto* gain = static_cast<harken::GainState*>(
        context->fxSlots[0].state);
    ASSERT_NE(nullptr, gain);
    EXPECT_NEAR(std::pow(10.0f, -6.0f / 20.0f),
                gain->targetGainLinear, 1e-5f);
}

TEST(Park, ImmediateVirtualRecipeCapturesConfigWithoutEffectInstance) {
    VnConfig config = ParkConfig(/*maxVoices=*/1, /*maxParkedVoices=*/1);
    config.fxSlotsPerVoice = 1;
    config.gainEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const harkentest::RecipePackArtifact artifact =
        harkentest::BuildGainRecipePack(
            "park_direct_virtual_recipe_config_only");
    ASSERT_TRUE(artifact.built);
    VnPackDesc packDesc{};
    packDesc.structSize = sizeof(VnPackDesc);
    packDesc.utf8Path = artifact.path.c_str();
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, Vn_LoadPack(&packDesc, &pack));

    VnPlayDesc loud = MakePlayDesc(harkentest::ObjectId(9101), 0.0f);
    loud.loopCount = 0;
    VnVoiceHandle incumbent = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&loud, &incumbent));
    ASSERT_EQ(1u, engine->effectBank.InUseOf(VN_EFFECT_GAIN));

    VnPlayDesc quiet = MakePlayDesc(harkentest::ObjectId(9101), -80.0f);
    quiet.loopCount = 0;
    VnVoiceHandle parked = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&quiet, &parked));
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(parked).kind);
    EXPECT_EQ(1u, engine->effectBank.InUseOf(VN_EFFECT_GAIN));
    ASSERT_EQ(1u,
              engine->parkRecords[static_cast<std::uint32_t>(parked)]
                  .effects.size());

    ASSERT_EQ(VN_OK, Vn_Stop(incumbent, 0));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(parked, 80.0f, 0));
    ASSERT_EQ(VN_OK, Vn_Update());
    const VnVoiceHandle returned = RenderHandle(engine, parked);
    ASSERT_NE(0u, returned);
    EXPECT_EQ(1u, engine->effectBank.InUseOf(VN_EFFECT_GAIN));
    engine->nullBackend.Pump(1);
    harken::VoiceRenderContext* context =
        engine->voices.Resolve(returned);
    ASSERT_NE(nullptr, context);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_EFFECT_GAIN),
              context->fxSlotEffectRef[0].type);
}

TEST(Park, ImmediateVirtualUsesAuthoredRecipeAttenuation) {
    EngineSession session(
        ParkConfig(/*maxVoices=*/1, /*maxParkedVoices=*/1));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterFarProfile(harkentest::ObjectId(9));
    const harkentest::RecipePackArtifact artifact =
        BuildSpatialRecipePack("park_direct_virtual_spatial_recipe");
    ASSERT_TRUE(artifact.built);
    VnPackDesc packDesc{};
    packDesc.structSize = sizeof(VnPackDesc);
    packDesc.utf8Path = artifact.path.c_str();
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, Vn_LoadPack(&packDesc, &pack));
    SetListenerX(0.0f);

    const std::uint32_t commandRoom = engine->commands.FreeSlots();
    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(9201), 100.0f, 0.0f, 0.0f);
    play.volumeDb = 0.0f;
    play.loopCount = 0;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    const VnVoiceHandle publicHandle = handle;
    const std::uint32_t ledgerSlot =
        static_cast<std::uint32_t>(handle);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(handle).kind);
    EXPECT_EQ(0u, RenderHandle(engine, handle));
    EXPECT_EQ(0u, engine->voices.ActiveCount());
    EXPECT_EQ(commandRoom, engine->commands.FreeSlots());
    const harken::ParkRecord& parked =
        engine->parkRecords[ledgerSlot];
    ASSERT_TRUE(parked.active);
    EXPECT_EQ(harkentest::ObjectId(9u), parked.spatial.attenuationId);
    EXPECT_FLOAT_EQ(1.0f, parked.spatial.distanceScale);

    SetListenerX(99.0f);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Live,
              engine->ledger.Classify(publicHandle).kind);
    EXPECT_EQ(publicHandle,
              engine->ledger.PublicHandleForSlot(ledgerSlot));
    const VnVoiceHandle render =
        RenderHandle(engine, publicHandle);
    ASSERT_NE(0u, render);
    ASSERT_NE(nullptr, engine->voices.Resolve(render));
    harken::Spatializer::SpatialVoiceState spatial;
    ASSERT_TRUE(engine->spatial.CaptureVoice(render, &spatial));
    EXPECT_EQ(harkentest::ObjectId(9u), spatial.attenuationId);
    EXPECT_FLOAT_EQ(1.0f, spatial.distanceScale);
}


TEST(Park, ImmediateVirtualWorksWhenRenderFullAndVFullFallsBack) {
    VnConfig config = ParkConfig(/*maxVoices=*/1, /*maxParkedVoices=*/1);
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterLeaf(harkentest::ObjectId(1));

    VnVoiceHandle live = 0;
    VnPlayDesc loud = MakePlayDesc(harkentest::ObjectId(1), 0.0f);
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&loud, &live));

    VnVoiceHandle parked = 0;
    VnPlayDesc quiet = MakePlayDesc(harkentest::ObjectId(1), -80.0f);
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&quiet, &parked));
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(parked).kind);
    EXPECT_EQ(1u, engine->voices.ActiveCount());

    ASSERT_EQ(VN_OK, Vn_Stop(live, 0));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    VnVoiceHandle fallback = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&quiet, &fallback));
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Live,
              engine->ledger.Classify(fallback).kind);
}



TEST(Park, LimitedVirtualCapacityParksLowestPriorityFirst) {
    EngineSession session(
        ParkConfig(/*maxVoices=*/3, /*maxParkedVoices=*/1));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterLeaf(harkentest::ObjectId(1));

    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1), 0.0f);
    VnVoiceHandle high = 0;
    VnVoiceHandle low = 0;
    VnVoiceHandle middle = 0;
    play.priority = 200;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &high));
    play.priority = 50;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &low));
    play.priority = 100;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &middle));
    engine->nullBackend.Pump(1);

    ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(high, -80.0f, 0));
    ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(low, -80.0f, 0));
    ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(middle, -80.0f, 0));
    ASSERT_EQ(VN_OK, Vn_Update());

    EXPECT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(low).kind);
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Live,
              engine->ledger.Classify(high).kind);
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Live,
              engine->ledger.Classify(middle).kind);
}

TEST(Park, LiveParkTieUsesLowestRenderSlot) {
    EngineSession session(
        ParkConfig(/*maxVoices=*/3, /*maxParkedVoices=*/1));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterLeaf(harkentest::ObjectId(1));

    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1), 0.0f);
    play.priority = 100;
    VnVoiceHandle voices[3] = {};
    for (VnVoiceHandle& voice : voices) {
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    }
    engine->nullBackend.Pump(1);
    for (VnVoiceHandle voice : voices) {
        ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(voice, -80.0f, 0));
    }
    ASSERT_EQ(VN_OK, Vn_Update());

    EXPECT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(voices[0]).kind);
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Live,
              engine->ledger.Classify(voices[1]).kind);
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Live,
              engine->ledger.Classify(voices[2]).kind);
}

// --- suspended lifecycle defers autonomous park and return decisions ---

TEST(Park, SuspendedUpdateDefersParkUntilResume) {
    EngineSession session(ParkConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterLeaf(harkentest::ObjectId(1));
    RegisterFarProfile(harkentest::ObjectId(9));

    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 100.0f, 0.0f, 0.0f);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, harkentest::ObjectId(9)));
    const VnVoiceHandle render = RenderHandle(engine, handle);
    SetListenerX(100.0f);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(0u, Snap().parkedVoiceCount);

    ASSERT_EQ(VN_OK, Vn_Suspend());
    SetListenerX(0.0f);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(0u, Snap().parkedVoiceCount);
    EXPECT_NE(nullptr, engine->voices.Resolve(render));

    ASSERT_EQ(VN_OK, Vn_Resume());
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(1u, Snap().parkedVoiceCount);
}

TEST(Park, SuspendedUpdateDefersReturnUntilResume) {
    EngineSession session(ParkConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterLeaf(harkentest::ObjectId(1));
    RegisterFarProfile(harkentest::ObjectId(9));

    PlayAndPark(engine, harkentest::ObjectId(1), harkentest::ObjectId(9));
    ASSERT_EQ(1u, Snap().parkedVoiceCount);

    ASSERT_EQ(VN_OK, Vn_Suspend());
    SetListenerX(100.0f);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(1u, Snap().parkedVoiceCount);
    EXPECT_EQ(0u, engine->voices.ActiveCount());

    ASSERT_EQ(VN_OK, Vn_Resume());
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(0u, Snap().parkedVoiceCount);
    EXPECT_EQ(1u, engine->voices.ActiveCount());
}

// --- park frees the render slot ---

TEST(Park, ParkFreesRenderSlotForANewPlay) {
    // A one-voice pool: the parked voice must free its slot so a new play fits.
    EngineSession session(ParkConfig(/*maxVoices=*/1, /*maxParkedVoices=*/1));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterLeaf(harkentest::ObjectId(1));
    RegisterFarProfile(harkentest::ObjectId(9));

    const VnVoiceHandle parked = PlayAndPark(engine, harkentest::ObjectId(1), harkentest::ObjectId(9));
    EXPECT_EQ(1u, Snap().parkedVoiceCount);
    EXPECT_EQ(0u, engine->voices.ActiveCount()); // render slot freed

    // The pool "held" one voice; parked, it now admits a new play.
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle fresh = 0;
    EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &fresh));
    EXPECT_NE(nullptr, engine->voices.Resolve(RenderHandle(engine, fresh)));
    // The parked handle is still valid: it classifies as Parked, not Stale.
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(parked).kind);
}

// --- only the PARKED notification is visible (internal stop consumed) ---

TEST(Park, OnlyParkedNotificationVisibleInternalStopConsumed) {
    NotificationSink sink;
    VnConfig config = ParkConfig(/*maxVoices=*/2, /*maxParkedVoices=*/2);
    config.onNotification = &SinkCb;
    config.notificationUserData = &sink;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterLeaf(harkentest::ObjectId(1));
    RegisterFarProfile(harkentest::ObjectId(9));

    const VnVoiceHandle handle = PlayAndPark(engine, harkentest::ObjectId(1), harkentest::ObjectId(9));
    // Drain the park notification and the consumed internal stop.
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());

    EXPECT_EQ(1u, sink.Count(VN_NOTIFY_VOICE_PARKED, handle));
    EXPECT_EQ(0u, sink.Count(VN_NOTIFY_VOICE_ENDED, handle));
    EXPECT_EQ(0u, sink.Count(VN_NOTIFY_VOICE_STOPPED_BY_FADE, handle));
}

// --- virtual time advances at pitch ---

TEST(Park, VirtualTimeAdvancesWhileParked) {
    EngineSession session(ParkConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterLeaf(harkentest::ObjectId(1));
    RegisterFarProfile(harkentest::ObjectId(9));
    const VnVoiceHandle handle = PlayAndPark(engine, harkentest::ObjectId(1), harkentest::ObjectId(9));

    // Find the park record for this handle.
    const std::uint32_t ledgerSlot = static_cast<std::uint32_t>(handle);
    ASSERT_TRUE(engine->parkRecords[ledgerSlot].active);
    const double before = engine->parkRecords[ledgerSlot].virtualCursor;

    // Advance several engine blocks; each Vn_Update advances the virtual cursor
    // by blockFrames per block at unity pitch and matched rates.
    engine->nullBackend.Pump(4);
    ASSERT_EQ(VN_OK, Vn_Update());
    const double after = engine->parkRecords[ledgerSlot].virtualCursor;
    EXPECT_GT(after, before);
    // ~4 blocks * 256 frames advanced (unity pitch, source rate == engine rate).
    EXPECT_NEAR(static_cast<double>(4 * kBlock), after - before,
                static_cast<double>(kBlock));
}

// --- return resumes at the virtual position ---

TEST(Park, ReturnResumesAtVirtualOffset) {
    NotificationSink sink;
    VnConfig config = ParkConfig();
    config.onNotification = &SinkCb;
    config.notificationUserData = &sink;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterLeaf(harkentest::ObjectId(1));
    RegisterFarProfile(harkentest::ObjectId(9));
    const VnVoiceHandle handle = PlayAndPark(engine, harkentest::ObjectId(1), harkentest::ObjectId(9));
    const std::uint32_t ledgerSlot = static_cast<std::uint32_t>(handle);
    ASSERT_EQ(harken::ResampleProfile::LegacyCompatibility,
              engine->parkRecords[ledgerSlot].facts.resampleProfile);

    // Advance virtual time, then bring the listener close (distance ~1) so the
    // metric rises past the +6 dB hysteresis and the voice returns.
    engine->nullBackend.Pump(4);
    ASSERT_EQ(VN_OK, Vn_Update());
    const std::uint64_t offset = static_cast<std::uint64_t>(
        engine->parkRecords[ledgerSlot].virtualCursor);
    SetListenerX(99.0f); // distance 1 -> ~0 dB -> audible
    ASSERT_EQ(VN_OK, Vn_Update()); // return pass rebinds a render slot
    EXPECT_EQ(1u, sink.Count(VN_NOTIFY_VOICE_RETURNED, handle));

    // The returned voice is live under the same public handle, resuming near the
    // virtual offset (within the documented tolerance).
    harken::VoiceRenderContext* ctx =
        engine->voices.Resolve(RenderHandle(engine, handle));
    ASSERT_NE(nullptr, ctx);
    EXPECT_EQ(harken::ResampleProfile::LegacyCompatibility,
              ctx->resampler.Profile());
    engine->nullBackend.Pump(1); // let it consume from the start frame
    ctx = engine->voices.Resolve(RenderHandle(engine, handle));
    ASSERT_NE(nullptr, ctx);
    EXPECT_GE(ctx->srcCursor, offset);
    EXPECT_LE(ctx->srcCursor, offset + kResumeToleranceFrames);
    EXPECT_EQ(0u, Snap().parkedVoiceCount);
}

TEST(Park, VoiceSidechainKeepsPublicIdentityAcrossDifferentReturnSlot) {
    for (std::uint32_t workers : {0u, 1u, 2u, 4u}) {
    SCOPED_TRACE(::testing::Message()
                 << "renderWorkerCount=" << workers);
    VnConfig config =
        ParkConfig(/*maxVoices=*/3, /*maxParkedVoices=*/1);
    config.renderWorkerCount = workers;
    config.fxSlotsPerBus = 1;
    config.compressorEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterLeaf(harkentest::ObjectId(1));
    RegisterLeaf(harkentest::ObjectId(2));
    RegisterFarProfile(harkentest::ObjectId(9));

    constexpr VnBusId kTargetBus = harkentest::ObjectId(91);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kTargetBus, VN_BUS_MASTER));
    VnCompressorDesc compressor{};
    compressor.structSize = sizeof compressor;
    compressor.thresholdDb = -40.0f;
    compressor.ratio = 20.0f;
    compressor.attackMs = 0.1f;
    compressor.releaseMs = 100.0f;
    ASSERT_EQ(
        VN_OK,
        Vn_SetBusEffect(
            kTargetBus, 0, VN_EFFECT_COMPRESSOR,
            reinterpret_cast<const VnEffectDesc*>(&compressor)));

    SetListenerX(99.0f);
    VnPlayDesc sourcePlay =
        MakePlayDesc3D(harkentest::ObjectId(1), 100.0f, 0.0f, 0.0f);
    VnVoiceHandle source = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&sourcePlay, &source));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(source, harkentest::ObjectId(9)));
    const VnVoiceHandle firstRender = RenderHandle(engine, source);
    ASSERT_NE(0u, firstRender);

    VnPlayDesc targetPlay = MakePlayDesc(harkentest::ObjectId(2));
    targetPlay.busId = kTargetBus;
    VnVoiceHandle target = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&targetPlay, &target));

    VnEffectDynamicRouteDesc route{};
    route.structSize = sizeof route;
    route.signalForm = VN_DYNAMIC_SIGNAL_RAW_AUDIO;
    route.sourceKind = VN_DYNAMIC_SOURCE_VOICE;
    route.tap = VN_DYNAMIC_TAP_VOICE_POST_GAIN_PRE_SPATIAL;
    route.sourceVoiceHandle = source;
    route.expectedEffectType = VN_EFFECT_COMPRESSOR;
    ASSERT_EQ(VN_OK,
              Vn_SetBusEffectDynamicRoute(kTargetBus, 0, 0, &route));

    engine->nullBackend.Pump(2);
    std::uint32_t required = 0;
    VnEffectDynamicRouteInfo status{};
    status.structSize = sizeof status;
    ASSERT_EQ(VN_OK,
              Vn_CaptureEffectDynamicRoutes(&status, 1, &required));
    ASSERT_EQ(1u, required);
    EXPECT_NE(0u, status.stateFlags & VN_DYNAMIC_ROUTE_ACTIVE);

    SetListenerX(0.0f);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(source).kind);
    status = {};
    status.structSize = sizeof status;
    ASSERT_EQ(VN_OK,
              Vn_CaptureEffectDynamicRoutes(&status, 1, &required));
    EXPECT_NE(0u, status.stateFlags & VN_DYNAMIC_ROUTE_STALE);
    EXPECT_EQ(VN_DYNAMIC_ROUTE_INACTIVE_SOURCE_STALE,
              status.inactiveReason);

    VnPlayDesc blockerPlay = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle blocker = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&blockerPlay, &blocker));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(static_cast<std::uint32_t>(firstRender),
              static_cast<std::uint32_t>(
                  RenderHandle(engine, blocker)));
    status = {};
    status.structSize = sizeof status;
    ASSERT_EQ(VN_OK,
              Vn_CaptureEffectDynamicRoutes(&status, 1, &required));
    EXPECT_NE(0u, status.stateFlags & VN_DYNAMIC_ROUTE_STALE)
        << "a different public Voice reusing the old render slot must not "
           "feed the parked Voice route";

    SetListenerX(99.0f);
    ASSERT_EQ(VN_OK, Vn_Update());
    const VnVoiceHandle returnedRender = RenderHandle(engine, source);
    ASSERT_NE(0u, returnedRender);
    EXPECT_NE(static_cast<std::uint32_t>(firstRender),
              static_cast<std::uint32_t>(returnedRender));

    engine->nullBackend.Pump(1);
    status = {};
    status.structSize = sizeof status;
    ASSERT_EQ(VN_OK,
              Vn_CaptureEffectDynamicRoutes(&status, 1, &required));
    EXPECT_NE(0u, status.stateFlags & VN_DYNAMIC_ROUTE_FIRST_BLOCK);

    engine->nullBackend.Pump(1);
    status = {};
    status.structSize = sizeof status;
    ASSERT_EQ(VN_OK,
              Vn_CaptureEffectDynamicRoutes(&status, 1, &required));
    EXPECT_NE(0u, status.stateFlags & VN_DYNAMIC_ROUTE_ACTIVE);
    EXPECT_EQ(source, status.sourceVoiceHandle);
    }
}

// --- hysteresis prevents flapping ---

TEST(Park, HysteresisKeepsAVoiceParkedWithinTheBand) {
    EngineSession session(ParkConfig(/*maxVoices=*/4, /*maxParkedVoices=*/4,
                                     /*thresholdDb=*/-40.0f));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterLeaf(harkentest::ObjectId(1));
    RegisterFarProfile(harkentest::ObjectId(9));
    const VnVoiceHandle handle = PlayAndPark(engine, harkentest::ObjectId(1), harkentest::ObjectId(9));
    ASSERT_EQ(1u, Snap().parkedVoiceCount);

    // Move the listener so the metric sits inside the hysteresis band (above the
    // park threshold -40 but below the +6 return point -34): distance ~98.6
    // gives about -37 dB. It must NOT return.
    SetListenerX(1.4f); // distance 98.6 -> ~-37 dB
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(1u, Snap().parkedVoiceCount); // stayed parked (no flap)
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(1u, Snap().parkedVoiceCount);

    // Past the +6 point: it returns.
    SetListenerX(99.0f); // distance 1 -> ~0 dB
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(0u, Snap().parkedVoiceCount);
    EXPECT_NE(nullptr, engine->voices.Resolve(RenderHandle(engine, handle)));
}

// --- one-shot ends while parked ---

TEST(Park, OneShotEndsWhileParked) {
    NotificationSink sink;
    VnConfig config = ParkConfig();
    config.onNotification = &SinkCb;
    config.notificationUserData = &sink;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    // A short one-shot: long enough to still be playing when parked (so it can
    // park), short enough that virtual time reaches its end within a few pumps.
    RegisterLeaf(harkentest::ObjectId(1), /*frames=*/kBlock * 3);
    RegisterFarProfile(harkentest::ObjectId(9));
    const VnVoiceHandle handle = PlayAndPark(engine, harkentest::ObjectId(1), harkentest::ObjectId(9));
    const std::uint32_t ledgerSlot = static_cast<std::uint32_t>(handle);
    ASSERT_TRUE(engine->parkRecords[ledgerSlot].active);

    // Advance well past the source length; the parked one-shot virtually ends.
    engine->nullBackend.Pump(4);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(1u, sink.Count(VN_NOTIFY_VOICE_ENDED, handle));
    EXPECT_EQ(0u, Snap().parkedVoiceCount);
    // The handle is now stale.
    EXPECT_EQ(0u, engine->ledger.ToRender(handle));
}

// --- loop wrap while parked ---

TEST(Park, LoopWrapsWhileParked) {
    EngineSession session(ParkConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterLeaf(harkentest::ObjectId(1), /*frames=*/kBlock * 2); // 512-frame source
    RegisterFarProfile(harkentest::ObjectId(9));

    // An infinite loop over the whole file.
    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 100.0f, 0.0f, 0.0f);
    play.loopCount = 0; // infinite
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, harkentest::ObjectId(9)));
    SetListenerX(0.0f);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    const std::uint32_t ledgerSlot = static_cast<std::uint32_t>(handle);
    ASSERT_TRUE(engine->parkRecords[ledgerSlot].active);

    // Advance far past the loop end; the virtual cursor stays inside the file.
    engine->nullBackend.Pump(8); // 2048 frames >> 512
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_TRUE(engine->parkRecords[ledgerSlot].active); // still parked, looping
    EXPECT_LT(engine->parkRecords[ledgerSlot].virtualCursor,
              static_cast<double>(kBlock * 2)); // wrapped into [0, 512)
    EXPECT_GE(engine->parkRecords[ledgerSlot].virtualCursor, 0.0);
}

// --- stop reaches a parked voice ---

TEST(Park, StopReachesParkedVoice) {
    NotificationSink sink;
    VnConfig config = ParkConfig();
    config.onNotification = &SinkCb;
    config.notificationUserData = &sink;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterLeaf(harkentest::ObjectId(1));
    RegisterFarProfile(harkentest::ObjectId(9));
    const VnVoiceHandle handle = PlayAndPark(engine, harkentest::ObjectId(1), harkentest::ObjectId(9));
    ASSERT_EQ(1u, Snap().parkedVoiceCount);

    // Stop the parked voice: it ends its ledger entry with no render involvement.
    EXPECT_EQ(VN_OK, Vn_Stop(handle, 0));
    EXPECT_EQ(0u, Snap().parkedVoiceCount);
    EXPECT_EQ(0u, engine->ledger.ToRender(handle)); // handle stale
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(1u, sink.Count(VN_NOTIFY_VOICE_ENDED, handle));
}

// --- return re-applies captured orientation (detached cone voice) ---

TEST(Park, ReturnReAppliesCapturedOrientation) {
    EngineSession session(ParkConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterLeaf(harkentest::ObjectId(1));
    // A cone profile: the cone term reads the voice's orientation, so a returning
    // voice that reverted to the default forward would render a wrong cone
    // attenuation — the degradation this guards against.
    VnAttenuationDesc cone = MakeAttenuationDesc(1.0f, 100.0f);
    cone.coneEnabled = 1;
    cone.coneInnerDeg = 30.0f;
    cone.coneOuterDeg = 90.0f;
    cone.coneOuterGainDb = -24.0f;
    cone.coneOuterLowpassHz = 20000.0f; // pass-through: cone validation requires >0
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(10), &cone));

    // Play a detached spatialized voice, orient it away from the origin along +X,
    // then park it via distance.
    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 100.0f, 0.0f, 0.0f);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, harkentest::ObjectId(10)));
    ASSERT_EQ(VN_OK, Vn_SetVoiceOrientation(handle, Vec3(1.0f, 0.0f, 0.0f)));
    SetListenerX(0.0f);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update()); // parks
    engine->nullBackend.Pump(1);
    const std::uint32_t ledgerSlot = static_cast<std::uint32_t>(handle);
    ASSERT_TRUE(engine->parkRecords[ledgerSlot].active);
    // The captured orientation is the away vector, not the default forward.
    const float capturedX = engine->parkRecords[ledgerSlot].spatial.fwdX;
    const float capturedY = engine->parkRecords[ledgerSlot].spatial.fwdY;
    const float capturedZ = engine->parkRecords[ledgerSlot].spatial.fwdZ;
    EXPECT_NEAR(1.0f, capturedX, 1e-5f);

    // Approach so the voice returns; it must render with the captured
    // orientation, not the (0,0,-1) default OnVoiceStarted seeds.
    SetListenerX(99.0f);
    ASSERT_EQ(VN_OK, Vn_Update()); // returns
    harken::Spatializer::SpatialVoiceState st;
    ASSERT_TRUE(engine->spatial.CaptureVoice(RenderHandle(engine, handle), &st));
    EXPECT_NEAR(capturedX, st.fwdX, 1e-5f);
    EXPECT_NEAR(capturedY, st.fwdY, 1e-5f);
    EXPECT_NEAR(capturedZ, st.fwdZ, 1e-5f);
    EXPECT_NEAR(1.0f, st.fwdX, 1e-5f); // concretely the away vector
    EXPECT_NEAR(0.0f, st.fwdZ, 1e-5f);
}

// --- live finite loops park with coherent render authority ---

TEST(Park, LiveFiniteUsesRealMirrorAndReturnsRemainingPasses) {
    NotificationSink sink;
    VnConfig config = ParkConfig();
    config.onNotification = &SinkCb;
    config.notificationUserData = &sink;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterLeaf(harkentest::ObjectId(1), /*frames=*/kBlock * 2);
    RegisterFarProfile(harkentest::ObjectId(9));
    SetListenerX(99.0f);

    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 100.0f, 0.0f, 0.0f);
    constexpr std::uint32_t kAuthoredPasses = 64;
    play.loopCount = kAuthoredPasses;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, harkentest::ObjectId(9)));
    ASSERT_EQ(VN_OK, Vn_Update());

    const VnVoiceHandle render = RenderHandle(engine, handle);
    ASSERT_NE(0u, render);
    const std::uint32_t slot = static_cast<std::uint32_t>(render);
    const std::uint32_t generation =
        static_cast<std::uint32_t>(render >> 32);

    std::vector<harken::SourceCursorMirror::Row> rows(
        engine->sourceCursors.SlotCount());
    harken::SourceCursorMirror::Row crossedRow{};
    std::uint64_t crossedBlock = 0;
    bool crossed = false;
    for (int i = 0; i < 8 && !crossed; ++i) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
        ASSERT_TRUE(
            engine->sourceCursors.Capture(rows.data(), &crossedBlock));
        const harken::SourceCursorMirror::Row& row = rows[slot];
        if (row.active && row.renderGeneration == generation &&
            row.loopPassesRemaining > 0u &&
            row.loopPassesRemaining < kAuthoredPasses) {
            crossedRow = row;
            crossed = true;
        }
    }
    ASSERT_TRUE(crossed);
    ASSERT_TRUE(crossedRow.active);
    ASSERT_EQ(generation, crossedRow.renderGeneration);
    ASSERT_GT(crossedRow.loopPassesRemaining, 0u);
    ASSERT_LT(crossedRow.loopPassesRemaining, kAuthoredPasses);
    harken::VoiceRenderContext* crossedCtx =
        engine->voices.Resolve(render);
    ASSERT_NE(nullptr, crossedCtx);
    ASSERT_EQ(crossedRow.loopPassesRemaining,
              crossedCtx->loopPassesRemaining);

    auto* const useCount =
        engine->voicePlayFacts[slot].view.useCount;
    ASSERT_NE(nullptr, useCount);
    ASSERT_EQ(1u, useCount->load(std::memory_order_acquire));

    SetListenerX(0.0f);
    const std::uint64_t deferredBefore = engine->parkDeferredCount;
    engine->serviceFrameClock = (crossedBlock + 2u) * kBlock;
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Live,
              engine->ledger.Classify(handle).kind);
    EXPECT_EQ(render, RenderHandle(engine, handle));
    EXPECT_EQ(deferredBefore + 1u, engine->parkDeferredCount);

    engine->nullBackend.Pump(3);
    std::uint64_t freshBlock = 0;
    ASSERT_TRUE(engine->sourceCursors.Capture(rows.data(), &freshBlock));
    const harken::SourceCursorMirror::Row freshRow = rows[slot];
    ASSERT_TRUE(freshRow.active);
    ASSERT_EQ(generation, freshRow.renderGeneration);
    ASSERT_GT(freshRow.loopPassesRemaining, 0u);
    ASSERT_LT(freshRow.loopPassesRemaining, kAuthoredPasses);
    harken::VoiceRenderContext* freshCtx =
        engine->voices.Resolve(render);
    ASSERT_NE(nullptr, freshCtx);
    ASSERT_EQ(freshRow.loopPassesRemaining,
              freshCtx->loopPassesRemaining);

    const std::uint32_t commandRoom = engine->commands.FreeSlots();
    const std::uint64_t parkedCountBefore = engine->voiceParkedCount;
    const std::uint64_t parkedHighWaterBefore = engine->parkedHighWater;
    harken::detail::ArmLogicAllocFaultForTests(
        1, harken::detail::LogicAllocSite::ParkCaptureStorage);
    const VnResult failedUpdate = Vn_Update();
    harken::detail::DisarmLogicAllocFaultForTests();
    ASSERT_EQ(VN_OK, failedUpdate);
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Live,
              engine->ledger.Classify(handle).kind);
    EXPECT_EQ(render, RenderHandle(engine, handle));
    EXPECT_FALSE(engine->parkRecords[static_cast<std::uint32_t>(handle)].active);
    EXPECT_EQ(commandRoom, engine->commands.FreeSlots());
    EXPECT_EQ(1u, useCount->load(std::memory_order_acquire));
    EXPECT_EQ(parkedCountBefore, engine->voiceParkedCount);
    EXPECT_EQ(parkedHighWaterBefore, engine->parkedHighWater);
    EXPECT_EQ(0u, sink.Count(VN_NOTIFY_VOICE_PARKED, handle));

    ASSERT_EQ(VN_OK, Vn_Update());
    const harken::VoiceLedger::Resolved parked =
        engine->ledger.Classify(handle);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked, parked.kind);
    harken::ParkRecord& record = engine->parkRecords[parked.ledgerSlot];
    EXPECT_DOUBLE_EQ(static_cast<double>(freshRow.sourceFrame),
                     record.virtualCursor);
    EXPECT_EQ(freshRow.loopPassesRemaining, record.loopsRemaining);
    EXPECT_EQ(kAuthoredPasses, record.facts.loopCount);
    EXPECT_EQ(1u, sink.Count(VN_NOTIFY_VOICE_PARKED, handle));

    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_TRUE(record.active);
    const std::uint32_t returnPasses = record.loopsRemaining;
    SetListenerX(99.0f);
    ASSERT_EQ(VN_OK, Vn_Update());

    EXPECT_EQ(harken::VoiceLedger::HandleKind::Live,
              engine->ledger.Classify(handle).kind);
    EXPECT_EQ(handle,
              engine->ledger.PublicHandleForSlot(parked.ledgerSlot));
    EXPECT_EQ(1u, sink.Count(VN_NOTIFY_VOICE_RETURNED, handle));
    const VnVoiceHandle returnedRender = RenderHandle(engine, handle);
    harken::VoiceRenderContext* ctx =
        engine->voices.Resolve(returnedRender);
    ASSERT_NE(nullptr, ctx);
    EXPECT_EQ(returnPasses, ctx->loopPassesRemaining);
    const std::uint32_t returnedSlot =
        static_cast<std::uint32_t>(returnedRender);
    ASSERT_LT(returnedSlot, engine->voicePlayFacts.size());
    EXPECT_EQ(kAuthoredPasses,
              engine->voicePlayFacts[returnedSlot].loopCount);
}

// --- due PCM direct parking does not depend on Render capacity ---







TEST(Park, FiniteTimelineKeepsUnitSpanAboveDoubleIntegerPrecision) {
    EngineSession session(ParkConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterLeaf(harkentest::ObjectId(1));
    RegisterFarProfile(harkentest::ObjectId(9));

    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 100.0f, 0.0f, 0.0f);
    play.loopCount = 3;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, harkentest::ObjectId(9)));
    SetListenerX(0.0f);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());

    const harken::VoiceLedger::Resolved parked =
        engine->ledger.Classify(handle);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked, parked.kind);
    harken::ParkRecord& record = engine->parkRecords[parked.ledgerSlot];

    constexpr std::uint64_t kLoopStart =
        (std::uint64_t{1} << 53) + 4u;
    constexpr std::uint64_t kLoopEnd = kLoopStart + 1u;
    record.facts.loopStartFrame = kLoopStart;
    record.facts.loopEndFrame = kLoopEnd;
    record.facts.sourceFrameCount = kLoopEnd + 16u;
    record.virtualCursor = static_cast<double>(kLoopStart);
    record.loopsRemaining = 3u;
    record.lastAdvanceFrame = engine->serviceFrameClock;

    ++engine->serviceFrameClock;
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_TRUE(record.active);
    EXPECT_EQ(2u, record.loopsRemaining);
    EXPECT_TRUE(std::isfinite(record.virtualCursor));
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(handle).kind);
    EXPECT_EQ(handle,
              engine->ledger.PublicHandleForSlot(parked.ledgerSlot));
}




// --- a steal-deferred start arriving inaudible parks directly ---

TEST(Park, PendingStartArrivingInaudibleParksDirectly) {
    NotificationSink sink;
    // One-voice pool, steal-oldest: a second play steals the incumbent and its
    // start defers behind the freed slot.
    VnConfig config = ParkConfig(/*maxVoices=*/1, /*maxParkedVoices=*/1);
    config.voiceStealPolicy = 2; // steal-oldest
    config.onNotification = &SinkCb;
    config.notificationUserData = &sink;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterLeaf(harkentest::ObjectId(1));
    RegisterFarProfile(harkentest::ObjectId(9));

    // The incumbent occupies the only render slot.
    VnPlayDesc nearPlay = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle incumbent = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&nearPlay, &incumbent));
    engine->nullBackend.Pump(1);

    // A far 3D voice steals the incumbent and becomes pending; bound to the far
    // profile with the listener at the origin, its would-be metric is inaudible.
    VnPlayDesc farPlay = MakePlayDesc3D(harkentest::ObjectId(1), 100.0f, 0.0f, 0.0f);
    VnVoiceHandle deferred = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&farPlay, &deferred));
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Pending,
              engine->ledger.Classify(deferred).kind);
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(deferred, harkentest::ObjectId(9)));
    ASSERT_EQ(VN_OK, Vn_SetVoiceSpatializationMix(deferred, 0.25f));
    SetListenerX(0.0f);

    // Direct parking remains behind the already-committed victim gate. Before
    // the audio thread applies the declick stop, the accepted voice stays
    // Pending and does not publish PARKED out of order.
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Pending,
              engine->ledger.Classify(deferred).kind);
    EXPECT_EQ(0u, engine->ledger.ParkedCount());
    EXPECT_EQ(1u, engine->voices.ActiveCount());
    EXPECT_EQ(0u, sink.Count(VN_NOTIFY_VOICE_PARKED, deferred));

    // The victim's declick frees the slot; the deferred start then parks directly
    // rather than compiling a render Play.
    engine->nullBackend.Pump(1);   // victim declick -> slot frees
    const std::uint32_t freeCommands = engine->commands.FreeSlots();
    ASSERT_EQ(VN_OK, Vn_Update()); // deferred start -> direct park

    EXPECT_EQ(1u, Snap().parkedVoiceCount);
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(deferred).kind);
    EXPECT_EQ(0u, engine->voices.ActiveCount()); // no render slot consumed
    EXPECT_EQ(freeCommands, engine->commands.FreeSlots());
    EXPECT_FLOAT_EQ(0.25f,
                    engine->parkRecords[static_cast<std::uint32_t>(deferred)]
                        .spatial.spatializationMix);
    EXPECT_EQ(1u, sink.Count(VN_NOTIFY_VOICE_PARKED, deferred));
    EXPECT_EQ(0u, sink.Count(VN_NOTIFY_VOICE_ENDED, deferred));

    // The freed slot admits a third play — proof the parked voice holds none.
    VnPlayDesc thirdPlay = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle fresh = 0;
    EXPECT_EQ(VN_OK, harkentest::PlayRaw(&thirdPlay, &fresh));
    EXPECT_NE(nullptr, engine->voices.Resolve(RenderHandle(engine, fresh)));

    // Free the slot again so the parked voice has somewhere to return to.
    ASSERT_EQ(VN_OK, Vn_Stop(fresh, 0));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());

    // Approaching the listener returns the directly-parked voice normally.
    SetListenerX(99.0f); // distance 1 -> audible
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(1u, sink.Count(VN_NOTIFY_VOICE_RETURNED, deferred));
    EXPECT_EQ(0u, Snap().parkedVoiceCount);
    const VnVoiceHandle render = RenderHandle(engine, deferred);
    EXPECT_NE(nullptr, engine->voices.Resolve(render));
    harken::Spatializer::SpatialVoiceState spatial;
    ASSERT_TRUE(engine->spatial.CaptureVoice(render, &spatial));
    EXPECT_FLOAT_EQ(0.25f, spatial.spatializationMix);
}

enum class AttenuationReplacementPhase { Pending, Parked, Live };

class ParkedAttenuationGeneration
    : public testing::TestWithParam<AttenuationReplacementPhase> {};

TEST_P(ParkedAttenuationGeneration,
       ReplacementAndRemovalPreserveAudibilityOutputAndRetirement) {
    const auto phase = GetParam();
    VnConfig config = ParkConfig(/*maxVoices=*/1, /*maxParkedVoices=*/1);
    config.voiceStealPolicy = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const auto sound = harkentest::ObjectId(1);
    const auto attenuation = harkentest::ObjectId(9);
    RegisterLeaf(sound);

    auto oldProfile = MakeAttenuationDesc(1.0f, 101.0f);
    oldProfile.highpassCurve.enabled = 1;
    oldProfile.highpassCurve.pointCount = 1;
    oldProfile.highpassCurve.points[0] = {0, 1000};
    const harken::Spatializer::PackProfileInput oldInput{attenuation, &oldProfile};
    harken::Spatializer::PreparedProfileSet first;
    ASSERT_EQ(VN_OK,
              engine->spatial.PreparePackProfiles(&oldInput, 1, 1, &first));
    engine->spatial.CommitPreparedSet(&first);

    const auto replaceProfile = [&] {
        const auto nextProfile = MakeAttenuationDesc(1.0f, 201.0f);
        const harken::Spatializer::PackProfileInput nextInput{
            attenuation, &nextProfile};
        harken::Spatializer::PreparedProfileSet replacement;
        ASSERT_EQ(VN_OK, engine->spatial.PreparePackProfiles(
                             &nextInput, 1, 2, &replacement, {{attenuation, 1}}));
        engine->spatial.CommitPreparedSet(&replacement);
    };
    const auto setListener = [&](float z) {
        ASSERT_EQ(VN_OK, Vn_SetListener(Vec3(0.0f, 0.0f, z),
                                      Vec3(0.0f, 0.0f, -1.0f),
                                      Vec3(0.0f, 1.0f, 0.0f)));
    };
    const auto expectOutput = [&](float expected) {
        const auto before = engine->mixer.BlocksRendered();
        harkentest::Capture capture;
        harkentest::PumpAndCapture(engine, 1, capture);
        ASSERT_EQ(before + 1, engine->mixer.BlocksRendered());
        ASSERT_EQ(kBlock, capture.left.size());
        ASSERT_GT(harkentest::Peak(capture.left), harkentest::kSilenceFloor);
        for (std::uint32_t frame = 0; frame < kBlock; ++frame) {
            EXPECT_NEAR(expected, capture.left[frame], 1e-4f) << frame;
            EXPECT_NEAR(expected, capture.right[frame], 1e-4f) << frame;
        }
    };

    VnVoiceHandle incumbent = 0;
    if (phase != AttenuationReplacementPhase::Live) {
        const auto play = MakePlayDesc(sound);
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &incumbent));
        engine->nullBackend.Pump(1);
    } else {
        ASSERT_NO_FATAL_FAILURE(setListener(-50.0f));
    }
    const auto play = MakePlayDesc3D(sound, 0.0f, 0.0f, -101.0f);
    VnVoiceHandle original = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &original));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(original, attenuation));
    if (phase == AttenuationReplacementPhase::Live) {
        ASSERT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(1);
        ASSERT_NO_FATAL_FAILURE(replaceProfile());
        ASSERT_NO_FATAL_FAILURE(setListener(0.0f));
    } else {
        ASSERT_EQ(harken::VoiceLedger::HandleKind::Pending,
                  engine->ledger.Classify(original).kind);
        if (phase == AttenuationReplacementPhase::Pending) {
            ASSERT_NO_FATAL_FAILURE(replaceProfile());
        }
        engine->nullBackend.Pump(1);
    }
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto parked = engine->ledger.Classify(original);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked, parked.kind);
    EXPECT_EQ(1u, engine->parkRecords[parked.ledgerSlot]
                      .spatial.attenuationGeneration);
    engine->nullBackend.Pump(1);
    if (phase == AttenuationReplacementPhase::Parked) {
        ASSERT_NO_FATAL_FAILURE(replaceProfile());
    }
    // At distance 101 only the current curve is audible: old gain 0, new 0.5.
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(original).kind);
    ASSERT_NE(nullptr, engine->spatial.FindProfile(attenuation, 1));

    VnVoiceHandle fresh = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &fresh));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(fresh, attenuation));
    ASSERT_EQ(VN_OK, Vn_Update());
    harken::Spatializer::SpatialVoiceState freshSpatial{};
    ASSERT_TRUE(engine->spatial.CaptureVoice(RenderHandle(engine, fresh),
                                             &freshSpatial));
    EXPECT_EQ(2u, freshSpatial.attenuationGeneration);
    EXPECT_FLOAT_EQ(0, engine->params.ResolvedValue(
        RenderHandle(engine, fresh), harken::kTargetVoiceHighpassCutoff));
    ASSERT_NO_FATAL_FAILURE(expectOutput(0.25f));
    ASSERT_EQ(VN_OK, Vn_Stop(fresh, 0));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());

    harken::Spatializer::PreparedProfileSet removed;
    ASSERT_EQ(VN_OK, engine->spatial.PreparePackProfiles(
                         nullptr, 0, 0, &removed, {{attenuation, 1}}));
    engine->spatial.CommitPreparedSet(&removed);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(nullptr, engine->spatial.FindProfile(attenuation));
    ASSERT_NE(nullptr, engine->spatial.FindProfile(attenuation, 1));
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(original).kind);

    // At distance 51 the retained curve has gain 0.5, not the replaced 0.75.
    ASSERT_NO_FATAL_FAILURE(setListener(-50.0f));
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Live,
              engine->ledger.Classify(original).kind);
    harken::Spatializer::SpatialVoiceState restored{};
    ASSERT_TRUE(engine->spatial.CaptureVoice(RenderHandle(engine, original),
                                             &restored));
    EXPECT_EQ(1u, restored.attenuationGeneration);
    const auto restoredHandle = RenderHandle(engine, original);
    EXPECT_FLOAT_EQ(1000, engine->params.ResolvedValue(
        restoredHandle, harken::kTargetVoiceHighpassCutoff));
    harkentest::Capture filtered;
    harkentest::PumpAndCapture(engine, 8, filtered);
    const auto* restoredContext = engine->voices.Resolve(restoredHandle);
    ASSERT_NE(nullptr, restoredContext);
    EXPECT_TRUE(restoredContext->hpfEnabled);
    ASSERT_GT(harkentest::Peak(filtered.left), harkentest::kSilenceFloor);
    for (std::size_t frame = filtered.left.size() - kBlock;
         frame < filtered.left.size(); ++frame) {
        EXPECT_NEAR(0, filtered.left[frame], 1e-5f) << frame;
        EXPECT_NEAR(0, filtered.right[frame], 1e-5f) << frame;
    }

    ASSERT_EQ(VN_OK, Vn_Stop(original, 0));
    for (unsigned step = 0; step < 4; ++step) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    EXPECT_EQ(nullptr, engine->spatial.FindProfile(attenuation, 1));
    EXPECT_EQ(0u, engine->spatial.RetainedProfileCount());
}

TEST_P(ParkedAttenuationGeneration,
       DistanceReferenceDrivesAdmissionParkingReturnAndDestroyedFallback) {
    const auto phase = GetParam();
    auto config = ParkConfig(1, 1);
    config.voiceStealPolicy = 2;
    config.maxEmitters = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    const auto sound = harkentest::ObjectId(1);
    const auto attenuation = harkentest::ObjectId(9);
    RegisterLeaf(sound);
    RegisterFarProfile(attenuation);

    harken::PackLogicStore::PreparedClaimToken token;
    ASSERT_EQ(VN_OK, engine->logicPacks.PrepareClaim(1, &token));
    harken::PackLogicRecord content;
    harken::PackLogicImage::VoiceRecipe recipe;
    recipe.header.playableId = sound;
    recipe.spatial.attenuationId = attenuation;
    recipe.spatial.distanceScale = 1;
    content.voiceRecipes.push_back(std::move(recipe));
    engine->logicPacks.CommitPreparedClaim(&token, std::move(content));
    const auto setListener = [&](float z) {
        ASSERT_EQ(VN_OK, Vn_SetListener(Vec3(0, 0, z), Vec3(0, 0, -1), Vec3(0, 1, 0)));
    };
    ASSERT_NO_FATAL_FAILURE(setListener(-99));
    VnEmitterHandle reference = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&reference));
    ASSERT_EQ(VN_OK, Vn_SetEmitterPosition(reference, Vec3(0, 0,
        phase == AttenuationReplacementPhase::Parked ? 0.0f : -99.0f)));
    ASSERT_EQ(VN_OK, Vn_SetListenerDistanceReference(reference));
    if (phase == AttenuationReplacementPhase::Pending) {
        const auto play = MakePlayDesc(sound);
        VnVoiceHandle incumbent = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &incumbent));
        engine->nullBackend.Pump(1);
    }
    const auto play = MakePlayDesc3D(sound, 0, 0, -100);
    VnVoiceHandle voice = 0;
    const auto commandsBefore = engine->commands.FreeSlots();
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    if (phase == AttenuationReplacementPhase::Parked) {
        EXPECT_EQ(commandsBefore, engine->commands.FreeSlots());
        ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked,
                  engine->ledger.Classify(voice).kind);
    } else {
        ASSERT_EQ(phase == AttenuationReplacementPhase::Pending
                      ? harken::VoiceLedger::HandleKind::Pending
                      : harken::VoiceLedger::HandleKind::Live,
                  engine->ledger.Classify(voice).kind);
        ASSERT_EQ(VN_OK, Vn_SetEmitterPosition(reference, Vec3(0, 0, 0)));
        engine->nullBackend.Pump(1);
    }
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(voice).kind);
    engine->nullBackend.Pump(1);
    ASSERT_NO_FATAL_FAILURE(setListener(99));
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(voice).kind);

    const auto returnAndCapture = [&] {
        ASSERT_EQ(VN_OK, Vn_Update());
        ASSERT_EQ(harken::VoiceLedger::HandleKind::Live,
                  engine->ledger.Classify(voice).kind);
        const auto before = engine->mixer.BlocksRendered();
        harkentest::Capture capture;
        harkentest::PumpAndCapture(engine, 1, capture);
        ASSERT_EQ(before + 1, engine->mixer.BlocksRendered());
        ASSERT_EQ(kBlock, capture.left.size());
        for (std::uint32_t frame = 0; frame < kBlock; ++frame) {
            EXPECT_NEAR(0.5f, capture.left[frame], 1e-4f) << frame;
            EXPECT_NEAR(0.5f, capture.right[frame], 1e-4f) << frame;
        }
    };
    ASSERT_EQ(VN_OK, Vn_SetEmitterPosition(reference, Vec3(0, 0, -99)));
    ASSERT_NO_FATAL_FAILURE(returnAndCapture());
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(reference));
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(voice).kind);
    engine->nullBackend.Pump(1);
    VnEmitterHandle replacement = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&replacement));
    EXPECT_EQ(static_cast<std::uint32_t>(reference), static_cast<std::uint32_t>(replacement));
    EXPECT_NE(reference, replacement);
    ASSERT_EQ(VN_OK, Vn_SetEmitterPosition(replacement, Vec3(0, 0, -99)));
    EXPECT_EQ(VN_STALE_HANDLE, Vn_SetListenerDistanceReference(reference));
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(voice).kind);
    ASSERT_EQ(VN_OK, Vn_SetEmitterPosition(replacement, Vec3(0, 0, 0)));
    ASSERT_NO_FATAL_FAILURE(setListener(-99));
    ASSERT_NO_FATAL_FAILURE(returnAndCapture());
}

INSTANTIATE_TEST_SUITE_P(
    Park, ParkedAttenuationGeneration,
    testing::Values(AttenuationReplacementPhase::Pending,
                    AttenuationReplacementPhase::Parked,
                    AttenuationReplacementPhase::Live),
    [](const testing::TestParamInfo<AttenuationReplacementPhase>& info) {
        switch (info.param) {
        case AttenuationReplacementPhase::Pending: return "Pending";
        case AttenuationReplacementPhase::Parked: return "Parked";
        case AttenuationReplacementPhase::Live: return "Live";
        }
        return "Unknown";
    });

// --- virtual time advances at the resolved pitch, not the base pitch ---

TEST(Park, VirtualTimeAdvancesAtResolvedPitch) {
    EngineSession session(ParkConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterLeaf(harkentest::ObjectId(1));
    RegisterFarProfile(harkentest::ObjectId(9));
    // Play, set an API-direct pitch of 2.0 (the combine-resolved pitch), then
    // park via distance.
    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 100.0f, 0.0f, 0.0f);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, harkentest::ObjectId(9)));
    ASSERT_EQ(VN_OK, Vn_SetVoicePitch(handle, 2.0f));
    SetListenerX(0.0f);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update()); // parks; captures the resolved pitch
    engine->nullBackend.Pump(1);
    const std::uint32_t ledgerSlot = static_cast<std::uint32_t>(handle);
    ASSERT_TRUE(engine->parkRecords[ledgerSlot].active);
    EXPECT_NEAR(2.0, engine->parkRecords[ledgerSlot].resolvedPitchRatio, 1e-6);

    const double before = engine->parkRecords[ledgerSlot].virtualCursor;
    engine->nullBackend.Pump(4);
    ASSERT_EQ(VN_OK, Vn_Update());
    const double after = engine->parkRecords[ledgerSlot].virtualCursor;
    // At 2x pitch the virtual cursor advances ~2x the unity-pitch span over the
    // same ~4 blocks (block-quantized slop scales with the pitch too).
    EXPECT_NEAR(static_cast<double>(2 * 4 * kBlock), after - before,
                static_cast<double>(2 * kBlock));
}

// --- return restores the effect chain configuration (fresh DSP state) ---

TEST(Park, ReturnRestoresEffectConfig) {
    VnConfig config = ParkConfig();
    config.fxSlotsPerVoice = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterLeaf(harkentest::ObjectId(1));
    RegisterFarProfile(harkentest::ObjectId(9));

    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 100.0f, 0.0f, 0.0f);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, harkentest::ObjectId(9)));

    // A three-band tone with non-default params; one param is then overridden and
    // the slot is bypassed, so restoration must carry params AND bypass.
    VnThreeBandToneDesc tone{};
    tone.structSize = sizeof(VnThreeBandToneDesc);
    tone.lowShelfGainDb = 6.0f;
    tone.lowShelfFreqHz = 200.0f;
    tone.peakGainDb = -3.0f;
    tone.peakFreqHz = 1000.0f;
    tone.peakQ = 1.5f;
    tone.highShelfGainDb = 2.0f;
    tone.highShelfFreqHz = 8000.0f;
    ASSERT_EQ(VN_OK,
              Vn_SetVoiceEffect(handle, 0, VN_EFFECT_THREE_BAND_TONE,
                                reinterpret_cast<const VnEffectDesc*>(&tone)));
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffectParam(handle, 0,
                                            harken::kToneParamPeakGainDb, -9.0f));
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffectBypass(handle, 0, 1));

    // Park via distance; the effect config is captured into the park record.
    SetListenerX(0.0f);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    const std::uint32_t ledgerSlot = static_cast<std::uint32_t>(handle);
    ASSERT_TRUE(engine->parkRecords[ledgerSlot].active);
    ASSERT_EQ(1u, engine->parkRecords[ledgerSlot].effects.size());

    // Return, then let the audio thread install the re-staged effect.
    SetListenerX(99.0f);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    harken::VoiceRenderContext* ctx =
        engine->voices.Resolve(RenderHandle(engine, handle));
    ASSERT_NE(nullptr, ctx);
    // The slot is populated with the same effect type...
    ASSERT_NE(nullptr, ctx->fxSlots[0].process);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_EFFECT_THREE_BAND_TONE),
              ctx->fxSlotEffectRef[0].type);
    // ...its committed params match (peak gain overridden to -9 dB)...
    auto* toneState =
        static_cast<harken::ToneEffectState*>(ctx->fxSlots[0].state);
    ASSERT_NE(nullptr, toneState);
    EXPECT_FLOAT_EQ(6.0f, toneState->lowShelfGainDb);
    EXPECT_FLOAT_EQ(-9.0f, toneState->peakGainDb);
    EXPECT_FLOAT_EQ(1.5f, toneState->peakQ);
    EXPECT_FLOAT_EQ(2.0f, toneState->highShelfGainDb);
    // ...and it is bypassed (fully dry), as captured.
    EXPECT_FLOAT_EQ(1.0f, ctx->fxBypassTarget[0]);
}

TEST(Park, ReturnEffectPreparationFailureIsAtomicAndRetryable) {
    VnConfig config = ParkConfig();
    config.fxSlotsPerVoice = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterLeaf(harkentest::ObjectId(1));
    RegisterFarProfile(harkentest::ObjectId(9));

    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 100.0f, 0.0f, 0.0f);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, harkentest::ObjectId(9)));
    VnThreeBandToneDesc tone{};
    tone.structSize = sizeof(VnThreeBandToneDesc);
    tone.lowShelfGainDb = 3.0f;
    tone.lowShelfFreqHz = 200.0f;
    tone.peakGainDb = -2.0f;
    tone.peakFreqHz = 1000.0f;
    tone.peakQ = 1.0f;
    tone.highShelfGainDb = 1.0f;
    tone.highShelfFreqHz = 8000.0f;
    ASSERT_EQ(VN_OK,
              Vn_SetVoiceEffect(handle, 0, VN_EFFECT_THREE_BAND_TONE,
                                reinterpret_cast<const VnEffectDesc*>(&tone)));

    SetListenerX(0.0f);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    const std::uint32_t ledgerSlot = static_cast<std::uint32_t>(handle);
    harken::ParkRecord& record = engine->parkRecords[ledgerSlot];
    ASSERT_TRUE(record.active);

    SetListenerX(99.0f);
    const std::uint32_t commandRoom = engine->commands.FreeSlots();
    engine->effectParamTargets.SetPrepareSeedFailureAfterForTests(0);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->effectParamTargets.SetPrepareSeedFailureAfterForTests(-1);
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(handle).kind);
    EXPECT_FALSE(record.returnEffectsPrepared);
    EXPECT_EQ(0u, record.returnEffectCount);
    EXPECT_EQ(commandRoom, engine->commands.FreeSlots());

    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Live,
              engine->ledger.Classify(handle).kind);
}

// --- an authored tone preset survives park and return ---

TEST(Park, ReturnRestoresAuthoredTonePreset) {
    VnConfig config = ParkConfig();
    config.fxSlotsPerVoice = 1;
    config.gainEffectPoolCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    // A recipe authoring ALL FOUR tone sections with non-default values:
    // restoration is asserted per section member, so a return that falls
    // back to any default is caught on that target.
    const harkentest::RecipePackArtifact artifact =
        harkentest::BuildFourSectionToneRecipePack("park_tone_return");
    ASSERT_TRUE(artifact.built);
    VnPackDesc packDesc{};
    packDesc.structSize = sizeof(VnPackDesc);
    packDesc.utf8Path = artifact.path.c_str();
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, Vn_LoadPack(&packDesc, &pack));
    RegisterFarProfile(harkentest::ObjectId(9));

    // Spatialized infinite loop so the voice neither ends while parked nor
    // resists parking; distance drives the audibility metric.
    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(9101), 100.0f, 0.0f, 0.0f);
    play.loopCount = 0;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, harkentest::ObjectId(9)));
    SetListenerX(0.0f);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    ASSERT_EQ(1u, Snap().parkedVoiceCount);

    SetListenerX(99.0f); // distance 1 -> ~0 dB -> returns
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(0u, Snap().parkedVoiceCount);
    const VnVoiceHandle returnedRender = RenderHandle(engine, handle);
    ASSERT_NE(nullptr, engine->voices.Resolve(returnedRender));

    // A fresh play of the same recipe is the authored reference: the
    // returned voice must resolve identical values across every
    // tone-family target (all four sections).
    VnPlayDesc freshPlay = MakePlayDesc(harkentest::ObjectId(9101));
    VnVoiceHandle fresh = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&freshPlay, &fresh));
    const VnVoiceHandle freshRender = RenderHandle(engine, fresh);
    const std::uint32_t toneTargets[] = {
        harken::kTargetVoiceLowpassCutoff,   harken::kTargetVoiceLowpassQ,
        harken::kTargetVoiceHighpassCutoff,  harken::kTargetVoiceHighpassQ,
        harken::kTargetVoiceLowShelfGainDb,  harken::kTargetVoiceLowShelfFreqHz,
        harken::kTargetVoiceLowShelfQ,       harken::kTargetVoiceHighShelfGainDb,
        harken::kTargetVoiceHighShelfFreqHz, harken::kTargetVoiceHighShelfQ,
    };
    for (const std::uint32_t target : toneTargets) {
        EXPECT_FLOAT_EQ(engine->params.ResolvedValue(freshRender, target),
                        engine->params.ResolvedValue(returnedRender, target))
            << "tone target " << target;
    }
}

// --- voice APIs reach a parked voice; the returned voice carries the changes ---

TEST(Park, ParkedPanAppliesOnFirstReturnedBlock) {
    EngineSession session(ParkConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterLeaf(harkentest::ObjectId(1));
    RegisterFarProfile(harkentest::ObjectId(9));
    const VnVoiceHandle handle = PlayAndPark(engine, harkentest::ObjectId(1), harkentest::ObjectId(9));
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(handle).kind);

    ASSERT_EQ(VN_OK, Vn_SetVoicePan(handle, 1.0f));
    ASSERT_EQ(VN_OK, Vn_SetVoicePosition(handle, Vec3(0.0f, 0.0f, -1.0f)));
    ASSERT_EQ(VN_OK, Vn_Update());

    harkentest::Capture returned;
    harkentest::PumpAndCapture(engine, 1, returned);
    HARKENTEST_ASSERT_NON_SILENT(returned.right);
    EXPECT_GT(harkentest::Peak(returned.right),
              harkentest::Peak(returned.left));
}

TEST(Park, PendingPanSurvivesDirectParkAndFirstReturnedBlock) {
    VnConfig config = ParkConfig(/*maxVoices=*/1, /*maxParkedVoices=*/1);
    config.voiceStealPolicy = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterLeaf(harkentest::ObjectId(1));
    RegisterFarProfile(harkentest::ObjectId(9));

    VnPlayDesc incumbentPlay = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle incumbent = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&incumbentPlay, &incumbent));
    engine->nullBackend.Pump(1);

    VnPlayDesc farPlay = MakePlayDesc3D(harkentest::ObjectId(1), 0.0f, 0.0f, -100.0f);
    VnVoiceHandle deferred = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&farPlay, &deferred));
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Pending,
              engine->ledger.Classify(deferred).kind);
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(deferred, harkentest::ObjectId(9)));
    ASSERT_EQ(VN_OK, Vn_SetVoicePan(deferred, 1.0f));
    ASSERT_EQ(VN_OK,
              Vn_SetListener(Vec3(0.0f, 0.0f, 0.0f),
                             Vec3(0.0f, 0.0f, -1.0f),
                             Vec3(0.0f, 1.0f, 0.0f)));

    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Pending,
              engine->ledger.Classify(deferred).kind);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(deferred).kind);

    ASSERT_EQ(VN_OK,
              Vn_SetListener(Vec3(0.0f, 0.0f, -99.0f),
                             Vec3(0.0f, 0.0f, -1.0f),
                             Vec3(0.0f, 1.0f, 0.0f)));
    ASSERT_EQ(VN_OK, Vn_Update());
    harkentest::Capture returned;
    harkentest::PumpAndCapture(engine, 1, returned);
    HARKENTEST_ASSERT_NON_SILENT(returned.right);
    EXPECT_GT(harkentest::Peak(returned.right),
              harkentest::Peak(returned.left));
}

TEST(Park, ParkedWidthReachesReturnedVoiceParameterState) {
    EngineSession session(ParkConfig(/*maxVoices=*/1,
                                     /*maxParkedVoices=*/1));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterLeaf(harkentest::ObjectId(1));

    VnPlayDesc quiet = MakePlayDesc(harkentest::ObjectId(1), -80.0f);
    quiet.loopCount = 0;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&quiet, &handle));
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(handle).kind);
    ASSERT_EQ(VN_OK, Vn_SetVoiceWidth(handle, 0.25f));
    ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(handle, 80.0f, 0));
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Live,
              engine->ledger.Classify(handle).kind);

    const VnVoiceHandle render = RenderHandle(engine, handle);
    ASSERT_NE(0u, render);
    EXPECT_EQ(engine->params.ResolvedValue(render, harken::kTargetVoiceWidth),
              0.25f);
    engine->nullBackend.Pump(1);
    const harken::VoiceRenderContext* context = engine->voices.Resolve(render);
    ASSERT_NE(nullptr, context);
    EXPECT_EQ(context->width, 0.25f);
    EXPECT_TRUE(!context->spatialMatrixHasRendered || context->spatialMatrix.Settled());
}

TEST(Park, ReturnCensusCountsEveryApiDirectTargetAtomically) {
    VnConfig config = ParkConfig(/*maxVoices=*/1,
                                 /*maxParkedVoices=*/1);
    config.commandQueueDepth = 16;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterLeaf(harkentest::ObjectId(1));
    RegisterFarProfile(harkentest::ObjectId(9));
    const VnVoiceHandle handle = PlayAndPark(engine, harkentest::ObjectId(1), harkentest::ObjectId(9));
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(handle).kind);

    ASSERT_EQ(VN_OK, Vn_SetVoicePan(handle, -0.5f));
    ASSERT_EQ(VN_OK, Vn_SetVoiceWidth(handle, 0.25f));
    const std::uint32_t ledgerSlot = static_cast<std::uint32_t>(handle);
    const harken::ParkRecord& record = engine->parkRecords[ledgerSlot];
    ASSERT_EQ(record.apiDirect.size(), 2u);
    EXPECT_EQ(record.apiDirect[0].targetId, harken::kTargetVoicePan);
    EXPECT_EQ(record.apiDirect[0].value, -0.5f);
    EXPECT_EQ(record.apiDirect[1].targetId, harken::kTargetVoiceWidth);
    EXPECT_EQ(record.apiDirect[1].value, 0.25f);
    ASSERT_EQ(VN_OK,
              Vn_SetVoicePosition(handle, Vec3(1.0f, 0.0f, 0.0f)));

    constexpr std::uint32_t demand = 4u + harken::kSpatialDrivenTargetsPerVoice;
    while (engine->commands.FreeSlots() > demand - 1) {
        ASSERT_EQ(VN_OK, Vn_SetBusVolume(VN_BUS_MASTER, -3.0f, 0));
    }
    ASSERT_EQ(engine->commands.FreeSlots(), demand - 1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(handle).kind);
    EXPECT_EQ(engine->commands.FreeSlots(), demand - 1);

    engine->nullBackend.Pump(1);
    while (engine->commands.FreeSlots() > demand) {
        ASSERT_EQ(VN_OK, Vn_SetBusVolume(VN_BUS_MASTER, -3.0f, 0));
    }
    const std::uint32_t exactRoom = engine->commands.FreeSlots();
    ASSERT_EQ(exactRoom, demand);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Live,
              engine->ledger.Classify(handle).kind);
    EXPECT_EQ(exactRoom - engine->commands.FreeSlots(), 6u);
}

TEST(Park, SettersReachParkedVoice) {
    VnConfig config = ParkConfig();
    config.fxSlotsPerVoice = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterLeaf(harkentest::ObjectId(1));
    RegisterFarProfile(harkentest::ObjectId(9));
    const VnVoiceHandle handle = PlayAndPark(engine, harkentest::ObjectId(1), harkentest::ObjectId(9));
    const std::uint32_t ledgerSlot = static_cast<std::uint32_t>(handle);
    ASSERT_TRUE(engine->parkRecords[ledgerSlot].active);

    // Every voice API resolves the parked handle rather than refusing
    // VN_STALE_HANDLE, mutating the replay state the return will apply.
    EXPECT_EQ(VN_OK, Vn_SetVoiceVolume(handle, -6.0f, 0));
    EXPECT_EQ(VN_OK, Vn_SetVoiceDistanceScale(handle, 2.0f));
    EXPECT_EQ(VN_OK, Vn_SetVoiceSpatializationMix(handle, 0.25f));
    EXPECT_FLOAT_EQ(2.0f,
                    engine->parkRecords[ledgerSlot].spatial.distanceScale);
    EXPECT_FLOAT_EQ(0.25f,
                    engine->parkRecords[ledgerSlot].spatial.spatializationMix);
    VnThreeBandToneDesc tone{};
    tone.structSize = sizeof(VnThreeBandToneDesc);
    tone.lowShelfGainDb = 3.0f;
    tone.lowShelfFreqHz = 200.0f;
    tone.peakGainDb = 0.0f;
    tone.peakFreqHz = 1000.0f;
    tone.peakQ = 1.0f;
    tone.highShelfGainDb = 0.0f;
    tone.highShelfFreqHz = 8000.0f;
    EXPECT_EQ(VN_OK,
              Vn_SetVoiceEffect(handle, 0, VN_EFFECT_THREE_BAND_TONE,
                                reinterpret_cast<const VnEffectDesc*>(&tone)));
    EXPECT_EQ(VN_OK, Vn_SetVoiceEffectParam(handle, 0,
                                            harken::kToneParamPeakGainDb, -9.0f));

    // The captured state reflects the changes while parked.
    EXPECT_EQ(1u, engine->parkRecords[ledgerSlot].effects.size());

    // Moving the parked voice next to the listener (via the parked position
    // setter) makes it audible; it returns and renders the captured changes.
    EXPECT_EQ(VN_OK, Vn_SetVoicePosition(handle, Vec3(1.0f, 0.0f, 0.0f)));
    ASSERT_EQ(VN_OK, Vn_Update()); // returns
    engine->nullBackend.Pump(1);   // audio applies the re-staged commands
    ASSERT_EQ(VN_OK, Vn_Update()); // spatial evaluate commits attenuation

    const VnVoiceHandle render = RenderHandle(engine, handle);
    ASSERT_NE(0u, render);
    harken::Spatializer::SpatialVoiceState spatial;
    ASSERT_TRUE(engine->spatial.CaptureVoice(render, &spatial));
    EXPECT_FLOAT_EQ(2.0f, spatial.distanceScale);
    EXPECT_FLOAT_EQ(0.25f, spatial.spatializationMix);
    // The set-while-parked volume is live on the returned voice (distance 1 on
    // the far profile is ~0 dB attenuation, so the resolved value is ~ -6 dB).
    EXPECT_NEAR(-6.0f,
                engine->params.ResolvedValue(render, harken::kTargetVoiceVolume),
                0.5f);
    // The set-while-parked effect is installed with its overridden param.
    harken::VoiceRenderContext* ctx = engine->voices.Resolve(render);
    ASSERT_NE(nullptr, ctx);
    ASSERT_NE(nullptr, ctx->fxSlots[0].process);
    auto* toneState =
        static_cast<harken::ToneEffectState*>(ctx->fxSlots[0].state);
    ASSERT_NE(nullptr, toneState);
    EXPECT_FLOAT_EQ(-9.0f, toneState->peakGainDb);
}

// --- parked per-slot bypass persists across a later effect install ---

TEST(Park, ParkedBypassPersistsAcrossEffectSet) {
    VnThreeBandToneDesc tone{};
    tone.structSize = sizeof(VnThreeBandToneDesc);
    tone.lowShelfGainDb = 6.0f; // a boost that would be audible if it leaked
    tone.lowShelfFreqHz = 200.0f;
    tone.peakGainDb = 0.0f;
    tone.peakFreqHz = 1000.0f;
    tone.peakQ = 1.0f;
    tone.highShelfGainDb = 0.0f;
    tone.highShelfFreqHz = 8000.0f;

    // Render the FIRST block after return with a bypassed effect, and — in a
    // separate session with identical timing — the first block with NO effect. A
    // bypassed effect must contribute nothing, so the two blocks are
    // sample-identical from the first sample. This pins the actual audio: the
    // post-pump bypass state settles to dry by the block's END regardless of
    // order, so only first-block audio catches an install-before-bypass wet leak.
    harkentest::Capture withBypassedEffect;
    {
        VnConfig config = ParkConfig();
        config.fxSlotsPerVoice = 1;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        RegisterLeaf(harkentest::ObjectId(1));
        RegisterFarProfile(harkentest::ObjectId(9));
        const VnVoiceHandle handle = PlayAndPark(engine, harkentest::ObjectId(1), harkentest::ObjectId(9));
        // Bypass the EMPTY slot, THEN install the effect — the order a live voice
        // supports, where the per-slot bypass persists independent of the effect.
        EXPECT_EQ(VN_OK, Vn_SetVoiceEffectBypass(handle, 0, 1));
        EXPECT_EQ(VN_OK,
                  Vn_SetVoiceEffect(handle, 0, VN_EFFECT_THREE_BAND_TONE,
                                    reinterpret_cast<const VnEffectDesc*>(&tone)));
        EXPECT_EQ(VN_OK, Vn_SetVoicePosition(handle, Vec3(1.0f, 0.0f, 0.0f)));
        ASSERT_EQ(VN_OK, Vn_Update()); // returns
        harkentest::PumpAndCapture(engine, 1, withBypassedEffect); // first block
        harken::VoiceRenderContext* ctx =
            engine->voices.Resolve(RenderHandle(engine, handle));
        ASSERT_NE(nullptr, ctx);
        ASSERT_NE(nullptr, ctx->fxSlots[0].process); // effect installed
    }
    harkentest::Capture noEffect;
    {
        VnConfig config = ParkConfig();
        config.fxSlotsPerVoice = 1;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        RegisterLeaf(harkentest::ObjectId(1));
        RegisterFarProfile(harkentest::ObjectId(9));
        const VnVoiceHandle handle = PlayAndPark(engine, harkentest::ObjectId(1), harkentest::ObjectId(9));
        EXPECT_EQ(VN_OK, Vn_SetVoicePosition(handle, Vec3(1.0f, 0.0f, 0.0f)));
        ASSERT_EQ(VN_OK, Vn_Update()); // returns, no effect installed
        harkentest::PumpAndCapture(engine, 1, noEffect); // first block
    }
    HARKENTEST_ASSERT_NON_SILENT(noEffect.right);
    ASSERT_EQ(noEffect.left.size(), withBypassedEffect.left.size());
    for (std::size_t i = 0; i < noEffect.left.size(); ++i) {
        EXPECT_FLOAT_EQ(noEffect.left[i], withBypassedEffect.left[i])
            << "left frame " << i << " (wet effect leaked on the first block)";
        EXPECT_FLOAT_EQ(noEffect.right[i], withBypassedEffect.right[i])
            << "right frame " << i;
    }
}

// --- parked virtual pitch is wire-clamped and preserves non-direct terms ---

TEST(Park, ParkedPitchUsesWireClampedResolvedRate) {
    EngineSession session(ParkConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterLeaf(harkentest::ObjectId(1));
    RegisterFarProfile(harkentest::ObjectId(9));

    // A base pitch of 1.5 stands in for any non-API-direct pitch contribution
    // (the same combine slot a Variable binding would feed): it must survive a
    // later API-direct change while parked.
    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 100.0f, 0.0f, 0.0f);
    play.pitchRatio = 1.5f;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, harkentest::ObjectId(9)));
    SetListenerX(0.0f);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update()); // parks
    engine->nullBackend.Pump(1);
    const std::uint32_t ledgerSlot = static_cast<std::uint32_t>(handle);
    ASSERT_TRUE(engine->parkRecords[ledgerSlot].active);
    // Captured resolved rate is the base 1.5 (wire-clamped, within range).
    EXPECT_NEAR(1.5, engine->parkRecords[ledgerSlot].resolvedPitchRatio, 1e-6);

    // Change API-direct pitch to 2.0 while parked: the resolved rate becomes
    // 1.5 * 2.0 = 3.0 — the base term is preserved via the cents delta, not
    // dropped.
    ASSERT_EQ(VN_OK, Vn_SetVoicePitch(handle, 2.0f));
    EXPECT_NEAR(3.0, engine->parkRecords[ledgerSlot].resolvedPitchRatio, 1e-6);

    // An absurd API-direct pitch (64x) would resolve to 1.5 * 64 = 96x, but the
    // virtual rate is wire-clamped to the engine's max ratio (64), never the raw
    // ratio — the render could not consume faster than that.
    ASSERT_EQ(VN_OK, Vn_SetVoicePitch(handle, 64.0f));
    EXPECT_NEAR(64.0, engine->parkRecords[ledgerSlot].resolvedPitchRatio, 1e-6);
}

// --- stop paths end an attached parked voice ---

TEST(Park, StopEmitterEndsAttachedParkedVoice) {
    NotificationSink sink;
    VnConfig config = ParkConfig();
    config.onNotification = &SinkCb;
    config.notificationUserData = &sink;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterLeaf(harkentest::ObjectId(1));
    RegisterFarProfile(harkentest::ObjectId(9));

    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_EQ(VN_OK, Vn_SetEmitterPosition(emitter, Vec3(100.0f, 0.0f, 0.0f)));

    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 100.0f, 0.0f, 0.0f);
    play.emitterHandle = emitter;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, harkentest::ObjectId(9)));
    SetListenerX(0.0f); // emitter at 100 -> inaudible
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update()); // parks the attached voice
    engine->nullBackend.Pump(1);
    ASSERT_EQ(1u, Snap().parkedVoiceCount);

    // Stopping the emitter reaches the parked voice attached to it.
    EXPECT_EQ(VN_OK, Vn_StopEmitter(emitter, 0));
    EXPECT_EQ(0u, Snap().parkedVoiceCount);
    EXPECT_EQ(0u, engine->ledger.ToRender(handle)); // handle stale
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(1u, sink.Count(VN_NOTIFY_VOICE_ENDED, handle));
}

// --- return is subject to steal policy (spec §6) ---

TEST(Park, ReturnStealsWhenPoolFull) {
    NotificationSink sink;
    // Two-voice pool, steal-oldest: a high-priority parked voice returns into a
    // pool full of lower-priority voices by stealing the oldest.
    VnConfig config = ParkConfig(/*maxVoices=*/2, /*maxParkedVoices=*/2);
    config.voiceStealPolicy = 2; // steal-oldest
    config.fxSlotsPerVoice = 1;
    config.toneEffectPoolCapacity = 1;
    config.onNotification = &SinkCb;
    config.notificationUserData = &sink;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterLeaf(harkentest::ObjectId(1));
    RegisterFarProfile(harkentest::ObjectId(9));

    // A high-priority spatialized voice, parked via distance.
    VnPlayDesc hi = MakePlayDesc3D(harkentest::ObjectId(1), 100.0f, 0.0f, 0.0f);
    hi.priority = 200;
    VnVoiceHandle parked = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&hi, &parked));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(parked, harkentest::ObjectId(9)));
    VnThreeBandToneDesc tone{};
    tone.structSize = sizeof(VnThreeBandToneDesc);
    tone.lowShelfFreqHz = 200.0f;
    tone.peakFreqHz = 1000.0f;
    tone.peakQ = 1.0f;
    tone.highShelfFreqHz = 8000.0f;
    ASSERT_EQ(VN_OK,
              Vn_SetVoiceEffect(parked, 0, VN_EFFECT_THREE_BAND_TONE,
                                reinterpret_cast<const VnEffectDesc*>(&tone)));
    SetListenerX(0.0f);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update()); // parks (frees its slot)
    engine->nullBackend.Pump(1);
    ASSERT_EQ(1u, Snap().parkedVoiceCount);

    // Fill the pool with two lower-priority voices.
    VnPlayDesc lo = MakePlayDesc(harkentest::ObjectId(1));
    lo.priority = 100;
    VnVoiceHandle l1 = 0;
    VnVoiceHandle l2 = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&lo, &l1));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&lo, &l2));
    engine->nullBackend.Pump(1); // both rendering, oldest = l1

    // Bring the parked voice next to the listener: it must return, and with no
    // free slot it steals the oldest lower-priority voice.
    ASSERT_EQ(VN_OK, Vn_SetVoicePosition(parked, Vec3(1.0f, 0.0f, 0.0f)));
    ASSERT_EQ(VN_OK, Vn_Update()); // issues the steal (victim declick)
    EXPECT_EQ(1u, Snap().voiceStolenCount);
    const std::uint32_t parkedSlot =
        static_cast<std::uint32_t>(parked);
    ASSERT_TRUE(engine->parkRecords[parkedSlot].returnEffectsPrepared);
    VnVoiceHandle claimedVictim = 0;
    ASSERT_TRUE(engine->voiceManager.ClaimFor(parked, &claimedVictim));
    const VnVoiceHandle expectedReturn =
        (static_cast<VnVoiceHandle>(
             harken::VoiceManager::SuccessorGeneration(claimedVictim))
         << 32u) |
        static_cast<std::uint32_t>(claimedVictim);
    EXPECT_EQ(expectedReturn,
              engine->parkRecords[parkedSlot].returnPreparedRenderHandle);
    const auto originalParams =
        engine->parkRecords[parkedSlot].effects[0].params;
    VnThreeBandToneDesc replacementTone = tone;
    replacementTone.peakGainDb = 6.0f;
    EXPECT_EQ(
        VN_NO_EFFECT,
        Vn_SetVoiceEffect(
            parked, 0, VN_EFFECT_THREE_BAND_TONE,
            reinterpret_cast<const VnEffectDesc*>(&replacementTone)));
    EXPECT_EQ(originalParams,
              engine->parkRecords[parkedSlot].effects[0].params);
    EXPECT_TRUE(engine->parkRecords[parkedSlot].returnEffectsPrepared);
    EXPECT_TRUE(
        engine->parkRecords[parkedSlot].returnSeedTokens[0].IsPrepared());

    engine->effectParamTargets.SetPrepareSeedFailureAfterForTests(0);
    EXPECT_EQ(
        VN_DEVICE_ERROR,
        Vn_SetVoiceEffectParam(
            parked, 0, harken::kToneParamPeakGainDb, -3.0f));
    engine->effectParamTargets.SetPrepareSeedFailureAfterForTests(-1);
    EXPECT_EQ(originalParams,
              engine->parkRecords[parkedSlot].effects[0].params);
    EXPECT_TRUE(engine->parkRecords[parkedSlot].returnEffectsPrepared);
    EXPECT_TRUE(
        engine->parkRecords[parkedSlot].returnSeedTokens[0].IsPrepared());

    ASSERT_EQ(VN_OK, Vn_SetVoiceEffectBypass(parked, 0, 1));
    EXPECT_TRUE(engine->parkRecords[parkedSlot].effectBypass[0]);
    EXPECT_TRUE(engine->parkRecords[parkedSlot].returnEffectsPrepared);
    EXPECT_TRUE(
        engine->parkRecords[parkedSlot].returnSeedTokens[0].IsPrepared());

    ASSERT_EQ(VN_OK, Vn_ClearVoiceEffect(parked, 0));
    EXPECT_TRUE(engine->parkRecords[parkedSlot].returnEffectsPrepared);
    EXPECT_EQ(0u, engine->parkRecords[parkedSlot].returnEffectCount);
    EXPECT_TRUE(engine->voiceManager.ClaimFor(parked, &claimedVictim));
    // The victim (oldest = l1) is marked but still renders its declick block; it
    // frees on the next pump. Before the claimant is serviced, the exact claim
    // keeps an unrelated low-priority play from consuming that generation.
    engine->nullBackend.Pump(1);
    VnPlayDesc unrelatedPlay = lo;
    unrelatedPlay.priority = 0;
    VnVoiceHandle unrelated = 0;
    EXPECT_EQ(VN_NO_VOICE, harkentest::PlayRaw(&unrelatedPlay, &unrelated));
    EXPECT_EQ(1u, Snap().voiceStolenCount);
    EXPECT_TRUE(engine->voiceManager.ClaimFor(parked, &claimedVictim));

    ASSERT_EQ(VN_OK, Vn_Update());
    bool returned =
        engine->voices.Resolve(RenderHandle(engine, parked)) != nullptr;
    for (int i = 0; i < 8 && !returned; ++i) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
        returned = engine->voices.Resolve(RenderHandle(engine, parked)) !=
                   nullptr;
    }
    EXPECT_TRUE(returned);
    EXPECT_EQ(0u, engine->ledger.ToRender(l1)); // the oldest was the victim
    EXPECT_EQ(1u, sink.Count(VN_NOTIFY_VOICE_RETURNED, parked));
    EXPECT_EQ(0u, Snap().parkedVoiceCount);
    EXPECT_EQ(1u, Snap().voiceStolenCount);
}

TEST(Park, PreparedReturnNewEffectReserveFailureIsAtomicAndRetryable) {
    VnConfig config = ParkConfig(/*maxVoices=*/1,
                                 /*maxParkedVoices=*/1);
    config.voiceStealPolicy = 2;
    config.fxSlotsPerVoice = 1;
    config.gainEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterLeaf(harkentest::ObjectId(1));

    VnPlayDesc quiet = MakePlayDesc(harkentest::ObjectId(1), -80.0f);
    quiet.priority = 200;
    quiet.loopCount = 0;
    VnVoiceHandle parked = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&quiet, &parked));
    const std::uint32_t parkedSlot =
        static_cast<std::uint32_t>(parked);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(parked).kind);
    ASSERT_TRUE(engine->parkRecords[parkedSlot].effects.empty());
    ASSERT_EQ(0u, engine->parkRecords[parkedSlot].effects.capacity());

    VnPlayDesc low = MakePlayDesc(harkentest::ObjectId(1));
    low.priority = 100;
    low.loopCount = 0;
    VnVoiceHandle victim = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&low, &victim));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(parked, 80.0f, 0));
    ASSERT_EQ(VN_OK, Vn_Update());

    harken::ParkRecord& record = engine->parkRecords[parkedSlot];
    VnVoiceHandle claimedVictim = 0;
    ASSERT_TRUE(
        engine->voiceManager.ClaimFor(parked, &claimedVictim));
    ASSERT_TRUE(record.returnEffectsPrepared);
    ASSERT_EQ(0u, record.returnEffectCount);
    ASSERT_EQ(0u, engine->effectBank.InUseOf(VN_EFFECT_GAIN));
    ASSERT_EQ(
        0u,
        engine->effectParamTargets.ReservedRecordCountForTests());

    VnGainDesc gain{};
    gain.structSize = sizeof(VnGainDesc);
    gain.gainDb = -3.0f;
    harken::detail::ArmLogicAllocFaultForTests(
        1, harken::detail::LogicAllocSite::VoiceEffectParked);
    EXPECT_EQ(
        VN_NO_EFFECT,
        Vn_SetVoiceEffect(
            parked, 0, VN_EFFECT_GAIN,
            reinterpret_cast<const VnEffectDesc*>(&gain)));
    harken::detail::DisarmLogicAllocFaultForTests();

    EXPECT_TRUE(record.effects.empty());
    EXPECT_EQ(0u, record.effects.capacity());
    EXPECT_TRUE(record.returnEffectsPrepared);
    EXPECT_EQ(0u, record.returnEffectCount);
    EXPECT_EQ(0u, engine->effectBank.InUseOf(VN_EFFECT_GAIN));
    EXPECT_EQ(
        0u,
        engine->effectParamTargets.ReservedRecordCountForTests());
    VnVoiceHandle claimAfterFailure = 0;
    EXPECT_TRUE(
        engine->voiceManager.ClaimFor(parked, &claimAfterFailure));
    EXPECT_EQ(claimedVictim, claimAfterFailure);

    ASSERT_EQ(
        VN_OK,
        Vn_SetVoiceEffect(
            parked, 0, VN_EFFECT_GAIN,
            reinterpret_cast<const VnEffectDesc*>(&gain)));
    ASSERT_EQ(1u, record.effects.size());
    EXPECT_EQ(VN_EFFECT_GAIN, record.effects[0].type);
    EXPECT_TRUE(record.returnEffectsPrepared);
    EXPECT_EQ(1u, record.returnEffectCount);
    EXPECT_EQ(1u, engine->effectBank.InUseOf(VN_EFFECT_GAIN));
    EXPECT_EQ(
        1u,
        engine->effectParamTargets.ReservedRecordCountForTests());
    VnVoiceHandle claimAfterRetry = 0;
    EXPECT_TRUE(
        engine->voiceManager.ClaimFor(parked, &claimAfterRetry));
    EXPECT_EQ(claimedVictim, claimAfterRetry);

    bool returned = false;
    for (int i = 0; i < 8 && !returned; ++i) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
        returned =
            engine->ledger.Classify(parked).kind ==
            harken::VoiceLedger::HandleKind::Live;
    }
    ASSERT_TRUE(returned);
    EXPECT_EQ(
        0u,
        engine->effectParamTargets.ReservedRecordCountForTests());
    VnVoiceHandle leakedClaim = 0;
    EXPECT_FALSE(
        engine->voiceManager.ClaimFor(parked, &leakedClaim));
    EXPECT_EQ(1u, engine->effectBank.InUseOf(VN_EFFECT_GAIN));
    EXPECT_EQ(1u, Snap().voiceStolenCount);
}

TEST(Park, ReturnClaimReleasesWhenVoiceLosesAudibilityQualification) {
    VnConfig config = ParkConfig(/*maxVoices=*/1, /*maxParkedVoices=*/1);
    config.voiceStealPolicy = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterLeaf(harkentest::ObjectId(1));
    RegisterFarProfile(harkentest::ObjectId(9));

    VnPlayDesc high = MakePlayDesc3D(harkentest::ObjectId(1), 100.0f, 0.0f, 0.0f);
    high.priority = 200;
    VnVoiceHandle parked = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&high, &parked));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(parked, harkentest::ObjectId(9)));
    SetListenerX(0.0f);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);

    VnPlayDesc low = MakePlayDesc(harkentest::ObjectId(1));
    low.priority = 100;
    VnVoiceHandle victim = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&low, &victim));
    engine->nullBackend.Pump(1);

    ASSERT_EQ(VN_OK,
              Vn_SetVoicePosition(parked, Vec3(1.0f, 0.0f, 0.0f)));
    ASSERT_EQ(VN_OK, Vn_Update());
    VnVoiceHandle claimedVictim = 0;
    ASSERT_TRUE(engine->voiceManager.ClaimFor(parked, &claimedVictim));
    const VnVoiceHandle originalClaimedVictim = claimedVictim;
    ASSERT_TRUE(
        engine->parkRecords[static_cast<std::uint32_t>(parked)]
            .returnEffectsPrepared);

    ASSERT_EQ(VN_OK,
              Vn_SetVoicePosition(parked, Vec3(100.0f, 0.0f, 0.0f)));
    ASSERT_EQ(VN_OK, Vn_Update());
    VnVoiceHandle residualClaim = 0;
    EXPECT_FALSE(engine->voiceManager.ClaimFor(parked, &residualClaim));
    EXPECT_FALSE(
        engine->parkRecords[static_cast<std::uint32_t>(parked)]
            .returnEffectsPrepared);

    bool victimReleased =
        engine->voices.Resolve(originalClaimedVictim) == nullptr;
    for (int i = 0; i < 8 && !victimReleased; ++i) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
        victimReleased =
            engine->voices.Resolve(originalClaimedVictim) == nullptr;
    }
    ASSERT_TRUE(victimReleased);
    EXPECT_EQ(1u, Snap().voiceStolenCount);
    VnVoiceHandle unrelated = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&low, &unrelated));
    EXPECT_NE(0u, RenderHandle(engine, unrelated));
    EXPECT_EQ(1u, Snap().voiceStolenCount);
}

TEST(Park, ReturnClaimSeedsPastFullyInstalledBaseBudget) {
    VnConfig config = ParkConfig(/*maxVoices=*/1,
                                 /*maxParkedVoices=*/1);
    config.voiceStealPolicy = 2;
    config.maxBuses = 1;
    config.fxSlotsPerVoice = 1;
    config.fxSlotsPerBus = 1;
    config.toneEffectPoolCapacity = 3;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterLeaf(harkentest::ObjectId(1));
    RegisterFarProfile(harkentest::ObjectId(9));
    const VnThreeBandToneDesc tone = FullParamTone();
    ASSERT_EQ(
        VN_OK,
        Vn_SetBusEffect(
            VN_BUS_MASTER, 0, VN_EFFECT_THREE_BAND_TONE,
            reinterpret_cast<const VnEffectDesc*>(&tone)));

    VnPlayDesc high = MakePlayDesc3D(harkentest::ObjectId(1), 100.0f, 0.0f, 0.0f);
    high.priority = 200;
    high.loopCount = 0;
    VnVoiceHandle parked = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&high, &parked));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(parked, harkentest::ObjectId(9)));
    ASSERT_EQ(
        VN_OK,
        Vn_SetVoiceEffect(
            parked, 0, VN_EFFECT_THREE_BAND_TONE,
            reinterpret_cast<const VnEffectDesc*>(&tone)));
    SetListenerX(0.0f);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(parked).kind);

    VnPlayDesc low = MakePlayDesc(harkentest::ObjectId(1));
    low.priority = 100;
    low.loopCount = 0;
    VnVoiceHandle victim = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&low, &victim));
    ASSERT_EQ(
        VN_OK,
        Vn_SetVoiceEffect(
            victim, 0, VN_EFFECT_THREE_BAND_TONE,
            reinterpret_cast<const VnEffectDesc*>(&tone)));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(2u, engine->effectBank.ToneInUse());

    ASSERT_EQ(
        VN_OK,
        Vn_SetVoicePosition(parked, Vec3(1.0f, 0.0f, 0.0f)));
    ASSERT_EQ(VN_OK, Vn_Update());
    VnVoiceHandle claimedVictim = 0;
    ASSERT_TRUE(
        engine->voiceManager.ClaimFor(parked, &claimedVictim));
    EXPECT_EQ(RenderHandle(engine, victim), claimedVictim);
    EXPECT_EQ(harken::kToneParamCount,
              engine->effectParamTargets.ReservedRecordCountForTests());
    EXPECT_EQ(3u, engine->effectBank.ToneInUse());
    EXPECT_EQ(1u, Snap().voiceStolenCount);

    bool returned = false;
    for (int i = 0; i < 8 && !returned; ++i) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
        returned =
            engine->ledger.Classify(parked).kind ==
            harken::VoiceLedger::HandleKind::Live;
    }
    ASSERT_TRUE(returned);
    EXPECT_EQ(
        0u,
        engine->effectParamTargets.ReservedRecordCountForTests());
    VnVoiceHandle leakedClaim = 0;
    EXPECT_FALSE(
        engine->voiceManager.ClaimFor(parked, &leakedClaim));
    EXPECT_EQ(1u, Snap().voiceStolenCount);
    EXPECT_EQ(2u, engine->effectBank.ToneInUse());
}

TEST(Park, ReturnRefusedUnderRefusePolicyStaysParked) {
    // Refuse policy: a full pool keeps the returning voice parked, and the
    // refusal counter moves.
    VnConfig config = ParkConfig(/*maxVoices=*/1, /*maxParkedVoices=*/1);
    config.voiceStealPolicy = 0; // refuse
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterLeaf(harkentest::ObjectId(1));
    RegisterFarProfile(harkentest::ObjectId(9));

    VnPlayDesc far = MakePlayDesc3D(harkentest::ObjectId(1), 100.0f, 0.0f, 0.0f);
    VnVoiceHandle parked = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&far, &parked));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(parked, harkentest::ObjectId(9)));
    SetListenerX(0.0f);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update()); // parks, freeing the only slot
    engine->nullBackend.Pump(1);

    // Fill the single slot with another voice.
    VnPlayDesc other = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle blocker = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&other, &blocker));
    engine->nullBackend.Pump(1);

    // The parked voice becomes audible but cannot return: refuse policy, full
    // pool. It stays parked and the refusal counter advances.
    ASSERT_EQ(VN_OK, Vn_SetVoicePosition(parked, Vec3(1.0f, 0.0f, 0.0f)));
    const std::uint64_t before = Snap().returnRefusedCount;
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(1u, Snap().parkedVoiceCount);
    EXPECT_EQ(0u, Snap().voiceStolenCount); // refuse never steals
    EXPECT_GT(Snap().returnRefusedCount, before);
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(parked).kind);
}

// --- §9.6 render golden: a returned voice equals a fresh play at its offset ---

TEST(Park, ReturnedVoiceMatchesFreshPlayGolden) {
    // Two sessions render the same distinct source through the identical
    // spatialized pipeline: one plays it fresh from frame 0 at (1,0,0), the other
    // parks a voice, advances virtual time, then returns it to the same (1,0,0).
    // Because unity-rate playback is sample-exact and both voices share the same
    // listener/position (hence pan + attenuation), the returned output equals a
    // WINDOW of the fresh output at some lag. That lag must fall within the
    // resume tolerance: at most one block of double-buffer staleness plus the
    // resampler FIFO + history pad (kResumeToleranceFrames). The first render
    // blocks of the returned voice cover the seek's history/FIFO fill, so the
    // comparison skips them and matches the steady-state tail.
    auto distinct = [](std::size_t frames) {
        std::vector<float> s(frames);
        double phase = 0.0;
        for (std::size_t i = 0; i < frames; ++i) {
            // A rising-frequency chirp: no two windows repeat, so the alignment
            // that matches is unique within the search range.
            phase += 0.02 + 0.00003 * static_cast<double>(i);
            s[i] = 0.4f * static_cast<float>(std::sin(phase));
        }
        return s;
    };
    const std::vector<float> source = distinct(8000);
    constexpr std::uint32_t kReturnBlocks = 10;

    // Per-block capture that commits the spatial state each block (Vn_Update
    // before the render), so both renders apply the identical pan/attenuation
    // for the shared (1,0,0) geometry — the comparison is then channel-exact.
    auto captureWithUpdate = [](harken::Engine* engine, std::uint32_t blocks,
                                harkentest::Capture& cap) {
        ASSERT_TRUE(cap.left.empty());
        ASSERT_TRUE(cap.right.empty());
        const std::uint32_t frames = engine->nullBackend.BlockFrames();
        const std::uint32_t channels = engine->nullBackend.Channels();
        ASSERT_GT(frames, 0u);
        ASSERT_EQ(channels, 2u);
        const auto before = engine->mixer.BlocksRendered();
        for (std::uint32_t b = 0; b < blocks; ++b) {
            ASSERT_EQ(VN_OK, Vn_Update());
            engine->nullBackend.Pump(1);
            ASSERT_EQ(before + b + 1, engine->mixer.BlocksRendered());
            const float* block = engine->nullBackend.LastBlock();
            ASSERT_NE(nullptr, block);
            for (std::uint32_t i = 0; i < frames; ++i) {
                cap.left.push_back(block[i * channels + 0]);
                cap.right.push_back(block[i * channels + 1]);
            }
        }
        ASSERT_EQ(blocks * frames, cap.left.size());
        ASSERT_EQ(cap.left.size(), cap.right.size());
    };

    // First render the returned voice (its own session), capturing the resume
    // offset; the fresh reference follows in a separate session (the engine is a
    // singleton, so the two sessions never overlap).
    std::uint64_t offset = 0;
    harkentest::Capture returned;
    {
        EngineSession session(ParkConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        VnPCMDesc desc = MonoDesc(source, kRate);
        desc.frames = source.size();
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
        RegisterFarProfile(harkentest::ObjectId(9));
        VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 100.0f, 0.0f, 0.0f);
        VnVoiceHandle handle = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, harkentest::ObjectId(9)));
        SetListenerX(0.0f);
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update()); // parks
        engine->nullBackend.Pump(1);
        const std::uint32_t ledgerSlot = static_cast<std::uint32_t>(handle);
        engine->nullBackend.Pump(4);
        ASSERT_EQ(VN_OK, Vn_Update()); // advance virtual time
        offset = static_cast<std::uint64_t>(
            engine->parkRecords[ledgerSlot].virtualCursor);
        ASSERT_EQ(VN_OK, Vn_SetVoicePosition(handle, Vec3(1.0f, 0.0f, 0.0f)));
        ASSERT_EQ(VN_OK, Vn_Update()); // returns
        ASSERT_NE(nullptr,
                  engine->voices.Resolve(RenderHandle(engine, handle)));
        ASSERT_NO_FATAL_FAILURE(captureWithUpdate(engine, kReturnBlocks, returned));
    }
    // A right-hand point source may have an exactly silent left channel.
    ASSERT_GT(std::max(harkentest::Peak(returned.left),
                       harkentest::Peak(returned.right)),
              harkentest::kSilenceFloor);

    // Fresh reference: a spatialized play at (1,0,0), listener at origin, from
    // frame 0. Enough blocks to span the search window.
    const std::uint32_t freshBlocks = static_cast<std::uint32_t>(
        (offset + kResumeToleranceFrames) / kBlock + kReturnBlocks + 4);
    harkentest::Capture fresh;
    {
        EngineSession session(ParkConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        VnPCMDesc desc = MonoDesc(source, kRate);
        desc.frames = source.size();
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
        RegisterFarProfile(harkentest::ObjectId(9));
        VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 1.0f, 0.0f, 0.0f);
        VnVoiceHandle v = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &v));
        ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(v, harkentest::ObjectId(9)));
        SetListenerX(0.0f);
        ASSERT_NO_FATAL_FAILURE(captureWithUpdate(engine, freshBlocks, fresh));
    }
    ASSERT_GT(std::max(harkentest::Peak(fresh.left),
                       harkentest::Peak(fresh.right)),
              harkentest::kSilenceFloor);

    // Skip the returned voice's first two blocks (seek FIFO/history fill), then
    // find the lag within the tolerance window that aligns it to the fresh
    // render; that lag must exist (near-exact match) and lie within tolerance.
    const std::size_t skip = 2 * kBlock;
    const std::size_t compareLen = 4 * kBlock;
    ASSERT_GT(returned.left.size(), skip + compareLen);
    const std::uint64_t lo =
        offset > kResumeToleranceFrames ? offset - kResumeToleranceFrames : 0;
    const std::uint64_t hi = offset + kResumeToleranceFrames;
    double bestErr = 1e9;
    std::uint64_t bestLag = 0;
    for (std::uint64_t lag = lo; lag <= hi; ++lag) {
        if (lag + skip + compareLen >= fresh.left.size()) {
            break;
        }
        double err = 0.0;
        for (std::size_t i = 0; i < compareLen; ++i) {
            const double dL = returned.left[skip + i] -
                              fresh.left[static_cast<std::size_t>(lag) + skip + i];
            const double dR = returned.right[skip + i] -
                              fresh.right[static_cast<std::size_t>(lag) + skip + i];
            err += std::fabs(dL) + std::fabs(dR);
            if (err >= bestErr) {
                break;
            }
        }
        if (err < bestErr) {
            bestErr = err;
            bestLag = lag;
        }
    }
    // The aligned window matches to within floating-point noise (unity-rate
    // playback is sample-exact), and its lag is inside the resume tolerance.
    EXPECT_LT(bestErr, 1e-3) << "best lag " << bestLag;
    EXPECT_GE(bestLag, lo);
    EXPECT_LE(bestLag, hi);
}

} // namespace
