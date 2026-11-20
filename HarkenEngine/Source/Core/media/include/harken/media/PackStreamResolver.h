#ifndef HARKEN_MEDIA_PACK_STREAM_RESOLVER_H
#define HARKEN_MEDIA_PACK_STREAM_RESOLVER_H

#include <harken/Harken.h>
#include "harken/media/StreamByteSource.h"
#include <string>

namespace harken {

// Shared by locators belonging to one immutable Pack generation.
class PackStreamResolver {
public:
    static VnResult Capture(const VnPackStreamResolverV1& input,
        std::shared_ptr<const PackStreamResolver>* out);
    ~PackStreamResolver();
    const VnPackStreamResolverV1& Descriptor() const noexcept { return descriptor_; }
private:
    VnPackStreamResolverV1 descriptor_{};
    bool retained_ = false;
};

// Destruction is inert until the caller transfers the source lease. A host
// context, when present, stays alive through the source's final release.
class HostStreamByteSource final : public StreamByteSource {
public:
    explicit HostStreamByteSource(const VnByteSourceDescV1& input,
        std::shared_ptr<const PackStreamResolver> context = {})
        : descriptor_(input), context_(std::move(context)) {}
    ~HostStreamByteSource() override;
    void Adopt() noexcept { armed_.store(true, std::memory_order_release); }
    std::uint64_t ByteCount() const noexcept override { return descriptor_.byteLength; }
    StreamReadResult ReadAt(std::uint64_t offset, void* destination,
        std::size_t bytes, StreamReadCancellation cancellation) const override;
private:
    VnByteSourceDescV1 descriptor_;
    std::shared_ptr<const PackStreamResolver> context_;
    std::atomic<bool> armed_{false};
};

std::shared_ptr<const StreamByteLocator> MakePackFileStreamLocator(std::string path,
    std::shared_ptr<const PackStreamResolver> context = {});
std::shared_ptr<const StreamByteLocator> MakePackHostStreamLocator(
    std::shared_ptr<const PackStreamResolver> context, std::uint64_t packStableId,
    std::string relativeLocator, std::uint64_t revision, std::uint64_t bytes);

} // namespace harken
#endif
