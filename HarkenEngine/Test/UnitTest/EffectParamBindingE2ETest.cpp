// End-to-end effect-parameter binding through the public C API on the null
// backend: a Variable drives an installed effect's parameter through a curve,
// the composed value reaches the audio thread on opcode 14/22, and the rendered
// output follows base + contribution in the parameter's declared domain. Covers
// the voice gainDb analytic path, the bus scope, the install-only base case,
// the detach return-to-base edge, bound-vs-unbound direct-set interposition
// (a bound direct set retargets the base and emits only through the fold, an
// unbound one emits directly), and the refusal matrix. The gain effect
// (memoryless linear gain) is the analytic vehicle: a DC input of amplitude A
// through g dB renders A * 10^(g/20), so the resolved dB is read directly off
// the output level; a peaking-EQ smoke uses a tone at the peak frequency.
#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "Common/TestUtil.h"
#include "harken/param/EffectParamLocator.h"
#include "harken/render/BuiltinEffects.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::MakePlayDesc;
using harkentest::MonoDesc;

constexpr std::uint32_t kBlock = 256;
constexpr std::uint32_t kRate = 48000;
constexpr float kDc = 0.5f;
constexpr VnVariableId kVar = harkentest::ObjectId(7);
constexpr std::uint32_t kSlot = 0;

void InstallBindingVariable(harken::Engine* engine) {
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(
                  engine, {kVar}, 0.0f, 1.0f).status);
}

VnConfig EffectBindConfig() {
    VnConfig config = harkentest::MakeNullConfig(0, kBlock, kRate);
    config.fxSlotsPerVoice = 1;
    config.fxSlotsPerBus = 1;
    config.delayEffectPoolCapacity = 4;
    config.toneEffectPoolCapacity = 4;
    config.gainEffectPoolCapacity = 4;
    config.compressorEffectPoolCapacity = 4;
    return config;
}

const VnEffectDesc* AsBase(const void* p) {
    return static_cast<const VnEffectDesc*>(p);
}

VnGainDesc GainDesc(float gainDb) {
    VnGainDesc d{};
    d.structSize = sizeof(VnGainDesc);
    d.gainDb = gainDb;
    return d;
}

VnFeedbackDelayDesc DelayDesc(float delayMs, float feedback, float wet,
                              float dry) {
    VnFeedbackDelayDesc d{};
    d.structSize = sizeof(VnFeedbackDelayDesc);
    d.delayMs = delayMs;
    d.feedback = feedback;
    d.wet = wet;
    d.dry = dry;
    return d;
}

VnThreeBandToneDesc ToneDesc(float peakGainDb, float peakFreqHz) {
    VnThreeBandToneDesc t{};
    t.structSize = sizeof(VnThreeBandToneDesc);
    t.lowShelfGainDb = 0.0f;
    t.lowShelfFreqHz = 200.0f;
    t.peakGainDb = peakGainDb;
    t.peakFreqHz = peakFreqHz;
    t.peakQ = 1.0f;
    t.highShelfGainDb = 0.0f;
    t.highShelfFreqHz = 6000.0f;
    return t;
}

VnCompressorDesc CompressorDesc() {
    VnCompressorDesc d{};
    d.structSize = sizeof(VnCompressorDesc);
    d.thresholdDb = -18.0f;
    d.ratio = 1.0f;
    d.attackMs = 0.1f;
    d.releaseMs = 100.0f;
    d.kneeDb = 0.0f;
    d.makeupDb = 0.0f;
    return d;
}

// A two-point linear binding: variable in [0,1] maps to contribution [y0,y1] on
// the packed effect-parameter locator.
VnBindingDesc EffectParamBinding(std::uint32_t scopeKind,
                                 std::uint32_t effectType,
                                 std::uint32_t paramIndex, float y0, float y1,
                                 std::uint32_t slot = kSlot) {
    VnBindingDesc desc{};
    desc.structSize = sizeof(VnBindingDesc);
    desc.variableId = kVar;
    desc.targetScopeKind = scopeKind;
    desc.targetId =
        harken::PackEffectParamLocator(effectType, slot, paramIndex);
    desc.transitionMs = 0;
    desc.curve.structSize = sizeof(VnCurveDesc);
    desc.curve.pointCount = 2;
    desc.curve.points[0] = {0.0f, y0};
    desc.curve.points[1] = {1.0f, y1};
    desc.curve.handles[0] = harkentest::EndpointHandles(
        desc.curve.points[0], desc.curve.points[1]);
    return desc;
}

// Plays a DC mono source and activates the voice (one pump), returning a live
// handle ready for a live-path Set / ApplyVoiceBinding.
VnVoiceHandle PlayActiveDc(EngineSession& session, VnSoundId id) {
    if (session.Engine()->variables.Definition(kVar) == nullptr) {
        InstallBindingVariable(session.Engine());
    }
    static const std::vector<float> dc(kRate, kDc);
    const VnPCMDesc pcm = MonoDesc(dc, kRate);
    EXPECT_EQ(VN_OK, Vn_RegisterPCM(id, &pcm));
    const VnPlayDesc play = MakePlayDesc(id);
    VnVoiceHandle handle = 0;
    EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    Capture warm;
    harkentest::PumpAndCapture(session.Engine(), 1, warm); // activate the voice
    return handle;
}

float SteadyLevel(const Capture& out) {
    // Memoryless-gain DC render settles immediately; read a late sample.
    return out.left.empty() ? 0.0f : std::fabs(out.left.back());
}

void CaptureFreshStereo(harken::Engine* engine, std::uint32_t blocks,
                        Capture& out) {
    ASSERT_TRUE(out.left.empty());
    ASSERT_TRUE(out.right.empty());
    ASSERT_EQ(2u, engine->nullBackend.Channels());
    const auto before = engine->mixer.BlocksRendered();
    harkentest::PumpAndCapture(engine, blocks, out);
    ASSERT_EQ(before + blocks, engine->mixer.BlocksRendered());
    ASSERT_EQ(blocks * engine->nullBackend.BlockFrames(), out.left.size());
    ASSERT_EQ(out.left.size(), out.right.size());
}

float SteadyStereoLevel(const Capture& out) {
    // Mono spatial power is constant across pan and width; either side
    // may be silent.
    return std::hypot(out.left.back(), out.right.back());
}

float PeakLevel(const Capture& out) {
    float peak = 0.0f;
    for (float v : out.left) {
        peak = std::fmax(peak, std::fabs(v));
    }
    return peak;
}

harken::PackLogicRecord* ClaimTestLogicRecord(harken::Engine* engine) {
    constexpr VnPackHandle kTestHandle = 1;
    harken::PackLogicStore::PreparedClaimToken token;
    if (engine->logicPacks.PrepareClaim(kTestHandle, &token) != VN_OK) {
        return nullptr;
    }
    harken::PackLogicRecord content;
    engine->logicPacks.CommitPreparedClaim(&token, std::move(content));
    return engine->logicPacks.RecordFor(kTestHandle);
}

void InstallGainRecipe(harken::Engine* engine, VnSoundId playable,
                       VnBindingId binding, float gainDb) {
    harken::PackLogicRecord* record = ClaimTestLogicRecord(engine);
    ASSERT_NE(nullptr, record);
    harken::PackLogicImage::VoiceRecipe recipe;
    recipe.header.playableId = playable;
    recipe.header.installCount = 1;
    recipe.header.bindingCount = binding == harkentest::ObjectId(0) ? 0u : 1u;
    harken::PackVoiceRecipeInstall install;
    install.slot = 0;
    install.effectType = VN_EFFECT_GAIN;
    install.descriptorVintage =
        harken::kPackV2EffectDescriptorVintage;
    install.paramCount = 1;
    install.params[0] = gainDb;
    recipe.installs.push_back(install);
    if (binding != harkentest::ObjectId(0)) {
        recipe.bindings.push_back(binding);
    }
    record->voiceRecipes.push_back(std::move(recipe));
}

void InstallCompressorRecipe(harken::Engine* engine, VnSoundId playable) {
    harken::PackLogicRecord* record = ClaimTestLogicRecord(engine);
    ASSERT_NE(nullptr, record);
    harken::PackLogicImage::VoiceRecipe recipe;
    recipe.header.playableId = playable;
    recipe.header.installCount = 1;
    recipe.header.bindingCount = 0;
    harken::PackVoiceRecipeInstall install;
    install.slot = 0;
    install.effectType = VN_EFFECT_COMPRESSOR;
    install.descriptorVintage = harken::kPackV2EffectDescriptorVintage;
    install.paramCount = harken::kCompressorParamCount;
    install.params[harken::kCompressorParamThresholdDb] = -18.0f;
    install.params[harken::kCompressorParamRatio] = 4.0f;
    install.params[harken::kCompressorParamAttackMs] = 0.1f;
    install.params[harken::kCompressorParamReleaseMs] = 100.0f;
    install.params[harken::kCompressorParamKneeDb] = 0.0f;
    install.params[harken::kCompressorParamMakeupDb] = 0.0f;
    recipe.installs.push_back(install);
    record->voiceRecipes.push_back(std::move(recipe));
}

// Stages a recipe that installs a gain on two distinct slots, so the recipe
// demands two gain instances from the pool.
void InstallTwoGainRecipe(harken::Engine* engine, VnSoundId playable,
                          float gainDb) {
    harken::PackLogicRecord* record = ClaimTestLogicRecord(engine);
    ASSERT_NE(nullptr, record);
    harken::PackLogicImage::VoiceRecipe recipe;
    recipe.header.playableId = playable;
    recipe.header.installCount = 2;
    recipe.header.bindingCount = 0;
    for (std::uint32_t slot = 0; slot < 2; ++slot) {
        harken::PackVoiceRecipeInstall install;
        install.slot = slot;
        install.effectType = VN_EFFECT_GAIN;
        install.descriptorVintage = harken::kPackV2EffectDescriptorVintage;
        install.paramCount = 1;
        install.params[0] = gainDb;
        recipe.installs.push_back(install);
    }
    record->voiceRecipes.push_back(std::move(recipe));
}


TEST(EffectParamBindingE2E,
     VoiceRecipeGainAndBindingAreCorrectOnFirstAudibleBlock) {
    EngineSession session(EffectBindConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallBindingVariable(engine);
    static const std::vector<float> dc(kRate, kDc);
    const VnPCMDesc pcm = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(100), &pcm));
    const VnBindingDesc binding = EffectParamBinding(
        VN_TARGET_SCOPE_VOICE_EFFECT_PARAM, VN_EFFECT_GAIN,
        harken::kGainParamGainDb, 0.0f, 6.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &binding));
    ASSERT_EQ(VN_OK, Vn_SetValue(kVar, 1.0f));
    harkentest::AdvanceVariableWrite(session.Engine());
    InstallGainRecipe(engine, harkentest::ObjectId(100), harkentest::ObjectId(1), -6.0f);

    VnVoiceHandle handle = 0;
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(100));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    Capture firstBlock;
    harkentest::PumpAndCapture(engine, 1, firstBlock);
    HARKENTEST_ASSERT_NON_SILENT(firstBlock.left);
    const float independentReference =
        kDc * std::pow(10.0f, (-6.0f + 6.0f) / 20.0f);
    EXPECT_NEAR(independentReference, SteadyLevel(firstBlock), 2e-3f);
}


TEST(EffectParamBindingE2E,
     ParkReturnRecipeGainAndBindingAreCorrectOnFirstReturnedBlock) {
    VnConfig config = EffectBindConfig();
    config.maxVoices = 2;
    config.maxParkedVoices = 1;
    config.audibilityParkEnabled = 1;
    config.parkThresholdDb = -40.0f;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallBindingVariable(engine);
    static const std::vector<float> dc(kRate, kDc);
    const VnPCMDesc pcm = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(111), &pcm));
    const VnAttenuationDesc attenuation =
        harkentest::MakeAttenuationDesc(1.0f, 100.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(211), &attenuation));
    const VnBindingDesc binding = EffectParamBinding(
        VN_TARGET_SCOPE_VOICE_EFFECT_PARAM, VN_EFFECT_GAIN,
        harken::kGainParamGainDb, 0.0f, 6.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(12), &binding));
    ASSERT_EQ(VN_OK, Vn_SetValue(kVar, 1.0f));
    harkentest::AdvanceVariableWrite(session.Engine());
    InstallGainRecipe(engine, harkentest::ObjectId(111), harkentest::ObjectId(12), -12.0f);
    const auto listenerAt = [](float x) {
        return Vn_SetListener(harkentest::Vec3(x, 0.0f, 0.0f),
                              harkentest::Vec3(0.0f, 0.0f, -1.0f),
                              harkentest::Vec3(0.0f, 1.0f, 0.0f));
    };
    ASSERT_EQ(VN_OK, listenerAt(99.0f));
    VnPlayDesc play =
        harkentest::MakePlayDesc3D(harkentest::ObjectId(111), 100.0f, 0.0f, 0.0f);
    play.loopCount = 0;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, harkentest::ObjectId(211)));
    engine->nullBackend.Pump(1);

    ASSERT_EQ(VN_OK, listenerAt(0.0f));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(handle).kind);

    ASSERT_EQ(VN_OK, listenerAt(99.0f));
    ASSERT_EQ(VN_OK, Vn_Update());
    Capture returned;
    harkentest::PumpAndCapture(engine, 1, returned);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Live,
              engine->ledger.Classify(handle).kind);
    HARKENTEST_ASSERT_NON_SILENT(returned.left);
    // Centered mono keeps the source level in each stereo channel.
    const float expected =
        kDc * std::pow(10.0f, -6.0f / 20.0f);
    EXPECT_NEAR(expected, SteadyLevel(returned), 2e-3f);
}

TEST(EffectParamBindingE2E, LoadedVoiceRecipePinsReferencedBinding) {
    EngineSession session(EffectBindConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const VnBindingDesc binding = EffectParamBinding(
        VN_TARGET_SCOPE_VOICE_EFFECT_PARAM, VN_EFFECT_GAIN,
        harken::kGainParamGainDb, 0.0f, 6.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(2), &binding));
    InstallGainRecipe(session.Engine(), harkentest::ObjectId(105), harkentest::ObjectId(2), -6.0f);
    EXPECT_EQ(VN_IN_USE, Vn_UnregisterBinding(harkentest::ObjectId(2)));
}

TEST(EffectParamBindingE2E, VoiceRecipeSeedFailureLeavesNoVoiceOrEffect) {
    EngineSession session(EffectBindConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallBindingVariable(engine);
    static const std::vector<float> dc(kRate, kDc);
    const VnPCMDesc pcm = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(101), &pcm));
    InstallGainRecipe(engine, harkentest::ObjectId(101), harkentest::ObjectId(0), -6.0f);
    engine->effectParamTargets.SetSeedFailureForTests(true);
    const std::uint32_t effectsBefore =
        engine->effectBank.InUseOf(VN_EFFECT_GAIN);
    const std::uint32_t effectHighWaterBefore =
        engine->effectBank.HighWaterOf(VN_EFFECT_GAIN);
    VnVoiceHandle handle = 0;
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(101));
    EXPECT_EQ(VN_DEVICE_ERROR, harkentest::PlayRaw(&play, &handle));
    EXPECT_EQ(effectsBefore, engine->effectBank.InUseOf(VN_EFFECT_GAIN));
    EXPECT_EQ(effectHighWaterBefore,
              engine->effectBank.HighWaterOf(VN_EFFECT_GAIN));
    EXPECT_EQ(0u, engine->voices.ActiveCount());
    EXPECT_EQ(0u, engine->effectParamTargets.LiveRecordCount());
    engine->effectParamTargets.SetSeedFailureForTests(false);
}

TEST(EffectParamBindingE2E, VoiceRecipePoolExhaustionIsAtomic) {
    VnConfig config = EffectBindConfig();
    config.maxVoices = 2;
    config.gainEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallBindingVariable(engine);
    static const std::vector<float> dc(kRate, kDc);
    const VnPCMDesc pcm = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(102), &pcm));
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(103), &pcm));
    VnVoiceHandle occupied = 0;
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(102));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &occupied));
    engine->nullBackend.Pump(1);
    const VnGainDesc gain = GainDesc(0.0f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(
                         occupied, 0, VN_EFFECT_GAIN, AsBase(&gain)));
    InstallGainRecipe(engine, harkentest::ObjectId(103), harkentest::ObjectId(0), -6.0f);
    const std::uint32_t voicesBefore = engine->voices.ActiveCount();
    VnVoiceHandle refused = 0;
    play = MakePlayDesc(harkentest::ObjectId(103));
    EXPECT_EQ(VN_NO_EFFECT, harkentest::PlayRaw(&play, &refused));
    EXPECT_EQ(voicesBefore, engine->voices.ActiveCount());
    EXPECT_EQ(1u, engine->effectBank.InUseOf(VN_EFFECT_GAIN));
}

TEST(EffectParamBindingE2E,
     VoiceRecipePartialPoolExhaustionDoesNotRaiseHighWater) {
    // Two gain instances, one already held, so a two-gain recipe is short by
    // exactly one: one free, two demanded. The pure-count precheck must refuse
    // the whole recipe before acquiring anything. If it instead let the acquire
    // loop run, the loop would take the one free instance, fail on the second,
    // and recycle both — leaving the footprint at zero but the pool's
    // high-water mark permanently raised by the transient acquire.
    VnConfig config = EffectBindConfig();
    config.maxVoices = 2;
    config.fxSlotsPerVoice = 2;
    config.gainEffectPoolCapacity = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallBindingVariable(engine);
    static const std::vector<float> dc(kRate, kDc);
    const VnPCMDesc pcm = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(102), &pcm));
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(103), &pcm));
    VnVoiceHandle occupied = 0;
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(102));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &occupied));
    engine->nullBackend.Pump(1);
    const VnGainDesc gain = GainDesc(0.0f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(
                         occupied, 0, VN_EFFECT_GAIN, AsBase(&gain)));
    InstallTwoGainRecipe(engine, harkentest::ObjectId(103), -6.0f);
    const std::uint32_t voicesBefore = engine->voices.ActiveCount();
    const std::uint32_t inUseBefore =
        engine->effectBank.InUseOf(VN_EFFECT_GAIN);
    const std::uint32_t highWaterBefore =
        engine->effectBank.HighWaterOf(VN_EFFECT_GAIN);
    const std::uint32_t liveRecordsBefore =
        engine->effectParamTargets.LiveRecordCount();
    ASSERT_EQ(1u, inUseBefore);
    ASSERT_EQ(1u, highWaterBefore);

    VnVoiceHandle refused = 0;
    play = MakePlayDesc(harkentest::ObjectId(103));
    EXPECT_EQ(VN_NO_EFFECT, harkentest::PlayRaw(&play, &refused));
    EXPECT_EQ(voicesBefore, engine->voices.ActiveCount());
    EXPECT_EQ(inUseBefore, engine->effectBank.InUseOf(VN_EFFECT_GAIN));
    EXPECT_EQ(liveRecordsBefore, engine->effectParamTargets.LiveRecordCount());
    // The refusal never touched the pool: high-water is exactly what it was.
    EXPECT_EQ(highWaterBefore, engine->effectBank.HighWaterOf(VN_EFFECT_GAIN));
}

TEST(EffectParamBindingE2E, VoiceRecipeBackpressureRecyclesStaging) {
    VnConfig config = EffectBindConfig();
    config.commandQueueDepth = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallBindingVariable(engine);
    static const std::vector<float> dc(kRate, kDc);
    const VnPCMDesc pcm = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(104), &pcm));
    InstallGainRecipe(engine, harkentest::ObjectId(104), harkentest::ObjectId(0), -6.0f);
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(VN_BUS_MASTER, -3.0f, 0));
    VnVoiceHandle refused = 0;
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(104));
    EXPECT_EQ(VN_BACKPRESSURE, harkentest::PlayRaw(&play, &refused));
    EXPECT_EQ(0u, engine->voices.ActiveCount());
    EXPECT_EQ(0u, engine->effectBank.InUseOf(VN_EFFECT_GAIN));
    EXPECT_EQ(0u, engine->effectBank.HighWaterOf(VN_EFFECT_GAIN));
    EXPECT_EQ(0u, engine->effectParamTargets.LiveRecordCount());
}

// A 1 kHz sine source (energy at the tone peak frequency), played and activated.
VnVoiceHandle PlayActiveSine(EngineSession& session, VnSoundId id,
                             double freqHz) {
    if (session.Engine()->variables.Definition(kVar) == nullptr) {
        InstallBindingVariable(session.Engine());
    }
    const std::vector<float> sine =
        harkentest::MakeSine(kRate, freqHz, kRate, kDc);
    const VnPCMDesc pcm = MonoDesc(sine, kRate);
    EXPECT_EQ(VN_OK, Vn_RegisterPCM(id, &pcm));
    const VnPlayDesc play = MakePlayDesc(id);
    VnVoiceHandle handle = 0;
    EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    Capture warm;
    harkentest::PumpAndCapture(session.Engine(), 1, warm);
    return handle;
}

// --- voice gainDb analytic ---

TEST(EffectParamBindingE2E, VoiceGainBindingComposesBasePlusContribution) {
    EngineSession session(EffectBindConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    VnVoiceHandle handle = PlayActiveDc(session, harkentest::ObjectId(1));

    const VnGainDesc gain = GainDesc(-6.0f); // base -6 dB
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(handle, kSlot, VN_EFFECT_GAIN,
                                       AsBase(&gain)));
    // Bind gainDb: variable 1.0 -> +6 dB, composing to 0 dB (unity gain).
    const VnBindingDesc bind = EffectParamBinding(
        VN_TARGET_SCOPE_VOICE_EFFECT_PARAM, VN_EFFECT_GAIN,
        harken::kGainParamGainDb, 0.0f, 6.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &bind));
    ASSERT_EQ(VN_OK, Vn_ApplyVoiceBinding(handle, harkentest::ObjectId(1)));
    ASSERT_EQ(VN_OK, Vn_SetValue(kVar, 1.0f));
    harkentest::AdvanceVariableWrite(session.Engine());

    Capture out;
    harkentest::PumpAndCapture(session.Engine(), 4, out);
    HARKENTEST_ASSERT_NON_SILENT(out.left);
    // base -6 dB + contribution +6 dB = 0 dB -> output == input amplitude.
    EXPECT_NEAR(kDc, SteadyLevel(out), 2e-3f);

    // Drive the variable to 0 -> contribution 0 dB -> resolved back to base -6.
    ASSERT_EQ(VN_OK, Vn_SetValue(kVar, 0.0f));
    harkentest::AdvanceVariableWrite(session.Engine());
    Capture out2;
    harkentest::PumpAndCapture(session.Engine(), 4, out2);
    EXPECT_NEAR(kDc * std::pow(10.0f, -6.0f / 20.0f), SteadyLevel(out2), 2e-3f);
}

// --- detach returns to base (explicit clear) ---

TEST(EffectParamBindingE2E, VoiceDetachReturnsToBase) {
    EngineSession session(EffectBindConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    VnVoiceHandle handle = PlayActiveDc(session, harkentest::ObjectId(1));

    const VnGainDesc gain = GainDesc(-6.0f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(handle, kSlot, VN_EFFECT_GAIN,
                                       AsBase(&gain)));
    const VnBindingDesc bind = EffectParamBinding(
        VN_TARGET_SCOPE_VOICE_EFFECT_PARAM, VN_EFFECT_GAIN,
        harken::kGainParamGainDb, 0.0f, 6.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &bind));
    ASSERT_EQ(VN_OK, Vn_ApplyVoiceBinding(handle, harkentest::ObjectId(1)));
    ASSERT_EQ(VN_OK, Vn_SetValue(kVar, 1.0f));
    harkentest::AdvanceVariableWrite(session.Engine());
    Capture bound;
    harkentest::PumpAndCapture(session.Engine(), 4, bound);
    ASSERT_NEAR(kDc, SteadyLevel(bound), 2e-3f); // composed to 0 dB

    // Clear the binding: the parameter returns to its base at the next update.
    ASSERT_EQ(VN_OK, Vn_ClearVoiceBinding(handle, harkentest::ObjectId(1)));
    ASSERT_EQ(VN_OK, Vn_Update());
    Capture detached;
    harkentest::PumpAndCapture(session.Engine(), 4, detached);
    EXPECT_NEAR(kDc * std::pow(10.0f, -6.0f / 20.0f), SteadyLevel(detached),
                2e-3f);
}

// --- bound vs unbound direct set: interpose defers, unbound emits directly ---

TEST(EffectParamBindingE2E, BoundDirectSetComposesOnNewBaseNoDoubleEmit) {
    EngineSession session(EffectBindConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    VnVoiceHandle handle = PlayActiveDc(session, harkentest::ObjectId(1));

    const VnGainDesc gain = GainDesc(-6.0f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(handle, kSlot, VN_EFFECT_GAIN,
                                       AsBase(&gain)));
    const VnBindingDesc bind = EffectParamBinding(
        VN_TARGET_SCOPE_VOICE_EFFECT_PARAM, VN_EFFECT_GAIN,
        harken::kGainParamGainDb, 0.0f, 6.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &bind));
    ASSERT_EQ(VN_OK, Vn_ApplyVoiceBinding(handle, harkentest::ObjectId(1)));
    ASSERT_EQ(VN_OK, Vn_SetValue(kVar, 1.0f)); // +6 dB
    harkentest::AdvanceVariableWrite(session.Engine());
    Capture composed;
    harkentest::PumpAndCapture(session.Engine(), 4, composed);
    ASSERT_NEAR(kDc, SteadyLevel(composed), 2e-3f); // base -6 + 6 = 0 dB

    // A direct set while bound retargets the base to -3 but MUST emit nothing
    // outside the fold+flush. Without an intervening Vn_Update the rendered
    // level must still be the last composed value (0 dB): an interpose that
    // fell through to the raw direct emit would render -3 dB here.
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffectParam(handle, kSlot,
                                            harken::kGainParamGainDb, -3.0f));
    Capture noUpdate;
    harkentest::PumpAndCapture(session.Engine(), 4, noUpdate);
    EXPECT_NEAR(kDc, SteadyLevel(noUpdate), 2e-3f); // STILL 0 dB, not raw -3

    // The next update composes the retargeted base with the contribution:
    // -3 dB base + 6 dB = +3 dB.
    ASSERT_EQ(VN_OK, Vn_Update());
    Capture out;
    harkentest::PumpAndCapture(session.Engine(), 4, out);
    EXPECT_NEAR(kDc * std::pow(10.0f, 3.0f / 20.0f), SteadyLevel(out), 3e-3f);
}

TEST(EffectParamBindingE2E, UnboundDirectSetIsUnaffectedByTheStore) {
    EngineSession session(EffectBindConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    VnVoiceHandle handle = PlayActiveDc(session, harkentest::ObjectId(1));

    const VnGainDesc gain = GainDesc(-6.0f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(handle, kSlot, VN_EFFECT_GAIN,
                                       AsBase(&gain)));
    // No binding on this parameter: the direct set emits normally.
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffectParam(handle, kSlot,
                                            harken::kGainParamGainDb, 0.0f));
    Capture out;
    harkentest::PumpAndCapture(session.Engine(), 4, out);
    EXPECT_NEAR(kDc, SteadyLevel(out), 2e-3f); // 0 dB unity, no store interposed
}

// --- install-only base composition (desc value is the base, never directly set) ---

TEST(EffectParamBindingE2E, InstallOnlyBaseComposesUnderMultiply) {
    EngineSession session(EffectBindConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    VnVoiceHandle handle = PlayActiveDc(session, harkentest::ObjectId(2));

    // Delay wet = 1.0 applied by descriptor only (never a direct set); a
    // Multiply 0.5 binding must render at 0.5 * wet, halving the wet path.
    const VnFeedbackDelayDesc delay = DelayDesc(10.0f, 0.0f, 1.0f, 1.0f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(handle, kSlot, VN_EFFECT_FEEDBACK_DELAY,
                                       AsBase(&delay)));
    const VnBindingDesc bind = EffectParamBinding(
        VN_TARGET_SCOPE_VOICE_EFFECT_PARAM, VN_EFFECT_FEEDBACK_DELAY,
        harken::kDelayParamWet, 0.5f, 0.5f); // constant 0.5 contribution
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &bind));
    ASSERT_EQ(VN_OK, Vn_ApplyVoiceBinding(handle, harkentest::ObjectId(1)));
    ASSERT_EQ(VN_OK, Vn_SetValue(kVar, 1.0f));
    harkentest::AdvanceVariableWrite(session.Engine());
    Capture out;
    harkentest::PumpAndCapture(session.Engine(), 8, out);
    HARKENTEST_ASSERT_NON_SILENT(out.left);
    // Analytic steady state past the 480-frame (10 ms) delay-line fill: with
    // feedback 0 the delayed tap is the DC input itself, so the output is
    // (dry + wet_composed) * kDc. wet base 1.0 * Multiply 0.5 = 0.5, giving
    // (1.0 + 0.5) * 0.5 = 0.75; a fold that never happened (wet stays 1.0)
    // would render (1.0 + 1.0) * 0.5 = 1.0.
    const float composed = (1.0f + 0.5f) * kDc;
    const float unfolded = (1.0f + 1.0f) * kDc;
    EXPECT_NEAR(composed, SteadyLevel(out), 2e-3f);
    EXPECT_GT(std::fabs(SteadyLevel(out) - unfolded), 0.1f);
}

TEST(EffectParamBindingE2E,
     CompressorRatioBindingUsesEffectOwnedTransitionAndLocalTarget) {
    EngineSession session(EffectBindConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    VnVoiceHandle handle = PlayActiveDc(session, harkentest::ObjectId(3));

    const VnCompressorDesc compressor = CompressorDesc();
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(
                         handle, kSlot, VN_EFFECT_COMPRESSOR,
                         AsBase(&compressor)));
    const VnBindingDesc bind = EffectParamBinding(
        VN_TARGET_SCOPE_VOICE_EFFECT_PARAM, VN_EFFECT_COMPRESSOR,
        harken::kCompressorParamRatio, 4.0f, 4.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &bind));
    ASSERT_EQ(VN_OK, Vn_ApplyVoiceBinding(handle, harkentest::ObjectId(1)));
    ASSERT_EQ(VN_OK, Vn_SetValue(kVar, 1.0f));
    harkentest::AdvanceVariableWrite(session.Engine());

    Capture out;
    harkentest::PumpAndCapture(session.Engine(), 4, out);
    harken::VoiceRenderContext* render = session.Engine()->voices.Resolve(
        harkentest::RenderHandle(session.Engine(), handle));
    ASSERT_NE(nullptr, render);
    ASSERT_EQ(VN_EFFECT_COMPRESSOR, render->fxSlotEffectRef[kSlot].type);
    const auto* state = static_cast<const harken::CompressorState*>(
        render->fxSlots[kSlot].state);
    ASSERT_NE(nullptr, state);
    EXPECT_FLOAT_EQ(0.25f, state->targetParameters.reciprocalRatio);
    EXPECT_EQ(0u, state->parameterTransitionFramesRemaining);
    EXPECT_LT(SteadyLevel(out), kDc - 0.05f);
}

TEST(EffectParamBindingE2E,
     VoiceRecipeInstallsCompressorBeforeTheFirstAudibleBlock) {
    EngineSession session(EffectBindConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    static const std::vector<float> dc(kRate, kDc);
    const VnPCMDesc pcm = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(4), &pcm));
    InstallCompressorRecipe(session.Engine(), harkentest::ObjectId(4));

    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(4));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    Capture out;
    harkentest::PumpAndCapture(session.Engine(), 4, out);

    harken::VoiceRenderContext* render = session.Engine()->voices.Resolve(
        harkentest::RenderHandle(session.Engine(), handle));
    ASSERT_NE(nullptr, render);
    ASSERT_EQ(VN_EFFECT_COMPRESSOR, render->fxSlotEffectRef[kSlot].type);
    const auto* state = static_cast<const harken::CompressorState*>(
        render->fxSlots[kSlot].state);
    ASSERT_NE(nullptr, state);
    EXPECT_FLOAT_EQ(0.25f, state->parameters.reciprocalRatio);
    EXPECT_FLOAT_EQ(-18.0f, state->parameters.thresholdDb);
    EXPECT_LT(SteadyLevel(out), kDc - 0.05f);
}

// --- bus scope: rendered composition + detach ---

TEST(EffectParamBindingE2E, BusGainBindingComposesAndDetaches) {
    EngineSession session(EffectBindConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    // A DC voice routes to the master bus by default; the bus gain effect scales
    // the master mix, so the rendered level reads the composed bus gainDb.
    VnVoiceHandle handle = PlayActiveDc(session, harkentest::ObjectId(5));
    (void)handle;

    const VnGainDesc gain = GainDesc(-6.0f);
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(VN_BUS_MASTER, kSlot, VN_EFFECT_GAIN,
                                     AsBase(&gain)));
    const VnBindingDesc bind = EffectParamBinding(
        VN_TARGET_SCOPE_BUS_EFFECT_PARAM, VN_EFFECT_GAIN,
        harken::kGainParamGainDb, 0.0f, 6.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &bind));
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(VnBindingAttachDesc);
    attach.scopeKind = VN_TARGET_SCOPE_BUS_EFFECT_PARAM;
    attach.objectId = VN_BUS_MASTER;
    ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(1), &attach));
    ASSERT_EQ(VN_OK, Vn_SetValue(kVar, 1.0f));
    harkentest::AdvanceVariableWrite(session.Engine());

    Capture out;
    harkentest::PumpAndCapture(session.Engine(), 4, out);
    HARKENTEST_ASSERT_NON_SILENT(out.left);
    // base -6 dB + contribution +6 dB = 0 dB: the master mix passes unattenuated
    // (the DC voice renders kDc per channel at unity, as the voice test pins).
    EXPECT_NEAR(kDc, SteadyLevel(out), 3e-3f);

    // Detach: the bus parameter returns to its base at the next update.
    ASSERT_EQ(VN_OK, Vn_DetachBinding(harkentest::ObjectId(1), &attach));
    ASSERT_EQ(VN_OK, Vn_Update());
    Capture based;
    harkentest::PumpAndCapture(session.Engine(), 4, based);
    EXPECT_NEAR(kDc * std::pow(10.0f, -6.0f / 20.0f), SteadyLevel(based), 3e-3f);
}

// --- bus attach enforces CanBind: empty slot / type mismatch refused ---

TEST(EffectParamBindingE2E, BusAttachRefusesUnbindableSlotThenAcceptsMatch) {
    EngineSession session(EffectBindConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    // A bus-effect binding whose locator names slot 0 with a GAIN parameter.
    const VnBindingDesc bind = EffectParamBinding(
        VN_TARGET_SCOPE_BUS_EFFECT_PARAM, VN_EFFECT_GAIN,
        harken::kGainParamGainDb, 0.0f, 6.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &bind));
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(VnBindingAttachDesc);
    attach.scopeKind = VN_TARGET_SCOPE_BUS_EFFECT_PARAM;
    attach.objectId = VN_BUS_MASTER;

    // Negative A: the master bus slot holds no effect, so the locator cannot
    // bind. Attach is refused with zero side effects — no attachment recorded.
    EXPECT_EQ(VN_INVALID_ARG, Vn_AttachBinding(harkentest::ObjectId(1), &attach));
    EXPECT_FALSE(session.Engine()->bindings.HasAttachment(
        harkentest::ObjectId(1), VN_TARGET_SCOPE_BUS_EFFECT_PARAM, VN_BUS_MASTER));

    // Negative B: install a DELAY at the slot the GAIN locator names. The
    // installed type mismatches the locator, so attach is still refused with no
    // attachment recorded.
    const VnFeedbackDelayDesc delay = DelayDesc(10.0f, 0.0f, 1.0f, 1.0f);
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(VN_BUS_MASTER, kSlot,
                                     VN_EFFECT_FEEDBACK_DELAY, AsBase(&delay)));
    EXPECT_EQ(VN_INVALID_ARG, Vn_AttachBinding(harkentest::ObjectId(1), &attach));
    EXPECT_FALSE(session.Engine()->bindings.HasAttachment(
        harkentest::ObjectId(1), VN_TARGET_SCOPE_BUS_EFFECT_PARAM, VN_BUS_MASTER));

    // Positive: replace the slot with a GAIN matching the locator. The same
    // attach now succeeds, proving the earlier refusals were the CanBind gate
    // (empty / type mismatch) and not some unrelated validation.
    const VnGainDesc gain = GainDesc(-6.0f);
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(VN_BUS_MASTER, kSlot, VN_EFFECT_GAIN,
                                     AsBase(&gain)));
    EXPECT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(1), &attach));
    EXPECT_TRUE(session.Engine()->bindings.HasAttachment(
        harkentest::ObjectId(1), VN_TARGET_SCOPE_BUS_EFFECT_PARAM, VN_BUS_MASTER));
}

// --- tone peakGain AddDb smoke: a bound peak boost changes the render ---

TEST(EffectParamBindingE2E, TonePeakGainBindingChangesRender) {
    EngineSession session(EffectBindConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    VnVoiceHandle handle = PlayActiveSine(session, harkentest::ObjectId(6), /*freqHz=*/1000.0);

    const VnThreeBandToneDesc tone = ToneDesc(/*peakGainDb=*/0.0f,
                                              /*peakFreqHz=*/1000.0f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(handle, kSlot, VN_EFFECT_THREE_BAND_TONE,
                                       AsBase(&tone)));
    // AddDb binding on the peak gain: variable 0 -> +0 dB, variable 1 -> +12 dB.
    const VnBindingDesc bind = EffectParamBinding(
        VN_TARGET_SCOPE_VOICE_EFFECT_PARAM, VN_EFFECT_THREE_BAND_TONE,
        harken::kToneParamPeakGainDb, 0.0f, 12.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &bind));
    ASSERT_EQ(VN_OK, Vn_ApplyVoiceBinding(handle, harkentest::ObjectId(1)));

    ASSERT_EQ(VN_OK, Vn_SetValue(kVar, 0.0f)); // +0 dB at the peak
    harkentest::AdvanceVariableWrite(session.Engine());
    Capture flat;
    harkentest::PumpAndCapture(session.Engine(), 8, flat);
    HARKENTEST_ASSERT_NON_SILENT(flat.left);

    ASSERT_EQ(VN_OK, Vn_SetValue(kVar, 1.0f)); // +12 dB at the peak (1 kHz)
    harkentest::AdvanceVariableWrite(session.Engine());
    Capture boosted;
    harkentest::PumpAndCapture(session.Engine(), 8, boosted);

    for (float v : boosted.left) {
        EXPECT_TRUE(std::isfinite(v));
    }
    // Load-bearing: the +12 dB peak boost raises the 1 kHz sine's level; a fold
    // that never reached the kernel would leave it unchanged.
    EXPECT_GT(PeakLevel(boosted), PeakLevel(flat) * 1.05f);
}

// --- live-path attach/apply eligibility (empty slot / wrong type / slot range) ---

TEST(EffectParamBindingE2E, ApplyRefusesUnbindableOnLivePath) {
    EngineSession session(EffectBindConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    VnVoiceHandle handle = PlayActiveDc(session, harkentest::ObjectId(7));

    const VnBindingDesc gainBind = EffectParamBinding(
        VN_TARGET_SCOPE_VOICE_EFFECT_PARAM, VN_EFFECT_GAIN,
        harken::kGainParamGainDb, 0.0f, 6.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &gainBind));
    // Empty slot: no effect installed -> apply refused, zero side effects.
    EXPECT_EQ(VN_INVALID_ARG, Vn_ApplyVoiceBinding(handle, harkentest::ObjectId(1)));

    // Wrong installed type: a DELAY is installed but the locator expects GAIN.
    const VnFeedbackDelayDesc delay = DelayDesc(10.0f, 0.0f, 1.0f, 1.0f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(handle, kSlot, VN_EFFECT_FEEDBACK_DELAY,
                                       AsBase(&delay)));
    EXPECT_EQ(VN_INVALID_ARG, Vn_ApplyVoiceBinding(handle, harkentest::ObjectId(1)));

    // Slot beyond fxSlotsPerVoice (=1): locator slot 1 has no record.
    const VnBindingDesc slot1 = EffectParamBinding(
        VN_TARGET_SCOPE_VOICE_EFFECT_PARAM, VN_EFFECT_GAIN,
        harken::kGainParamGainDb, 0.0f, 6.0f, /*slot=*/1);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(2), &slot1));
    EXPECT_EQ(VN_INVALID_ARG, Vn_ApplyVoiceBinding(handle, harkentest::ObjectId(2)));
}

// --- unregister protection covers voice-effect instances ---

TEST(EffectParamBindingE2E, UnregisterRefusedWhileVoiceEffectApplied) {
    EngineSession session(EffectBindConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    VnVoiceHandle handle = PlayActiveDc(session, harkentest::ObjectId(8));
    const VnGainDesc gain = GainDesc(-6.0f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(handle, kSlot, VN_EFFECT_GAIN,
                                       AsBase(&gain)));
    const VnBindingDesc bind = EffectParamBinding(
        VN_TARGET_SCOPE_VOICE_EFFECT_PARAM, VN_EFFECT_GAIN,
        harken::kGainParamGainDb, 0.0f, 6.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &bind));
    ASSERT_EQ(VN_OK, Vn_ApplyVoiceBinding(handle, harkentest::ObjectId(1)));
    // A live voice-effect instance references the definition: unregister refused.
    EXPECT_EQ(VN_IN_USE, Vn_UnregisterBinding(harkentest::ObjectId(1)));
    // After clearing the binding, unregister succeeds.
    ASSERT_EQ(VN_OK, Vn_ClearVoiceBinding(handle, harkentest::ObjectId(1)));
    EXPECT_EQ(VN_OK, Vn_UnregisterBinding(harkentest::ObjectId(1)));
}

// --- in-place type replace: stale binding does not interpose the direct set ---

TEST(EffectParamBindingE2E, TypeReplaceMakesDirectSetTakeUnboundPath) {
    EngineSession session(EffectBindConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    VnVoiceHandle handle = PlayActiveSine(session, harkentest::ObjectId(9), /*freqHz=*/1000.0);

    const VnGainDesc gain = GainDesc(0.0f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(handle, kSlot, VN_EFFECT_GAIN,
                                       AsBase(&gain)));
    const VnBindingDesc bind = EffectParamBinding(
        VN_TARGET_SCOPE_VOICE_EFFECT_PARAM, VN_EFFECT_GAIN,
        harken::kGainParamGainDb, 0.0f, 6.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &bind));
    ASSERT_EQ(VN_OK, Vn_ApplyVoiceBinding(handle, harkentest::ObjectId(1)));

    // In-place replace the slot with a TONE effect. The gain-locator binding is
    // now stale (its expected type no longer matches the installed type).
    const VnThreeBandToneDesc tone = ToneDesc(0.0f, 1000.0f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(handle, kSlot, VN_EFFECT_THREE_BAND_TONE,
                                       AsBase(&tone)));
    // A direct set on a tone parameter must take the unbound (immediate emit)
    // path — the stale gain binding must not interpose it. Structural check: the
    // direct +12 dB peak boost reaches the kernel and raises the 1 kHz level.
    Capture before;
    harkentest::PumpAndCapture(session.Engine(), 8, before);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffectParam(handle, kSlot,
                                            harken::kToneParamPeakGainDb, 12.0f));
    Capture after;
    harkentest::PumpAndCapture(session.Engine(), 8, after);
    EXPECT_GT(PeakLevel(after), PeakLevel(before) * 1.05f);
}

TEST(EffectParamBindingE2E, BusTypeReplaceMakesDirectSetTakeUnboundPath) {
    EngineSession session(EffectBindConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallBindingVariable(engine);
    VnVoiceHandle handle = PlayActiveSine(session, harkentest::ObjectId(10), /*freqHz=*/1000.0);
    (void)handle;

    const VnGainDesc gain = GainDesc(0.0f);
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(VN_BUS_MASTER, kSlot, VN_EFFECT_GAIN,
                                     AsBase(&gain)));
    const VnBindingDesc bind = EffectParamBinding(
        VN_TARGET_SCOPE_BUS_EFFECT_PARAM, VN_EFFECT_GAIN,
        harken::kGainParamGainDb, 0.0f, 6.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &bind));
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(VnBindingAttachDesc);
    attach.scopeKind = VN_TARGET_SCOPE_BUS_EFFECT_PARAM;
    attach.objectId = VN_BUS_MASTER;
    ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(1), &attach));

    // Replacing the slot leaves the attachment structurally live, but its Gain
    // locator is stale against the newly installed Tone type. The store must be
    // replaced as one complete seven-parameter Tone record set.
    const VnThreeBandToneDesc tone = ToneDesc(0.0f, 1000.0f);
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(VN_BUS_MASTER, kSlot,
                                     VN_EFFECT_THREE_BAND_TONE,
                                     AsBase(&tone)));
    EXPECT_TRUE(engine->bindings.HasAttachment(
        harkentest::ObjectId(1), VN_TARGET_SCOPE_BUS_EFFECT_PARAM, VN_BUS_MASTER));
    EXPECT_EQ(7u, engine->effectParamTargets.LiveRecordCount());
    EXPECT_FLOAT_EQ(
        0.0f,
        engine->effectParamTargets.LastCommitted(
            harken::EffectParamTargetStore::Scope::Bus, 0, kSlot,
            harken::kToneParamPeakGainDb));

    Capture flat;
    harkentest::PumpAndCapture(engine, 8, flat);
    HARKENTEST_ASSERT_NON_SILENT(flat.left);
    EXPECT_NEAR(kDc, PeakLevel(flat), 5e-3f);

    // The stale Gain binding must not interpose this Tone direct set. The set
    // publishes immediately, survives a later binding sweep, and raises the
    // absolute 1 kHz level without mutating the attachment into a Tone binding.
    ASSERT_EQ(VN_OK, Vn_SetBusEffectParam(
                         VN_BUS_MASTER, kSlot,
                         harken::kToneParamPeakGainDb, 12.0f));
    EXPECT_FLOAT_EQ(
        12.0f,
        engine->effectParamTargets.LastCommitted(
            harken::EffectParamTargetStore::Scope::Bus, 0, kSlot,
            harken::kToneParamPeakGainDb));
    ASSERT_EQ(VN_OK, Vn_SetValue(kVar, 1.0f));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_TRUE(engine->bindings.HasAttachment(
        harkentest::ObjectId(1), VN_TARGET_SCOPE_BUS_EFFECT_PARAM, VN_BUS_MASTER));
    EXPECT_EQ(7u, engine->effectParamTargets.LiveRecordCount());
    EXPECT_FLOAT_EQ(
        12.0f,
        engine->effectParamTargets.LastCommitted(
            harken::EffectParamTargetStore::Scope::Bus, 0, kSlot,
            harken::kToneParamPeakGainDb));

    Capture boosted;
    harkentest::PumpAndCapture(engine, 8, boosted);
    HARKENTEST_ASSERT_NON_SILENT(boosted.left);
    EXPECT_GT(PeakLevel(boosted), 0.9f);
    EXPECT_GT(PeakLevel(boosted), PeakLevel(flat) * 1.05f);
    EXPECT_EQ(VN_IN_USE, Vn_UnregisterBinding(harkentest::ObjectId(1)));
}

// --- no store/instance leak across natural voice-end cycles ---

TEST(EffectParamBindingE2E, VoiceEndCyclesDoNotLeakStoreOrInstances) {
    // Small instance + effect pools so a leak exhausts fast: if a naturally
    // ended voice's voice-effect instance or effect instance is NOT reclaimed
    // on the delivered-terminal path, a later cycle's apply/install refuses.
    VnConfig config = EffectBindConfig();
    config.maxBindingAttachments = 4; // sizes the voice-effect instance pool
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    // One definition, applied to a fresh voice each cycle; the voice ends with
    // the binding still applied, so voice-end cleanup (not an explicit clear)
    // must reclaim the instance and the store rows.
    const VnBindingDesc bind = EffectParamBinding(
        VN_TARGET_SCOPE_VOICE_EFFECT_PARAM, VN_EFFECT_GAIN,
        harken::kGainParamGainDb, 0.0f, 6.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &bind));

    for (int cycle = 0; cycle < 10; ++cycle) {
        VnVoiceHandle handle = PlayActiveDc(session, harkentest::ObjectId(100 + cycle));
        const VnGainDesc gain = GainDesc(-6.0f);
        ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(handle, kSlot, VN_EFFECT_GAIN,
                                           AsBase(&gain)));
        ASSERT_EQ(VN_OK, Vn_ApplyVoiceBinding(handle, harkentest::ObjectId(1)));
        ASSERT_EQ(VN_OK, Vn_SetValue(kVar, 1.0f));
        harkentest::AdvanceVariableWrite(session.Engine());
        Capture out;
        harkentest::PumpAndCapture(session.Engine(), 4, out);
        EXPECT_NEAR(kDc, SteadyLevel(out), 3e-3f); // composed 0 dB each cycle

        // End the voice naturally (no explicit clear): the delivered terminal
        // notification's reap must reclaim the effect + binding-instance + store
        // state, or a later cycle exhausts a pool and the asserts above fail.
        ASSERT_EQ(VN_OK, Vn_Stop(handle, 0));
        Capture drain;
        harkentest::PumpAndCapture(session.Engine(), 8, drain);
        ASSERT_EQ(VN_OK, Vn_Update()); // process terminal notification -> cleanup
    }
}

// --- deferred-start voice seeds the store and composes ---

TEST(EffectParamBindingE2E, DeferredStartVoiceSeedsAndComposes) {
    // Steal setup (per StealTest): a one-voice pool, a second play steals the
    // first and its own start is DEFERRED to the next update.
    VnConfig config = EffectBindConfig();
    config.maxVoices = 1;
    config.voiceStealPolicy = 2; // steal-oldest: the incoming start defers
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallBindingVariable(engine);
    static const std::vector<float> dc(kRate, kDc);
    const VnPCMDesc pcm = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));

    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle first = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &first));
    engine->nullBackend.Pump(1); // activate the pool occupant
    VnVoiceHandle incoming = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &incoming)); // steals -> deferred start

    // Stage a gain effect on the deferred voice; it installs at the deferred
    // start, where the store MUST be seeded (the original gap).
    const VnGainDesc gain = GainDesc(-6.0f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(incoming, kSlot, VN_EFFECT_GAIN,
                                       AsBase(&gain)));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update()); // deferred start issues play + install + seed
    engine->nullBackend.Pump(1);   // audio applies

    // Apply a binding: it must succeed (seeded record) and compose.
    const VnBindingDesc bind = EffectParamBinding(
        VN_TARGET_SCOPE_VOICE_EFFECT_PARAM, VN_EFFECT_GAIN,
        harken::kGainParamGainDb, 0.0f, 6.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &bind));
    ASSERT_EQ(VN_OK, Vn_ApplyVoiceBinding(incoming, harkentest::ObjectId(1)));
    ASSERT_EQ(VN_OK, Vn_SetValue(kVar, 1.0f));
    harkentest::AdvanceVariableWrite(session.Engine());
    Capture out;
    harkentest::PumpAndCapture(engine, 4, out);
    EXPECT_NEAR(kDc, SteadyLevel(out), 3e-3f); // base -6 + 6 = 0 dB

    // A bound direct-set on the deferred voice must INTERPOSE, not be swallowed
    // (the original silent no-op). No update -> still composed; update -> the
    // retargeted base composes: -3 + 6 = +3 dB.
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffectParam(incoming, kSlot,
                                            harken::kGainParamGainDb, -3.0f));
    Capture noUpdate;
    harkentest::PumpAndCapture(engine, 4, noUpdate);
    EXPECT_NEAR(kDc, SteadyLevel(noUpdate), 3e-3f); // still 0 dB
    ASSERT_EQ(VN_OK, Vn_Update());
    Capture composed;
    harkentest::PumpAndCapture(engine, 4, composed);
    EXPECT_NEAR(kDc * std::pow(10.0f, 3.0f / 20.0f), SteadyLevel(composed),
                3e-3f);
}

// --- park -> return keeps the effect-parameter binding driving ---

TEST(EffectParamBindingE2E, ParkedBoundVoiceReturnsAndBindingStillDrives) {
    // Audibility-park setup (per ParkTest): a 3D voice on a distance profile,
    // parked by moving the listener far and returned by moving it near.
    VnConfig config = EffectBindConfig();
    config.maxVoices = 2;
    config.maxParkedVoices = 2;
    config.audibilityParkEnabled = 1;
    config.parkThresholdDb = -40.0f;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallBindingVariable(engine);
    static const std::vector<float> dc(kRate, kDc);
    const VnPCMDesc pcm = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    const VnAttenuationDesc atten = harkentest::MakeAttenuationDesc(1.0f, 100.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(9), &atten));

    auto listenerAt = [](float x) {
        ASSERT_EQ(VN_OK, Vn_SetListener(harkentest::Vec3(x, 0.0f, 0.0f),
                                        harkentest::Vec3(0.0f, 0.0f, -1.0f),
                                        harkentest::Vec3(0.0f, 1.0f, 0.0f)));
    };

    const VnPlayDesc play = harkentest::MakePlayDesc3D(harkentest::ObjectId(1), 100.0f, 0.0f, 0.0f);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, harkentest::ObjectId(9)));
    listenerAt(99.0f); // distance 1 -> ~0 dB -> audible
    engine->nullBackend.Pump(1); // activate the voice

    const VnGainDesc gain = GainDesc(-6.0f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(handle, kSlot, VN_EFFECT_GAIN,
                                       AsBase(&gain)));
    const VnBindingDesc bind = EffectParamBinding(
        VN_TARGET_SCOPE_VOICE_EFFECT_PARAM, VN_EFFECT_GAIN,
        harken::kGainParamGainDb, 0.0f, 6.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &bind));
    ASSERT_EQ(VN_OK, Vn_ApplyVoiceBinding(handle, harkentest::ObjectId(1)));
    ASSERT_EQ(VN_OK, Vn_SetValue(kVar, 1.0f));
    harkentest::AdvanceVariableWrite(session.Engine());

    // Park: listener far -> inaudible. The voice-effect binding keeps its pool
    // slot as a held reservation and the vacated slot's store records clear.
    listenerAt(0.0f); // distance 100 -> attenuation floor -> inaudible
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update()); // parks
    engine->nullBackend.Pump(1);   // audio applies the internal stop
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(handle).kind);

    // Return: listener near -> audible. The return re-seeds the effect and
    // retargets the reserved binding instance onto the fresh render slot.
    listenerAt(99.0f);
    ASSERT_EQ(VN_OK, Vn_Update()); // returns
    engine->nullBackend.Pump(1);
    ASSERT_NE(nullptr,
              engine->voices.Resolve(harkentest::RenderHandle(engine, handle)));

    // The binding still drives: composed 0 dB (var 1) renders louder than the
    // base -6 dB (var 0). Use both channels so the ratio isolates the
    // composed gain across both the point-source and fully widened images.
    for (float width : {0.0f, 1.0f}) {
        SCOPED_TRACE(width);
        ASSERT_EQ(VN_OK, Vn_SetVoiceWidth(handle, width));
        ASSERT_EQ(VN_OK, Vn_SetValue(kVar, 1.0f));
        harkentest::AdvanceVariableWrite(session.Engine());
        Capture high;
        ASSERT_NO_FATAL_FAILURE(CaptureFreshStereo(engine, 4, high));
        ASSERT_EQ(VN_OK, Vn_SetValue(kVar, 0.0f));
        harkentest::AdvanceVariableWrite(session.Engine());
        Capture low;
        ASSERT_NO_FATAL_FAILURE(CaptureFreshStereo(engine, 4, low));
        ASSERT_GT(SteadyStereoLevel(low), harkentest::kSilenceFloor);
        EXPECT_GT(SteadyStereoLevel(high), SteadyStereoLevel(low) * 1.05f);
    }

    // Detach after return cleans up: the voice keeps rendering on the base.
    ASSERT_EQ(VN_OK, Vn_ClearVoiceBinding(handle, harkentest::ObjectId(1)));
    ASSERT_EQ(VN_OK, Vn_Update());
    Capture afterDetach;
    ASSERT_NO_FATAL_FAILURE(CaptureFreshStereo(engine, 4, afterDetach));
    ASSERT_GT(SteadyStereoLevel(afterDetach), harkentest::kSilenceFloor);
}

// --- park reservation: a parked voice's binding slot stays booked ---

TEST(EffectParamBindingE2E, ParkedVoiceEffectBindingReservationRefusesNewApply) {
    // Instance pool of exactly one: voice A applies the only slot then parks, so
    // the slot must stay RESERVED while parked. A second live voice's apply must
    // then be refused for want of capacity, and A's binding must still drive
    // after it returns. Under a park that freed the slot, B's apply would wrongly
    // succeed and A would silently lose its binding on return.
    VnConfig config = EffectBindConfig();
    config.maxVoices = 2;
    config.maxParkedVoices = 1;
    config.audibilityParkEnabled = 1;
    config.parkThresholdDb = -40.0f;
    config.maxBindingAttachments = 1; // sizes the voice-effect instance pool to 1
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallBindingVariable(engine);
    static const std::vector<float> dc(kRate, kDc);
    const VnPCMDesc pcm = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    const VnAttenuationDesc atten = harkentest::MakeAttenuationDesc(1.0f, 100.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(9), &atten));
    auto listenerAt = [](float x) {
        ASSERT_EQ(VN_OK, Vn_SetListener(harkentest::Vec3(x, 0.0f, 0.0f),
                                        harkentest::Vec3(0.0f, 0.0f, -1.0f),
                                        harkentest::Vec3(0.0f, 1.0f, 0.0f)));
    };

    // Voice A: a 3D voice on an attenuation profile (parkable by distance).
    const VnPlayDesc playA = harkentest::MakePlayDesc3D(harkentest::ObjectId(1), 100.0f, 0.0f, 0.0f);
    VnVoiceHandle a = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&playA, &a));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(a, harkentest::ObjectId(9)));
    listenerAt(99.0f); // distance 1 -> audible
    engine->nullBackend.Pump(1);
    const VnGainDesc gain = GainDesc(-6.0f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(a, kSlot, VN_EFFECT_GAIN, AsBase(&gain)));
    const VnBindingDesc bind = EffectParamBinding(
        VN_TARGET_SCOPE_VOICE_EFFECT_PARAM, VN_EFFECT_GAIN,
        harken::kGainParamGainDb, 0.0f, 6.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &bind));
    ASSERT_EQ(VN_OK, Vn_ApplyVoiceBinding(a, harkentest::ObjectId(1))); // consumes the only slot

    // Voice B: a plain 2D voice (never parks) with the same effect installed so
    // its apply is eligible and reaches the pool-capacity check.
    VnVoiceHandle b = PlayActiveDc(session, harkentest::ObjectId(2));
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(b, kSlot, VN_EFFECT_GAIN, AsBase(&gain)));

    // Park A: listener far -> A inaudible -> parks. Its instance keeps its slot.
    listenerAt(0.0f);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update()); // parks A
    engine->nullBackend.Pump(1);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(a).kind);

    // The reservation holds: B's apply is refused for want of capacity.
    EXPECT_EQ(VN_NO_BINDING, Vn_ApplyVoiceBinding(b, harkentest::ObjectId(1)));

    // Stop B so only A renders for the drive check below.
    ASSERT_EQ(VN_OK, Vn_Stop(b, 0));
    engine->nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_Update());

    // Return A: its reserved binding retargets onto the fresh slot and drives.
    listenerAt(99.0f);
    ASSERT_EQ(VN_OK, Vn_Update()); // returns A
    engine->nullBackend.Pump(1);
    ASSERT_NE(nullptr,
              engine->voices.Resolve(harkentest::RenderHandle(engine, a)));
    ASSERT_EQ(VN_OK, Vn_SetValue(kVar, 1.0f));
    harkentest::AdvanceVariableWrite(session.Engine());
    Capture high;
    ASSERT_NO_FATAL_FAILURE(CaptureFreshStereo(engine, 4, high));
    ASSERT_EQ(VN_OK, Vn_SetValue(kVar, 0.0f));
    harkentest::AdvanceVariableWrite(session.Engine());
    Capture low;
    ASSERT_NO_FATAL_FAILURE(CaptureFreshStereo(engine, 4, low));
    ASSERT_GT(SteadyStereoLevel(low), harkentest::kSilenceFloor);
    EXPECT_GT(SteadyStereoLevel(high), SteadyStereoLevel(low) * 1.05f);
}

TEST(EffectParamBindingE2E,
     ParkedOneShotNaturalEndReleasesBindingReservation) {
    VnConfig config = EffectBindConfig();
    config.maxVoices = 2;
    config.maxParkedVoices = 1;
    config.audibilityParkEnabled = 1;
    config.parkThresholdDb = -40.0f;
    config.maxBindingAttachments = 1; // one voice-effect instance slot
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallBindingVariable(engine);

    static const std::vector<float> shortDc(kBlock * 6, kDc);
    const VnPCMDesc shortPcm = MonoDesc(shortDc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &shortPcm));
    const VnAttenuationDesc atten =
        harkentest::MakeAttenuationDesc(1.0f, 100.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(9), &atten));
    auto listenerAt = [](float x) {
        ASSERT_EQ(VN_OK, Vn_SetListener(harkentest::Vec3(x, 0.0f, 0.0f),
                                        harkentest::Vec3(0.0f, 0.0f, -1.0f),
                                        harkentest::Vec3(0.0f, 1.0f, 0.0f)));
    };

    const VnPlayDesc play = harkentest::MakePlayDesc3D(harkentest::ObjectId(1), 100.0f, 0.0f, 0.0f);
    VnVoiceHandle parked = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &parked));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(parked, harkentest::ObjectId(9)));
    listenerAt(99.0f);
    engine->nullBackend.Pump(1);
    const VnGainDesc gain = GainDesc(-6.0f);
    ASSERT_EQ(VN_OK,
              Vn_SetVoiceEffect(parked, kSlot, VN_EFFECT_GAIN, AsBase(&gain)));
    const VnBindingDesc bind = EffectParamBinding(
        VN_TARGET_SCOPE_VOICE_EFFECT_PARAM, VN_EFFECT_GAIN,
        harken::kGainParamGainDb, 0.0f, 6.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &bind));
    ASSERT_EQ(VN_OK, Vn_ApplyVoiceBinding(parked, harkentest::ObjectId(1)));

    listenerAt(0.0f);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(parked).kind);

    // Advance virtual time until the parked one-shot ends without returning.
    for (int i = 0;
         i < 16 && engine->ledger.Classify(parked).kind ==
                       harken::VoiceLedger::HandleKind::Parked;
         ++i) {
        ASSERT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(1);
    }
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Stale,
              engine->ledger.Classify(parked).kind);
    ASSERT_EQ(VN_OK, Vn_UnregisterBinding(harkentest::ObjectId(1)));

    // The only instance slot is free again for a fresh voice and definition.
    VnVoiceHandle fresh = PlayActiveDc(session, harkentest::ObjectId(2));
    ASSERT_EQ(VN_OK,
              Vn_SetVoiceEffect(fresh, kSlot, VN_EFFECT_GAIN, AsBase(&gain)));
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(2), &bind));
    EXPECT_EQ(VN_OK, Vn_ApplyVoiceBinding(fresh, harkentest::ObjectId(2)));
}

// --- multi-binding park: ALL bindings survive the round trip ---

TEST(EffectParamBindingE2E, MultiBindingParkedVoiceReturnsWithAllBindingsIntact) {
    VnConfig config = EffectBindConfig();
    config.maxVoices = 2;
    config.maxParkedVoices = 1;
    config.audibilityParkEnabled = 1;
    config.parkThresholdDb = -40.0f;
    config.maxBindingAttachments = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallBindingVariable(engine);
    static const std::vector<float> dc(kRate, kDc);
    const VnPCMDesc pcm = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    const VnAttenuationDesc atten = harkentest::MakeAttenuationDesc(1.0f, 100.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(9), &atten));
    auto listenerAt = [](float x) {
        ASSERT_EQ(VN_OK, Vn_SetListener(harkentest::Vec3(x, 0.0f, 0.0f),
                                        harkentest::Vec3(0.0f, 0.0f, -1.0f),
                                        harkentest::Vec3(0.0f, 1.0f, 0.0f)));
    };
    const VnPlayDesc play = harkentest::MakePlayDesc3D(harkentest::ObjectId(1), 100.0f, 0.0f, 0.0f);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, harkentest::ObjectId(9)));
    listenerAt(99.0f);
    engine->nullBackend.Pump(1);

    // Two AddDb bindings on the same gainDb parameter, base -12 dB: composed at
    // var 1 is -12 + 6 + 6 = 0 dB. Losing either on return leaves -6 dB.
    const VnGainDesc gain = GainDesc(-12.0f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(handle, kSlot, VN_EFFECT_GAIN,
                                       AsBase(&gain)));
    const VnBindingDesc bind1 = EffectParamBinding(
        VN_TARGET_SCOPE_VOICE_EFFECT_PARAM, VN_EFFECT_GAIN,
        harken::kGainParamGainDb, 0.0f, 6.0f);
    const VnBindingDesc bind2 = EffectParamBinding(
        VN_TARGET_SCOPE_VOICE_EFFECT_PARAM, VN_EFFECT_GAIN,
        harken::kGainParamGainDb, 0.0f, 6.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &bind1));
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(2), &bind2));
    ASSERT_EQ(VN_OK, Vn_ApplyVoiceBinding(handle, harkentest::ObjectId(1)));
    ASSERT_EQ(VN_OK, Vn_ApplyVoiceBinding(handle, harkentest::ObjectId(2)));
    ASSERT_EQ(VN_OK, Vn_SetValue(kVar, 1.0f));
    harkentest::AdvanceVariableWrite(session.Engine());

    // Park then return.
    listenerAt(0.0f);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(handle).kind);
    listenerAt(99.0f);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    ASSERT_NE(nullptr,
              engine->voices.Resolve(harkentest::RenderHandle(engine, handle)));

    // Both bindings drive after return: the var 1 (both fold, +12 dB) vs var 0
    // (neither folds, base -12 dB) level ratio is ~4x. If only one binding
    // replayed the ratio would be ~2x, so a >3x gate proves BOTH survived.
    ASSERT_EQ(VN_OK, Vn_SetValue(kVar, 1.0f));
    harkentest::AdvanceVariableWrite(session.Engine());
    Capture high;
    ASSERT_NO_FATAL_FAILURE(CaptureFreshStereo(engine, 4, high));
    ASSERT_EQ(VN_OK, Vn_SetValue(kVar, 0.0f));
    harkentest::AdvanceVariableWrite(session.Engine());
    Capture low;
    ASSERT_NO_FATAL_FAILURE(CaptureFreshStereo(engine, 4, low));
    ASSERT_GT(SteadyStereoLevel(low), harkentest::kSilenceFloor);
    EXPECT_GT(SteadyStereoLevel(high), SteadyStereoLevel(low) * 3.0f);
}

// --- parked reservation is a live reference: unregister refused ---

TEST(EffectParamBindingE2E, ParkedVoiceEffectBindingRefusesUnregisterThenReturns) {
    VnConfig config = EffectBindConfig();
    config.maxVoices = 2;
    config.maxParkedVoices = 1;
    config.audibilityParkEnabled = 1;
    config.parkThresholdDb = -40.0f;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    InstallBindingVariable(engine);
    static const std::vector<float> dc(kRate, kDc);
    const VnPCMDesc pcm = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    const VnAttenuationDesc atten = harkentest::MakeAttenuationDesc(1.0f, 100.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(9), &atten));
    auto listenerAt = [](float x) {
        ASSERT_EQ(VN_OK, Vn_SetListener(harkentest::Vec3(x, 0.0f, 0.0f),
                                        harkentest::Vec3(0.0f, 0.0f, -1.0f),
                                        harkentest::Vec3(0.0f, 1.0f, 0.0f)));
    };
    const VnPlayDesc play = harkentest::MakePlayDesc3D(harkentest::ObjectId(1), 100.0f, 0.0f, 0.0f);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, harkentest::ObjectId(9)));
    listenerAt(99.0f);
    engine->nullBackend.Pump(1);
    const VnGainDesc gain = GainDesc(-6.0f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(handle, kSlot, VN_EFFECT_GAIN,
                                       AsBase(&gain)));
    const VnBindingDesc bind = EffectParamBinding(
        VN_TARGET_SCOPE_VOICE_EFFECT_PARAM, VN_EFFECT_GAIN,
        harken::kGainParamGainDb, 0.0f, 6.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &bind));
    ASSERT_EQ(VN_OK, Vn_ApplyVoiceBinding(handle, harkentest::ObjectId(1)));
    ASSERT_EQ(VN_OK, Vn_SetValue(kVar, 1.0f));
    harkentest::AdvanceVariableWrite(session.Engine());

    // Park: the parked voice keeps its binding as a held reservation.
    listenerAt(0.0f);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(handle).kind);

    // The parked reservation is a live reference: unregister is refused, so a
    // recycled definition slot can never be driven by the parked instance.
    EXPECT_EQ(VN_IN_USE, Vn_UnregisterBinding(harkentest::ObjectId(1)));

    // Return: the binding still drives (var 1 louder than var 0).
    listenerAt(99.0f);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    ASSERT_NE(nullptr,
              engine->voices.Resolve(harkentest::RenderHandle(engine, handle)));
    ASSERT_EQ(VN_OK, Vn_SetValue(kVar, 1.0f));
    harkentest::AdvanceVariableWrite(session.Engine());
    Capture high;
    ASSERT_NO_FATAL_FAILURE(CaptureFreshStereo(engine, 4, high));
    ASSERT_EQ(VN_OK, Vn_SetValue(kVar, 0.0f));
    harkentest::AdvanceVariableWrite(session.Engine());
    Capture low;
    ASSERT_NO_FATAL_FAILURE(CaptureFreshStereo(engine, 4, low));
    ASSERT_GT(SteadyStereoLevel(low), harkentest::kSilenceFloor);
    EXPECT_GT(SteadyStereoLevel(high), SteadyStereoLevel(low) * 1.05f);

    // After return + clear, no reference lingers: unregister succeeds.
    ASSERT_EQ(VN_OK, Vn_ClearVoiceBinding(handle, harkentest::ObjectId(1)));
    EXPECT_EQ(VN_OK, Vn_UnregisterBinding(harkentest::ObjectId(1)));
}

// --- freq (AddLogHz) binding composes and reaches the kernel ---

TEST(EffectParamBindingE2E, VoiceFreqBindingRetunesPeakAndEmits) {
    // A behavioral witness that a composed frequency reaches the audio thread
    // (never suppressed): a +12 dB peak based at 500 Hz is retuned by an AddLogHz
    // binding onto a 1 kHz sine, so the sine's level rises only if the composed
    // freq was actually emitted.
    EngineSession session(EffectBindConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    VnVoiceHandle handle = PlayActiveSine(session, harkentest::ObjectId(6), /*freqHz=*/1000.0);

    const VnThreeBandToneDesc tone = ToneDesc(/*peakGainDb=*/12.0f,
                                              /*peakFreqHz=*/500.0f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(handle, kSlot, VN_EFFECT_THREE_BAND_TONE,
                                       AsBase(&tone)));
    // AddLogHz on peakFreqHz: var 0 -> +0 octaves (peak stays 500 Hz), var 1 ->
    // +1 octave (peak retunes to 1000 Hz = the sine).
    const VnBindingDesc bind = EffectParamBinding(
        VN_TARGET_SCOPE_VOICE_EFFECT_PARAM, VN_EFFECT_THREE_BAND_TONE,
        harken::kToneParamPeakFreqHz, 0.0f, 1.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &bind));
    ASSERT_EQ(VN_OK, Vn_ApplyVoiceBinding(handle, harkentest::ObjectId(1)));

    ASSERT_EQ(VN_OK, Vn_SetValue(kVar, 0.0f)); // peak at 500 Hz
    harkentest::AdvanceVariableWrite(session.Engine());
    Capture off;
    harkentest::PumpAndCapture(session.Engine(), 8, off);
    HARKENTEST_ASSERT_NON_SILENT(off.left);

    ASSERT_EQ(VN_OK, Vn_SetValue(kVar, 1.0f)); // peak retuned to 1000 Hz
    harkentest::AdvanceVariableWrite(session.Engine());
    Capture onPeak;
    harkentest::PumpAndCapture(session.Engine(), 8, onPeak);
    for (float v : onPeak.left) {
        EXPECT_TRUE(std::isfinite(v));
    }
    // The composed freq reached the kernel: the boost lands on the sine and its
    // level rises. A suppressed or never-emitted freq update leaves the peak at
    // 500 Hz and the 1 kHz level unchanged.
    EXPECT_GT(PeakLevel(onPeak), PeakLevel(off) * 1.05f);
}

// --- refusal battery ---

TEST(EffectParamBindingE2E, RefusalBattery) {
    EngineSession session(EffectBindConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    // None-domain parameter (delay feedback) refuses at REGISTER (fail-early).
    const VnBindingDesc noneDomain = EffectParamBinding(
        VN_TARGET_SCOPE_VOICE_EFFECT_PARAM, VN_EFFECT_FEEDBACK_DELAY,
        harken::kDelayParamFeedback, 0.0f, 0.5f);
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterBinding(harkentest::ObjectId(10), &noneDomain));

    // Out-of-range parameter index for the type refuses at register.
    const VnBindingDesc badIndex =
        EffectParamBinding(VN_TARGET_SCOPE_VOICE_EFFECT_PARAM, VN_EFFECT_GAIN,
                           harken::kGainParamCount, 0.0f, 1.0f);
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterBinding(harkentest::ObjectId(11), &badIndex));

    // A valid voice-effect definition attached via the playable-id attach path
    // is refused: a voice-effect binding applies to a live voice, and the
    // playable-id (Recipe) attach path for it is not supported yet.
    const VnBindingDesc voiceDef = EffectParamBinding(
        VN_TARGET_SCOPE_VOICE_EFFECT_PARAM, VN_EFFECT_GAIN,
        harken::kGainParamGainDb, 0.0f, 6.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(12), &voiceDef));
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(VnBindingAttachDesc);
    attach.scopeKind = VN_TARGET_SCOPE_VOICE_EFFECT_PARAM;
    attach.objectId = harkentest::ObjectId(1); // a playable id
    EXPECT_EQ(VN_INVALID_ARG, Vn_AttachBinding(harkentest::ObjectId(12), &attach));

    // Applying to a stale voice handle refuses VN_STALE_HANDLE.
    EXPECT_EQ(VN_STALE_HANDLE, Vn_ApplyVoiceBinding(/*bogus handle*/ 999999, harkentest::ObjectId(12)));

    // Frozen structural identity: Replace may not change scope/target/variable.
    VnBindingDesc moved = voiceDef;
    moved.targetId = harken::PackEffectParamLocator(
        VN_EFFECT_GAIN, /*slot=*/1, harken::kGainParamGainDb);
    EXPECT_EQ(VN_INVALID_ARG, Vn_ReplaceBinding(harkentest::ObjectId(12), &moved));
}

} // namespace
