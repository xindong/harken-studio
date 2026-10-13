#pragma once

#include "harken_studio_build/PackFormatV2.h"

#include <set>
#include <string>

namespace harkenstudiobuild {

inline constexpr std::uint32_t kVariableNameCatalogHeaderBytes = 64;
inline constexpr std::uint32_t kVariableNameCatalogRowBytes = 32;
inline constexpr std::uint32_t kVariableNameCatalogMaxRows = 8192;
inline constexpr std::uint32_t kVariableNameCatalogMaxBytes = 1048576;

struct PackVariableNameRow {
    harkenstudio::ObjectId variableId{};
    std::string name;
};

struct PackVariableNameCatalog {
    std::array<std::uint8_t, 16> projectGuid{};
    std::uint64_t initPackStableId = 0;
    std::uint64_t initContentRevision = 0;
    std::vector<PackVariableNameRow> rows;
};

inline bool IsSdkCallableName(std::string_view name) {
    if (name.empty() || name.size() > 128) return false;
    const auto letter = [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    };
    if (!letter(static_cast<unsigned char>(name.front()))) return false;
    for (const unsigned char c : name) {
        if (!letter(c) && (c < '0' || c > '9') && c != '_') return false;
    }
    return true;
}

inline bool ValidateVariableNameCatalog(const PackVariableNameCatalog& catalog) {
    if (std::all_of(catalog.projectGuid.begin(), catalog.projectGuid.end(),
                    [](auto byte) { return byte == 0; }) ||
        catalog.initPackStableId == 0 || catalog.initContentRevision == 0 ||
        catalog.rows.size() > kVariableNameCatalogMaxRows) return false;
    std::set<harkenstudio::ObjectId> active;
    std::string_view prior;
    for (const auto& row : catalog.rows) {
        if (row.variableId == harkenstudio::ObjectId{} || !IsSdkCallableName(row.name) ||
            (!prior.empty() && prior >= row.name)) return false;
        if (!active.insert(row.variableId).second) return false;
        prior = row.name;
    }
    return true;
}

inline bool EncodeVariableNameCatalog(PackVariableNameCatalog catalog,
                                      std::vector<std::uint8_t>* out) {
    if (out == nullptr) return false;
    std::sort(catalog.rows.begin(), catalog.rows.end(),
              [](const auto& a, const auto& b) { return a.name < b.name; });
    if (!ValidateVariableNameCatalog(catalog)) return false;
    std::size_t nameBytes = 0;
    for (const auto& row : catalog.rows) nameBytes += row.name.size();
    nameBytes = (nameBytes + 7) & ~std::size_t{7};
    const std::size_t poolOffset = kVariableNameCatalogHeaderBytes +
        catalog.rows.size() * kVariableNameCatalogRowBytes;
    if (poolOffset + nameBytes > kVariableNameCatalogMaxBytes) return false;
    std::vector<std::uint8_t> payload(poolOffset + nameBytes, 0);
    StoreLE32(payload.data(), 1);
    StoreLE32(payload.data() + 4, static_cast<std::uint32_t>(catalog.rows.size()));
    std::memcpy(payload.data() + 8, catalog.projectGuid.data(), 16);
    StoreLE64(payload.data() + 24, catalog.initPackStableId);
    StoreLE64(payload.data() + 32, catalog.initContentRevision);
    StoreLE32(payload.data() + 40, static_cast<std::uint32_t>(nameBytes));
    std::uint32_t cursor = 0;
    for (std::size_t i = 0; i < catalog.rows.size(); ++i) {
        const auto& row = catalog.rows[i];
        auto* bytes = payload.data() + kVariableNameCatalogHeaderBytes +
            i * kVariableNameCatalogRowBytes;
        StoreObjectId(bytes, row.variableId);
        StoreLE32(bytes + 16, cursor);
        StoreLE16(bytes + 20, static_cast<std::uint16_t>(row.name.size()));
        StoreLE64(bytes + 24, Fnv1a64(bytes, 24));
        std::memcpy(payload.data() + poolOffset + cursor, row.name.data(), row.name.size());
        cursor += static_cast<std::uint32_t>(row.name.size());
    }
    StoreLE64(payload.data() + 48, Fnv1a64(
        payload.data() + kVariableNameCatalogHeaderBytes,
        payload.size() - kVariableNameCatalogHeaderBytes));
    StoreLE64(payload.data() + 56, Fnv1a64(payload.data(), 56));
    *out = std::move(payload);
    return true;
}

inline bool DecodeVariableNameCatalog(const std::uint8_t* bytes, std::uint64_t size,
                                      PackVariableNameCatalog* out) {
    if (bytes == nullptr || out == nullptr || size < kVariableNameCatalogHeaderBytes ||
        size > kVariableNameCatalogMaxBytes || LoadLE32(bytes) != 1 ||
        LoadLE32(bytes + 44) != 0 || LoadLE64(bytes + 56) != Fnv1a64(bytes, 56))
        return false;
    const auto count = LoadLE32(bytes + 4);
    const auto nameBytes = LoadLE32(bytes + 40);
    const std::uint64_t poolOffset = kVariableNameCatalogHeaderBytes +
        static_cast<std::uint64_t>(count) * kVariableNameCatalogRowBytes;
    if (count > kVariableNameCatalogMaxRows || (nameBytes & 7) != 0 ||
        poolOffset + nameBytes != size ||
        LoadLE64(bytes + 48) != Fnv1a64(bytes + kVariableNameCatalogHeaderBytes,
                                       static_cast<std::size_t>(size - kVariableNameCatalogHeaderBytes)))
        return false;
    PackVariableNameCatalog catalog;
    std::memcpy(catalog.projectGuid.data(), bytes + 8, 16);
    catalog.initPackStableId = LoadLE64(bytes + 24);
    catalog.initContentRevision = LoadLE64(bytes + 32);
    catalog.rows.reserve(count);
    std::uint32_t cursor = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto* rowBytes = bytes + kVariableNameCatalogHeaderBytes +
            static_cast<std::size_t>(i) * kVariableNameCatalogRowBytes;
        const auto offset = LoadLE32(rowBytes + 16);
        const auto length = LoadLE16(rowBytes + 20);
        if (offset != cursor || offset > nameBytes || length > nameBytes - offset ||
            LoadLE16(rowBytes + 22) != 0 ||
            LoadLE64(rowBytes + 24) != Fnv1a64(rowBytes, 24)) return false;
        PackVariableNameRow row;
        row.variableId = LoadObjectId(rowBytes);
        row.name.assign(reinterpret_cast<const char*>(bytes + poolOffset + offset), length);
        catalog.rows.push_back(std::move(row));
        cursor += length;
    }
    if (((static_cast<std::uint64_t>(cursor) + 7) & ~std::uint64_t{7}) != nameBytes ||
        !ValidateVariableNameCatalog(catalog)) return false;
    for (; cursor < nameBytes; ++cursor)
        if (bytes[poolOffset + cursor] != 0) return false;
    *out = std::move(catalog);
    return true;
}

}
