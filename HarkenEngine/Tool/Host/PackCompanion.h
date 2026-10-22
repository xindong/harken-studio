#ifndef HARKEN_HOST_PACK_COMPANION_H
#define HARKEN_HOST_PACK_COMPANION_H

#include "JsonReader.h"
#include "PackPlayPath.h"
#include <harken/Harken.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace harkenhost {

inline bool PackCompanionName(const std::string& name) {
    const auto letter = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    };
    if (name.empty() || name.size() > 128 || !letter(name.front())) return false;
    return std::all_of(name.begin(), name.end(), [&](char c) {
        return letter(c) || (c >= '0' && c <= '9') || c == '_';
    });
}

inline bool PackCompanionLanguage(const std::string& language) {
    if (language.empty()) return true;
    if (language == "." || language == ".." || language.front() == ' ' || language.back() == ' ')
        return false;
    return std::none_of(language.begin(), language.end(), [](unsigned char c) {
        return c < 32 || std::string("/\\:*?\"<>|").find(static_cast<char>(c)) != std::string::npos;
    });
}

// One owned capture. Native borrows these bytes/names only during its call.
// Keep the object in place because name rows point into the parsed JSON.
struct PackCompanion {
    PackCompanion() = default;
    PackCompanion(const PackCompanion&) = delete;
    PackCompanion& operator=(const PackCompanion&) = delete;
    std::vector<std::uint8_t> bytes;
    JsonValue metadata;
    std::string name;
    std::string language;
    std::vector<VnPackVariableNameV1> variables;
    std::vector<VnPackLabelNameV1> labels;
    VnPackNamesV1 names{};
    VnPackInspectReportV1 facts{};

    VnPackByteSpan Span() const { return {sizeof(VnPackByteSpan), 0, bytes.data(), bytes.size()}; }

    static VnResult Provide(void* context, std::uint64_t id, const char*, std::uint32_t,
                            std::uint64_t, VnPackByteSpan* out) {
        const auto& input = *static_cast<const PackCompanion*>(context);
        if (id != input.facts.packStableId) return VN_CONTENT_NOT_LOADED;
        *out = input.Span();
        return VN_OK;
    }
    static void Release(void*, std::uint64_t, VnPackByteSpan) {}
};

inline bool PackCompanionGuid(const JsonValue& row, VnObjectId* id) {
    const auto* value = row.Find("guid");
    return value && value->kind == JsonValue::Kind::String &&
        harken::ParseObjectId(value->text.c_str(), id) && !harken::IsNull(*id) &&
        harken::ObjectIdString(*id) == value->text;
}

inline VnResult ParsePackCompanion(const std::string& json, const std::string& name,
                                    const std::string& language, VnObjectId project,
                                    PackCompanion* out) {
    std::string error;
    out->metadata = {};
    if (!ParseJson(json, &out->metadata, &error, true) ||
        out->metadata.kind != JsonValue::Kind::Object) return VN_BAD_PACK;
    out->name = name;
    out->language = language;
    out->variables.clear();
    out->labels.clear();
    out->names = {};
    out->names.structSize = sizeof(out->names);
    out->names.projectId = project;
    if (!name.empty()) {
        const auto* named = out->metadata.Find("name");
        if (!PackCompanionName(name) || name == "Init" || !named ||
            named->kind != JsonValue::Kind::String || named->text != name ||
            !PackCompanionGuid(out->metadata, &out->names.rootActionId)) return VN_BAD_PACK;
    } else {
        const auto* rows = out->metadata.Find("variables");
        if (!rows || rows->kind != JsonValue::Kind::Array) return VN_BAD_PACK;
        for (const auto& row : rows->items) {
            VnObjectId id{};
            const auto* named = row.Find("name");
            const auto* labels = row.Find("labels");
            if (!PackCompanionGuid(row, &id) || !named || named->kind != JsonValue::Kind::String ||
                !PackCompanionName(named->text) || !labels || labels->kind != JsonValue::Kind::Array)
                return VN_BAD_PACK;
            out->variables.push_back({id, named->text.c_str()});
            for (const auto& label : labels->items) {
                VnObjectId labelId{};
                const auto* text = label.Find("name");
                if (!PackCompanionGuid(label, &labelId) || !text || text->kind != JsonValue::Kind::String ||
                    text->text.empty() || text->text.find('\0') != std::string::npos) return VN_BAD_PACK;
                out->labels.push_back({id, labelId, text->text.c_str()});
            }
        }
    }
    out->names.utf8RootName = out->name.c_str();
    out->names.utf8Language = out->language.c_str();
    out->names.variableCount = static_cast<std::uint32_t>(out->variables.size());
    out->names.variables = out->variables.data();
    out->names.labelCount = static_cast<std::uint32_t>(out->labels.size());
    out->names.labels = out->labels.data();
    return VN_OK;
}

namespace companiondetail {

inline std::filesystem::path Path(const std::string& text) {
    return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

inline VnResult Status(const std::filesystem::path& path, bool* exists) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) { *exists = false; return VN_OK; }
    if (error) return VN_PACK_PROVIDER_FAILED;
    *exists = std::filesystem::exists(status);
    return VN_OK;
}

inline VnResult NotPublishing(const std::filesystem::path& root) {
    bool exists = false;
    const auto result = Status(root / ".harken-pack-publishing", &exists);
    return result != VN_OK ? result : exists ? VN_PACK_STALE_REVISION : VN_OK;
}

struct Identity {
    std::uint64_t device{}, file{}, size{}, modified{}, modifiedNs{}, changed{}, changedNs{};
    bool operator==(const Identity&) const = default;
};

class File {
public:
    File() = default;
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    ~File() {
#if defined(_WIN32)
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
        if (handle_ >= 0) close(handle_);
#endif
    }
    bool present = false;
    VnResult Open(const std::filesystem::path& path, const std::string& language) {
#if defined(_WIN32)
        handle_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? VN_OK : VN_PACK_PROVIDER_FAILED;
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle_, &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
            return VN_PACK_PROVIDER_FAILED;
        std::wstring resolved(32768, L'\0');
        const auto count = GetFinalPathNameByHandleW(handle_, resolved.data(), static_cast<DWORD>(resolved.size()), 0);
        if (count == 0 || count >= resolved.size()) return VN_PACK_PROVIDER_FAILED;
        resolved.resize(count);
        if (resolved.starts_with(L"\\\\?\\UNC\\")) resolved = L"\\\\" + resolved.substr(8);
        else if (resolved.starts_with(L"\\\\?\\")) resolved.erase(0, 4);
        const std::filesystem::path actual(resolved);
        if (_wcsicmp(actual.parent_path().c_str(), path.parent_path().make_preferred().c_str()) != 0)
            return VN_PACK_PROVIDER_FAILED;
#else
        handle_ = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
        if (handle_ < 0) return errno == ENOENT ? VN_OK : VN_PACK_PROVIDER_FAILED;
        struct stat info{};
        if (fstat(handle_, &info) != 0 || !S_ISREG(info.st_mode)) return VN_PACK_PROVIDER_FAILED;
        char resolved[4096]{};
#if defined(__APPLE__)
        if (fcntl(handle_, F_GETPATH, resolved) != 0) return VN_PACK_PROVIDER_FAILED;
#else
        const auto link = "/proc/self/fd/" + std::to_string(handle_);
        const auto count = readlink(link.c_str(), resolved, sizeof(resolved) - 1);
        if (count <= 0 || static_cast<std::size_t>(count) >= sizeof(resolved) - 1) return VN_PACK_PROVIDER_FAILED;
        resolved[count] = '\0';
#endif
        const std::filesystem::path actual(resolved);
        if (actual.parent_path() != path.parent_path()) return VN_PACK_PROVIDER_FAILED;
#endif
        if (actual.filename() != path.filename() ||
            (!language.empty() && actual.parent_path().filename() != Path(language))) return VN_BAD_PACK;
        present = true;
        return VN_OK;
    }
    VnResult Identify(Identity* out) const {
#if defined(_WIN32)
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle_, &info)) return VN_PACK_PROVIDER_FAILED;
        *out = {info.dwVolumeSerialNumber, (std::uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow,
            (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow,
            (std::uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime};
#else
        struct stat info{};
        if (fstat(handle_, &info) != 0) return VN_PACK_PROVIDER_FAILED;
#if defined(__APPLE__)
        const auto modified = info.st_mtimespec, changed = info.st_ctimespec;
#else
        const auto modified = info.st_mtim, changed = info.st_ctim;
#endif
        *out = {std::uint64_t(info.st_dev), std::uint64_t(info.st_ino), std::uint64_t(info.st_size),
            std::uint64_t(modified.tv_sec), std::uint64_t(modified.tv_nsec),
            std::uint64_t(changed.tv_sec), std::uint64_t(changed.tv_nsec)};
#endif
        return VN_OK;
    }
    VnResult Read(std::uint64_t size, std::vector<std::uint8_t>* out) const {
        if (size > std::uint64_t(std::numeric_limits<std::int32_t>::max())) return VN_PACK_PROVIDER_FAILED;
        out->resize(static_cast<std::size_t>(size));
        std::size_t offset = 0;
        while (offset < out->size()) {
#if defined(_WIN32)
            DWORD count = 0;
            if (!ReadFile(handle_, out->data() + offset, static_cast<DWORD>(out->size() - offset), &count, nullptr) || count == 0)
                return VN_PACK_PROVIDER_FAILED;
#else
            const auto count = read(handle_, out->data() + offset, out->size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return VN_PACK_PROVIDER_FAILED;
#endif
            offset += static_cast<std::size_t>(count);
        }
        return VN_OK;
    }
private:
#if defined(_WIN32)
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int handle_ = -1;
#endif
};

inline VnResult Pair(const std::filesystem::path& root, const std::string& basename,
                     const std::string& language, std::vector<std::uint8_t>* bytes,
                     std::vector<std::uint8_t>* json) {
    VnResult result = NotPublishing(root);
    if (result != VN_OK) return result;
    const auto directory = language.empty() ? root : root / Path(language);
    if (!language.empty()) {
        bool exists = false;
        if (Status(directory, &exists) != VN_OK) return VN_PACK_PROVIDER_FAILED;
        if (exists) {
#if defined(_WIN32)
            const auto attributes = GetFileAttributesW(directory.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return VN_PACK_PROVIDER_FAILED;
#else
            std::error_code error;
            if (std::filesystem::is_symlink(std::filesystem::symlink_status(directory, error)) || error) return VN_PACK_PROVIDER_FAILED;
#endif
        }
    }
    const auto binaryPath = directory / Path(basename + ".bytes");
    const auto jsonPath = directory / Path(basename + ".json");
    File binary, metadata;
    if ((result = binary.Open(binaryPath, language)) != VN_OK ||
        (result = metadata.Open(jsonPath, language)) != VN_OK) return result;
    if (!binary.present && !metadata.present) {
        File againBinary, againJson;
        if ((result = againBinary.Open(binaryPath, language)) != VN_OK ||
            (result = againJson.Open(jsonPath, language)) != VN_OK) return result;
        return !againBinary.present && !againJson.present && NotPublishing(root) == VN_OK
            ? VN_CONTENT_NOT_LOADED : VN_PACK_STALE_REVISION;
    }
    if (!binary.present || !metadata.present) return VN_BAD_PACK;
    Identity binaryBefore{}, jsonBefore{};
    if (binary.Identify(&binaryBefore) != VN_OK || metadata.Identify(&jsonBefore) != VN_OK)
        return VN_PACK_PROVIDER_FAILED;
    if ((result = binary.Read(binaryBefore.size, bytes)) != VN_OK ||
        (result = metadata.Read(jsonBefore.size, json)) != VN_OK) return result;
    File currentBinary, currentJson;
    if ((result = currentBinary.Open(binaryPath, language)) != VN_OK ||
        (result = currentJson.Open(jsonPath, language)) != VN_OK) return result;
    if (!currentBinary.present || !currentJson.present || NotPublishing(root) != VN_OK) return VN_PACK_STALE_REVISION;
    Identity a{}, b{}, c{}, d{};
    if (binary.Identify(&a) != VN_OK || metadata.Identify(&b) != VN_OK ||
        currentBinary.Identify(&c) != VN_OK || currentJson.Identify(&d) != VN_OK) return VN_PACK_PROVIDER_FAILED;
    return a == binaryBefore && b == jsonBefore && c == a && d == b ? VN_OK : VN_PACK_STALE_REVISION;
}
} // namespace companiondetail

inline VnResult CapturePackCompanion(const std::filesystem::path& root, const std::string& name,
                                     const std::string& language, const PackCompanion* init,
                                     PackCompanion* out) {
    if ((!name.empty() && (!PackCompanionName(name) || name == "Init")) ||
        !PackCompanionLanguage(language)) return VN_INVALID_ARG;
    std::string scope = name.empty() ? std::string{} : language;
    const std::string basename = name.empty() ? "Init" : name;
    std::vector<std::uint8_t> json;
    auto result = companiondetail::Pair(root, basename, scope, &out->bytes, &json);
    if (result == VN_CONTENT_NOT_LOADED && !scope.empty()) {
        result = companiondetail::Pair(root, basename, {}, &out->bytes, &json);
        std::vector<std::uint8_t> ignoredBytes, ignoredJson;
        if (companiondetail::Pair(root, basename, scope, &ignoredBytes, &ignoredJson) != VN_CONTENT_NOT_LOADED)
            return VN_PACK_STALE_REVISION;
        scope.clear();
    }
    if (result != VN_OK) return result;
    const std::string text(json.begin(), json.end());
    result = ParsePackCompanion(text, name, scope, init ? init->facts.projectId : VnObjectId{}, out);
    if (result != VN_OK) return result;
    VnPackInspectDescV1 desc{};
    desc.structSize = sizeof(desc);
    desc.pack = out->Span();
    desc.init = init ? init->Span() : VnPackByteSpan{sizeof(VnPackByteSpan), 0, nullptr, 0};
    desc.names = &out->names;
    desc.maxInputBytes = out->bytes.size() + desc.init.byteCount + 1024 +
        (out->variables.size() + out->labels.size()) * 256 + json.size();
    out->facts = {};
    out->facts.structSize = sizeof(out->facts);
    result = Vn_InspectCanonicalPackV1(&desc, &out->facts);
    if (result == VN_OK) out->names.projectId = out->facts.projectId;
    return result;
}

inline VnResult ActivatePackCompanions(PackCompanion& init, PackCompanion& content) {
    VnCanonicalPackSetMemberV2 members[2]{};
    VnCanonicalPackSetMemberResultV1 results[2]{};
    PackCompanion* inputs[]{&init, &content};
    for (std::size_t i = 0; i < 2; ++i) {
        auto& member = members[i];
        member.structSize = sizeof(member);
        member.operation = VN_PACK_MEMBER_CONTENT;
        member.names = &inputs[i]->names;
        member.source.structSize = sizeof(member.source);
        member.source.flags = VN_CANONICAL_PACK_SET_MEMBER_PROVIDER;
        member.source.packStableId = inputs[i]->facts.packStableId;
        member.source.packKind = inputs[i]->facts.packKind;
        member.source.providerUserData = inputs[i];
        member.source.provide = PackCompanion::Provide;
        member.source.release = PackCompanion::Release;
        results[i].structSize = sizeof(results[i]);
    }
    VnCanonicalPackSetActivationDescV2 desc{};
    desc.structSize = sizeof(desc);
    desc.setRevision = 1;
    desc.memberCount = 2;
    desc.memberStructSize = sizeof(members[0]);
    desc.members = members;
    desc.outputMemberCapacity = 2;
    desc.outputMemberStructSize = sizeof(results[0]);
    desc.outputMembers = results;
    VnCanonicalPackSetActivationReportV1 report{};
    report.structSize = sizeof(report);
    return Vn_ActivateCanonicalPackSetV2(&desc, &report);
}

} // namespace harkenhost
#endif
