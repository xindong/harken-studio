#include "Common/VorbisFixture.h"
#include "harken/media/PackFormat.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <limits>

namespace {
using namespace harken;
void Write(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint64_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i) bytes.at(at + i) = static_cast<std::uint8_t>(value >> (8 * i));
}

TEST(VorbisMedia, FixtureSeeksAndLoopBoundariesMatchIndependentLinearDecode) {
    for (unsigned channels : {1u, 2u, 4u, 6u, 8u}) {
        SCOPED_TRACE(channels);
        const auto fixture = test::MakeVorbisFixture(channels, true, 48713, 95019);
        VorbisMedia media;
        ASSERT_TRUE(ParseVorbisTestMedia(fixture.media.data(), fixture.media.size(), media));
        EXPECT_EQ(media.frames, 144137u); EXPECT_EQ(media.channels, channels);
        EXPECT_EQ(media.sampleRate, 48000u);
        EXPECT_EQ(media.loopStart, 48713u); EXPECT_EQ(media.loopEnd, 95019u);
        for (auto frame : {0ull, 1ull, 127ull, 4095ull, 4096ull, 48713ull, 95018ull, 143001ull, 144136ull}) {
            SCOPED_TRACE(frame);
            const auto count = std::min<std::uint64_t>(2048, media.frames - frame);
            const auto actual = test::DecodeReferencePackets(media, frame, count);
            ASSERT_EQ(actual.size(), count * channels);
            for (std::size_t i = 0; i < actual.size(); ++i)
                ASSERT_FLOAT_EQ(actual[i], fixture.reference[frame * channels + i]) << i;
        }
        for (std::size_t i = 0; i < media.seek.size(); ++i) EXPECT_TRUE(media.VerifySegment(i));
        const auto stream = test::WrapVorbisStream(fixture.media);
        VorbisMedia fromStream;
        ASSERT_TRUE(ParseVorbisTestStream(stream.data(), stream.size(), fromStream));
        EXPECT_EQ(fromStream.revision, media.revision);
        EXPECT_EQ(stream.size(), fixture.media.size() + vorbis_test_wire::kStreamHeaderBytes);
        EXPECT_TRUE(std::equal(fixture.media.begin(), fixture.media.end(),
                              stream.begin() + vorbis_test_wire::kStreamHeaderBytes));
    }
}

TEST(VorbisMedia, RejectsMalformedMetadataAndCorruptionWithoutPublishing) {
    const auto fixture = test::MakeVorbisFixture(2, true, 48713, 95019);
    VorbisMedia published;
    ASSERT_TRUE(ParseVorbisTestMedia(fixture.media.data(), fixture.media.size(), published));
    const auto* originalPackets = published.packets;
    auto reject = [&](const std::vector<std::uint8_t>& bytes) {
        EXPECT_FALSE(ParseVorbisTestMedia(bytes.data(), bytes.size(), published));
        EXPECT_EQ(published.packets, originalPackets);
        EXPECT_EQ(published.frames, 144137u);
    };
    for (std::size_t size = 0; size < vorbis_test_wire::kHeaderBytes; ++size)
        EXPECT_FALSE(ParseVorbisTestMedia(fixture.media.data(), size, published));
    for (const auto offset : {0u, 4u, 6u, 14u, 16u, 24u, 28u, 32u, 36u, 40u, 108u}) {
        auto bytes = fixture.media; Write(bytes, offset, 0xffffffffu, 4); reject(bytes);
    }
    for (unsigned channels : {0u, 3u, 5u, 7u, 9u}) {
        auto bytes = fixture.media; Write(bytes, 12, channels, 2); reject(bytes);
    }
    auto bytes = fixture.media; bytes.back() ^= 1; reject(bytes);
    bytes = fixture.media; bytes.pop_back(); reject(bytes);
    bytes = fixture.media; bytes.push_back(0); reject(bytes);
    // Recompute the aggregate checksum to isolate packet framing validation.
    bytes = fixture.media;
    const auto packetOffset = published.packets - fixture.media.data();
    Write(bytes, packetOffset, 0, 2);
    Write(bytes, 100, Fnv1a64(bytes.data() + packetOffset, published.packetBytes), 8); reject(bytes);
    const auto table = vorbis_test_wire::kHeaderBytes + published.identBytes + published.setupBytes;
    bytes = fixture.media; Write(bytes, table + vorbis_test_wire::kSeekBytes + 8, 1, 8); reject(bytes);
    bytes = fixture.media; Write(bytes, table + published.seek.size() * vorbis_test_wire::kSeekBytes, 0, 8); reject(bytes);
    const auto stream = test::WrapVorbisStream(fixture.media);
    for (const auto offset : {0u, 4u, 6u, 8u, 16u}) {
        bytes = stream; Write(bytes, offset, 0xffffffffu, 4);
        EXPECT_FALSE(ParseVorbisTestStream(bytes.data(), bytes.size(), published));
        EXPECT_EQ(published.packets, originalPackets);
    }
}

TEST(VorbisMedia, StreamingMetadataDoesNotRequireOrExposePacketPayload) {
    const auto fixture = test::MakeVorbisFixture(6, true, 48713, 95019);
    const auto count = VorbisTestMetadataBytes(fixture.media.data(), fixture.media.size());
    ASSERT_GT(count, vorbis_test_wire::kHeaderBytes);
    ASSERT_LT(count, fixture.media.size());
    std::vector<std::uint8_t> prefix(fixture.media.begin(), fixture.media.begin() + count);
    VorbisMedia media;
    ASSERT_TRUE(ParseVorbisTestMediaHeaders(prefix.data(), prefix.size(), fixture.media.size(), media));
    EXPECT_EQ(count, media.packetDataOffset);
    EXPECT_EQ(nullptr, media.packets);
    EXPECT_EQ(6u, media.channels);
    EXPECT_EQ(144137u, media.frames);
    const std::uint8_t* packet = nullptr;
    std::uint16_t size = 0;
    EXPECT_FALSE(media.Packet(0, packet, size));
    EXPECT_FALSE(media.VerifySegment(0));
    EXPECT_FALSE(ParseVorbisTestMedia(prefix.data(), prefix.size(), media));
    EXPECT_FALSE(ParseVorbisTestMediaHeaders(prefix.data(), prefix.size() - 1, fixture.media.size(), media));
    EXPECT_FALSE(ParseVorbisTestMediaHeaders(prefix.data(), prefix.size(), fixture.media.size() + 1, media));
    EXPECT_EQ(count, media.packetDataOffset);
    EXPECT_EQ(144137u, media.frames);
}

TEST(VorbisMedia, ResidentAndDenseStreamIndexesUseTheSamePackets) {
    const auto resident = test::MakeVorbisFixture(1);
    const auto streamed = test::MakeVorbisFixture(1, true, 0, 0, 64);
    VorbisMedia a, b;
    ASSERT_TRUE(ParseVorbisTestMedia(resident.media.data(), resident.media.size(), a));
    ASSERT_TRUE(ParseVorbisTestMedia(streamed.media.data(), streamed.media.size(), b));
    EXPECT_TRUE(a.segmentChecksums.empty()); EXPECT_EQ(a.revision, b.revision);
    ASSERT_EQ(a.packetBytes, b.packetBytes);
    EXPECT_TRUE(std::equal(a.packets, a.packets + a.packetBytes, b.packets));
    EXPECT_EQ(a.Seek(a.frames), nullptr);
    for (auto frame : {63ull, 64ull, 65ull, 144136ull}) {
        const auto actual = test::DecodeReferencePackets(b, frame, 1);
        ASSERT_EQ(actual.size(), 1u); EXPECT_FLOAT_EQ(actual[0], streamed.reference[frame]);
    }
}
}
