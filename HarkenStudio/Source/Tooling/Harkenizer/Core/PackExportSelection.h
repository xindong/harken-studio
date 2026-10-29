#pragma once

#include "Utils/ObjectId.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <optional>
#include <vector>

namespace VNX {

struct ActionPackTargeting
{
    harkenstudio::ObjectId stableId{};
    bool allPlatforms = true;
    QStringList platformIds;
    std::optional<QString> language = std::nullopt;

    bool includesPlatform(const QString& platform) const {
        return allPlatforms || platformIds.contains(platform);
    }

    bool operator==(const ActionPackTargeting& other) const {
        return stableId == other.stableId
            && allPlatforms == other.allPlatforms
            && platformIds == other.platformIds
            && language == other.language;
    }
    bool operator!=(const ActionPackTargeting& other) const { return !(*this == other); }

    QJsonObject toJson() const {
        QJsonObject obj;
        obj["stableId"] = objectIdString(stableId);
        obj["allPlatforms"] = allPlatforms;
        QJsonArray platformArray;
        for (const auto& p : platformIds) platformArray.append(p);
        obj["platformIds"] = platformArray;
        if (language) obj["language"] = *language;
        return obj;
    }

    static ActionPackTargeting fromJson(const QJsonObject& obj) {
        ActionPackTargeting targeting;
        if (!parseObjectIdJson(obj["stableId"], &targeting.stableId)) return {};
        QStringList rawPlatforms;
        for (const auto& v : obj["platformIds"].toArray())
            rawPlatforms.append(v.toString());
        targeting.platformIds = rawPlatforms;
        targeting.allPlatforms = obj["allPlatforms"].toBool();
        if (obj.contains("language")) targeting.language = obj["language"].toString();
        return targeting;
    }
};

struct PackExportSelection
{
    QString currentPlatformId;
    QStringList selectedPlatformIds;
    std::vector<ActionPackTargeting> actionTargeting;

    bool includesActionPack(harkenstudio::ObjectId id, const QString& platform,
                            const QString& language) const;
    bool operator==(const PackExportSelection&) const = default;
};

struct ProjectData;

class PackExportSelectionStore
{
public:
    static PackExportSelectionStore& instance();

    void openProject(const ProjectData& project);
    void closeProject();
    const PackExportSelection& selection() const;
    void setSelection(const ProjectData& project, PackExportSelection selection);

private:
    void save() const;

    QString cachePath_;
    PackExportSelection selection_;
};

}
