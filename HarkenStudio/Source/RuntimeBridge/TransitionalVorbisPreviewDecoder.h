#pragma once

#include <QString>
#include <optional>
#include <vector>

namespace VNX::VorbisEncoder { struct EncodedMedia; }

namespace HarkenEditor::Bridge::Detail {

std::optional<std::vector<float>> decodeTransitionalVorbisPreview(
    const VNX::VorbisEncoder::EncodedMedia& media, QString* error);

}
