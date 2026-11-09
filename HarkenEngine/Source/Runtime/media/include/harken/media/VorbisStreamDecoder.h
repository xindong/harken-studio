#ifndef HARKEN_MEDIA_VORBIS_STREAM_DECODER_H
#define HARKEN_MEDIA_VORBIS_STREAM_DECODER_H

#include "harken/media/StreamByteSource.h"


namespace harken {

// One slot owns one decoder. All methods, including destruction, run on I/O
// lanes; the manager transfers ownership only after the previous job settles.
class VorbisStreamDecoder {
public:
    enum class Failure { None, Cancelled, Io, Format, Unsupported };
    static constexpr std::uint64_t kMaxEncodedBytes = INT32_MAX;
    VorbisStreamDecoder(const StreamByteSource& source, std::uint64_t offset,
                        std::uint64_t bytes, StreamReadWindow& window);
    ~VorbisStreamDecoder();
    VorbisStreamDecoder(const VorbisStreamDecoder&) = delete;
    VorbisStreamDecoder& operator=(const VorbisStreamDecoder&) = delete;

    bool Open(std::uint32_t rate, std::uint32_t channels, std::uint64_t frames,
              StreamReadCancellation cancellation);
    bool PrepareLoop(std::uint64_t frame, StreamReadCancellation cancellation);
    bool Read(std::uint64_t frame, std::uint32_t count, float* planar,
              std::uint32_t stride, StreamReadCancellation cancellation);
    Failure Error() const noexcept { return m_failure; }

    // Intrusive deferred destruction; no decoder work on the manager thread.
    std::unique_ptr<VorbisStreamDecoder> nextCleanup;

private:
    static std::size_t ReadCallback(void*, std::size_t, std::size_t, void*) noexcept;
    static int SeekCallback(void*, std::int64_t, int) noexcept;
    static long TellCallback(void*) noexcept;
    bool Cancelled() noexcept;
    bool CheckEndPage();
    bool SeekLoop();
    bool ReadFrames(std::uint32_t count, float* planar, std::uint32_t stride);
    const StreamByteSource& m_source;
    StreamReadWindow& m_window;
    std::uint64_t m_offset, m_bytes, m_position = 0, m_frames = 0;
    std::uint32_t m_channels = 0;
    std::uint64_t m_lastSeek = 0, m_loopFrame = UINT64_MAX, m_loopRaw = 0;
    StreamReadCancellation m_cancellation;
    Failure m_failure = Failure::None;
    struct State;
    std::unique_ptr<State> m_state;
    bool m_open = false;
};

} // namespace harken
#endif
