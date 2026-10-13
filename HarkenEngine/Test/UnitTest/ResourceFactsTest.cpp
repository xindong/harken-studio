// Resource-facts reader contract tests.
//
// What matters here is what the reader refuses to report. An unknown id and an
// unusable registration must both come back invalid with zeroed numbers, because
// a compiler that received a plausible-looking zero-length resource would plan
// events against material it cannot read.

#include "Common/ObjectIdTestSupport.h"
#include "harken/media/ResourceFacts.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

VnPCMDesc PlanarDesc(const float* const* planes, std::uint32_t channels,
                     std::uint32_t sampleRate, std::uint64_t frames)
{
    VnPCMDesc desc{};
    desc.structSize = sizeof(VnPCMDesc);
    desc.sampleRate = sampleRate;
    desc.channels = channels;
    desc.layout = VN_PCM_LAYOUT_PLANAR;
    desc.policy = VN_PCM_POLICY_COPY;
    desc.frames = frames;
    desc.samplesPlanar = planes;
    return desc;
}

} // namespace

TEST(ResourceFacts, ReportsRateChannelsAndLengthOfAResidentSound)
{
    harken::PCMRegistry registry;
    const std::vector<float> left(64, 0.25f);
    const std::vector<float> right(64, -0.25f);
    const float* planes[2] = {left.data(), right.data()};
    const VnPCMDesc desc = PlanarDesc(planes, 2, 44100, left.size());
    ASSERT_EQ(VN_OK, registry.RegisterPCM(harkentest::ObjectId(9), &desc));

    const harken::ResourceFacts facts = harken::ResidentPcmFacts(registry, harkentest::ObjectId(9));
    EXPECT_TRUE(facts.valid);
    EXPECT_EQ(facts.sampleRate, 44100u);
    EXPECT_EQ(facts.channelCount, 2u);
    EXPECT_EQ(facts.frameCount, 64u);
    EXPECT_EQ(facts.contentRevision,
              registry.Lookup(harkentest::ObjectId(9)).contentRevision);
    EXPECT_NE(facts.contentRevision, 0u);
}

// An id the registry does not hold is not a resource of length zero. Zeroing
// every field alongside valid == false is what stops a caller that forgets the
// flag from reading a number that means nothing.
TEST(ResourceFacts, ReportsAnUnknownResidentIdAsInvalidWithZeroedFields)
{
    harken::PCMRegistry registry;
    const harken::ResourceFacts facts = harken::ResidentPcmFacts(registry, harkentest::ObjectId(404));
    EXPECT_FALSE(facts.valid);
    EXPECT_EQ(facts.sampleRate, 0u);
    EXPECT_EQ(facts.channelCount, 0u);
    EXPECT_EQ(facts.frameCount, 0u);
    EXPECT_EQ(facts.contentRevision, 0u);
}

// Both resource classes reach a caller in one shape, so a compiler does not
// branch on where the material lives.
TEST(ResourceFacts, ReportsStreamFactsInTheSameShape)
{
    harken::StreamManager::StreamFacts stream;
    stream.sampleRate = 48000;
    stream.channelCount = 1;
    stream.frameCount = 96000;
    stream.contentRevision = 0x123456789abcdef0ull;

    const harken::ResourceFacts facts = harken::StreamResourceFacts(stream);
    EXPECT_TRUE(facts.valid);
    EXPECT_EQ(facts.sampleRate, 48000u);
    EXPECT_EQ(facts.channelCount, 1u);
    EXPECT_EQ(facts.frameCount, 96000u);
    EXPECT_EQ(facts.contentRevision, stream.contentRevision);
}

// A description with no channels or no frames describes nothing playable, and
// must not pass as a resource just because the struct was populated.
TEST(ResourceFacts, RejectsStreamFactsThatDescribeNoMaterial)
{
    harken::StreamManager::StreamFacts noFrames;
    noFrames.sampleRate = 48000;
    noFrames.channelCount = 2;
    noFrames.frameCount = 0;
    EXPECT_FALSE(harken::StreamResourceFacts(noFrames).valid);

    harken::StreamManager::StreamFacts noChannels;
    noChannels.sampleRate = 48000;
    noChannels.channelCount = 0;
    noChannels.frameCount = 1024;
    EXPECT_FALSE(harken::StreamResourceFacts(noChannels).valid);
}
