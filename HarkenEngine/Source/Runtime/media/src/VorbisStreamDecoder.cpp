#include "harken/media/VorbisStreamDecoder.h"
#include "harken/core/ChannelLayout.h"

#include <vorbis/vorbisfile.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace harken {
struct VorbisStreamDecoder::State { OggVorbis_File file{}; };

VorbisStreamDecoder::VorbisStreamDecoder(const StreamByteSource& source,
    std::uint64_t offset, std::uint64_t bytes, StreamReadWindow& window)
    : m_source(source), m_window(window), m_offset(offset), m_bytes(bytes), m_state(std::make_unique<State>()) {}

VorbisStreamDecoder::~VorbisStreamDecoder() {
    if (m_open) ov_clear(&m_state->file);
}

bool VorbisStreamDecoder::Cancelled() noexcept {
    if (m_cancellation.IsCancelled()) m_failure = Failure::Cancelled;
    return m_failure != Failure::None;
}

std::size_t VorbisStreamDecoder::ReadCallback(void* output, std::size_t size,
    std::size_t count, void* context) noexcept {
    auto& d = *static_cast<VorbisStreamDecoder*>(context);
    if (size == 0 || count == 0 || d.Cancelled()) return 0;
    if (count > std::numeric_limits<std::size_t>::max() / size) {
        d.m_failure = Failure::Format;
        return 0;
    }
    const auto wanted = static_cast<std::size_t>(std::min<std::uint64_t>(
        size * count, d.m_bytes - d.m_position));
    std::size_t copied = 0;
    try {
        auto& w = d.m_window;
        while (copied < wanted && !d.Cancelled()) {
            if (d.m_position < w.offset || d.m_position - w.offset >= w.validBytes) {
                w.validBytes = 0;
                w.offset = d.m_position;
                const auto read = static_cast<std::size_t>(std::min<std::uint64_t>(
                    StreamReadWindow::kCapacityBytes, d.m_bytes - d.m_position));
                w.bytes.resize(read);
                w.capacityBytes.store(w.bytes.capacity(), std::memory_order_release);
                const auto result = ReadStreamBytes(d.m_source, d.m_offset + d.m_position,
                    w.bytes.data(), read, d.m_cancellation);
                if (result != StreamReadStatus::Ok) {
                    d.m_failure = result == StreamReadStatus::Cancelled ? Failure::Cancelled : Failure::Io;
                    return 0;
                }
                w.validBytes = read;
            }
            const auto within = static_cast<std::size_t>(d.m_position - w.offset);
            const auto n = std::min(wanted - copied, w.validBytes - within);
            std::memcpy(static_cast<std::uint8_t*>(output) + copied, w.bytes.data() + within, n);
            copied += n;
            d.m_position += n;
        }
    } catch (...) { d.m_failure = Failure::Io; return 0; }
    return copied / size;
}

int VorbisStreamDecoder::SeekCallback(void* context, std::int64_t offset, int origin) noexcept {
    auto& d = *static_cast<VorbisStreamDecoder*>(context);
    if (d.Cancelled()) return -1;
    std::int64_t base = 0;
    if (origin == SEEK_CUR) base = static_cast<std::int64_t>(d.m_position);
    else if (origin == SEEK_END) base = static_cast<std::int64_t>(d.m_bytes);
    else if (origin != SEEK_SET) return -1;
    if (offset < -base || offset > static_cast<std::int64_t>(d.m_bytes) - base) return -1;
    d.m_position = static_cast<std::uint64_t>(base + offset);
    d.m_lastSeek = d.m_position;
    return 0;
}

long VorbisStreamDecoder::TellCallback(void* context) noexcept {
    auto& d = *static_cast<VorbisStreamDecoder*>(context);
    return d.Cancelled() ? -1 : static_cast<long>(d.m_position);
}

bool VorbisStreamDecoder::CheckEndPage() {
    // A seekable decoder can infer a shorter length from a truncated file.
    // Require the final physical page to carry EOS, its CRC and the declared
    // granule position. A single Ogg page is at most 65307 bytes.
    const auto tailBytes = static_cast<std::size_t>(std::min<std::uint64_t>(m_bytes, 65307));
    std::vector<unsigned char> tail(tailBytes);
    const auto result = ReadStreamBytes(m_source, m_offset + m_bytes - tailBytes,
        tail.data(), tailBytes, m_cancellation);
    if (result != StreamReadStatus::Ok) {
        m_failure = result == StreamReadStatus::Cancelled ? Failure::Cancelled : Failure::Io;
        return false;
    }
    ogg_sync_state sync{};
    ogg_sync_init(&sync);
    char* bytes = ogg_sync_buffer(&sync, static_cast<long>(tailBytes));
    if (bytes == nullptr) { ogg_sync_clear(&sync); return false; }
    std::memcpy(bytes, tail.data(), tailBytes);
    ogg_sync_wrote(&sync, static_cast<long>(tailBytes));
    ogg_page page{};
    bool valid = false;
    int pageResult = 0;
    while ((pageResult = ogg_sync_pageout(&sync, &page)) != 0) {
        if (pageResult == 1 && page.header != nullptr && page.body != nullptr &&
            page.body + page.body_len == sync.data + sync.fill &&
            ogg_page_eos(&page) && ogg_page_serialno(&page) == ov_serialnumber(&m_state->file, 0) &&
            ogg_page_granulepos(&page) >= static_cast<ogg_int64_t>(m_frames)) valid = true;
    }
    ogg_sync_clear(&sync);
    return valid;
}

bool VorbisStreamDecoder::Open(std::uint32_t rate, std::uint32_t channels,
    std::uint64_t frames, StreamReadCancellation cancellation) {
    m_cancellation = cancellation;
    if (m_open || Cancelled()) return false;
    if (m_bytes > kMaxEncodedBytes || LayoutFromChannelCount(channels) == VN_CHANNEL_LAYOUT_DEFAULT ||
        frames > static_cast<std::uint64_t>(INT64_MAX)) {
        m_failure = Failure::Unsupported; return false;
    }
    if (m_bytes == 0 || m_offset > m_source.ByteCount() ||
        m_bytes > m_source.ByteCount() - m_offset) {
        m_failure = Failure::Format; return false;
    }
    m_window.validBytes = 0;
    const ov_callbacks callbacks{ReadCallback, SeekCallback, nullptr, TellCallback};
    const auto result = ov_open_callbacks(this, &m_state->file, nullptr, 0, callbacks);
    if (result != 0) {
        if (m_failure == Failure::None) m_failure = Failure::Format;
        return false;
    }
    m_open = true;
    m_channels = channels;
    m_frames = frames;
    const auto* info = ov_info(&m_state->file, 0);
    if (Cancelled() || ov_streams(&m_state->file) != 1 || !info || info->channels != static_cast<int>(channels) ||
        info->rate != static_cast<long>(rate) || ov_pcm_total(&m_state->file, -1) != static_cast<ogg_int64_t>(frames) ||
        !CheckEndPage()) {
        if (m_failure == Failure::None) m_failure = Failure::Format;
        return false;
    }
    return true;
}

bool VorbisStreamDecoder::ReadFrames(std::uint32_t count, float* planar, std::uint32_t stride) {
    // Vorbis order differs from the canonical SMPTE order for surround.
    static constexpr unsigned map6[]{0, 2, 1, 5, 3, 4};
    static constexpr unsigned map8[]{0, 2, 1, 7, 5, 6, 3, 4};
    std::uint32_t done = 0;
    while (done < count && !Cancelled()) {
        float** pcm = nullptr;
        int link = 0;
        const long n = ov_read_float(&m_state->file, &pcm, static_cast<int>(count - done), &link);
        if (Cancelled()) return false;
        if (n <= 0 || link != 0) { m_failure = Failure::Format; return false; }
        for (unsigned ch = 0; ch < m_channels; ++ch) {
            const unsigned from = m_channels == 6 ? map6[ch] : m_channels == 8 ? map8[ch] : ch;
            for (long i = 0; i < n; ++i) {
                const float value = pcm[from][i];
                if (!std::isfinite(value)) { m_failure = Failure::Format; return false; }
                if (planar) planar[static_cast<std::size_t>(ch) * stride + done + i] = value;
            }
        }
        done += static_cast<std::uint32_t>(n);
    }
    return done == count && !Cancelled();
}

bool VorbisStreamDecoder::Read(std::uint64_t frame, std::uint32_t count, float* planar,
    std::uint32_t stride, StreamReadCancellation cancellation) {
    m_cancellation = cancellation;
    if (!m_open || Cancelled() || frame > m_frames || count > m_frames - frame || stride < count) return false;
    if (ov_pcm_tell(&m_state->file) != static_cast<ogg_int64_t>(frame)) {
        const bool sought = frame == m_loopFrame ? SeekLoop() :
            ov_pcm_seek(&m_state->file, static_cast<ogg_int64_t>(frame)) == 0;
        if (!sought || Cancelled() ||
            ov_pcm_tell(&m_state->file) != static_cast<ogg_int64_t>(frame)) {
            if (m_failure == Failure::None) m_failure = Failure::Format;
            return false;
        }
    }
    return ReadFrames(count, planar, stride);
}

bool VorbisStreamDecoder::SeekLoop() {
    if (ov_raw_seek(&m_state->file, static_cast<ogg_int64_t>(m_loopRaw)) != 0 || Cancelled()) return false;
    const auto position = ov_pcm_tell(&m_state->file);
    if (position < 0 || static_cast<std::uint64_t>(position) > m_loopFrame) return false;
    auto discard = m_loopFrame - static_cast<std::uint64_t>(position);
    while (discard != 0) {
        const auto n = static_cast<std::uint32_t>(std::min<std::uint64_t>(discard, 4096));
        if (!ReadFrames(n, nullptr, n)) return false;
        discard -= n;
    }
    return true;
}

bool VorbisStreamDecoder::PrepareLoop(std::uint64_t frame, StreamReadCancellation cancellation) {
    m_cancellation = cancellation;
    if (!m_open || frame >= m_frames || Cancelled()) return false;
    // Validate a replay point against the library's exact seek. One full long
    // block covers the overlap dependency; later synthesis is deterministic.
    const auto block = static_cast<std::uint64_t>(vorbis_info_blocksize(ov_info(&m_state->file, 0), 1));
    const auto count = static_cast<std::uint32_t>(std::min(m_frames - frame, block));
    std::vector<float> exact(static_cast<std::size_t>(count) * m_channels);
    std::vector<float> replay(exact.size());
    if (!Read(frame, count, exact.data(), count, cancellation)) return false;
    auto preRoll = frame > 2 * block ? frame - 2 * block : 0;
    for (;;) {
        if (ov_pcm_seek(&m_state->file, static_cast<ogg_int64_t>(preRoll)) != 0 || Cancelled()) return false;
        // This is the actual byte seek made by libvorbisfile, not raw_tell
        // after decoding (which includes pages already buffered internally).
        m_loopRaw = m_lastSeek;
        m_loopFrame = frame;
        const bool positioned = SeekLoop();
        if (positioned && ReadFrames(count, replay.data(), count) && replay == exact) return true;
        if (Cancelled()) return false;
        m_loopFrame = UINT64_MAX;
        if (preRoll == 0) { m_failure = Failure::Format; return false; }
        // A continued packet can need an earlier complete page. Preparation
        // alone searches farther back; steady loops reuse the proven anchor.
        preRoll /= 2;
    }
}

} // namespace harken
