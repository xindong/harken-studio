#pragma once

#include "ActionClosureSemantics.h"

#include <QJsonObject>
#include <QHash>
#include <QString>
#include <vector>
#include <set>

namespace VNX {

struct PackBuildVariant
{
    harkenstudio::ObjectId actionStableId = {};
    BuildVariant variant;
    ProjectPlatform platformDefinition;
    QString artifactName;
    QString relativePackPath;
    ActionClosurePlan closure;
    RuntimeCapabilityAssessment capability;
    std::vector<harkenstudio::ObjectId> dependencyOrder;
};

struct PackBuildUnit
{
    QString name;
    QString artifactKey;
    BuildVariant variant;
    ProjectPlatform platformDefinition;
    QString relativePackPath;
    std::vector<harkenstudio::ObjectId> actionStableIds;
    bool grouped = false;
};

struct PackBuildPlan
{
    std::vector<PackBuildVariant> variants;
    std::vector<PackBuildUnit> units;
    std::set<QString> sharedArtifactIds;
    std::set<harkenstudio::ObjectId> sharedObjectIds;
    std::vector<ActionClosureDiagnostic> diagnostics;
    QJsonObject groupMetadata;

    bool ready() const;
    QJsonObject toJson() const;
};

namespace PackBuildPlanning {

QHash<harkenstudio::ObjectId, QStringList> artifactLanguages(
    const ProjectData& project, const QStringList& platforms = {});
PackBuildPlan build(const ProjectData& project,
                    const QStringList& platforms = {},
                    const QStringList& languages = {},
                    bool includeUnchecked = false);
PackBuildPlan buildForAction(const ProjectData& project,
                            harkenstudio::ObjectId actionStableId,
                            const QStringList& platforms = {},
                            const QStringList& languages = {},
                            const QString& artifactName = {});
QString stableArtifactKey(harkenstudio::ObjectId actionStableId);
QString packNameError(const QString& name, bool grouped = false);
QString relativePackPath(const QString& platform, const QString& language,
                         const QString& artifact);
QString packFileName(const QString& language, const QString& artifact,
                     const QString& extension);
bool actionIsLocalized(const ProjectData& project, harkenstudio::ObjectId actionStableId);

}
}
