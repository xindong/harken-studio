#include "harken/core/PackCompanionNames.h"
#include "harken/param/VariableService.h"

#include <algorithm>
#include <set>
#include <tuple>

namespace harken {
namespace {

bool Utf8(std::string_view text) noexcept {
    std::uint32_t remaining = 0, code = 0, minimum = 0;
    for (const unsigned char c : text) {
        if (remaining == 0) {
            if (c < 0x80) continue;
            if (c >= 0xc2 && c <= 0xdf) { remaining = 1; code = c & 31; minimum = 0x80; }
            else if (c >= 0xe0 && c <= 0xef) { remaining = 2; code = c & 15; minimum = 0x800; }
            else if (c >= 0xf0 && c <= 0xf4) { remaining = 3; code = c & 7; minimum = 0x10000; }
            else return false;
        } else {
            if ((c & 0xc0) != 0x80) return false;
            code = (code << 6) | (c & 63);
            if (--remaining == 0 && (code < minimum || code > 0x10ffff ||
                (code >= 0xd800 && code <= 0xdfff))) return false;
        }
    }
    return remaining == 0;
}

bool Capture(const char* input, std::uint64_t& budget, std::string& out) {
    if (input == nullptr) return false;
    std::size_t size = 0;
    while (size < budget && input[size] != 0) ++size;
    if (size == budget) return false;
    budget -= size + 1;
    out.assign(input, size);
    return Utf8(out);
}

void HashString(std::uint64_t& hash, const std::string& value) noexcept {
    const auto size = static_cast<std::uint64_t>(value.size());
    std::uint8_t bytes[8];
    StoreLE64(bytes, size);
    hash = Fnv1a64(bytes, sizeof(bytes), hash);
    hash = Fnv1a64(value.data(), value.size(), hash);
}

void HashId(std::uint64_t& hash, VnObjectId id) noexcept {
    const auto bytes = ObjectIdBytes(id);
    hash = Fnv1a64(bytes.data(), bytes.size(), hash);
}

} // namespace

VnResult CapturePackCompanionNames(const VnPackNamesV1& input,
    std::uint64_t& byteBudget, PackCompanionNames* out) {
    if (out == nullptr || input.structSize != sizeof(input) || input.flags != 0 ||
        input.projectId == VnObjectId{} ||
        (input.variableCount != 0 && input.variables == nullptr) ||
        (input.labelCount != 0 && input.labels == nullptr)) return VN_INVALID_ARG;
    const auto rowBytes = sizeof(PackCompanionNames) + static_cast<std::uint64_t>(input.variableCount) * sizeof(PackCompanionNames::Variable) +
        static_cast<std::uint64_t>(input.labelCount) * sizeof(PackCompanionNames::Label);
    if (rowBytes > byteBudget) return VN_PACK_RESIDENT_CAPACITY;
    byteBudget -= rowBytes;
    PackCompanionNames names;
    names.projectId = input.projectId;
    names.rootActionId = input.rootActionId;
    if (!Capture(input.utf8RootName, byteBudget, names.rootName) ||
        !Capture(input.utf8Language, byteBudget, names.language)) return VN_INVALID_ARG;
    if (names.rootActionId == VnObjectId{}) {
        if (!names.rootName.empty() || !names.language.empty()) return VN_INVALID_ARG;
    } else if (!IsSdkCallableName(names.rootName) || input.variableCount != 0 || input.labelCount != 0) {
        return VN_INVALID_ARG;
    }
    if (names.language == "." || names.language == ".." ||
        names.language.find_first_of("/\\:") != std::string::npos ||
        (!names.language.empty() && (names.language.front() == ' ' || names.language.back() == ' ')))
        return VN_INVALID_ARG;
    names.variables.reserve(input.variableCount);
    names.labels.reserve(input.labelCount);
    std::set<VnObjectId> variableIds;
    for (std::uint32_t i = 0; i < input.variableCount; ++i) {
        PackCompanionNames::Variable row{input.variables[i].variableId, {}};
        if (row.id == VnObjectId{} || !Capture(input.variables[i].utf8Name, byteBudget, row.name) ||
            !IsSdkCallableName(row.name) || !variableIds.insert(row.id).second) return VN_INVALID_ARG;
        names.variables.push_back(std::move(row));
    }
    std::set<std::pair<VnObjectId, VnObjectId>> labelIds;
    for (std::uint32_t i = 0; i < input.labelCount; ++i) {
        const auto& source = input.labels[i];
        PackCompanionNames::Label row{source.variableId, source.labelId, {}};
        if (row.id == VnObjectId{} || !variableIds.contains(row.variableId) ||
            !Capture(source.utf8Name, byteBudget, row.name) || row.name.empty() ||
            !labelIds.emplace(row.variableId, row.id).second) return VN_INVALID_ARG;
        names.labels.push_back(std::move(row));
    }
    std::sort(names.variables.begin(), names.variables.end(),
        [](const auto& a, const auto& b) { return a.name < b.name; });
    std::sort(names.labels.begin(), names.labels.end(), [](const auto& a, const auto& b) {
        return std::tie(a.variableId, a.name) < std::tie(b.variableId, b.name);
    });
    for (std::size_t i = 1; i < names.variables.size(); ++i)
        if (names.variables[i - 1].name == names.variables[i].name) return VN_INVALID_ARG;
    for (std::size_t i = 1; i < names.labels.size(); ++i)
        if (names.labels[i - 1].variableId == names.labels[i].variableId &&
            names.labels[i - 1].name == names.labels[i].name) return VN_INVALID_ARG;
    auto hash = Fnv1a64(nullptr, 0);
    HashId(hash, names.projectId);
    HashId(hash, names.rootActionId);
    HashString(hash, names.rootName);
    HashString(hash, names.language);
    for (const auto& row : names.variables) { HashId(hash, row.id); HashString(hash, row.name); }
    for (const auto& row : names.labels) {
        HashId(hash, row.variableId); HashId(hash, row.id); HashString(hash, row.name);
    }
    names.digest = hash;
    *out = std::move(names);
    return VN_OK;
}

std::uint64_t CompanionActionPackId(std::uint64_t initPackId,
    VnObjectId rootActionId, std::string_view language) noexcept {
    // Inverse of the producer's FNV-1a multiplication modulo 2^64.
    constexpr std::uint64_t inverse = UINT64_C(0xce965057aff6957b);
    constexpr std::string_view suffix = "/Init";
    auto hash = initPackId;
    for (auto i = suffix.size(); i != 0; --i)
        hash = (hash * inverse) ^ static_cast<unsigned char>(suffix[i - 1]);
    hash = Fnv1a64("/", 1, hash);
    if (!language.empty()) {
        hash = Fnv1a64(language.data(), language.size(), hash);
        hash = Fnv1a64("/", 1, hash);
    }
    hash = Fnv1a64("action_", 7, hash);
    const auto id = FormatObjectId(rootActionId);
    return Fnv1a64(id.data(), 36, hash);
}

VnResult ValidatePackCompanionNames(const PackCompanionNames& names,
    std::uint32_t kind, std::uint64_t packId, std::uint64_t initPackId,
    const ContainerWorldInstallViewV1& view,
    const std::vector<PackCanonicalImage::VariablePolicyOverlay>& variables) {
    if (names.projectId != view.identity.projectId) return VN_GAMEPLAY_WRONG_PROJECT;
    if (kind == kPackKindCanonicalContent) {
        if (names.rootActionId == VnObjectId{} || initPackId == 0 ||
            CompanionActionPackId(initPackId, names.rootActionId, names.language) != packId)
            return VN_BAD_PACK;
        for (std::uint32_t i = 0; i < view.actionCount; ++i)
            if (view.actions[i].actionStableId == names.rootActionId) return VN_OK;
        return VN_BAD_PACK;
    }
    if (kind != kPackKindInit || names.rootActionId != VnObjectId{} ||
        packId != initPackId || names.variables.size() != view.variableCount) return VN_BAD_PACK;
    for (const auto& name : names.variables) {
        bool found = false;
        for (std::uint32_t i = 0; i < view.variableCount; ++i)
            found |= view.variables[i].variableId == name.id;
        if (!found) return VN_BAD_PACK;
    }
    for (const auto& label : names.labels) {
        bool found = false;
        for (const auto& variable : variables) {
            if (variable.variableId != label.variableId) continue;
            for (const auto& definition : variable.labels) found |= definition.labelId == label.id;
        }
        if (!found) return VN_BAD_PACK;
    }
    return VN_OK;
}

} // namespace harken
