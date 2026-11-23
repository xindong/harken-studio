#include "ResidentVorbis.h"
#include "../Core/VorbisEncoder.h"
#include <harken_studio_build/PackFormat.h>
#include <cstring>

namespace harken::studio::VorbisEncoder {
std::shared_ptr<const ResidentVorbisBlob> prepareResidentVorbis(
    const EncodedMedia& media, QString* error) {
    const auto refuse = [&](const char* reason) -> std::shared_ptr<const ResidentVorbisBlob> {
        if (error) *error = QString::fromLatin1(reason);
        return {};
    };
    if (!supportsChannels(media.channels))
        return refuse("Resident Vorbis supports 1, 2, 4, 6 or 8 channels");
    if (media.seekGrainFrames != HK_PACK_VORBIS_SEEK_GRAIN || media.seekTable.empty() ||
        media.seekTable.size() > UINT32_MAX || media.identificationHeader.size() != 30 ||
        media.setupHeader.size() < 7 || media.setupHeader.size() > UINT32_MAX ||
        media.packets.empty())
        return refuse("Invalid resident Vorbis descriptor");

    using namespace harken::studio::build;
    auto result = std::make_shared<ResidentVorbisBlob>();
    auto& bytes = result->bytes;
    const auto seekStart = sizeof(HkPackVorbisHeader) +
        static_cast<uint64_t>(media.identificationHeader.size() + media.setupHeader.size());
    const auto packetStart = seekStart + uint64_t(media.seekTable.size()) * sizeof(HkPackVorbisSeek);
    uint64_t total = packetStart;
    for (const auto& packet : media.packets) {
        const auto size = static_cast<uint64_t>(packet.bytes.size());
        if (!size || size > UINT32_MAX || total > UINT64_MAX - sizeof(uint32_t) - size)
            return refuse("Invalid resident Vorbis packet size");
        total += sizeof(uint32_t) + size;
    }
    if (total > bytes.max_size()) return refuse("Resident Vorbis payload is too large");
    bytes.resize(static_cast<size_t>(total));
    auto* header = bytes.data();
    StoreLE32(header + offsetof(HkPackVorbisHeader, version), HK_PACK_VORBIS_VERSION);
    StoreLE32(header + offsetof(HkPackVorbisHeader, sampleRate), media.sampleRate);
    StoreLE16(header + offsetof(HkPackVorbisHeader, channels), media.channels);
    StoreLE16(header + offsetof(HkPackVorbisHeader, smallBlockFrames), static_cast<uint16_t>(media.smallBlockFrames));
    StoreLE16(header + offsetof(HkPackVorbisHeader, largeBlockFrames), static_cast<uint16_t>(media.largeBlockFrames));
    StoreLE64(header + offsetof(HkPackVorbisHeader, frameCount), media.frameCount);
    StoreLE64(header + offsetof(HkPackVorbisHeader, prerollFrames), media.prerollFrames);
    StoreLE64(header + offsetof(HkPackVorbisHeader, tailFrames), media.tailFrames);
    StoreLE64(header + offsetof(HkPackVorbisHeader, loopStartFrame), media.loopStartFrame);
    StoreLE64(header + offsetof(HkPackVorbisHeader, loopEndFrame), media.loopEndFrame);
    StoreLE32(header + offsetof(HkPackVorbisHeader, identificationBytes), static_cast<uint32_t>(media.identificationHeader.size()));
    StoreLE32(header + offsetof(HkPackVorbisHeader, setupBytes), static_cast<uint32_t>(media.setupHeader.size()));
    StoreLE32(header + offsetof(HkPackVorbisHeader, seekCount), static_cast<uint32_t>(media.seekTable.size()));
    StoreLE32(header + offsetof(HkPackVorbisHeader, seekGrainFrames), media.seekGrainFrames);
    auto* ident = header + sizeof(HkPackVorbisHeader);
    std::memcpy(ident, media.identificationHeader.constData(), media.identificationHeader.size());
    std::memcpy(ident + media.identificationHeader.size(), media.setupHeader.constData(), media.setupHeader.size());
    uint64_t offset = packetStart;
    size_t seek = 0;
    for (size_t index = 0; index < media.packets.size(); ++index) {
        while (seek < media.seekTable.size() && media.seekTable[seek].packetIndex == index) {
            const auto& point = media.seekTable[seek];
            if (point.byteOffset != offset - packetStart - uint64_t(index) * sizeof(uint32_t) ||
                point.discardFrames > UINT32_MAX)
                return refuse("Invalid resident Vorbis seek point");
            auto* dst = bytes.data() + seekStart + seek * sizeof(HkPackVorbisSeek);
            StoreLE64(dst + offsetof(HkPackVorbisSeek, outputFrame), point.outputFrame);
            StoreLE64(dst + offsetof(HkPackVorbisSeek, packetOffset), offset);
            StoreLE32(dst + offsetof(HkPackVorbisSeek, discardFrames), static_cast<uint32_t>(point.discardFrames));
            ++seek;
        }
        const auto& packet = media.packets[index].bytes;
        StoreLE32(bytes.data() + offset, static_cast<uint32_t>(packet.size()));
        std::memcpy(bytes.data() + offset + sizeof(uint32_t), packet.constData(), packet.size());
        offset += sizeof(uint32_t) + static_cast<uint64_t>(packet.size());
    }
    if (seek != media.seekTable.size()) return refuse("Invalid resident Vorbis seek index");
    result->checksum = Fnv1a64(bytes.data(), bytes.size());
    return result;
}
}
