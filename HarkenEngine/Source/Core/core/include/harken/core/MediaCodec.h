#ifndef HARKEN_CORE_MEDIA_CODEC_H
#define HARKEN_CORE_MEDIA_CODEC_H
#include <cstddef>
#include <cstdint>
#include <span>

namespace harken {
struct PCMView;

// A prepared voice owns one decoder. Read returns a borrowed planar span,
// valid until the next read; retirement never frees memory on render.
class MediaDecoder {
public:
    virtual bool Read(std::uint64_t frame, std::uint32_t frames,
                      std::uint32_t renderThread, PCMView& output) noexcept = 0;
    virtual void Retire() noexcept = 0;
protected:
    ~MediaDecoder() = default;
};

// Immutable media is pinned by the registry's existing generation use count.
// Acquisition runs only on logic, before a voice can be published.
class EncodedMedia {
public:
    virtual ~EncodedMedia() = default;
    virtual MediaDecoder* Acquire() const noexcept = 0;
    virtual std::size_t ResidentBytes() const noexcept = 0;
    // Complete self-describing bytes prove exact content identity on logic.
    virtual std::span<const std::uint8_t> ContentBytes() const noexcept = 0;
};
}
#endif
