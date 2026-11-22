#include "TransitionalVorbisPreviewDecoder.h"
#include "Tooling/Harkenizer/Core/VorbisEncoder.h"

#include <vorbis/codec.h>

#include <array>
#include <cmath>
#include <limits>

namespace harken::studio::bridge::Detail {
namespace {

struct Decoder
{
    vorbis_info info{};
    vorbis_comment comment{};
    vorbis_dsp_state state{};
    vorbis_block block{};
    bool stateReady = false;
    bool blockReady = false;

    Decoder()
    {
        vorbis_info_init(&info);
        vorbis_comment_init(&comment);
    }

    ~Decoder()
    {
        if (blockReady) vorbis_block_clear(&block);
        if (stateReady) vorbis_dsp_clear(&state);
        vorbis_comment_clear(&comment);
        vorbis_info_clear(&info);
    }
};

constexpr std::array<std::array<uint8_t, 8>, 8> kVorbisChannels{{
    {{0}}, {{0, 1}}, {{0, 2, 1}}, {{0, 1, 2, 3}},
    {{0, 2, 1, 3, 4}}, {{0, 2, 1, 4, 5, 3}},
    {{0, 2, 1, 5, 6, 4, 3}}, {{0, 2, 1, 6, 7, 4, 5, 3}}
}};

}

std::optional<std::vector<float>> decodeTransitionalVorbisPreview(
    const harken::studio::VorbisEncoder::EncodedMedia& media, QString* error)
{
    const auto fail = [&](const QString& message) -> std::optional<std::vector<float>> {
        if (error) *error = message;
        return std::nullopt;
    };
    std::vector<float> samples;
    if (media.channels == 0 || media.channels > kVorbisChannels.size() || media.sampleRate == 0
        || media.frameCount == 0 || media.frameCount > samples.max_size() / media.channels
        || media.prerollFrames > std::numeric_limits<uint64_t>::max() - media.frameCount
        || media.tailFrames > std::numeric_limits<uint64_t>::max() - media.frameCount - media.prerollFrames
        || media.packets.size() < 2)
        return fail(QStringLiteral("Vorbis preview metadata is invalid"));
    Decoder decoder;
    const auto comment = QByteArray::fromHex("03766f72626973000000000000000001");
    const std::array<const QByteArray*, 3> headers{&media.identificationHeader, &comment, &media.setupHeader};
    for (size_t index = 0; index < headers.size(); ++index) {
        if (headers[index]->isEmpty() || headers[index]->size() > std::numeric_limits<long>::max())
            return fail(QStringLiteral("Vorbis preview header size is invalid"));
        ogg_packet packet{};
        packet.packet = reinterpret_cast<unsigned char*>(const_cast<char*>(headers[index]->constData()));
        packet.bytes = static_cast<long>(headers[index]->size());
        packet.b_o_s = index == 0;
        packet.packetno = static_cast<ogg_int64_t>(index);
        if (vorbis_synthesis_headerin(&decoder.info, &decoder.comment, &packet) != 0)
            return fail(QStringLiteral("Vorbis preview header is invalid"));
    }
    if (decoder.info.channels != media.channels || static_cast<int64_t>(decoder.info.rate) != media.sampleRate
        || static_cast<int64_t>(vorbis_info_blocksize(&decoder.info, 0)) != media.smallBlockFrames
        || static_cast<int64_t>(vorbis_info_blocksize(&decoder.info, 1)) != media.largeBlockFrames)
        return fail(QStringLiteral("Vorbis preview header does not match media metadata"));
    if (vorbis_synthesis_init(&decoder.state, &decoder.info) != 0)
        return fail(QStringLiteral("Cannot initialize Vorbis preview decoder"));
    decoder.stateReady = true;
    if (vorbis_block_init(&decoder.state, &decoder.block) != 0)
        return fail(QStringLiteral("Cannot initialize Vorbis preview block"));
    decoder.blockReady = true;
    samples.resize(static_cast<size_t>(media.frameCount * media.channels));
    const uint64_t expectedFrames = media.prerollFrames + media.frameCount + media.tailFrames;
    uint64_t decodedFrames = 0;
    for (size_t index = 0; index < media.packets.size(); ++index) {
        const auto& encoded = media.packets[index];
        if (encoded.bytes.isEmpty() || encoded.bytes.size() > std::numeric_limits<long>::max())
            return fail(QStringLiteral("Vorbis preview packet size is invalid"));
        ogg_packet packet{};
        packet.packet = reinterpret_cast<unsigned char*>(const_cast<char*>(encoded.bytes.constData()));
        packet.bytes = static_cast<long>(encoded.bytes.size());
        packet.packetno = static_cast<ogg_int64_t>(index + 3);
        packet.granulepos = -1;
        if (vorbis_synthesis(&decoder.block, &packet) != 0
            || vorbis_synthesis_blockin(&decoder.state, &decoder.block) != 0)
            return fail(QStringLiteral("Vorbis preview packet could not be decoded"));
        float** channels = nullptr;
        int available = 0;
        while ((available = vorbis_synthesis_pcmout(&decoder.state, &channels)) > 0) {
            if (static_cast<uint64_t>(available) > expectedFrames - decodedFrames)
                return fail(QStringLiteral("Vorbis preview contains excess frames"));
            for (int frame = 0; frame < available; ++frame, ++decodedFrames) {
                if (decodedFrames < media.prerollFrames
                    || decodedFrames - media.prerollFrames >= media.frameCount)
                    continue;
                const auto output = static_cast<size_t>((decodedFrames - media.prerollFrames) * media.channels);
                for (uint16_t channel = 0; channel < media.channels; ++channel) {
                    const auto sample = channels[channel][frame];
                    if (!std::isfinite(sample))
                        return fail(QStringLiteral("Vorbis preview contains non-finite samples"));
                    samples[output + kVorbisChannels[media.channels - 1][channel]] = sample;
                }
            }
            if (vorbis_synthesis_read(&decoder.state, available) != 0)
                return fail(QStringLiteral("Cannot consume Vorbis preview samples"));
        }
        if (available < 0)
            return fail(QStringLiteral("Cannot read Vorbis preview samples"));
    }
    if (decodedFrames != expectedFrames)
        return fail(QStringLiteral("Vorbis preview frame count is inconsistent"));
    return samples;
}

}
