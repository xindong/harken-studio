#include "Tooling/Harkenizer/Core/VorbisEncoder.h"

#include <vorbis/vorbisenc.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace harken::studio::VorbisEncoder {
namespace {

struct CodecState
{
    vorbis_info info{};
    vorbis_comment comment{};
    vorbis_dsp_state dsp{};
    vorbis_block block{};
    bool dspReady = false;
    bool blockReady = false;

    CodecState()
    {
        vorbis_info_init(&info);
        vorbis_comment_init(&comment);
    }

    ~CodecState()
    {
        if (blockReady) vorbis_block_clear(&block);
        if (dspReady) vorbis_dsp_clear(&dsp);
        vorbis_comment_clear(&comment);
        vorbis_info_clear(&info);
    }
};

constexpr std::array<std::array<uint8_t, 8>, 8> kVorbisChannels{{
    {{0}},
    {{0, 1}},
    {},
    {{0, 1, 2, 3}},
    {},
    {{0, 2, 1, 4, 5, 3}},
    {},
    {{0, 2, 1, 6, 7, 4, 5, 3}}
}};

bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

ogg_packet codecPacket(const QByteArray& bytes)
{
    ogg_packet packet{};
    packet.packet = reinterpret_cast<unsigned char*>(const_cast<char*>(bytes.constData()));
    packet.bytes = static_cast<long>(bytes.size());
    return packet;
}

bool readHeaders(const EncodedMedia& media, CodecState& codec, QString* error)
{
    const auto comment = QByteArray::fromHex("03766f72626973000000000000000001");
    const std::array<const QByteArray*, 3> headers{
        &media.identificationHeader, &comment, &media.setupHeader};
    for (size_t index = 0; index < headers.size(); ++index) {
        if (headers[index]->isEmpty() || headers[index]->size() > std::numeric_limits<long>::max())
            return fail(error, QStringLiteral("Vorbis header size is invalid"));
        auto packet = codecPacket(*headers[index]);
        packet.b_o_s = index == 0;
        packet.packetno = static_cast<ogg_int64_t>(index);
        if (vorbis_synthesis_headerin(&codec.info, &codec.comment, &packet) != 0)
            return fail(error, QStringLiteral("Vorbis header is invalid"));
    }
    if (codec.info.channels != media.channels || codec.info.rate != media.sampleRate
        || vorbis_info_blocksize(&codec.info, 0) != media.smallBlockFrames
        || vorbis_info_blocksize(&codec.info, 1) != media.largeBlockFrames)
        return fail(error, QStringLiteral("Vorbis header does not match media metadata"));
    return true;
}

bool packetPositions(const EncodedMedia& media, vorbis_info& info,
                     std::vector<uint64_t>& frames, std::vector<uint64_t>& offsets,
                     QString* error)
{
    if (media.packets.size() < 2 || media.packets.size() > std::numeric_limits<uint32_t>::max())
        return fail(error, QStringLiteral("Vorbis packet count is invalid"));
    frames.reserve(media.packets.size());
    offsets.reserve(media.packets.size());
    uint64_t frame = 0;
    uint64_t offset = 0;
    long previousBlock = 0;
    for (size_t index = 0; index < media.packets.size(); ++index) {
        const auto& stored = media.packets[index];
        if (stored.bytes.isEmpty() || stored.bytes.size() > std::numeric_limits<long>::max())
            return fail(error, QStringLiteral("Vorbis packet size is invalid"));
        auto packet = codecPacket(stored.bytes);
        const auto block = vorbis_packet_blocksize(&info, &packet);
        if (block <= 0)
            return fail(error, QStringLiteral("Vorbis audio packet is invalid"));
        if (index != 0) frame += static_cast<uint64_t>((previousBlock + block) / 4);
        frames.push_back(frame);
        offsets.push_back(offset);
        offset += static_cast<uint64_t>(stored.bytes.size());
        previousBlock = block;
    }
    return true;
}

std::vector<SeekEntry> makeSeekTable(const EncodedMedia& media,
                                    const std::vector<uint64_t>& frames,
                                    const std::vector<uint64_t>& offsets)
{
    std::vector<uint64_t> targets;
    for (uint64_t frame = 0; frame < media.frameCount; frame += media.seekGrainFrames)
        targets.push_back(frame);
    if (media.loopEndFrame != 0) {
        const auto at = std::lower_bound(targets.begin(), targets.end(), media.loopStartFrame);
        if (at == targets.end() || *at != media.loopStartFrame)
            targets.insert(at, media.loopStartFrame);
    }
    std::vector<SeekEntry> entries;
    entries.reserve(targets.size());
    for (const auto target : targets) {
        const auto rawFrame = target + media.prerollFrames;
        const auto after = std::upper_bound(frames.begin(), frames.end() - 1, rawFrame);
        const auto index = static_cast<size_t>(std::distance(frames.begin(), after) - 1);
        entries.push_back({target, static_cast<uint32_t>(index), offsets[index],
                           rawFrame - frames[index]});
    }
    return entries;
}

}

std::optional<EncodedMedia> encode(const std::vector<float>& samples,
                                  uint32_t sampleRate,
                                  uint16_t channels,
                                  float quality,
                                  uint64_t loopStart,
                                  uint64_t loopEnd,
                                  QString* error)
{
    if (!supportsChannels(channels)) {
        fail(error, QStringLiteral("Vorbis supports 1, 2, 4, 6 or 8 channels"));
        return std::nullopt;
    }
    if (sampleRate == 0 || sampleRate > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())
        || samples.empty()
        || samples.size() % channels != 0 || !std::isfinite(quality)
        || quality < 0.0f || quality > 1.0f) {
        fail(error, QStringLiteral("Vorbis encoding parameters are invalid"));
        return std::nullopt;
    }
    const auto frameCount = static_cast<uint64_t>(samples.size() / channels);
    if ((loopEnd == 0 ? loopStart != 0 : loopStart >= loopEnd || loopEnd > frameCount)
        || !std::all_of(samples.begin(), samples.end(), [](float sample) { return std::isfinite(sample); })) {
        fail(error, QStringLiteral("Vorbis input samples or loop range are invalid"));
        return std::nullopt;
    }
    CodecState codec;
    const auto initialized = vorbis_encode_init_vbr(&codec.info, channels, sampleRate, quality);
    if (initialized != 0) {
        fail(error, QStringLiteral("Vorbis encoder rejected %1 Hz, %2 channels, quality %3 (error %4)")
            .arg(sampleRate).arg(channels).arg(quality).arg(initialized));
        return std::nullopt;
    }
    if (vorbis_analysis_init(&codec.dsp, &codec.info) != 0) {
        fail(error, QStringLiteral("Cannot initialize Vorbis analysis"));
        return std::nullopt;
    }
    codec.dspReady = true;
    if (vorbis_block_init(&codec.dsp, &codec.block) != 0) {
        fail(error, QStringLiteral("Cannot initialize Vorbis block"));
        return std::nullopt;
    }
    codec.blockReady = true;
    ogg_packet identification{}, comment{}, setup{};
    if (vorbis_analysis_headerout(&codec.dsp, &codec.comment, &identification, &comment, &setup) != 0) {
        fail(error, QStringLiteral("Cannot create Vorbis headers"));
        return std::nullopt;
    }
    EncodedMedia media;
    media.sampleRate = sampleRate;
    media.channels = channels;
    media.smallBlockFrames = static_cast<uint32_t>(vorbis_info_blocksize(&codec.info, 0));
    media.largeBlockFrames = static_cast<uint32_t>(vorbis_info_blocksize(&codec.info, 1));
    media.frameCount = frameCount;
    media.loopStartFrame = loopStart;
    media.loopEndFrame = loopEnd;
    media.encoderVersion = QString::fromUtf8(vorbis_version_string());
    media.identificationHeader = QByteArray(reinterpret_cast<const char*>(identification.packet), identification.bytes);
    media.setupHeader = QByteArray(reinterpret_cast<const char*>(setup.packet), setup.bytes);
    uint64_t position = 0;
    bool ended = false;
    do {
        const auto count = static_cast<int>(std::min<uint64_t>(1024, frameCount - position));
        if (count != 0) {
            auto** buffer = vorbis_analysis_buffer(&codec.dsp, count);
            if (!buffer) {
                fail(error, QStringLiteral("Cannot allocate Vorbis analysis buffer"));
                return std::nullopt;
            }
            for (uint16_t channel = 0; channel < channels; ++channel) {
                const auto sourceChannel = kVorbisChannels[channels - 1][channel];
                for (int frame = 0; frame < count; ++frame)
                    buffer[channel][frame] = samples[(position + frame) * channels + sourceChannel];
            }
        }
        if (vorbis_analysis_wrote(&codec.dsp, count) != 0) {
            fail(error, QStringLiteral("Cannot submit Vorbis input samples"));
            return std::nullopt;
        }
        position += static_cast<uint64_t>(count);
        int available;
        while ((available = vorbis_analysis_blockout(&codec.dsp, &codec.block)) == 1) {
            if (vorbis_analysis(&codec.block, nullptr) != 0 || vorbis_bitrate_addblock(&codec.block) != 0) {
                fail(error, QStringLiteral("Vorbis packet encoding failed"));
                return std::nullopt;
            }
            ogg_packet packet{};
            int flushed;
            while ((flushed = vorbis_bitrate_flushpacket(&codec.dsp, &packet)) == 1) {
                if (ended || packet.bytes <= 0) {
                    fail(error, QStringLiteral("Vorbis encoder returned an invalid packet sequence"));
                    return std::nullopt;
                }
                media.packets.push_back({QByteArray(reinterpret_cast<const char*>(packet.packet), packet.bytes),
                                         packet.granulepos});
                ended = packet.e_o_s != 0;
            }
            if (flushed < 0) {
                fail(error, QStringLiteral("Cannot retrieve Vorbis packets"));
                return std::nullopt;
            }
        }
        if (available < 0 || (count == 0 && !ended)) {
            fail(error, QStringLiteral("Vorbis encoder did not finish the stream"));
            return std::nullopt;
        }
    } while (!ended);
    std::vector<uint64_t> frames, offsets;
    if (!packetPositions(media, codec.info, frames, offsets, error)) return std::nullopt;
    if (frames.back() < frameCount || media.packets.front().granuleFrame != 0
        || media.packets.back().granuleFrame != static_cast<int64_t>(frameCount)) {
        fail(error, QStringLiteral("Vorbis encoder returned inconsistent output frame positions"));
        return std::nullopt;
    }
    media.tailFrames = frames.back() - frameCount;
    media.seekTable = makeSeekTable(media, frames, offsets);
    if (!validate(media, error)) return std::nullopt;
    return media;
}

bool validate(const EncodedMedia& media, QString* error)
{
    if (media.sampleRate == 0 || !supportsChannels(media.channels)
        || media.frameCount == 0 || media.frameCount > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
        || media.seekGrainFrames == 0 || media.encoderVersion.isEmpty()
        || (media.loopEndFrame == 0 ? media.loopStartFrame != 0
            : media.loopStartFrame >= media.loopEndFrame || media.loopEndFrame > media.frameCount))
        return fail(error, QStringLiteral("Vorbis media metadata is invalid"));
    CodecState codec;
    if (!readHeaders(media, codec, error)) return false;
    std::vector<uint64_t> frames, offsets;
    if (!packetPositions(media, codec.info, frames, offsets, error)) return false;
    if (media.prerollFrames > frames.back() || media.frameCount > frames.back() - media.prerollFrames
        || media.tailFrames != frames.back() - media.prerollFrames - media.frameCount)
        return fail(error, QStringLiteral("Vorbis output frame count is inconsistent"));
    for (size_t index = 0; index < media.packets.size(); ++index) {
        const auto expected = index + 1 == media.packets.size() ? media.frameCount
            : frames[index] - std::min(frames[index], media.prerollFrames);
        if (media.packets[index].granuleFrame != static_cast<int64_t>(expected))
            return fail(error, QStringLiteral("Vorbis packet frame position is inconsistent"));
    }
    const uint64_t expectedCount = (media.frameCount - 1) / media.seekGrainFrames + 1
        + (media.loopEndFrame != 0 && media.loopStartFrame % media.seekGrainFrames != 0 ? 1 : 0);
    if (media.seekTable.size() != expectedCount)
        return fail(error, QStringLiteral("Vorbis seek table is incomplete"));
    const auto expected = makeSeekTable(media, frames, offsets);
    for (size_t index = 0; index < expected.size(); ++index) {
        const auto& entry = media.seekTable[index];
        const auto& target = expected[index];
        if (entry.outputFrame != target.outputFrame || entry.packetIndex != target.packetIndex
            || entry.byteOffset != target.byteOffset || entry.discardFrames != target.discardFrames)
            return fail(error, QStringLiteral("Vorbis seek entry is inconsistent"));
    }
    return true;
}

}
