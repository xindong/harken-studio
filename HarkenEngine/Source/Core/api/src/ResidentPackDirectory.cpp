#include "ResidentPackDirectory.h"
#include "harken/media/VorbisSetupBatch.h"
#include "harken/media/PackLogicImage.h"
#include <harken/Harken.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <algorithm>
#include <array>
#include <atomic>
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
// Largest byte array accepted by the Windows Unity reader and publisher.
constexpr uint64_t MaxResidentArrayBytes = 0x7fffffc7u;
constexpr uint32_t Hrp1 = 0x31505248;
constexpr uint32_t Hrp2 = 0x32505248;
using Row = VnResidentPackRowV1;

struct Owner {
  HANDLE file = INVALID_HANDLE_VALUE;
  std::vector<uint8_t> bytes;
  ~Owner() {
    if (file != INVALID_HANDLE_VALUE)
      CloseHandle(file);
  }
};
VnResult Open(Owner &owner, const std::wstring &path, bool directory = false, uint64_t* fileBytes = nullptr) {
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
  if (fileBytes)
    *fileBytes = (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
  return VN_OK;
}
VnResult CheckMemberName(HANDLE handle, std::wstring_view expected) {
  const auto compare = [expected](std::wstring_view actual) {
    return actual.substr(actual.find_last_of(L'\\') + 1) == expected ? VN_OK : VN_BAD_PACK;
  };
  // The name must come from the held file. Its volume or drive name is irrelevant.
  alignas(FILE_NAME_INFO) std::array<unsigned char, 1024> storage;
  if (GetFileInformationByHandleEx(handle, FileNormalizedNameInfo, storage.data(),
                                   static_cast<DWORD>(storage.size()))) {
    const auto& info = *reinterpret_cast<const FILE_NAME_INFO*>(storage.data());
    if (info.FileNameLength % sizeof(wchar_t) != 0 ||
        info.FileNameLength > storage.size() - offsetof(FILE_NAME_INFO, FileName))
      return VN_PACK_PROVIDER_FAILED;
    return compare({info.FileName, info.FileNameLength / sizeof(wchar_t)});
  }
  // Older systems and longer paths use the variable-length API.
  std::wstring name(260, L'\0');
  auto length = GetFinalPathNameByHandleW(handle, name.data(), DWORD(name.size()), VOLUME_NAME_NONE);
  if (length >= name.size()) {
    name.resize(length);
    length = GetFinalPathNameByHandleW(handle, name.data(), DWORD(name.size()), VOLUME_NAME_NONE);
  }
  if (!length || length >= name.size()) return VN_PACK_PROVIDER_FAILED;
  return compare({name.data(), length});
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
bool Unpack(DECOMPRESSOR_HANDLE decoder, uint32_t format,
            std::span<const uint8_t> input, size_t remaining, uint8_t* output) {
  if (format == Hrp1) {
    SIZE_T written{};
    if (!output)
      return !Decompress(decoder, input.data(), input.size(), nullptr, 0, &written) &&
             GetLastError() == ERROR_INSUFFICIENT_BUFFER && written == remaining;
    return Decompress(decoder, input.data(), input.size(), output, remaining, &written) &&
           written == remaining;
  }
  while (remaining) {
    uint32_t stored{};
    if (input.size() < sizeof(stored)) return false;
    memcpy(&stored, input.data(), sizeof(stored));
    input = input.subspan(sizeof(stored));
    const size_t bytes = std::min<size_t>(8192, remaining);
    if (!stored || stored > bytes || stored > input.size()) return false;
    if (output) {
      if (stored == bytes) {
        memcpy(output, input.data(), bytes);
      } else {
        SIZE_T written{};
        if (!Decompress(decoder, input.data(), stored, output, bytes, &written) ||
            written != bytes) return false;
      }
      output += bytes;
    }
    input = input.subspan(stored);
    remaining -= bytes;
  }
  return input.empty();
}

struct VisitScope {
  std::span<Owner> owners;
  std::span<harken::PackCanonicalImage> images;
  std::span<Row> rows;
  VisitScope* prior;
  static thread_local VisitScope* current;
  VisitScope(std::span<Owner> owners, std::span<harken::PackCanonicalImage> images, std::span<Row> rows)
      : owners(owners), images(images), rows(rows), prior(current) { current = this; }
  ~VisitScope() { current = prior; }
  size_t Index(void* context) const noexcept {
    const auto begin = reinterpret_cast<uintptr_t>(owners.data());
    const auto address = reinterpret_cast<uintptr_t>(context);
    if (address < begin || address - begin >= owners.size_bytes() ||
        (address - begin) % sizeof(Owner) != 0)
      return owners.size();
    return (address - begin) / sizeof(Owner);
  }
};
thread_local VisitScope* VisitScope::current = nullptr;

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
                             VnPackInspectReportV1 &report, const harken::detail::PackDecodeBudget& budget,
                             harken::PackCanonicalImage& image) {
  report.rootActionId = request.rootActionId;
  auto res = harken::ParsePackForResidentPreparation(
      request.pack.bytes, request.pack.byteCount, &image, &report.contentHash,
      budget, &report);
  if (res != VN_OK)
    return res;
  if (!image.hasPartition || image.requirement.storageModelVersion != 2 ||
      image.packKind != HK_PACK_KIND_CANONICAL_CONTENT)
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

bool harken::IsResidentDirectorySource(VnPackByteProviderFn provide, VnPackByteReleaseFn release, void* context) noexcept {
#if defined(_WIN32)
  const auto* visit = VisitScope::current;
  return provide == Provide && release == Release && visit && visit->Index(context) < visit->owners.size();
#else
  return false;
#endif
}

bool harken::TakeResidentDirectoryImage(const VnPackLoadDesc& source, const VnPackByteSpan& span,
    const detail::PackDecodeBudget& budget, PackCanonicalImage& output, uint64_t& hash) {
#if defined(_WIN32)
  if (!IsResidentDirectorySource(source.provide, source.release, source.providerUserData)) return false;
  auto& visit = *VisitScope::current;
  const auto index = visit.Index(source.providerUserData);
  if (index >= visit.images.size()) return false;
  auto& image = visit.images[index];
  const auto& bytes = visit.owners[index].bytes;
  if (!image.hasPartition || image.packStableId != source.packStableId ||
      bytes.data() != span.bytes || bytes.size() != span.byteCount) return false;
  detail::PackDecodeCharge charge(budget);
  charge.Add(image.staging.Bytes());
  image.staging = std::move(charge);
  output = std::move(image);
  image.hasPartition = false;
  hash = visit.rows[index].facts.contentHash;
  return true;
#else
  return false;
#endif
}

static VnResult VisitDirectory(const VnResidentPackVisitDescV1 *desc, bool keepImages, bool allowParallel, bool& submitted, bool warm = false) {
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
    std::vector<harken::PackCanonicalImage> images;
    if (keepImages) harken::detail::DecodeResize(images, count, retained);
    std::vector<Owner> owners(count);
    std::vector<Row> rows(count);
    harken::VorbisSetupBatch setupBatch;
    auto prepareRange = [&](uint32_t begin, uint32_t endIndex, std::wstring_view rest,
                            const harken::detail::PackDecodeBudget& laneBudget,
                            harken::detail::PackDecodeCharge& rawBytes) -> VnResult {
      Decoder decoder;
      if (!CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &decoder.h))
        return VN_DEVICE_ERROR;
      for (uint32_t j = begin; j < endIndex; ++j) {
        size_t end = rest.find(L'\n');
        auto wide = rest.substr(0, end);
        if (wide.empty() || wide.size() > 128 ||
            ((end == std::wstring_view::npos) != (j + 1 == endIndex)))
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
        uint64_t fileBytes{};
        auto status = Open(owner, directory + L"\\" + file, false, &fileBytes);
        if (status != VN_OK)
          return status;
        status = CheckMemberName(owner.file, file);
        if (status != VN_OK)
          return status;
        constexpr size_t digestBytes = 32;
        uint32_t h[4];
        if (fileBytes < sizeof(h) + digestBytes || fileBytes > MaxResidentArrayBytes)
          return VN_BAD_PACK;
        DWORD read{};
        if (!ReadFile(owner.file, h, sizeof(h), &read, nullptr) || read != sizeof(h))
          return VN_PACK_PROVIDER_FAILED;
        if ((h[0] != Hrp1 && h[0] != Hrp2) || !h[1] || h[1] > MaxResidentArrayBytes ||
            !h[2] || !h[3] || sizeof(h) + uint64_t(h[2]) + h[3] + digestBytes != fileBytes)
          return VN_BAD_PACK;
        harken::detail::PackDecodeCharge input(laneBudget);
        input.Add(fileBytes);
        rawBytes.Add(h[1]);
        std::vector<uint8_t> image(static_cast<size_t>(fileBytes));
        memcpy(image.data(), h, sizeof(h));
        const auto remaining = static_cast<DWORD>(image.size() - sizeof(h));
        if (!ReadFile(owner.file, image.data() + sizeof(h), remaining, &read, nullptr) || read != remaining)
          return VN_PACK_PROVIDER_FAILED;
        uint8_t digest[32];
        if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, image.data(),
                       ULONG(image.size() - 32), digest, 32) < 0)
          return VN_DEVICE_ERROR;
        if (memcmp(digest, image.data() + image.size() - 32, 32))
          return VN_BAD_PACK;
        VnObjectId id{};
        if (!Meta({reinterpret_cast<char *>(image.data() + 16), h[2]}, name, id))
          return VN_CAPABILITY_UNAVAILABLE;
        const std::span<const uint8_t> compressed{image.data() + sizeof(h) + h[2], h[3]};
        // Validate the declared output against the compressed data before allocating it.
        if (!Unpack(decoder.h, h[0], compressed, h[1], nullptr)) return VN_BAD_PACK;
        owner.bytes.resize(h[1]);
        if (!Unpack(decoder.h, h[0], compressed, h[1], owner.bytes.data()))
          return VN_BAD_PACK;
        VnPackInspectDescV1 req{};
        req.structSize = sizeof req;
        req.pack = {sizeof(VnPackByteSpan), 0, owner.bytes.data(),
                    owner.bytes.size()};
        req.init = desc->init;
        req.rootActionId = id;
        req.maxInputBytes = desc->init.byteCount + owner.bytes.size();
        row.facts.structSize = sizeof row.facts;
        harken::PackCanonicalImage scratch;
        status = BindingFacts(req, initImage, row.facts, laneBudget, keepImages ? images[j] : scratch);
        if (status != VN_OK)
          return status;
        if (warm) {
          for (const auto& media : images[j].media) {
            if (media.sampleFormat == HK_PACK_SAMPLE_FORMAT_VORBIS_RESIDENT)
              setupBatch.Prepare(std::span<const uint8_t>(owner.bytes).subspan(media.blobOffset, media.blobBytes), rawBytes);
          }
        }
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
      return VN_OK;
    };
    constexpr uint32_t laneCount = 4;
    if (allowParallel && count >= laneCount * 2) {
      // Partition the remaining allowance before submitting work. No mutable
      // decoder, budget counter or file owner is shared between worker lanes.
      const auto laneLimit = (budget - staging->live) / laneCount;
      struct Range { uint32_t first, last; std::wstring_view names; };
      std::array<Range, laneCount> ranges{};
      std::wstring_view rest(names);
      for (uint32_t lane = 0; lane < laneCount; ++lane) {
        auto& range = ranges[lane];
        range.first = static_cast<uint32_t>(uint64_t(count) * lane / laneCount);
        range.last = static_cast<uint32_t>(uint64_t(count) * (lane + 1) / laneCount);
        if (lane + 1 == laneCount) { range.names = rest; break; }
        size_t offset = 0;
        for (auto j = range.first; j < range.last; ++j) {
          const auto delimiter = rest.find(L'\n', offset);
          if (delimiter == std::wstring_view::npos) return VN_INVALID_ARG;
          offset = delimiter + 1;
        }
        range.names = rest.substr(0, offset - 1);
        rest.remove_prefix(offset);
      }
      std::array<VnResult, laneCount> results{};
      std::atomic<uint32_t> next{};
      auto run = [&]() noexcept {
        const auto index = next.fetch_add(1, std::memory_order_relaxed);
        const auto& range = ranges[index];
        try {
          auto localBudget = harken::detail::MakePackDecodeBudget(laneLimit);
          harken::detail::PackDecodeCharge rawBytes(localBudget);
          results[index] = prepareRange(range.first, range.last, range.names, localBudget, rawBytes);
        } catch (const std::bad_alloc&) { results[index] = VN_PACK_RESIDENT_CAPACITY; }
          catch (...) { results[index] = VN_DEVICE_ERROR; }
      };
      PTP_WORK work = CreateThreadpoolWork([](PTP_CALLBACK_INSTANCE, void* ctx, PTP_WORK) {
        (*static_cast<decltype(run)*>(ctx))();
      }, &run, nullptr);
      if (work) {
        for (uint32_t i = 1; i < laneCount; ++i) SubmitThreadpoolWork(work);
        run();
        WaitForThreadpoolWorkCallbacks(work, FALSE);
        CloseThreadpoolWork(work);
        for (auto result : results) if (result != VN_OK) return result;
      } else {
        status = prepareRange(0, count, names, staging, retained);
        if (status != VN_OK) return status;
      }
    } else {
      status = prepareRange(0, count, names, staging, retained);
      if (status != VN_OK) return status;
    }
    VisitScope scope(owners, images, rows);
    harken::VorbisSetupBatch::Scope setupScope(warm ? &setupBatch : nullptr);
    submitted = true;
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

extern "C" VnResult Vn_VisitResidentPackDirectoryV1(const VnResidentPackVisitDescV1 *desc) {
  bool submitted = false;
  const auto result = VisitDirectory(desc, true, true, submitted);
  if (result == VN_PACK_RESIDENT_CAPACITY && !submitted)
    // Admit tight inputs using one temporary image at a time. The visitor has not run yet.
    return VisitDirectory(desc, false, false, submitted);
  return result;
}

VnResult harken::VisitDirectoryForActivation(const VnResidentPackVisitDescV1* desc) {
  if (desc && desc->memberCount >= 8) {
    bool submitted = false;
    const auto result = VisitDirectory(desc, true, true, submitted, true);
    if (result != VN_PACK_RESIDENT_CAPACITY || submitted) return result;
  }
  return Vn_VisitResidentPackDirectoryV1(desc);
}
