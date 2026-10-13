#ifndef HARKEN_TOOLING_EDITOR_WAVEFORM_PUBLICATION_H
#define HARKEN_TOOLING_EDITOR_WAVEFORM_PUBLICATION_H

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace harkenstudiobuild::tooling {

inline constexpr char kEditorWaveformFileName[] = "HarkenEditorWaveforms.json";
inline constexpr std::size_t kEditorWaveformMaxBytes = 16 * 1024 * 1024;
inline constexpr std::uint32_t kEditorWaveformMaxBuckets = 2048;
inline constexpr std::uint32_t kEditorWaveformMaxChannels = 32;
inline constexpr std::uint32_t kEditorWaveformMaxActions = 4096;
inline constexpr std::uint32_t kEditorWaveformMaxCandidates = 256;

struct EditorMetadataPackBytes {
    std::uint64_t packStableId = 0;
    std::vector<std::uint8_t> bytes;
};

// Pure offline inspection of one already-selected, final Pack publication.
bool BuildEditorWaveformPublication(
    std::string_view selectedManifestUtf8,
    std::span<const EditorMetadataPackBytes> packs,
    std::string* companionJson, std::string* error);

} // namespace harkenstudiobuild::tooling

#endif
