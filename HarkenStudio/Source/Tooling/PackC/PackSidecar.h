#pragma once

#include "../Common/JsonReader.h"
#include "PackPlayPath.h"
#include "PackSchemaGenerated.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <set>

namespace harkenstudiojson {

struct PackSidecarPlatform {
    std::string id;
    std::string name;
    std::string basePlatform;
    std::string path;
    bool operator==(const PackSidecarPlatform&) const = default;
};

struct PackSidecarEntry {
    std::string name;
    std::string path;
    std::uint64_t packStableId = 0;
    std::uint64_t contentHash = 0;
    std::vector<std::string> dependencies;
    bool isInit = false;
};

inline std::string PackSidecarText(const JsonValue& value, const char* key) {
    const auto* field = value.Find(key);
    return field && field->kind == JsonValue::Kind::String ? field->text : std::string{};
}

inline bool PackSidecarUuid(const std::string& value) {
    if (value.size() != 36 || value == "00000000-0000-0000-0000-000000000000") return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

inline bool PackSidecarBase(const std::string& value) {
    return value == "Windows" || value == "macOS" || value == "Linux"
        || value == "Android" || value == "iOS";
}

inline std::string PackSidecarFold(std::string value) {
    for (char& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return value;
}

inline bool PackSidecarName(const std::string& name) {
    if (name.empty() || name.size() > 120 || name.front() == ' ' || name.back() == ' '
        || name.back() == '.' || name.find_first_of("<>:\"/\\|?*") != std::string::npos) return false;
    for (unsigned char c : name) if (c < 32 || c == 127) return false;
    const auto stem = PackSidecarFold(name.substr(0, name.find('.')));
    return stem != "con" && stem != "prn" && stem != "aux" && stem != "nul"
        && stem != "conin$" && stem != "conout$"
        && !(stem.size() == 4 && (stem.substr(0, 3) == "com" || stem.substr(0, 3) == "lpt")
            && stem[3] >= '1' && stem[3] <= '9');
}

inline bool PackSidecarPlatformValid(const PackSidecarPlatform& platform) {
    return PackSidecarUuid(platform.id) && PackSidecarName(platform.name)
        && PackSidecarBase(platform.basePlatform) && platform.path == platform.name;
}

inline bool PackSidecarKeys(const JsonValue& value, std::initializer_list<const char*> keys) {
    if (value.kind != JsonValue::Kind::Object) return false;
    for (const auto& member : value.members) {
        if (std::none_of(keys.begin(), keys.end(), [&](const char* key) { return member.first == key; })) return false;
    }
    return true;
}

inline bool PackSidecarRoot(const std::string& json, JsonValue* root, std::string* error) {
    if (!ParseJson(json, root, error, true)) return false;
    const auto* version = root->Find("schema_version");
    const double schemaVersion =
        version && version->kind == JsonValue::Kind::Number
            ? std::strtod(version->text.c_str(), nullptr)
            : 0;
    if (schemaVersion != VNX::PackSchema::kPackManifestSchemaVersion) {
        *error = "Unsupported Pack sidecar schema_version; re-export the Packs with this Studio version";
        return false;
    }
    return true;
}

inline bool ParsePackSidecarIndex(const std::string& json, std::vector<PackSidecarPlatform>* platforms, std::string* error) {
    platforms->clear();
    JsonValue root;
    if (!PackSidecarRoot(json, &root, error)) return false;
    *error = "Invalid Pack index platform identity";
    const auto* values = root.Find("platforms");
    if (!PackSidecarKeys(root, {"schema_version", "platforms"})
        || !values || values->kind != JsonValue::Kind::Array) return false;
    std::set<std::string> ids;
    std::set<std::string> paths;
    for (const auto& row : values->items) {
        if (!PackSidecarKeys(row, {"id", "name", "basePlatform", "path"})) return false;
        PackSidecarPlatform platform{PackSidecarText(row, "id"), PackSidecarText(row, "name"),
            PackSidecarText(row, "basePlatform"), PackSidecarText(row, "path")};
        if (!PackSidecarPlatformValid(platform) || !ids.insert(platform.id).second
            || !paths.insert(PackSidecarFold(platform.path)).second) return false;
        platforms->push_back(std::move(platform));
    }
    error->clear();
    return true;
}

inline bool SelectPackSidecarPlatform(const std::vector<PackSidecarPlatform>& platforms,
    const std::string& id, const std::string& name, const std::string& base,
    PackSidecarPlatform* selected, std::string* error) {
    if ((!id.empty() && (!name.empty() || !PackSidecarUuid(id)))
        || (!name.empty() && !PackSidecarName(name))
        || (id.empty() && name.empty() && !PackSidecarBase(base))) {
        *error = "Specify one --platform-id or --platform-name, or a supported host base platform";
        return false;
    }
    std::vector<const PackSidecarPlatform*> matches;
    for (const auto& platform : platforms) {
        if (!id.empty() ? platform.id == id : !name.empty() ? platform.name == name : platform.basePlatform == base)
            matches.push_back(&platform);
    }
    if (matches.size() != 1) {
        *error = matches.empty() ? "No published Pack platform matches the selection"
            : "Multiple Pack platforms match " + base + "; specify --platform-id or --platform-name:";
        for (const auto* platform : matches) *error += " " + platform->name + " (" + platform->id + ")";
        return false;
    }
    *selected = *matches.front();
    error->clear();
    return true;
}

inline bool PackSidecarU64(const std::string& text, std::uint64_t* value) {
    JsonValue number;
    number.kind = JsonValue::Kind::Number;
    number.text = text;
    return number.AsU64(value);
}

inline bool ParsePackSidecarManifest(const std::string& json, PackSidecarPlatform* platform,
    std::vector<PackSidecarEntry>* packs, std::string* error) {
    packs->clear();
    JsonValue root;
    if (!PackSidecarRoot(json, &root, error)) return false;
    *error = "Invalid Pack manifest platform identity or Pack set";
    if (!PackSidecarKeys(root, {"schema_version", "platformId", "platformName", "basePlatform",
            "packs", "initPackStableId", "language", "groups", "catalogChecksum",
            "gameplayCatalog", "variableCatalog"})) return false;
    *platform = {PackSidecarText(root, "platformId"), PackSidecarText(root, "platformName"),
        PackSidecarText(root, "basePlatform"), PackSidecarText(root, "platformName")};
    const auto* rows = root.Find("packs");
    std::uint64_t initId = 0;
    if (!PackSidecarPlatformValid(*platform) || !rows || rows->kind != JsonValue::Kind::Array
        || !PackSidecarU64(PackSidecarText(root, "initPackStableId"), &initId) || initId == 0) return false;
    std::set<std::uint64_t> ids;
    std::set<std::string> paths;
    for (const auto& row : rows->items) {
        PackSidecarEntry pack;
        pack.name = PackSidecarText(row, "name");
        pack.path = PackSidecarText(row, "path");
        const auto* deps = row.Find("dependencies");
        if (!PackSidecarKeys(row, {"name", "path", "platformId", "packStableId", "contentHash", "packKind",
                "contentRevision", "language", "variantKey", "dependencies"})
            || pack.name.empty() || !PackPlayLocatorLegal(pack.path)
            || PackSidecarText(row, "platformId") != platform->id
            || !PackSidecarU64(PackSidecarText(row, "packStableId"), &pack.packStableId) || pack.packStableId == 0
            || !PackSidecarU64(PackSidecarText(row, "contentHash"), &pack.contentHash)
            || !ids.insert(pack.packStableId).second || !paths.insert(PackSidecarFold(pack.path)).second
            || !deps || deps->kind != JsonValue::Kind::Array) return false;
        pack.isInit = pack.packStableId == initId;
        for (const auto& dep : deps->items) {
            if (dep.kind != JsonValue::Kind::String) return false;
            pack.dependencies.push_back(dep.text);
        }
        packs->push_back(std::move(pack));
    }
    if (!ids.count(initId)) return false;
    for (const auto& pack : *packs) {
        for (const auto& dep : pack.dependencies) {
            std::uint64_t id = 0;
            if (!PackSidecarU64(dep, &id) || !ids.count(id)) return false;
        }
    }
    error->clear();
    return true;
}

inline std::string PackSidecarRead(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

inline bool LoadPackSidecarManifest(const std::filesystem::path& root, PackSidecarPlatform* platform,
    std::vector<PackSidecarEntry>* packs, std::string* error,
    const std::string& language = {}) {
    if (std::filesystem::exists(root / ".harken-pack-publishing")) {
        *error = "Pack platform is being published; retry after the build completes";
        return false;
    }
    const auto inventoryJson = PackSidecarRead(root / "HarkenPackManifest.json");
    if (!ParsePackSidecarManifest(inventoryJson, platform, packs, error)) return false;
    JsonValue inventory;
    if (!PackSidecarRoot(inventoryJson, &inventory, error)) return false;
    const std::string selectedLanguage = language.empty() ? PackSidecarText(inventory, "language") : language;
    if (!selectedLanguage.empty() && inventory.Find("gameplayCatalog")) {
        *error = "Pack language publication is missing or inconsistent";
        std::filesystem::path manifestPath;
        if (!PackPlayLocatorLegal(selectedLanguage) || selectedLanguage.find('/') != std::string::npos
            || !PackPlayJoinContained(root, selectedLanguage + "/HarkenPackManifest.json", &manifestPath)) return false;
        const auto selectedJson = PackSidecarRead(manifestPath);
        PackSidecarPlatform selectedPlatform;
        std::vector<PackSidecarEntry> selected;
        JsonValue selectedRoot;
        if (!ParsePackSidecarManifest(selectedJson, &selectedPlatform, &selected, error)
            || selectedPlatform != *platform || !PackSidecarRoot(selectedJson, &selectedRoot, error)
            || PackSidecarText(selectedRoot, "language") != selectedLanguage
            || PackSidecarText(selectedRoot, "initPackStableId") != PackSidecarText(inventory, "initPackStableId")) return false;
        std::set<std::uint64_t> expected;
        bool knownLanguage = false;
        for (const auto& row : inventory.Find("packs")->items) {
            knownLanguage = knownLanguage || PackSidecarText(row, "language") == selectedLanguage;
            if (PackSidecarText(row, "packStableId") == PackSidecarText(inventory, "initPackStableId")
                || PackSidecarText(row, "language").empty() || PackSidecarText(row, "language") == selectedLanguage) {
                std::uint64_t id = 0;
                if (!PackSidecarU64(PackSidecarText(row, "packStableId"), &id)) return false;
                expected.insert(id);
            }
        }
        if (!knownLanguage || expected.size() != selected.size()) return false;
        for (const auto& pack : selected) {
            const auto original = std::find_if(packs->begin(), packs->end(),
                [&](const auto& row) { return row.packStableId == pack.packStableId; });
            if (original == packs->end() || expected.erase(pack.packStableId) != 1
                || original->name != pack.name || original->dependencies != pack.dependencies) return false;
            if (pack.isInit) {
                if (pack.path != original->path && pack.path != selectedLanguage + "/" + original->path) return false;
                if (pack.path == original->path && pack.contentHash != original->contentHash) return false;
            } else if (pack.path != original->path || pack.contentHash != original->contentHash) return false;
        }
        *packs = std::move(selected);
    } else if (!language.empty()) {
        *error = "Pack manifest has no language publication";
        return false;
    }
    for (const auto& pack : *packs) {
        std::filesystem::path path;
        if (!PackPlayJoinContained(root, pack.path, &path)) {
            *error = "Pack path escapes the platform directory";
            return false;
        }
        std::ifstream file(path, std::ios::binary);
        std::uint64_t hash = 14695981039346656037ull;
        char bytes[65536];
        std::uint64_t count = 0;
        while (file) {
            file.read(bytes, sizeof(bytes));
            const auto read = file.gcount();
            count += static_cast<std::uint64_t>(read);
            for (std::streamsize i = 0; i < read; ++i) hash = (hash ^ static_cast<unsigned char>(bytes[i])) * 1099511628211ull;
        }
        if (!file.eof() || count == 0 || hash != pack.contentHash) {
            *error = "Pack file does not match its platform manifest: " + pack.path;
            return false;
        }
    }
    error->clear();
    return true;
}

inline bool ResolvePackSidecarRoot(const std::filesystem::path& requested, const std::string& id,
    const std::string& name, const std::string& base, std::filesystem::path* resolved, std::string* error,
    const std::string& language = {}) {
    auto indexPath = requested / "HarkenPackIndex.json";
    std::error_code indexError;
    auto indexStatus = std::filesystem::symlink_status(indexPath, indexError);
    if (indexError && indexError != std::errc::no_such_file_or_directory) {
        *error = "Cannot inspect Pack index: " + indexPath.string();
        return false;
    }
    PackSidecarPlatform selected;
    PackSidecarPlatform actual;
    std::vector<PackSidecarEntry> packs;
    if (std::filesystem::exists(indexStatus)) {
        std::vector<PackSidecarPlatform> platforms;
        if (!ParsePackSidecarIndex(PackSidecarRead(indexPath), &platforms, error)
            || !SelectPackSidecarPlatform(platforms, id, name, base, &selected, error)
            || !PackPlayJoinContained(requested, selected.path, resolved)
            || !LoadPackSidecarManifest(*resolved, &actual, &packs, error, language)) return false;
        if (selected != actual) { *error = "Pack index and manifest platform identities do not match"; return false; }
    } else {
        if (!LoadPackSidecarManifest(requested, &actual, &packs, error, language)
            || !SelectPackSidecarPlatform({actual}, id, name, base, &selected, error)) return false;
        *resolved = requested;
    }
    return true;
}

}
