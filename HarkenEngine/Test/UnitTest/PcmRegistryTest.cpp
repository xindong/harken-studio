// PCM registry behavior: descriptor validation, duplicate ids, both
// ownership policies (copy detaches from caller memory, reference reads
// it), interleaved and planar layouts, and the VN_IN_USE refusal while a
// voice plays the sound.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"
#include "harken/core/MediaContentRevision.h"
#include "harken/core/FloatMode.h"
#include "harken/core/PcmRegistration.h"
#include "harken/core/StableRowStorage.h"
#include "harken/media/PcmRegistry.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>

namespace {

using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::MonoDesc;

constexpr std::uint32_t kBlockFrames = 256;
constexpr std::uint64_t kCookieSentinel = 0xd00dfeedbadc0ffeULL;

struct PcmRegisterFaultGuard {
    explicit PcmRegisterFaultGuard(harken::PcmRegisterFault fault) {
        harken::SetPcmRegisterFaultForTests(fault);
    }
    ~PcmRegisterFaultGuard() {
        harken::SetPcmRegisterFaultForTests(harken::PcmRegisterFault::None);
    }
    PcmRegisterFaultGuard(const PcmRegisterFaultGuard&) = delete;
    PcmRegisterFaultGuard& operator=(const PcmRegisterFaultGuard&) = delete;
};

void ExpectNoRegistration(const harken::PCMRegistry& registry,
                          VnSoundId soundId) {
    EXPECT_EQ(0u, registry.Count());
    EXPECT_EQ(0u, registry.CookieOf(soundId));
    EXPECT_FALSE(registry.Lookup(soundId).Valid());
}

TEST(MediaContentRevision, PcmCanonicalBytesHaveStableGolden) {
    const float samples[2] = {0.0f, 0.5f};
    const float* channels[1] = {samples};
    std::uint64_t revision = 0;
    ASSERT_TRUE(harken::ComputeMediaPcmContentRevisionV1(
        48000, 1, 2, channels, 1, &revision));
    EXPECT_EQ(0x30be476552ab515bull, revision);
}

TEST(MediaContentRevision, PcmPairPreservesCanonicalBytesAndInvalidOutputs) {
    using Input = harken::MediaPcmContentRevisionInputV1;
    constexpr std::uint64_t firstSentinel = 0x123456789abcdef0ull;
    constexpr std::uint64_t secondSentinel = 0xfedcba9876543210ull;
    constexpr std::uint64_t monoGolden = 0x30be476552ab515bull;
    constexpr std::uint64_t stereo44100Golden = 0x188b8e3e5a2f7689ull;
    constexpr std::uint64_t stereo48000Golden = 0x8e616be8462b6a3dull;
    const float positive[2] = {0.0f, 0.5f};
    const float negativeZero[2] = {-0.0f, 0.5f};
    const float right[2] = {-0.0f, -0.25f};
    const float interleaved[4] = {0.0f, -0.0f, 0.5f, -0.25f};
    const float nan[2] = {std::numeric_limits<float>::quiet_NaN(), 0.5f};
    const float infinity[2] = {0.0f, std::numeric_limits<float>::infinity()};
    const float negativeInfinity[2] = {-std::numeric_limits<float>::infinity(), 0.5f};
    const float* monoPlanes[1] = {positive};
    const float* negativeZeroPlanes[1] = {negativeZero};
    const float* stereoPlanes[2] = {positive, right};
    const float* interleavedPlanes[2] = {interleaved, interleaved + 1};
    const float* nanPlanes[1] = {nan};
    const float* infinityPlanes[1] = {infinity};
    const float* negativeInfinityPlanes[1] = {negativeInfinity};
    const float* nullPlane[1] = {nullptr};
    const Input mono{48000, 1, 2, monoPlanes, 1};
    const Input negativeMono{48000, 1, 2, negativeZeroPlanes, 1};
    const Input stereo44100{44100, 2, 2, stereoPlanes, 1, 0, 2};
    const Input stereo48000{48000, 2, 2, stereoPlanes, 1, 1, 2};
    const Input stridedStereo{48000, 2, 2, interleavedPlanes, 2, 1, 2};
    const Input nonFinite{48000, 1, 2, nanPlanes, 1};
    const Input positiveInfinity{48000, 1, 2, infinityPlanes, 1};
    const Input negativeInfinite{48000, 1, 2, negativeInfinityPlanes, 1};
    const Input missingPlane{48000, 1, 2, nullPlane, 1};
    struct PairCase {
        const char* name;
        Input inputs[2];
        bool expectedValid[2];
        std::uint64_t expectedRevision[2];
    };
    const PairCase pairs[] = {
        {"mono signed zero", {mono, negativeMono}, {true, true},
         {monoGolden, monoGolden}},
        {"independent stereo rates and loops", {stereo44100, stereo48000},
         {true, true}, {stereo44100Golden, stereo48000Golden}},
        {"frame-major strided stereo", {stereo44100, stridedStereo},
         {true, true}, {stereo44100Golden, stereo48000Golden}},
        {"first NaN", {nonFinite, mono}, {false, true},
         {firstSentinel, monoGolden}},
        {"second infinity", {mono, positiveInfinity}, {true, false},
         {monoGolden, secondSentinel}},
        {"both non-finite", {negativeInfinite, nonFinite}, {false, false},
         {firstSentinel, secondSentinel}},
        {"first null plane", {missingPlane, mono}, {false, true},
         {firstSentinel, monoGolden}},
        {"second null plane", {mono, missingPlane}, {true, false},
         {monoGolden, secondSentinel}},
    };
    for (const auto& pair : pairs) {
        SCOPED_TRACE(pair.name);
        std::uint64_t revisions[2] = {firstSentinel, secondSentinel};
        bool valid[2] = {!pair.expectedValid[0], !pair.expectedValid[1]};
        ASSERT_TRUE(harken::TryComputeMediaPcmContentRevisionPairV1(
            pair.inputs[0], pair.inputs[1], revisions, valid));
        for (std::size_t side = 0; side < 2; ++side) {
            SCOPED_TRACE(side);
            EXPECT_EQ(pair.expectedValid[side], valid[side]);
            EXPECT_EQ(pair.expectedRevision[side], revisions[side]);
            const auto& input = pair.inputs[side];
            auto single = side == 0 ? firstSentinel : secondSentinel;
            EXPECT_EQ(pair.expectedValid[side], harken::ComputeMediaPcmContentRevisionV1(
                input.sampleRate, input.channelCount, input.frameCount,
                input.channels, input.stride, &single,
                input.loopStartFrame, input.loopEndFrame));
            EXPECT_EQ(pair.expectedRevision[side], single);
        }
    }

    struct FallbackCase {
        const char* name;
        Input input;
    };
    const FallbackCase fallbacks[] = {
        {"different frames", {48000, 1, 1, monoPlanes, 1}},
        {"different channels", {48000, 2, 2, stereoPlanes, 1}},
        {"zero rate", {0, 1, 2, monoPlanes, 1}},
        {"zero channels", {48000, 0, 2, monoPlanes, 1}},
        {"zero frames", {48000, 1, 0, monoPlanes, 1}},
        {"null channels", {48000, 1, 2, nullptr, 1}},
        {"zero stride", {48000, 1, 2, monoPlanes, 0}},
        {"missing loop end", {48000, 1, 2, monoPlanes, 1, 1, 0}},
        {"reversed loop", {48000, 1, 2, monoPlanes, 1, 2, 1}},
        {"loop outside frames", {48000, 1, 2, monoPlanes, 1, 0, 3}},
        {"index overflow", {48000, 1, std::numeric_limits<std::uint64_t>::max(),
                            monoPlanes, 2}},
    };
    for (const auto& fallback : fallbacks) {
        SCOPED_TRACE(fallback.name);
        for (std::size_t side = 0; side < 2; ++side) {
            SCOPED_TRACE(side);
            Input inputs[2] = {mono, mono};
            inputs[side] = fallback.input;
            std::uint64_t revisions[2] = {firstSentinel, secondSentinel};
            bool valid[2] = {false, true};
            EXPECT_FALSE(harken::TryComputeMediaPcmContentRevisionPairV1(
                inputs[0], inputs[1], revisions, valid));
            EXPECT_EQ(firstSentinel, revisions[0]);
            EXPECT_EQ(secondSentinel, revisions[1]);
            EXPECT_FALSE(valid[0]);
            EXPECT_TRUE(valid[1]);
        }
    }
}

TEST(MediaContentRevision, StreamCanonicalBytesHaveStableGolden) {
    const harken::MediaStreamSemanticHeaderV1 header{
        48000, 2, 1000, 0, 1, 256, 4};
    const std::uint64_t checksums[4] = {
        0x0123456789abcdefull,
        0xfedcba9876543210ull,
        1,
        2,
    };
    std::uint64_t revision = 0;
    ASSERT_TRUE(harken::ComputeMediaStreamContentRevisionV1(
        header, checksums, &revision));
    EXPECT_EQ(0x2c21bdf8be9e69bfull, revision);
}

TEST(PCMRegistry, LoopRegionParticipatesInContentRevisionAndReplacement) {
    harken::PCMRegistry registry;
    const auto samples = harkentest::MakeDC(1024, 0.25f);
    const auto desc = MonoDesc(samples, 48000, VN_PCM_POLICY_COPY);
    ASSERT_EQ(registry.RegisterPCM(harkentest::ObjectId(11), &desc, nullptr, {128, 512}), VN_OK);
    const auto original = registry.Lookup(harkentest::ObjectId(11));
    ASSERT_TRUE(original.Valid());
    EXPECT_EQ(original.loop.startFrame, 128u);
    EXPECT_EQ(original.LoopEndFrame(), 512u);
    original.useCount->fetch_add(1);
    harken::PCMRegistry::PreparedReplaceToken replacement;
    ASSERT_EQ(registry.PrepareReplacePCM(harkentest::ObjectId(11), &desc, &replacement, {256, 768}), VN_OK);
    ASSERT_EQ(registry.CommitPreparedReplacePCM(&replacement), VN_OK);
    const auto updated = registry.Lookup(harkentest::ObjectId(11));
    EXPECT_NE(updated.contentRevision, original.contentRevision);
    EXPECT_EQ(updated.loop.startFrame, 256u);
    EXPECT_EQ(updated.LoopEndFrame(), 768u);
    EXPECT_EQ(original.loop.startFrame, 128u);
    EXPECT_EQ(original.LoopEndFrame(), 512u);
    original.useCount->fetch_sub(1);
    registry.SweepRetired();
    ASSERT_EQ(registry.ReplacePCM(harkentest::ObjectId(11), &desc), VN_OK);
    EXPECT_EQ(registry.Lookup(harkentest::ObjectId(11)).loop.startFrame, 0u);
    EXPECT_EQ(registry.Lookup(harkentest::ObjectId(11)).LoopEndFrame(), samples.size());
}

TEST(PCMRegistry, InvalidLoopRegionsDoNotChangeRegisteredMedia) {
    harken::PCMRegistry registry;
    const auto samples = harkentest::MakeDC(1024, 0.25f);
    const auto desc = MonoDesc(samples, 48000, VN_PCM_POLICY_COPY);
    const harken::PcmLoopRegion invalid[] = {{128, 0}, {512, 128}, {128, 128}, {0, 1025}};
    for (const auto loop : invalid)
        EXPECT_EQ(registry.RegisterPCM(harkentest::ObjectId(11), &desc, nullptr, loop), VN_INVALID_ARG);
    EXPECT_FALSE(registry.Lookup(harkentest::ObjectId(11)).Valid());
    ASSERT_EQ(registry.RegisterPCM(harkentest::ObjectId(11), &desc), VN_OK);
    const auto revision = registry.Lookup(harkentest::ObjectId(11)).contentRevision;
    for (const auto loop : invalid) {
        harken::PCMRegistry::PreparedReplaceToken replacement;
        EXPECT_EQ(registry.PrepareReplacePCM(harkentest::ObjectId(11), &desc, &replacement, loop), VN_INVALID_ARG);
        EXPECT_FALSE(replacement.IsPrepared());
        EXPECT_EQ(registry.Lookup(harkentest::ObjectId(11)).contentRevision, revision);
    }
}

TEST(PCMRegistry, LoopRegistrationPreservesLegacyPublicRegistration) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(session.InitResult(), VN_OK);
    const auto samples = harkentest::MakeDC(1024, 0.25f);
    const auto desc = MonoDesc(samples, 48000, VN_PCM_POLICY_COPY);
    std::uint64_t revision = 0;
    ASSERT_EQ(harken::RegisterPcmWithLoopRegionV1(harkentest::ObjectId(11), desc, {128, 512}, &revision), VN_OK);
    EXPECT_NE(revision, 0u);
    const auto previous = revision;
    ASSERT_EQ(harken::RegisterPcmWithLoopRegionV1(harkentest::ObjectId(11), desc, {256, 768}, &revision), VN_OK);
    EXPECT_NE(revision, previous);
    EXPECT_EQ(Vn_RegisterPCM(harkentest::ObjectId(11), &desc), VN_IN_USE);
    EXPECT_EQ(Vn_ReplacePCM(harkentest::ObjectId(11), &desc), VN_OK);
    EXPECT_EQ(Vn_UnregisterPCM(harkentest::ObjectId(11)), VN_OK);
}

TEST(PCMRegistry, CopyInterleavedSampleBackingFailureIsAtomic) {
    harken::PCMRegistry registry;
    const std::vector<float> samples = harkentest::MakeDC(16, 0.5f);
    const VnPCMDesc desc = MonoDesc(samples, 48000, VN_PCM_POLICY_COPY);
    std::uint64_t outCookie = kCookieSentinel;

    {
        PcmRegisterFaultGuard fault(harken::PcmRegisterFault::SampleBacking);
        EXPECT_THROW(registry.RegisterPCM(harkentest::ObjectId(11), &desc, &outCookie),
                     std::bad_alloc);
    }

    ExpectNoRegistration(registry, harkentest::ObjectId(11));
    EXPECT_EQ(kCookieSentinel, outCookie);
    EXPECT_EQ(VN_OK, registry.RegisterPCM(harkentest::ObjectId(11), &desc, &outCookie));
    EXPECT_TRUE(registry.Lookup(harkentest::ObjectId(11)).Valid());
}

TEST(PCMRegistry, CopyPlanarSampleBackingFailureIsAtomic) {
    harken::PCMRegistry registry;
    const std::vector<float> left = harkentest::MakeDC(16, 0.5f);
    const std::vector<float> right = harkentest::MakeDC(16, -0.25f);
    const float* planes[2] = {left.data(), right.data()};
    VnPCMDesc desc{};
    desc.structSize = sizeof(VnPCMDesc);
    desc.sampleRate = 48000;
    desc.channels = 2;
    desc.layout = VN_PCM_LAYOUT_PLANAR;
    desc.policy = VN_PCM_POLICY_COPY;
    desc.frames = left.size();
    desc.samplesPlanar = planes;
    std::uint64_t outCookie = kCookieSentinel;

    {
        PcmRegisterFaultGuard fault(harken::PcmRegisterFault::SampleBacking);
        EXPECT_THROW(registry.RegisterPCM(harkentest::ObjectId(12), &desc, &outCookie),
                     std::bad_alloc);
    }

    ExpectNoRegistration(registry, harkentest::ObjectId(12));
    EXPECT_EQ(kCookieSentinel, outCookie);
    EXPECT_EQ(VN_OK, registry.RegisterPCM(harkentest::ObjectId(12), &desc, &outCookie));
    EXPECT_TRUE(registry.Lookup(harkentest::ObjectId(12)).Valid());
}

TEST(PCMRegistry, ReferenceMapCommitFailureIsAtomic) {
    harken::PCMRegistry registry;
    const std::vector<float> samples = harkentest::MakeDC(16, 0.5f);
    const VnPCMDesc desc =
        MonoDesc(samples, 48000, VN_PCM_POLICY_REFERENCE);
    std::uint64_t outCookie = kCookieSentinel;

    {
        PcmRegisterFaultGuard fault(harken::PcmRegisterFault::MapCommit);
        EXPECT_THROW(registry.RegisterPCM(harkentest::ObjectId(13), &desc, &outCookie),
                     std::bad_alloc);
    }

    ExpectNoRegistration(registry, harkentest::ObjectId(13));
    EXPECT_EQ(kCookieSentinel, outCookie);
    EXPECT_EQ(VN_OK, registry.RegisterPCM(harkentest::ObjectId(13), &desc, &outCookie));
    EXPECT_TRUE(registry.Lookup(harkentest::ObjectId(13)).Valid());
}

TEST(PCMRegistry, CopyMapCommitFailureDoesNotConsumeCookie) {
    const auto pageBytes = harken::detail::StablePageReservation::SystemPageBytes();
    ASSERT_NE(0u, pageBytes);
    for (const auto frames : {std::size_t{16}, 17 * pageBytes / sizeof(float) + 1}) {
        SCOPED_TRACE(frames);
        harken::PCMRegistry registry;
        const std::vector<float> samples = harkentest::MakeDC(frames, 0.5f);
        const VnPCMDesc desc = MonoDesc(samples, 48000, VN_PCM_POLICY_COPY);
        std::uint64_t firstCookie = kCookieSentinel;
        ASSERT_EQ(VN_OK, registry.RegisterPCM(harkentest::ObjectId(14), &desc, &firstCookie));
        ASSERT_EQ(VN_OK, registry.UnregisterPCM(harkentest::ObjectId(14)));

        std::uint64_t outCookie = kCookieSentinel;
        {
            PcmRegisterFaultGuard fault(harken::PcmRegisterFault::MapCommit);
            EXPECT_THROW(registry.RegisterPCM(harkentest::ObjectId(14), &desc, &outCookie),
                         std::bad_alloc);
        }

        ExpectNoRegistration(registry, harkentest::ObjectId(14));
        EXPECT_EQ(kCookieSentinel, outCookie);
        std::uint64_t secondCookie = kCookieSentinel;
        EXPECT_EQ(VN_OK, registry.RegisterPCM(harkentest::ObjectId(14), &desc, &secondCookie));
        EXPECT_EQ(firstCookie + 1, secondCookie);
        EXPECT_TRUE(registry.Lookup(harkentest::ObjectId(14)).Valid());
    }
}

TEST(PCMRegistry, CopyPageReservationFailureLeavesRegistrationAndCookieIntact) {
    const auto pageBytes = harken::detail::StablePageReservation::SystemPageBytes();
    ASSERT_NE(0u, pageBytes);
    harken::PCMRegistry registry;
    const auto samples = harkentest::MakeDC(17 * pageBytes / sizeof(float) + 1, 0.25f);
    const auto desc = MonoDesc(samples, 48000, VN_PCM_POLICY_COPY);
    std::uint64_t cookie = kCookieSentinel;
    harken::detail::StablePageReservation::FailNextReservationForTests();
    EXPECT_THROW(registry.RegisterPCM(harkentest::ObjectId(14), &desc, &cookie),
                 std::bad_alloc);
    ExpectNoRegistration(registry, harkentest::ObjectId(14));
    EXPECT_EQ(kCookieSentinel, cookie);
    ASSERT_EQ(VN_OK, registry.RegisterPCM(harkentest::ObjectId(14), &desc, &cookie));
    const auto view = registry.Lookup(harkentest::ObjectId(14));
    ASSERT_TRUE(view.Valid());
    for (std::size_t i = 0; i < samples.size(); ++i)
        EXPECT_EQ(samples[i], view.ReadSample(0, i));
    EXPECT_EQ(VN_OK, registry.UnregisterPCM(harkentest::ObjectId(14)));
}

TEST(PCMRegistry, PublicRegisterAllocationFailureStaysInsideCAbi) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> samples = harkentest::MakeDC(16, 0.5f);
    const VnPCMDesc desc = MonoDesc(samples, 48000, VN_PCM_POLICY_COPY);
    {
        PcmRegisterFaultGuard fault(harken::PcmRegisterFault::SampleBacking);
        EXPECT_EQ(VN_DEVICE_ERROR, Vn_RegisterPCM(harkentest::ObjectId(15), &desc));
    }

    EXPECT_FALSE(session.Engine()->registry.Lookup(harkentest::ObjectId(15)).Valid());
    EXPECT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(15), &desc));
    EXPECT_EQ(VN_OK, Vn_UnregisterPCM(harkentest::ObjectId(15)));
}

TEST(PCMRegistry, RejectsInvalidDescriptors) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> samples = harkentest::MakeDC(16, 0.5f);

    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterPCM(harkentest::ObjectId(1), nullptr));

    VnPCMDesc badSize = MonoDesc(samples, 48000);
    badSize.structSize = 0;
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterPCM(harkentest::ObjectId(1), &badSize));

    VnPCMDesc badRate = MonoDesc(samples, 48000);
    badRate.sampleRate = 0;
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterPCM(harkentest::ObjectId(1), &badRate));

    VnPCMDesc badChannels = MonoDesc(samples, 48000);
    badChannels.channels = 0;
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterPCM(harkentest::ObjectId(1), &badChannels));
    badChannels.channels = 9; // beyond the compile-time channel capacity
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterPCM(harkentest::ObjectId(1), &badChannels));

    VnPCMDesc badFrames = MonoDesc(samples, 48000);
    badFrames.frames = 0;
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterPCM(harkentest::ObjectId(1), &badFrames));

    // A frame count whose float count or byte count cannot fit size_t must
    // be refused before any allocation: a wrapped size would register a
    // view larger than its backing.
    VnPCMDesc overflowFrames = MonoDesc(samples, 48000);
    overflowFrames.frames = UINT64_MAX;
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterPCM(harkentest::ObjectId(1), &overflowFrames));
    overflowFrames.frames = (UINT64_MAX / sizeof(float)) + 1;
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterPCM(harkentest::ObjectId(1), &overflowFrames));

    VnPCMDesc badLayout = MonoDesc(samples, 48000);
    badLayout.layout = 7;
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterPCM(harkentest::ObjectId(1), &badLayout));

    VnPCMDesc badPolicy = MonoDesc(samples, 48000);
    badPolicy.policy = 7;
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterPCM(harkentest::ObjectId(1), &badPolicy));

    VnPCMDesc nullData = MonoDesc(samples, 48000);
    nullData.samplesInterleaved = nullptr;
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterPCM(harkentest::ObjectId(1), &nullData));

    VnPCMDesc nullPlane = MonoDesc(samples, 48000);
    nullPlane.layout = VN_PCM_LAYOUT_PLANAR;
    nullPlane.samplesPlanar = nullptr;
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterPCM(harkentest::ObjectId(1), &nullPlane));
}

TEST(PCMRegistry, NonFiniteSamplesRefuseBeforeAllocationAndMutation) {
    harken::PCMRegistry registry;
    std::vector<float> samples = {
        0.25f, std::numeric_limits<float>::quiet_NaN(), -0.25f};
    VnPCMDesc desc = MonoDesc(samples, 48000, VN_PCM_POLICY_COPY);
    std::uint64_t outCookie = kCookieSentinel;

    {
        PcmRegisterFaultGuard fault(harken::PcmRegisterFault::SampleBacking);
        EXPECT_EQ(VN_INVALID_ARG,
                  registry.RegisterPCM(harkentest::ObjectId(16), &desc, &outCookie));
    }
    ExpectNoRegistration(registry, harkentest::ObjectId(16));
    EXPECT_EQ(kCookieSentinel, outCookie);

    const std::vector<float> finite = {0.25f, 0.5f, -0.25f};
    const std::vector<float> infinite = {
        0.25f, std::numeric_limits<float>::infinity(), -0.25f};
    const float* planes[2] = {finite.data(), infinite.data()};
    desc.structSize = sizeof(VnPCMDesc);
    desc.sampleRate = 48000;
    desc.channels = 2;
    desc.layout = VN_PCM_LAYOUT_PLANAR;
    desc.policy = VN_PCM_POLICY_REFERENCE;
    desc.frames = finite.size();
    desc.samplesInterleaved = nullptr;
    desc.samplesPlanar = planes;
    EXPECT_EQ(VN_INVALID_ARG, registry.RegisterPCM(harkentest::ObjectId(17), &desc, &outCookie));
    ExpectNoRegistration(registry, harkentest::ObjectId(17));
    EXPECT_EQ(kCookieSentinel, outCookie);
}

TEST(PCMRegistry, PackMutationPreservesFailureOrderAndCookieFloor) {
    using Registration = harken::PCMRegistry::PackReferenceRegistration;
    using Fault = harken::PcmRegisterFault;
    enum class Position { Current, Following, AfterDuplicate };
    enum class Invalid { Sample, Loop, Plane };
    struct FailureCase {
        const char* name;
        Position position;
        Invalid invalid;
    };
    const FailureCase cases[] = {
        {"following NaN", Position::Following, Invalid::Sample},
        {"following invalid loop", Position::Following, Invalid::Loop},
        {"following null plane", Position::Following, Invalid::Plane},
        {"current NaN", Position::Current, Invalid::Sample},
        {"current invalid loop", Position::Current, Invalid::Loop},
        {"current null plane", Position::Current, Invalid::Plane},
        {"duplicate before NaN", Position::AfterDuplicate, Invalid::Sample},
        {"duplicate before invalid loop", Position::AfterDuplicate, Invalid::Loop},
        {"duplicate before null plane", Position::AfterDuplicate, Invalid::Plane},
    };
    const float samples[2] = {0.0f, 0.5f};
    const float invalidSamples[2] = {0.0f, std::numeric_limits<float>::quiet_NaN()};
    const float* planes[1] = {samples};
    const float* invalidPlanes[1] = {invalidSamples};
    const float* nullPlane[1] = {nullptr};
    VnPCMDesc desc{};
    desc.structSize = sizeof(VnPCMDesc);
    desc.sampleRate = 48000;
    desc.channels = 1;
    desc.layout = VN_PCM_LAYOUT_PLANAR;
    desc.policy = VN_PCM_POLICY_COPY;
    desc.frames = 2;
    desc.samplesPlanar = planes;
    const auto oldId = harkentest::ObjectId(100);
    const auto firstId = harkentest::ObjectId(101);
    const auto secondId = harkentest::ObjectId(102);
    const auto fourthId = harkentest::ObjectId(104);

    for (const auto& failure : cases) {
        SCOPED_TRACE(failure.name);
        for (const auto fault : {Fault::None, Fault::MapCommit, Fault::SampleBacking}) {
            if (failure.position == Position::AfterDuplicate && fault != Fault::None)
                continue;
            SCOPED_TRACE(static_cast<std::uint32_t>(fault));
            harken::PCMRegistry registry;
            std::uint64_t oldCookie = 0;
            ASSERT_EQ(VN_OK, registry.RegisterPCM(oldId, &desc, &oldCookie));
            const auto oldView = registry.Lookup(oldId);
            const auto oldOwner = registry.ReferenceOf(oldId, oldCookie);
            ASSERT_TRUE(oldView.Valid());
            ASSERT_TRUE(oldOwner.Valid());

            Registration registrations[4] = {
                {firstId, desc, {}}, {secondId, desc, {}},
                {firstId, desc, {}}, {fourthId, desc, {}},
            };
            const std::size_t count =
                failure.position == Position::AfterDuplicate ? 4 : 2;
            const std::size_t invalidIndex = failure.position == Position::Current ? 0 :
                (failure.position == Position::Following ? 1 : 3);
            auto& invalid = registrations[invalidIndex];
            if (failure.invalid == Invalid::Sample)
                invalid.desc.samplesPlanar = invalidPlanes;
            else if (failure.invalid == Invalid::Loop)
                invalid.loop = {2, 1};
            else
                invalid.desc.samplesPlanar = nullPlane;

            harken::PCMRegistry::PreparedPackMutation prepared;
            {
                PcmRegisterFaultGuard inject(fault);
                if (failure.position == Position::Following && fault != Fault::None) {
                    EXPECT_THROW(registry.PreparePackMutation(
                        registrations, count, 0, &prepared), std::bad_alloc);
                } else {
                    const auto expected = failure.position == Position::AfterDuplicate
                        ? VN_IN_USE : VN_INVALID_ARG;
                    EXPECT_EQ(expected, registry.PreparePackMutation(
                        registrations, count, 0, &prepared));
                }
            }

            EXPECT_FALSE(prepared.IsPrepared());
            EXPECT_EQ(1u, registry.Count());
            EXPECT_EQ(0u, registry.RetiredCount());
            for (const auto id : {firstId, secondId, fourthId}) {
                EXPECT_EQ(0u, prepared.CookieOf(id));
                EXPECT_FALSE(prepared.ReferenceOf(id).Valid());
                EXPECT_EQ(0u, registry.CookieOf(id));
                EXPECT_FALSE(registry.Lookup(id).Valid());
            }
            const auto after = registry.Lookup(oldId);
            ASSERT_TRUE(after.Valid());
            EXPECT_EQ(oldCookie, registry.CookieOf(oldId));
            EXPECT_EQ(oldView.contentRevision, after.contentRevision);
            EXPECT_EQ(oldView.channels[0], after.channels[0]);
            EXPECT_EQ(oldView.useCount, after.useCount);
            EXPECT_EQ(oldCookie, oldOwner.Cookie());
            EXPECT_EQ(oldView.channels[0], oldOwner.View().channels[0]);
            EXPECT_FLOAT_EQ(samples[0], after.ReadSample(0, 0));
            EXPECT_FLOAT_EQ(samples[1], after.ReadSample(0, 1));

            const Registration retry[2] = {{firstId, desc, {}}, {secondId, desc, {}}};
            ASSERT_EQ(VN_OK, registry.PreparePackMutation(retry, 2, 0, &prepared));
            ASSERT_TRUE(prepared.IsPrepared());
            EXPECT_EQ(oldCookie + 1, prepared.CookieOf(firstId));
            EXPECT_EQ(oldCookie + 2, prepared.CookieOf(secondId));
            EXPECT_EQ(1u, registry.Count());
            registry.CommitPreparedPackMutation(&prepared);
            EXPECT_FALSE(prepared.IsPrepared());
            EXPECT_EQ(3u, registry.Count());
            EXPECT_EQ(oldCookie + 1, registry.CookieOf(firstId));
            EXPECT_EQ(oldCookie + 2, registry.CookieOf(secondId));
            EXPECT_EQ(0x30be476552ab515bull, registry.Lookup(firstId).contentRevision);
            EXPECT_EQ(0x30be476552ab515bull, registry.Lookup(secondId).contentRevision);
        }
    }
}

TEST(PCMRegistry, ContentRevisionUsesLogicalSamplesNotLayoutOrPolicy) {
    harken::PCMRegistry registry;
    const std::vector<float> left = {0.0f, 0.25f, 0.75f};
    const std::vector<float> right = {-0.0f, -0.5f, 1.0f};
    std::vector<float> interleaved;
    for (std::size_t frame = 0; frame < left.size(); ++frame) {
        interleaved.push_back(left[frame]);
        interleaved.push_back(right[frame]);
    }
    const float* planes[2] = {left.data(), right.data()};

    VnPCMDesc interleavedCopy{};
    interleavedCopy.structSize = sizeof(VnPCMDesc);
    interleavedCopy.sampleRate = 44100;
    interleavedCopy.channels = 2;
    interleavedCopy.layout = VN_PCM_LAYOUT_INTERLEAVED;
    interleavedCopy.policy = VN_PCM_POLICY_COPY;
    interleavedCopy.frames = left.size();
    interleavedCopy.samplesInterleaved = interleaved.data();

    VnPCMDesc planarReference = interleavedCopy;
    planarReference.layout = VN_PCM_LAYOUT_PLANAR;
    planarReference.policy = VN_PCM_POLICY_REFERENCE;
    planarReference.samplesInterleaved = nullptr;
    planarReference.samplesPlanar = planes;

    std::uint64_t cookie = 0;
    ASSERT_EQ(VN_OK, registry.RegisterPCM(harkentest::ObjectId(21), &interleavedCopy, &cookie));
    ASSERT_EQ(VN_OK, registry.RegisterPCM(harkentest::ObjectId(22), &planarReference));
    const std::uint64_t expected = registry.Lookup(harkentest::ObjectId(21)).contentRevision;
    ASSERT_NE(0u, expected);
    EXPECT_EQ(expected, registry.Lookup(harkentest::ObjectId(22)).contentRevision);
    EXPECT_NE(cookie, expected);

    VnPCMDesc interleavedReference = interleavedCopy;
    interleavedReference.policy = VN_PCM_POLICY_REFERENCE;
    ASSERT_EQ(VN_OK, registry.RegisterPCM(harkentest::ObjectId(25), &interleavedReference));
    EXPECT_EQ(expected, registry.Lookup(harkentest::ObjectId(25)).contentRevision);

    VnPCMDesc planarCopy = planarReference;
    planarCopy.policy = VN_PCM_POLICY_COPY;
    ASSERT_EQ(VN_OK, registry.RegisterPCM(harkentest::ObjectId(26), &planarCopy));
    EXPECT_EQ(expected, registry.Lookup(harkentest::ObjectId(26)).contentRevision);

    std::vector<float> positiveZeroRight = right;
    positiveZeroRight[0] = 0.0f;
    const float* positiveZeroPlanes[2] = {left.data(),
                                          positiveZeroRight.data()};
    VnPCMDesc positiveZero = planarReference;
    positiveZero.samplesPlanar = positiveZeroPlanes;
    ASSERT_EQ(VN_OK, registry.RegisterPCM(harkentest::ObjectId(23), &positiveZero));
    EXPECT_EQ(expected, registry.Lookup(harkentest::ObjectId(23)).contentRevision);

    std::vector<float> changedLeft = left;
    changedLeft[1] = 0.251f;
    const float* changedPlanes[2] = {changedLeft.data(), right.data()};
    VnPCMDesc changed = planarReference;
    changed.samplesPlanar = changedPlanes;
    ASSERT_EQ(VN_OK, registry.RegisterPCM(harkentest::ObjectId(24), &changed));
    EXPECT_NE(expected, registry.Lookup(harkentest::ObjectId(24)).contentRevision);
}

TEST(PCMRegistry, ReplaceRejectsNonFiniteCandidateBeforeAllocationOrSwap) {
    harken::PCMRegistry registry;
    const std::vector<float> original = {0.25f, 0.5f, 0.75f};
    const VnPCMDesc originalDesc =
        MonoDesc(original, 48000, VN_PCM_POLICY_COPY);
    std::uint64_t originalCookie = 0;
    ASSERT_EQ(VN_OK,
              registry.RegisterPCM(harkentest::ObjectId(25), &originalDesc, &originalCookie));
    const harken::PCMView originalView = registry.Lookup(harkentest::ObjectId(25));

    const std::vector<float> invalid = {
        0.25f, std::numeric_limits<float>::infinity(), 0.75f};
    const VnPCMDesc invalidDesc =
        MonoDesc(invalid, 48000, VN_PCM_POLICY_COPY);
    {
        PcmRegisterFaultGuard fault(harken::PcmRegisterFault::SampleBacking);
        EXPECT_EQ(VN_INVALID_ARG, registry.ReplacePCM(harkentest::ObjectId(25), &invalidDesc));
    }

    const harken::PCMView after = registry.Lookup(harkentest::ObjectId(25));
    EXPECT_EQ(originalCookie, registry.CookieOf(harkentest::ObjectId(25)));
    EXPECT_EQ(originalView.contentRevision, after.contentRevision);
    EXPECT_EQ(originalView.channels[0], after.channels[0]);
    EXPECT_EQ(0u, registry.RetiredCount());
}

TEST(PCMRegistry, DuplicateIdRefusedAndUnknownUnregister) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> samples = harkentest::MakeDC(16, 0.5f);
    const VnPCMDesc desc = MonoDesc(samples, 48000);

    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    EXPECT_EQ(VN_IN_USE, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    EXPECT_EQ(VN_UNKNOWN_ID, Vn_UnregisterPCM(harkentest::ObjectId(99)));
    EXPECT_EQ(VN_OK, Vn_UnregisterPCM(harkentest::ObjectId(1)));
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_UnregisterPCM(harkentest::ObjectId(1)));
}

TEST(PCMRegistry, CopyPolicySurvivesCallerBufferMutation) {
    EngineSession session(MakeNullConfig(0, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());

    const auto pageBytes = harken::detail::StablePageReservation::SystemPageBytes();
    ASSERT_NE(0u, pageBytes);
    const auto largeFrames = ((17 * pageBytes / sizeof(float) + kBlockFrames - 1) /
                              kBlockFrames) * kBlockFrames;
    for (const auto frames : {std::size_t{kBlockFrames}, largeFrames}) {
        SCOPED_TRACE(frames);
        std::vector<float> samples =
            harkentest::MakeSine(frames, 1000.0, 48000.0, 0.8f);
        HARKENTEST_ASSERT_NON_SILENT(samples);
        const std::vector<float> original = samples;

        const VnPCMDesc desc = MonoDesc(samples, 48000, VN_PCM_POLICY_COPY);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

        // Trash the caller buffer after registration; the copy must be immune.
        std::fill(samples.begin(), samples.end(), -1.0f);

        const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
        VnVoiceHandle handle = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

        Capture out;
        harkentest::PumpAndCapture(session.Engine(), static_cast<std::uint32_t>(frames / kBlockFrames), out);
        HARKENTEST_ASSERT_NON_SILENT(out.left);
        for (std::size_t i = 0; i < frames; ++i) {
            ASSERT_EQ(original[i], out.left[i])
                << "left sample " << i;
            ASSERT_EQ(original[i], out.right[i])
                << "right sample " << i;
        }
        ASSERT_EQ(VN_OK, Vn_Update());
        ASSERT_EQ(VN_OK, Vn_UnregisterPCM(harkentest::ObjectId(1)));
    }
}

TEST(PCMRegistry, CopyPolicyPlanarStereoSurvivesCallerBufferMutation) {
    EngineSession session(MakeNullConfig(0, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());

    const auto pageBytes = harken::detail::StablePageReservation::SystemPageBytes();
    ASSERT_NE(0u, pageBytes);
    const auto largeFrames = ((17 * pageBytes / sizeof(float) + kBlockFrames - 1) /
                              kBlockFrames) * kBlockFrames;
    for (const auto frames : {std::size_t{kBlockFrames}, largeFrames}) {
        SCOPED_TRACE(frames);
        std::vector<float> left = harkentest::MakeDC(frames, 0.5f);
        std::vector<float> right = harkentest::MakeDC(frames, -0.25f);
        HARKENTEST_ASSERT_NON_SILENT(left);
        const float* planes[2] = {left.data(), right.data()};

        VnPCMDesc desc{};
        desc.structSize = sizeof(VnPCMDesc);
        desc.sampleRate = 48000;
        desc.channels = 2;
        desc.layout = VN_PCM_LAYOUT_PLANAR;
        desc.policy = VN_PCM_POLICY_COPY;
        desc.frames = frames;
        desc.samplesPlanar = planes;
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

        // Trash both caller planes after registration; the copy must be immune.
        std::fill(left.begin(), left.end(), -1.0f);
        std::fill(right.begin(), right.end(), -1.0f);

        const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
        VnVoiceHandle handle = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

        Capture out;
        harkentest::PumpAndCapture(session.Engine(), static_cast<std::uint32_t>(frames / kBlockFrames), out);
        HARKENTEST_ASSERT_NON_SILENT(out.left);
        for (std::size_t i = 0; i < frames; ++i) {
            ASSERT_EQ(0.5f, out.left[i]) << "left sample " << i;
            ASSERT_EQ(-0.25f, out.right[i]) << "right sample " << i;
        }
        ASSERT_EQ(VN_OK, Vn_Update());
        ASSERT_EQ(VN_OK, Vn_UnregisterPCM(harkentest::ObjectId(1)));
    }
}

TEST(PCMRegistry, ReferencePolicyPlanarStereoKeepsChannelsSeparate) {
    EngineSession session(MakeNullConfig(0, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> left = harkentest::MakeDC(kBlockFrames, 0.5f);
    const std::vector<float> right = harkentest::MakeDC(kBlockFrames, -0.25f);
    HARKENTEST_ASSERT_NON_SILENT(left);
    const float* planes[2] = {left.data(), right.data()};

    VnPCMDesc desc{};
    desc.structSize = sizeof(VnPCMDesc);
    desc.sampleRate = 48000;
    desc.channels = 2;
    desc.layout = VN_PCM_LAYOUT_PLANAR;
    desc.policy = VN_PCM_POLICY_REFERENCE;
    desc.frames = kBlockFrames;
    desc.samplesPlanar = planes;
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    Capture out;
    harkentest::PumpAndCapture(session.Engine(), 1, out);
    HARKENTEST_ASSERT_NON_SILENT(out.left);
    for (std::size_t i = 0; i < kBlockFrames; ++i) {
        ASSERT_EQ(0.5f, out.left[i]) << "left sample " << i;
        ASSERT_EQ(-0.25f, out.right[i]) << "right sample " << i;
    }
}

TEST(PCMRegistry, InterleavedStereoStridePathReadsBothChannels) {
    EngineSession session(MakeNullConfig(0, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());

    // Interleaved L R pairs with distinct constants per channel.
    std::vector<float> interleaved(kBlockFrames * 2);
    for (std::size_t i = 0; i < kBlockFrames; ++i) {
        interleaved[i * 2 + 0] = 0.6f;
        interleaved[i * 2 + 1] = -0.3f;
    }
    HARKENTEST_ASSERT_NON_SILENT(interleaved);

    VnPCMDesc desc{};
    desc.structSize = sizeof(VnPCMDesc);
    desc.sampleRate = 48000;
    desc.channels = 2;
    desc.layout = VN_PCM_LAYOUT_INTERLEAVED;
    desc.policy = VN_PCM_POLICY_COPY;
    desc.frames = kBlockFrames;
    desc.samplesInterleaved = interleaved.data();
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    // Copy policy detaches from the caller buffer here too: trash it after
    // registration, completing the layout x policy deep-copy matrix.
    std::fill(interleaved.begin(), interleaved.end(), -1.0f);

    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    Capture out;
    harkentest::PumpAndCapture(session.Engine(), 1, out);
    HARKENTEST_ASSERT_NON_SILENT(out.left);
    for (std::size_t i = 0; i < kBlockFrames; ++i) {
        ASSERT_EQ(0.6f, out.left[i]) << "left sample " << i;
        ASSERT_EQ(-0.3f, out.right[i]) << "right sample " << i;
    }
}

TEST(PCMRegistry, ExactResidentCopyRoundTripsEveryInt16ValueAndLayout) {
    const auto pageBytes = harken::detail::StablePageReservation::SystemPageBytes();
    const auto frames = std::max(std::size_t{65536}, 16 * pageBytes / sizeof(std::int16_t));
    for (const auto channels : {1u, 2u, harken::kMaxChannels}) {
        for (const bool planar : {false, true}) {
            SCOPED_TRACE(::testing::Message() << channels << " planar=" << planar);
            harken::PCMRegistry registry;
            std::vector<float> input(frames * channels);
            for (std::size_t i = 0; i < input.size(); ++i)
                input[i] = static_cast<float>(static_cast<int>(i % 65536) - 32768) * 0x1p-15f;
            const float* planes[harken::kMaxChannels]{};
            for (std::uint32_t ch = 0; ch < channels; ++ch)
                planes[ch] = input.data() + ch * frames;
            VnPCMDesc desc{};
            desc.structSize = sizeof(desc);
            desc.channels = channels;
            desc.sampleRate = 48000;
            desc.frames = frames;
            desc.layout = planar ? VN_PCM_LAYOUT_PLANAR : VN_PCM_LAYOUT_INTERLEAVED;
            desc.policy = VN_PCM_POLICY_COPY;
            desc.samplesPlanar = planes;
            desc.samplesInterleaved = input.data();
            const auto sound = harkentest::ObjectId(1);
            ASSERT_EQ(VN_OK, registry.RegisterPCM(sound, &desc));
            const auto copy = registry.Lookup(sound);
            ASSERT_EQ(harken::PcmSampleFormat::ExactInt16, copy.sampleFormat);
            ASSERT_TRUE(registry.ReferenceOf(sound, registry.CookieOf(sound)).OwnsSamples());
            desc.policy = VN_PCM_POLICY_REFERENCE;
            ASSERT_EQ(VN_OK, registry.RegisterPCM(harkentest::ObjectId(2), &desc));
            const auto reference = registry.Lookup(harkentest::ObjectId(2));
            EXPECT_EQ(harken::PcmSampleFormat::Float32, reference.sampleFormat);
            EXPECT_EQ(reference.contentRevision, copy.contentRevision);
            for (std::uint32_t ch = 0; ch < channels; ++ch) {
                for (std::size_t i = 0; i < frames; ++i) {
                    const float expected = reference.ReadSample(ch, i);
                    const float actual = copy.ReadSample(ch, i);
                    ASSERT_EQ(0, std::memcmp(&expected, &actual, sizeof(float)))
                        << "channel " << ch << " sample " << i;
                }
            }
            const auto expected = copy.ReadSample(0, 0);
            input[0] = 0.1f;
            EXPECT_EQ(expected, copy.ReadSample(0, 0));
            EXPECT_EQ(0.1f, reference.ReadSample(0, 0));
        }
    }
}

TEST(PCMRegistry, ResidentCopyKeepsNonReversibleFloatBits) {
    const auto pageBytes = harken::detail::StablePageReservation::SystemPageBytes();
    std::vector<float> input(16 * pageBytes / sizeof(std::int16_t) + 1, 0.5f);
    const auto check = [&]() {
        for (const float value : {-0.0f, 1.0f, -1.25f, 0.1f, 0x1.fffffep-1f,
                                  std::numeric_limits<float>::max(),
                                  -std::numeric_limits<float>::max(),
                                  std::numeric_limits<float>::denorm_min(),
                                  -std::numeric_limits<float>::denorm_min()}) {
          for (const auto offset : {std::size_t{0}, std::size_t{1}, std::size_t{2},
                                    std::size_t{3}, input.size() - 1}) {
            harken::PCMRegistry registry;
            input[offset] = value;
            const auto desc = MonoDesc(input, 48000, VN_PCM_POLICY_COPY);
            ASSERT_EQ(VN_OK, registry.RegisterPCM(harkentest::ObjectId(1), &desc));
            const auto view = registry.Lookup(harkentest::ObjectId(1));
            EXPECT_EQ(harken::PcmSampleFormat::Float32, view.sampleFormat);
            const float actual = view.ReadSample(0, offset);
            EXPECT_EQ(0, std::memcmp(&value, &actual, sizeof(float)));
            input[offset] = 0.5f;
          }
        }
    };
    check();
    const harken::floatmode::ScopedFlushDenormals flushDenormals;
    check();
}

TEST(PCMRegistry, ResidentFormatReplacementRetainsReadersAndRollsBackFailures) {
    const auto pageBytes = harken::detail::StablePageReservation::SystemPageBytes();
    const auto frames = 16 * pageBytes / sizeof(std::int16_t) + 1;
    for (const bool compactFirst : {false, true}) {
        harken::PCMRegistry registry;
        const auto sound = harkentest::ObjectId(1);
        const auto initial = harkentest::MakeDC(frames, compactFirst ? 0.5f : 0.1f);
        const auto next = harkentest::MakeDC(frames, compactFirst ? 0.1f : 0.5f);
        const auto initialDesc = MonoDesc(initial, 48000, VN_PCM_POLICY_COPY);
        const auto nextDesc = MonoDesc(next, 48000, VN_PCM_POLICY_COPY);
        ASSERT_EQ(VN_OK, registry.RegisterPCM(sound, &initialDesc));
        const auto before = registry.Lookup(sound);
        const auto cookie = registry.CookieOf(sound);
        before.useCount->fetch_add(1, std::memory_order_acq_rel);
        for (const auto fault : {harken::PcmRegisterFault::SampleBacking,
                                 harken::PcmRegisterFault::MapCommit}) {
            PcmRegisterFaultGuard guard(fault);
            EXPECT_THROW(registry.ReplacePCM(sound, &nextDesc), std::bad_alloc);
            EXPECT_EQ(cookie, registry.CookieOf(sound));
            EXPECT_EQ(before.useCount, registry.Lookup(sound).useCount);
            EXPECT_EQ(before.channels[0], registry.Lookup(sound).channels[0]);
            EXPECT_EQ(0u, registry.RetiredCount());
        }
        harken::detail::StablePageReservation::FailNextReservationForTests();
        EXPECT_THROW(registry.ReplacePCM(sound, &nextDesc), std::bad_alloc);
        EXPECT_EQ(cookie, registry.CookieOf(sound));
        EXPECT_EQ(0u, registry.RetiredCount());
        ASSERT_EQ(VN_OK, registry.ReplacePCM(sound, &nextDesc));
        const auto after = registry.Lookup(sound);
        EXPECT_EQ(cookie + 1, registry.CookieOf(sound));
        EXPECT_NE(before.sampleFormat, after.sampleFormat);
        EXPECT_EQ(initial.back(), before.ReadSample(0, frames - 1));
        EXPECT_EQ(next.back(), after.ReadSample(0, frames - 1));
        EXPECT_EQ(1u, registry.RetiredCount());
        registry.SweepRetired();
        EXPECT_EQ(1u, registry.RetiredCount());
        before.useCount->fetch_sub(1, std::memory_order_acq_rel);
        registry.SweepRetired();
        EXPECT_EQ(0u, registry.RetiredCount());
        EXPECT_EQ(VN_OK, registry.UnregisterPCM(sound));
    }
}

TEST(PCMRegistry, UnregisterRefusedWhileVoicePlays) {
    EngineSession session(MakeNullConfig(0, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());

    // Two blocks of material so the voice is still live after one pump.
    const std::vector<float> sine =
        harkentest::MakeSine(kBlockFrames * 2, 1000.0, 48000.0, 0.8f);
    HARKENTEST_ASSERT_NON_SILENT(sine);
    const VnPCMDesc desc = MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    session.Engine()->nullBackend.Pump(1);
    EXPECT_EQ(VN_IN_USE, Vn_UnregisterPCM(harkentest::ObjectId(1)));

    // Let the voice finish; the refusal lifts.
    session.Engine()->nullBackend.Pump(2);
    EXPECT_EQ(nullptr, session.Engine()->voices.Resolve(harkentest::RenderHandle(
        session.Engine(), handle)));
    EXPECT_EQ(VN_OK, Vn_UnregisterPCM(harkentest::ObjectId(1)));
}

} // namespace
