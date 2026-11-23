#pragma once

#include <QByteArray>
#include <QString>
#include <cstdint>
#include <optional>
#include <vector>

namespace harken::studio::VorbisEncoder {

inline constexpr uint32_t kSeekGrainFrames = 4096;
inline constexpr uint32_t kFormatVersion = 1;

inline constexpr bool supportsChannels(uint32_t channels)
{
    return channels == 1 || channels == 2 || channels == 4 || channels == 6 || channels == 8;
}

struct Packet
{
    QByteArray bytes;
    int64_t granuleFrame = 0;
};

struct SeekEntry
{
    uint64_t outputFrame = 0;
    uint32_t packetIndex = 0;
    uint64_t byteOffset = 0;
    uint64_t discardFrames = 0;
};

struct EncodedMedia
{
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint32_t smallBlockFrames = 0;
    uint32_t largeBlockFrames = 0;
    uint32_t seekGrainFrames = kSeekGrainFrames;
    uint64_t frameCount = 0;
    uint64_t prerollFrames = 0;
    uint64_t tailFrames = 0;
    uint64_t loopStartFrame = 0;
    uint64_t loopEndFrame = 0;
    QString encoderVersion;
    QByteArray identificationHeader;
    QByteArray setupHeader;
    std::vector<Packet> packets;
    std::vector<SeekEntry> seekTable;
};

std::optional<EncodedMedia> encode(const std::vector<float>& samples,
                                  uint32_t sampleRate,
                                  uint16_t channels,
                                  float quality,
                                  uint64_t loopStart,
                                  uint64_t loopEnd,
                                  QString* error = nullptr);
bool validate(const EncodedMedia& media, QString* error = nullptr);

}
