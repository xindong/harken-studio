#pragma once

#include "harken/media/PackMediaRange.h"
#include "harken/media/StreamFormat.h"
#include "harken/media/StreamManager.h"
#include "harken/media/VorbisDecoder.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>

namespace harkentest {

// Keep existing PCM corruption/read probes in media-relative coordinates while
// supplying the external container header used by the Pack consumer.
class ExternalPcmFixtureSource final : public harken::StreamByteSource {
public:
    explicit ExternalPcmFixtureSource(std::shared_ptr<const harken::StreamByteSource> media)
        : media_(std::move(media)) {
        harken::StoreLE32(header_.data(), harken::vorbis_test_wire::kStreamMagic);
        harken::StoreLE16(header_.data() + 4, harken::vorbis_test_wire::kVersion);
        harken::StoreLE16(header_.data() + 6, 0);
        harken::StoreLE64(header_.data() + 8, header_.size());
        harken::StoreLE64(header_.data() + 16, media_->ByteCount());
    }
    std::uint64_t ByteCount() const noexcept override { return header_.size() + media_->ByteCount(); }
    harken::StreamReadResult ReadAt(std::uint64_t offset, void* destination, std::size_t count,
        harken::StreamReadCancellation cancel) const override {
        if (cancel.IsCancelled()) return {harken::StreamReadStatus::Cancelled, 0};
        if (offset > ByteCount() || count > ByteCount() - offset) return {};
        if (offset < header_.size()) {
            count = std::min<std::size_t>(count, header_.size() - static_cast<std::size_t>(offset));
            std::memcpy(destination, header_.data() + offset, count);
            return {harken::StreamReadStatus::Ok, count};
        }
        return media_->ReadAt(offset - header_.size(), destination, count, cancel);
    }
private:
    std::shared_ptr<const harken::StreamByteSource> media_;
    std::array<std::uint8_t, harken::vorbis_test_wire::kStreamHeaderBytes> header_{};
};

class ExternalPcmFixtureLocator final : public harken::StreamByteLocator {
public:
    using OpenFn = std::function<std::shared_ptr<const harken::StreamByteSource>(harken::StreamReadCancellation)>;
    explicit ExternalPcmFixtureLocator(OpenFn open) : open_(std::move(open)) {}
    std::shared_ptr<const harken::StreamByteSource> Open(harken::StreamReadCancellation cancel) const override {
        auto source = open_(cancel);
        return source ? std::make_shared<ExternalPcmFixtureSource>(std::move(source)) : nullptr;
    }
private:
    OpenFn open_;
};

// Derive declarations from the valid fixture before applying any corruption to
// its eventual source. Runtime must independently verify those declarations.
inline std::shared_ptr<const harken::PackMediaRange> ExternalPcmFixtureRange(
    const std::vector<std::uint8_t>& expected, ExternalPcmFixtureLocator::OpenFn open) {
    harken::StreamHeader header{};
    if (expected.size() < harken::kStreamHeaderBytes ||
        !harken::ParseStreamHeader(expected.data(), &header) || header.tocOffset > expected.size() ||
        harken::kStreamTocEntryBytes > expected.size() - header.tocOffset) return {};
    const auto toc = harken::ParseStreamTocEntry(expected.data() + header.tocOffset);
    if (toc.chunkChecksumOffset > expected.size() ||
        std::uint64_t(toc.chunkCount) * 8 > expected.size() - toc.chunkChecksumOffset) return {};
    std::vector<std::uint64_t> checksums(toc.chunkCount);
    for (std::size_t i = 0; i < checksums.size(); ++i)
        checksums[i] = harken::LoadLE64(expected.data() + toc.chunkChecksumOffset + i * 8);
    const bool loop = (toc.flags & harken::kStreamFlagLoopable) != 0;
    const auto loopEnd = loop ? (toc.loopEndFrame ? toc.loopEndFrame : toc.frameCount) : 0;
    harken::MediaStreamSemanticHeaderV1 semantic{toc.sampleRate, toc.channelCount, toc.frameCount,
        toc.sampleFormat, header.formatVersion, header.chunkFrames, toc.chunkCount,
        loop ? toc.loopStartFrame : 0, loopEnd};
    auto range = std::make_shared<harken::PackMediaRange>();
    if (!harken::ComputeMediaStreamContentRevisionV1(semantic, checksums.data(), &range->contentRevision)) return {};
    auto external = std::make_shared<harken::PackExternalStream>();
    external->codec = harken::PackExternalStream::Codec::Pcm;
    external->fileBytes = expected.size() + harken::vorbis_test_wire::kStreamHeaderBytes;
    external->locator = std::make_shared<ExternalPcmFixtureLocator>(std::move(open));
    range->external = std::move(external);
    range->blobOffset = harken::vorbis_test_wire::kStreamHeaderBytes;
    range->sampleRate = toc.sampleRate;
    range->channelCount = toc.channelCount;
    range->frameCount = toc.frameCount;
    range->packFlags = toc.flags;
    range->loop = {loop ? toc.loopStartFrame : 0, loopEnd};
    return range;
}

} // namespace harkentest
