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

namespace harkenizer::resourcecompiler {

enum class Container { Pack, Stream };

struct Request {
    std::vector<std::string> manifestPaths;
    Container container = Container::Pack;
    bool disableCache = false;
    std::string reportPath;
    std::string cachePath;
    std::string emitManifestPath;
};

struct Result {
    bool success = false;
    std::string error;
    std::string reportPath;
    std::string outputLog;
    std::string diagnosticLog;
};

struct MediaInput {
    harkenstudio::ObjectId id{};
    std::string sourcePath;
    std::string contentHash;
    std::shared_ptr<const std::vector<float>> samplesOwner;
    const float* interleavedSamples = nullptr;
    std::size_t sampleCount = 0;
    std::uint32_t sampleRate = 0;
    std::uint16_t channels = 0;
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
    harkenstudio::ObjectId id{};
    std::uint64_t relationOrdinal = 0;
    std::vector<std::uint8_t> payload;
    std::shared_ptr<const std::vector<float>> media;
};

bool compileEntries(const std::string& logicSections, bool resident,
                    const std::vector<MediaInput>& media,
                    std::vector<CompiledEntry>* entries, std::string* error,
                    MediaCache* mediaCache = nullptr);

Result compile(const Request& request);

}
