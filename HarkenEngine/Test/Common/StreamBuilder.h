// Test-local .stream v1 image builder. Deliberately independent of the
// packc tool: engine-side tests need to construct containers in memory so
// they can corrupt one specific region and prove the reader refuses it,
// which a tool that only writes valid files cannot provide. The layout is
// produced entirely through the format header's serialize helpers and
// chunk geometry math, so builder and reader share one definition of the
// bytes.
#ifndef HARKEN_TESTS_TESTSUPPORT_STREAM_BUILDER_H
#define HARKEN_TESTS_TESTSUPPORT_STREAM_BUILDER_H

#include "Common/ObjectIdTestSupport.h"
#include "harken/media/StreamFormat.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace harkentest {

struct StreamSpec {
    VnObjectId soundId = harkentest::ObjectId(0);
    std::uint32_t sampleRate = 48000;
    std::uint16_t channelCount = 1;
    std::uint32_t chunkFrames = 256;
    bool loopable = false;
    std::uint64_t loopStartFrame = 0;
    std::uint64_t loopEndFrame = 0; // 0 = whole file when loopable
};

// Builds a complete, valid container image: header at 0, one TOC entry
// after it, the chunk-checksum table after the TOC, payload at the next
// 64-byte boundary. `planar` is the source as concatenated channel planes
// (channel 0's frames, then channel 1's, ...); frameCount is derived from
// its size.
inline std::vector<std::uint8_t> BuildStreamBytes(
    const StreamSpec& spec, const std::vector<float>& planar) {
    const std::uint64_t frameCount =
        planar.size() / spec.channelCount;
    const std::uint64_t chunkBytes =
        harken::StreamChunkBytes(spec.chunkFrames, spec.channelCount);
    const std::uint32_t chunkCount =
        harken::StreamChunkCount(frameCount, spec.chunkFrames);
    const std::uint64_t checksumOffset =
        static_cast<std::uint64_t>(harken::kStreamHeaderBytes) +
        harken::kStreamTocEntryBytes;
    const std::uint64_t checksumBytes =
        static_cast<std::uint64_t>(chunkCount) *
        harken::kStreamChunkChecksumBytes;
    const std::uint64_t align = harken::kStreamPayloadAlign;
    const std::uint64_t payloadOffset =
        (checksumOffset + checksumBytes + align - 1) / align * align;
    const std::uint64_t payloadBytes =
        static_cast<std::uint64_t>(chunkCount) * chunkBytes;
    const std::uint64_t fileBytes = payloadOffset + payloadBytes;

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(fileBytes), 0);

    // Payload: planar within each chunk, final chunk zero-padded; each
    // chunk's checksum covers the full fixed span including the padding.
    for (std::uint32_t c = 0; c < chunkCount; ++c) {
        const std::uint32_t valid = harken::StreamChunkValidFrames(
            c, spec.chunkFrames, frameCount);
        std::uint8_t* chunk =
            bytes.data() +
            static_cast<std::size_t>(harken::StreamChunkFileOffset(
                payloadOffset, c, chunkBytes));
        for (std::uint32_t ch = 0; ch < spec.channelCount; ++ch) {
            const float* src =
                planar.data() +
                static_cast<std::size_t>(ch) * frameCount +
                static_cast<std::size_t>(c) * spec.chunkFrames;
            std::memcpy(chunk + static_cast<std::size_t>(ch) *
                                    spec.chunkFrames * sizeof(float),
                        src,
                        static_cast<std::size_t>(valid) * sizeof(float));
        }
        harken::StoreLE64(
            bytes.data() + static_cast<std::size_t>(checksumOffset) +
                static_cast<std::size_t>(c) *
                    harken::kStreamChunkChecksumBytes,
            harken::Fnv1a64(chunk, static_cast<std::size_t>(chunkBytes)));
    }

    harken::StreamTocEntry toc;
    toc.soundId = spec.soundId;
    toc.sampleRate = spec.sampleRate;
    toc.channelCount = spec.channelCount;
    toc.sampleFormat = harken::kStreamSampleFormatF32Planar;
    toc.frameCount = frameCount;
    toc.payloadOffset = payloadOffset;
    toc.payloadBytes = payloadBytes;
    toc.chunkChecksumOffset = checksumOffset;
    toc.chunkCount = chunkCount;
    toc.flags = spec.loopable ? harken::kStreamFlagLoopable : 0;
    toc.loopStartFrame = spec.loopable ? spec.loopStartFrame : 0;
    toc.loopEndFrame = spec.loopable ? spec.loopEndFrame : 0;
    harken::SerializeStreamTocEntry(
        toc, bytes.data() + harken::kStreamHeaderBytes);

    // tocChecksum chains the TOC entry bytes then the chunk-checksum
    // table bytes, matching the reader's registration-time recomputation.
    std::uint64_t tocChecksum =
        harken::Fnv1a64(bytes.data() + harken::kStreamHeaderBytes,
                       harken::kStreamTocEntryBytes);
    tocChecksum = harken::Fnv1a64(
        bytes.data() + static_cast<std::size_t>(checksumOffset),
        static_cast<std::size_t>(checksumBytes), tocChecksum);

    harken::StreamHeader header;
    header.formatVersion = harken::kStreamFormatVersion;
    header.headerBytes = harken::kStreamHeaderBytes;
    header.fileBytes = fileBytes;
    header.entryCount = harken::kStreamEntryCount;
    header.tocOffset = harken::kStreamHeaderBytes;
    header.chunkFrames = spec.chunkFrames;
    header.tocChecksum = tocChecksum;
    harken::SerializeStreamHeader(header, bytes.data());
    return bytes;
}

inline void WriteFileBytes(const std::string& path,
                           const std::vector<std::uint8_t>& bytes) {
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.good()) << "cannot open " << path;
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(out.good()) << "cannot write " << path;
}

} // namespace harkentest

#endif // HARKEN_TESTS_TESTSUPPORT_STREAM_BUILDER_H
