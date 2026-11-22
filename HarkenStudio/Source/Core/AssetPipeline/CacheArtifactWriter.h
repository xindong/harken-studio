#pragma once

#include <QString>
#include <QByteArray>
#include <cstdint>
#include <functional>
#include <optional>

class QIODevice;

namespace harken::studio {

namespace VorbisEncoder { struct EncodedMedia; }

struct CachedMediaProperties
{
    QString path;
    uint32_t sampleRate;
    uint16_t channels;
    uint64_t frames;
    uint64_t outputBytes;
};

bool writeCacheArtifact(const QString& path, const std::function<bool(QIODevice&)>& write);
std::optional<CachedMediaProperties> inspectVorbisCacheArtifact(const QString& path);
QByteArray encodeVorbisCacheArtifact(const VorbisEncoder::EncodedMedia& media,
    uint64_t mediaRevision, QString* contentHash = nullptr, QString* error = nullptr,
    uint64_t* outputBytes = nullptr);
bool readVorbisCacheArtifact(const QString& path, VorbisEncoder::EncodedMedia& media,
    uint64_t& mediaRevision, QString* contentHash = nullptr, QString* error = nullptr,
    uint64_t* outputBytes = nullptr);

}
