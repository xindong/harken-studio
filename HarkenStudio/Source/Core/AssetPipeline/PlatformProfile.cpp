
#include "PlatformProfile.h"
#include "../ProjectPlatform.h"
#include <limits>

namespace VNX {

bool validateSizePolicies(const std::vector<SizePolicy>& policies, QString* error)
{
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (policies.empty()) return fail(QStringLiteral("At least one size limit is required"));
    std::optional<uint64_t> previous;
    for (size_t i = 0; i < policies.size(); ++i) {
        const auto maximum = policies[i].maxBytes;
        if (!maximum) {
            if (i + 1 != policies.size())
                return fail(QStringLiteral("Only the last size limit may be Infinite"));
        } else {
            if (*maximum > uint64_t(std::numeric_limits<qint64>::max()))
                return fail(QStringLiteral("Size limits must fit a nonnegative 64-bit integer"));
            if (previous && *maximum <= *previous)
                return fail(QStringLiteral("Size limits must be unique and increasing"));
            if (i + 1 == policies.size())
                return fail(QStringLiteral("The last size limit must be Infinite"));
            previous = maximum;
        }
    }
    return true;
}

PlatformProfileManager& PlatformProfileManager::instance()
{
    static PlatformProfileManager mgr;
    return mgr;
}

PlatformProfileManager::PlatformProfileManager()
{
    addProfile(createWindowsDefault());
    addProfile(createMacOSDefault());
    addProfile(createLinuxDefault());
    addProfile(createAndroidDefault());
    addProfile(createiOSDefault());
}

void PlatformProfileManager::addProfile(const PlatformProfile& profile)
{
    profiles_[static_cast<uint8_t>(profile.platform)] = profile;
}

const PlatformProfile* PlatformProfileManager::getProfile(Platform platform) const
{
    auto it = profiles_.find(static_cast<uint8_t>(platform));
    return (it != profiles_.end()) ? &it->second : nullptr;
}

void PlatformProfileManager::setFileOverride(uint32_t sourceID, const ConversionSettings& config)
{
    fileOverrides_[sourceID] = config;
}

void PlatformProfileManager::removeFileOverride(uint32_t sourceID)
{
    fileOverrides_.erase(sourceID);
}

const ConversionSettings* PlatformProfileManager::getFileOverride(uint32_t sourceID) const
{
    auto it = fileOverrides_.find(sourceID);
    return (it != fileOverrides_.end()) ? &it->second : nullptr;
}

QHash<uint32_t, ConversionSettings> PlatformProfileManager::fileOverrides() const
{
    QHash<uint32_t, ConversionSettings> result;
    for (const auto& [id, settings] : fileOverrides_) result.insert(id, settings);
    return result;
}

ConversionSettings PlatformProfileManager::effectiveSettingsForAsset(
    const AssetInfo& asset, const ProjectPlatform& platform,
    const ConversionSettings* fileOverride)
{
    if (!fileOverride && asset.settings.hash() != ConversionSettings{}.hash()) fileOverride = &asset.settings;
    if (fileOverride) return {*fileOverride, platform.profile.performResample && fileOverride->performResample};
    for (const auto& policy : platform.profile.sizePolicies) {
        if (!policy.maxBytes || asset.metadata.fileSize <= *policy.maxBytes)
            return {policy.config, platform.profile.performResample};
    }
    qFatal("Platform size policies must cover all source sizes");
}

QJsonObject PlatformProfileManager::toJson() const
{
    QJsonObject obj;
    QJsonArray profileArr;
    for (const auto& [_, p] : profiles_)
        profileArr.append(p.toJson());
    obj["profiles"] = profileArr;

    QJsonObject overrides;
    for (const auto& [id, cfg] : fileOverrides_)
        overrides[QString::number(id)] = cfg.toJson();
    obj["fileOverrides"] = overrides;

    return obj;
}

void PlatformProfileManager::loadFromJson(const QJsonObject& obj)
{
    profiles_.clear();
    for (const auto& v : obj["profiles"].toArray()) {
        PlatformProfile p = PlatformProfile::fromJson(v.toObject());
        profiles_[static_cast<uint8_t>(p.platform)] = p;
    }

    fileOverrides_.clear();
    QJsonObject overrides = obj["fileOverrides"].toObject();
    for (auto it = overrides.begin(); it != overrides.end(); ++it) {
        uint32_t id = it.key().toUInt();
        fileOverrides_[id] = ConversionSettings::fromJson(it.value().toObject());
    }
}

PlatformProfile PlatformProfileManager::createWindowsDefault()
{
    PlatformProfile p;
    p.platform = Platform::Windows;
    return p;
}

PlatformProfile PlatformProfileManager::createMacOSDefault()
{
    PlatformProfile p = createWindowsDefault();
    p.platform = Platform::macOS;
    return p;
}

PlatformProfile PlatformProfileManager::createLinuxDefault()
{
    PlatformProfile p = createWindowsDefault();
    p.platform = Platform::Linux;
    return p;
}

PlatformProfile PlatformProfileManager::createAndroidDefault()
{
    PlatformProfile p = createWindowsDefault();
    p.platform = Platform::Android;
    return p;
}

PlatformProfile PlatformProfileManager::createiOSDefault()
{
    PlatformProfile p = createWindowsDefault();
    p.platform = Platform::iOS;
    return p;
}

}
