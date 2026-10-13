// .stream container layer in isolation: serialize/parse round-trips
// for the header and TOC entry, magic refusal, and the chunk geometry /
// loop-seek arithmetic both the tool and the streaming reader rely on.
// All pure math over the format header — no engine, no files.
#include "Common/ObjectIdTestSupport.h"
#include "harken/media/StreamFormat.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

TEST(StreamFormat, HeaderRoundTripPreservesEveryField) {
    harken::StreamHeader in;
    in.formatVersion = harken::kStreamFormatVersion;
    in.headerBytes = harken::kStreamHeaderBytes;
    in.fileBytes = 0x0123456789abcdefull;
    in.entryCount = harken::kStreamEntryCount;
    in.tocOffset = harken::kStreamHeaderBytes;
    in.chunkFrames = 4096;
    in.tocChecksum = 0xfedcba9876543210ull;

    std::uint8_t bytes[harken::kStreamHeaderBytes] = {};
    harken::SerializeStreamHeader(in, bytes);
    EXPECT_EQ('V', bytes[0]);
    EXPECT_EQ('S', bytes[1]);
    EXPECT_EQ('D', bytes[2]);
    EXPECT_EQ('A', bytes[3]);

    harken::StreamHeader out;
    ASSERT_TRUE(harken::ParseStreamHeader(bytes, &out));
    EXPECT_EQ(in.formatVersion, out.formatVersion);
    EXPECT_EQ(in.headerBytes, out.headerBytes);
    EXPECT_EQ(in.fileBytes, out.fileBytes);
    EXPECT_EQ(in.entryCount, out.entryCount);
    EXPECT_EQ(in.tocOffset, out.tocOffset);
    EXPECT_EQ(in.chunkFrames, out.chunkFrames);
    EXPECT_EQ(in.tocChecksum, out.tocChecksum);
}

TEST(StreamFormat, HeaderParseRefusesWrongMagic) {
    harken::StreamHeader in;
    std::uint8_t bytes[harken::kStreamHeaderBytes] = {};
    harken::SerializeStreamHeader(in, bytes);
    bytes[2] = 'P'; // one wrong magic byte suffices
    harken::StreamHeader out;
    EXPECT_FALSE(harken::ParseStreamHeader(bytes, &out));
}

TEST(StreamFormat, TocEntryRoundTripPreservesEveryField) {
    harken::StreamTocEntry in;
    in.soundId = harkentest::ObjectId(0x1122334455667788ull);
    in.sampleRate = 44100;
    in.channelCount = 6;
    in.sampleFormat = harken::kStreamSampleFormatF32Planar;
    in.frameCount = 123456789ull;
    in.payloadOffset = 128;
    in.payloadBytes = 0xaabbccddull;
    in.chunkChecksumOffset = 112;
    in.chunkCount = 30140;
    in.flags = harken::kStreamFlagLoopable;
    in.loopStartFrame = 4096;
    in.loopEndFrame = 100000000ull;

    std::uint8_t bytes[harken::kStreamTocEntryBytes] = {};
    harken::SerializeStreamTocEntry(in, bytes);
    const harken::StreamTocEntry out =
        harken::ParseStreamTocEntry(bytes);
    EXPECT_EQ(in.soundId, out.soundId);
    EXPECT_EQ(in.sampleRate, out.sampleRate);
    EXPECT_EQ(in.channelCount, out.channelCount);
    EXPECT_EQ(in.sampleFormat, out.sampleFormat);
    EXPECT_EQ(in.frameCount, out.frameCount);
    EXPECT_EQ(in.payloadOffset, out.payloadOffset);
    EXPECT_EQ(in.payloadBytes, out.payloadBytes);
    EXPECT_EQ(in.chunkChecksumOffset, out.chunkChecksumOffset);
    EXPECT_EQ(in.chunkCount, out.chunkCount);
    EXPECT_EQ(in.flags, out.flags);
    EXPECT_EQ(in.loopStartFrame, out.loopStartFrame);
    EXPECT_EQ(in.loopEndFrame, out.loopEndFrame);
}

TEST(StreamFormat, ChunkGeometryCoversFrameCountExactly) {
    // Ragged tail: 1000 frames over 256-frame chunks is 3 full chunks
    // plus 232 valid frames of padding-backed tail.
    const std::uint32_t chunkFrames = 256;
    const std::uint64_t frameCount = 1000;
    const std::uint16_t channels = 2;

    EXPECT_EQ(256u * 2u * sizeof(float),
              harken::StreamChunkBytes(chunkFrames, channels));
    const std::uint32_t chunkCount =
        harken::StreamChunkCount(frameCount, chunkFrames);
    EXPECT_EQ(4u, chunkCount);

    std::uint64_t covered = 0;
    for (std::uint32_t c = 0; c < chunkCount; ++c) {
        covered += harken::StreamChunkValidFrames(c, chunkFrames,
                                                    frameCount);
    }
    EXPECT_EQ(frameCount, covered);
    EXPECT_EQ(256u,
              harken::StreamChunkValidFrames(0, chunkFrames, frameCount));
    EXPECT_EQ(232u,
              harken::StreamChunkValidFrames(3, chunkFrames, frameCount));
    // Past the end there is no material at all.
    EXPECT_EQ(0u,
              harken::StreamChunkValidFrames(4, chunkFrames, frameCount));

    // An exact multiple has no partial chunk.
    EXPECT_EQ(4u, harken::StreamChunkCount(1024, chunkFrames));
    EXPECT_EQ(256u, harken::StreamChunkValidFrames(3, chunkFrames, 1024));
}

TEST(StreamFormat, SeekDecomposesFrameAgainstChunkGrid) {
    const std::uint32_t chunkFrames = 512;

    // Boundaries: frame 0, last frame of a chunk, first frame of the next.
    harken::StreamSeek s = harken::StreamSeekToFrame(0, chunkFrames);
    EXPECT_EQ(0u, s.chunkIndex);
    EXPECT_EQ(0u, s.frameInChunk);
    s = harken::StreamSeekToFrame(511, chunkFrames);
    EXPECT_EQ(0u, s.chunkIndex);
    EXPECT_EQ(511u, s.frameInChunk);
    s = harken::StreamSeekToFrame(512, chunkFrames);
    EXPECT_EQ(1u, s.chunkIndex);
    EXPECT_EQ(0u, s.frameInChunk);

    // The decomposition identity holds across arbitrary positions,
    // including frames far past 32 bits of samples.
    const std::uint64_t frames[] = {1, 513, 12345, 0x100000000ull + 777};
    for (const std::uint64_t frame : frames) {
        s = harken::StreamSeekToFrame(frame, chunkFrames);
        EXPECT_LT(s.frameInChunk, chunkFrames);
        EXPECT_EQ(frame,
                  static_cast<std::uint64_t>(s.chunkIndex) * chunkFrames +
                      s.frameInChunk);
    }
}

TEST(StreamFormat, LoopSeekLandsOnChunkBoundaryPlusOffset) {
    // The streamed-loop wrap story: a wrap to loopStartFrame must resolve
    // to the file offset of the containing chunk plus an intra-chunk
    // frame offset — pure arithmetic, no scan. Model a stereo file with
    // an awkward loop start that is not chunk-aligned.
    const std::uint32_t chunkFrames = 300; // deliberately not a power of two
    const std::uint16_t channels = 2;
    const std::uint64_t payloadOffset = 128;
    const std::uint64_t loopStartFrame = 1234;

    const std::uint64_t chunkBytes =
        harken::StreamChunkBytes(chunkFrames, channels);
    const harken::StreamSeek seek =
        harken::StreamSeekToFrame(loopStartFrame, chunkFrames);
    EXPECT_EQ(4u, seek.chunkIndex);   // 1234 / 300
    EXPECT_EQ(34u, seek.frameInChunk); // 1234 % 300

    const std::uint64_t offset = harken::StreamChunkFileOffset(
        payloadOffset, seek.chunkIndex, chunkBytes);
    EXPECT_EQ(payloadOffset + 4ull * 300ull * 2ull * sizeof(float), offset);

    // Consuming from that chunk at frameInChunk reaches exactly the loop
    // start: frames before it in the grid equal loopStartFrame.
    EXPECT_EQ(loopStartFrame,
              static_cast<std::uint64_t>(seek.chunkIndex) * chunkFrames +
                  seek.frameInChunk);
}

} // namespace
