#include "PackagingBridge.h"
#include "ProjectSyncBridge.h"
#include "Utils/AudioFileReader.h"
#include <harken_studio_build/BindingTransition.h>
#include <harken_studio_build/BindingTargets.h>

#include <miniaudio.h>
#include <vorbis/vorbisfile.h>

#include <harken/Harken.h>
#include <harken_studio_build/PackFormat.h>
#include <harken_studio_build/PackFormatV2.h>
#include <harken_studio_build/EffectEncoding.h>
#include <harken_studio_build/MediaContentRevision.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <cmath>
#include <climits>
#include <cstdint>
#include <cstring>
#include <limits>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace HarkenEditor {
namespace Bridge {

namespace {

struct LocalMediaEntry
{
    QString path;
    uint64_t fileSize = 0;
};

QHash<uint32_t, LocalMediaEntry>& localMediaRegistry()
{
    static QHash<uint32_t, LocalMediaEntry> registry;
    return registry;
}

QMutex& localMediaRegistryMutex()
{
    static QMutex mutex;
    return mutex;
}

QString normalizedMediaPath(const QString& path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

uint32_t fnv1a32(const QByteArray& bytes)
{
    uint32_t hash = 2166136261u;
    for (char c : bytes)
    {
        hash ^= static_cast<uint8_t>(c);
        hash *= 16777619u;
    }
    return hash;
}

uint32_t crc32(const uint8_t* data, uint32_t length)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < length; ++i)
    {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

uint16_t readBe16(const unsigned char* data)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) |
                                 static_cast<uint16_t>(data[1]));
}

uint32_t readBe32(const unsigned char* data)
{
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

uint64_t readBe64(const unsigned char* data)
{
    return (static_cast<uint64_t>(readBe32(data)) << 32) |
           static_cast<uint64_t>(readBe32(data + 4));
}

uint32_t readLe32(const unsigned char* data)
{
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

double readExtended80(const unsigned char* data)
{
    const uint16_t signExponent = readBe16(data);
    const uint16_t exponent = signExponent & 0x7FFFu;
    const uint64_t mantissa = readBe64(data + 2);
    if ((signExponent & 0x8000u) != 0 || exponent == 0x7FFFu)
        return 0.0;
    if (exponent == 0 && mantissa == 0)
        return 0.0;
    return std::ldexp(static_cast<double>(mantissa),
                      static_cast<int>(exponent) - 16383 - 63);
}

struct AiffDescriptor
{
    uint32_t channels = 0;
    uint32_t sampleRate = 0;
    uint32_t bitDepth = 0;
    uint64_t frameCount = 0;
    uint64_t sampleOffset = 0;
    uint64_t sampleBytes = 0;
    bool littleEndian = false;
    bool floatingPoint = false;
};

bool parseAiff(const QByteArray& bytes, AiffDescriptor& out)
{
    out = {};
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.constData());
    const uint64_t size = static_cast<uint64_t>(bytes.size());
    if (size < 12 || std::memcmp(data, "FORM", 4) != 0)
        return false;

    const uint64_t formSize = readBe32(data + 4);
    if (formSize < 4 || formSize > size - 8)
        return false;
    const uint64_t formEnd = 8 + formSize;
    const bool compressed = std::memcmp(data + 8, "AIFC", 4) == 0;
    if (!compressed && std::memcmp(data + 8, "AIFF", 4) != 0)
        return false;

    bool hasCommon = false;
    bool hasSound = false;
    for (uint64_t offset = 12; offset < formEnd;)
    {
        if (formEnd - offset < 8)
            return false;
        const unsigned char* header = data + offset;
        const uint64_t chunkSize = readBe32(header + 4);
        const uint64_t chunkData = offset + 8;
        if (chunkSize > formEnd - chunkData)
            return false;
        const uint64_t next = chunkData + chunkSize + (chunkSize & 1u);
        if (next > formEnd)
            return false;

        if (std::memcmp(header, "COMM", 4) == 0)
        {
            if (hasCommon || chunkSize < 18 || (compressed && chunkSize < 22))
                return false;
            out.channels = readBe16(data + chunkData);
            out.frameCount = readBe32(data + chunkData + 2);
            out.bitDepth = readBe16(data + chunkData + 6);
            const double sampleRate = readExtended80(data + chunkData + 8);
            if (!std::isfinite(sampleRate) || sampleRate < 1.0 ||
                sampleRate > 768000.0)
                return false;
            out.sampleRate = static_cast<uint32_t>(std::llround(sampleRate));
            if (compressed)
            {
                const unsigned char* compression = data + chunkData + 18;
                if (std::memcmp(compression, "sowt", 4) == 0)
                    out.littleEndian = true;
                else if (std::memcmp(compression, "fl32", 4) == 0 ||
                         std::memcmp(compression, "FL32", 4) == 0)
                    out.floatingPoint = true;
                else if (std::memcmp(compression, "NONE", 4) != 0 &&
                         std::memcmp(compression, "twos", 4) != 0)
                    return false;
            }
            hasCommon = true;
        }
        else if (std::memcmp(header, "SSND", 4) == 0)
        {
            if (hasSound || chunkSize < 8)
                return false;
            const uint64_t soundOffset = readBe32(data + chunkData);
            if (soundOffset > chunkSize - 8)
                return false;
            out.sampleOffset = chunkData + 8 + soundOffset;
            out.sampleBytes = chunkSize - 8 - soundOffset;
            hasSound = true;
        }
        offset = next;
    }

    if (!hasCommon || !hasSound || out.channels == 0 || out.channels > 32 ||
        out.frameCount == 0 || out.sampleRate == 0)
        return false;
    if (out.bitDepth != 8 && out.bitDepth != 16 && out.bitDepth != 24 &&
        out.bitDepth != 32)
        return false;
    if (out.floatingPoint && out.bitDepth != 32)
        return false;

    const uint64_t bytesPerSample = (out.bitDepth + 7u) / 8u;
    if (out.frameCount > std::numeric_limits<uint64_t>::max() / out.channels)
        return false;
    const uint64_t sampleCount = out.frameCount * out.channels;
    if (sampleCount > std::numeric_limits<uint64_t>::max() / bytesPerSample)
        return false;
    return sampleCount * bytesPerSample <= out.sampleBytes;
}

bool readAiffFile(const QString& path, QByteArray& bytes, AiffDescriptor& descriptor)
{
    VNX::AudioFileReader file(path);
    if (!file.openReadOnly())
        return false;
    bytes = file.readAll();
    return !bytes.isEmpty() && parseAiff(bytes, descriptor);
}

bool decodeAiffFile(const QString& path, std::vector<float>& outInterleaved,
                    int& outChannels, int& outSampleRate)
{
    QByteArray bytes;
    AiffDescriptor descriptor;
    if (!readAiffFile(path, bytes, descriptor))
        return false;

    const uint64_t sampleCount = descriptor.frameCount * descriptor.channels;
    if (sampleCount > static_cast<uint64_t>(outInterleaved.max_size()))
        return false;
    outInterleaved.resize(static_cast<size_t>(sampleCount));
    const auto* sample = reinterpret_cast<const unsigned char*>(bytes.constData()) +
                         descriptor.sampleOffset;
    const uint32_t sampleBytes = (descriptor.bitDepth + 7u) / 8u;
    for (uint64_t index = 0; index < sampleCount; ++index, sample += sampleBytes)
    {
        float value = 0.0f;
        if (descriptor.floatingPoint)
        {
            const uint32_t bits = descriptor.littleEndian
                ? readLe32(sample) : readBe32(sample);
            std::memcpy(&value, &bits, sizeof(value));
            if (!std::isfinite(value))
                return false;
        }
        else if (descriptor.bitDepth == 8)
        {
            value = static_cast<float>(static_cast<int8_t>(sample[0])) / 128.0f;
        }
        else
        {
            uint32_t raw = 0;
            if (descriptor.littleEndian)
            {
                for (uint32_t byte = 0; byte < sampleBytes; ++byte)
                    raw |= static_cast<uint32_t>(sample[byte]) << (byte * 8u);
            }
            else
            {
                for (uint32_t byte = 0; byte < sampleBytes; ++byte)
                    raw = (raw << 8) | sample[byte];
            }
            const uint32_t shift = 32u - descriptor.bitDepth;
            const int32_t signedValue = static_cast<int32_t>(raw << shift) >> shift;
            value = static_cast<float>(
                static_cast<double>(signedValue) /
                static_cast<double>(uint64_t{1} << (descriptor.bitDepth - 1u)));
        }
        outInterleaved[static_cast<size_t>(index)] = value;
    }

    outChannels = static_cast<int>(descriptor.channels);
    outSampleRate = static_cast<int>(descriptor.sampleRate);
    return true;
}

size_t oggRead(void* pointer, size_t size, size_t count, void* dataSource)
{
    if (pointer == nullptr || dataSource == nullptr || size == 0 || count == 0 ||
        count > static_cast<size_t>(std::numeric_limits<qint64>::max()) / size)
        return 0;
    QFile* file = static_cast<QFile*>(dataSource);
    const qint64 bytes = file->read(static_cast<char*>(pointer),
                                    static_cast<qint64>(size * count));
    return bytes <= 0 ? 0 : static_cast<size_t>(bytes) / size;
}

int seekAudioFile(void* dataSource, ogg_int64_t offset, int origin)
{
    if (dataSource == nullptr)
        return -1;
    QFile* file = static_cast<QFile*>(dataSource);
    qint64 base = 0;
    if (origin == SEEK_CUR)
        base = file->pos();
    else if (origin == SEEK_END)
        base = file->size();
    else if (origin != SEEK_SET)
        return -1;
    if (base < 0 || offset > std::numeric_limits<qint64>::max() - base ||
        offset < -base)
        return -1;
    return file->seek(base + offset) ? 0 : -1;
}

long oggTell(void* dataSource)
{
    if (dataSource == nullptr)
        return -1;
    const qint64 position = static_cast<QFile*>(dataSource)->pos();
    return position < 0 || position > LONG_MAX ? -1 : static_cast<long>(position);
}

int oggClose(void*)
{
    return 0;
}

bool openOgg(const QString& path, VNX::AudioFileReader& file, OggVorbis_File& vorbis)
{
    file.setFileName(path);
    if (!file.openReadOnly())
        return false;
    const ov_callbacks callbacks{oggRead, seekAudioFile, oggClose, oggTell};
    return ov_open_callbacks(&file, &vorbis, nullptr, 0, callbacks) == 0;
}

bool decodeOggFile(const QString& path, std::vector<float>& outInterleaved,
                   int& outChannels, int& outSampleRate)
{
    VNX::AudioFileReader file;
    OggVorbis_File vorbis{};
    if (!openOgg(path, file, vorbis))
        return false;

    const int streams = ov_streams(&vorbis);
    const ogg_int64_t totalFrames = ov_pcm_total(&vorbis, -1);
    vorbis_info* first = streams > 0 ? ov_info(&vorbis, 0) : nullptr;
    bool valid = totalFrames > 0 && first != nullptr && first->channels > 0 &&
                 first->channels <= 32 && first->rate > 0 && first->rate <= 768000;
    for (int stream = 1; valid && stream < streams; ++stream)
    {
        vorbis_info* info = ov_info(&vorbis, stream);
        valid = info != nullptr && info->channels == first->channels &&
                info->rate == first->rate;
    }
    if (valid && static_cast<uint64_t>(totalFrames) >
                     outInterleaved.max_size() / static_cast<size_t>(first->channels))
        valid = false;
    if (!valid)
    {
        ov_clear(&vorbis);
        return false;
    }

    outChannels = first->channels;
    outSampleRate = first->rate;
    outInterleaved.reserve(static_cast<size_t>(totalFrames) *
                           static_cast<size_t>(outChannels));
    ogg_int64_t decodedFrames = 0;
    int bitstream = 0;
    for (;;)
    {
        float** channels = nullptr;
        const long frames = ov_read_float(&vorbis, &channels, 4096, &bitstream);
        if (frames == 0)
            break;
        vorbis_info* info = frames > 0 ? ov_info(&vorbis, bitstream) : nullptr;
        if (frames < 0 || info == nullptr || info->channels <= 0 ||
            info->channels > 32 || info->rate <= 0 || info->rate > 768000 ||
            channels == nullptr)
        {
            valid = false;
            break;
        }
        if (outChannels != info->channels || outSampleRate != info->rate ||
            frames > totalFrames - decodedFrames)
        {
            valid = false;
            break;
        }
        const size_t frameCount = static_cast<size_t>(frames);
        const size_t channelCount = static_cast<size_t>(outChannels);
        if (frameCount > (outInterleaved.max_size() - outInterleaved.size()) /
                             channelCount)
        {
            valid = false;
            break;
        }
        const size_t oldSize = outInterleaved.size();
        outInterleaved.resize(oldSize + frameCount * channelCount);
        for (size_t frame = 0; frame < frameCount; ++frame)
        {
            for (size_t channel = 0; channel < channelCount; ++channel)
                outInterleaved[oldSize + frame * channelCount + channel] =
                    channels[channel][frame];
        }
        decodedFrames += frames;
    }
    ov_clear(&vorbis);
    if (!valid || decodedFrames != totalFrames || outInterleaved.empty())
    {
        outInterleaved.clear();
        outChannels = 0;
        outSampleRate = 0;
        return false;
    }
    return true;
}

bool isOggPath(const QString& path)
{
    return QFileInfo(path).suffix().compare(QStringLiteral("ogg"),
                                            Qt::CaseInsensitive) == 0;
}

bool isAiffPath(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix();
    return suffix.compare(QStringLiteral("aiff"), Qt::CaseInsensitive) == 0 ||
           suffix.compare(QStringLiteral("aif"), Qt::CaseInsensitive) == 0;
}

AudioFileInfoData aiffFileInfo(const QString& path)
{
    AudioFileInfoData out;
    QByteArray bytes;
    AiffDescriptor descriptor;
    if (!readAiffFile(path, bytes, descriptor) ||
        descriptor.frameCount > static_cast<uint64_t>(
            std::numeric_limits<int64_t>::max()))
        return out;

    out.valid = true;
    out.channels = static_cast<int>(descriptor.channels);
    out.sampleRate = static_cast<int>(descriptor.sampleRate);
    out.bitDepth = static_cast<int>(descriptor.bitDepth);
    out.frameCount = static_cast<int64_t>(descriptor.frameCount);
    out.durationSeconds = static_cast<double>(descriptor.frameCount) /
                          static_cast<double>(descriptor.sampleRate);
    return out;
}

AudioFileInfoData oggFileInfo(const QString& path)
{
    AudioFileInfoData out;
    VNX::AudioFileReader file;
    OggVorbis_File vorbis{};
    if (!openOgg(path, file, vorbis))
        return out;

    const int streams = ov_streams(&vorbis);
    const ogg_int64_t totalFrames = ov_pcm_total(&vorbis, -1);
    vorbis_info* first = streams > 0 ? ov_info(&vorbis, 0) : nullptr;
    bool valid = totalFrames > 0 && first != nullptr && first->channels > 0 &&
                 first->channels <= 32 && first->rate > 0 && first->rate <= 768000;
    for (int stream = 1; valid && stream < streams; ++stream)
    {
        vorbis_info* info = ov_info(&vorbis, stream);
        valid = info != nullptr && info->channels == first->channels &&
                info->rate == first->rate;
    }
    if (valid)
    {
        out.valid = true;
        out.channels = first->channels;
        out.sampleRate = first->rate;
        out.bitDepth = 32;
        out.frameCount = static_cast<int64_t>(totalFrames);
        out.durationSeconds = static_cast<double>(totalFrames) /
                              static_cast<double>(first->rate);
    }
    ov_clear(&vorbis);
    return out;
}

bool initAudioDecoder(VNX::AudioFileReader& file, ma_decoder& decoder)
{
    if (!file.openReadOnly())
        return false;
    const auto read = [](ma_decoder* decoder, void* output, size_t bytes, size_t* bytesRead) -> ma_result {
        auto* file = static_cast<VNX::AudioFileReader*>(decoder->pUserData);
        const qint64 count = file->read(static_cast<char*>(output), static_cast<qint64>(bytes));
        if (bytesRead)
            *bytesRead = count > 0 ? static_cast<size_t>(count) : 0;
        return count < 0 ? MA_IO_ERROR : (count == 0 ? MA_AT_END : MA_SUCCESS);
    };
    const auto seek = [](ma_decoder* decoder, ma_int64 offset, ma_seek_origin origin) -> ma_result {
        auto* file = static_cast<VNX::AudioFileReader*>(decoder->pUserData);
        const int whence = origin == ma_seek_origin_start ? SEEK_SET
            : (origin == ma_seek_origin_current ? SEEK_CUR : SEEK_END);
        return seekAudioFile(file, offset, whence) == 0 ? MA_SUCCESS : MA_BAD_SEEK;
    };
    const ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    return ma_decoder_init(read, seek, &file, &config, &decoder) == MA_SUCCESS;
}

QString quoteProcessError(QProcess& process)
{
    const QString stdErr = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
    const QString stdOut = QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
    if (!stdErr.isEmpty() && !stdOut.isEmpty())
        return stdErr + QStringLiteral("\n") + stdOut;
    return !stdErr.isEmpty() ? stdErr : stdOut;
}

QString findPackcExecutable()
{
    const QString env = qEnvironmentVariable(QStringLiteral("HARKEN_PACKC_PATH").toUtf8().constData());
    if (!env.isEmpty() && QFileInfo::exists(env))
        return env;

    const QStringList exeNames =
#ifdef _WIN32
        {QStringLiteral("packc.exe"), QStringLiteral("harken_packc.exe")};
#else
        {QStringLiteral("packc"), QStringLiteral("harken_packc")};
#endif

    const QStringList roots = {
        QCoreApplication::applicationDirPath(),
        QDir::currentPath(),
        QDir(QDir::currentPath()).filePath(QStringLiteral("bin")),
        QDir(QDir::currentPath()).filePath(QStringLiteral("../bin"))
    };
    for (const QString& root : roots)
    {
        for (const QString& exeName : exeNames)
        {
            const QString candidate = QDir(root).filePath(exeName);
            if (QFileInfo::exists(candidate))
                return candidate;
        }
    }
    for (const QString& exeName : exeNames)
    {
        const QString path = QStandardPaths::findExecutable(exeName);
        if (!path.isEmpty())
            return path;
    }
    return QString();
}

} // namespace

QString packArtifactExtension() { return QStringLiteral(".bytes"); }
QString packManifestFileName() { return QStringLiteral("HarkenPackManifest.json"); }
QString packIndexFileName() { return QStringLiteral("HarkenPackIndex.json"); }
QString packVorbisCodecName() { return QStringLiteral("unavailable"); }
QString packOpusCodecName() { return QStringLiteral("unavailable"); }
uint32_t legacyPackContainerMagic() { return 0; }

uint32_t packFormatCrc32(const uint8_t* data, uint32_t length)
{
    if (data == nullptr && length != 0)
        return 0;
    return crc32(data, length);
}

QString mediaRegistryFindPath(uint32_t sourceId)
{
    QMutexLocker lock(&localMediaRegistryMutex());
    const auto it = localMediaRegistry().constFind(sourceId);
    return it == localMediaRegistry().constEnd() ? QString() : it->path;
}

void mediaRegistryRegister(uint32_t sourceId, const QString& absolutePath, uint64_t fileSize)
{
    if (sourceId == 0 || absolutePath.isEmpty())
        return;
    QMutexLocker lock(&localMediaRegistryMutex());
    localMediaRegistry().insert(sourceId, LocalMediaEntry{normalizedMediaPath(absolutePath), fileSize});
}

void mediaRegistryUnregister(uint32_t sourceId)
{
    QMutexLocker lock(&localMediaRegistryMutex());
    localMediaRegistry().remove(sourceId);
}

uint32_t mediaRegistryCount()
{
    QMutexLocker lock(&localMediaRegistryMutex());
    return static_cast<uint32_t>(localMediaRegistry().size());
}

void mediaRegistryClear()
{
    QMutexLocker lock(&localMediaRegistryMutex());
    localMediaRegistry().clear();
}

uint32_t packLanguageIdFromName(const QString& name)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return 0;
    return fnv1a32(trimmed.toUtf8());
}

bool decodeAudioFileInterleaved(const QString& filePath,
                                std::vector<float>& outInterleaved,
                                int& outChannels,
                                int& outSampleRate)
{
    outInterleaved.clear();
    outChannels = 0;
    outSampleRate = 0;

    if (isOggPath(filePath))
        return decodeOggFile(filePath, outInterleaved, outChannels, outSampleRate);
    if (isAiffPath(filePath))
        return decodeAiffFile(filePath, outInterleaved, outChannels, outSampleRate);

    ma_decoder decoder{};
    VNX::AudioFileReader file(filePath);
    if (!initAudioDecoder(file, decoder))
        return false;

    const ma_uint32 channels = decoder.outputChannels;
    const ma_uint32 sampleRate = decoder.outputSampleRate;
    if (channels == 0 || sampleRate == 0)
    {
        ma_decoder_uninit(&decoder);
        return false;
    }

    constexpr ma_uint64 kChunkFrames = 4096;
    std::vector<float> chunk(static_cast<size_t>(kChunkFrames) * static_cast<size_t>(channels));
    for (;;)
    {
        ma_uint64 framesRead = 0;
        const ma_result result =
            ma_decoder_read_pcm_frames(&decoder, chunk.data(), kChunkFrames, &framesRead);
        if (framesRead > 0)
        {
            const size_t sampleCount =
                static_cast<size_t>(framesRead) * static_cast<size_t>(channels);
            outInterleaved.insert(outInterleaved.end(), chunk.begin(), chunk.begin() + sampleCount);
        }
        if (result == MA_AT_END)
            break;
        if (result != MA_SUCCESS)
        {
            ma_decoder_uninit(&decoder);
            outInterleaved.clear();
            return false;
        }
        if (framesRead == 0)
            break;
    }
    ma_decoder_uninit(&decoder);

    if (outInterleaved.empty())
    {
        outInterleaved.clear();
        return false;
    }

    outChannels = static_cast<int>(channels);
    outSampleRate = static_cast<int>(sampleRate);
    return true;
}

bool computePackMediaContentRevision(const PackMediaDesc& media, uint64_t* revision)
{
    if (!revision || media.downmixToMono || !media.data.empty()
        || media.format != PackMediaFormat::PcmFloat) return false;
    std::vector<float> interleaved;
    int channels = 0, sampleRate = 0;
    if (!decodeAudioFileInterleaved(media.sourcePath, interleaved, channels, sampleRate)
        || channels <= 0 || sampleRate <= 0 || interleaved.empty()
        || interleaved.size() % static_cast<std::size_t>(channels) != 0) return false;
    std::vector<const float*> planes(static_cast<std::size_t>(channels));
    for (int c = 0; c < channels; ++c) planes[c] = interleaved.data() + c;
    return harkenstudiobuild::ComputeMediaPcmContentRevisionV1(
        static_cast<uint32_t>(sampleRate), static_cast<uint32_t>(channels),
        interleaved.size() / static_cast<std::size_t>(channels), planes.data(),
        static_cast<uint32_t>(channels), revision,
        media.loopable ? media.loopStartFrame : 0, media.loopable ? media.loopEndFrame : 0);
}

AudioFileInfoData audioFileInfo(const QString& path)
{
    if (isOggPath(path))
        return oggFileInfo(path);
    if (isAiffPath(path))
        return aiffFileInfo(path);

    AudioFileInfoData out;
    ma_decoder decoder{};
    VNX::AudioFileReader file(path);
    if (!initAudioDecoder(file, decoder))
        return out;
    ma_uint64 frameCount = 0;
    if (ma_decoder_get_length_in_pcm_frames(&decoder, &frameCount) != MA_SUCCESS)
        frameCount = 0;
    if (decoder.outputChannels > 0 && decoder.outputChannels <= 32 &&
        decoder.outputSampleRate > 0 && decoder.outputSampleRate <= 768000 &&
        frameCount <= static_cast<ma_uint64>(std::numeric_limits<int64_t>::max()))
    {
        out.valid = true;
        out.channels = static_cast<int>(decoder.outputChannels);
        out.sampleRate = static_cast<int>(decoder.outputSampleRate);
        out.bitDepth = 32;
        out.frameCount = static_cast<int64_t>(frameCount);
        out.durationSeconds = static_cast<double>(frameCount) /
                              static_cast<double>(decoder.outputSampleRate);
    }
    ma_decoder_uninit(&decoder);
    return out;
}

uint32_t mediaRegistryFindSourceIdByPath(const QString& absolutePath)
{
    const QString needle = normalizedMediaPath(absolutePath);
    QMutexLocker lock(&localMediaRegistryMutex());
    for (auto it = localMediaRegistry().constBegin(); it != localMediaRegistry().constEnd(); ++it)
    {
        if (it->path == needle)
            return it.key();
    }
    return 0;
}

bool encodePackVorbis(const std::vector<float>&, int, int,
                      const PackVorbisEncodeSettings&,
                      std::vector<uint8_t>& outData,
                      PackCodecMetadata& outMetadata,
                      std::vector<uint8_t>* outSharedCodebook)
{
    outData.clear();
    outMetadata = {};
    if (outSharedCodebook)
        outSharedCodebook->clear();
    return false;
}

bool encodePackOpus(const std::vector<float>&, int, int,
                    std::vector<uint8_t>& outData,
                    PackCodecMetadata& outMetadata)
{
    outData.clear();
    outMetadata = {};
    return false;
}

void applyLoopRegion(PackCodecMetadata& metadata,
                     uint64_t loopStartFrame,
                     uint64_t loopEndFrame)
{
    Q_UNUSED(metadata);
    Q_UNUSED(loopStartFrame);
    Q_UNUSED(loopEndFrame);
}

QString packCodecName(uint16_t codecId)
{
    return codecId == 0 ? QStringLiteral("PCMFloat") : QStringLiteral("Unavailable");
}

QString packCodecContainerName(uint16_t containerId)
{
    return containerId == 0 ? QStringLiteral("PackF32Planar") : QStringLiteral("Unavailable");
}

PackCodecFlagsView decodePackCodecFlags(uint32_t flags)
{
    PackCodecFlagsView view;
    view.fullDecodeFallback = flags == 0;
    return view;
}

bool parsePackCodecMetadataSection(const QByteArray&, std::vector<PackCodecEntryInfo>& out,
                                   QString* error)
{
    out.clear();
    if (error)
        *error = QStringLiteral("Legacy CODC metadata is unavailable for .pack artifacts");
    return false;
}

bool parsePackCodecCodebookSection(const QByteArray&, std::vector<PackCodecCodebookInfo>& out,
                                   QString* error)
{
    out.clear();
    if (error)
        *error = QStringLiteral("Legacy CDBK metadata is unavailable for .pack artifacts");
    return false;
}

bool isPackCodecMetadataSection(uint32_t) { return false; }
bool isPackCodecCodebookSection(uint32_t) { return false; }

struct PackBuildSession::Impl
{
    struct MediaEntry
    {
        harkenstudio::ObjectId id{};
        uint32_t authoringSourceId = 0;
        QString path;
        bool downmixToMono = false;
        bool loopable = false;
        uint64_t loopStartFrame = 0;
        uint64_t loopEndFrame = 0;
    };

    struct MetadataEntry
    {
        harkenstudio::ObjectId ownerObjectId = {};
        QByteArray guidRfc4122;
        QString objectPath;
    };

    bool initPack = false;
    QString lastError;
    QStringList warnings;
    QStringList pendingWarnings;
    std::vector<MediaEntry> media;
    std::vector<PackBus> buses;
    std::vector<PackBusSend> busSends;
    std::vector<PackAttenuation> attenuations;
    std::vector<harkenstudio::ObjectId> discreteVariables;
    std::vector<PackMeterWiring> meterWirings;
    std::vector<PackParameterBinding> bindings;
    std::vector<PackBindingAttachment> bindingAttachments;
    std::vector<std::pair<harkenstudio::ObjectId, PackModulator>> modulatorBindings;
    std::vector<std::pair<harkenstudio::ObjectId, QString>> stringMappings;
    std::vector<MetadataEntry> objectMetadata;

    void warn(const QString& message)
    {
        if (!warnings.contains(message))
            warnings.append(message);
    }

    struct CurvePoint
    {
        float x = 0.0f;
        float y = 0.0f;
    };

    QJsonObject curveObject(const std::vector<CurvePoint>& inputPoints,
                            const std::vector<PackCurveHandle>& inputHandles,
                            const QString& context)
    {
        if (inputPoints.empty())
            return {};

        std::vector<CurvePoint> points = inputPoints;
        std::vector<PackCurveHandle> handles = inputHandles;
        if (points.size() > 8)
        {
            warn(QStringLiteral("%1: curve has %2 points; the pack format carries "
                                "at most 8, extra points and handles were dropped")
                     .arg(context).arg(points.size()));
            points.resize(8);
            handles.resize(7);
        }

        if ((points.size() > 1 && handles.size() != points.size() - 1) ||
            (points.size() == 1 && !handles.empty()))
        {
            warn(QStringLiteral("%1: points and handles must be paired; "
                                "the entire curve was dropped").arg(context));
            return {};
        }
        QJsonArray pointValues;
        for (size_t i = 0; i < points.size(); ++i)
        {
            if (!std::isfinite(points[i].x) || !std::isfinite(points[i].y) ||
                (i > 0 && !(points[i].x > points[i - 1].x)))
            {
                warn(QStringLiteral("%1: curve contains an invalid point; "
                                    "the entire curve was dropped").arg(context));
                return {};
            }
            pointValues.append(QJsonArray{points[i].x, points[i].y});
        }
        QJsonArray handleValues;
        for (const PackCurveHandle& handle : handles)
        {
            if (!std::isfinite(handle.h1x) || !std::isfinite(handle.h1y) ||
                !std::isfinite(handle.h2x) || !std::isfinite(handle.h2y))
            {
                warn(QStringLiteral("%1: curve contains an invalid handle; "
                                    "the entire curve was dropped").arg(context));
                return {};
            }
            handleValues.append(QJsonArray{handle.h1x, handle.h1y, handle.h2x, handle.h2y});
        }
        QJsonObject result{{QStringLiteral("points"), pointValues}};
        if (!handleValues.isEmpty())
            result.insert(QStringLiteral("handles"), handleValues);
        return result;
    }

    QJsonObject residentSections()
    {
        QJsonObject result = bindingSections(true);
        QJsonArray busValues;
        QJsonArray effectValues;
        std::unordered_set<harkenstudio::ObjectId> seenBuses;
        for (const PackBus& bus : buses)
        {
            if (harkenstudio::IsNull(bus.id))
                continue;
            if (bus.id == harkenstudio::kMasterBusObjectId) {
                lastError = QStringLiteral("The Runtime output root cannot be an authored Bus");
                return {};
            }
            if (!seenBuses.insert(bus.id).second)
            {
                warn(QStringLiteral("Bus '%1' repeats id %2 and was dropped")
                         .arg(bus.name, VNX::objectIdString(bus.id)));
                continue;
            }
            const harkenstudio::ObjectId parent =
                (harkenstudio::IsNull(bus.parentId) || bus.parentId == bus.id) ? harkenstudio::kMasterBusObjectId
                                                                     : bus.parentId;
            busValues.append(QJsonObject{
                {QStringLiteral("id"), VNX::objectIdString(bus.id)},
                {QStringLiteral("parent"), VNX::objectIdString(parent)},
                {QStringLiteral("volume"), bus.volume},
                {QStringLiteral("outputBusVolume"), bus.outputBusVolume},
                {QStringLiteral("makeUpGain"), bus.makeUpGain}});
            for (size_t slot = 0; slot < bus.effects.size(); ++slot) {
                const auto& effect = bus.effects[slot];
                const auto* schema = harkenstudiobuild::FindPackEffectSchemaByType(effect.type);
                if (!schema || harkenstudio::IsNull(effect.id)) {
                    lastError = QStringLiteral("Bus '%1' contains an invalid effect identity or type")
                        .arg(bus.name);
                    return {};
                }
                QJsonArray parameters;
                for (float value : effect.parameters) parameters.append(value);
                effectValues.append(QJsonObject{
                    {QStringLiteral("effectId"), VNX::objectIdString(effect.id)},
                    {QStringLiteral("bus"), VNX::objectIdString(bus.id)},
                    {QStringLiteral("slot"), static_cast<int>(slot)},
                    {QStringLiteral("type"), QString::fromLatin1(schema->manifestName)},
                    {QStringLiteral("descriptorVintage"), static_cast<int>(schema->descriptorVintage)},
                    {QStringLiteral("bypass"), effect.bypass},
                    {QStringLiteral("faderPointGuard"), 8},
                    {QStringLiteral("parameters"), parameters}});
            }
        }
        if (!busValues.isEmpty())
            result.insert(QStringLiteral("buses"), busValues);
        if (!effectValues.isEmpty())
            result.insert(QStringLiteral("busEffects"), effectValues);

        QJsonArray sendValues;
        std::set<std::pair<harkenstudio::ObjectId, harkenstudio::ObjectId>> seenEdges;
        for (const PackBusSend& send : busSends)
        {
            if (harkenstudio::IsNull(send.fromBusId) || harkenstudio::IsNull(send.toBusId) ||
                send.fromBusId == send.toBusId)
                continue;
            if (!seenEdges.insert({send.fromBusId, send.toBusId}).second)
                continue;
            sendValues.append(QJsonObject{
                {QStringLiteral("from"), VNX::objectIdString(send.fromBusId)},
                {QStringLiteral("to"), VNX::objectIdString(send.toBusId)},
                {QStringLiteral("levelDb"), send.levelDb}});
        }
        if (!sendValues.isEmpty())
            result.insert(QStringLiteral("busSends"), sendValues);

        QJsonArray attenuationValues;
        std::unordered_set<harkenstudio::ObjectId> seenAttenuations;
        for (const PackAttenuation& att : attenuations)
        {
            if (harkenstudio::IsNull(att.id) || !seenAttenuations.insert(att.id).second)
                continue;
            if (!(att.maxDistance > 0.0f))
            {
                warn(QStringLiteral("Attenuation %1: maxDistance must be > 0; "
                                    "the profile was dropped").arg(VNX::objectIdString(att.id)));
                continue;
            }
            const float minDistance = std::min(1.0f, att.maxDistance * 0.5f);
            QJsonObject value{
                {QStringLiteral("id"), VNX::objectIdString(att.id)},
                {QStringLiteral("rolloff"), QStringLiteral("linear")},
                {QStringLiteral("minDistance"), minDistance},
                {QStringLiteral("maxDistance"), att.maxDistance},
                {QStringLiteral("airAbsorptionNearHz"), 20000},
                {QStringLiteral("airAbsorptionFarHz"), 20000}};
            for (const PackAttenuation::Slot& slot : att.curveSlots)
            {
                if (slot.slot != 0)
                    continue;
                const QString context = QStringLiteral("Attenuation %1 slot %2")
                                            .arg(VNX::objectIdString(att.id)).arg(slot.slot);
                std::vector<CurvePoint> points;
                points.reserve(slot.points.size());
                for (const auto& point : slot.points)
                    points.push_back({point.distance, point.value});
                const QJsonObject curve = curveObject(points, slot.handles, context);
                if (!curve.isEmpty())
                    value.insert(QStringLiteral("volumeCurve"), curve);
            }
            attenuationValues.append(value);
        }
        if (!attenuationValues.isEmpty())
            result.insert(QStringLiteral("attenuations"), attenuationValues);

        if (!discreteVariables.empty() || !meterWirings.empty())
            warn(QStringLiteral("Discrete variables and meter wirings have no pack "
                                "representation in the current Variable world and "
                                "were not emitted"));
        return result;
    }

    QJsonObject bindingSections(bool resident)
    {
        QJsonObject result;
        QJsonArray bindingValues;
        std::unordered_set<harkenstudio::ObjectId> seenBindings;
        for (const PackParameterBinding& binding : bindings)
        {
            const QString context = QStringLiteral("Binding %1").arg(VNX::objectIdString(binding.id));
            const auto* targetMeta = harkenstudiobuild::FindTargetMeta(binding.scope, binding.target);
            const uint32_t transitionMs = binding.transitionMs;
            if (!harkenstudiobuild::IsValidBindingTransitionMs(transitionMs)) {
                lastError = QStringLiteral("%1: transitionMs must be 0..60000").arg(context);
                return {};
            }
            if (targetMeta && transitionMs != 0 && !targetMeta->supportsTransition) {
                lastError = QStringLiteral("%1: target does not support a ramp").arg(context);
                return {};
            }
            if (binding.scope > 1)
            {
                warn(QStringLiteral("%1: scope %2 has no pack representation and "
                                    "was dropped").arg(context).arg(binding.scope));
                continue;
            }
            if (resident && binding.scope == 0)
            {
                warn(QStringLiteral("%1: voice-scope bindings reference "
                                    "content-pack playables and were dropped from "
                                    "the resident pack").arg(context));
                continue;
            }
            if (!targetMeta)
            {
                warn(QStringLiteral("%1: target %2 is unavailable for its scope "
                                    "and the binding was dropped")
                         .arg(context).arg(binding.target));
                continue;
            }
            if (!seenBindings.insert(binding.id).second)
            {
                warn(QStringLiteral("%1 repeats its id and was dropped").arg(context));
                continue;
            }
            std::vector<CurvePoint> points;
            points.reserve(binding.points.size());
            for (const PackParameterBinding::CurvePoint& p : binding.points)
                points.push_back({p.x, p.y});
            const QJsonObject curve = curveObject(points, binding.handles, context);
            if (curve.isEmpty())
            {
                warn(QStringLiteral("%1 has no usable curve points and was "
                                    "dropped").arg(context));
                seenBindings.erase(binding.id);
                continue;
            }
            bindingValues.append(QJsonObject{
                {QStringLiteral("id"), VNX::objectIdString(binding.id)},
                {QStringLiteral("variable"), VNX::objectIdString(binding.variableId)},
                {QStringLiteral("scope"), binding.scope == 1 ? QStringLiteral("bus")
                                                             : QStringLiteral("voice")},
                {QStringLiteral("target"), static_cast<qint64>(binding.target)},
                {QStringLiteral("transitionMs"), static_cast<qint64>(transitionMs)},
                {QStringLiteral("curve"), curve}});
        }
        if (!bindingValues.isEmpty())
            result.insert(QStringLiteral("bindings"), bindingValues);

        QJsonArray attachmentValues;
        std::set<std::tuple<harkenstudio::ObjectId, uint8_t, harkenstudio::ObjectId>> seenAttachments;
        for (const PackBindingAttachment& attachment : bindingAttachments)
        {
            if (attachment.scope > 1)
            {
                warn(QStringLiteral("Attachment of binding %1: scope %2 has no "
                                    "pack representation and was dropped")
                         .arg(VNX::objectIdString(attachment.bindingId)).arg(attachment.scope));
                continue;
            }
            if (resident && attachment.scope == 0)
            {
                warn(QStringLiteral("Attachment of binding %1: voice-scope "
                                    "attachments reference content-pack playables "
                                    "and were dropped from the resident pack")
                         .arg(VNX::objectIdString(attachment.bindingId)));
                continue;
            }
            if (!seenAttachments
                     .insert({attachment.bindingId, attachment.scope, attachment.objectId})
                     .second)
                continue;
            attachmentValues.append(QJsonObject{
                {QStringLiteral("binding"), VNX::objectIdString(attachment.bindingId)},
                {QStringLiteral("scope"), attachment.scope == 1 ? QStringLiteral("bus")
                                                                : QStringLiteral("voice")},
                {QStringLiteral("object"), VNX::objectIdString(attachment.objectId)}});
        }
        if (!attachmentValues.isEmpty())
            result.insert(QStringLiteral("attachments"), attachmentValues);
        return result;
    }

    bool writeManifest(const QString& manifestPath, const QString& outputPath)
    {
        const bool resident = initPack;
        if (resident && !media.empty())
            warn(QStringLiteral("Resident pack refuses media; "
                                "media entries were not emitted"));
        if (!resident && (!buses.empty() || !busSends.empty() || !attenuations.empty()))
            warn(QStringLiteral("Content pack refuses bus/send/attenuation "
                                "infrastructure; those sections were not emitted"));
        if (!resident && (!discreteVariables.empty() || !meterWirings.empty()))
            warn(QStringLiteral("Content pack refuses discrete-variable/meter-wiring "
                                "infrastructure; those sections were not emitted"));
        if (!modulatorBindings.empty())
            warn(QStringLiteral("Modulator bindings have no pack representation "
                                "and were not emitted"));
        if (!stringMappings.empty())
            warn(QStringLiteral("String mappings have no pack representation "
                                "and were not emitted"));
        if (!objectMetadata.empty())
            warn(QStringLiteral("Object metadata has no pack representation "
                                "and was not emitted"));

        QJsonObject root = resident ? residentSections() : bindingSections(false);
        if (!lastError.isEmpty())
            return false;
        if (resident && root.isEmpty())
        {
            lastError = QStringLiteral("Resident pack has no logic entries; add at least one bus");
            return false;
        }
        QJsonArray entries;
        if (!resident)
        {
            std::unordered_set<harkenstudio::ObjectId> seenMedia;
            for (const MediaEntry& entry : media)
            {
                if (harkenstudio::IsNull(entry.id) || entry.path.isEmpty() ||
                    !seenMedia.insert(entry.id).second)
                    continue;
                QJsonObject value{
                    {QStringLiteral("path"), QFileInfo(entry.path).absoluteFilePath()},
                    {QStringLiteral("id"), VNX::objectIdString(entry.id)},
                    {QStringLiteral("codec"), QStringLiteral("pcm-f32")}};
                if (entry.loopable)
                {
                    value.insert(QStringLiteral("loopStartFrame"), static_cast<qint64>(entry.loopStartFrame));
                    value.insert(QStringLiteral("loopEndFrame"), static_cast<qint64>(entry.loopEndFrame));
                }
                if (entry.downmixToMono)
                    value.insert(QStringLiteral("downmix"), QStringLiteral("mono"));
                entries.append(value);
            }
        }
        if (!entries.isEmpty())
            root.insert(QStringLiteral("entries"), entries);
        root.insert(QStringLiteral("manifestVersion"), 3);
        root.insert(QStringLiteral("output"), QFileInfo(outputPath).absoluteFilePath());
        root.insert(QStringLiteral("packKind"), resident ? QStringLiteral("resident")
                                                        : QStringLiteral("content"));
        root.insert(QStringLiteral("options"), QJsonObject{
            {QStringLiteral("cache"), true},
            {QStringLiteral("reportPath"),
             QFileInfo(outputPath + QStringLiteral(".report.json")).absoluteFilePath()}});

        QDir().mkpath(QFileInfo(manifestPath).absolutePath());
        QFile file(manifestPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            lastError = QStringLiteral("Cannot write packc manifest: %1").arg(manifestPath);
            return false;
        }
        const QByteArray data = QJsonDocument(root).toJson();
        if (file.write(data) != data.size())
        {
            lastError = QStringLiteral("Cannot finish packc manifest: %1").arg(manifestPath);
            return false;
        }
        return true;
    }
};

PackBuildSession::PackBuildSession()
    : impl_(new Impl)
{
}

PackBuildSession::~PackBuildSession() = default;

void PackBuildSession::setInitPack(bool enable) { impl_->initPack = enable; }

void PackBuildSession::addBus(const PackBus& bus)
{
    impl_->buses.push_back(bus);
}

void PackBuildSession::addBusSend(const PackBusSend& send)
{
    impl_->busSends.push_back(send);
}

void PackBuildSession::addModulatorBinding(harkenstudio::ObjectId targetObjectId,
                                           const PackModulator& modulator)
{
    impl_->modulatorBindings.emplace_back(targetObjectId, modulator);
}

void PackBuildSession::addAttenuation(const PackAttenuation& attenuation)
{
    impl_->attenuations.push_back(attenuation);
}

void PackBuildSession::addDiscreteVariable(harkenstudio::ObjectId variableId)
{
    impl_->discreteVariables.push_back(variableId);
}

void PackBuildSession::addMeterWiring(const PackMeterWiring& wiring)
{
    impl_->meterWirings.push_back(wiring);
}

void PackBuildSession::addBinding(const PackParameterBinding& binding)
{
    impl_->bindings.push_back(binding);
}

void PackBuildSession::addBindingAttachment(const PackBindingAttachment& attachment)
{
    impl_->bindingAttachments.push_back(attachment);
}

void PackBuildSession::addStringMapping(harkenstudio::ObjectId objectId, const QString& name)
{
    impl_->stringMappings.emplace_back(objectId, name);
}

void PackBuildSession::addObjectMetadata(harkenstudio::ObjectId ownerObjectId,
                                         const QByteArray& guidRfc4122,
                                         const QString& objectPath)
{
    impl_->objectMetadata.push_back({ownerObjectId, guidRfc4122, objectPath});
}

void PackBuildSession::addWarning(const QString& warning)
{
    if (!warning.isEmpty() && !impl_->pendingWarnings.contains(warning))
        impl_->pendingWarnings.append(warning);
}

void PackBuildSession::addMedia(const PackMediaDesc& media)
{
    if (harkenstudio::IsNull(media.sourceId))
        return;
    Impl::MediaEntry entry;
    entry.id = media.sourceId;
    entry.authoringSourceId = media.authoringSourceId;
    entry.path = media.sourcePath;
    entry.downmixToMono = media.downmixToMono;
    entry.loopable = media.loopable;
    entry.loopStartFrame = media.loopStartFrame;
    entry.loopEndFrame = media.loopEndFrame;
    impl_->media.push_back(std::move(entry));
}

namespace {

std::uint64_t alignUp(std::uint64_t value, std::uint64_t align)
{
    return (value + align - 1) / align * align;
}

bool assembleCanonicalPack(const std::vector<PackBuildSession::CanonicalPackEntry>& entries,
                           PackBuildSession::CanonicalPackKind packKind,
                           std::vector<std::uint8_t>* out,
                           QString* error)
{
    const std::uint64_t dirOffset = harkenstudiobuild::kPackV2HeaderBytes;
    const std::uint64_t dirBytes =
        static_cast<std::uint64_t>(entries.size()) * harkenstudiobuild::kPackV2DirectoryEntryBytes;
    std::uint64_t cursor = alignUp(dirOffset + dirBytes, harkenstudiobuild::kPackV2PayloadAlign);
    std::vector<std::uint64_t> offsets(entries.size(), 0);
    std::vector<std::uint64_t> payloadSizes(entries.size(), 0);
    std::uint64_t contentEnd = dirOffset + dirBytes;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        offsets[i] = cursor;
        payloadSizes[i] = entries[i].kind == harkenstudiobuild::kPackEntryKindMedia
            ? harkenstudiobuild::kPackV2MediaPayloadBytes
            : entries[i].payload.size();
        contentEnd = cursor + payloadSizes[i];
        cursor = alignUp(contentEnd, harkenstudiobuild::kPackV2PayloadAlign);
    }
    std::vector<std::uint64_t> blobOffsets(entries.size(), 0);
    std::uint64_t fileBytes = contentEnd;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].kind != harkenstudiobuild::kPackEntryKindMedia)
            continue;
        if (entries[i].payload.size() < harkenstudiobuild::kPackV2MediaPayloadBytes) {
            if (error)
                *error = QStringLiteral("Media entry is missing its TOC payload");
            return false;
        }
        blobOffsets[i] = alignUp(fileBytes, harkenstudiobuild::kPackBlobAlign);
        fileBytes = blobOffsets[i] +
                    (entries[i].payload.size() - harkenstudiobuild::kPackV2MediaPayloadBytes);
    }
    out->assign(static_cast<std::size_t>(fileBytes), 0);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].payload.empty())
            continue;
        if (entries[i].kind == harkenstudiobuild::kPackEntryKindMedia) {
            harkenstudiobuild::PackTocEntry toc =
                harkenstudiobuild::ParsePackTocEntry(entries[i].payload.data());
            const std::uint64_t blobBytes =
                entries[i].payload.size() - harkenstudiobuild::kPackV2MediaPayloadBytes;
            toc.blobOffset = blobOffsets[i];
            toc.blobBytes = blobBytes;
            toc.blobChecksum = harkenstudiobuild::Fnv1a64(
                entries[i].payload.data() + harkenstudiobuild::kPackV2MediaPayloadBytes,
                static_cast<std::size_t>(blobBytes));
            harkenstudiobuild::SerializePackTocEntry(
                toc, out->data() + static_cast<std::size_t>(offsets[i]));
            if (blobBytes != 0) {
                std::memcpy(out->data() + static_cast<std::size_t>(blobOffsets[i]),
                            entries[i].payload.data() + harkenstudiobuild::kPackV2MediaPayloadBytes,
                            static_cast<std::size_t>(blobBytes));
            }
        } else {
            std::memcpy(out->data() + static_cast<std::size_t>(offsets[i]),
                        entries[i].payload.data(), entries[i].payload.size());
        }
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        harkenstudiobuild::PackV2DirectoryEntry record;
        record.kind = entries[i].kind;
        record.vintage = entries[i].vintage;
        record.id = entries[i].id;
        record.relationOrdinal = entries[i].relationOrdinal;
        record.offset = offsets[i];
        record.size = payloadSizes[i];
        record.payloadChecksum =
            harkenstudiobuild::Fnv1a64(out->data() + static_cast<std::size_t>(offsets[i]),
                           static_cast<std::size_t>(record.size));
        harkenstudiobuild::SerializePackV2DirectoryEntry(
            record, out->data() + static_cast<std::size_t>(dirOffset) +
                        i * harkenstudiobuild::kPackV2DirectoryEntryBytes);
    }
    harkenstudiobuild::PackV2Header header;
    header.formatVersion = harkenstudiobuild::kPackFormatVersion2;
    header.headerBytes = harkenstudiobuild::kPackV2HeaderBytes;
    header.fileBytes = fileBytes;
    header.packKind = static_cast<std::uint32_t>(packKind);
    header.entryCount = static_cast<std::uint32_t>(entries.size());
    header.directoryOffset = dirOffset;
    header.directoryChecksum =
        harkenstudiobuild::Fnv1a64(out->data() + static_cast<std::size_t>(dirOffset),
                       static_cast<std::size_t>(dirBytes));
    harkenstudiobuild::SerializePackV2Header(header, out->data());
    return true;
}

bool writeBytesAtomically(const QString& outputPath, const std::vector<std::uint8_t>& bytes,
                          QString* error)
{
    QDir().mkpath(QFileInfo(outputPath).absolutePath());
    QFile file(outputPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = QStringLiteral("Cannot write pack '%1'").arg(outputPath);
        return false;
    }
    const qint64 wrote = file.write(reinterpret_cast<const char*>(bytes.data()),
                                    static_cast<qint64>(bytes.size()));
    if (wrote != static_cast<qint64>(bytes.size()) || !file.flush()) {
        if (error)
            *error = QStringLiteral("Failed writing pack '%1'").arg(outputPath);
        return false;
    }
    return true;
}

bool collectExistingEntries(const QString& sourcePath,
                            std::vector<PackBuildSession::CanonicalPackEntry>* entries,
                            QString* error)
{
    QFile file(sourcePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("Cannot read pack '%1'").arg(sourcePath);
        return false;
    }
    const QByteArray bytes = file.readAll();
    if (bytes.size() < static_cast<int>(harkenstudiobuild::kPackV2HeaderBytes)) {
        if (error)
            *error = QStringLiteral("Pack '%1' is too small").arg(sourcePath);
        return false;
    }
    harkenstudiobuild::PackV2Header header;
    if (!harkenstudiobuild::ParsePackV2Header(reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                                  &header) ||
        header.formatVersion != harkenstudiobuild::kPackFormatVersion2) {
        if (error)
            *error = QStringLiteral("Pack '%1' is not a v2 container").arg(sourcePath);
        return false;
    }
    for (std::uint32_t i = 0; i < header.entryCount; ++i) {
        const harkenstudiobuild::PackV2DirectoryEntry record = harkenstudiobuild::ParsePackV2DirectoryEntry(
            reinterpret_cast<const std::uint8_t*>(bytes.constData()) +
            header.directoryOffset + i * harkenstudiobuild::kPackV2DirectoryEntryBytes);
        PackBuildSession::CanonicalPackEntry entry;
        entry.kind = record.kind;
        entry.vintage = record.vintage;
        entry.id = record.id;
        entry.relationOrdinal = record.relationOrdinal;
        entry.payload.assign(
            reinterpret_cast<const std::uint8_t*>(bytes.constData()) + record.offset,
            reinterpret_cast<const std::uint8_t*>(bytes.constData()) + record.offset +
                record.size);
        if (record.kind == harkenstudiobuild::kPackEntryKindMedia) {
            const harkenstudiobuild::PackTocEntry toc = harkenstudiobuild::ParsePackTocEntry(entry.payload.data());
            entry.payload.resize(static_cast<std::size_t>(record.size + toc.blobBytes));
            std::memcpy(entry.payload.data() + record.size,
                        reinterpret_cast<const std::uint8_t*>(bytes.constData()) +
                            toc.blobOffset,
                        static_cast<std::size_t>(toc.blobBytes));
        }
        entries->push_back(std::move(entry));
    }
    return true;
}

} // namespace

bool PackBuildSession::writeCanonicalPackFile(
    const QString& outputPath, CanonicalPackKind packKind,
    const std::vector<CanonicalPackEntry>& entries, QString* error)
{
    std::vector<std::uint8_t> bytes;
    if (!assembleCanonicalPack(entries, packKind, &bytes, error))
        return false;
    return writeBytesAtomically(outputPath, bytes, error);
}

bool PackBuildSession::wrapCanonicalPackFile(
    const QString& sourcePath, const QString& outputPath, CanonicalPackKind packKind,
    const std::vector<CanonicalPackEntry>& extraEntries, QString* error)
{
    std::vector<CanonicalPackEntry> entries;
    if (!sourcePath.isEmpty() && QFileInfo::exists(sourcePath) &&
        !collectExistingEntries(sourcePath, &entries, error))
        return false;
    entries.insert(entries.end(), extraEntries.begin(), extraEntries.end());
    return writeCanonicalPackFile(outputPath, packKind, entries, error);
}

bool PackBuildSession::buildToFile(const QString& outputPath)
{
    impl_->lastError.clear();
    impl_->warnings = impl_->pendingWarnings;
    const QString packc = findPackcExecutable();
    if (packc.isEmpty())
    {
        impl_->lastError = QStringLiteral("harken_packc executable was not found");
        return false;
    }

    QDir().mkpath(QFileInfo(outputPath).absolutePath());
    QTemporaryDir workDir(QDir(QFileInfo(outputPath).absolutePath())
                              .filePath(QStringLiteral(".packc_XXXXXX")));
    if (!workDir.isValid())
    {
        impl_->lastError = QStringLiteral("Cannot create packc work directory near %1")
                               .arg(outputPath);
        return false;
    }

    const QString manifestPath = QDir(workDir.path()).filePath(QStringLiteral("manifest.json"));
    if (!impl_->writeManifest(manifestPath, outputPath))
        return false;

    QProcess process;
    process.setProgram(packc);
    process.setArguments({manifestPath});
    process.start();
    if (!process.waitForStarted())
    {
        impl_->lastError = QStringLiteral("Cannot start harken_packc: %1").arg(packc);
        return false;
    }
    if (!process.waitForFinished(300000))
    {
        process.kill();
        process.waitForFinished();
        impl_->lastError = QStringLiteral("harken_packc timed out for %1").arg(outputPath);
        return false;
    }

    const int exitCode = process.exitCode();
    const QString processText = quoteProcessError(process);
    if (process.exitStatus() != QProcess::NormalExit || exitCode != 0)
    {
        QString reportText;
        QFile report(outputPath + QStringLiteral(".report.json"));
        if (report.open(QIODevice::ReadOnly | QIODevice::Text))
            reportText = QString::fromUtf8(report.readAll()).trimmed();
        impl_->lastError = !reportText.isEmpty()
            ? reportText
            : (processText.isEmpty()
                   ? QStringLiteral("harken_packc failed with exit code %1").arg(exitCode)
                   : processText);
        return false;
    }

    if (!QFileInfo::exists(outputPath))
    {
        impl_->lastError = QStringLiteral("harken_packc did not create output: %1").arg(outputPath);
        return false;
    }
    return true;
}

QString PackBuildSession::lastError() const
{
    return impl_->lastError;
}

QStringList PackBuildSession::warnings() const
{
    return impl_->warnings;
}

} // namespace Bridge
} // namespace HarkenEditor
