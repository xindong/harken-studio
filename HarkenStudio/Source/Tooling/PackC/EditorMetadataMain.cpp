#include "EditorWaveformPublication.h"
#include "PackSidecar.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <stdexcept>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
namespace fs = std::filesystem;
using harkenstudiobuild::tooling::EditorMetadataPackBytes;

std::vector<std::uint8_t> Read(const fs::path& path, std::uintmax_t limit = SIZE_MAX) {
    const auto size = fs::file_size(path);
    if (size > limit || size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max()))
        throw std::runtime_error("Input exceeds its byte budget");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream file(path, std::ios::binary);
    if (!file || !file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())) ||
        file.peek() != std::char_traits<char>::eof()) throw std::runtime_error("Cannot capture complete input");
    return bytes;
}

bool Aliases(const fs::path& left, const fs::path& right) {
    std::error_code error;
    if (fs::equivalent(left, right, error)) return true;
    return fs::weakly_canonical(left) == fs::weakly_canonical(right);
}

fs::path Utf8Path(const std::string& text) {
    return fs::path(std::u8string_view(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

void Publish(const fs::path& output, const std::string& json) {
    std::random_device random;
    auto stage = output;
    stage += ".metadata-" + std::to_string(random()) + "-" + std::to_string(random());
    if (!fs::create_directory(stage)) throw std::runtime_error("Cannot create companion staging directory");
    const auto temp = stage / "companion.json";
    try {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        file.write(json.data(), static_cast<std::streamsize>(json.size()));
        file.close();
        if (!file) throw std::runtime_error("Cannot write complete companion");
#ifdef _WIN32
        if (!MoveFileExW(temp.c_str(), output.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot atomically publish companion");
#else
        fs::rename(temp, output);
#endif
        fs::remove(stage);
    } catch (...) {
        std::error_code ignored;
        fs::remove(temp, ignored);
        fs::remove(stage, ignored);
        throw;
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        std::map<std::string, std::string> args;
        for (int i = 1; i < argc; i += 2) {
            const std::string key = argv[i];
            if (i + 1 == argc || (key != "--manifest" && key != "--pack-root" &&
                key != "--pack-suffix" && key != "--output") || !args.emplace(key, argv[i + 1]).second)
                throw std::runtime_error("Expected --manifest PATH --pack-root PATH [--pack-suffix .bytes] --output PATH");
        }
        for (const auto* key : {"--manifest", "--pack-root", "--output"})
            if (!args.count(key) || args.at(key).empty()) throw std::runtime_error("Missing required path argument");
        const std::string suffix = args.count("--pack-suffix") ? args.at("--pack-suffix") : "";
        if (!suffix.empty() && suffix != ".bytes") throw std::runtime_error("Pack suffix must be empty or .bytes");
        const auto manifestPath = fs::absolute(Utf8Path(args.at("--manifest")));
        const auto root = fs::absolute(Utf8Path(args.at("--pack-root")));
        const auto output = fs::absolute(Utf8Path(args.at("--output")));
        const auto marker = root / ".harken-pack-publishing";
        if (fs::exists(marker)) throw std::runtime_error("Pack publication is in progress");
        if (Aliases(output, manifestPath)) throw std::runtime_error("Output aliases the manifest");
        const auto manifestBytes = Read(manifestPath, harkenstudiobuild::tooling::kEditorWaveformMaxBytes);
        const std::string manifest(manifestBytes.begin(), manifestBytes.end());
        harkenstudiojson::PackSidecarPlatform platform;
        std::vector<harkenstudiojson::PackSidecarEntry> entries;
        std::string error;
        if (!harkenstudiojson::ParsePackSidecarManifest(manifest, &platform, &entries, &error)) throw std::runtime_error(error);
        std::vector<EditorMetadataPackBytes> sources;
        std::vector<fs::path> paths;
        for (const auto& entry : entries) {
            fs::path path;
            if (!harkenstudiojson::PackPlayJoinContained(root, entry.path + suffix, &path))
                throw std::runtime_error("Pack path escapes the publication root");
            if (Aliases(path, output)) throw std::runtime_error("Output aliases a Pack input");
            paths.push_back(path);
            sources.push_back({entry.packStableId, Read(path)});
        }
        std::string companion;
        if (!harkenstudiobuild::tooling::BuildEditorWaveformPublication(manifest, sources, &companion, &error))
            throw std::runtime_error(error);
        if (fs::exists(marker) || Read(manifestPath, harkenstudiobuild::tooling::kEditorWaveformMaxBytes) != manifestBytes)
            throw std::runtime_error("Manifest changed during metadata generation");
        for (std::size_t i = 0; i < paths.size(); ++i)
            if (Read(paths[i]) != sources[i].bytes || Aliases(paths[i], output))
                throw std::runtime_error("Pack identity changed during metadata generation");
        if (Aliases(output, manifestPath) || fs::exists(marker)) throw std::runtime_error("Publication identity changed");
        Publish(output, companion);
        std::cout << "Published " << companion.size() << " companion bytes\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Editor metadata: " << error.what() << '\n';
        return 1;
    }
}
