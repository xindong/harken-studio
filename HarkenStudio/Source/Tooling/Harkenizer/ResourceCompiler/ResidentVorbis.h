#pragma once

#include <cstdint>
#include <memory>
#include <vector>

class QString;

namespace harken::studio::VorbisEncoder {

struct EncodedMedia;

struct ResidentVorbisBlob
{
    std::vector<std::uint8_t> bytes;
    std::uint64_t checksum = 0;
    std::uint64_t revision() const { return checksum ? checksum : 1; }
};

std::shared_ptr<const ResidentVorbisBlob> prepareResidentVorbis(
    const EncodedMedia& media, QString* error = nullptr);

}
