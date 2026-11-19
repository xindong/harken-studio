#if !defined(_WIN32) && !defined(_FILE_OFFSET_BITS)
#define _FILE_OFFSET_BITS 64
#endif
#include "harken/media/StreamByteSource.h"
#include "harken/media/PackStreamResolver.h"
#include "harken/core/Utf8Path.h"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
static_assert(sizeof(off_t) >= 8, "Stream sources require 64-bit file offsets");
#endif

namespace harken {

namespace {
class ByteRangeSource final : public StreamByteSource {
public:
    ByteRangeSource(std::shared_ptr<const StreamByteSource> source, std::uint64_t offset, std::uint64_t bytes)
        : source_(std::move(source)), offset_(offset), bytes_(bytes) {}
    std::uint64_t ByteCount() const noexcept override { return bytes_; }
    StreamReadResult ReadAt(std::uint64_t offset, void* destination, std::size_t bytes,
        StreamReadCancellation cancellation) const override {
        if (cancellation.IsCancelled()) return {StreamReadStatus::Cancelled, 0};
        if (offset > bytes_ || bytes > bytes_ - offset || (bytes != 0 && !destination)) return {};
        return source_->ReadAt(offset_ + offset, destination, bytes, cancellation);
    }
private:
    std::shared_ptr<const StreamByteSource> source_;
    std::uint64_t offset_, bytes_;
};
}

std::shared_ptr<const StreamByteSource> StreamByteRange(
    std::shared_ptr<const StreamByteSource> source, std::uint64_t offset, std::uint64_t bytes) {
    if (!source || offset > source->ByteCount() || bytes > source->ByteCount() - offset) return {};
    return std::make_shared<ByteRangeSource>(std::move(source), offset, bytes);
}

StreamReadStatus ReadStreamBytes(const StreamByteSource& source,
    std::uint64_t offset, void* destination, std::size_t bytes,
    StreamReadCancellation cancellation) {
    const auto length = source.ByteCount();
    if (offset > length || bytes > length - offset || (bytes != 0 && !destination))
        return StreamReadStatus::IoError;
    auto* output = static_cast<std::uint8_t*>(destination);
    while (bytes != 0) {
        if (cancellation.IsCancelled()) return StreamReadStatus::Cancelled;
        const auto result = source.ReadAt(offset, output, bytes, cancellation);
        if (result.status != StreamReadStatus::Ok) return result.status;
        if (result.bytes == 0 || result.bytes > bytes) return StreamReadStatus::IoError;
        offset += result.bytes;
        output += result.bytes;
        bytes -= result.bytes;
    }
    return cancellation.IsCancelled() ? StreamReadStatus::Cancelled : StreamReadStatus::Ok;
}

namespace {
#ifdef _WIN32
struct ReadEvent {
    HANDLE handle = nullptr;
    ~ReadEvent() { if (handle != nullptr) CloseHandle(handle); }
};
#endif
class FileSource final : public StreamByteSource {
public:
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~FileSource() override { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
#else
    int handle = -1;
    ~FileSource() override { if (handle != -1) close(handle); }
#endif
    std::uint64_t length = 0;
    std::uint64_t ByteCount() const noexcept override { return length; }
    StreamReadResult ReadAt(std::uint64_t offset, void* destination,
        std::size_t bytes, StreamReadCancellation cancellation) const override {
        if (offset > length || bytes > length - offset) return {};
        if (cancellation.IsCancelled()) return {StreamReadStatus::Cancelled, 0};
        if (bytes == 0) return {StreamReadStatus::Ok, 0};
#ifdef _WIN32
        // Each executing thread has only one synchronous operation at a time.
        // Its event is reusable only because completion is drained below.
        thread_local ReadEvent event;
        if (event.handle == nullptr) event.handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (event.handle == nullptr || !ResetEvent(event.handle)) return {};
        OVERLAPPED operation{};
        operation.Offset = static_cast<DWORD>(offset);
        operation.OffsetHigh = static_cast<DWORD>(offset >> 32u);
        operation.hEvent = event.handle;
        DWORD read = 0;
        const auto count = static_cast<DWORD>(std::min<std::size_t>(bytes, MAXDWORD));
        BOOL ok = ReadFile(handle, destination, count, &read, &operation);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            while (WaitForSingleObject(operation.hEvent, 2) == WAIT_TIMEOUT) {
                if (cancellation.IsCancelled()) {
                    CancelIoEx(handle, &operation);
                    break;
                }
            }
            // Cancellation is a request, not proof the kernel released the buffer.
            ok = GetOverlappedResult(handle, &operation, &read, TRUE);
        }
        if (cancellation.IsCancelled()) return {StreamReadStatus::Cancelled, 0};
        return ok ? StreamReadResult{StreamReadStatus::Ok, read} : StreamReadResult{};
#else
        if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) return {};
        const auto count = std::min<std::size_t>(bytes, std::numeric_limits<ssize_t>::max());
        ssize_t read;
        do {
            if (cancellation.IsCancelled()) return {StreamReadStatus::Cancelled, 0};
            read = pread(handle, destination, count, static_cast<off_t>(offset));
        } while (read < 0 && errno == EINTR);
        return read >= 0 ? StreamReadResult{StreamReadStatus::Ok, static_cast<std::size_t>(read)}
                         : StreamReadResult{};
#endif
    }
};
}

std::shared_ptr<const StreamByteSource> OpenStreamByteSource(const char* utf8Path) {
    std::filesystem::path path;
    if (!TryMakeUtf8Path(utf8Path, &path)) return {};
    auto source = std::make_shared<FileSource>();
#ifdef _WIN32
    source->handle = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    LARGE_INTEGER length{};
    if (source->handle == INVALID_HANDLE_VALUE || !GetFileSizeEx(source->handle, &length) ||
        length.QuadPart < 0) return {};
    source->length = static_cast<std::uint64_t>(length.QuadPart);
#else
    source->handle = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    struct stat facts{};
    if (source->handle == -1 || fstat(source->handle, &facts) != 0 || facts.st_size < 0) return {};
    source->length = static_cast<std::uint64_t>(facts.st_size);
#endif
    return source;
}

} // namespace harken

namespace harken {
namespace {
bool SafeFilePath(const std::string& text) {
    std::filesystem::path path;
    if (!TryMakeUtf8Path(text.c_str(), &path) || !path.is_absolute()) return false;
    std::filesystem::path component;
    for (const auto& part : path) {
        if (part == "..") return false;
        component /= part;
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(component.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
#else
        std::error_code error;
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(component, error))) return false;
#endif
    }
    return true;
}
std::uint32_t IsCancelled(const void* context) {
    return static_cast<const StreamReadCancellation*>(context)->IsCancelled() ? 1u : 0u;
}
class FileLocator final : public StreamByteLocator {
public:
    FileLocator(std::string path, std::shared_ptr<const PackStreamResolver> context)
        : path_(std::move(path)), context_(std::move(context)) {}
    std::shared_ptr<const StreamByteSource> Open(StreamReadCancellation cancellation) const override {
        if (cancellation.IsCancelled() || !SafeFilePath(path_)) return {};
        auto source = OpenStreamByteSource(path_.c_str());
        return cancellation.IsCancelled() ? nullptr : source;
    }
private:
    std::string path_;
    std::shared_ptr<const PackStreamResolver> context_;
};
class HostLocator final : public StreamByteLocator {
public:
    HostLocator(std::shared_ptr<const PackStreamResolver> context, std::uint64_t pack,
        std::string locator, std::uint64_t revision, std::uint64_t bytes)
        : context_(std::move(context)), pack_(pack), locator_(std::move(locator)),
          revision_(revision), bytes_(bytes) {}
    std::shared_ptr<const StreamByteSource> Open(StreamReadCancellation cancellation) const override {
        if (cancellation.IsCancelled()) return {};
        VnByteSourceDescV1 result{};
        result.structSize = sizeof(result);
        VnReadCancellationViewV1 view{sizeof(view), 0, &cancellation, &IsCancelled};
        const auto& host = context_->Descriptor();
        try {
            if (host.open(host.userData, pack_, locator_.c_str(), revision_, bytes_, &view, &result) != VN_OK)
                return {};
        } catch (...) { return {}; }
        // Ownership transfers on successful Open, before validating the shape
        // or allocating the C++ wrapper. Every later refusal still releases it.
        struct Lease {
            VnByteSourceDescV1* value;
            ~Lease() { if (value && value->release) { try { value->release(value->userData); } catch (...) {} } }
        } lease{&result};
        if (result.structSize != sizeof(result) || result.flags || !result.readAt || !result.release ||
            result.byteLength != bytes_ || cancellation.IsCancelled()) return {};
        auto source = std::make_shared<HostStreamByteSource>(result, context_);
        source->Adopt();
        lease.value = nullptr;
        return source;
    }
private:
    std::shared_ptr<const PackStreamResolver> context_;
    std::uint64_t pack_;
    std::string locator_;
    std::uint64_t revision_, bytes_;
};
}

VnResult PackStreamResolver::Capture(const VnPackStreamResolverV1& input,
    std::shared_ptr<const PackStreamResolver>* out) {
    if (!out || *out || input.structSize != sizeof(input) || input.flags ||
        !input.retain || !input.release || (input.path == nullptr) == (input.open == nullptr))
        return VN_INVALID_ARG;
    auto captured = std::make_shared<PackStreamResolver>();
    captured->descriptor_ = input;
    VnResult result;
    try { result = input.retain(input.userData); } catch (...) { return VN_PACK_PROVIDER_FAILED; }
    if (result != VN_OK) return result;
    captured->retained_ = true;
    *out = std::move(captured);
    return VN_OK;
}
PackStreamResolver::~PackStreamResolver() {
    if (retained_) { try { descriptor_.release(descriptor_.userData); } catch (...) {} }
}
HostStreamByteSource::~HostStreamByteSource() {
    if (armed_.load(std::memory_order_acquire)) {
        try { descriptor_.release(descriptor_.userData); } catch (...) {}
    }
}
StreamReadResult HostStreamByteSource::ReadAt(std::uint64_t offset, void* destination,
    std::size_t count, StreamReadCancellation cancellation) const {
    if (cancellation.IsCancelled()) return {StreamReadStatus::Cancelled, 0};
    if (offset > descriptor_.byteLength || count > descriptor_.byteLength - offset ||
        (count && !destination)) return {};
    VnReadCancellationViewV1 view{sizeof(view), 0, &cancellation, &IsCancelled};
    std::uint32_t read = 0;
    try {
        const auto requested = static_cast<std::uint32_t>(std::min<std::size_t>(count, UINT32_MAX));
        const auto result = descriptor_.readAt(descriptor_.userData, offset, destination, requested, &read, &view);
        if (cancellation.IsCancelled() || result == VN_BYTE_READ_CANCELLED)
            return {StreamReadStatus::Cancelled, 0};
        if (result != VN_BYTE_READ_OK || read > requested) return {};
        return {StreamReadStatus::Ok, read};
    } catch (...) { return {}; }
}
std::shared_ptr<const StreamByteLocator> MakePackFileStreamLocator(std::string path,
    std::shared_ptr<const PackStreamResolver> context) {
    if (!SafeFilePath(path)) return {};
    return std::make_shared<FileLocator>(std::move(path), std::move(context));
}
std::shared_ptr<const StreamByteLocator> MakePackHostStreamLocator(
    std::shared_ptr<const PackStreamResolver> context, std::uint64_t packStableId,
    std::string relativeLocator, std::uint64_t revision, std::uint64_t bytes) {
    if (!context || !context->Descriptor().open) return {};
    return std::make_shared<HostLocator>(std::move(context), packStableId,
        std::move(relativeLocator), revision, bytes);
}
} // namespace harken
