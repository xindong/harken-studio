#pragma once

#include <harken_studio/ObjectId.h>

#include <cstddef>
#include <cstdint>
#include <string>
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
    const float* interleavedSamples = nullptr;
    std::size_t sampleCount = 0;
    std::uint32_t sampleRate = 0;
    std::uint16_t channels = 0;
    bool downmixToMono = false;
    bool loopable = false;
    std::uint64_t loopStartFrame = 0;
    std::uint64_t loopEndFrame = 0;
};

struct CompiledEntry {
    std::uint32_t kind = 0;
    std::uint32_t vintage = 1;
    harkenstudio::ObjectId id{};
    std::uint64_t relationOrdinal = 0;
    std::vector<std::uint8_t> payload;
};

bool compileEntries(const std::string& logicSections, bool resident,
                    const std::vector<MediaInput>& media,
                    std::vector<CompiledEntry>* entries, std::string* error);

Result compile(const Request& request);

}
