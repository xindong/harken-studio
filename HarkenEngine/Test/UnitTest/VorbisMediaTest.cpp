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
}

TEST(VorbisMedia, ResidentPacketLengthsAndSeekOffsetsAreByteExact) {
    auto fixture = harken::test::MakeVorbisFixture(1, false, 6503, 11997, 4096, true, 173);
    VorbisMedia m;
    ASSERT_TRUE(ParseResidentVorbis(fixture.media.data(), fixture.media.size(), m));
    EXPECT_THROW(ParseResidentVorbis(fixture.media.data(), fixture.media.size(), m,
        m.seek.size() * sizeof(VorbisSeekPoint) - 1), std::bad_alloc);
    const auto index = sizeof(HkPackVorbisHeader) + m.identBytes + m.setupBytes;
    auto broken = fixture.media;
    ++broken[index + offsetof(HkPackVorbisSeek, packetOffset)];
    EXPECT_FALSE(ParseResidentVorbis(broken.data(), broken.size(), m));
    for (const auto length : {65535u, 65536u}) {
        auto bytes = fixture.media;
        const auto position = bytes.size();
        bytes.resize(position + 4 + length);
        StoreLE32(bytes.data() + position, length);
        ASSERT_TRUE(ParseResidentVorbis(bytes.data(), bytes.size(), m));
        const std::uint8_t* packet;
        std::uint32_t actual;
        ASSERT_TRUE(m.Packet(position - static_cast<std::size_t>(m.packets - bytes.data()), packet, actual));
        EXPECT_EQ(length, actual);
        EXPECT_FALSE(ParseResidentVorbis(bytes.data(), bytes.size() - 1, m));
    }
}
}
