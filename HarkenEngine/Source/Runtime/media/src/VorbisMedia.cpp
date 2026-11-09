#include "harken/media/VorbisMedia.h"
#include "harken/media/PackFormat.h"
#include <algorithm>
#include <cstring>
#include <utility>

namespace harken {
namespace {
struct Reader {
    const std::uint8_t* p;
    std::size_t remaining;
    bool valid = true;
    std::uint64_t Read(unsigned size) noexcept {
        if (size > remaining) { valid = false; return 0; }
        std::uint64_t value = 0;
        for (unsigned i = 0; i < size; ++i) value |= std::uint64_t(p[i]) << (8 * i);
        p += size; remaining -= size; return value;
    }
    bool Take(std::size_t size, const std::uint8_t*& out) noexcept {
        if (size > remaining) return false;
        out = p; p += size; remaining -= size; return true;
    }
};
bool BlockSize(std::uint16_t n) noexcept {
    return n >= 64 && n <= 8192 && (n & (n - 1)) == 0;
}
}

bool VorbisMedia::Packet(std::uint64_t offset, const std::uint8_t*& data,
                         std::uint16_t& bytes) const noexcept {
    if (offset > packetBytes || packetBytes - offset < 2) return false;
    bytes = static_cast<std::uint16_t>(packets[offset] | (packets[offset + 1] << 8));
    if (!bytes || bytes > packetBytes - offset - 2) return false;
    data = packets + offset + 2;
    return true;
}

const VorbisSeekPoint* VorbisMedia::Seek(std::uint64_t frame) const noexcept {
    if (frame >= frames || !seekGranularity || seek.empty()) return nullptr;
    const auto index = frame / seekGranularity;
    return index < seek.size() ? &seek[static_cast<std::size_t>(index)] : nullptr;
}

bool VorbisMedia::VerifySegment(std::size_t index) const noexcept {
    if (index >= segmentChecksums.size()) return false;
    const auto begin = seek[index].packetOffset;
    const auto end = index + 1 < seek.size() ? seek[index + 1].packetOffset : packetBytes;
    return Fnv1a64(packets + begin, static_cast<std::size_t>(end - begin)) == segmentChecksums[index];
}

bool ParseVorbisTestStream(const void* data, std::size_t bytes, VorbisMedia& output) {
    if (!data || bytes < vorbis_test_wire::kStreamHeaderBytes) return false;
    Reader r{static_cast<const std::uint8_t*>(data), bytes};
    if (r.Read(4) != vorbis_test_wire::kStreamMagic || r.Read(2) != vorbis_test_wire::kVersion ||
        r.Read(2) != 1 || r.Read(8) != vorbis_test_wire::kStreamHeaderBytes ||
        r.Read(8) != bytes - vorbis_test_wire::kStreamHeaderBytes) return false;
    return ParseVorbisTestMedia(r.p, r.remaining, output);
}

bool ParseVorbisTestMedia(const void* data, std::size_t bytes, VorbisMedia& output) {
    if (!data || bytes < vorbis_test_wire::kHeaderBytes) return false;
    Reader r{static_cast<const std::uint8_t*>(data), bytes};
    if (r.Read(4) != vorbis_test_wire::kMediaMagic || r.Read(2) != vorbis_test_wire::kVersion)
        return false;
    const auto flags = r.Read(2);
    VorbisMedia m;
    m.sampleRate = static_cast<std::uint32_t>(r.Read(4));
    m.channels = static_cast<std::uint16_t>(r.Read(2));
    if (r.Read(2) != 0 || flags > 1 || !m.sampleRate ||
        (m.channels != 1 && m.channels != 2 && m.channels != 4 && m.channels != 6 && m.channels != 8))
        return false;
    m.frames = r.Read(8);
    m.blockShort = static_cast<std::uint16_t>(r.Read(2));
    m.blockLong = static_cast<std::uint16_t>(r.Read(2));
    m.identBytes = static_cast<std::uint32_t>(r.Read(4));
    m.setupBytes = static_cast<std::uint32_t>(r.Read(4));
    m.seekGranularity = static_cast<std::uint32_t>(r.Read(4));
    const auto seekCount = r.Read(4);
    m.loopStart = r.Read(8); m.loopEnd = r.Read(8);
    m.loopStartPacket = r.Read(8); m.loopDiscard = static_cast<std::uint32_t>(r.Read(4));
    m.loopEndPacket = r.Read(8); m.loopTrim = static_cast<std::uint32_t>(r.Read(4));
    m.packetBytes = r.Read(8); m.revision = r.Read(8);
    const auto checksum = r.Read(8), segmentCount = r.Read(4);
    if (!r.valid || !m.frames || !BlockSize(m.blockShort) || !BlockSize(m.blockLong) ||
        m.blockShort > m.blockLong || !m.seekGranularity ||
        seekCount != 1 + (m.frames - 1) / m.seekGranularity ||
        (segmentCount != 0 && segmentCount != seekCount) || m.identBytes != 30 || m.setupBytes < 7)
        return false;
    if (flags ? (m.loopStart >= m.loopEnd || m.loopEnd > m.frames) :
        (m.loopStart || m.loopEnd || m.loopStartPacket || m.loopDiscard || m.loopEndPacket || m.loopTrim))
        return false;
    if (!r.Take(m.identBytes, m.ident) || !r.Take(m.setupBytes, m.setup)) return false;
    if (m.ident[0] != 1 || std::memcmp(m.ident + 1, "vorbis", 6) ||
        m.setup[0] != 5 || std::memcmp(m.setup + 1, "vorbis", 6)) return false;
    Reader ident{m.ident + 7, 23};
    if (ident.Read(4) != 0 || ident.Read(1) != m.channels || ident.Read(4) != m.sampleRate ||
        (1u << (m.ident[28] & 15)) != m.blockShort ||
        (1u << (m.ident[28] >> 4)) != m.blockLong || m.ident[29] != 1) return false;
    // Bound every count by the supplied bytes before allocating any index.
    const auto indexBytes = seekCount * vorbis_test_wire::kSeekBytes + segmentCount * 8;
    if (indexBytes > r.remaining || m.packetBytes != r.remaining - indexBytes) return false;
    m.seek.reserve(static_cast<std::size_t>(seekCount));
    for (std::uint64_t i = 0; i < seekCount; ++i) {
        VorbisSeekPoint s;
        s.frame = r.Read(8); s.packetOffset = r.Read(8);
        s.discardFrames = static_cast<std::uint32_t>(r.Read(4));
        if (s.frame != i * m.seekGranularity || s.packetOffset >= m.packetBytes ||
            (i && s.packetOffset < m.seek.back().packetOffset) ||
            s.discardFrames > m.blockLong / 2 || (i == 0 && (s.packetOffset || s.discardFrames)))
            return false;
        m.seek.push_back(s);
    }
    for (std::uint64_t i = 0; i < segmentCount; ++i) m.segmentChecksums.push_back(r.Read(8));
    m.packets = r.p;
    if (Fnv1a64(m.packets, static_cast<std::size_t>(m.packetBytes)) != checksum) return false;
    std::size_t seekIndex = 0;
    bool loopStartFound = !flags, loopEndFound = !flags;
    for (std::uint64_t at = 0; at < m.packetBytes;) {
        const std::uint8_t* packet = nullptr; std::uint16_t count = 0;
        if (!m.Packet(at, packet, count) || (packet[0] & 1)) return false;
        while (seekIndex < m.seek.size() && m.seek[seekIndex].packetOffset == at) ++seekIndex;
        if (at == m.loopStartPacket) loopStartFound = true;
        if (at == m.loopEndPacket) loopEndFound = true;
        at += 2 + count;
    }
    if (seekIndex != m.seek.size() || !loopStartFound || !loopEndFound ||
        m.loopDiscard > m.blockLong / 2 || m.loopTrim > m.blockLong / 2) return false;
    for (std::size_t i = 0; i < m.segmentChecksums.size(); ++i)
        if (!m.VerifySegment(i)) return false;
    output = std::move(m);
    return true;
}
} // namespace harken
