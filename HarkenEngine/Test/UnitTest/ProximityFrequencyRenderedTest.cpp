// Rendered pins for authored proximity frequency. New descriptors carry
// the authored corner; legacy descriptors retain the 250 Hz fallback.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace {

using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::MakeAttenuationDesc;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc3D;
using harkentest::MakeSine;
using harkentest::MonoDesc;

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint32_t kBlockFrames = 256;
constexpr float kAmp = 0.35f;
constexpr VnAttenuationId kProfile = harkentest::ObjectId(13);
constexpr std::uint32_t kBlocks = 60;
constexpr std::uint32_t kSettleFrames = 960;
constexpr std::uint32_t kWindowFrames = kBlocks * kBlockFrames - kSettleFrames;
constexpr std::uint32_t kPcmFrames = kBlocks * kBlockFrames + kBlockFrames;
constexpr float kAnchorHz = 4000.0f;
constexpr float kProbeLowHz = 200.0f;
constexpr float kProbeHighHz = 250.0f;
constexpr float kDistance = 5.0f;
constexpr float kToleranceDb = 0.5f;
constexpr float kCornerGainDb = 3.0f;
constexpr float kSeparationMarginDb = 2.0f;

float WindowRms(const Capture& captured) {
    double sumSq = 0.0;
    for (std::uint32_t i = kSettleFrames; i < kBlocks * kBlockFrames; ++i) {
        const double sample = static_cast<double>(captured.left[i]);
        sumSq += sample * sample;
    }
    return static_cast<float>(std::sqrt(sumSq / kWindowFrames));
}

VnAttenuationDesc MakeNearFieldProfile() {
    VnAttenuationDesc desc = MakeAttenuationDesc(1.0f, 101.0f);
    desc.proximityRadius = 10.0f;
    desc.proximityMaxBoostDb = 12.0f;
    desc.volumeCurve.enabled = 1;
    desc.volumeCurve.pointCount = 1;
    desc.volumeCurve.points[0].x = 0.0f;
    desc.volumeCurve.points[0].y = 0.0f;
    return desc;
}

VnAttenuationDesc MakeAuthoredProfile(float cornerHz) {
    VnAttenuationDesc desc = MakeNearFieldProfile();
    desc.proximityFrequency = cornerHz;
    return desc;
}

float RenderShelfRms(const VnAttenuationDesc& desc, float freqHz) {
    EngineSession session(MakeNullConfig(2, kBlockFrames, kSampleRate));
    EXPECT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::vector<float> sine =
        MakeSine(kPcmFrames, freqHz, kSampleRate, kAmp);
    const VnPCMDesc pcm = MonoDesc(sine, kSampleRate);
    EXPECT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    EXPECT_EQ(VN_OK, Vn_RegisterAttenuation(kProfile, &desc));
    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 0.0f, 0.0f, -kDistance);
    VnVoiceHandle handle = 0;
    EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    EXPECT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, kProfile));
    EXPECT_EQ(VN_OK, Vn_Update());
    Capture captured;
    harkentest::PumpAndCapture(engine, kBlocks, captured);
    return WindowRms(captured);
}

float ShelfGainDb(const VnAttenuationDesc& desc, float freqHz) {
    const float probe = RenderShelfRms(desc, freqHz);
    const float anchor = RenderShelfRms(desc, kAnchorHz);
    EXPECT_GT(probe, harkentest::kSilenceFloor);
    EXPECT_GT(anchor, harkentest::kSilenceFloor);
    return 20.0f * std::log10(probe / anchor);
}

TEST(ProximityFrequencyRendered, AuthoredCornerDrivesRenderedShelf) {
    const VnAttenuationDesc profileA = MakeAuthoredProfile(200.0f);
    const VnAttenuationDesc profileB = MakeAuthoredProfile(500.0f);
    const float gainAAtA = ShelfGainDb(profileA, 200.0f);
    const float gainBAtB = ShelfGainDb(profileB, 500.0f);
    EXPECT_NEAR(kCornerGainDb, gainAAtA, kToleranceDb);
    EXPECT_NEAR(kCornerGainDb, gainBAtB, kToleranceDb);
    const float gainAAt500 = ShelfGainDb(profileA, 500.0f);
    EXPECT_GT(gainBAtB - gainAAt500, kSeparationMarginDb);
}

TEST(ProximityFrequencyRendered, LegacyDescriptorKeepsCompatibilityCorner) {
    VnAttenuationDesc legacy = MakeNearFieldProfile();
    legacy.structSize =
        static_cast<std::uint32_t>(offsetof(VnAttenuationDesc,
                                             proximityFrequency));
    const float gainAtLegacyCorner = ShelfGainDb(legacy, kProbeHighHz);
    EXPECT_NEAR(kCornerGainDb, gainAtLegacyCorner, kToleranceDb);
}

} // namespace
