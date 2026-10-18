#pragma once

#include "PackExportSelection.h"

#include "ActionClosureSemantics.h"

#include <QHash>
#include <QJsonObject>
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

PackBuildPlan build(const ProjectData& project,
                    const QStringList& platforms = {},
                    const QStringList& languages = {},
                    bool includeUnchecked = false,
                    const PackExportSelection& selection = {});
PackBuildPlan buildForAction(const ProjectData& project,
                            harkenstudio::ObjectId actionStableId,
                            const QStringList& platforms = {},
                            const QStringList& languages = {},
                            const QString& artifactName = {},
                            bool includeUnchecked = false,
                            const PackExportSelection& selection = {});
QString stableArtifactKey(harkenstudio::ObjectId actionStableId);
QString packNameError(const QString& name, bool grouped = false);
QString relativePackPath(const QString& platform, const QString& language,
                         const QString& artifact);
QString packFileName(const QString& language, const QString& artifact,
                     const QString& extension);
bool actionIsLocalized(const ProjectData& project, harkenstudio::ObjectId actionStableId);
QHash<harkenstudio::ObjectId, QStringList> actionLanguages(const ProjectData& project);

}
}
