#include "harken/core/CurrentWorldCodec.h"
#include "harken/media/PackLogicImage.h"
#include <harken/Harken.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <algorithm>
#include <array>
#include <windows.h>
#include <bcrypt.h>
#include <compressapi.h>
#include <cstring>
#include <cwchar>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#pragma comment(lib, "cabinet.lib")
#pragma comment(lib, "bcrypt.lib")
namespace {
constexpr size_t Max = 16u * 1024u * 1024u;
using Row = VnResidentPackRowV1;

struct Owner {
  HANDLE file = INVALID_HANDLE_VALUE;
  std::vector<uint8_t> bytes;
  ~Owner() {
    if (file != INVALID_HANDLE_VALUE)
      CloseHandle(file);
  }
};
VnResult Open(Owner &owner, const std::wstring &path, bool directory = false) {
  owner.file = CreateFileW(path.c_str(), GENERIC_READ,
                           directory ? FILE_SHARE_READ | FILE_SHARE_WRITE
                                     : FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT |
                               (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0),
                           nullptr);
  if (owner.file == INVALID_HANDLE_VALUE) {
    DWORD e = GetLastError();
    return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND
               ? VN_CONTENT_NOT_LOADED
               : VN_PACK_PROVIDER_FAILED;
  }
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(owner.file, &info) ||
      bool(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory ||
      info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
    return VN_PACK_PROVIDER_FAILED;
  return VN_OK;
}
bool MarkerClear(const std::wstring &root) {
  DWORD a = GetFileAttributesW((root + L"\\.harken-pack-publishing").c_str());
  if (a != INVALID_FILE_ATTRIBUTES)
    return false;
  auto e = GetLastError();
  return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;
}
VnResult Identity(HANDLE file, const uint64_t *expected) {
  BY_HANDLE_FILE_INFORMATION i{};
  FILE_BASIC_INFO b{};
  if (!GetFileInformationByHandle(file, &i) ||
      !GetFileInformationByHandleEx(file, FileBasicInfo, &b, sizeof b))
    return VN_PACK_PROVIDER_FAILED;
  const uint64_t id[] = {i.dwVolumeSerialNumber,
                         uint64_t(i.nFileIndexHigh) << 32 | i.nFileIndexLow,
                         uint64_t(i.nFileSizeHigh) << 32 | i.nFileSizeLow,
                         uint64_t(i.ftLastWriteTime.dwHighDateTime) << 32 |
                             i.ftLastWriteTime.dwLowDateTime,
                         uint64_t(b.ChangeTime.QuadPart)};
  return std::equal(id, id + 5, expected) ? VN_OK : VN_PACK_STALE_REVISION;
}
bool Token(std::string_view &json, std::string_view expected) {
  while (!json.empty() && (json.front() == ' ' || json.front() == '\r' ||
                           json.front() == '\n' || json.front() == '\t'))
    json.remove_prefix(1);
  if (json.substr(0, expected.size()) != expected)
    return false;
  json.remove_prefix(expected.size());
  return true;
}
bool Meta(std::string_view json, std::string_view name, VnObjectId &id) {
  if (!Token(json, "{") || !Token(json, "\"guid\"") || !Token(json, ":") ||
      !Token(json, "\"") || json.size() < 37)
    return false;
  uint64_t hi = 0, lo = 0;
  unsigned digits = 0;
  for (unsigned i = 0; i < 36; i++) {
    char c = json[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-')
        return false;
      continue;
    }
    unsigned v = c >= '0' && c <= '9'   ? c - '0'
                 : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                        : 16;
    if (v > 15)
      return false;
    auto &out = digits++ < 16 ? hi : lo;
    out = (out << 4) | v;
  }
  if (json[36] != '"')
    return false;
  id.high = hi;
  id.low = lo;
  json.remove_prefix(37);
  if ((!hi && !lo) || !Token(json, ",") || !Token(json, "\"name\"") ||
      !Token(json, ":") || !Token(json, "\""))
    return false;
  if (json.size() <= name.size() || json.substr(0, name.size()) != name ||
      json[name.size()] != '"')
    return false;
  json.remove_prefix(name.size() + 1);
  return Token(json, ",") && Token(json, "\"packMetadataVersion\"") &&
         Token(json, ":") && Token(json, "4") && Token(json, "}") &&
         Token(json, "") && json.empty();
}
VnResult Provide(void *ctx, uint64_t, const char *, uint32_t, uint64_t,
                 VnPackByteSpan *span) {
  const auto &bytes = static_cast<Owner *>(ctx)->bytes;
  *span = {sizeof(*span), 0, bytes.data(), bytes.size()};
  return VN_OK;
}
void Release(void *, uint64_t, VnPackByteSpan) {}
struct Decoder {
  DECOMPRESSOR_HANDLE h{};
  ~Decoder() {
    if (h)
      CloseDecompressor(h);
  }
};
} // namespace
static VnResult BindingFacts(const VnPackInspectDescV1 &request,
                             const harken::PackCanonicalImage &init,
                             VnPackInspectReportV1 &report, const harken::detail::PackDecodeBudget& budget) {
  harken::PackCanonicalImage image;
  auto res = harken::ParsePackForResidentPreparation(
      request.pack.bytes, request.pack.byteCount, &image, &report.contentHash,
      budget);
  if (res != VN_OK)
    return res;
  if (!image.hasPartition || image.requirement.storageModelVersion != 2 ||
      image.packKind != HK_PACK_KIND_CANONICAL_CONTENT)
    return VN_BAD_PACK;
  harken::CurrentWorldBodyImage body;
  auto decoded = body.DecodeWithBudget(
      image.partitionBody.data(), image.partitionBody.size(),
      harken::kCanonicalDefinitionPartitionBodyVintage,
      harken::CurrentWorldDecodeMode::StrictArtifact, budget);
  if (decoded == harken::CurrentWorldBodyDecodeResult::Capacity)
    return VN_PACK_RESIDENT_CAPACITY;
  const auto *view = body.ProductionView();
  if (decoded != harken::CurrentWorldBodyDecodeResult::Ok || !view)
    return VN_BAD_PACK;
  bool rooted = false;
  for (uint32_t i = 0; i < view->actionCount; i++)
    rooted |=
        (view->actions[i].actionStableId.high == request.rootActionId.high &&
         view->actions[i].actionStableId.low == request.rootActionId.low);
  if (!rooted)
    return VN_BAD_PACK;
  bool linked = false;
  for (const auto &edge : image.dependencyEdges) {
    if (!edge.required01)
      continue;
    if (edge.edgeKind != HK_PACK_DEPENDENCY_EDGE_KIND_INIT ||
        edge.prerequisitePackStableId != init.packStableId)
      return VN_CONTENT_NOT_LOADED;
    linked = true;
    const auto &bus = init.busDefinitionSnapshotHeader;
    if ((edge.expectedPrerequisiteContentRevision &&
         edge.expectedPrerequisiteContentRevision !=
             init.requirement.packContentRevision) ||
        (edge.expectedBusContentGeneration &&
         edge.expectedBusContentGeneration != bus.contentGeneration) ||
        (edge.expectedPublishedBusFingerprint &&
         edge.expectedPublishedBusFingerprint != bus.publishedBusFingerprint) ||
        (edge.expectedCompileTargetProfileFingerprint &&
         edge.expectedCompileTargetProfileFingerprint !=
             bus.compileTargetProfileFingerprint))
      return VN_PACK_STALE_REVISION;
  }
  if (!linked)
    return VN_BAD_PACK;
  report.packKind = image.packKind;
  report.packStableId = image.packStableId;
  report.contentRevision = image.requirement.packContentRevision;
  report.rootActionId = request.rootActionId;
  report.projectId = view->identity.projectId;
  report.executionFingerprint = view->identity.runtimeSemanticFingerprint;
  return VN_OK;
}
static std::wstring Wide(const char *utf8) {
  const int n =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, nullptr, 0);
  if (n <= 0)
    return {};
  std::wstring value(static_cast<size_t>(n), L'\0');
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1,
                           value.data(), n))
    return {};
  value.pop_back();
  return value;
}
#endif

extern "C" VnResult
Vn_VisitResidentPackDirectoryV1(const VnResidentPackVisitDescV1 *desc) {
  if (!desc || desc->structSize != sizeof(*desc) || !desc->memberCount ||
      !desc->utf8Directory || !desc->utf8Names || !desc->visit ||
      !desc->maxInputBytes || desc->init.structSize != sizeof(VnPackByteSpan) ||
      desc->init.reserved0 || !desc->init.bytes || !desc->init.byteCount)
    return VN_INVALID_ARG;
#if defined(_WIN32)
  try {
    const size_t budget =
        static_cast<size_t>(std::min<uint64_t>(SIZE_MAX, desc->maxInputBytes));
    const auto count = desc->memberCount;
    if (desc->init.byteCount > budget ||
        count > (budget - desc->init.byteCount) / (sizeof(Owner) + sizeof(Row)))
      return VN_PACK_RESIDENT_CAPACITY;
    std::wstring directory = Wide(desc->utf8Directory);
    std::wstring names = Wide(desc->utf8Names);
    if (directory.empty() || names.empty())
      return VN_INVALID_ARG;
    if (!MarkerClear(directory))
      return VN_PACK_STALE_REVISION;
    const uint64_t identity[] = {desc->initVolumeId, desc->initFileId,
                                 desc->initFileSize, desc->initWriteTime,
                                 desc->initChangeTime};
    const auto staging = harken::detail::MakePackDecodeBudget(budget);
    harken::detail::PackDecodeCharge retained(staging);
    retained.Add(desc->init.byteCount);
    retained.AddRows<Owner>(count);
    retained.AddRows<Row>(count);
    harken::PackCanonicalImage initImage;
    const auto parsedInit = harken::ParsePackCanonicalImageWithBudget(desc->init.bytes,
        desc->init.byteCount, &initImage, nullptr, staging);
    if (parsedInit != VN_OK) return parsedInit;
    if (initImage.packKind != HK_PACK_KIND_INIT || !initImage.hasBusDefinitionSnapshot) return VN_BAD_PACK;
    Owner dir, boot;
    auto status = Open(dir, directory, true);
    if (status != VN_OK)
      return status;
    status = Open(boot, directory + L"\\Init.hrp");
    if (status != VN_OK)
      return status;
    status = Identity(boot.file, identity);
    if (status != VN_OK)
      return status;
    Decoder decoder;
    if (!CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr,
                            &decoder.h))
      return VN_DEVICE_ERROR;
    std::vector<Owner> owners(count);
    std::vector<Row> rows(count);
    std::wstring_view rest(names);
    for (uint32_t j = 0; j < count; j++) {
      size_t end = rest.find(L'\n');
      auto wide = rest.substr(0, end);
      if (wide.empty() || wide.size() > 128 ||
          ((end == std::wstring_view::npos) != (j + 1 == count)))
        return VN_INVALID_ARG;
      if (!((wide.front() >= 'A' && wide.front() <= 'Z') ||
            (wide.front() >= 'a' && wide.front() <= 'z')) ||
          wide == L"Init")
        return VN_INVALID_ARG;
      std::string name;
      for (wchar_t c : wide) {
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_'))
          return VN_INVALID_ARG;
        name.push_back(char(c));
      }
      auto file = std::wstring(wide) + L".hrp";
      auto &owner = owners[j];
      auto &row = rows[j];
      status = Open(owner, directory + L"\\" + file);
      if (status != VN_OK)
        return status;
      std::wstring normalized(
          std::max<size_t>(260, directory.size() + file.size() + 32), L'\0');
      auto n =
          GetFinalPathNameByHandleW(owner.file, normalized.data(),
                                    static_cast<DWORD>(normalized.size()), 0);
      if (n >= normalized.size()) {
        normalized.resize(n + 1);
        n = GetFinalPathNameByHandleW(owner.file, normalized.data(),
                                      static_cast<DWORD>(normalized.size()), 0);
      }
      if (!n || n >= normalized.size())
        return VN_PACK_PROVIDER_FAILED;
      std::wstring_view actual(normalized.data(), n);
      if (actual.substr(actual.find_last_of(L'\\') + 1) != file)
        return VN_BAD_PACK;
      LARGE_INTEGER size{};
      if (!GetFileSizeEx(owner.file, &size) || size.QuadPart < 48 ||
          size.QuadPart > 3 * Max)
        return VN_BAD_PACK;
      harken::detail::PackDecodeCharge input(staging);
      input.Add(static_cast<uint64_t>(size.QuadPart));
      std::vector<uint8_t> image(static_cast<size_t>(size.QuadPart));
      DWORD read = 0;
      if (!ReadFile(owner.file, image.data(), DWORD(image.size()), &read,
                    nullptr) ||
          read != image.size())
        return VN_PACK_PROVIDER_FAILED;
      uint32_t h[4];
      memcpy(h, image.data(), 16);
      if (h[0] != 0x31505248 || h[1] == 0 || h[1] > Max || !h[2] ||
          h[2] > Max || !h[3] || size_t(h[2]) + h[3] + 48 != image.size())
        return VN_BAD_PACK;
      retained.Add(h[1]);
      uint8_t digest[32];
      if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, image.data(),
                     ULONG(image.size() - 32), digest, 32) < 0)
        return VN_DEVICE_ERROR;
      if (memcmp(digest, image.data() + image.size() - 32, 32))
        return VN_BAD_PACK;
      VnObjectId id{};
      if (!Meta({reinterpret_cast<char *>(image.data() + 16), h[2]}, name, id))
        return VN_CAPABILITY_UNAVAILABLE;
      owner.bytes.resize(h[1]);
      SIZE_T written{};
      if (!Decompress(decoder.h, image.data() + 16 + h[2], h[3],
                      owner.bytes.data(), h[1], &written) ||
          written != h[1])
        return VN_BAD_PACK;
      VnPackInspectDescV1 req{};
      req.structSize = sizeof req;
      req.pack = {sizeof(VnPackByteSpan), 0, owner.bytes.data(),
                  owner.bytes.size()};
      req.init = desc->init;
      req.rootActionId = id;
      req.maxInputBytes = desc->init.byteCount + owner.bytes.size();
      row.facts.structSize = sizeof row.facts;
      status = BindingFacts(req, initImage, row.facts, staging);
      if (status != VN_OK)
        return status;
      row.byteCount = owner.bytes.size();
      auto &p = row.provider;
      p.structSize = sizeof p;
      p.packStableId = row.facts.packStableId;
      p.packKind = VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
      p.flags = VN_CANONICAL_PACK_SET_MEMBER_PROVIDER;
      p.providerUserData = &owner;
      p.provide = Provide;
      p.release = Release;
      if (end != std::wstring_view::npos)
        rest.remove_prefix(end + 1);
    }
    if (!MarkerClear(directory))
      return VN_PACK_STALE_REVISION;
    return desc->visit(desc->userData, rows.data(), count);
  } catch (const std::bad_alloc &) {
    return VN_PACK_RESIDENT_CAPACITY;
  } catch (...) {
    return VN_DEVICE_ERROR;
  }
#else
  return VN_CAPABILITY_UNAVAILABLE;
#endif
}
