#pragma once

#include "Tooling/Harkenizer/Core/PackExportSelection.h"

#include "ActionClosureSemantics.h"
#include "PlaybackReferenceValidation.h"

#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <vector>
#include <set>
#include <optional>

namespace VNX {

struct BindingRuntimeLowering;

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
    QJsonObject toJson(const ProjectData& project) const;
};

struct PackBuildPlanKey
{
    QString platform;
    QString language;
    PackExportSelection selection;
    bool updateSelectedOnly = false;
    QSet<QString> onlyUnits;
    QSet<QString> onlyPackPaths;
    std::set<harkenstudio::ObjectId> onlyActionStableIds;

    bool operator==(const PackBuildPlanKey&) const = default;
};

struct PackBuildContext
{
    explicit PackBuildContext(const ProjectData& project)
        : project(&project), capabilities(std::make_shared<RuntimeCapabilityBuildSession>(project)),
          closures(std::make_shared<ActionClosureBuildContext>(project))
    {
    }

    const ProjectData* project;
    std::shared_ptr<RuntimeCapabilityBuildSession> capabilities;
    std::shared_ptr<ActionClosureBuildContext> closures;
    std::vector<std::pair<PackBuildPlanKey, std::shared_ptr<const PackBuildPlan>>> plans;

    const BindingRuntimeLowering& bindingLowering();
    bool validatePlayback(QString* error = nullptr, PlaybackReferenceError* code = nullptr);

private:
    std::shared_ptr<const BindingRuntimeLowering> bindings_;
    std::optional<bool> playbackValid_;
    QString playbackError_;
    PlaybackReferenceError playbackErrorCode_ = PlaybackReferenceError::MissingTarget;
};

namespace PackBuildPlanning {

QHash<harkenstudio::ObjectId, QStringList> artifactLanguages(
    const ProjectData& project, const QStringList& platforms = {});
PackBuildPlan build(const ProjectData& project,
                    const QStringList& platforms = {},
                    const QStringList& languages = {},
                    bool includeUnchecked = false,
                    const PackExportSelection& selection = {},
                    const RuntimeCapabilityContext& context = {},
                    const ActionClosureBuildContext* closures = nullptr);
PackBuildPlan buildForAction(const ProjectData& project,
                            harkenstudio::ObjectId actionStableId,
                            const QStringList& platforms = {},
                            const QStringList& languages = {},
                            const QString& artifactName = {},
                            bool includeUnchecked = false,
                            const PackExportSelection& selection = {},
                            const RuntimeCapabilityContext& context = {},
                            const ActionClosureBuildContext* closures = nullptr);
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
