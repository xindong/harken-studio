#pragma once

#include <QString>
#include <optional>
#include <vector>

namespace harken::studio::VorbisEncoder { struct EncodedMedia; }

namespace harken::studio::bridge::Detail {

std::optional<std::vector<float>> decodeTransitionalVorbisPreview(
    const harken::studio::VorbisEncoder::EncodedMedia& media, QString* error);

}
