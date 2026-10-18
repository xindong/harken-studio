#pragma once

#include "Core/ProjectTypes.h"

namespace HarkenTest {

inline VNX::ProjectPlatformId platformId(const QString& name)
{
    return QUuid::createUuidV5(QUuid(QStringLiteral("a89a43a2-1938-4900-bcec-70cc9e4c4fe3")), name);
}

namespace PlatformIds {
inline const auto Windows = platformId(QStringLiteral("Windows"));
inline const auto macOS = platformId(QStringLiteral("macOS"));
inline const auto Linux = platformId(QStringLiteral("Linux"));
inline const auto Android = platformId(QStringLiteral("Android"));
inline const auto iOS = platformId(QStringLiteral("iOS"));
inline const VNX::ProjectPlatformId Universal;
}

inline QVector<VNX::ProjectPlatform> platforms(const QStringList& names)
{
    QVector<VNX::ProjectPlatform> result;
    for (const auto& name : names)
        result.append({platformId(name), name,
                       VNX::ProjectPlatformSemantics::parseBasePlatform(name).value()});
    return result;
}

inline QString platformIdString(const QString& name)
{
    return VNX::ProjectPlatformSemantics::idString(platformId(name));
}

inline QStringList platformIdStrings(const QStringList& names)
{
    return VNX::ProjectPlatformSemantics::ids(platforms(names));
}

inline VNX::ProjectData projectWithPlatforms(const QStringList& names = {QStringLiteral("Windows")})
{
    VNX::ProjectData project;
    project.platforms = platforms(names);
    return project;
}

inline void setSharedSoundSource(VNX::ProjectItem& item, uint32_t sourceId)
{
    item.sourceID = sourceId;
    auto* sound = item.compOrCreate<VNX::VSound>();
    sound->sources.plain.entries = {{QUuid::createUuid(), sourceId}};
    sound->sources.plain.active = sound->sources.plain.entries.front().entryId;
}

}
