#include "CacheArtifactWriter.h"
#include "Tooling/Harkenizer/Core/VorbisEncoder.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QTemporaryFile>
#include <limits>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cstdio>
#endif

namespace harken::studio {

namespace {

constexpr quint32 kVorbisCacheMagic = 0x314d4356;
constexpr qsizetype kVorbisHeaderBytes = 104;
constexpr qsizetype kDigestBytes = 32;

QByteArray vorbisDigest(QByteArrayView header, QByteArrayView payload)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(header);
    hash.addData(payload);
    return hash.result();
}

bool cacheError(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

struct VorbisCacheHeader
{
    quint32 sampleRate, channels, smallBlockFrames, largeBlockFrames, seekGrainFrames;
    quint64 frameCount, prerollFrames, tailFrames, loopStartFrame, loopEndFrame;
    quint64 revision = 0;
    quint64 payloadBytes = 0;
    quint32 encoderBytes = 0;
    quint32 identificationBytes = 0;
    quint32 setupBytes = 0;
    quint32 packetCount = 0;
    quint32 seekCount = 0;
};

bool readVorbisHeader(const QByteArray& bytes, qint64 fileSize, VorbisCacheHeader& header)
{
    if (bytes.size() != kVorbisHeaderBytes || fileSize < kVorbisHeaderBytes + kDigestBytes) return false;
    QDataStream fields(bytes);
    fields.setByteOrder(QDataStream::LittleEndian);
    quint32 magic = 0, version = 0;
    fields >> magic >> version >> header.sampleRate >> header.channels >> header.smallBlockFrames
           >> header.largeBlockFrames >> header.seekGrainFrames >> header.frameCount
           >> header.prerollFrames >> header.tailFrames >> header.loopStartFrame >> header.loopEndFrame
           >> header.revision >> header.encoderBytes >> header.identificationBytes >> header.setupBytes
           >> header.packetCount >> header.seekCount >> header.payloadBytes;
    const auto minimumBytes = static_cast<uint64_t>(header.encoderBytes) + header.identificationBytes
        + header.setupBytes + static_cast<uint64_t>(header.packetCount) * 13
        + static_cast<uint64_t>(header.seekCount) * 28;
    if (fields.status() != QDataStream::Ok || magic != kVorbisCacheMagic
        || version != VorbisEncoder::kFormatVersion || header.channels == 0 || header.channels > 8
        || !header.sampleRate || !header.frameCount || !header.seekGrainFrames
        || header.frameCount > static_cast<quint64>(std::numeric_limits<qint64>::max())
        || (header.loopEndFrame == 0 ? header.loopStartFrame != 0
            : header.loopStartFrame >= header.loopEndFrame || header.loopEndFrame > header.frameCount)
        || !header.encoderBytes || !header.identificationBytes || !header.setupBytes
        || !header.packetCount || !header.seekCount
        || header.payloadBytes != static_cast<uint64_t>(fileSize - kVorbisHeaderBytes - kDigestBytes)
        || minimumBytes > header.payloadBytes) return false;
    return true;
}

}

bool writeCacheArtifact(const QString& path, const std::function<bool(QIODevice&)>& write)
{
    const QFileInfo destination(path);
    if (!QFileInfo(destination.absolutePath()).isDir()) {
        static QMutex directoryCreation;
        QMutexLocker lock(&directoryCreation);
        if (!QDir().mkpath(destination.absolutePath())) return false;
    }
    QString temporaryPath;
    {
        QTemporaryFile temporary(destination.absoluteFilePath() + QStringLiteral(".XXXXXX"));
        if (!temporary.open() || !write(temporary) || !temporary.flush()) return false;
        temporaryPath = temporary.fileName();
        temporary.setAutoRemove(false);
    }
#ifdef Q_OS_WIN
    const auto windowsPath = [](const QString& path) {
        const auto native = QDir::toNativeSeparators(path);
        if (native.startsWith(QStringLiteral("\\\\?\\"))) return native;
        return native.startsWith(QStringLiteral("\\\\"))
            ? QStringLiteral("\\\\?\\UNC\\") + native.mid(2) : QStringLiteral("\\\\?\\") + native;
    };
    const auto sourcePath = windowsPath(temporaryPath);
    const auto targetPath = windowsPath(destination.absoluteFilePath());
    const bool replaced = MoveFileExW(
        reinterpret_cast<LPCWSTR>(sourcePath.utf16()),
        reinterpret_cast<LPCWSTR>(targetPath.utf16()),
        MOVEFILE_REPLACE_EXISTING) != 0;
#else
    const bool replaced = std::rename(QFile::encodeName(temporaryPath).constData(),
        QFile::encodeName(destination.absoluteFilePath()).constData()) == 0;
#endif
    if (!replaced) QFile::remove(temporaryPath);
    return replaced;
}

QByteArray encodeVorbisCacheArtifact(const VorbisEncoder::EncodedMedia& media,
    uint64_t mediaRevision, QString* contentHash, QString* error, uint64_t* outputBytes)
{
    if (!VorbisEncoder::validate(media, error)) return {};
    const auto encoderVersion = media.encoderVersion.toUtf8();
    const auto fitsLength = [](auto size) {
        return static_cast<uint64_t>(size) <= std::numeric_limits<quint32>::max();
    };
    if (!fitsLength(encoderVersion.size()) || !fitsLength(media.identificationHeader.size())
        || !fitsLength(media.setupHeader.size()) || !fitsLength(media.packets.size())
        || !fitsLength(media.seekTable.size())) {
        cacheError(error, QStringLiteral("Vorbis cache exceeds format limits"));
        return {};
    }
    QByteArray payload;
    QDataStream data(&payload, QIODevice::WriteOnly);
    data.setByteOrder(QDataStream::LittleEndian);
    data.device()->write(encoderVersion);
    data.device()->write(media.identificationHeader);
    data.device()->write(media.setupHeader);
    for (const auto& packet : media.packets) {
        if (!fitsLength(packet.bytes.size())) {
            cacheError(error, QStringLiteral("Vorbis packet exceeds cache format limits"));
            return {};
        }
        data << static_cast<quint32>(packet.bytes.size()) << static_cast<qint64>(packet.granuleFrame);
        data.device()->write(packet.bytes);
    }
    for (const auto& seek : media.seekTable)
        data << static_cast<quint64>(seek.outputFrame) << static_cast<quint32>(seek.packetIndex)
             << static_cast<quint64>(seek.byteOffset) << static_cast<quint64>(seek.discardFrames);
    QByteArray header;
    QDataStream fields(&header, QIODevice::WriteOnly);
    fields.setByteOrder(QDataStream::LittleEndian);
    fields << kVorbisCacheMagic << static_cast<quint32>(VorbisEncoder::kFormatVersion)
           << static_cast<quint32>(media.sampleRate) << static_cast<quint32>(media.channels)
           << static_cast<quint32>(media.smallBlockFrames) << static_cast<quint32>(media.largeBlockFrames)
           << static_cast<quint32>(media.seekGrainFrames)
           << static_cast<quint64>(media.frameCount) << static_cast<quint64>(media.prerollFrames)
           << static_cast<quint64>(media.tailFrames) << static_cast<quint64>(media.loopStartFrame)
           << static_cast<quint64>(media.loopEndFrame) << static_cast<quint64>(mediaRevision)
           << static_cast<quint32>(encoderVersion.size())
           << static_cast<quint32>(media.identificationHeader.size())
           << static_cast<quint32>(media.setupHeader.size())
           << static_cast<quint32>(media.packets.size()) << static_cast<quint32>(media.seekTable.size())
           << static_cast<quint64>(payload.size());
    if (data.status() != QDataStream::Ok || fields.status() != QDataStream::Ok) {
        cacheError(error, QStringLiteral("Could not serialize Vorbis cache"));
        return {};
    }
    const auto digest = vorbisDigest(header, payload);
    if (contentHash) *contentHash = QString::fromLatin1(digest.toHex());
    if (outputBytes) *outputBytes = static_cast<uint64_t>(payload.size());
    return header + digest + payload;
}

std::optional<CachedMediaProperties> inspectVorbisCacheArtifact(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Unbuffered)) return std::nullopt;
    VorbisCacheHeader header;
    if (!readVorbisHeader(file.read(kVorbisHeaderBytes), file.size(), header)) return std::nullopt;
    return CachedMediaProperties{path, header.sampleRate, static_cast<uint16_t>(header.channels),
        header.frameCount, header.payloadBytes};
}

bool readVorbisCacheArtifact(const QString& path, VorbisEncoder::EncodedMedia& media,
    uint64_t& mediaRevision, QString* contentHash, QString* error, uint64_t* outputBytes)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return cacheError(error, file.errorString());
    const auto invalid = [&] { return cacheError(error, QStringLiteral("Invalid Vorbis cache: %1").arg(path)); };
    const auto headerBytes = file.read(kVorbisHeaderBytes);
    VorbisCacheHeader header;
    if (!readVorbisHeader(headerBytes, file.size(), header)) return invalid();
    const auto digest = file.read(kDigestBytes);
    const auto payload = file.readAll();
    if (static_cast<uint64_t>(payload.size()) != header.payloadBytes
        || vorbisDigest(headerBytes, payload) != digest) return invalid();
    VorbisEncoder::EncodedMedia decoded;
    decoded.sampleRate = header.sampleRate;
    decoded.channels = static_cast<uint16_t>(header.channels);
    decoded.smallBlockFrames = header.smallBlockFrames;
    decoded.largeBlockFrames = header.largeBlockFrames;
    decoded.seekGrainFrames = header.seekGrainFrames;
    decoded.frameCount = header.frameCount;
    decoded.prerollFrames = header.prerollFrames;
    decoded.tailFrames = header.tailFrames;
    decoded.loopStartFrame = header.loopStartFrame;
    decoded.loopEndFrame = header.loopEndFrame;
    QDataStream data(payload);
    data.setByteOrder(QDataStream::LittleEndian);
    decoded.encoderVersion = QString::fromUtf8(data.device()->read(header.encoderBytes));
    decoded.identificationHeader = data.device()->read(header.identificationBytes);
    decoded.setupHeader = data.device()->read(header.setupBytes);
    decoded.packets.reserve(header.packetCount);
    for (quint32 i = 0; i < header.packetCount; ++i) {
        quint32 count = 0;
        qint64 granule = 0;
        data >> count >> granule;
        if (data.status() != QDataStream::Ok || count == 0 || count > data.device()->bytesAvailable())
            return invalid();
        decoded.packets.push_back({data.device()->read(count), granule});
    }
    if (static_cast<uint64_t>(data.device()->bytesAvailable()) != static_cast<uint64_t>(header.seekCount) * 28)
        return invalid();
    decoded.seekTable.reserve(header.seekCount);
    for (quint32 i = 0; i < header.seekCount; ++i) {
        quint64 outputFrame = 0, byteOffset = 0, discardFrames = 0;
        quint32 packetIndex = 0;
        data >> outputFrame >> packetIndex >> byteOffset >> discardFrames;
        decoded.seekTable.push_back({outputFrame, packetIndex, byteOffset, discardFrames});
    }
    if (data.status() != QDataStream::Ok || !data.atEnd() || !VorbisEncoder::validate(decoded, error))
        return invalid();
    media = std::move(decoded);
    mediaRevision = header.revision;
    if (contentHash) *contentHash = QString::fromLatin1(digest.toHex());
    if (outputBytes) *outputBytes = header.payloadBytes;
    return true;
}

}
