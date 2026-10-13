#include "harken_project_schema/VariableContract.h"

#include "harken_studio_build/VariableExecutionFingerprint.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace harkenprojectschema {
namespace {

constexpr char kMagic[4] = {'V', 'N', 'V', 'C'};
constexpr std::size_t kMinimumVariableBytesV1 =
    kVariableExecutionRecordBytesV1 + 4u * sizeof(std::uint32_t) +
    sizeof(std::int32_t) + 2u * sizeof(std::uint8_t) +
    sizeof(std::uint16_t) + sizeof(harkenstudio::ObjectId) + sizeof(std::uint32_t) +
    sizeof(std::uint64_t);
constexpr std::size_t kMinimumLabelBytesV1 =
    sizeof(std::int32_t) + sizeof(std::uint32_t) + sizeof(harkenstudio::ObjectId);

void WriteU8(std::vector<std::uint8_t>& out, std::uint8_t value) {
    out.push_back(value);
}

void WriteU16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void WriteU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 24));
}

void WriteU64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    WriteU32(out, static_cast<std::uint32_t>(value));
    WriteU32(out, static_cast<std::uint32_t>(value >> 32));
}

void WriteI32(std::vector<std::uint8_t>& out, std::int32_t value) {
    WriteU32(out, static_cast<std::uint32_t>(value));
}

void WriteObjectId(std::vector<std::uint8_t>& out, harkenstudio::ObjectId value) {
    const auto bytes = harkenstudio::ObjectIdBytes(value);
    out.insert(out.end(), bytes.begin(), bytes.end());
}

bool WritePaddedBytes(std::vector<std::uint8_t>& out, const std::string& value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    const auto n = static_cast<std::uint32_t>(value.size());
    WriteU32(out, n);
    out.insert(out.end(), value.begin(), value.end());
    const std::uint32_t pad = (4u - (n % 4u)) % 4u;
    out.insert(out.end(), pad, 0);
    return true;
}

struct Cursor {
    const std::uint8_t* p = nullptr;
    const std::uint8_t* end = nullptr;
    bool ok = true;

    std::size_t Remaining() const noexcept {
        return ok && end >= p ? static_cast<std::size_t>(end - p) : 0;
    }
};

std::uint8_t ReadU8(Cursor& c) {
    if (!c.ok || c.Remaining() < 1) {
        c.ok = false;
        return 0;
    }
    const std::uint8_t value = *c.p;
    ++c.p;
    return value;
}

std::uint16_t ReadU16(Cursor& c) {
    if (!c.ok || c.Remaining() < 2) {
        c.ok = false;
        return 0;
    }
    const std::uint16_t value = static_cast<std::uint16_t>(c.p[0]) |
                                static_cast<std::uint16_t>(c.p[1] << 8);
    c.p += 2;
    return value;
}

std::uint32_t ReadU32(Cursor& c) {
    if (!c.ok || c.Remaining() < 4) {
        c.ok = false;
        return 0;
    }
    const std::uint32_t value = static_cast<std::uint32_t>(c.p[0]) |
                                (static_cast<std::uint32_t>(c.p[1]) << 8) |
                                (static_cast<std::uint32_t>(c.p[2]) << 16) |
                                (static_cast<std::uint32_t>(c.p[3]) << 24);
    c.p += 4;
    return value;
}

std::uint64_t ReadU64(Cursor& c) {
    const std::uint32_t lo = ReadU32(c);
    const std::uint32_t hi = ReadU32(c);
    return static_cast<std::uint64_t>(lo) |
           (static_cast<std::uint64_t>(hi) << 32);
}

std::int32_t ReadI32(Cursor& c) {
    return static_cast<std::int32_t>(ReadU32(c));
}

harkenstudio::ObjectId ReadObjectId(Cursor& c) {
    if (c.Remaining() < 16) {
        c.ok = false;
        return {};
    }
    const auto value = harkenstudio::ObjectIdFromBytes(std::span<const std::uint8_t, 16>(c.p, 16));
    c.p += 16;
    return value;
}

bool ReadPaddedBytes(Cursor& c, std::string* out) {
    const std::uint32_t n = ReadU32(c);
    if (!c.ok || c.Remaining() < n) {
        c.ok = false;
        return false;
    }
    out->assign(reinterpret_cast<const char*>(c.p), n);
    c.p += n;
    const std::uint32_t pad = (4u - (n % 4u)) % 4u;
    if (c.Remaining() < pad) {
        c.ok = false;
        return false;
    }
    for (std::uint32_t i = 0; i < pad; ++i) {
        if (c.p[i] != 0) {
            c.ok = false;
            return false;
        }
    }
    c.p += pad;
    return true;
}

void AddLenBytes(harkenstudiobuild::MediaContentRevisionHasherV1& hash,
                 const std::string& value) {
    hash.AddU32(static_cast<std::uint32_t>(value.size()));
    if (!value.empty()) {
        hash.AddBytes(value.data(), value.size());
    }
}

void AddU8(harkenstudiobuild::MediaContentRevisionHasherV1& hash, std::uint8_t value) {
    hash.AddBytes(&value, sizeof(value));
}

void AddU16(harkenstudiobuild::MediaContentRevisionHasherV1& hash, std::uint16_t value) {
    const std::uint8_t bytes[2] = {static_cast<std::uint8_t>(value),
                                   static_cast<std::uint8_t>(value >> 8)};
    hash.AddBytes(bytes, sizeof(bytes));
}

bool CollectAnchors(const VariableCatalogBlockV1& catalog,
                    std::vector<std::int32_t>* anchors) {
    anchors->clear();
    anchors->reserve(catalog.labels.size());
    for (const auto& label : catalog.labels) {
        anchors->push_back(label.anchor);
    }
    return true;
}

void ComputeArtifactFingerprints(
    const std::vector<VariableContractRecordV1>& variables,
    std::uint64_t* executionCatalogFingerprint,
    std::uint64_t* sdkCatalogFingerprint) {
    std::vector<harkenstudiobuild::VariableCatalogFingerprintRowV1> executionRows;
    std::vector<std::uint64_t> catalogRows;
    executionRows.reserve(variables.size());
    catalogRows.reserve(variables.size());
    for (const auto& record : variables) {
        executionRows.push_back({record.execution.variableId,
                                 record.execution.executionFingerprint});
        catalogRows.push_back(record.catalogFingerprint);
    }
    *executionCatalogFingerprint =
        harkenstudiobuild::ComputeVariableExecutionCatalogFingerprintV1(
            executionRows.empty() ? nullptr : executionRows.data(),
            static_cast<std::uint32_t>(executionRows.size()));
    *sdkCatalogFingerprint = harkenstudiobuild::ComputeVariableSdkCatalogFingerprintV1(
        catalogRows.empty() ? nullptr : catalogRows.data(),
        static_cast<std::uint32_t>(catalogRows.size()));
}

bool Canonicalize(VariableContractArtifactV1 artifact,
                  VariableContractArtifactV1* out) {
    if (out == nullptr) {
        return false;
    }
    if (artifact.schemaVersion != kVariableContractSchemaVersionV1) {
        return false;
    }
    std::sort(artifact.variables.begin(), artifact.variables.end(),
              [](const VariableContractRecordV1& a,
                 const VariableContractRecordV1& b) {
                  return a.execution.variableId < b.execution.variableId;
              });
    for (std::size_t i = 0; i < artifact.variables.size(); ++i) {
        auto& record = artifact.variables[i];
        if (i != 0 && record.execution.variableId ==
                          artifact.variables[i - 1].execution.variableId) {
            return false;
        }
        std::sort(record.catalog.labels.begin(), record.catalog.labels.end(),
                  [](const VariableCatalogLabelV1& a,
                     const VariableCatalogLabelV1& b) {
                      return a.anchor < b.anchor;
                  });
        std::vector<std::int32_t> anchors;
        if (record.execution.kind == ControlValueKindV1::Discrete)
            CollectAnchors(record.catalog, &anchors);
        std::set<harkenstudio::ObjectId> ids;
        std::set<std::string> names;
        std::int32_t previousAnchor = -1;
        for (const auto& label : record.catalog.labels) {
            if (label.anchor < 0 || label.anchor > 16777216
                || label.anchor <= previousAnchor
                || label.anchor < record.execution.range.minimum
                || label.anchor > record.execution.range.maximum
                || harkenstudio::IsNull(label.labelId) || !ids.insert(label.labelId).second
                || label.name.find('\0') != std::string::npos
                || (!label.name.empty() && !names.insert(label.name).second)) return false;
            previousAnchor = label.anchor;
        }
        if (record.execution.range.minimum < 0.0f) return false;
        const std::int32_t* pointer =
            anchors.empty() ? nullptr : anchors.data();
        if (!harkenstudiobuild::ComputeVariableExecutionFingerprintV1(
                record.execution.variableId,
                static_cast<std::uint8_t>(record.execution.kind),
                static_cast<std::uint8_t>(record.execution.scopePolicy),
                static_cast<std::uint8_t>(record.execution.writers),
                record.execution.range.minimum, record.execution.range.maximum,
                record.execution.range.defaultValue, pointer,
                static_cast<std::uint32_t>(anchors.size()),
                &record.execution.executionFingerprint)) {
            return false;
        }
        record.catalogFingerprint = ComputeVariableCatalogFingerprintV1(
            record.execution.variableId, record.catalog);
    }

    ComputeArtifactFingerprints(artifact.variables,
                                &artifact.executionCatalogFingerprint,
                                &artifact.sdkCatalogFingerprint);
    *out = std::move(artifact);
    return true;
}

void AppendJsonRaw(std::string* out, const char* text) { *out += text; }

void AppendJsonU64(std::string* out, std::uint64_t value) {
    *out += std::to_string(value);
}

void AppendJsonI32(std::string* out, std::int32_t value) {
    *out += std::to_string(value);
}

void AppendJsonString(std::string* out, const std::string& value) {
    out->push_back('"');
    for (unsigned char c : value) {
        switch (c) {
        case '"':
            *out += "\\\"";
            break;
        case '\\':
            *out += "\\\\";
            break;
        case '\b':
            *out += "\\b";
            break;
        case '\f':
            *out += "\\f";
            break;
        case '\n':
            *out += "\\n";
            break;
        case '\r':
            *out += "\\r";
            break;
        case '\t':
            *out += "\\t";
            break;
        default:
            if (c < 0x20) {
                const char hex[] = "0123456789abcdef";
                *out += "\\u00";
                out->push_back(hex[c >> 4]);
                out->push_back(hex[c & 0x0f]);
            } else {
                out->push_back(static_cast<char>(c));
            }
            break;
        }
    }
    out->push_back('"');
}

bool EncodeBinary(const VariableContractArtifactV1& artifact,
                  std::vector<std::uint8_t>* outBytes) {
    if (artifact.variables.size() >
        std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    std::vector<std::uint8_t> bytes;
    bytes.insert(bytes.end(), kMagic, kMagic + 4);
    WriteU16(bytes, kVariableContractSchemaVersionV1);
    WriteU16(bytes, 0);
    WriteU64(bytes, artifact.projectBuildId);
    WriteU64(bytes, artifact.executionCatalogFingerprint);
    WriteU64(bytes, artifact.sdkCatalogFingerprint);
    WriteU32(bytes, static_cast<std::uint32_t>(artifact.variables.size()));
    WriteU32(bytes, 0);
    for (const auto& record : artifact.variables) {
        std::uint32_t minBits = 0;
        std::uint32_t maxBits = 0;
        std::uint32_t defaultBits = 0;
        if (!harkenstudiobuild::CanonicalF32Bits(record.execution.range.minimum, &minBits) ||
            !harkenstudiobuild::CanonicalF32Bits(record.execution.range.maximum, &maxBits) ||
            !harkenstudiobuild::CanonicalF32Bits(record.execution.range.defaultValue,
                                     &defaultBits)) {
            return false;
        }
        const std::size_t recordBegin = bytes.size();
        WriteObjectId(bytes, record.execution.variableId);
        WriteU8(bytes, static_cast<std::uint8_t>(record.execution.kind));
        WriteU8(bytes, static_cast<std::uint8_t>(record.execution.scopePolicy));
        WriteU8(bytes, static_cast<std::uint8_t>(record.execution.writers));
        WriteU8(bytes, 0);
        WriteU32(bytes, minBits);
        WriteU32(bytes, maxBits);
        WriteU32(bytes, defaultBits);
        WriteU64(bytes, record.execution.executionFingerprint);
        if (bytes.size() - recordBegin != kVariableExecutionRecordBytesV1) {
            return false;
        }
        if (!WritePaddedBytes(bytes, record.catalog.codeName) ||
            !WritePaddedBytes(bytes, record.catalog.displayName) ||
            !WritePaddedBytes(bytes, record.catalog.description) ||
            !WritePaddedBytes(bytes, record.catalog.unit)) {
            return false;
        }
        WriteI32(bytes, record.catalog.precision);
        WriteU8(bytes, record.catalog.displayScale);
        WriteU8(bytes, record.catalog.deprecated01);
        WriteU16(bytes, 0);
        WriteObjectId(bytes, record.catalog.replacementId);
        if (record.catalog.labels.size() >
            std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        WriteU32(bytes, static_cast<std::uint32_t>(record.catalog.labels.size()));
        for (const auto& label : record.catalog.labels) {
            WriteI32(bytes, label.anchor);
            WriteObjectId(bytes, label.labelId);
            if (!WritePaddedBytes(bytes, label.name)) {
                return false;
            }
        }
        WriteU64(bytes, record.catalog.ownerInitPackId);
    }
    WriteU32(bytes, artifact.capacity.definitionCount);
    WriteU32(bytes, artifact.capacity.maxReferencedPerContentPack);
    WriteU32(bytes, artifact.capacity.maxVariableBindings);
    WriteU32(bytes, artifact.capacity.maxStaticEmitterOverrides);
    WriteU32(bytes, artifact.capacity.recommendedMaxEmitters);
    WriteU32(bytes, artifact.capacity.recommendedEmitterVariableSlots);
    WriteU32(bytes, artifact.capacity.recommendedPendingRows);
    WriteU32(bytes, artifact.capacity.reserved);
    *outBytes = std::move(bytes);
    return true;
}

}  // namespace

std::uint64_t ComputeVariableCatalogFingerprintV1(
    harkenstudio::ObjectId variableId, const VariableCatalogBlockV1& catalog) {
    harkenstudiobuild::MediaContentRevisionHasherV1 hash(
        harkenstudiobuild::kVariableCatalogDomain, sizeof(harkenstudiobuild::kVariableCatalogDomain));
    hash.AddObjectId(variableId);
    AddLenBytes(hash, catalog.codeName);
    AddLenBytes(hash, catalog.displayName);
    AddLenBytes(hash, catalog.description);
    AddLenBytes(hash, catalog.unit);
    hash.AddU32(static_cast<std::uint32_t>(catalog.precision));
    AddU8(hash, catalog.displayScale);
    AddU8(hash, catalog.deprecated01);
    AddU16(hash, 0);
    hash.AddObjectId(catalog.replacementId);
    hash.AddU32(static_cast<std::uint32_t>(catalog.labels.size()));
    for (const auto& label : catalog.labels) {
        hash.AddU32(static_cast<std::uint32_t>(label.anchor));
        hash.AddObjectId(label.labelId);
        AddLenBytes(hash, label.name);
    }
    hash.AddU64(catalog.ownerInitPackId);
    return hash.Finish();
}

bool EncodeVariableContractArtifactV1(
    const VariableContractArtifactV1& artifact,
    std::vector<std::uint8_t>* outBytes) {
    if (outBytes == nullptr) {
        return false;
    }
    VariableContractArtifactV1 canonical;
    if (!Canonicalize(artifact, &canonical)) {
        return false;
    }
    return EncodeBinary(canonical, outBytes);
}

bool DecodeVariableContractArtifactV1(const std::uint8_t* bytes,
                                      std::size_t byteCount,
                                      VariableContractArtifactV1* outArtifact) {
    if (bytes == nullptr || outArtifact == nullptr || byteCount < 40) {
        return false;
    }
    Cursor c{bytes, bytes + byteCount, true};
    if (std::memcmp(c.p, kMagic, 4) != 0) {
        return false;
    }
    c.p += 4;
    VariableContractArtifactV1 parsed;
    parsed.schemaVersion = ReadU16(c);
    const std::uint16_t reservedFlags = ReadU16(c);
    parsed.projectBuildId = ReadU64(c);
    parsed.executionCatalogFingerprint = ReadU64(c);
    parsed.sdkCatalogFingerprint = ReadU64(c);
    const std::uint32_t variableCount = ReadU32(c);
    const std::uint32_t reserved0 = ReadU32(c);
    if (!c.ok || parsed.schemaVersion != kVariableContractSchemaVersionV1 ||
        reservedFlags != 0 || reserved0 != 0) {
        return false;
    }
    if (c.Remaining() < kVariableCapacityBytesV1 ||
        variableCount >
            (c.Remaining() - kVariableCapacityBytesV1) /
                kMinimumVariableBytesV1) {
        return false;
    }
    parsed.variables.reserve(variableCount);
    harkenstudio::ObjectId previousId{};
    bool havePrevious = false;
    for (std::uint32_t i = 0; i < variableCount; ++i) {
        VariableContractRecordV1 record;
        record.execution.variableId = ReadObjectId(c);
        const std::uint8_t kind = ReadU8(c);
        const std::uint8_t scopePolicy = ReadU8(c);
        const std::uint8_t writers = ReadU8(c);
        const std::uint8_t recordReserved0 = ReadU8(c);
        const std::uint32_t minBits = ReadU32(c);
        const std::uint32_t maxBits = ReadU32(c);
        const std::uint32_t defaultBits = ReadU32(c);
        record.execution.executionFingerprint = ReadU64(c);
        if (!c.ok || recordReserved0 != 0 ||
            (havePrevious && record.execution.variableId <= previousId)) {
            return false;
        }
        record.execution.kind = static_cast<ControlValueKindV1>(kind);
        record.execution.scopePolicy =
            static_cast<VariableScopePolicyV1>(scopePolicy);
        record.execution.writers = static_cast<VariableWriterMaskV1>(writers);
        record.execution.range.minimum = std::bit_cast<float>(minBits);
        record.execution.range.maximum = std::bit_cast<float>(maxBits);
        record.execution.range.defaultValue = std::bit_cast<float>(defaultBits);
        if (!ReadPaddedBytes(c, &record.catalog.codeName) ||
            !ReadPaddedBytes(c, &record.catalog.displayName) ||
            !ReadPaddedBytes(c, &record.catalog.description) ||
            !ReadPaddedBytes(c, &record.catalog.unit)) {
            return false;
        }
        record.catalog.precision = ReadI32(c);
        record.catalog.displayScale = ReadU8(c);
        record.catalog.deprecated01 = ReadU8(c);
        const std::uint16_t catalogReserved = ReadU16(c);
        record.catalog.replacementId = ReadObjectId(c);
        const std::uint32_t labelCount = ReadU32(c);
        if (!c.ok || catalogReserved != 0) {
            return false;
        }
        const std::size_t remainingVariables = variableCount - i - 1u;
        const std::size_t requiredTail =
            sizeof(std::uint64_t) + kVariableCapacityBytesV1 +
            remainingVariables * kMinimumVariableBytesV1;
        if (c.Remaining() < requiredTail ||
            labelCount >
                (c.Remaining() - requiredTail) / kMinimumLabelBytesV1) {
            return false;
        }
        record.catalog.labels.reserve(labelCount);
        for (std::uint32_t labelIndex = 0; labelIndex < labelCount;
             ++labelIndex) {
            VariableCatalogLabelV1 label;
            label.anchor = ReadI32(c);
            label.labelId = ReadObjectId(c);
            if (!ReadPaddedBytes(c, &label.name)) {
                return false;
            }
            record.catalog.labels.push_back(std::move(label));
        }
        record.catalog.ownerInitPackId = ReadU64(c);
        if (!c.ok) {
            return false;
        }
        std::vector<std::int32_t> anchors;
        if (record.execution.kind == ControlValueKindV1::Discrete)
            CollectAnchors(record.catalog, &anchors);
        const std::int32_t* pointer =
            anchors.empty() ? nullptr : anchors.data();
        std::uint64_t recomputed = 0;
        if (!harkenstudiobuild::ComputeVariableExecutionFingerprintV1(
                record.execution.variableId, kind, scopePolicy, writers,
                record.execution.range.minimum, record.execution.range.maximum,
                record.execution.range.defaultValue, pointer,
                static_cast<std::uint32_t>(anchors.size()), &recomputed) ||
            recomputed != record.execution.executionFingerprint) {
            return false;
        }
        record.catalogFingerprint = ComputeVariableCatalogFingerprintV1(
            record.execution.variableId, record.catalog);
        previousId = record.execution.variableId;
        havePrevious = true;
        parsed.variables.push_back(std::move(record));
    }
    parsed.capacity.definitionCount = ReadU32(c);
    parsed.capacity.maxReferencedPerContentPack = ReadU32(c);
    parsed.capacity.maxVariableBindings = ReadU32(c);
    parsed.capacity.maxStaticEmitterOverrides = ReadU32(c);
    parsed.capacity.recommendedMaxEmitters = ReadU32(c);
    parsed.capacity.recommendedEmitterVariableSlots = ReadU32(c);
    parsed.capacity.recommendedPendingRows = ReadU32(c);
    parsed.capacity.reserved = ReadU32(c);
    if (!c.ok || c.Remaining() != 0) {
        return false;
    }
    std::uint64_t executionCatalogFingerprint = 0;
    std::uint64_t sdkCatalogFingerprint = 0;
    ComputeArtifactFingerprints(parsed.variables, &executionCatalogFingerprint,
                                &sdkCatalogFingerprint);
    if (executionCatalogFingerprint != parsed.executionCatalogFingerprint ||
        sdkCatalogFingerprint != parsed.sdkCatalogFingerprint) {
        return false;
    }
    *outArtifact = std::move(parsed);
    return true;
}

bool EncodeVariableContractArtifactJsonV1(
    const VariableContractArtifactV1& artifact, std::string* outJson) {
    if (outJson == nullptr) {
        return false;
    }
    VariableContractArtifactV1 canonical;
    if (!Canonicalize(artifact, &canonical)) {
        return false;
    }
    std::string json;
    AppendJsonRaw(&json, "{\"schemaVersion\":");
    AppendJsonU64(&json, canonical.schemaVersion);
    AppendJsonRaw(&json, ",\"projectBuildId\":");
    AppendJsonU64(&json, canonical.projectBuildId);
    AppendJsonRaw(&json, ",\"executionCatalogFingerprint\":");
    AppendJsonU64(&json, canonical.executionCatalogFingerprint);
    AppendJsonRaw(&json, ",\"sdkCatalogFingerprint\":");
    AppendJsonU64(&json, canonical.sdkCatalogFingerprint);
    AppendJsonRaw(&json, ",\"variables\":[");
    for (std::size_t i = 0; i < canonical.variables.size(); ++i) {
        const auto& record = canonical.variables[i];
        std::uint32_t minBits = 0;
        std::uint32_t maxBits = 0;
        std::uint32_t defaultBits = 0;
        if (!harkenstudiobuild::CanonicalF32Bits(record.execution.range.minimum, &minBits) ||
            !harkenstudiobuild::CanonicalF32Bits(record.execution.range.maximum, &maxBits) ||
            !harkenstudiobuild::CanonicalF32Bits(record.execution.range.defaultValue,
                                     &defaultBits)) {
            return false;
        }
        if (i != 0) {
            json.push_back(',');
        }
        AppendJsonRaw(&json, "{\"variableId\":");
        AppendJsonString(&json, harkenstudio::ObjectIdString(record.execution.variableId));
        AppendJsonRaw(&json, ",\"kind\":");
        AppendJsonU64(&json, static_cast<std::uint8_t>(record.execution.kind));
        AppendJsonRaw(&json, ",\"scopePolicy\":");
        AppendJsonU64(&json,
                      static_cast<std::uint8_t>(record.execution.scopePolicy));
        AppendJsonRaw(&json, ",\"writers\":");
        AppendJsonU64(&json,
                      static_cast<std::uint8_t>(record.execution.writers));
        AppendJsonRaw(&json, ",\"rangeMinBits\":");
        AppendJsonU64(&json, minBits);
        AppendJsonRaw(&json, ",\"rangeMaxBits\":");
        AppendJsonU64(&json, maxBits);
        AppendJsonRaw(&json, ",\"defaultBits\":");
        AppendJsonU64(&json, defaultBits);
        AppendJsonRaw(&json, ",\"executionFingerprint\":");
        AppendJsonU64(&json, record.execution.executionFingerprint);
        AppendJsonRaw(&json, ",\"codeName\":");
        AppendJsonString(&json, record.catalog.codeName);
        AppendJsonRaw(&json, ",\"displayName\":");
        AppendJsonString(&json, record.catalog.displayName);
        AppendJsonRaw(&json, ",\"description\":");
        AppendJsonString(&json, record.catalog.description);
        AppendJsonRaw(&json, ",\"unit\":");
        AppendJsonString(&json, record.catalog.unit);
        AppendJsonRaw(&json, ",\"precision\":");
        AppendJsonI32(&json, record.catalog.precision);
        AppendJsonRaw(&json, ",\"displayScale\":");
        AppendJsonU64(&json, record.catalog.displayScale);
        AppendJsonRaw(&json, ",\"deprecated\":");
        AppendJsonU64(&json, record.catalog.deprecated01);
        AppendJsonRaw(&json, ",\"replacementId\":");
        AppendJsonString(&json, harkenstudio::ObjectIdString(record.catalog.replacementId));
        AppendJsonRaw(&json, ",\"labels\":[");
        for (std::size_t labelIndex = 0;
             labelIndex < record.catalog.labels.size(); ++labelIndex) {
            const auto& label = record.catalog.labels[labelIndex];
            if (labelIndex != 0) {
                json.push_back(',');
            }
            AppendJsonRaw(&json, "{\"anchor\":");
            AppendJsonI32(&json, label.anchor);
            AppendJsonRaw(&json, ",\"labelId\":");
            AppendJsonString(&json, harkenstudio::ObjectIdString(label.labelId));
            AppendJsonRaw(&json, ",\"name\":");
            AppendJsonString(&json, label.name);
            json.push_back('}');
        }
        AppendJsonRaw(&json, "],\"ownerInitPackId\":");
        AppendJsonU64(&json, record.catalog.ownerInitPackId);
        json.push_back('}');
    }
    AppendJsonRaw(&json, "],\"capacity\":{\"definitionCount\":");
    AppendJsonU64(&json, canonical.capacity.definitionCount);
    AppendJsonRaw(&json, ",\"maxReferencedPerContentPack\":");
    AppendJsonU64(&json, canonical.capacity.maxReferencedPerContentPack);
    AppendJsonRaw(&json, ",\"maxVariableBindings\":");
    AppendJsonU64(&json, canonical.capacity.maxVariableBindings);
    AppendJsonRaw(&json, ",\"maxStaticEmitterOverrides\":");
    AppendJsonU64(&json, canonical.capacity.maxStaticEmitterOverrides);
    AppendJsonRaw(&json, ",\"recommendedMaxEmitters\":");
    AppendJsonU64(&json, canonical.capacity.recommendedMaxEmitters);
    AppendJsonRaw(&json, ",\"recommendedEmitterVariableSlots\":");
    AppendJsonU64(&json, canonical.capacity.recommendedEmitterVariableSlots);
    AppendJsonRaw(&json, ",\"recommendedPendingRows\":");
    AppendJsonU64(&json, canonical.capacity.recommendedPendingRows);
    AppendJsonRaw(&json, ",\"reserved\":");
    AppendJsonU64(&json, canonical.capacity.reserved);
    AppendJsonRaw(&json, "}}");
    *outJson = std::move(json);
    return true;
}

}  // namespace harkenprojectschema
