#pragma once

#include "harken_studio_build/PackFormatV2.h"

namespace harkenstudiobuild {

inline constexpr std::uint32_t kPackGameplayCatalogVintage = 1;
inline constexpr std::uint32_t kGameplayCatalogHeaderBytes = 96;
inline constexpr std::uint32_t kGameplayCatalogActiveRowBytes = 72;
inline constexpr std::uint32_t kGameplayCatalogRetiredRowBytes = 40;
inline constexpr std::uint32_t kGameplayCatalogMaxEntries = 4096;
inline constexpr std::uint32_t kGameplayCatalogMaxBytes = 1048576;
inline constexpr std::uint32_t kGameplayCatalogMaxNameBytes = 128;
inline constexpr std::uint32_t kGameplayCatalogRetiredReasonRenamed = 1;
inline constexpr std::uint32_t kGameplayCatalogRetiredReasonDeleted = 2;
inline constexpr std::uint32_t kGameplayCatalogRetiredReasonReused = 3;
inline constexpr std::uint32_t kGameplayCatalogFlagHasRetired = 1u;
inline constexpr std::uint32_t kGameplayCatalogActiveFlagOptional = 1u;

inline bool GameplayCatalogBytes(std::uint32_t activeCount,
                                 std::uint32_t retiredCount,
                                 std::uint32_t namePoolBytes,
                                 std::uint64_t* outBytes) {
    std::uint64_t activeBytes = 0;
    std::uint64_t retiredBytes = 0;
    std::uint64_t afterHeader = 0;
    std::uint64_t afterActive = 0;
    if (!CheckedMulU64(activeCount, kGameplayCatalogActiveRowBytes,
                       &activeBytes) ||
        !CheckedMulU64(retiredCount, kGameplayCatalogRetiredRowBytes,
                       &retiredBytes) ||
        !CheckedAddU64(kGameplayCatalogHeaderBytes, activeBytes, &afterHeader) ||
        !CheckedAddU64(afterHeader, retiredBytes, &afterActive) ||
        !CheckedAddU64(afterActive, namePoolBytes, outBytes)) {
        return false;
    }
    return true;
}

struct PackGameplayCatalogHeader {
    std::uint32_t schemaVersion = 0;
    std::uint32_t layoutVintage = 0;
    std::array<std::uint8_t, 16> projectGuid{};
    std::uint64_t digest = 0;
    std::uint32_t activeCount = 0;
    std::uint32_t retiredCount = 0;
    std::uint32_t namePoolBytes = 0;
    std::uint32_t reserved0 = 0;
    std::uint64_t requiredCapabilityMask = 0;
    std::uint64_t publicationPairingFingerprint = 0;
    std::uint64_t initPackStableId = 0;
    std::uint64_t initContentRevision = 0;
    std::uint32_t flags = 0;
    std::uint32_t reserved1 = 0;
    std::uint64_t headerChecksum = 0;
};

struct PackGameplayCatalogActiveRow {
    harkenstudio::ObjectId actionStableId{};
    std::uint64_t nameBindingRevision = 0;
    std::uint64_t callableFingerprint = 0;
    std::uint64_t ownerPackStableId = 0;
    std::uint64_t providerClosureFingerprint = 0;
    std::uint32_t nameOffset = 0;
    std::uint16_t nameLength = 0;
    std::uint16_t flags = 0;
    std::uint64_t reserved0 = 0;
    std::uint64_t rowChecksum = 0;
};

struct PackGameplayCatalogRetiredRow {
    harkenstudio::ObjectId lastActionStableId{};
    std::uint64_t lastNameBindingRevision = 0;
    std::uint32_t reason = 0;
    std::uint16_t nameLength = 0;
    std::uint16_t reserved0 = 0;
    std::uint32_t nameOffset = 0;
    std::uint32_t reserved1 = 0;
};

struct PackGameplayCatalogActiveDraft {
    harkenstudio::ObjectId actionStableId{};
    std::uint64_t nameBindingRevision = 0;
    std::uint64_t callableFingerprint = 0;
    std::uint64_t ownerPackStableId = 0;
    std::uint64_t providerClosureFingerprint = 0;
    std::uint16_t flags = 0;
    std::string_view name;
};

struct PackGameplayCatalogRetiredDraft {
    harkenstudio::ObjectId lastActionStableId{};
    std::uint64_t lastNameBindingRevision = 0;
    std::uint32_t reason = 0;
    std::string_view name;
};

inline std::uint64_t GameplayCallableFingerprint(
    harkenstudio::ObjectId actionId, std::uint64_t nameRevision, std::string_view name) {
    std::array<std::uint8_t, 26> prefix{};
    StoreObjectId(prefix.data(), actionId);
    StoreLE64(prefix.data() + 16, nameRevision);
    StoreLE16(prefix.data() + 24, static_cast<std::uint16_t>(name.size()));
    auto hash = Fnv1a64(prefix.data(), prefix.size());
    hash = Fnv1a64(name.data(), name.size(), hash);
    std::array<std::uint8_t, 4> suffix{};
    StoreLE32(suffix.data(), kInitCatalogIdentityClassAction);
    return Fnv1a64(suffix.data(), suffix.size(), hash);
}

inline int CompareGameplayCatalogNameBytes(const void* a, std::uint16_t aLen,
                                           const void* b, std::uint16_t bLen) {
    const std::size_t n = aLen < bLen ? aLen : bLen;
    const int c = std::memcmp(a, b, n);
    if (c != 0) {
        return c;
    }
    return aLen < bLen ? -1 : (aLen > bLen ? 1 : 0);
}

inline std::uint64_t GameplayCatalogDigest(const std::uint8_t* payload,
                                           std::size_t size) {
    if (payload == nullptr || size < kGameplayCatalogHeaderBytes) {
        return 0;
    }
    const std::uint8_t zeros[8] = {};
    std::uint64_t hash = Fnv1a64(payload, 24);
    hash = Fnv1a64(zeros, 8, hash);
    hash = Fnv1a64(payload + 32, 24, hash);
    hash = Fnv1a64(zeros, 8, hash);
    hash = Fnv1a64(payload + 64, 24, hash);
    hash = Fnv1a64(zeros, 8, hash);
    if (size > kGameplayCatalogHeaderBytes) {
        hash = Fnv1a64(payload + kGameplayCatalogHeaderBytes,
                       size - kGameplayCatalogHeaderBytes, hash);
    }
    return hash;
}

inline std::uint64_t GameplayCatalogPublicationPairing(
    const PackGameplayCatalogHeader& header) {
    std::uint8_t bytes[40] = {};
    std::memcpy(bytes, header.projectGuid.data(), 16);
    StoreLE64(bytes + 16, header.initPackStableId);
    StoreLE64(bytes + 24, header.initContentRevision);
    StoreLE64(bytes + 32, header.digest);
    return Fnv1a64(bytes, sizeof bytes);
}

inline void SerializePackGameplayCatalogHeader(
    const PackGameplayCatalogHeader& header, std::uint8_t* out) {
    StoreLE32(out + 0, header.schemaVersion);
    StoreLE32(out + 4, header.layoutVintage);
    std::memcpy(out + 8, header.projectGuid.data(), 16);
    StoreLE64(out + 24, header.digest);
    StoreLE32(out + 32, header.activeCount);
    StoreLE32(out + 36, header.retiredCount);
    StoreLE32(out + 40, header.namePoolBytes);
    StoreLE32(out + 44, header.reserved0);
    StoreLE64(out + 48, header.requiredCapabilityMask);
    StoreLE64(out + 56, header.publicationPairingFingerprint);
    StoreLE64(out + 64, header.initPackStableId);
    StoreLE64(out + 72, header.initContentRevision);
    StoreLE32(out + 80, header.flags);
    StoreLE32(out + 84, header.reserved1);
    StoreLE64(out + 88, header.headerChecksum);
}

inline void LoadPackGameplayCatalogHeader(const std::uint8_t* in,
                                          PackGameplayCatalogHeader* out) {
    out->schemaVersion = LoadLE32(in + 0);
    out->layoutVintage = LoadLE32(in + 4);
    std::memcpy(out->projectGuid.data(), in + 8, 16);
    out->digest = LoadLE64(in + 24);
    out->activeCount = LoadLE32(in + 32);
    out->retiredCount = LoadLE32(in + 36);
    out->namePoolBytes = LoadLE32(in + 40);
    out->reserved0 = LoadLE32(in + 44);
    out->requiredCapabilityMask = LoadLE64(in + 48);
    out->publicationPairingFingerprint = LoadLE64(in + 56);
    out->initPackStableId = LoadLE64(in + 64);
    out->initContentRevision = LoadLE64(in + 72);
    out->flags = LoadLE32(in + 80);
    out->reserved1 = LoadLE32(in + 84);
    out->headerChecksum = LoadLE64(in + 88);
}

inline void SerializePackGameplayCatalogActiveRow(
    const PackGameplayCatalogActiveRow& row, std::uint8_t* out) {
    StoreObjectId(out + 0, row.actionStableId);
    StoreLE64(out + 16, row.nameBindingRevision);
    StoreLE64(out + 24, row.callableFingerprint);
    StoreLE64(out + 32, row.ownerPackStableId);
    StoreLE64(out + 40, row.providerClosureFingerprint);
    StoreLE32(out + 48, row.nameOffset);
    StoreLE16(out + 52, row.nameLength);
    StoreLE16(out + 54, row.flags);
    StoreLE64(out + 56, row.reserved0);
    StoreLE64(out + 64, row.rowChecksum);
}

inline bool ParsePackGameplayCatalogActiveRow(
    const std::uint8_t* in, PackGameplayCatalogActiveRow* out) {
    out->actionStableId = LoadObjectId(in + 0);
    out->nameBindingRevision = LoadLE64(in + 16);
    out->callableFingerprint = LoadLE64(in + 24);
    out->ownerPackStableId = LoadLE64(in + 32);
    out->providerClosureFingerprint = LoadLE64(in + 40);
    out->nameOffset = LoadLE32(in + 48);
    out->nameLength = LoadLE16(in + 52);
    out->flags = LoadLE16(in + 54);
    out->reserved0 = LoadLE64(in + 56);
    out->rowChecksum = LoadLE64(in + 64);
    if (out->actionStableId == harkenstudio::ObjectId{} || out->ownerPackStableId == 0 ||
        out->nameBindingRevision < 1 || out->nameLength < 1 ||
        out->nameLength > kGameplayCatalogMaxNameBytes ||
        (out->flags & ~kGameplayCatalogActiveFlagOptional) != 0 ||
        out->reserved0 != 0) {
        return false;
    }
    return Fnv1a64(in, kGameplayCatalogActiveRowBytes - 8) == out->rowChecksum;
}

inline void SerializePackGameplayCatalogRetiredRow(
    const PackGameplayCatalogRetiredRow& row, std::uint8_t* out) {
    StoreObjectId(out + 0, row.lastActionStableId);
    StoreLE64(out + 16, row.lastNameBindingRevision);
    StoreLE32(out + 24, row.reason);
    StoreLE16(out + 28, row.nameLength);
    StoreLE16(out + 30, row.reserved0);
    StoreLE32(out + 32, row.nameOffset);
    StoreLE32(out + 36, row.reserved1);
}

inline bool ParsePackGameplayCatalogRetiredRow(
    const std::uint8_t* in, PackGameplayCatalogRetiredRow* out) {
    out->lastActionStableId = LoadObjectId(in + 0);
    out->lastNameBindingRevision = LoadLE64(in + 16);
    out->reason = LoadLE32(in + 24);
    out->nameLength = LoadLE16(in + 28);
    out->reserved0 = LoadLE16(in + 30);
    out->nameOffset = LoadLE32(in + 32);
    out->reserved1 = LoadLE32(in + 36);
    return out->lastActionStableId != harkenstudio::ObjectId{} && out->nameLength >= 1 &&
           out->nameLength <= kGameplayCatalogMaxNameBytes &&
           (out->reason == kGameplayCatalogRetiredReasonRenamed ||
            out->reason == kGameplayCatalogRetiredReasonDeleted ||
            out->reason == kGameplayCatalogRetiredReasonReused) &&
           out->reserved0 == 0 && out->reserved1 == 0;
}

inline bool EncodeGameplayCatalog(
    const PackGameplayCatalogHeader& identity,
    std::vector<PackGameplayCatalogActiveDraft> active,
    std::vector<PackGameplayCatalogRetiredDraft> retired,
    std::vector<std::uint8_t>* out) {
    if (out == nullptr ||
        active.size() > kGameplayCatalogMaxEntries ||
        retired.size() > kGameplayCatalogMaxEntries) {
        return false;
    }
    for (const auto& row : active) {
        if (row.name.empty() || row.name.size() > kGameplayCatalogMaxNameBytes ||
            row.actionStableId == harkenstudio::ObjectId{} || row.ownerPackStableId == 0 ||
            row.nameBindingRevision < 1 ||
            (row.flags & ~kGameplayCatalogActiveFlagOptional) != 0) {
            return false;
        }
    }
    for (const auto& row : retired) {
        if (row.name.empty() || row.name.size() > kGameplayCatalogMaxNameBytes ||
            row.lastActionStableId == harkenstudio::ObjectId{} ||
            (row.reason != kGameplayCatalogRetiredReasonRenamed &&
             row.reason != kGameplayCatalogRetiredReasonDeleted &&
             row.reason != kGameplayCatalogRetiredReasonReused)) {
            return false;
        }
    }
    std::sort(active.begin(), active.end(),
              [](const PackGameplayCatalogActiveDraft& a,
                 const PackGameplayCatalogActiveDraft& b) {
                  const int c = CompareGameplayCatalogNameBytes(
                      a.name.data(),
                      static_cast<std::uint16_t>(a.name.size()),
                      b.name.data(),
                      static_cast<std::uint16_t>(b.name.size()));
                  if (c != 0) {
                      return c < 0;
                  }
                  return a.actionStableId < b.actionStableId;
              });
    std::sort(retired.begin(), retired.end(),
              [](const PackGameplayCatalogRetiredDraft& a,
                 const PackGameplayCatalogRetiredDraft& b) {
                  const int c = CompareGameplayCatalogNameBytes(
                      a.name.data(),
                      static_cast<std::uint16_t>(a.name.size()),
                      b.name.data(),
                      static_cast<std::uint16_t>(b.name.size()));
                  if (c != 0) {
                      return c < 0;
                  }
                  return a.lastActionStableId < b.lastActionStableId;
              });

    std::uint32_t rawNameBytes = 0;
    for (const auto& row : active) {
        rawNameBytes += static_cast<std::uint32_t>(row.name.size());
    }
    for (const auto& row : retired) {
        rawNameBytes += static_cast<std::uint32_t>(row.name.size());
    }
    const std::uint32_t namePoolBytes = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(rawNameBytes) + 7u) & ~7ull);
    std::uint64_t payloadBytes = 0;
    if (!GameplayCatalogBytes(static_cast<std::uint32_t>(active.size()),
                              static_cast<std::uint32_t>(retired.size()),
                              namePoolBytes, &payloadBytes) ||
        payloadBytes > kGameplayCatalogMaxBytes) {
        return false;
    }

    std::vector<std::uint8_t> payload(static_cast<std::size_t>(payloadBytes), 0);
    PackGameplayCatalogHeader header = identity;
    header.schemaVersion = 1;
    header.layoutVintage = kPackGameplayCatalogVintage;
    header.digest = 0;
    header.activeCount = static_cast<std::uint32_t>(active.size());
    header.retiredCount = static_cast<std::uint32_t>(retired.size());
    header.namePoolBytes = namePoolBytes;
    header.reserved0 = 0;
    header.publicationPairingFingerprint = 0;
    header.flags =
        header.retiredCount > 0 ? kGameplayCatalogFlagHasRetired : 0;
    header.reserved1 = 0;
    header.headerChecksum = 0;
    SerializePackGameplayCatalogHeader(header, payload.data());

    std::uint8_t* pool =
        payload.data() + kGameplayCatalogHeaderBytes +
        static_cast<std::size_t>(header.activeCount) *
            kGameplayCatalogActiveRowBytes +
        static_cast<std::size_t>(header.retiredCount) *
            kGameplayCatalogRetiredRowBytes;
    std::uint32_t nameOffset = 0;
    for (std::uint32_t i = 0; i < header.activeCount; ++i) {
        PackGameplayCatalogActiveRow row;
        row.actionStableId = active[i].actionStableId;
        row.nameBindingRevision = active[i].nameBindingRevision;
        row.callableFingerprint = active[i].callableFingerprint;
        row.ownerPackStableId = active[i].ownerPackStableId;
        row.providerClosureFingerprint = active[i].providerClosureFingerprint;
        row.nameOffset = nameOffset;
        row.nameLength = static_cast<std::uint16_t>(active[i].name.size());
        row.flags = active[i].flags;
        std::uint8_t* rowBytes =
            payload.data() + kGameplayCatalogHeaderBytes +
            static_cast<std::size_t>(i) * kGameplayCatalogActiveRowBytes;
        SerializePackGameplayCatalogActiveRow(row, rowBytes);
        row.rowChecksum = Fnv1a64(rowBytes, kGameplayCatalogActiveRowBytes - 8);
        StoreLE64(rowBytes + kGameplayCatalogActiveRowBytes - 8, row.rowChecksum);
        std::memcpy(pool + nameOffset, active[i].name.data(), row.nameLength);
        nameOffset += row.nameLength;
    }
    for (std::uint32_t i = 0; i < header.retiredCount; ++i) {
        PackGameplayCatalogRetiredRow row;
        row.lastActionStableId = retired[i].lastActionStableId;
        row.lastNameBindingRevision = retired[i].lastNameBindingRevision;
        row.reason = retired[i].reason;
        row.nameLength = static_cast<std::uint16_t>(retired[i].name.size());
        row.nameOffset = nameOffset;
        std::uint8_t* rowBytes =
            payload.data() + kGameplayCatalogHeaderBytes +
            static_cast<std::size_t>(header.activeCount) *
                kGameplayCatalogActiveRowBytes +
            static_cast<std::size_t>(i) * kGameplayCatalogRetiredRowBytes;
        SerializePackGameplayCatalogRetiredRow(row, rowBytes);
        std::memcpy(pool + nameOffset, retired[i].name.data(), row.nameLength);
        nameOffset += row.nameLength;
    }

    header.digest = GameplayCatalogDigest(payload.data(), payload.size());
    StoreLE64(payload.data() + 24, header.digest);
    header.publicationPairingFingerprint =
        GameplayCatalogPublicationPairing(header);
    StoreLE64(payload.data() + 56, header.publicationPairingFingerprint);
    header.headerChecksum = Fnv1a64(payload.data(), 88);
    StoreLE64(payload.data() + 88, header.headerChecksum);
    *out = std::move(payload);
    return true;
}

struct GameplayCatalogImage {
    PackGameplayCatalogHeader header{};
    std::vector<PackGameplayCatalogActiveRow> active;
    std::vector<PackGameplayCatalogRetiredRow> retired;
    std::vector<std::uint8_t> namePool;
};

inline harkenstudiobuild::PackResult ParseGameplayCatalog(const std::uint8_t* payload,
                                     std::uint64_t size,
                                     GameplayCatalogImage* out) {
    if (size > kGameplayCatalogMaxBytes) {
        return harkenstudiobuild::kPackCatalogCapacity;
    }
    if (payload == nullptr || out == nullptr ||
        size < kGameplayCatalogHeaderBytes) {
        return harkenstudiobuild::kPackInvalid;
    }
    PackGameplayCatalogHeader header;
    LoadPackGameplayCatalogHeader(payload, &header);
    if (header.activeCount > kGameplayCatalogMaxEntries ||
        header.retiredCount > kGameplayCatalogMaxEntries) {
        return harkenstudiobuild::kPackCatalogCapacity;
    }
    std::uint64_t expectedBytes = 0;
    if (!GameplayCatalogBytes(header.activeCount, header.retiredCount,
                              header.namePoolBytes, &expectedBytes) ||
        expectedBytes > kGameplayCatalogMaxBytes) {
        return harkenstudiobuild::kPackCatalogCapacity;
    }
    if (expectedBytes != size) {
        return harkenstudiobuild::kPackInvalid;
    }
    if (header.schemaVersion != 1 ||
        header.layoutVintage != kPackGameplayCatalogVintage) {
        return harkenstudiobuild::kPackIncompatible;
    }
    if (header.reserved0 != 0 || header.reserved1 != 0 ||
        (header.flags & ~kGameplayCatalogFlagHasRetired) != 0 ||
        ((header.flags & kGameplayCatalogFlagHasRetired) != 0) !=
            (header.retiredCount > 0) ||
        (header.namePoolBytes & 7u) != 0 ||
        Fnv1a64(payload, 88) != header.headerChecksum ||
        GameplayCatalogDigest(payload, static_cast<std::size_t>(size)) !=
            header.digest ||
        GameplayCatalogPublicationPairing(header) !=
            header.publicationPairingFingerprint) {
        return harkenstudiobuild::kPackInvalid;
    }

    const std::uint8_t* activeBase = payload + kGameplayCatalogHeaderBytes;
    const std::uint8_t* retiredBase =
        activeBase + static_cast<std::size_t>(header.activeCount) *
                         kGameplayCatalogActiveRowBytes;
    const std::uint8_t* pool =
        retiredBase + static_cast<std::size_t>(header.retiredCount) *
                          kGameplayCatalogRetiredRowBytes;

    GameplayCatalogImage image;
    image.header = header;
    image.active.reserve(header.activeCount);
    image.retired.reserve(header.retiredCount);
    image.namePool.assign(pool, pool + header.namePoolBytes);

    std::uint32_t cursor = 0;
    for (std::uint32_t i = 0; i < header.activeCount; ++i) {
        PackGameplayCatalogActiveRow row;
        if (!ParsePackGameplayCatalogActiveRow(
                activeBase + static_cast<std::size_t>(i) *
                                 kGameplayCatalogActiveRowBytes,
                &row) ||
            row.nameOffset != cursor ||
            row.nameOffset > header.namePoolBytes ||
            row.nameLength > header.namePoolBytes - row.nameOffset) {
            return harkenstudiobuild::kPackInvalid;
        }
        for (std::uint32_t j = 0; j < i; ++j) {
            if (image.active[j].actionStableId == row.actionStableId) {
                return harkenstudiobuild::kPackInvalid;
            }
            const int nameCmp = CompareGameplayCatalogNameBytes(
                image.namePool.data() + image.active[j].nameOffset,
                image.active[j].nameLength,
                image.namePool.data() + row.nameOffset, row.nameLength);
            if (nameCmp == 0) {
                return harkenstudiobuild::kPackInvalid;
            }
        }
        if (i > 0) {
            const PackGameplayCatalogActiveRow& prev = image.active.back();
            const int nameCmp = CompareGameplayCatalogNameBytes(
                image.namePool.data() + prev.nameOffset, prev.nameLength,
                image.namePool.data() + row.nameOffset, row.nameLength);
            if (nameCmp > 0 ||
                (nameCmp == 0 && prev.actionStableId >= row.actionStableId)) {
                return harkenstudiobuild::kPackInvalid;
            }
        }
        cursor += row.nameLength;
        image.active.push_back(row);
    }
    for (std::uint32_t i = 0; i < header.retiredCount; ++i) {
        PackGameplayCatalogRetiredRow row;
        if (!ParsePackGameplayCatalogRetiredRow(
                retiredBase + static_cast<std::size_t>(i) *
                                  kGameplayCatalogRetiredRowBytes,
                &row) ||
            row.nameOffset != cursor ||
            row.nameOffset > header.namePoolBytes ||
            row.nameLength > header.namePoolBytes - row.nameOffset) {
            return harkenstudiobuild::kPackInvalid;
        }
        for (const auto& active : image.active) {
            if (CompareGameplayCatalogNameBytes(
                    image.namePool.data() + active.nameOffset, active.nameLength,
                    image.namePool.data() + row.nameOffset, row.nameLength) ==
                0) {
                return harkenstudiobuild::kPackInvalid;
            }
        }
        if (i > 0) {
            const PackGameplayCatalogRetiredRow& prev = image.retired.back();
            const int nameCmp = CompareGameplayCatalogNameBytes(
                image.namePool.data() + prev.nameOffset, prev.nameLength,
                image.namePool.data() + row.nameOffset, row.nameLength);
            if (nameCmp > 0 ||
                (nameCmp == 0 &&
                 prev.lastActionStableId >= row.lastActionStableId)) {
                return harkenstudiobuild::kPackInvalid;
            }
        }
        cursor += row.nameLength;
        image.retired.push_back(row);
    }
    const std::uint32_t padded = (cursor + 7u) & ~7u;
    if (padded != header.namePoolBytes) {
        return harkenstudiobuild::kPackInvalid;
    }
    for (std::uint32_t i = cursor; i < header.namePoolBytes; ++i) {
        if (image.namePool[i] != 0) {
            return harkenstudiobuild::kPackInvalid;
        }
    }
    *out = std::move(image);
    return harkenstudiobuild::kPackOk;
}

}
