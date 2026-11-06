#if !defined(_WIN32) && !defined(_FILE_OFFSET_BITS)
#define _FILE_OFFSET_BITS 64
#endif
#include "harken/media/StreamByteSource.h"
#include "harken/core/Utf8Path.h"

#include <algorithm>
#include <filesystem>
#include <limits>

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
