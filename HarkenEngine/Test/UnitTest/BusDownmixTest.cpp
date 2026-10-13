// The per-bus (non-master) output fold in isolation (spec M5 §6.3 boundary 2),
// the downmix-API refusals, and the config/snapshot vintage edges for the
// multichannel amendment.
//
// Isolated per-bus fold: with mixChannelLayout = 5.1 and deviceChannelLayout =
// 5.1 the master does NOT fold, so the only downmix in the render is a sub-bus
// set to a stereo output layout. Its folded stereo name-places back into the
// mix-width planes (L->FL, R->FR) and routes up to the master, so the captured
// master FL/FR carry exactly the bus's fold — proving the matrix is a property
// of the bus, not a master-only setting.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include "harken/core/ChannelLayout.h"

#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace {

using harkentest::CaptureMulti;
using harkentest::EngineSession;
using harkentest::MakeNullConfig;
using harkentest::MakeNullConfigLayout;
using harkentest::MonoDesc;

constexpr std::uint32_t kBlockFrames = 256;
constexpr float kAmp = 0.25f;
constexpr float kTol = 1e-5f;
constexpr VnBusId kSubBus = harkentest::ObjectId(10);

const float kInv2 = 1.0f / std::sqrt(2.0f);

// A 5.1 -> stereo downmix descriptor with the given Lo<-FL and Ro<-FR gains and
// everything else zero (a clearly non-default matrix).
VnDownmixMatrixDesc MakeStereoFold(float loFromFl, float roFromFr) {
    VnDownmixMatrixDesc desc{};
    desc.structSize = sizeof(VnDownmixMatrixDesc);
    desc.srcLayout = VN_CHANNEL_LAYOUT_5_1;
    desc.dstLayout = VN_CHANNEL_LAYOUT_STEREO;
    desc.coeffs[0 * 8 + 0] = loFromFl; // dst Lo (row 0) from src FL (col 0)
    desc.coeffs[1 * 8 + 1] = roFromFr; // dst Ro (row 1) from src FR (col 1)
    return desc;
}

TEST(BusDownmix, PerBusFoldIsolatedFromMaster) {
    EngineSession session(MakeNullConfigLayout(2, kBlockFrames, 48000,
                                               VN_CHANNEL_LAYOUT_5_1,
                                               VN_CHANNEL_LAYOUT_5_1));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> dc = harkentest::MakeDC(48000, kAmp);
    const VnPCMDesc desc = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSubBus, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_SetBusOutputLayout(kSubBus, VN_CHANNEL_LAYOUT_STEREO));
    const VnDownmixMatrixDesc custom = MakeStereoFold(2.0f, 3.0f);
    ASSERT_EQ(VN_OK, Vn_SetBusDownmixMatrix(kSubBus, &custom));

    // A mono 2D voice into the sub-bus lands on the front L/R pair (each = amp);
    // the custom fold then scales FL by 2 and FR by 3.
    VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    play.busId = kSubBus;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    CaptureMulti cap;
    harkentest::PumpAndCaptureMulti(engine, 2, cap);
    ASSERT_EQ(6u, cap.channelCount);
    const std::size_t s = kBlockFrames + kBlockFrames / 2;
    ASSERT_GT(std::fabs(cap.channels[0][s]), harkentest::kSilenceFloor)
        << "silence-guard: compared signal is silent";
    EXPECT_NEAR(2.0f * kAmp, cap.channels[0][s], kTol); // master FL
    EXPECT_NEAR(3.0f * kAmp, cap.channels[1][s], kTol); // master FR
    EXPECT_NEAR(0.0f, cap.channels[2][s], kTol);        // FC untouched
    EXPECT_NEAR(0.0f, cap.channels[4][s], kTol);        // BL untouched

    // Reverting to the ITU default (Lo = FL + .7071 FC + .7071 BL; here FC=BL=0)
    // makes both fronts carry amp again.
    ASSERT_EQ(VN_OK, Vn_ResetBusDownmixMatrix(kSubBus));
    CaptureMulti cap2;
    harkentest::PumpAndCaptureMulti(engine, 2, cap2);
    EXPECT_NEAR(kAmp, cap2.channels[0][s], kTol);
    EXPECT_NEAR(kAmp, cap2.channels[1][s], kTol);
    (void)kInv2;
}

TEST(BusDownmix, MonoOutputBusPlacesToCenterWhenMixHasIt) {
    // Name placement (spec §6.3): a sub-bus with a MONO output layer folds
    // 5.1 -> mono, and the mono channel (role FC) name-places into the 5.1
    // mix's center — and nowhere else (no synthesis).
    EngineSession session(MakeNullConfigLayout(2, kBlockFrames, 48000,
                                               VN_CHANNEL_LAYOUT_5_1,
                                               VN_CHANNEL_LAYOUT_5_1));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> dc = harkentest::MakeDC(48000, kAmp);
    const VnPCMDesc desc = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSubBus, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_SetBusOutputLayout(kSubBus, VN_CHANNEL_LAYOUT_MONO));

    VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    play.busId = kSubBus;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    CaptureMulti cap;
    harkentest::PumpAndCaptureMulti(engine, 2, cap);
    ASSERT_EQ(6u, cap.channelCount);
    const std::size_t s = kBlockFrames + kBlockFrames / 2;
    // bus10 (5.1): mono 2D -> FL=FR=amp; the default 5.1->mono fold gives
    // M = .7071 FL + .7071 FR = sqrt(2)*amp; placed at the 5.1 center (idx 2).
    const float expected = kAmp * 2.0f * kInv2;
    ASSERT_GT(std::fabs(cap.channels[2][s]), harkentest::kSilenceFloor)
        << "silence-guard: compared signal is silent";
    EXPECT_NEAR(expected, cap.channels[2][s], kTol); // FC carries the mono
    EXPECT_NEAR(0.0f, cap.channels[0][s], kTol);     // FL
    EXPECT_NEAR(0.0f, cap.channels[1][s], kTol);     // FR
    EXPECT_NEAR(0.0f, cap.channels[3][s], kTol);     // LFE
    EXPECT_NEAR(0.0f, cap.channels[4][s], kTol);     // BL
    EXPECT_NEAR(0.0f, cap.channels[5][s], kTol);     // BR
}

TEST(BusDownmix, MonoOutputBusDroppedWhenMixHasNoCenter) {
    // The QUAD mix has no FC channel, so the mono output's center role matches
    // nothing and name placement drops it (no upmix synthesis) — the bus is
    // silent. A snapshot confirms the voice is genuinely rendering, so the
    // silence is the drop, not an absent voice.
    EngineSession session(MakeNullConfigLayout(2, kBlockFrames, 48000,
                                               VN_CHANNEL_LAYOUT_QUAD,
                                               VN_CHANNEL_LAYOUT_QUAD));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> dc = harkentest::MakeDC(48000, kAmp);
    const VnPCMDesc desc = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSubBus, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_SetBusOutputLayout(kSubBus, VN_CHANNEL_LAYOUT_MONO));

    VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    play.busId = kSubBus;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    CaptureMulti cap;
    harkentest::PumpAndCaptureMulti(engine, 2, cap);
    ASSERT_EQ(4u, cap.channelCount);
    const std::size_t s = kBlockFrames + kBlockFrames / 2;
    for (std::uint32_t c = 0; c < 4; ++c) {
        EXPECT_NEAR(0.0f, cap.channels[c][s], kTol) << "quad channel " << c;
    }
    VnSnapshot snap{};
    snap.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&snap));
    EXPECT_GE(snap.activeVoiceCount, 1u)
        << "the voice must be rendering — the silence is the drop, not an "
           "absent voice";
}

// --- refusals (spec §6.3 / §11.3) ---

TEST(BusDownmix, SetOutputLayoutRefusals) {
    EngineSession session(MakeNullConfigLayout(1, kBlockFrames, 48000,
                                               VN_CHANNEL_LAYOUT_5_1,
                                               VN_CHANNEL_LAYOUT_5_1));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSubBus, VN_BUS_MASTER));

    // The DEFAULT sentinel is not a concrete output layout.
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetBusOutputLayout(kSubBus, VN_CHANNEL_LAYOUT_DEFAULT));
    // An out-of-range layout value the build cannot honor.
    EXPECT_EQ(VN_UNSUPPORTED_LAYOUT, Vn_SetBusOutputLayout(kSubBus, 99));
    // A 7.1 output is wider than the 5.1 mix.
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetBusOutputLayout(kSubBus, VN_CHANNEL_LAYOUT_7_1));
    // An unknown bus.
    EXPECT_EQ(VN_UNKNOWN_ID,
              Vn_SetBusOutputLayout(harkentest::ObjectId(999), VN_CHANNEL_LAYOUT_STEREO));
    // A narrower-or-equal layout is accepted.
    EXPECT_EQ(VN_OK, Vn_SetBusOutputLayout(kSubBus, VN_CHANNEL_LAYOUT_STEREO));
}

TEST(BusDownmix, SetDownmixMatrixRefusals) {
    EngineSession session(MakeNullConfigLayout(1, kBlockFrames, 48000,
                                               VN_CHANNEL_LAYOUT_5_1,
                                               VN_CHANNEL_LAYOUT_5_1));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSubBus, VN_BUS_MASTER));
    // The bus's fold is 5.1 -> stereo, so a matching desc (src 5.1, dst stereo)
    // reaches the value checks below; the mismatch cases live in their own test.
    ASSERT_EQ(VN_OK, Vn_SetBusOutputLayout(kSubBus, VN_CHANNEL_LAYOUT_STEREO));

    EXPECT_EQ(VN_INVALID_ARG, Vn_SetBusDownmixMatrix(kSubBus, nullptr));

    VnDownmixMatrixDesc badSize = MakeStereoFold(1.0f, 1.0f);
    badSize.structSize = sizeof(VnDownmixMatrixDesc) - 4;
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetBusDownmixMatrix(kSubBus, &badSize));

    // A non-finite coefficient in the read sub-block (the desc matches the
    // bus's 5.1 -> stereo fold, so the value check is what refuses it).
    VnDownmixMatrixDesc nonFinite = MakeStereoFold(1.0f, 1.0f);
    nonFinite.coeffs[0] = std::numeric_limits<float>::infinity();
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetBusDownmixMatrix(kSubBus, &nonFinite));

    // A non-narrowing src/dst pair (stereo -> 5.1).
    VnDownmixMatrixDesc widening = MakeStereoFold(1.0f, 1.0f);
    widening.srcLayout = VN_CHANNEL_LAYOUT_STEREO;
    widening.dstLayout = VN_CHANNEL_LAYOUT_5_1;
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetBusDownmixMatrix(kSubBus, &widening));

    // The DEFAULT sentinel is not a concrete layout the build can honor.
    VnDownmixMatrixDesc sentinel = MakeStereoFold(1.0f, 1.0f);
    sentinel.dstLayout = VN_CHANNEL_LAYOUT_DEFAULT;
    EXPECT_EQ(VN_UNSUPPORTED_LAYOUT, Vn_SetBusDownmixMatrix(kSubBus, &sentinel));

    // An unknown bus (with an otherwise valid desc): the bus resolve precedes
    // the fold-match check, so this is VN_UNKNOWN_ID.
    const VnDownmixMatrixDesc valid = MakeStereoFold(1.0f, 1.0f);
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_SetBusDownmixMatrix(harkentest::ObjectId(999), &valid));
    EXPECT_EQ(VN_OK, Vn_SetBusDownmixMatrix(kSubBus, &valid));
}

TEST(BusDownmix, SetDownmixMatrixRejectsFoldMismatch) {
    // Fold-match validation (spec §6.3/§10): the desc must describe THIS bus's actual fold —
    // src == the mix layout, dst == the bus's output layout (device layout for
    // the master). A matrix for a different fold is VN_INVALID_ARG.
    EngineSession session(MakeNullConfigLayout(1, kBlockFrames, 48000,
                                               VN_CHANNEL_LAYOUT_5_1,
                                               VN_CHANNEL_LAYOUT_5_1));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSubBus, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_SetBusOutputLayout(kSubBus, VN_CHANNEL_LAYOUT_STEREO));

    // Wrong src: quad -> stereo (narrowing and concrete) but src != mix (5.1).
    VnDownmixMatrixDesc wrongSrc = MakeStereoFold(1.0f, 1.0f);
    wrongSrc.srcLayout = VN_CHANNEL_LAYOUT_QUAD;
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetBusDownmixMatrix(kSubBus, &wrongSrc));

    // Wrong dst: 5.1 -> mono but the bus's output layer is stereo.
    VnDownmixMatrixDesc wrongDst = MakeStereoFold(1.0f, 1.0f);
    wrongDst.dstLayout = VN_CHANNEL_LAYOUT_MONO;
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetBusDownmixMatrix(kSubBus, &wrongDst));

    // The matching fold (5.1 -> stereo) is accepted.
    const VnDownmixMatrixDesc match = MakeStereoFold(1.0f, 1.0f);
    EXPECT_EQ(VN_OK, Vn_SetBusDownmixMatrix(kSubBus, &match));

    // The master's fold dst is the device layout (5.1 here): a stereo-dst desc
    // mismatches, so it is refused too.
    VnDownmixMatrixDesc masterWrong = MakeStereoFold(1.0f, 1.0f); // dst stereo
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetBusDownmixMatrix(VN_BUS_MASTER, &masterWrong));
}

TEST(BusDownmix, DeviceLayoutFromNativeChannelsFloorsToSupported) {
    // Native-count mapping (unit-testable half of the follow-device-native resolution):
    // a standard count maps exactly; a non-standard count floors to the widest
    // supported layout not exceeding it (0 floors to stereo defensively).
    using harken::DeviceLayoutFromNativeChannels;
    EXPECT_EQ(VN_CHANNEL_LAYOUT_STEREO, DeviceLayoutFromNativeChannels(0));
    EXPECT_EQ(VN_CHANNEL_LAYOUT_MONO, DeviceLayoutFromNativeChannels(1));
    EXPECT_EQ(VN_CHANNEL_LAYOUT_STEREO, DeviceLayoutFromNativeChannels(2));
    EXPECT_EQ(VN_CHANNEL_LAYOUT_STEREO, DeviceLayoutFromNativeChannels(3));
    EXPECT_EQ(VN_CHANNEL_LAYOUT_QUAD, DeviceLayoutFromNativeChannels(4));
    EXPECT_EQ(VN_CHANNEL_LAYOUT_QUAD, DeviceLayoutFromNativeChannels(5));
    EXPECT_EQ(VN_CHANNEL_LAYOUT_5_1, DeviceLayoutFromNativeChannels(6));
    EXPECT_EQ(VN_CHANNEL_LAYOUT_5_1, DeviceLayoutFromNativeChannels(7));
    EXPECT_EQ(VN_CHANNEL_LAYOUT_7_1, DeviceLayoutFromNativeChannels(8));
    EXPECT_EQ(VN_CHANNEL_LAYOUT_7_1, DeviceLayoutFromNativeChannels(16));
}

TEST(BusDownmix, InitialDeviceRateResolutionPreservesLegacyIdentity) {
    VnConfig config = MakeNullConfig();
    config.deviceMode = VN_DEVICE_REAL;
    config.sampleRate = 0;

    harken::EngineConfig resolved;
    ASSERT_EQ(VN_OK, harken::ResolveConfigForInitialDevice(
                         &config, 0, 44100, &resolved));
    EXPECT_EQ(44100u, resolved.mixRate.SampleRate());
    EXPECT_EQ(harken::MixRateResolution::InitialDeviceObservation,
              resolved.mixRate.Resolution());

    ASSERT_EQ(VN_OK, harken::ResolveConfigForInitialDevice(
                         &config, 0, 0, &resolved));
    EXPECT_EQ(48000u, resolved.mixRate.SampleRate());
    EXPECT_EQ(harken::MixRateResolution::InitialDeviceFallback,
              resolved.mixRate.Resolution());
}

TEST(BusDownmix, ResetAndUninitializedRefusals) {
    // With no engine initialized every downmix call is VN_NOT_INITIALIZED.
    EXPECT_EQ(VN_NOT_INITIALIZED,
              Vn_SetBusOutputLayout(kSubBus, VN_CHANNEL_LAYOUT_STEREO));
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_ResetBusDownmixMatrix(kSubBus));

    EngineSession session(MakeNullConfigLayout(1, kBlockFrames, 48000,
                                               VN_CHANNEL_LAYOUT_5_1,
                                               VN_CHANNEL_LAYOUT_5_1));
    ASSERT_EQ(VN_OK, session.InitResult());
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_ResetBusDownmixMatrix(harkentest::ObjectId(999)));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSubBus, VN_BUS_MASTER));
    EXPECT_EQ(VN_OK, Vn_ResetBusDownmixMatrix(kSubBus));
}

// --- config resolution + snapshot vintage ---

VnSnapshot CaptureFull() {
    VnSnapshot snap{};
    snap.structSize = sizeof(VnSnapshot);
    EXPECT_EQ(VN_OK, Vn_CaptureSnapshot(&snap));
    return snap;
}

TEST(BusDownmix, SnapshotReportsMixAndDeviceCounts) {
    EngineSession session(MakeNullConfigLayout(1, kBlockFrames, 48000,
                                               VN_CHANNEL_LAYOUT_5_1,
                                               VN_CHANNEL_LAYOUT_STEREO));
    ASSERT_EQ(VN_OK, session.InitResult());
    const VnSnapshot snap = CaptureFull();
    EXPECT_EQ(6u, snap.mixChannelCount);
    EXPECT_EQ(2u, snap.deviceChannelCount);
    EXPECT_EQ(1u, snap.outputDownmixActive); // device narrower than the mix
    EXPECT_EQ(0u, snap.renderWorkerCount);   // serial in this segment
}

TEST(BusDownmix, StereoDefaultReportsNoDownmix) {
    EngineSession session(MakeNullConfig(1, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    const VnSnapshot snap = CaptureFull();
    EXPECT_EQ(2u, snap.mixChannelCount);
    EXPECT_EQ(2u, snap.deviceChannelCount);
    EXPECT_EQ(0u, snap.outputDownmixActive);
}

TEST(BusDownmix, StereoDefaultFoldInTakesFrozenLaw) {
    // The stereo-default byte-identity guard at the fold-in: a spatialized mono
    // DC voice at dead center must preserve the original sample on each side.
    // This guards the mono reference independently of mode-to-mode equality.
    EngineSession session(MakeNullConfig(2, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> dc = harkentest::MakeDC(48000, kAmp);
    const VnPCMDesc desc = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    VnPlayDesc play = harkentest::MakePlayDesc3D(harkentest::ObjectId(1), 0.0f, 0.0f, -10.0f); // center
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_Update());

    CaptureMulti cap;
    harkentest::PumpAndCaptureMulti(engine, 2, cap);
    ASSERT_EQ(2u, cap.channelCount);
    const std::size_t s = kBlockFrames + kBlockFrames / 2;
    const float expected = kAmp;
    ASSERT_GT(std::fabs(cap.channels[0][s]), harkentest::kSilenceFloor)
        << "silence-guard: compared signal is silent";
    EXPECT_NEAR(expected, cap.channels[0][s], kTol);
    EXPECT_NEAR(expected, cap.channels[1][s], kTol);
}

TEST(BusDownmix, ConfigRefusesUnsupportedLayoutAndWorkerCeiling) {
    VnConfig bad = MakeNullConfig(1, kBlockFrames, 48000);
    bad.mixChannelLayout = 99;
    EXPECT_EQ(VN_UNSUPPORTED_LAYOUT, Vn_Init(&bad));

    VnConfig workers = MakeNullConfig(1, kBlockFrames, 48000);
    workers.renderWorkerCount = 65; // above the sanity ceiling of 64
    EXPECT_EQ(VN_INVALID_CONFIG, Vn_Init(&workers));

    VnConfig ok = MakeNullConfig(1, kBlockFrames, 48000);
    ok.renderWorkerCount = 64; // exactly at the ceiling
    ASSERT_EQ(VN_OK, Vn_Init(&ok));
    Vn_Term();
}

TEST(BusDownmix, OlderConfigVintageDefaultsToStereo) {
    // A caller whose struct predates the multichannel fields (structSize ends
    // at maxTriggerDepth's vintage) reads the mix layout as its stereo default.
    VnConfig legacy{};
    legacy.structSize =
        static_cast<std::uint32_t>(offsetof(VnConfig, mixChannelLayout));
    legacy.deviceMode = VN_DEVICE_NULL;
    legacy.blockFrames = kBlockFrames;
    ASSERT_EQ(VN_OK, Vn_Init(&legacy));
    const VnSnapshot snap = CaptureFull();
    EXPECT_EQ(2u, snap.mixChannelCount);
    EXPECT_EQ(2u, snap.deviceChannelCount);
    EXPECT_EQ(0u, snap.outputDownmixActive);
    Vn_Term();
}

TEST(BusDownmix, OlderSnapshotVintageOmitsNewFields) {
    EngineSession session(MakeNullConfigLayout(1, kBlockFrames, 48000,
                                               VN_CHANNEL_LAYOUT_5_1,
                                               VN_CHANNEL_LAYOUT_STEREO));
    ASSERT_EQ(VN_OK, session.InitResult());
    // A snapshot struct predating the multichannel fields does not receive
    // them: the caller's sentinel survives the capture.
    VnSnapshot snap{};
    snap.structSize =
        static_cast<std::uint32_t>(offsetof(VnSnapshot, mixChannelCount));
    snap.mixChannelCount = 0xABCDu;
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&snap));
    EXPECT_EQ(0xABCDu, snap.mixChannelCount); // untouched by the engine
}

} // namespace
