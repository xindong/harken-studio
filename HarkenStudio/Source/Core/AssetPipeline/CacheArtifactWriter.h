#pragma once

#include <QString>
#include <QByteArray>
#include <cstdint>
#include <functional>

class QIODevice;

namespace VNX {

namespace VorbisEncoder { struct EncodedMedia; }

bool writeCacheArtifact(const QString& path, const std::function<bool(QIODevice&)>& write);
QByteArray encodeVorbisCacheArtifact(const VorbisEncoder::EncodedMedia& media,
    uint64_t mediaRevision, QString* contentHash = nullptr, QString* error = nullptr);
bool readVorbisCacheArtifact(const QString& path, VorbisEncoder::EncodedMedia& media,
    uint64_t& mediaRevision, QString* contentHash = nullptr, QString* error = nullptr);

}
