// PCM registry behavior: descriptor validation, duplicate ids, both
// ownership policies (copy detaches from caller memory, reference reads
// it), interleaved and planar layouts, and the VN_IN_USE refusal while a
// voice plays the sound.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"
#include "harken/core/MediaContentRevision.h"
#include "harken/core/PcmRegistration.h"
#include "harken/media/PcmRegistry.h"

#include <algorithm>
#include <cstdint>
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
    harken::PCMRegistry registry;
    const std::vector<float> samples = harkentest::MakeDC(16, 0.5f);
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

    std::vector<float> samples =
        harkentest::MakeSine(kBlockFrames, 1000.0, 48000.0, 0.8f);
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
    harkentest::PumpAndCapture(session.Engine(), 1, out);
    HARKENTEST_ASSERT_NON_SILENT(out.left);
    for (std::size_t i = 0; i < kBlockFrames; ++i) {
        ASSERT_EQ(original[i], out.left[i])
            << "left sample " << i;
        ASSERT_EQ(original[i], out.right[i])
            << "right sample " << i;
    }
}

TEST(PCMRegistry, CopyPolicyPlanarStereoSurvivesCallerBufferMutation) {
    EngineSession session(MakeNullConfig(0, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());

    std::vector<float> left = harkentest::MakeDC(kBlockFrames, 0.5f);
    std::vector<float> right = harkentest::MakeDC(kBlockFrames, -0.25f);
    HARKENTEST_ASSERT_NON_SILENT(left);
    const float* planes[2] = {left.data(), right.data()};

    VnPCMDesc desc{};
    desc.structSize = sizeof(VnPCMDesc);
    desc.sampleRate = 48000;
    desc.channels = 2;
    desc.layout = VN_PCM_LAYOUT_PLANAR;
    desc.policy = VN_PCM_POLICY_COPY;
    desc.frames = kBlockFrames;
    desc.samplesPlanar = planes;
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    // Trash both caller planes after registration; the copy must be immune.
    std::fill(left.begin(), left.end(), -1.0f);
    std::fill(right.begin(), right.end(), -1.0f);

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
