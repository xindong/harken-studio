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

namespace harken::studio {

struct BindingRuntimeLowering;

struct PackBuildVariant
{
    harken::studio::types::ObjectId actionStableId = {};
    BuildVariant variant;
    ProjectPlatform platformDefinition;
    QString artifactName;
    QString relativePackPath;
    ActionClosurePlan closure;
    RuntimeCapabilityAssessment capability;
    std::vector<harken::studio::types::ObjectId> dependencyOrder;
};

struct PackBuildUnit
{
    QString name;
    QString artifactKey;
    BuildVariant variant;
    ProjectPlatform platformDefinition;
    QString relativePackPath;
    std::vector<harken::studio::types::ObjectId> actionStableIds;
    bool grouped = false;
};

struct PackBuildPlan
{
    std::vector<PackBuildVariant> variants;
    std::vector<PackBuildUnit> units;
    std::set<QString> sharedArtifactIds;
    std::set<harken::studio::types::ObjectId> sharedObjectIds;
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
    std::set<harken::studio::types::ObjectId> onlyActionStableIds;

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

QHash<harken::studio::types::ObjectId, QStringList> artifactLanguages(
    const ProjectData& project, const QStringList& platforms = {});
PackBuildPlan build(const ProjectData& project,
                    const QStringList& platforms = {},
                    const QStringList& languages = {},
                    bool includeUnchecked = false,
                    const PackExportSelection& selection = {},
                    const RuntimeCapabilityContext& context = {},
                    const ActionClosureBuildContext* closures = nullptr);
PackBuildPlan buildForAction(const ProjectData& project,
                            harken::studio::types::ObjectId actionStableId,
                            const QStringList& platforms = {},
                            const QStringList& languages = {},
                            const QString& artifactName = {},
                            bool includeUnchecked = false,
                            const PackExportSelection& selection = {},
                            const RuntimeCapabilityContext& context = {},
                            const ActionClosureBuildContext* closures = nullptr);
QString stableArtifactKey(harken::studio::types::ObjectId actionStableId);
QString packNameError(const QString& name, bool grouped = false);
QString relativePackPath(const QString& platform, const QString& language,
                         const QString& artifact);
QString packFileName(const QString& language, const QString& artifact,
                     const QString& extension);
bool actionIsLocalized(const ProjectData& project, harken::studio::types::ObjectId actionStableId);
QHash<harken::studio::types::ObjectId, QStringList> actionLanguages(const ProjectData& project);

}
}
