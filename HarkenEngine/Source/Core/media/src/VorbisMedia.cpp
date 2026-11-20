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
                         std::uint32_t& bytes) const noexcept {
    if (!packets || offset > packetBytes || packetBytes - offset < packetPrefixBytes) return false;
    bytes = packetPrefixBytes == 4 ? LoadLE32(packets + offset) : LoadLE16(packets + offset);
    if (!bytes || bytes > packetBytes - offset - packetPrefixBytes) return false;
    data = packets + offset + packetPrefixBytes;
    return true;
}

const VorbisSeekPoint* VorbisMedia::Seek(std::uint64_t frame) const noexcept {
    if (frame >= frames || !seekGranularity || seek.empty()) return nullptr;
    const auto index = frame / seekGranularity;
    return index < seek.size() ? &seek[static_cast<std::size_t>(index)] : nullptr;
}

bool VorbisMedia::VerifySegment(std::size_t index) const noexcept {
    if (!packets || index >= segmentChecksums.size()) return false;
    const auto begin = seek[index].packetOffset;
    const auto end = index + 1 < seek.size() ? seek[index + 1].packetOffset : packetBytes;
    return Fnv1a64(packets + begin, static_cast<std::size_t>(end - begin), segmentChecksumSeed) == segmentChecksums[index];
}

bool ParseResidentVorbis(const void* data, std::size_t bytes, VorbisMedia& output,
                        std::size_t indexBudget) {
    if (!data || bytes < sizeof(HkPackVorbisHeader)) return false;
    const auto* header = static_cast<const std::uint8_t*>(data);
    if (LoadLE32(header + offsetof(HkPackVorbisHeader, version)) != HK_PACK_VORBIS_VERSION ||
        LoadLE16(header + offsetof(HkPackVorbisHeader, reserved)) != 0) return false;
    VorbisMedia m;
    m.packetPrefixBytes = 4;
    m.sampleRate = LoadLE32(header + offsetof(HkPackVorbisHeader, sampleRate));
    m.channels = LoadLE16(header + offsetof(HkPackVorbisHeader, channels));
    m.blockShort = LoadLE16(header + offsetof(HkPackVorbisHeader, smallBlockFrames));
    m.blockLong = LoadLE16(header + offsetof(HkPackVorbisHeader, largeBlockFrames));
    m.frames = LoadLE64(header + offsetof(HkPackVorbisHeader, frameCount));
    m.prerollFrames = LoadLE64(header + offsetof(HkPackVorbisHeader, prerollFrames));
    m.tailFrames = LoadLE64(header + offsetof(HkPackVorbisHeader, tailFrames));
    m.loopStart = LoadLE64(header + offsetof(HkPackVorbisHeader, loopStartFrame));
    m.loopEnd = LoadLE64(header + offsetof(HkPackVorbisHeader, loopEndFrame));
    m.identBytes = LoadLE32(header + offsetof(HkPackVorbisHeader, identificationBytes));
    m.setupBytes = LoadLE32(header + offsetof(HkPackVorbisHeader, setupBytes));
    m.seekGranularity = LoadLE32(header + offsetof(HkPackVorbisHeader, seekGrainFrames));
    const auto count = LoadLE32(header + offsetof(HkPackVorbisHeader, seekCount));
    if (!m.sampleRate || !m.frames || m.frames > UINT64_MAX - m.prerollFrames ||
        m.tailFrames > UINT64_MAX - m.prerollFrames - m.frames ||
        (m.channels != 1 && m.channels != 2 && m.channels != 4 && m.channels != 6 && m.channels != 8) ||
        !BlockSize(m.blockShort) || !BlockSize(m.blockLong) || m.blockShort > m.blockLong ||
        m.identBytes != 30 || m.setupBytes < 7 || m.seekGranularity != HK_PACK_VORBIS_SEEK_GRAIN ||
        (m.loopEnd ? (m.loopStart >= m.loopEnd || m.loopEnd > m.frames) : m.loopStart != 0))
        return false;
    const auto gridCount = 1 + (m.frames - 1) / m.seekGranularity;
    const bool extraLoopPoint = m.loopEnd && m.loopStart % m.seekGranularity;
    if (count != gridCount + extraLoopPoint) return false;
    const auto packetDataOffset = sizeof(HkPackVorbisHeader) + std::uint64_t(m.identBytes) + m.setupBytes +
        std::uint64_t(count) * sizeof(HkPackVorbisSeek);
    if (packetDataOffset >= bytes) return false;
    m.ident = header + sizeof(HkPackVorbisHeader);
    m.setup = m.ident + m.identBytes;
    if (m.ident[0] != 1 || std::memcmp(m.ident + 1, "vorbis", 6) ||
        LoadLE32(m.ident + 7) != 0 || m.ident[11] != m.channels || LoadLE32(m.ident + 12) != m.sampleRate ||
        (1u << (m.ident[28] & 15)) != m.blockShort || (1u << (m.ident[28] >> 4)) != m.blockLong ||
        m.ident[29] != 1 || m.setup[0] != 5 || std::memcmp(m.setup + 1, "vorbis", 6)) return false;
    m.packets = header + packetDataOffset;
    m.packetBytes = bytes - packetDataOffset;
    if (gridCount > indexBudget / sizeof(VorbisSeekPoint)) throw std::bad_alloc{};
    m.seek.reserve(static_cast<std::size_t>(gridCount));
    const auto* row = m.setup + m.setupBytes;
    std::uint64_t packet = 0, lastFrame = 0;
    bool foundLoop = !m.loopEnd;
    for (std::uint32_t i = 0; i < count; ++i, row += sizeof(HkPackVorbisSeek)) {
        const auto frame = LoadLE64(row + offsetof(HkPackVorbisSeek, outputFrame));
        const auto offset = LoadLE64(row + offsetof(HkPackVorbisSeek, packetOffset));
        const auto discard = LoadLE32(row + offsetof(HkPackVorbisSeek, discardFrames));
        if ((i && frame <= lastFrame) || frame >= m.frames || offset < packetDataOffset ||
            offset >= bytes || discard > m.blockLong / 2) return false;
        const auto relative = offset - packetDataOffset;
        while (packet < relative) {
            const std::uint8_t* payload; std::uint32_t size;
            if (!m.Packet(packet, payload, size) || (payload[0] & 1)) return false;
            packet += std::uint64_t(size) + 4;
        }
        if (packet != relative) return false;
        if (m.loopEnd && frame == m.loopStart) {
            m.loopStartPacket = relative; m.loopDiscard = discard; foundLoop = true;
        }
        if (frame % m.seekGranularity == 0) {
            if (frame / m.seekGranularity != m.seek.size()) return false;
            m.seek.push_back({frame, relative, discard});
        } else if (!m.loopEnd || frame != m.loopStart) return false;
        lastFrame = frame;
    }
    while (packet < m.packetBytes) {
        const std::uint8_t* payload; std::uint32_t size;
        if (!m.Packet(packet, payload, size) || (payload[0] & 1)) return false;
        packet += std::uint64_t(size) + 4;
    }
    if (!foundLoop || m.seek.size() != gridCount) return false;
    m.revision = Fnv1a64(data, bytes);
    if (!m.revision) m.revision = 1;
    output = std::move(m);
    return true;
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
    m.packetChecksum = r.Read(8);
    const auto segmentCount = r.Read(4);
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
    const auto metadataBytes = vorbis_test_wire::kHeaderBytes +
        static_cast<std::uint64_t>(m.identBytes) + m.setupBytes + indexBytes;
    if (indexBytes > r.remaining || metadataBytes > bytes ||
        m.packetBytes != bytes - metadataBytes) return false;
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
    // Exclude the checksum table itself to avoid a circular digest. Every
    // independently read segment must vouch for its positioning metadata.
    m.segmentChecksumSeed = Fnv1a64(data, static_cast<std::size_t>(metadataBytes - segmentCount * 8));
    for (std::uint64_t i = 0; i < segmentCount; ++i) m.segmentChecksums.push_back(r.Read(8));
    if (m.loopEnd && (m.loopStartPacket >= m.packetBytes || m.loopEndPacket >= m.packetBytes ||
        m.loopDiscard > m.blockLong / 2 || m.loopTrim > m.blockLong / 2)) return false;
    m.packets = static_cast<const std::uint8_t*>(data) + metadataBytes;
    if (Fnv1a64(m.packets, static_cast<std::size_t>(m.packetBytes)) != m.packetChecksum) return false;
    std::size_t seekIndex = 0;
    bool loopStartFound = !m.loopEnd, loopEndFound = !m.loopEnd;
    for (std::uint64_t at = 0; at < m.packetBytes;) {
        const std::uint8_t* packet = nullptr; std::uint32_t count = 0;
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
