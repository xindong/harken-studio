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

namespace VNX {

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

bool readVorbisCacheArtifact(const QString& path, VorbisEncoder::EncodedMedia& media,
    uint64_t& mediaRevision, QString* contentHash, QString* error, uint64_t* outputBytes)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return cacheError(error, file.errorString());
    const auto bytes = file.readAll();
    const auto invalid = [&] { return cacheError(error, QStringLiteral("Invalid Vorbis cache: %1").arg(path)); };
    if (bytes.size() < kVorbisHeaderBytes + kDigestBytes || bytes.size() != file.size()) return invalid();
    const auto header = bytes.first(kVorbisHeaderBytes);
    const auto digest = bytes.mid(kVorbisHeaderBytes, kDigestBytes);
    const auto payload = bytes.sliced(kVorbisHeaderBytes + kDigestBytes);
    if (vorbisDigest(header, payload) != digest) return invalid();
    QDataStream fields(header);
    fields.setByteOrder(QDataStream::LittleEndian);
    quint32 magic = 0, version = 0, sampleRate = 0, channels = 0, smallBlock = 0, largeBlock = 0, grain = 0;
    quint64 frames = 0, preroll = 0, tail = 0, loopStart = 0, loopEnd = 0, revision = 0, payloadBytes = 0;
    quint32 encoderBytes = 0, identificationBytes = 0, setupBytes = 0, packetCount = 0, seekCount = 0;
    fields >> magic >> version >> sampleRate >> channels >> smallBlock >> largeBlock >> grain
           >> frames >> preroll >> tail >> loopStart >> loopEnd >> revision
           >> encoderBytes >> identificationBytes >> setupBytes >> packetCount >> seekCount >> payloadBytes;
    const auto minimumBytes = static_cast<uint64_t>(encoderBytes) + identificationBytes + setupBytes
        + static_cast<uint64_t>(packetCount) * 13 + static_cast<uint64_t>(seekCount) * 28;
    if (fields.status() != QDataStream::Ok || magic != kVorbisCacheMagic
        || version != VorbisEncoder::kFormatVersion || channels > 8
        || payloadBytes != static_cast<uint64_t>(payload.size()) || minimumBytes > payloadBytes)
        return invalid();
    VorbisEncoder::EncodedMedia decoded;
    decoded.sampleRate = sampleRate;
    decoded.channels = static_cast<uint16_t>(channels);
    decoded.smallBlockFrames = smallBlock;
    decoded.largeBlockFrames = largeBlock;
    decoded.seekGrainFrames = grain;
    decoded.frameCount = frames;
    decoded.prerollFrames = preroll;
    decoded.tailFrames = tail;
    decoded.loopStartFrame = loopStart;
    decoded.loopEndFrame = loopEnd;
    QDataStream data(payload);
    data.setByteOrder(QDataStream::LittleEndian);
    decoded.encoderVersion = QString::fromUtf8(data.device()->read(encoderBytes));
    decoded.identificationHeader = data.device()->read(identificationBytes);
    decoded.setupHeader = data.device()->read(setupBytes);
    decoded.packets.reserve(packetCount);
    for (quint32 i = 0; i < packetCount; ++i) {
        quint32 count = 0;
        qint64 granule = 0;
        data >> count >> granule;
        if (data.status() != QDataStream::Ok || count == 0 || count > data.device()->bytesAvailable())
            return invalid();
        decoded.packets.push_back({data.device()->read(count), granule});
    }
    if (static_cast<uint64_t>(data.device()->bytesAvailable()) != static_cast<uint64_t>(seekCount) * 28)
        return invalid();
    decoded.seekTable.reserve(seekCount);
    for (quint32 i = 0; i < seekCount; ++i) {
        quint64 outputFrame = 0, byteOffset = 0, discardFrames = 0;
        quint32 packetIndex = 0;
        data >> outputFrame >> packetIndex >> byteOffset >> discardFrames;
        decoded.seekTable.push_back({outputFrame, packetIndex, byteOffset, discardFrames});
    }
    if (data.status() != QDataStream::Ok || !data.atEnd() || !VorbisEncoder::validate(decoded, error))
        return invalid();
    media = std::move(decoded);
    mediaRevision = revision;
    if (contentHash) *contentHash = QString::fromLatin1(digest.toHex());
    if (outputBytes) *outputBytes = payloadBytes;
    return true;
}

}
