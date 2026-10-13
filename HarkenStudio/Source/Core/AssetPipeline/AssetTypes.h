
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

#include "RuntimeBridge/PackagingBridge.h"

namespace VNX {

inline constexpr uint32_t kMediaCacheArtifactVersion = 4;

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
        return meta;
    }
};

inline bool audioMetadataEqual(const AudioMetadata& a, const AudioMetadata& b)
{
    return a.toJson() == b.toJson();
}

struct ConversionParameters {
    enum class Codec {
        PCM_16,
        PCM_Float,
        ADPCM,
        Vorbis,
        HarkenPackVorbis,
        HarkenPackOpus
    };

    enum class VorbisEncodeMode {
        VBRQuality,
        ManagedBitrate
    };

    Codec format = Codec::PCM_Float;
    float vorbisQuality = 0.6f;
    VorbisEncodeMode vorbisEncodeMode = VorbisEncodeMode::VBRQuality;
    uint32_t vorbisAverageBitrate = 0;
    uint32_t vorbisMinBitrate = 0;
    uint32_t vorbisMaxBitrate = 0;

    uint32_t targetSampleRate = 48000;

    QJsonObject toJson() const {
        QJsonObject obj;
        obj["format"] = static_cast<int>(format);
        obj["vorbisQuality"] = static_cast<double>(vorbisQuality);
        obj["vorbisEncodeMode"] = static_cast<int>(vorbisEncodeMode);
        obj["vorbisAverageBitrate"] = static_cast<qint64>(vorbisAverageBitrate);
        obj["vorbisMinBitrate"] = static_cast<qint64>(vorbisMinBitrate);
        obj["vorbisMaxBitrate"] = static_cast<qint64>(vorbisMaxBitrate);
        obj["targetSampleRate"] = static_cast<qint64>(targetSampleRate);
        return obj;
    }

    static ConversionParameters fromJson(const QJsonObject& obj) {
        ConversionParameters s;
        s.format = static_cast<Codec>(
            obj["format"].toInt(static_cast<int>(Codec::PCM_Float)));
        s.vorbisQuality = static_cast<float>(obj["vorbisQuality"].toDouble(0.6));
        s.vorbisEncodeMode = static_cast<VorbisEncodeMode>(
            obj["vorbisEncodeMode"].toInt(static_cast<int>(VorbisEncodeMode::VBRQuality)));
        s.vorbisAverageBitrate = static_cast<uint32_t>(obj["vorbisAverageBitrate"].toInteger(0));
        s.vorbisMinBitrate = static_cast<uint32_t>(obj["vorbisMinBitrate"].toInteger(0));
        s.vorbisMaxBitrate = static_cast<uint32_t>(obj["vorbisMaxBitrate"].toInteger(0));
        s.targetSampleRate = static_cast<uint32_t>(obj["targetSampleRate"].toInteger(48000));
        return s;
    }

    static QString codecToString(Codec c) {
        switch (c) {
            case Codec::PCM_16:    return QStringLiteral("PCM_16");
            case Codec::PCM_Float: return QStringLiteral("PCM_Float");
            case Codec::ADPCM:     return QStringLiteral("ADPCM");
            case Codec::Vorbis:    return QStringLiteral("Vorbis");
            case Codec::HarkenPackVorbis: return HarkenEditor::Bridge::packVorbisCodecName();
            case Codec::HarkenPackOpus: return HarkenEditor::Bridge::packOpusCodecName();
            default:               return QStringLiteral("Unknown");
        }
    }

    static Codec codecFromString(const QString& s) {
        if (s == QStringLiteral("PCM_Float")) return Codec::PCM_Float;
        if (s == QStringLiteral("ADPCM"))     return Codec::ADPCM;
        if (s == QStringLiteral("Vorbis"))    return Codec::Vorbis;
        if (s == HarkenEditor::Bridge::packVorbisCodecName()) return Codec::HarkenPackVorbis;
        if (s == HarkenEditor::Bridge::packOpusCodecName()) return Codec::HarkenPackOpus;
        return Codec::PCM_Float;
    }
};

struct ConversionSettings : ConversionParameters {
    bool performResample = false;

    ConversionSettings() = default;
    ConversionSettings(const ConversionParameters& parameters, bool resample)
        : ConversionParameters(parameters), performResample(resample) {}

    uint32_t hash() const {
        uint32_t h = 2166136261u;
        auto mix = [&h](uint32_t value) {
            h ^= value;
            h *= 16777619u;
        };

        mix(kMediaCacheArtifactVersion);
        mix(performResample ? 1u : 0u);
        if (performResample)
            mix(targetSampleRate);
        return h;
    }

    QJsonObject toJson() const {
        auto json = ConversionParameters::toJson();
        json["performResample"] = performResample;
        return json;
    }

    static ConversionSettings fromJson(const QJsonObject& json) {
        return {ConversionParameters::fromJson(json), json["performResample"].toBool()};
    }
};

struct CacheEntry {
    bool valid = false;
    uint64_t timestamp = 0;
    uint64_t lastAccessTime = 0;
    uint32_t settingsHash = 0;
    QString fingerprint;
    QString cachePath;
    LoopInfo loopInfo;

    QJsonObject toJson() const {
        QJsonObject obj;
        obj["valid"] = valid;
        obj["timestamp"] = static_cast<qint64>(timestamp);
        obj["lastAccessTime"] = static_cast<qint64>(lastAccessTime);
        obj["settingsHash"] = static_cast<qint64>(settingsHash);
        obj["fingerprint"] = fingerprint;
        obj["cachePath"] = cachePath;
        obj["loopInfo"] = loopInfo.toJson();
        return obj;
    }

    static CacheEntry fromJson(const QJsonObject& obj) {
        CacheEntry entry;
        entry.valid = obj["valid"].toBool();
        entry.timestamp = static_cast<uint64_t>(obj["timestamp"].toInteger());
        entry.lastAccessTime = static_cast<uint64_t>(obj["lastAccessTime"].toInteger());
        entry.settingsHash = static_cast<uint32_t>(obj["settingsHash"].toInteger());
        entry.fingerprint = obj["fingerprint"].toString();
        entry.cachePath = obj["cachePath"].toString();
        entry.loopInfo = LoopInfo::fromJson(obj["loopInfo"].toObject());
        return entry;
    }
};

struct AssetInfo {
    uint32_t sourceID = 0;
    QString relativePath;
    QString absolutePath;

    QString contentHash;

    QString ownerObjectGuid;

    AudioMetadata metadata;
    ConversionSettings settings;

    uint64_t lastModified = 0;
    QHash<QUuid, CacheEntry> platformCaches;

    bool isOrphaned = false;

    QJsonObject toJson() const {
        QJsonObject obj;
        obj["sourceID"] = static_cast<qint64>(sourceID);
        obj["relativePath"] = relativePath;
        obj["contentHash"] = contentHash;
        obj["lastModified"] = static_cast<qint64>(lastModified);
        obj["metadata"] = metadata.toJson();
        obj["settings"] = settings.toJson();
        obj["isOrphaned"] = isOrphaned;
        obj["ownerObjectGuid"] = ownerObjectGuid;

        QJsonObject caches;
        for (auto it = platformCaches.begin(); it != platformCaches.end(); ++it) {
            caches[it.key().toString(QUuid::WithoutBraces)] = it.value().toJson();
        }
        obj["platformCaches"] = caches;

        return obj;
    }

    static AssetInfo fromJson(const QJsonObject& obj) {
        AssetInfo info;
        info.sourceID = static_cast<uint32_t>(obj["sourceID"].toInteger());
        info.relativePath = obj["relativePath"].toString();
        info.contentHash = obj["contentHash"].toString();
        info.lastModified = static_cast<uint64_t>(obj["lastModified"].toInteger());
        info.metadata = AudioMetadata::fromJson(obj["metadata"].toObject());
        info.settings = ConversionSettings::fromJson(obj["settings"].toObject());
        info.isOrphaned = obj["isOrphaned"].toBool();
        info.ownerObjectGuid = obj["ownerObjectGuid"].toString();

        QJsonObject caches = obj["platformCaches"].toObject();
        for (auto it = caches.begin(); it != caches.end(); ++it) {
            const QUuid p = QUuid::fromString(it.key());
            if (p.isNull() || p.toString(QUuid::WithoutBraces) != it.key())
                continue;
            const auto entry = CacheEntry::fromJson(it.value().toObject());
            if (!entry.fingerprint.isEmpty()) info.platformCaches[p] = entry;
        }

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
    Imported,
    Converted,
    Missing,
    Outdated,
    Orphaned,
    Converting,
    Error
};

inline QString mediaStatusToString(MediaStatus s) {
    switch (s) {
        case MediaStatus::Imported:   return QStringLiteral("Imported");
        case MediaStatus::Converted:  return QStringLiteral("Converted");
        case MediaStatus::Missing:    return QStringLiteral("Missing");
        case MediaStatus::Outdated:   return QStringLiteral("Outdated");
        case MediaStatus::Orphaned:   return QStringLiteral("Orphaned");
        case MediaStatus::Converting: return QStringLiteral("Converting");
        case MediaStatus::Error:      return QStringLiteral("Error");
        default:                      return QStringLiteral("Unknown");
    }
}

inline MediaStatus mediaStatusFromString(const QString& s) {
    if (s == QStringLiteral("Imported"))   return MediaStatus::Imported;
    if (s == QStringLiteral("Converted"))  return MediaStatus::Converted;
    if (s == QStringLiteral("Missing"))    return MediaStatus::Missing;
    if (s == QStringLiteral("Outdated"))   return MediaStatus::Outdated;
    if (s == QStringLiteral("Orphaned"))   return MediaStatus::Orphaned;
    if (s == QStringLiteral("Converting")) return MediaStatus::Converting;
    if (s == QStringLiteral("Error"))      return MediaStatus::Error;
    return MediaStatus::Imported;
}

enum class ConversionFailurePolicy {
    AbortBuild,
    SkipAndWarn
};

inline QString conversionFailurePolicyToString(ConversionFailurePolicy policy)
{
    switch (policy) {
        case ConversionFailurePolicy::AbortBuild:
            return QStringLiteral("abort_build");
        case ConversionFailurePolicy::SkipAndWarn:
            return QStringLiteral("skip_and_warn");
    }
    return QStringLiteral("abort_build");
}

inline ConversionFailurePolicy conversionFailurePolicyFromString(const QString& value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("skip_and_warn") ||
        normalized == QStringLiteral("skip") ||
        normalized == QStringLiteral("warn"))
        return ConversionFailurePolicy::SkipAndWarn;
    return ConversionFailurePolicy::AbortBuild;
}

}

inline size_t qHash(VNX::Platform key, size_t seed = 0) {
    return qHash(static_cast<uint8_t>(key), seed);
}

Q_DECLARE_METATYPE(VNX::MediaStatus)
Q_DECLARE_METATYPE(VNX::Platform)
Q_DECLARE_METATYPE(VNX::ConversionFailurePolicy)
