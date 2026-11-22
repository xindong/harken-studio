#pragma once

#include "Core/ProjectTypes.h"

namespace HarkenTest {

inline harken::studio::ProjectPlatformId platformId(const QString& name)
{
    return QUuid::createUuidV5(QUuid(QStringLiteral("a89a43a2-1938-4900-bcec-70cc9e4c4fe3")), name);
}

namespace PlatformIds {
inline const auto Windows = platformId(QStringLiteral("Windows"));
inline const auto macOS = platformId(QStringLiteral("macOS"));
inline const auto Linux = platformId(QStringLiteral("Linux"));
inline const auto Android = platformId(QStringLiteral("Android"));
inline const auto iOS = platformId(QStringLiteral("iOS"));
inline const harken::studio::ProjectPlatformId Universal;
}

inline QVector<harken::studio::ProjectPlatform> platforms(const QStringList& names)
{
    QVector<harken::studio::ProjectPlatform> result;
    for (const auto& name : names)
        result.append({platformId(name), name,
                       harken::studio::ProjectPlatformSemantics::parseBasePlatform(name).value()});
    return result;
}

inline QString platformIdString(const QString& name)
{
    return harken::studio::ProjectPlatformSemantics::idString(platformId(name));
}

inline QStringList platformIdStrings(const QStringList& names)
{
    return harken::studio::ProjectPlatformSemantics::ids(platforms(names));
}

inline harken::studio::ProjectData projectWithPlatforms(const QStringList& names = {QStringLiteral("Windows")})
{
    harken::studio::ProjectData project;
    project.platforms = platforms(names);
    return project;
}

inline void setSharedSoundSource(harken::studio::ProjectItem& item, uint32_t sourceId)
{
    item.sourceID = sourceId;
    auto* sound = item.compOrCreate<harken::studio::VSound>();
    sound->sources.plain.entries = {{QUuid::createUuid(), sourceId}};
    sound->sources.plain.active = sound->sources.plain.entries.front().entryId;
}

}
