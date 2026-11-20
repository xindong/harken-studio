#ifndef HARKEN_HOST_STREAM_IMAGE_H
#define HARKEN_HOST_STREAM_IMAGE_H

#include "harken/media/PackFormatV2.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace harkenhost {

struct StreamSpec {
    VnObjectId soundId{};
    std::uint32_t sampleRate = 48000;
    std::uint16_t channelCount = 1;
    std::uint32_t chunkFrames = 256;
    bool loopable = false;
    std::uint64_t loopStartFrame = 0;
    std::uint64_t loopEndFrame = 0;
};

inline std::vector<std::uint8_t> BuildStreamBytes(
    const StreamSpec& spec, const std::vector<float>& planar) {
    const std::uint64_t frameCount =
        planar.size() / spec.channelCount;
    const std::uint64_t chunkBytes =
        hkPackStreamFileChunkBytes(spec.chunkFrames, spec.channelCount);
    const std::uint32_t chunkCount =
        hkPackStreamFileChunkCount(frameCount, spec.chunkFrames);
    const std::uint64_t checksumOffset =
        static_cast<std::uint64_t>(HK_PACK_STREAM_FILE_HEADER_BYTES) +
        HK_PACK_STREAM_FILE_TOC_ENTRY_BYTES;
    const std::uint64_t checksumBytes =
        static_cast<std::uint64_t>(chunkCount) *
        HK_PACK_STREAM_FILE_CHUNK_CHECKSUM_BYTES;
    const std::uint64_t align = HK_PACK_STREAM_FILE_PAYLOAD_ALIGN;
    const std::uint64_t payloadOffset =
        (checksumOffset + checksumBytes + align - 1) / align * align;
    const std::uint64_t payloadBytes =
        static_cast<std::uint64_t>(chunkCount) * chunkBytes;
    const std::uint64_t fileBytes = payloadOffset + payloadBytes;

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(fileBytes), 0);

    for (std::uint32_t c = 0; c < chunkCount; ++c) {
        const std::uint32_t valid = hkPackStreamFileChunkValidFrames(
            c, spec.chunkFrames, frameCount);
        std::uint8_t* chunk =
            bytes.data() +
            static_cast<std::size_t>(hkPackStreamFileChunkOffset(
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
                    HK_PACK_STREAM_FILE_CHUNK_CHECKSUM_BYTES,
            harken::Fnv1a64(chunk, static_cast<std::size_t>(chunkBytes)));
    }

    harken::StreamTocEntry toc;
    toc.soundId = spec.soundId;
    toc.sampleRate = spec.sampleRate;
    toc.channelCount = spec.channelCount;
    toc.sampleFormat = HK_PACK_SAMPLE_FORMAT_F32_PLANAR;
    toc.frameCount = frameCount;
    toc.payloadOffset = payloadOffset;
    toc.payloadBytes = payloadBytes;
    toc.chunkChecksumOffset = checksumOffset;
    toc.chunkCount = chunkCount;
    toc.flags = spec.loopable ? HK_PACK_MEDIA_FLAG_LOOPABLE : 0;
    toc.loopStartFrame = spec.loopable ? spec.loopStartFrame : 0;
    toc.loopEndFrame = spec.loopable ? spec.loopEndFrame : 0;
    harken::SerializeStreamTocEntry(
        toc, bytes.data() + HK_PACK_STREAM_FILE_HEADER_BYTES);

    std::uint64_t tocChecksum =
        harken::Fnv1a64(bytes.data() + HK_PACK_STREAM_FILE_HEADER_BYTES,
                       HK_PACK_STREAM_FILE_TOC_ENTRY_BYTES);
    tocChecksum = harken::Fnv1a64(
        bytes.data() + static_cast<std::size_t>(checksumOffset),
        static_cast<std::size_t>(checksumBytes), tocChecksum);

    harken::StreamHeader header;
    header.formatVersion = HK_PACK_STREAM_FILE_FORMAT_VERSION;
    header.headerBytes = HK_PACK_STREAM_FILE_HEADER_BYTES;
    header.fileBytes = fileBytes;
    header.entryCount = HK_PACK_STREAM_FILE_ENTRY_COUNT;
    header.tocOffset = HK_PACK_STREAM_FILE_HEADER_BYTES;
    header.chunkFrames = spec.chunkFrames;
    header.tocChecksum = tocChecksum;
    harken::SerializeStreamHeader(header, bytes.data());
    return bytes;
}

inline std::vector<float> DeinterleaveFrames(
    const std::vector<float>& interleaved, std::uint32_t channels) {
    const std::size_t frames = interleaved.size() / channels;
    std::vector<float> planar(interleaved.size());
    for (std::size_t f = 0; f < frames; ++f) {
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            planar[ch * frames + f] = interleaved[f * channels + ch];
        }
    }
    return planar;
}

} // namespace harkenhost

#endif // HARKEN_HOST_STREAM_IMAGE_H
