
#pragma once

#include <QString>
#include <QStringList>
#include <QHash>
#include <QJsonObject>
#include <QJsonArray>
#include <QUuid>
#include <cstdint>
#include <vector>

#include "AuthoringPlatform.h"

#include "Core/Build/PackagingBridge.h"

namespace VNX {

inline constexpr uint32_t kMediaCacheArtifactVersion = 5;

using Platform = AuthoringPlatform;

inline QString platformToString(Platform p) {
    const auto name = AuthoringPlatformName(p);
    return QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size()));
}

inline Platform platformFromString(const QString& s) {
    return ParseAuthoringPlatform(s.toStdString())
        .value_or(Platform::Universal);
}

inline QStringList knownTargetPlatformNames() {
    QStringList names;
    for (const auto platform : kTargetAuthoringPlatforms)
        names.append(platformToString(platform));
    return names;
}

inline QStringList normalizeTargetPlatforms(const QStringList& names) {
    static const QHash<QString, QStringList> legacyAliases = {
        {QStringLiteral("Windows / Mac / Linux"),
         {QStringLiteral("Windows"), QStringLiteral("macOS"), QStringLiteral("Linux")}},
        {QStringLiteral("Android / iOS"),
         {QStringLiteral("Android"), QStringLiteral("iOS")}},
        {QStringLiteral("Web / HTML5"), {}},
    };
    QStringList out;
    const auto push = [&out](const QString& name) {
        if (!out.contains(name))
            out << name;
    };
    for (const QString& raw : names) {
        const QString name = raw.trimmed();
        const auto it = legacyAliases.constFind(name);
        if (it != legacyAliases.constEnd()) {
            for (const QString& mapped : *it)
                push(mapped);
            continue;
        }
        if (platformToString(platformFromString(name)) == name)
            push(name);
    }
    return out;
}

struct LoopInfo {
    bool enabled = false;
    uint64_t loopStart = 0;
    uint64_t loopEnd = 0;
    uint32_t loopCount = 0;

    QJsonObject toJson() const {
        QJsonObject obj;
        obj["enabled"] = enabled;
        obj["loopStart"] = static_cast<qint64>(loopStart);
        obj["loopEnd"] = static_cast<qint64>(loopEnd);
        obj["loopCount"] = static_cast<qint64>(loopCount);
        return obj;
    }

    static LoopInfo fromJson(const QJsonObject& obj) {
        LoopInfo info;
        info.enabled = obj["enabled"].toBool();
        info.loopStart = static_cast<uint64_t>(obj["loopStart"].toInteger());
        info.loopEnd = static_cast<uint64_t>(obj["loopEnd"].toInteger());
        info.loopCount = static_cast<uint32_t>(obj["loopCount"].toInteger());
        return info;
    }
};

struct ValidationFlags {
    bool hasClipping = false;
    bool hasDCOffset = false;
    float peakDb = 0.0f;
    float rmsDb = 0.0f;
    float truePeakDb = 0.0f;
    float integratedLUFS = -100.0f;

    QJsonObject toJson() const {
        QJsonObject obj;
        obj["hasClipping"] = hasClipping;
        obj["hasDCOffset"] = hasDCOffset;
        obj["peakDb"] = static_cast<double>(peakDb);
        obj["rmsDb"] = static_cast<double>(rmsDb);
        obj["truePeakDb"] = static_cast<double>(truePeakDb);
        obj["integratedLUFS"] = static_cast<double>(integratedLUFS);
        return obj;
    }

    static ValidationFlags fromJson(const QJsonObject& obj) {
        ValidationFlags flags;
        flags.hasClipping = obj["hasClipping"].toBool();
        flags.hasDCOffset = obj["hasDCOffset"].toBool();
        flags.peakDb = static_cast<float>(obj["peakDb"].toDouble());
        flags.rmsDb = static_cast<float>(obj["rmsDb"].toDouble());
        flags.truePeakDb = static_cast<float>(obj["truePeakDb"].toDouble());
        flags.integratedLUFS = static_cast<float>(obj["integratedLUFS"].toDouble(-100.0));
        return flags;
    }
};

struct AudioMetadata {
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint16_t bitDepth = 0;
    uint64_t totalSamples = 0;
    double durationSec = 0.0;
    uint64_t fileSize = 0;

    LoopInfo loopInfo;
    ValidationFlags validation;

    QString originalFormat;
    QString codec;

    QJsonObject toJson() const {
        QJsonObject obj;
        obj["sampleRate"] = static_cast<qint64>(sampleRate);
        obj["channels"] = channels;
        obj["bitDepth"] = bitDepth;
        obj["totalSamples"] = static_cast<qint64>(totalSamples);
        obj["durationSec"] = durationSec;
        obj["fileSize"] = static_cast<qint64>(fileSize);
        obj["loopInfo"] = loopInfo.toJson();

        obj["validation"] = validation.toJson();
        obj["originalFormat"] = originalFormat;
        obj["codec"] = codec;
        return obj;
    }

    static AudioMetadata fromJson(const QJsonObject& obj) {
        AudioMetadata meta;
        meta.sampleRate = static_cast<uint32_t>(obj["sampleRate"].toInteger());
        meta.channels = static_cast<uint16_t>(obj["channels"].toInt());
        meta.bitDepth = static_cast<uint16_t>(obj["bitDepth"].toInt());
        meta.totalSamples = static_cast<uint64_t>(obj["totalSamples"].toInteger());
        meta.durationSec = obj["durationSec"].toDouble();
        meta.fileSize = static_cast<uint64_t>(obj["fileSize"].toInteger());
        meta.loopInfo = LoopInfo::fromJson(obj["loopInfo"].toObject());

        meta.validation = ValidationFlags::fromJson(obj["validation"].toObject());
        meta.originalFormat = obj["originalFormat"].toString();
        meta.codec = obj["codec"].toString();
        return meta;
    }
};

inline bool audioMetadataEqual(const AudioMetadata& a, const AudioMetadata& b)
{
    return a.toJson() == b.toJson();
}

struct AssetInfo {
    uint32_t sourceID = 0;
    QString relativePath;
    QString absolutePath;

    QString contentHash;
    QByteArray sourceState;
    QByteArray waveformState;

    QString ownerObjectGuid;

    AudioMetadata metadata;

    uint64_t lastModified = 0;

    bool isOrphaned = false;

    QJsonObject toJson() const {
        QJsonObject obj;
        obj["sourceID"] = static_cast<qint64>(sourceID);
        obj["relativePath"] = relativePath;
        obj["contentHash"] = contentHash;
        if (!sourceState.isEmpty()) obj["sourceState"] = QString::fromLatin1(sourceState.toHex());
        if (!waveformState.isEmpty()) obj["waveformState"] = QString::fromLatin1(waveformState.toHex());
        obj["lastModified"] = static_cast<qint64>(lastModified);
        obj["metadata"] = metadata.toJson();
        obj["isOrphaned"] = isOrphaned;
        obj["ownerObjectGuid"] = ownerObjectGuid;

        return obj;
    }

    static AssetInfo fromJson(const QJsonObject& obj) {
        AssetInfo info;
        info.sourceID = static_cast<uint32_t>(obj["sourceID"].toInteger());
        info.relativePath = obj["relativePath"].toString();
        info.contentHash = obj["contentHash"].toString();
        info.sourceState = QByteArray::fromHex(obj["sourceState"].toString().toLatin1());
        info.waveformState = QByteArray::fromHex(obj["waveformState"].toString().toLatin1());
        info.lastModified = static_cast<uint64_t>(obj["lastModified"].toInteger());
        info.metadata = AudioMetadata::fromJson(obj["metadata"].toObject());
        info.isOrphaned = obj["isOrphaned"].toBool();
        info.ownerObjectGuid = obj["ownerObjectGuid"].toString();

        return info;
    }
};

enum class AssetError {
    None = 0,
    E_ASSET_MISSING,
    E_ID_COLLISION,
    E_CONVERSION_FAILED,
    E_UNSUPPORTED_FORMAT,
    E_CORRUPTED_FILE,
    E_INVALID_METADATA
};

inline QString assetErrorToString(AssetError e) {
    switch (e) {
        case AssetError::None:               return QStringLiteral("None");
        case AssetError::E_ASSET_MISSING:    return QStringLiteral("E_ASSET_MISSING");
        case AssetError::E_ID_COLLISION:     return QStringLiteral("E_ID_COLLISION");
        case AssetError::E_CONVERSION_FAILED: return QStringLiteral("E_CONVERSION_FAILED");
        case AssetError::E_UNSUPPORTED_FORMAT: return QStringLiteral("E_UNSUPPORTED_FORMAT");
        case AssetError::E_CORRUPTED_FILE:   return QStringLiteral("E_CORRUPTED_FILE");
        case AssetError::E_INVALID_METADATA: return QStringLiteral("E_INVALID_METADATA");
        default:                             return QStringLiteral("Unknown");
    }
}

inline QString assetErrorMessage(AssetError e) {
    switch (e) {
        case AssetError::None:
            return QStringLiteral("No error");
        case AssetError::E_ASSET_MISSING:
            return QStringLiteral("Source file was deleted or moved externally");
        case AssetError::E_ID_COLLISION:
            return QStringLiteral("Hash collision detected - rename one of the files");
        case AssetError::E_CONVERSION_FAILED:
            return QStringLiteral("Audio conversion failed - check file integrity");
        case AssetError::E_UNSUPPORTED_FORMAT:
            return QStringLiteral("Unsupported audio format");
        case AssetError::E_CORRUPTED_FILE:
            return QStringLiteral("File is corrupted and cannot be decoded");
        case AssetError::E_INVALID_METADATA:
            return QStringLiteral("Invalid audio metadata (e.g., sample rate is 0)");
        default:
            return QStringLiteral("Unknown error");
    }
}

enum class MediaStatus : uint8_t {
    Available,
    Missing,
    Orphaned,
    Error
};

inline QString mediaStatusToString(MediaStatus s) {
    switch (s) {
        case MediaStatus::Available:  return QStringLiteral("Available");
        case MediaStatus::Missing:    return QStringLiteral("Missing");
        case MediaStatus::Orphaned:   return QStringLiteral("Orphaned");
        case MediaStatus::Error:      return QStringLiteral("Error");
        default:                      return QStringLiteral("Unknown");
    }
}

inline MediaStatus mediaStatusFromString(const QString& s) {
    if (s == QStringLiteral("Available"))  return MediaStatus::Available;
    if (s == QStringLiteral("Missing"))    return MediaStatus::Missing;
    if (s == QStringLiteral("Orphaned"))   return MediaStatus::Orphaned;
    if (s == QStringLiteral("Error"))      return MediaStatus::Error;
    return MediaStatus::Available;
}

}

inline size_t qHash(VNX::Platform key, size_t seed = 0) {
    return qHash(static_cast<uint8_t>(key), seed);
}

Q_DECLARE_METATYPE(VNX::MediaStatus)
Q_DECLARE_METATYPE(VNX::Platform)
