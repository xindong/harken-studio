#ifndef HARKEN_MEDIA_STREAM_BYTE_SOURCE_H
#define HARKEN_MEDIA_STREAM_BYTE_SOURCE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace harken {

struct StreamReadCancellation {
    const std::atomic<bool>* requested = nullptr;
    bool IsCancelled() const noexcept {
        return requested != nullptr && requested->load(std::memory_order_acquire);
    }
};

enum class StreamReadStatus { Ok, Cancelled, IoError };
struct StreamReadResult {
    StreamReadStatus status = StreamReadStatus::IoError;
    std::size_t bytes = 0;
};

// An immutable opened byte identity. Reads of the same source may overlap;
// implementations must not share a mutable seek position or reenter Runtime.
// Destination and cancellation are borrowed only for the duration of ReadAt.
class StreamByteSource {
public:
    virtual ~StreamByteSource() = default;
    virtual std::uint64_t ByteCount() const noexcept = 0;
    virtual StreamReadResult ReadAt(std::uint64_t offset, void* destination,
        std::size_t bytes, StreamReadCancellation cancellation) const = 0;
};

// An immutable locator retains host context without opening a media handle.
// Open and destruction of the returned source run only on I/O executors.
class StreamByteLocator {
public:
    virtual ~StreamByteLocator() = default;
    virtual std::shared_ptr<const StreamByteSource> Open(StreamReadCancellation cancellation) const = 0;
};

StreamReadStatus ReadStreamBytes(const StreamByteSource& source,
    std::uint64_t offset, void* destination, std::size_t bytes,
    StreamReadCancellation cancellation);

// Opens once and obtains length from that same handle. Called on an I/O lane.
std::shared_ptr<const StreamByteSource> OpenStreamByteSource(const char* utf8Path);

} // namespace harken
#endif
