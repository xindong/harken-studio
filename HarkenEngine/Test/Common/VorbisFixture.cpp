#include "VorbisFixture.h"
#include "harken/media/PackFormat.h"
#include <vorbis/vorbisfile.h>
extern "C" int harken_test_vorbis_floor_supported(const vorbis_info* info);
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace harken::test {
namespace {
void Require(bool ok) { if (!ok) throw std::runtime_error("Invalid Vorbis fixture"); }
void Put(std::vector<std::uint8_t>& out, std::uint64_t value, unsigned size) {
    for (unsigned i = 0; i < size; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
ogg_packet Packet(const std::vector<std::uint8_t>& data, long bos = 0) {
    ogg_packet p{}; p.packet = const_cast<unsigned char*>(data.data());
    p.bytes = static_cast<long>(data.size()); p.b_o_s = bos; p.granulepos = -1; return p;
}
struct Headers {
    vorbis_info info{}; vorbis_comment comment{};
    Headers() { vorbis_info_init(&info); vorbis_comment_init(&comment); }
    ~Headers() { vorbis_comment_clear(&comment); vorbis_info_clear(&info); }
    void Read(const std::vector<std::uint8_t>& data, bool first = false) {
        auto p = Packet(data, first); Require(vorbis_synthesis_headerin(&info, &comment, &p) == 0);
    }
};
struct OggReader {
    ogg_sync_state sync{}; ogg_stream_state stream{}; bool hasStream = false;
    OggReader() { Require(ogg_sync_init(&sync) == 0); }
    ~OggReader() { if (hasStream) ogg_stream_clear(&stream); ogg_sync_clear(&sync); }
};
struct Decoder {
    vorbis_dsp_state dsp{}; vorbis_block block{};
    explicit Decoder(vorbis_info& info) {
        Require(vorbis_synthesis_init(&dsp, &info) == 0);
        vorbis_block_init(&dsp, &block);
    }
    ~Decoder() { vorbis_block_clear(&block); vorbis_dsp_clear(&dsp); }
};
std::vector<float> Linear(const std::vector<std::uint8_t>& ogg) {
    struct Cursor { const std::vector<std::uint8_t>& data; std::size_t at = 0; } c{ogg};
    ov_callbacks callbacks{};
    callbacks.read_func = [](void* out, std::size_t size, std::size_t count, void* ptr) {
        auto& cursor = *static_cast<Cursor*>(ptr);
        if (!size) return std::size_t(0);
        const auto items = std::min(count, (cursor.data.size() - cursor.at) / size);
        std::memcpy(out, cursor.data.data() + cursor.at, items * size);
        cursor.at += items * size; return items;
    };
    OggVorbis_File vf{};
    Require(ov_open_callbacks(&c, &vf, nullptr, 0, callbacks) == 0);
    struct Close { OggVorbis_File* f; ~Close() { ov_clear(f); } } close{&vf};
    const auto channels = ov_info(&vf, -1)->channels;
    std::vector<float> result;
    for (;;) {
        float** pcm = nullptr;
        const auto count = ov_read_float(&vf, &pcm, 2048, nullptr);
        Require(count >= 0); if (!count) break;
        for (long i = 0; i < count; ++i)
            for (int ch = 0; ch < channels; ++ch) result.push_back(pcm[ch][i]);
    }
    return result;
}
}

VorbisFixture MakeVorbisFixture(unsigned channels, bool streaming,
    std::uint64_t loopStart, std::uint64_t loopEnd, std::uint32_t granularity) {
    Require(granularity != 0);
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() /
        "Fixtures" / "Vorbis" / ("signal-" + std::to_string(channels) + ".ogg");
    std::ifstream file(path, std::ios::binary);
    std::vector<std::uint8_t> ogg(std::istreambuf_iterator<char>(file), {});
    Require(!ogg.empty() && ogg.size() < std::numeric_limits<long>::max());
    OggReader reader;
    std::memcpy(ogg_sync_buffer(&reader.sync, static_cast<long>(ogg.size())), ogg.data(), ogg.size());
    Require(ogg_sync_wrote(&reader.sync, static_cast<long>(ogg.size())) == 0);
    std::vector<std::vector<std::uint8_t>> packets;
    ogg_page page{}; ogg_packet packet{};
    std::uint64_t frames = 0; bool ended = false;
    int pageResult = 0;
    while ((pageResult = ogg_sync_pageout(&reader.sync, &page)) == 1) {
        Require(!ended);
        if (!reader.hasStream) {
            Require(ogg_stream_init(&reader.stream, ogg_page_serialno(&page)) == 0);
            reader.hasStream = true;
        }
        Require(ogg_stream_pagein(&reader.stream, &page) == 0);
        int packetResult = 0;
        while ((packetResult = ogg_stream_packetout(&reader.stream, &packet)) == 1) {
            Require(packet.bytes > 0 && packet.bytes <= 65535);
            packets.emplace_back(packet.packet, packet.packet + packet.bytes);
            if (packet.e_o_s) { Require(packet.granulepos > 0); frames = packet.granulepos; ended = true; }
        }
        Require(packetResult == 0);
    }
    Require(pageResult == 0 && ended && packets.size() > 4);
    Headers headers;
    for (std::size_t i = 0; i < 3; ++i) headers.Read(packets[i], i == 0);
    if (!harken_test_vorbis_floor_supported(&headers.info))
        throw std::runtime_error("Unsupported Vorbis floor type");
    Require(static_cast<unsigned>(headers.info.channels) == channels);
    std::vector<std::uint64_t> offsets, starts, ends;
    std::vector<std::uint8_t> data;
    long previous = 0; std::uint64_t produced = 0;
    for (std::size_t i = 3; i < packets.size(); ++i) {
        auto p = Packet(packets[i]); const auto block = vorbis_packet_blocksize(&headers.info, &p);
        Require(block > 0);
        offsets.push_back(data.size()); starts.push_back(produced);
        if (previous) produced += previous / 4 + block / 4;
        ends.push_back(produced); previous = block;
        Put(data, packets[i].size(), 2); data.insert(data.end(), packets[i].begin(), packets[i].end());
    }
    Require(produced >= frames && (!loopEnd || (loopStart < loopEnd && loopEnd <= frames)));
    auto containing = [&](std::uint64_t frame) {
        return static_cast<std::size_t>(std::upper_bound(ends.begin(), ends.end(), frame) - ends.begin());
    };
    auto point = [&](std::uint64_t frame) {
        const auto i = containing(frame); Require(i > 0 && i < offsets.size());
        return VorbisSeekPoint{frame, offsets[i - 1], static_cast<std::uint32_t>(frame - starts[i])};
    };
    std::vector<VorbisSeekPoint> seek;
    for (std::uint64_t frame = 0; frame < frames; frame += granularity) seek.push_back(point(frame));
    const auto loop = loopEnd ? point(loopStart) : VorbisSeekPoint{};
    const auto loopLast = loopEnd ? containing(loopEnd - 1) : 0;
    VorbisFixture result; auto& out = result.media;
    Put(out, vorbis_test_wire::kMediaMagic, 4); Put(out, vorbis_test_wire::kVersion, 2);
    Put(out, loopEnd ? 1 : 0, 2); Put(out, headers.info.rate, 4); Put(out, channels, 2); Put(out, 0, 2);
    Put(out, frames, 8); Put(out, vorbis_info_blocksize(&headers.info, 0), 2);
    Put(out, vorbis_info_blocksize(&headers.info, 1), 2);
    Put(out, packets[0].size(), 4); Put(out, packets[2].size(), 4);
    Put(out, granularity, 4); Put(out, seek.size(), 4);
    Put(out, loopStart, 8); Put(out, loopEnd, 8);
    Put(out, loop.packetOffset, 8); Put(out, loop.discardFrames, 4);
    Put(out, loopEnd ? offsets[loopLast] : 0, 8); Put(out, loopEnd ? ends[loopLast] - loopEnd : 0, 4);
    Put(out, data.size(), 8); Put(out, Fnv1a64(ogg.data(), ogg.size()), 8);
    Put(out, Fnv1a64(data.data(), data.size()), 8); Put(out, streaming ? seek.size() : 0, 4);
    Require(out.size() == vorbis_test_wire::kHeaderBytes);
    out.insert(out.end(), packets[0].begin(), packets[0].end());
    out.insert(out.end(), packets[2].begin(), packets[2].end());
    for (const auto& s : seek) { Put(out, s.frame, 8); Put(out, s.packetOffset, 8); Put(out, s.discardFrames, 4); }
    if (streaming) for (std::size_t i = 0; i < seek.size(); ++i) {
        const auto end = i + 1 < seek.size() ? seek[i + 1].packetOffset : data.size();
        Put(out, Fnv1a64(data.data() + seek[i].packetOffset, end - seek[i].packetOffset), 8);
    }
    out.insert(out.end(), data.begin(), data.end());
    result.reference = Linear(ogg); Require(result.reference.size() == frames * channels);
    return result;
}

std::vector<std::uint8_t> WrapVorbisStream(const std::vector<std::uint8_t>& media) {
    std::vector<std::uint8_t> result;
    Put(result, vorbis_test_wire::kStreamMagic, 4); Put(result, vorbis_test_wire::kVersion, 2);
    Put(result, 1, 2); Put(result, vorbis_test_wire::kStreamHeaderBytes, 8); Put(result, media.size(), 8);
    result.insert(result.end(), media.begin(), media.end()); return result;
}

std::vector<float> DecodeReferencePackets(const VorbisMedia& media, std::uint64_t frame, std::uint64_t count) {
    Require(frame <= media.frames && count <= media.frames - frame);
    Headers headers;
    headers.Read({media.ident, media.ident + media.identBytes}, true);
    // The media container intentionally omits user comments.
    headers.Read({3, 'v', 'o', 'r', 'b', 'i', 's', 0, 0, 0, 0, 0, 0, 0, 0, 1});
    headers.Read({media.setup, media.setup + media.setupBytes});
    Decoder decoder(headers.info);
    const auto* seek = media.Seek(frame); Require(seek != nullptr);
    std::uint64_t skip = seek->discardFrames + frame - seek->frame;
    std::uint64_t at = seek->packetOffset;
    std::vector<float> out;
    while (out.size() / media.channels < count) {
        const std::uint8_t* bytes = nullptr; std::uint16_t size = 0;
        Require(media.Packet(at, bytes, size));
        ogg_packet p{}; p.packet = const_cast<unsigned char*>(bytes); p.bytes = size; p.granulepos = -1;
        Require(vorbis_synthesis(&decoder.block, &p) == 0);
        Require(vorbis_synthesis_blockin(&decoder.dsp, &decoder.block) == 0);
        float** pcm = nullptr; const int n = vorbis_synthesis_pcmout(&decoder.dsp, &pcm);
        for (int i = 0; i < n; ++i) {
            if (skip) { --skip; continue; }
            if (out.size() / media.channels == count) break;
            for (unsigned ch = 0; ch < media.channels; ++ch) out.push_back(pcm[ch][i]);
        }
        Require(vorbis_synthesis_read(&decoder.dsp, n) == 0); at += 2 + size;
    }
    return out;
}
}
