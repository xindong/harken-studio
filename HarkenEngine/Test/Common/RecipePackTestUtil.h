#ifndef HARKEN_TEST_SUPPORT_RECIPE_PACK_TEST_UTIL_H
#define HARKEN_TEST_SUPPORT_RECIPE_PACK_TEST_UTIL_H

#include <harken/Harken.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <process.h>

#ifndef HARKEN_PACKC_PATH
#error "HARKEN_PACKC_PATH must point at the packc executable"
#endif
#ifndef HARKEN_TEMP_DIR
#error "HARKEN_TEMP_DIR must point at the repository's .temp directory"
#endif

namespace harkentest {

struct RecipePackArtifact {
    bool built = false;
    std::string path;
};

inline bool WriteRecipeWav(const std::string& path,
                           std::uint32_t frames = 2048) {
    constexpr std::uint32_t channels = 1;
    constexpr std::uint32_t sampleRate = 48000;
    constexpr std::uint32_t bytesPerSample = 4;
    const std::vector<float> samples(frames, 0.5f);
    const std::uint16_t blockAlign =
        static_cast<std::uint16_t>(channels * bytesPerSample);
    const std::uint32_t dataBytes =
        static_cast<std::uint32_t>(samples.size() * sizeof(float));
    const std::uint32_t byteRate = sampleRate * blockAlign;
    const std::uint32_t fmtSize = 18;
    const std::uint32_t factSize = 4;
    const std::uint32_t riffSize =
        4 + (8 + fmtSize) + (8 + factSize) + (8 + dataBytes);
    const std::uint16_t formatIeeeFloat = 3;
    const std::uint16_t bitsPerSample = 32;
    const std::uint16_t cbSize = 0;
    const std::uint16_t channels16 = channels;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.good()) {
        return false;
    }
    const auto write = [&](const void* data, std::size_t bytes) {
        out.write(static_cast<const char*>(data),
                  static_cast<std::streamsize>(bytes));
    };
    write("RIFF", 4);
    write(&riffSize, 4);
    write("WAVE", 4);
    write("fmt ", 4);
    write(&fmtSize, 4);
    write(&formatIeeeFloat, 2);
    write(&channels16, 2);
    write(&sampleRate, 4);
    write(&byteRate, 4);
    write(&blockAlign, 2);
    write(&bitsPerSample, 2);
    write(&cbSize, 2);
    write("fact", 4);
    write(&factSize, 4);
    write(&frames, 4);
    write("data", 4);
    write(&dataBytes, 4);
    write(samples.data(), dataBytes);
    return out.good();
}

inline RecipePackArtifact BuildRecipePack(
    const std::string& stem, const char* effectType,
    const char* effectParameters,
    const std::vector<VnBindingId>& bindings = {},
    std::uint32_t sourceFrames = 2048) {
    std::filesystem::create_directories(HARKEN_TEMP_DIR);
    const std::string unique =
        stem + "_" + std::to_string(_getpid());
    const std::filesystem::path base =
        std::filesystem::path(HARKEN_TEMP_DIR);
    const std::string wav =
        (base / (unique + ".wav")).generic_string();
    const std::string pack =
        (base / (unique + ".pack")).generic_string();
    const std::string manifest =
        (base / (unique + ".json")).generic_string();
    RecipePackArtifact artifact;
    artifact.path = pack;
    if (!WriteRecipeWav(wav, sourceFrames)) {
        return artifact;
    }
    std::string bindingJson;
    for (std::size_t i = 0; i < bindings.size(); ++i) {
        if (i != 0) {
            bindingJson += ",";
        }
        bindingJson += "\"" + harken::ObjectIdString(bindings[i]) + "\"";
    }
    const std::string json =
        "{\n"
        "  \"manifestVersion\": 3,\n"
        "  \"output\": \"" + pack + "\",\n"
        "  \"packKind\": \"content\",\n"
        "  \"entries\": [{\"path\": \"" + wav +
        "\", \"id\": \"3da91a64-b947-4560-0000-00000000238d\"}],\n"
        "  \"voiceRecipes\": [{\n"
        "    \"playable\": \"3da91a64-b947-4560-0000-00000000238d\",\n"
        "    \"effects\": [{\"slot\": 0, \"type\": \"" +
        std::string(effectType) + "\", "
        "\"descriptorVintage\": 2, \"bypass\": false, "
        "\"parameters\": [" + std::string(effectParameters) + "]}],\n"
        "    \"bindings\": [" + bindingJson + "]\n"
        "  }]\n"
        "}\n";
    {
        std::ofstream out(manifest,
                          std::ios::binary | std::ios::trunc);
        if (!out.good()) {
            return artifact;
        }
        out.write(json.data(), static_cast<std::streamsize>(json.size()));
        if (!out.good()) {
            return artifact;
        }
    }
    std::string exe = HARKEN_PACKC_PATH;
    for (char& c : exe) {
        if (c == '/') {
            c = '\\';
        }
    }
    const std::string command =
        "\"\"" + exe + "\" \"" + manifest + "\"\"";
    artifact.built = std::system(command.c_str()) == 0;
    return artifact;
}

inline RecipePackArtifact BuildGainRecipePack(
    const std::string& stem, const std::vector<VnBindingId>& bindings = {}) {
    return BuildRecipePack(stem, "gain", "-12.0", bindings);
}

// A recipe carrying an authored Tone segment. packc always serializes the
// current VoiceRecipe table; the tone JSON fills the tone slot rather than
// selecting a smaller vintage. The caller provides the tone block's JSON
// body; every tone field must carry an in-range value because the validator
// checks all of them.
inline RecipePackArtifact BuildToneRecipePack(
    const std::string& stem, const std::string& toneJson,
    const std::vector<VnBindingId>& bindings = {}) {
    std::filesystem::create_directories(HARKEN_TEMP_DIR);
    const std::string unique = stem + "_" + std::to_string(_getpid());
    const std::filesystem::path base =
        std::filesystem::path(HARKEN_TEMP_DIR);
    const std::string wav =
        (base / (unique + ".wav")).generic_string();
    const std::string pack =
        (base / (unique + ".pack")).generic_string();
    const std::string manifest =
        (base / (unique + ".json")).generic_string();
    RecipePackArtifact artifact;
    artifact.path = pack;
    if (!WriteRecipeWav(wav, 2048)) {
        return artifact;
    }
    std::string bindingJson;
    for (std::size_t i = 0; i < bindings.size(); ++i) {
        if (i != 0) {
            bindingJson += ",";
        }
        bindingJson += "\"" + harken::ObjectIdString(bindings[i]) + "\"";
    }
    const std::string json =
        "{\n"
        "  \"manifestVersion\": 3,\n"
        "  \"output\": \"" + pack + "\",\n"
        "  \"packKind\": \"content\",\n"
        "  \"entries\": [{\"path\": \"" + wav +
        "\", \"id\": \"3da91a64-b947-4560-0000-00000000238d\"}],\n"
        "  \"voiceRecipes\": [{\n"
        "    \"playable\": \"3da91a64-b947-4560-0000-00000000238d\",\n"
        "    \"tone\": {\n" + toneJson +
        "    },\n"
        "    \"effects\": [{\"slot\": 0, \"type\": \"gain\", "
        "\"descriptorVintage\": 2, \"bypass\": false, "
        "\"parameters\": [-6.0]}],\n"
        "    \"bindings\": [" + bindingJson + "]\n"
        "  }]\n"
        "}\n";
    {
        std::ofstream out(manifest,
                          std::ios::binary | std::ios::trunc);
        if (!out.good()) {
            return artifact;
        }
        out.write(json.data(), static_cast<std::streamsize>(json.size()));
        if (!out.good()) {
            return artifact;
        }
    }
    std::string exe = HARKEN_PACKC_PATH;
    for (char& c : exe) {
        if (c == '/') {
            c = '\\';
        }
    }
    const std::string command =
        "\"\"" + exe + "\" \"" + manifest + "\"\"";
    artifact.built = std::system(command.c_str()) == 0;
    return artifact;
}

// A single enabled low shelf keeps the rendered effect non-trivial while
// leaving the other three sections at their defaults.
inline RecipePackArtifact BuildToneGainRecipePack(
    const std::string& stem, const std::vector<VnBindingId>& bindings = {}) {
    return BuildToneRecipePack(
        stem,
        "      \"lowpassEnabled\": false, \"lowpassFrequencyHz\": 8000.0, "
        "\"lowpassQ\": 0.707,\n"
        "      \"highpassEnabled\": false, \"highpassFrequencyHz\": 80.0, "
        "\"highpassQ\": 0.707,\n"
        "      \"lowShelfEnabled\": true, \"lowShelfFrequencyHz\": 200.0, "
        "\"lowShelfQ\": 0.707, \"lowShelfGainDb\": 6.0,\n"
        "      \"highShelfEnabled\": false, \"highShelfFrequencyHz\": 6000.0, "
        "\"highShelfQ\": 0.707, \"highShelfGainDb\": 0.0\n",
        bindings);
}

// Every section enabled with values distinct from the engine defaults, so a
// restoration check (park/return) discriminates all four physical sections
// and every tone-family target.
inline RecipePackArtifact BuildFourSectionToneRecipePack(
    const std::string& stem, const std::vector<VnBindingId>& bindings = {}) {
    return BuildToneRecipePack(
        stem,
        "      \"lowpassEnabled\": true, \"lowpassFrequencyHz\": 8000.0, "
        "\"lowpassQ\": 1.0,\n"
        "      \"highpassEnabled\": true, \"highpassFrequencyHz\": 80.0, "
        "\"highpassQ\": 1.0,\n"
        "      \"lowShelfEnabled\": true, \"lowShelfFrequencyHz\": 200.0, "
        "\"lowShelfQ\": 1.0, \"lowShelfGainDb\": 6.0,\n"
        "      \"highShelfEnabled\": true, \"highShelfFrequencyHz\": 4000.0, "
        "\"highShelfQ\": 1.0, \"highShelfGainDb\": -3.0\n",
        bindings);
}

inline RecipePackArtifact BuildLimiterRecipePack(
    const std::string& stem, const std::vector<VnBindingId>& bindings = {}) {
    return BuildRecipePack(stem, "limiter", "-12.0, 150.0", bindings,
                           8192);
}

} // namespace harkentest

#endif // HARKEN_TEST_SUPPORT_RECIPE_PACK_TEST_UTIL_H
