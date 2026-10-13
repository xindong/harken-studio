
#pragma once

#include "AssetTypes.h"
#include <QJsonObject>
#include <QJsonArray>
#include <QString>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <optional>

namespace VNX {

struct ProjectPlatform;
struct ProjectPlatformProfile;

struct SizePolicy {
    std::optional<uint64_t> maxBytes;
    ConversionParameters config;

    QJsonObject toJson() const {
        QJsonObject obj;
        obj["maxBytes"] = maxBytes ? QJsonValue(static_cast<qint64>(*maxBytes)) : QJsonValue::Null;
        obj["config"] = config.toJson();
        return obj;
    }

    static SizePolicy fromJson(const QJsonObject& obj) {
        SizePolicy p;
        if (!obj["maxBytes"].isNull()) p.maxBytes = static_cast<uint64_t>(obj["maxBytes"].toInteger());
        p.config = ConversionParameters::fromJson(obj["config"].toObject());
        return p;
    }
};

bool validateSizePolicies(const std::vector<SizePolicy>& policies, QString* error = nullptr);

struct PlatformProfile {
    Platform platform = Platform::Universal;
    bool performResample = false;
    std::vector<SizePolicy> sizePolicies{{}};

    QJsonObject toJson() const {
        QJsonObject obj;
        obj["platform"] = platformToString(platform);
        obj["performResample"] = performResample;

        QJsonArray arr;
        for (const auto& sp : sizePolicies)
            arr.append(sp.toJson());
        obj["sizePolicies"] = arr;
        return obj;
    }

    static PlatformProfile fromJson(const QJsonObject& obj) {
        PlatformProfile p;
        p.platform = platformFromString(obj["platform"].toString());
        p.performResample = obj["performResample"].toBool();
        p.sizePolicies.clear();
        for (const auto& v : obj["sizePolicies"].toArray())
            p.sizePolicies.push_back(SizePolicy::fromJson(v.toObject()));
        return p;
    }
};

class PlatformProfileManager {
public:
    static PlatformProfileManager& instance();

    void addProfile(const PlatformProfile& profile);
    const PlatformProfile* getProfile(Platform platform) const;

    void setFileOverride(uint32_t sourceID, const ConversionSettings& config);
    void removeFileOverride(uint32_t sourceID);
    const ConversionSettings* getFileOverride(uint32_t sourceID) const;

    QHash<uint32_t, ConversionSettings> fileOverrides() const;
    static ConversionSettings effectiveSettingsForAsset(
        const AssetInfo& asset, const ProjectPlatform& platform,
        const ConversionSettings* fileOverride = nullptr);

    QJsonObject toJson() const;
    void loadFromJson(const QJsonObject& obj);

    static PlatformProfile createWindowsDefault();
    static PlatformProfile createMacOSDefault();
    static PlatformProfile createLinuxDefault();
    static PlatformProfile createAndroidDefault();
    static PlatformProfile createiOSDefault();

private:
    PlatformProfileManager();

    std::unordered_map<uint8_t, PlatformProfile> profiles_;
    std::unordered_map<uint32_t, ConversionSettings> fileOverrides_;
};

}
