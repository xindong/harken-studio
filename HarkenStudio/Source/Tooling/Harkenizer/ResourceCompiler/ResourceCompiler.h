#pragma once

#include <harken_studio/ObjectId.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace harken::studio::VorbisEncoder { struct EncodedMedia; }

namespace harkenizer::resourcecompiler {

// Serialized once per export input; the same bytes identify control and audible media.
struct ResidentVorbisBlob {
    std::vector<std::uint8_t> bytes;
    std::uint64_t checksum = 0;
    std::uint64_t revision() const { return checksum ? checksum : 1; }
};
std::shared_ptr<const ResidentVorbisBlob> prepareResidentVorbis(
    const harken::studio::VorbisEncoder::EncodedMedia& media, std::string* error);

struct MediaInput {
    harken::studio::types::ObjectId id{};
    std::string sourcePath;
    std::string contentHash;
    std::shared_ptr<const std::vector<float>> samplesOwner;
    std::shared_ptr<const ResidentVorbisBlob> residentVorbis;
    const float* interleavedSamples = nullptr;
    std::size_t sampleCount = 0;
    std::uint32_t sampleRate = 0;
    std::uint16_t channels = 0;
    std::uint64_t frameCount = 0;
    bool downmixToMono = false;
    bool loopable = false;
    std::uint64_t loopStartFrame = 0;
    std::uint64_t loopEndFrame = 0;
};

struct PreparedMediaBlob {
    std::shared_ptr<const std::vector<float>> samples;
    std::uint64_t checksum = 0;
};

class MediaCache {
public:
    PreparedMediaBlob prepare(const MediaInput& input);
    std::shared_ptr<const std::vector<float>> intern(
        const std::uint8_t* data, std::size_t bytes, std::uint64_t checksum);
    void clear();

private:
    std::mutex mutex_;
    std::map<std::tuple<std::string, std::uint16_t, bool>, PreparedMediaBlob> prepared_;
    std::unordered_multimap<std::uint64_t,
        std::shared_ptr<const std::vector<float>>> blobs_;
};

struct CompiledEntry {
    std::uint32_t kind = 0;
    std::uint32_t vintage = 1;
    harken::studio::types::ObjectId id{};
    std::uint64_t relationOrdinal = 0;
    std::vector<std::uint8_t> payload;
    std::shared_ptr<const std::vector<float>> media;
    std::shared_ptr<const std::vector<std::uint8_t>> encodedBytes;
};

bool compileEntries(const std::string& logicSections, bool resident,
                    const std::vector<MediaInput>& media,
                    std::vector<CompiledEntry>* entries, std::string* error,
                    MediaCache* mediaCache = nullptr);

}
