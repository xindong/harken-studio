
#include "Tooling/Harkenizer/Core/ProjectPackExporter.h"
#include "Tooling/Harkenizer/Core/ProjectBuildSnapshot.h"
#include "PlaybackReferenceValidation.h"
#include "Tooling/Harkenizer/Core/CanonicalPackSetExport.h"
#include "Tooling/Harkenizer/Core/CanonicalRuntimeProjectAdapter.h"
#include "Tooling/Harkenizer/Core/CanonicalRuntimeProjectAdapterPrivate.h"
#include "AttenuationStore.h"
#include "FieldGroupProjectAdapter.h"
#include "ItemTypeCatalog.h"
#include "ProjectTypes.h"
#include "ProjectLanguageSemantics.h"
#include "Tooling/Harkenizer/Core/PackBuildPlan.h"
#include "Tooling/Harkenizer/Core/SoundSourceDerivative.h"
#include "SoundSourceEditing.h"
#include "SoundSourceSemantics.h"
#include "HarkenMath.h"
#include "BindingRuntimeLowering.h"
#include "Components/VAudio.h"
#include "Components/VMixNode.h"
#include "Components/VAction.h"
#include "AssetPipeline/AssetConverter.h"
#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/PathUtils.h"
#include "AssetPipeline/PlatformProfile.h"
#include "AssetPipeline/WaveformCache.h"

#include "PlayablePropertyAuthoringV1.h"
#include "ProjectPlatformActivity.h"
#include <harken_studio_build/PackFormat.h>
#include "Components/VBus.h"
#include "RuntimeBridge/PackagingBridge.h"
#include "ActionStepUtils.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDebug>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSet>
#include <QDataStream>

#include <algorithm>
#include <limits>
#include <functional>
#include <vector>
#include <cmath>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <map>
#include <unordered_map>

namespace VNX {

namespace Bridge = HarkenEditor::Bridge;

static bool buildPackResources(Bridge::PackBuildSession& builder, const QString& outputPath,
                               const ProjectPackExporter::ExportOptions& options,
                               ProjectPackExporter::ExportResult& result,
                               std::vector<Bridge::PackBuildSession::CanonicalPackEntry>* resourceEntries = nullptr)
{
    if (options.resourceReuse) {
        result.resourceFingerprint = builder.inputFingerprint();
        if (result.resourceFingerprint.isEmpty()) {
            result.error = builder.lastError();
            return false;
        }
        result.reusedResources = !options.forceReexport && !options.resourceReuse->path.isEmpty()
            && result.resourceFingerprint == options.resourceReuse->fingerprint;
    }
    if (!result.reusedResources) {
        const bool built = resourceEntries ? builder.buildEntries(resourceEntries) : builder.buildToFile(outputPath);
        if (!built) {
            result.error = builder.lastError();
            return false;
        }
    }
    result.warnings += builder.warnings();
    result.success = true;
    result.outputPath = result.reusedResources ? options.resourceReuse->path : outputPath;
    result.fileSize = resourceEntries && !result.reusedResources ? 0 : QFileInfo(result.outputPath).size();
    return true;
}

static ProjectPlatformId exportPlatformOf(const ProjectPackExporter::ExportOptions& options)
{
    return QUuid::fromString(options.platform);
}

static QString exportLanguageOf(const ProjectPackExporter::ExportOptions& options)
{
    return options.language.trimmed();
}

static uint32_t fnvOfName(const QString& name)
{
    QByteArray utf8 = name.toUtf8();
    return HarkenEditor::hashString(utf8.constData());
}

static uint32_t counterToFNV(const ProjectData& project, uint32_t counter)
{
    if (counter == 0u) return 0u;
    if (auto* item = findItemByAuthoringHandleInProject(
            const_cast<ProjectData*>(&project), counter))
        return fnvOfName(item->name);
    return 0u;
}

static harkenstudio::ObjectId busIdentityForHandle(const ProjectData& project, uint32_t counter)
{
    if (counter == kMasterBusID)
        return project.busses.empty() || !project.busses.front().rootItem
            ? harkenstudio::ObjectId{} : project.busses.front().rootItem->runtimeId();
    const auto* item = findItemByAuthoringHandleInProject(
        const_cast<ProjectData*>(&project), counter);
    return item && item->type == ProjectItem::Type::Bus ? item->runtimeId() : harkenstudio::ObjectId{};
}

static void forEachBusItem(const ProjectData& project,
                           const std::function<void(const ProjectItem*)>& fn)
{
    std::function<void(const ProjectItem*)> walk = [&](const ProjectItem* item) {
        if (!item) return;
        if (item->type == ProjectItem::Type::Bus &&
            item->comp<VBus>() && item->comp<VMixNode>())
            fn(item);
        for (const auto& child : item->children)
            walk(child.get());
    };
    for (const auto& binding : project.busses)
        walk(binding.rootItem.get());
}

static std::vector<const ProjectItem*> collectAllActionItems(const ProjectData& project)
{
    std::vector<const ProjectItem*> actions;
    for (const auto* binding : project.allFileBindings())
    {
        if (!binding || !binding->rootItem) continue;
        std::function<void(const ProjectItem*)> walk = [&](const ProjectItem* item) {
            if (!item) return;
            const auto* actionComp = item->comp<VAction>();
            if (item->type == ProjectItem::Type::Action && actionComp)
            {
                actions.push_back(item);
            }
            for (const auto& child : item->children)
                walk(child.get());
        };
        walk(binding->rootItem.get());
    }
    return actions;
}

static bool actionHasSteps(const ProjectItem* item)
{
    const auto* actionComp = item ? item->comp<VAction>() : nullptr;
    return actionComp && !actionComp->steps.empty();
}

static harkenstudio::ObjectId stableIdForItem(const ProjectItem* item,
                                ProjectPlatformId platform,
                                const QString& language)
{
    return item ? item->runtimeId() : harkenstudio::ObjectId{};
}

static harkenstudio::ObjectId actionIdentity(const ProjectItem* item)
{
    return stableIdForItem(item, ProjectPlatformId{}, {});
}

static QString actionCapabilityError(
    const ProjectItem& action,
    const RuntimeCapabilityAssessment& capability)
{
    const auto issue = std::find_if(
        capability.issues.begin(), capability.issues.end(),
        [](const RuntimeCapabilityIssue& candidate) {
            return candidate.state == RuntimeCapabilityState::UnavailableEngineContract
                || candidate.state == RuntimeCapabilityState::InvalidAuthoring;
        });
    const QString reason = issue != capability.issues.end()
        && !issue->diagnostic.isEmpty()
        ? issue->diagnostic
        : QStringLiteral("runtime capability is %1")
              .arg(RuntimeCapabilityMatrix::stateName(capability.state));
    return QStringLiteral("Action '%1' cannot be exported: %2")
        .arg(action.name, reason);
}

static QString actionClosureExportError(const ActionClosurePlan& closure,
                                        bool rejectExcludedDependency)
{
    const auto issue = std::find_if(
        closure.diagnostics.begin(), closure.diagnostics.end(),
        [rejectExcludedDependency](const ActionClosureDiagnostic& candidate) {
            return candidate.code == ActionClosureDiagnosticCode::AmbiguousSourceSelection
                || (rejectExcludedDependency
                    && candidate.code == ActionClosureDiagnosticCode::ExcludedDependency);
        });
    return issue == closure.diagnostics.end() ? QString() : issue->message;
}

namespace {

using ObjectIndex = std::unordered_map<uint32_t, const ProjectItem*>;

ObjectIndex buildProjectObjectIndex(const ProjectData& project);

}

std::set<harkenstudio::ObjectId> ProjectPackExporter::CollectActionStableIds(const ProjectData& project)
{
    std::set<harkenstudio::ObjectId> ids;
    for (const ProjectItem* action : collectAllActionItems(project))
        ids.insert(actionIdentity(action));
    return ids;
}

std::vector<ProjectPackExporter::ActionPackUnit>
ProjectPackExporter::CollectActionPackUnits(const ProjectData& project)
{
    std::vector<ActionPackUnit> units;
    for (const ProjectItem* action : collectAllActionItems(project))
        units.push_back({actionIdentity(action), action->objectID, action->name});
    return units;
}

ProjectPackExporter::ProjectPackExporter(QObject* parent)
    : QObject(parent)
{
}

ProjectPackExporter::~ProjectPackExporter() = default;


namespace {
std::mutex              g_exportGateMutex;
std::condition_variable g_exportGateCv;
int                     g_exportsInFlight = 0;
}

void ProjectPackExporter::beginBackgroundExport()
{
    ProjectPlatformActivity::instance().begin();
    std::lock_guard<std::mutex> lock(g_exportGateMutex);
    ++g_exportsInFlight;
}

void ProjectPackExporter::endBackgroundExport()
{
    {
        std::lock_guard<std::mutex> lock(g_exportGateMutex);
        --g_exportsInFlight;
    }
    g_exportGateCv.notify_all();
    ProjectPlatformActivity::instance().end();
}

bool ProjectPackExporter::waitForBackgroundExports(int timeoutMs)
{
    std::unique_lock<std::mutex> lock(g_exportGateMutex);
    return g_exportGateCv.wait_for(lock,
                                   std::chrono::milliseconds(timeoutMs),
                                   [] { return g_exportsInFlight == 0; });
}

bool ProjectPackExporter::hasBackgroundExportInFlight()
{
    std::lock_guard<std::mutex> lock(g_exportGateMutex);
    return g_exportsInFlight > 0;
}


uint32_t ProjectPackExporter::FNVHash(const QString& str)
{
    QByteArray utf8 = str.toUtf8();
    return HarkenEditor::hashString(utf8.constData());
}

void ProjectPackExporter::ApplyMediaCollectionStats(ExportResult& result,
                                                    const MediaCollectionStats& stats)
{
    result.mediaCacheHits += stats.cacheHits;
    result.mediaConversions += stats.conversions;
    result.mediaFallbacks += stats.fallbacks;
    result.warnings.append(stats.warnings);
    result.soundContentRevisions.insert(
        stats.soundContentRevisions.begin(), stats.soundContentRevisions.end());
}


static bool isSupportedExportPlatform(const ProjectData& project,
                                       const QString& platform, QString* errorOut)
{
    const auto id = ProjectPlatformSemantics::parseId(platform);
    if (!id || !ProjectPlatformSemantics::find(project.platforms, *id)) {
        if (errorOut) *errorOut = QStringLiteral("Unknown project platform ID: '%1'").arg(platform);
        return false;
    }
    return true;
}

QString ProjectPackExporter::platformOutputDir(const QString& baseDir,
                                               const ProjectPlatform& platform)
{
    return QDir(baseDir).filePath(platform.name);
}

bool ProjectPackExporter::ValidatePackBuildPlatforms(const QVector<ProjectPlatform>& captured,
                                                     const QVector<ProjectPlatform>& current,
                                                     const QStringList& succeededIds,
                                                     QString* errorOut)
{
    for (const auto& id : succeededIds) {
        const auto* before = ProjectPlatformSemantics::find(captured, QUuid(id));
        const auto* after = ProjectPlatformSemantics::find(current, QUuid(id));
        if (!after) {
            if (errorOut) *errorOut = QStringLiteral("Pack platform was deleted during the build: %1").arg(id);
            return false;
        }
        if (!before || before->toJson() != after->toJson()) {
            if (errorOut) *errorOut = QStringLiteral("Pack platform changed during the build: %1").arg(id);
            return false;
        }
    }
    return true;
}

QStringList ProjectPackExporter::resolveExportPlatforms(const ProjectData& project,
                                                        const QString& explicitPlatform,
                                                        const PackExportSelection& selection,
                                                        QString* errorOut)
{
    if (!explicitPlatform.isEmpty())
    {
        if (!isSupportedExportPlatform(project, explicitPlatform, errorOut))
            return {};
        if (!project.platformIds().contains(explicitPlatform))
        {
            if (errorOut)
                *errorOut = QStringLiteral("Platform '%1' is not a target platform "
                                           "of this project")
                                .arg(explicitPlatform);
            return {};
        }
        return {explicitPlatform};
    }

    if (selection.selectedPlatformIds.isEmpty())
    {
        if (errorOut)
            *errorOut = QStringLiteral("No pack export platforms selected; pass an "
                                       "explicit platform or select platforms in the "
                                       "project");
        return {};
    }

    QStringList platforms;
    for (const QString& platform : selection.selectedPlatformIds)
        if (project.platformIds().contains(platform) && !platforms.contains(platform))
            platforms.append(platform);
    if (platforms.isEmpty())
    {
        if (errorOut)
            *errorOut = QStringLiteral("None of the selected pack export platforms "
                                       "are target platforms of this project");
        return {};
    }
    return platforms;
}


static bool actionRequested(const ProjectData& project,
                            const ProjectPackExporter::ExportOptions& options,
                            harkenstudio::ObjectId id, const QString& name)
{
    if (!options.onlyActionStableIds.empty())
        return options.onlyActionStableIds.count(id) != 0;
    if (!options.onlyUnits.isEmpty())
        return options.onlyUnits.contains(name);
    if (!options.onlyPackPaths.isEmpty()) {
        if (options.onlyPackPaths.contains(name)) return true;
        for (const auto& language : ProjectLanguageSemantics::buildLanguages(project))
            if (options.onlyPackPaths.contains(PackBuildPlanning::packFileName(language, name, {})))
                return true;
        return false;
    }
    return true;
}

std::shared_ptr<PackBuildContext> ProjectPackExporter::createBuildContext(const ProjectData& project)
{
    return std::make_shared<PackBuildContext>(project);
}

std::shared_ptr<const PackBuildPlan> ProjectPackExporter::createBuildPlan(
    const ProjectData& project, const ExportOptions& options)
{
    const PackBuildPlanKey key{options.platform, options.language.trimmed(), options.selection,
        options.updateSelectedOnly || options.includeUnchecked, options.onlyUnits, options.onlyPackPaths, options.onlyActionStableIds};
    RuntimeCapabilityContext context;
    context.deferCurrentWorldCompilation = true;
    if (options.buildContext) {
        context.buildSession = options.buildContext->capabilities;
        for (const auto& cached : options.buildContext->plans)
            if (cached.first == key) return cached.second;
    }
    QStringList languages;
    if (!options.language.trimmed().isEmpty())
        languages.append(options.language.trimmed());

    PackBuildPlan buildPlan = PackBuildPlanning::build(project, {options.platform}, languages,
        options.updateSelectedOnly || options.includeUnchecked, options.selection, context,
        options.buildContext ? options.buildContext->closures.get() : nullptr);
    using VariantKey = std::pair<harkenstudio::ObjectId, QString>;
    std::set<VariantKey> selectedVariants;
    for (const auto& variant : buildPlan.variants) {
        if (actionRequested(project, options, variant.actionStableId, variant.artifactName)
            && (options.onlyPackPaths.isEmpty() || options.onlyPackPaths.contains(
                PackBuildPlanning::packFileName(variant.variant.language, variant.artifactName, {}))))
            selectedVariants.emplace(variant.actionStableId, variant.variant.language);
    }
    buildPlan.variants.erase(std::remove_if(buildPlan.variants.begin(), buildPlan.variants.end(),
        [&](const auto& variant) {
            return !selectedVariants.count({variant.actionStableId, variant.variant.language});
        }), buildPlan.variants.end());
    buildPlan.units.erase(std::remove_if(buildPlan.units.begin(), buildPlan.units.end(),
        [&](const auto& unit) {
            return !selectedVariants.count({unit.actionStableIds.front(), unit.variant.language});
        }), buildPlan.units.end());
    auto prepared = std::make_shared<const PackBuildPlan>(std::move(buildPlan));
    if (options.buildContext)
        options.buildContext->plans.emplace_back(key, prepared);
    return prepared;
}

ProjectPackExporter::ExportResult
ProjectPackExporter::ExportAllPacks(const ProjectData& project,
                                     const QString& outputDir,
                                     const ExportOptions& options,
                                     std::vector<PackBuildRecord>* outRecords)
{
    if (!outputDir.isEmpty()
        && PathUtils::isProtectedPackOutputPath(outputDir, project.projectPath)
        && (options.selfCleaning || !PathUtils::isProjectRootDirectory(outputDir, project.projectPath))) {
        ExportResult failure;
        failure.error = PathUtils::packBuildOutputLocationError();
        return failure;
    }
    if (!options.snapshot || &project != &options.snapshot->project || !options.buildContext
        || options.buildContext->project != &project || outputDir.isEmpty()) {
        ExportOptions prepared = options;
        ExportResult failure;
        if (!prepared.snapshot) prepared.snapshot = ProjectBuildSnapshot::capture(project, &failure.error);
        if (!prepared.snapshot) return failure;
        if (!prepared.buildContext || prepared.buildContext->project != &prepared.snapshot->project) {
            prepared.buildContext = createBuildContext(prepared.snapshot->project);
            prepared.buildPlan.reset();
        }
        if (outputDir.isEmpty()) {
            const auto& captured = prepared.snapshot->project;
            if (!isSupportedExportPlatform(captured, prepared.platform, &failure.error)) return failure;
            const auto unsupported = Bridge::validateBusPublicationSupport(
                captured, QUuid(prepared.platform));
            if (!unsupported.empty()) {
                failure.error = unsupported.front().message;
                return failure;
            }
            const QString root = PathUtils::packBuildOutputRoot(captured.projectPath, captured.packBuildOutputDir);
            if (PathUtils::isProtectedPackOutputPath(root, captured.projectPath)) {
                failure.error = PathUtils::packBuildOutputLocationError();
                return failure;
            }
            const auto outputLock = LockPackOutputRoot(root, &failure.error);
            if (!outputLock) return failure;
            auto writeOptions = prepared;
            writeOptions.selfCleaning = false;
            if (!ValidatePackBuildWriteAccess(captured, root, {prepared.platform}, writeOptions, &failure.error)) {
                failure.canceled = prepared.cancelCheck && prepared.cancelCheck();
                return failure;
            }
            const auto& platform = *ProjectPlatformSemantics::find(captured.platforms, QUuid(prepared.platform));
            auto result = ExportAllPacks(captured, platformOutputDir(root, platform), prepared, outRecords);
            if (!result.success)
                return result;
            QString indexError;
            if (!ValidatePackBuildPlatforms(captured.platforms, project.platforms,
                    {prepared.platform}, &indexError)) {
                result.success = false;
                result.error = indexError;
                return result;
            }
            const auto publication = FinalizePackExport(root, captured, {prepared.platform},
                false, result.artifacts, prepared.cancelCheck);
            result.success = publication.success;
            result.canceled = publication.canceled;
            result.error = publication.error;
            result.warnings.append(publication.warnings);
            return result;
        }
        return ExportAllPacks(prepared.snapshot->project, outputDir, prepared, outRecords);
    }

    const auto platformActivity = ProjectPlatformActivity::acquire();
    ExportResult result;
    PlaybackReferenceError referenceError;
    const bool referencesValid = options.buildContext->validatePlayback(&result.error, &referenceError);
    if (!referencesValid && referenceError == PlaybackReferenceError::InvalidStructure)
        return result;

    if (!isSupportedExportPlatform(project, options.platform, &result.error))
        return result;

    QDir dir(outputDir);
    const QString packExt = Bridge::packArtifactExtension();

    ExportOptions effectiveOpts = options;
    const auto& allActions = options.buildContext->capabilities->sourceIndex()->actions;
    std::unordered_map<harkenstudio::ObjectId, const ProjectItem*> actionsByStableId;
    for (const ProjectItem* evt : allActions)
        actionsByStableId.emplace(actionIdentity(evt), evt);

    effectiveOpts.buildPlan = createBuildPlan(project, effectiveOpts);
    const PackBuildPlan& buildPlan = *effectiveOpts.buildPlan;
    std::map<std::pair<QString, QString>, PackGenerationProgress> unitProgress;
    if (options.progressCallback) {
        effectiveOpts.progressCallback = [&](const PackGenerationProgress& progress) {
            const auto found = unitProgress.find({progress.language, progress.unitKey});
            if (found == unitProgress.end()) return;
            if (found->second.stage == PackGenerationStage::Failed
                || found->second.stage == PackGenerationStage::Canceled
                || found->second.stage == PackGenerationStage::Skipped
                || found->second.stage == PackGenerationStage::Complete)
                return;
            found->second = progress;
            options.progressCallback(progress);
        };
    }
    const auto finishProgress = qScopeGuard([&] {
        if (!options.progressCallback) return;
        for (auto& [key, progress] : unitProgress) {
            if (progress.stage == PackGenerationStage::Failed
                || progress.stage == PackGenerationStage::Canceled
                || progress.stage == PackGenerationStage::Skipped
                || progress.stage == PackGenerationStage::Complete)
                continue;
            progress.stage = result.success ? PackGenerationStage::Complete
                : result.canceled ? PackGenerationStage::Canceled : PackGenerationStage::Failed;
            if (result.success) progress.percent = 100;
            options.progressCallback(progress);
        }
    });
    const auto queueUnit = [&](const QString& language, const QString& unitKey) {
        if (!options.progressCallback) return;
        const PackGenerationProgress progress{
            options.platform, language, unitKey, 0, PackGenerationStage::Queued};
        unitProgress.emplace(std::make_pair(language, unitKey), progress);
        options.progressCallback(progress);
    };
    queueUnit({}, QStringLiteral("Init"));
    for (const auto& unit : buildPlan.units) {
        if (effectiveOpts.updateSelectedOnly) {
            if (!effectiveOpts.onlyPackPaths.isEmpty()) {
                if (!effectiveOpts.onlyPackPaths.contains(
                        PackBuildPlanning::packFileName(unit.variant.language, unit.name, {})))
                    continue;
            } else if (!effectiveOpts.onlyActionStableIds.empty()) {
                if (std::none_of(unit.actionStableIds.begin(), unit.actionStableIds.end(),
                        [&](auto id) { return effectiveOpts.onlyActionStableIds.count(id) != 0; }))
                    continue;
            } else if (!effectiveOpts.onlyUnits.isEmpty() && !effectiveOpts.onlyUnits.contains(unit.name)) {
                continue;
            }
        }
        queueUnit(unit.variant.language, unit.grouped ? QStringLiteral("g:") + unit.artifactKey
            : QStringLiteral("a:") + objectIdString(unit.actionStableIds.front()));
    }
    if (!referencesValid) return result;
    const auto unsupported = Bridge::validateBusPublicationSupport(
        project, QUuid(options.platform));
    if (!unsupported.empty()) {
        result.error = unsupported.front().message;
        return result;
    }
    if (!options.updateSelectedOnly) {
        const ValidationResult readiness =
            ValidatePackBuildReadiness(project, outputDir, options.buildContext.get());
        if (readiness.errorCount > 0) {
            QStringList errors;
            for (const ValidationIssue& issue : readiness.issues)
                if (issue.severity == ValidationIssue::Error)
                    errors.append(issue.message);
            result.error = QStringLiteral("Pack build readiness failed: %1")
                               .arg(errors.join(QStringLiteral("; ")));
            return result;
        }
    }
    if (!dir.exists())
        dir.mkpath(".");
    if (effectiveOpts.updateSelectedOnly)
        effectiveOpts.selfCleaning = false;
    for (const auto& diagnostic : buildPlan.diagnostics) {
        if (diagnostic.code != ActionClosureDiagnosticCode::LocalizedWithoutLanguages
            && diagnostic.code != ActionClosureDiagnosticCode::InvalidBuildLanguage
            && diagnostic.code != ActionClosureDiagnosticCode::InvalidPackName)
            continue;
        const auto action = actionsByStableId.find(diagnostic.actionStableId);
        if (action != actionsByStableId.end() && actionRequested(project, effectiveOpts, action->first, action->second->name)) {
            result.error = diagnostic.message;
            return result;
        }
    }
    std::set<harkenstudio::ObjectId> buildActionIds;
    for (const auto& variant : buildPlan.variants) buildActionIds.insert(variant.actionStableId);

    for (const PackBuildVariant& variant : buildPlan.variants) {
        const auto action = actionsByStableId.find(variant.actionStableId);
        if (action == actionsByStableId.end()
            || buildActionIds.count(variant.actionStableId) == 0)
            continue;
        const auto cycle = std::find_if(
            variant.closure.diagnostics.begin(), variant.closure.diagnostics.end(),
            [](const ActionClosureDiagnostic& diagnostic) {
                return diagnostic.code == ActionClosureDiagnosticCode::PostActionCycle;
            });
        if (cycle != variant.closure.diagnostics.end()) {
            result.error = QStringLiteral("PostAction dependency cycle for '%1': %2")
                               .arg(action->second->name, cycle->message);
            return result;
        }
        if (const QString sourceError = actionClosureExportError(variant.closure, true);
            !sourceError.isEmpty()) {
            result.error = QStringLiteral("Action '%1' cannot be exported: %2")
                               .arg(action->second->name, sourceError);
            return result;
        }
        for (harkenstudio::ObjectId dependencyId : variant.dependencyOrder) {
            const auto dependency = actionsByStableId.find(dependencyId);
            if (dependency == actionsByStableId.end()) {
                result.error = QStringLiteral(
                    "PostAction dependency %1 for '%2' does not resolve to an Action")
                                   .arg(VNX::objectIdString(dependencyId))
                                   .arg(action->second->name);
                return result;
            }
            if (dependencyId != variant.actionStableId
                && !actionHasSteps(dependency->second)) {
                result.error = QStringLiteral(
                    "PostAction dependency '%1' for '%2' has no steps")
                                   .arg(dependency->second->name,
                                        action->second->name);
                return result;
            }
            buildActionIds.insert(dependencyId);
        }
        if (actionHasSteps(action->second) && !variant.capability.permitted()) {
            result.error = actionCapabilityError(
                *action->second, variant.capability);
            return result;
        }
    }

    std::set<harkenstudio::ObjectId> packableActionIds = buildActionIds;
    std::set<harkenstudio::ObjectId> emptyActionIds;
    for (harkenstudio::ObjectId stableId : buildActionIds) {
        const auto action = actionsByStableId.find(stableId);
        if (action == actionsByStableId.end() || actionHasSteps(action->second))
            continue;
        emptyActionIds.insert(stableId);
        packableActionIds.erase(stableId);
    }
    for (const PackBuildUnit& unit : buildPlan.units) {
        const auto empty = std::find_if(unit.actionStableIds.begin(), unit.actionStableIds.end(),
            [&](const auto id) { return emptyActionIds.count(id) != 0; });
        if (empty == unit.actionStableIds.end()
            || std::any_of(unit.actionStableIds.begin(), unit.actionStableIds.end(),
                [&](const auto id) { return packableActionIds.count(id) != 0; }))
            continue;
        const QString unitKey = unit.grouped ? QStringLiteral("g:") + unit.artifactKey
            : QStringLiteral("a:") + objectIdString(*empty);
        if (effectiveOpts.progressCallback)
            effectiveOpts.progressCallback({effectiveOpts.platform, unit.variant.language,
                unitKey, 100, PackGenerationStage::Skipped});
        if (!outRecords) continue;
        const QString warning = unit.grouped
            ? QStringLiteral("Pack group has no Actions with steps; nothing to pack")
            : QStringLiteral("Action has no steps; nothing to pack");
        outRecords->push_back({
            unit.name, unit.grouped ? 0 : actionsByStableId.at(*empty)->objectID,
            unit.grouped ? PackBuildRecord::Source::Group : PackBuildRecord::Source::Action, true,
            dir.filePath(PackBuildPlanning::packFileName(
                unit.variant.language, unit.name, packExt)),
            {}, 0,
            effectiveOpts.platform, {warning}, -1, unit.variant.language,
            unit.platformDefinition.name, unit.platformDefinition.basePlatform,
            unit.platformDefinition.toJson(), false,
            unitKey, true});
    }
    if (!emptyActionIds.empty())
        result.warnings.append(
            QStringLiteral("%1 action(s) have no steps and were not packed")
                .arg(emptyActionIds.size()));

    CanonicalPackSetExportRequest setRequest;
    setRequest.project = &project;
    setRequest.outputDir = outputDir;
    setRequest.options = effectiveOpts;
    setRequest.includedActionIds = buildActionIds;
    setRequest.plan = &buildPlan;
    const QStringList orchestrationWarnings = result.warnings;
    result = ExportCanonicalPackSet(*this, setRequest, outRecords);
    result.warnings = orchestrationWarnings + result.warnings;
    if (!result.success)
        return result;

    return result;
}


ProjectPackExporter::ExportResult
ProjectPackExporter::ExportInitPack(const ProjectData& project,
                                     const QString& outputPath,
                                     const ExportOptions& options,
                                     std::vector<Bridge::PackBuildSession::CanonicalPackEntry>* resourceEntries)
{
    if (PathUtils::isProtectedPackOutputPath(outputPath, project.projectPath)) {
        ExportResult failure;
        failure.error = PathUtils::packBuildOutputLocationError();
        return failure;
    }
    if (!options.snapshot || &project != &options.snapshot->project || !options.buildContext
        || options.buildContext->project != &project) {
        ExportOptions prepared = options;
        ExportResult failure;
        if (!prepared.snapshot) prepared.snapshot = ProjectBuildSnapshot::capture(project, &failure.error);
        if (!prepared.snapshot) return failure;
        if (!prepared.buildContext || prepared.buildContext->project != &prepared.snapshot->project) {
            prepared.buildContext = createBuildContext(prepared.snapshot->project);
            prepared.buildPlan.reset();
        }
        return ExportInitPack(prepared.snapshot->project, outputPath, prepared, resourceEntries);
    }

    const auto platformActivity = ProjectPlatformActivity::acquire();
    ExportResult result;
    if (!options.buildContext->validatePlayback(&result.error)) return result;
    if (!isSupportedExportPlatform(project, options.platform, &result.error))
        return result;

    const PlatformScope exportScope = QUuid::fromString(options.platform);
    const auto unsupportedBusProperties = Bridge::validateBusPublicationSupport(
        project, *exportScope);
    if (!unsupportedBusProperties.empty()) {
        result.error = unsupportedBusProperties.front().message;
        return result;
    }

    Bridge::PackBuildSession builder;
    builder.setInitPack(true);

    emit progressChanged(10, "Collecting bus hierarchy...");
    CollectBuses(project, builder, exportScope);
    if (!CollectSends(project, builder, exportScope, &result.error)) return result;
    CollectAttenuations(project, builder);
    if (!options.deferBindings
        && !CollectTypedBindings(project, builder, nullptr, &result.error, options.buildContext.get()))
        return result;

    emit progressChanged(90, "Writing Init pack...");

    if (!buildPackResources(builder, outputPath, options, result, resourceEntries)) return result;

    emit progressChanged(100, "Init pack exported.");
    return result;
}


ProjectPackExporter::ExportResult
ProjectPackExporter::ExportActionPack(const ProjectData& project,
                                      const QString& actionName,
                                      const QString& outputPath,
                                      const ExportOptions& options,
                                      const std::set<uint32_t>* sharedMediaIds,
                                      const std::set<uint32_t>* sharedObjectIds,
                                      std::vector<Bridge::PackBuildSession::CanonicalPackEntry>* resourceEntries)
{
    if (PathUtils::isProtectedPackOutputPath(outputPath, project.projectPath)) {
        ExportResult failure;
        failure.error = PathUtils::packBuildOutputLocationError();
        return failure;
    }
    if (!options.snapshot || &project != &options.snapshot->project || !options.buildContext
        || options.buildContext->project != &project) {
        ExportOptions prepared = options;
        ExportResult failure;
        if (!prepared.snapshot) prepared.snapshot = ProjectBuildSnapshot::capture(project, &failure.error);
        if (!prepared.snapshot) return failure;
        if (!prepared.buildContext || prepared.buildContext->project != &prepared.snapshot->project) {
            prepared.buildContext = createBuildContext(prepared.snapshot->project);
            prepared.buildPlan.reset();
        }
        return ExportActionPack(prepared.snapshot->project, actionName, outputPath, prepared, sharedMediaIds, sharedObjectIds, resourceEntries);
    }

    const auto platformActivity = ProjectPlatformActivity::acquire();
    ExportResult result;
    if (!options.buildContext->validatePlayback(&result.error)) return result;
    if (options.cancelCheck && options.cancelCheck())
    {
        result.canceled = true;
        result.error = QStringLiteral("Export canceled");
        return result;
    }
    if (!isSupportedExportPlatform(project, options.platform, &result.error))
        return result;

    std::vector<const ProjectItem*> matches;
    for (const ProjectItem* action : options.buildContext->capabilities->sourceIndex()->actions)
        if (action->name == actionName)
            matches.push_back(action);

    if (matches.empty())
    {
        result.error = QStringLiteral("Packable Action '%1' was not found")
                           .arg(actionName);
        return result;
    }
    if (matches.size() > 1)
    {
        result.error = QStringLiteral(
            "Action name '%1' is ambiguous; %2 packable Actions have that name")
                           .arg(actionName)
                           .arg(matches.size());
        return result;
    }
    if (!actionHasSteps(matches.front()))
    {
        result.error = QStringLiteral("Action '%1' has no steps; nothing to pack")
                           .arg(actionName);
        return result;
    }

    if (options.cancelCheck && options.cancelCheck())
    {
        result.canceled = true;
        result.error = QStringLiteral("Export canceled");
        return result;
    }

    return exportSingleActionPack(project, *matches.front(), outputPath, options,
                                  sharedMediaIds, sharedObjectIds, resourceEntries);
}


ProjectPackExporter::ExportResult
ProjectPackExporter::exportSingleActionPack(
    const ProjectData& project,
    const ProjectItem& action,
    const QString& outputPath,
    const ExportOptions& options,
    const std::set<uint32_t>* sharedMediaIds,
    const std::set<uint32_t>* sharedObjectIds,
    std::vector<Bridge::PackBuildSession::CanonicalPackEntry>* resourceEntries)
{
    emit progressChanged(10, QStringLiteral("Collecting Action dependencies..."));
    ExportResult result;
    const auto effectiveGroups = options.buildContext
        ? options.buildContext->capabilities->fieldGroups(exportPlatformOf(options))
        : std::make_shared<FieldGroupEffectiveSnapshot>(buildFieldGroupEffectiveSnapshot(
            project, toFieldGroupScope(exportPlatformOf(options))));
    if (!effectiveGroups->ok()) {
        result.error = effectiveGroups->diagnostics.front().message;
        return result;
    }
    Bridge::PackBuildSession builder;

    const auto& globalIndex = options.buildContext->capabilities->sourceIndex()->byObjectId;
    const QString language = options.language.trimmed();
    const QStringList languages = language.isEmpty() ? QStringList() : QStringList{language};
    std::optional<PackBuildPlan> localPlan;
    const PackBuildPlan* buildPlan = options.buildPlan.get();
    if (!buildPlan) {
        RuntimeCapabilityContext context;
        if (options.buildContext) context.buildSession = options.buildContext->capabilities;
        localPlan = PackBuildPlanning::buildForAction(
            project, action.runtimeId(), {options.platform}, languages,
            QFileInfo(outputPath).completeBaseName(), options.updateSelectedOnly || options.includeUnchecked, options.selection, context,
            options.buildContext ? options.buildContext->closures.get() : nullptr);
        buildPlan = &*localPlan;
    }
    const auto planned = std::find_if(buildPlan->variants.begin(), buildPlan->variants.end(),
                                      [&](const auto& value) {
        return value.actionStableId == action.runtimeId() && value.variant.platform == options.platform
            && (language.isEmpty() || value.variant.language.isEmpty() || value.variant.language == language);
    });
    if (planned == buildPlan->variants.end()) {
        result.error = QStringLiteral("Action pack export failed for '%1': %2")
                           .arg(action.name, buildPlan->diagnostics.empty()
                               ? QStringLiteral("build plan is missing")
                               : buildPlan->diagnostics.front().message);
        return result;
    }
    if (language.isEmpty() && !planned->variant.language.isEmpty()) {
        result.error = QStringLiteral(
            "Action pack export failed for '%1': a localized single-pack export requires an explicit project language")
                           .arg(action.name);
        return result;
    }
    if (const QString sourceError = actionClosureExportError(planned->closure, true);
        !sourceError.isEmpty()) {
        result.error = QStringLiteral("Action pack export failed for '%1': %2")
                           .arg(action.name, sourceError);
        return result;
    }
    if (!planned->capability.permitted()) {
        result.error = QStringLiteral("Action pack export failed for '%1': %2")
                           .arg(action.name,
                                actionCapabilityError(action, planned->capability));
        return result;
    }
    ExportOptions variantOptions = options;
    variantOptions.language = planned->variant.language;
    const ActionClosurePlan& closure = planned->closure;
    std::set<uint32_t> visitedObjects;
    std::set<uint32_t> requiredMedia;

    for (const auto& member : closure.objects)
    {
        auto dependency = globalIndex.find(member.objectId);
        if (dependency == globalIndex.end() || !dependency->second)
            continue;
        CollectContentInputsFromItem(dependency->second, builder, visitedObjects,
                                     requiredMedia, project, exportPlatformOf(variantOptions),
                                     exportLanguageOf(variantOptions), sharedObjectIds);
    }
    if (!options.deferBindings) {
        std::set<harkenstudio::ObjectId> closureObjectIds;
        for (const auto& member : closure.objects)
            if (!harkenstudio::IsNull(member.stableId) && visitedObjects.count(member.objectId) != 0)
                closureObjectIds.insert(member.stableId);
        if (!CollectTypedBindings(project, builder, &closureObjectIds, &result.error, options.buildContext.get()))
            return result;
    }
    if (sharedMediaIds)
        for (uint32_t sharedId : *sharedMediaIds)
            requiredMedia.erase(sharedId);
    result.objectCount = static_cast<int>(visitedObjects.size());

    for (const auto& member : closure.actions)
        builder.addStringMapping(member.stableId, member.name);

    MediaCollectionStats mediaStats;
    if (!CollectMedia(project, builder, requiredMedia, &visitedObjects,
                      variantOptions, &mediaStats, &result.error, &closure,
                      sharedMediaIds))
    {
        ApplyMediaCollectionStats(result, mediaStats);
        result.canceled = result.error == QStringLiteral("Export canceled");
        if (!result.canceled)
            result.error = QStringLiteral("Action pack export failed for '%1': %2")
                               .arg(action.name, result.error);
        return result;
    }
    ApplyMediaCollectionStats(result, mediaStats);
    result.mediaCount = static_cast<int>(requiredMedia.size());
    result.actionCount = static_cast<int>(closure.actions.size());

    if (options.debugStrings)
    {
        for (uint32_t id : visitedObjects) {
            const auto item = globalIndex.find(id);
            if (item != globalIndex.end()) builder.addStringMapping(item->second->runtimeId(), item->second->name);
        }
    }

    if (!buildPackResources(builder, outputPath, options, result, resourceEntries))
    {
        result.error = QStringLiteral("Action pack export failed for '%1': %2")
                           .arg(action.name, result.error);
        return result;
    }

    emit progressChanged(100, QStringLiteral("Action pack exported."));
    return result;
}


ProjectPackExporter::ExportResult
ProjectPackExporter::ExportSharedPack(const ProjectData& project,
                                      const QString& outputPath,
                                      const ExportOptions& options,
                                      const std::set<uint32_t>& sharedMedia,
                                      const std::set<uint32_t>& sharedObjects)
{
    if (PathUtils::isProtectedPackOutputPath(outputPath, project.projectPath)) {
        ExportResult failure;
        failure.error = PathUtils::packBuildOutputLocationError();
        return failure;
    }
    if (!options.snapshot || &project != &options.snapshot->project || !options.buildContext
        || options.buildContext->project != &project) {
        ExportOptions prepared = options;
        ExportResult failure;
        if (!prepared.snapshot) prepared.snapshot = ProjectBuildSnapshot::capture(project, &failure.error);
        if (!prepared.snapshot) return failure;
        if (!prepared.buildContext || prepared.buildContext->project != &prepared.snapshot->project) {
            prepared.buildContext = createBuildContext(prepared.snapshot->project);
            prepared.buildPlan.reset();
        }
        return ExportSharedPack(prepared.snapshot->project, outputPath, prepared, sharedMedia, sharedObjects);
    }

    const auto platformActivity = ProjectPlatformActivity::acquire();
    ExportResult result;
    if (!options.buildContext->validatePlayback(&result.error)) return result;
    if (!isSupportedExportPlatform(project, options.platform, &result.error))
        return result;

    const auto effectiveGroups = buildFieldGroupEffectiveSnapshot(
        project, toFieldGroupScope(exportPlatformOf(options)));
    if (!effectiveGroups.ok()) {
        result.error = effectiveGroups.diagnostics.front().message;
        return result;
    }
    Bridge::PackBuildSession builder;

    std::set<uint32_t> visitedObjects;
    std::set<uint32_t> requiredMedia = sharedMedia;
    std::function<void(const ProjectItem*)> collectSharedRoots =
        [&](const ProjectItem* item) {
            if (!item)
                return;
            if (sharedObjects.count(item->objectID))
            {
                CollectContentInputsFromItem(item, builder, visitedObjects, requiredMedia,
                                             project, exportPlatformOf(options),
                                             exportLanguageOf(options));
                return;
            }
            for (const auto& child : item->children)
                collectSharedRoots(child.get());
        };
    for (const auto* binding : project.allFileBindings())
        if (binding && binding->rootItem)
            collectSharedRoots(binding->rootItem.get());
    std::set<harkenstudio::ObjectId> sharedObjectRuntimeIds;
    for (uint32_t objectId : visitedObjects) {
        const ProjectItem* item = findItemByAuthoringHandleInProject(
            const_cast<ProjectData*>(&project), objectId);
        if (item && !harkenstudio::IsNull(item->runtimeId()))
            sharedObjectRuntimeIds.insert(item->runtimeId());
    }
    if (!CollectTypedBindings(project, builder, &sharedObjectRuntimeIds, &result.error, options.buildContext.get()))
        return result;

    emit progressChanged(10, "Collecting shared content...");
    MediaCollectionStats mediaStats;
    if (!CollectMedia(project, builder, requiredMedia, nullptr,
                      options, &mediaStats, &result.error))
    {
        ApplyMediaCollectionStats(result, mediaStats);
        result.canceled = result.error == QStringLiteral("Export canceled");
        return result;
    }
    ApplyMediaCollectionStats(result, mediaStats);
    result.objectCount = static_cast<int>(visitedObjects.size());
    result.mediaCount = static_cast<int>(requiredMedia.size());

    emit progressChanged(90, "Writing shared pack...");

    if (!builder.buildToFile(outputPath))
    {
        result.error = builder.lastError();
        return result;
    }
    result.warnings += builder.warnings();

    QFileInfo fi(outputPath);
    result.success    = true;
    result.outputPath = outputPath;
    result.fileSize   = fi.size();

    emit progressChanged(100, "Shared pack exported.");
    return result;
}


namespace {

void buildObjectIndex(const ProjectItem* item, ObjectIndex& outIndex)
{
    if (!item) return;
    if (item->objectID != 0)
        outIndex.emplace(item->objectID, item);
    for (const auto& child : item->children)
        buildObjectIndex(child.get(), outIndex);
}

ObjectIndex buildProjectObjectIndex(const ProjectData& project)
{
    ObjectIndex index;
    for (const auto* binding : project.allFileBindings()) {
        if (!binding || !binding->rootItem) continue;
        const auto* wu = binding;
        buildObjectIndex(wu->rootItem.get(), index);
    }
    return index;
}

}

ProjectPackExporter::SharedOwnership
ProjectPackExporter::computeSharedOwnership(const ProjectData& project,
                                            const PackBuildPlan& plan,
                                            const std::set<harkenstudio::ObjectId>& includedActionIds) const
{
    const ObjectIndex index = buildProjectObjectIndex(project);
    std::unordered_map<uint32_t, int> unitRefCounts;
    std::unordered_map<uint32_t, int> objectRefCounts;
    for (const PackBuildVariant& variant : plan.variants) {
        if (includedActionIds.count(variant.actionStableId) == 0)
            continue;

        std::set<harkenstudio::ObjectId> dependencyActionIds;
        for (const ActionClosureMember& action : variant.closure.actions)
            if (action.stableId != variant.actionStableId)
                dependencyActionIds.insert(action.stableId);

        std::set<uint32_t> unitMediaIds;
        std::set<uint32_t> unitObjectIds;
        std::map<harkenstudio::ObjectId, uint32_t> unitSoundObjects;
        for (const ActionClosureMember& member : variant.closure.objects) {
            if (std::any_of(member.referencePath.begin(), member.referencePath.end(),
                            [&](harkenstudio::ObjectId id) {
                                return dependencyActionIds.count(id) != 0;
                            }))
                continue;
            if (member.type == ProjectItem::Type::Layer)
                unitObjectIds.insert(member.objectId);
            if (member.type != ProjectItem::Type::Sound)
                continue;
            unitSoundObjects.emplace(member.stableId, member.objectId);
        }
        std::set<harkenstudio::ObjectId> resolvedSoundIds;
        for (const ActionClosureMedia& media : variant.closure.media) {
            if (unitSoundObjects.count(media.soundStableId) != 0
                && media.authoringSourceId != 0) {
                unitMediaIds.insert(media.authoringSourceId);
                resolvedSoundIds.insert(media.soundStableId);
            }
        }
        for (const auto& [soundStableId, objectId] : unitSoundObjects) {
            if (resolvedSoundIds.count(soundStableId) != 0)
                continue;
            const auto item = index.find(objectId);
            if (item == index.end() || !item->second)
                continue;
            const uint32_t sourceId = SoundSourceEditing::activeSourceId(
                *item->second, QUuid::fromString(variant.variant.platform),
                SoundSourceEditing::exportLanguage(*item->second, variant.variant.language));
            if (sourceId != 0)
                unitMediaIds.insert(sourceId);
        }
        for (uint32_t sourceID : unitMediaIds)
            ++unitRefCounts[sourceID];
        for (uint32_t objectID : unitObjectIds)
            ++objectRefCounts[objectID];
    }

    SharedOwnership ownership;
    for (const auto& kv : unitRefCounts)
        if (kv.second > 1)
            ownership.mediaIds.insert(kv.first);
    for (const auto& kv : objectRefCounts)
        if (kv.second > 1)
            ownership.objectIds.insert(kv.first);
    return ownership;
}


void ProjectPackExporter::CollectContentInputsFromItem(const ProjectItem* item,
                                                       Bridge::PackBuildSession& builder,
                                                       std::set<uint32_t>& visitedObjects,
                                                       std::set<uint32_t>& requiredMedia,
                                                       const ProjectData& project,
                                                       ProjectPlatformId platform,
                                                       const QString& language,
                                                       const std::set<uint32_t>* excludedObjectIds)
{
    if (!item) return;

    if ((item->type != ProjectItem::Type::Sound
         && item->type != ProjectItem::Type::Layer
         && item->type != ProjectItem::Type::Container)
        || item->objectID == 0)
    {
        for (const auto& child : item->children)
            CollectContentInputsFromItem(child.get(), builder, visitedObjects, requiredMedia,
                                         project, platform, language, excludedObjectIds);
        return;
    }

    if (excludedObjectIds && excludedObjectIds->count(item->objectID))
        return;

    if (visitedObjects.count(item->objectID)) return;

    visitedObjects.insert(item->objectID);

    const auto objectId = stableIdForItem(item, platform, language);
    if (item->type == ProjectItem::Type::Sound)
    {
        const uint32_t sourceId = SoundSourceEditing::activeSourceId(
            *item, platform, SoundSourceEditing::exportLanguage(*item, language));
        if (sourceId != 0)
            requiredMedia.insert(sourceId);
    }

    builder.addStringMapping(objectId, item->name);

    const auto* mix = item->comp<VMixNode>();
    if (mix && !mix->bindings.isEmpty())
    {
        for (const ParameterBindingDef& authoredBinding : mix->bindings.entries())
        {
            if (authoredBinding.typed) continue;
            const QJsonValue val(authoredBinding.toJson());
            QJsonObject mj = val.toObject();
            Bridge::PackModulator mod;
            mod.id           = counterToFNV(project,
                                            static_cast<uint32_t>(mj["id"].toInt()));
            mod.subtype      = static_cast<uint8_t>(mj["subtype"].toInt());
            mod.scope        = static_cast<uint8_t>(mj["scope"].toInt());
            mod.lfoFrequency = static_cast<float>(mj["lfoFrequency"].toDouble(1.0));
            mod.lfoDepth     = static_cast<float>(mj["lfoDepth"].toDouble(1.0));
            mod.lfoOffset    = static_cast<float>(mj["lfoOffset"].toDouble());
            mod.lfoPwm       = static_cast<float>(mj["lfoPwm"].toDouble(0.5));
            mod.lfoSmooth    = static_cast<float>(mj["lfoSmooth"].toDouble());
            mod.lfoWaveform  = static_cast<uint8_t>(mj["lfoWaveform"].toInt());
            mod.lfoUnipolar  = static_cast<uint8_t>(mj["lfoUnipolar"].toInt());
            mod.envAttackTime   = static_cast<float>(mj["envAttackTime"].toDouble(0.01));
            mod.envHoldTime     = static_cast<float>(mj["envHoldTime"].toDouble());
            mod.envDecayTime    = static_cast<float>(mj["envDecayTime"].toDouble(0.1));
            mod.envSustainLevel = static_cast<float>(mj["envSustainLevel"].toDouble(1.0));
            mod.envReleaseTime  = static_cast<float>(mj["envReleaseTime"].toDouble(0.1));
            mod.envAttackCurve  = static_cast<uint8_t>(mj["envAttackCurve"].toInt());
            mod.envDecayCurve   = static_cast<uint8_t>(mj["envDecayCurve"].toInt());
            mod.envReleaseCurve = static_cast<uint8_t>(mj["envReleaseCurve"].toInt());
            mod.envAutoRelease  = static_cast<uint8_t>(mj["envAutoRelease"].toInt());
            mod.timeDurationSec  = static_cast<float>(mj["timeDurationSec"].toDouble(mj["timeDuration"].toDouble(1.0)));
            mod.timeInitialValue = static_cast<float>(mj["timeInitialValue"].toDouble());
            mod.timeTargetValue  = static_cast<float>(mj["timeTargetValue"].toDouble(1.0));
            mod.timeCurveType    = static_cast<uint8_t>(mj["timeCurveType"].toInt());
            mod.timeLoop         = static_cast<uint8_t>(mj["timeLoop"].toInt());
            mod.timePingPong     = static_cast<uint8_t>(mj["timePingPong"].toInt());

            QJsonArray bindings = mj["bindings"].toArray();
            for (const QJsonValue& bv : bindings)
            {
                QJsonObject bj = bv.toObject();
                Bridge::PackModulator::Binding be;
                be.targetParam = static_cast<uint8_t>(bj["targetParam"].toInt());
                be.curveType   = static_cast<uint8_t>(bj["curveType"].toInt());
                be.enabled     = static_cast<uint8_t>(bj["enabled"].toInt(1));
                be.mappedMin   = static_cast<float>(bj["min"].toDouble());
                be.mappedMax   = static_cast<float>(bj["max"].toDouble(1.0));
                mod.bindings.push_back(be);
            }

            builder.addModulatorBinding(objectId, mod);
        }
    }

    for (const auto& child : item->children)
        CollectContentInputsFromItem(child.get(), builder, visitedObjects, requiredMedia,
                                     project, platform, language, excludedObjectIds);
}

bool ProjectPackExporter::CollectTypedBindings(const ProjectData& project,
                                               Bridge::PackBuildSession& builder,
                                               const std::set<harkenstudio::ObjectId>* allowedRuntimeObjectIds,
                                               QString* errorOut,
                                               PackBuildContext* buildContext)
{
    std::optional<BindingRuntimeLowering> ownedLowering;
    const auto& lowering = buildContext ? buildContext->bindingLowering()
        : ownedLowering.emplace(BindingRuntimeLoweringSemantics::lowerProject(project));
    if (lowering.failed) {
        if (errorOut)
            *errorOut = lowering.error;
        return false;
    }
    std::set<harkenstudio::ObjectId> keptBindingIds;
    for (const BindingRuntimeAttachment& attachment : lowering.attachments) {
        const bool global = attachment.scopeKind == harkenstudio::kTargetScopeBus ||
            attachment.scopeKind == harkenstudio::kTargetScopeBusEffectParam;
        if (global != (allowedRuntimeObjectIds == nullptr))
            continue;
        if (allowedRuntimeObjectIds && allowedRuntimeObjectIds->count(attachment.objectId) == 0)
            continue;
        Bridge::PackBindingAttachment row;
        row.bindingId = attachment.bindingId;
        row.scope = static_cast<uint8_t>(attachment.scopeKind);
        row.objectId = attachment.objectId;
        builder.addBindingAttachment(row);
        keptBindingIds.insert(attachment.bindingId);
    }
    for (const BindingRuntimeProgram& program : lowering.programs) {
        if (keptBindingIds.count(program.bindingId) == 0)
            continue;
        Bridge::PackParameterBinding binding;
        binding.id = program.bindingId;
        binding.variableId = program.variableId;
        binding.scope = static_cast<uint8_t>(program.targetScopeKind);
        binding.target = program.targetId;
        binding.transitionMs = program.transitionMs;
        for (const CurvePointDef& point : program.points)
            binding.points.push_back({point.x, point.value});
        for (const CurveHandlePairDef& handle : program.handles)
            binding.handles.push_back({handle.h1x, handle.h1y, handle.h2x, handle.h2y});
        builder.addBinding(binding);
    }
    return true;
}

bool ProjectPackExporter::CollectMedia(const ProjectData& project,
                                         Bridge::PackBuildSession& builder,
                                         const std::set<uint32_t>& requiredMedia,
                                         const std::set<uint32_t>* includedObjectIds,
                                         const ExportOptions& options,
                                         MediaCollectionStats* stats,
                                         QString* errorOut,
                                         const ActionClosurePlan* closure,
                                         const std::set<uint32_t>* excludedClosureMediaIds)
{
    const auto canceled = [&] {
        if (!options.cancelCheck || !options.cancelCheck()) return false;
        if (errorOut) *errorOut = QStringLiteral("Export canceled");
        return true;
    };
    if (canceled()) return false;
    QString projectDir = QFileInfo(project.projectPath).absolutePath();
    QString AudioSourcesDir = PathUtils::getAudioSourcesDir(projectDir);
    const ProjectPlatformId platform = QUuid::fromString(options.platform);
    const auto& mediaSnapshot = options.snapshot->media;
    AssetConverter converter;
    converter.setBuildSnapshot(mediaSnapshot);

    std::set<uint32_t> processedMedia;
    std::set<harkenstudio::ObjectId> processedSounds;

    if (closure && (!closure->media.empty() || !closure->controlMedia.empty())) {
        const auto& sourceIndex = *options.buildContext->capabilities->sourceIndex();
        const QString derivativeDir = QDir(PathUtils::getCacheDir(projectDir))
            .filePath(QStringLiteral("pack-derivatives/%1").arg(options.platform));
        auto inputs = closure->media;
        inputs.insert(inputs.end(), closure->controlMedia.begin(), closure->controlMedia.end());
        for (const ActionClosureMedia& media : inputs) {
            if (canceled()) return false;
            if (processedSounds.count(media.soundStableId) != 0) continue;
            const auto matchesSound = [&](const auto& candidate) {
                return candidate.soundStableId == media.soundStableId;
            };
            const bool controlInput = std::any_of(
                closure->controlMedia.begin(), closure->controlMedia.end(), matchesSound);
            const auto owner = sourceIndex.byQtGuid.find(QUuid(objectIdString(media.soundStableId)));
            const ProjectItem* soundItem = owner == sourceIndex.byQtGuid.end()
                ? nullptr : owner->second;
            const auto* sound = soundItem ? soundItem->comp<VSound>() : nullptr;
            if (!sound) {
                if (errorOut) *errorOut = QStringLiteral("Closure media owner %1 is missing")
                    .arg(VNX::objectIdString(media.soundStableId));
                return false;
            }
            const bool publish = std::any_of(closure->media.begin(), closure->media.end(), matchesSound)
                && (!includedObjectIds || includedObjectIds->count(soundItem->objectID) != 0)
                && (!excludedClosureMediaIds
                    || excludedClosureMediaIds->count(media.authoringSourceId) == 0);
            if (!publish && !controlInput) continue;
            const auto* source = SoundSourceSemantics::findEntry(*sound, media.sourceEntryId);
            if (!source) {
                if (errorOut) *errorOut = QStringLiteral("Closure source entry is missing for Sound %1")
                    .arg(VNX::objectIdString(media.soundStableId));
                return false;
            }
            QString derivativeError;
            const auto conversion = mediaSnapshot.request(source->sourceId, platform, &derivativeError);
            if (!conversion || !conversion->sourceMatches()) {
                if (errorOut) *errorOut = conversion
                    ? QStringLiteral("Source file changed or is unavailable for Sound %1").arg(VNX::objectIdString(media.soundStableId))
                    : derivativeError;
                return false;
            }
            const auto authoredSource = SoundSourceSemantics::authoredView(*source, &conversion->asset);
            auto artifact = SoundSourceDerivative::materialize(
                conversion->inputPath(), authoredSource,
                sound->scheduleOffsetSec, platform, derivativeDir, conversion->settings,
                &derivativeError, options.snapshot->derivativeCache.get(), conversion->sourceInput.get());
            if (canceled()) return false;
            if (!conversion->sourceMatches()) {
                if (errorOut) *errorOut = QStringLiteral("Source file changed during Sound conversion");
                return false;
            }
            const QString conversionError = derivativeError;
            bool fallback = false;
            if (!artifact) {
                artifact = SoundSourceDerivative::materialize(
                    conversion->inputPath(), authoredSource,
                    sound->scheduleOffsetSec, platform, derivativeDir, ConversionSettings{},
                    &derivativeError, options.snapshot->derivativeCache.get(), conversion->sourceInput.get());
                if (canceled()) return false;
                fallback = artifact.has_value();
            }
            if (!artifact || !conversion->sourceMatches()) {
                if (errorOut) *errorOut = QStringLiteral("Derivative media failed for Sound %1: %2")
                    .arg(VNX::objectIdString(media.soundStableId)).arg(derivativeError);
                return false;
            }
            if (stats) {
                if (fallback) {
                    ++stats->fallbacks;
                    stats->warnings.append(QStringLiteral("Sound %1 used unprocessed media after conversion failed: %2")
                        .arg(VNX::objectIdString(media.soundStableId)).arg(conversionError));
                } else if (artifact->cacheHit) ++stats->cacheHits;
                else ++stats->conversions;
            }
            Bridge::PackMediaDesc desc;
            desc.sourceId = media.soundStableId;
            desc.authoringSourceId = source->sourceId;
            desc.sourcePath = conversion->asset.absolutePath;
            desc.interleavedSamples = artifact->samples;
            desc.contentHash = artifact->contentHash;
            desc.sampleRate = artifact->sampleRate;
            desc.channels = artifact->channels;
            desc.loopable = artifact->loopable;
            desc.loopStartFrame = artifact->loopStartFrame;
            desc.loopEndFrame = artifact->loopEndFrame;
            desc.format = Bridge::PackMediaFormat::PcmFloat;
            if (publish)
                builder.addMedia(desc);
            if (!publish || options.buildPlan) {
                const auto revision = options.snapshot->derivativeCache->contentRevision(*artifact);
                if (!stats || !revision) {
                    if (errorOut) *errorOut = QStringLiteral("Cannot compute converted media revision for Sound %1")
                        .arg(VNX::objectIdString(media.soundStableId));
                    return false;
                }
                stats->soundContentRevisions.emplace(media.soundStableId, *revision);
            }
            processedSounds.insert(media.soundStableId);
        }
    }

    auto stableSoundsForSource = [&](uint32_t sourceID) {
        std::set<harkenstudio::ObjectId> sounds;
        std::function<void(const ProjectItem*)> scan = [&](const ProjectItem* item) {
            if (!item)
                return;
            if (item->type == ProjectItem::Type::Sound &&
                SoundSourceEditing::activeSourceId(
                    *item, exportPlatformOf(options),
                    SoundSourceEditing::exportLanguage(*item, exportLanguageOf(options))) == sourceID &&
                (!includedObjectIds ||
                 includedObjectIds->count(item->objectID) != 0))
            {
                const harkenstudio::ObjectId soundId = stableIdForItem(
                    item, exportPlatformOf(options),
                    SoundSourceEditing::exportLanguage(*item, exportLanguageOf(options)));
                if (!harkenstudio::IsNull(soundId) && processedSounds.count(soundId) == 0)
                    sounds.insert(soundId);
            }
            for (const auto& child : item->children)
                scan(child.get());
        };
        if (project.root)
            scan(project.root.get());
        for (const auto* binding : project.allFileBindings())
            if (binding && binding->rootItem)
                scan(binding->rootItem.get());
        return sounds;
    };

    for (uint32_t sourceID : requiredMedia)
    {
        if (canceled()) return false;
        if (processedMedia.count(sourceID)) continue;
        processedMedia.insert(sourceID);
        const auto sounds = stableSoundsForSource(sourceID);
        if (sounds.empty()) continue;
        const auto assetIt = mediaSnapshot.assets.constFind(sourceID);
        const AssetInfo* assetInfo = assetIt == mediaSnapshot.assets.cend() ? nullptr : &*assetIt;
        std::optional<MediaConversionRequest> inputRequest;
        QString filePath;

        if (assetInfo && mediaSnapshot.sourceInputs) {
            inputRequest = mediaSnapshot.request(sourceID, platform, errorOut);
            if (!inputRequest) return false;
            assetInfo = &inputRequest->asset;
            filePath = inputRequest->inputPath();
        } else if (assetInfo && !assetInfo->absolutePath.isEmpty()) {
            filePath = assetInfo->absolutePath;
        } else if (assetInfo && !assetInfo->relativePath.isEmpty()) {
            filePath = AudioSourcesDir + "/" + assetInfo->relativePath;
        } else {
            const AudioSourceData* srcPtr = nullptr;
            for (const auto& as : project.audioSources) {
                if (as.sourceID == sourceID) { srcPtr = &as; break; }
            }
            if (!srcPtr) continue;
            filePath = AudioSourcesDir + "/" + srcPtr->relativePath;
        }

        filePath = QFileInfo(filePath).absoluteFilePath();
        if (!QFileInfo::exists(filePath)) {
            if (errorOut) {
                *errorOut = QStringLiteral("Pack source file does not exist for sourceID %1: %2")
                                .arg(sourceID)
                                .arg(filePath);
            }
            return false;
        }

        QString packInputPath;
        LoopInfo loopInfo;
        bool loopEndExclusive = false;

        if (assetInfo) {
            const ConversionResult conversion = inputRequest
                ? converter.convert(*inputRequest) : converter.convert(sourceID, platform);
            if (canceled()) return false;
            if (conversion.success) {
                packInputPath = conversion.absolutePath;
                loopInfo = conversion.cacheEntry.loopInfo;
                loopEndExclusive = true;
                if (stats) {
                    if (conversion.cacheHit) ++stats->cacheHits;
                    else ++stats->conversions;
                }
            } else {
                packInputPath = filePath;
                loopInfo = assetInfo->metadata.loopInfo;
                if (stats) {
                    ++stats->fallbacks;
                    stats->warnings.append(QStringLiteral("sourceID %1 fell back to source file after conversion failed: %2")
                        .arg(sourceID).arg(conversion.errorMessage));
                }
            }
        }

        if (packInputPath.isEmpty()) {
            packInputPath = filePath;
            if (assetInfo)
                loopInfo = assetInfo->metadata.loopInfo;
            if (stats) {
                ++stats->fallbacks;
                stats->warnings.append(
                    QStringLiteral("sourceID %1 used source file because no registry cache entry was available")
                        .arg(sourceID));
            }
        }

        packInputPath = QFileInfo(packInputPath).absoluteFilePath();
        if (!QFileInfo::exists(packInputPath)) {
            if (errorOut) {
                *errorOut = QStringLiteral("Pack input file does not exist for sourceID %1: %2")
                                .arg(sourceID)
                                .arg(packInputPath);
            }
            return false;
        }

        for (const auto& soundId : sounds) {
            if (canceled()) return false;
            Bridge::PackMediaDesc desc;
            desc.sourceId = soundId;
            desc.authoringSourceId = sourceID;
            desc.sourcePath = packInputPath;
            desc.loopable = loopInfo.enabled
                && (loopEndExclusive
                        ? loopInfo.loopEnd > loopInfo.loopStart
                        : loopInfo.loopEnd >= loopInfo.loopStart)
                && (loopEndExclusive
                        || loopInfo.loopEnd != std::numeric_limits<uint64_t>::max());
            desc.loopStartFrame = loopInfo.loopStart;
            desc.loopEndFrame = desc.loopable
                ? loopInfo.loopEnd + (loopEndExclusive ? 0 : 1) : 0;
            desc.format = Bridge::PackMediaFormat::PcmFloat;
            builder.addMedia(desc);
        }
    }
    return true;
}


void ProjectPackExporter::CollectBuses(const ProjectData& project,
                                       Bridge::PackBuildSession& builder,
                                       PlatformScope scope)
{
    forEachBusItem(project, [&](const ProjectItem* item) {
        Bridge::PackBus bd;
        bd.id = item->runtimeId();
        bd.parentId = (item->parent && item->parent->type == ProjectItem::Type::Bus)
                          ? item->parent->runtimeId() : harkenstudio::kMasterBusObjectId;
        bd.name     = item->name;
        if (const auto* mix = item->comp<VMixNode>()) {
            for (const auto& effect : mix->effects.entries()) {
                bd.effects.push_back({effect.runtimeId(), static_cast<uint32_t>(effect.type),
                    effect.bypass, effect.parameters});
            }
        }
        const auto locator = PlayablePropertyAuthoringV1::definitionLocator(*item);
        if (!locator) {
            builder.addWarning(
                QStringLiteral("Bus '%1': authored mix is not readable")
                    .arg(item->name));
            builder.addBus(bd);
            return;
        }
        const auto readMixSlot = [&](harkenplayableproperties::PlayablePropertyIdV1 id,
                                     float* out,
                                     const char* slotName) {
            const auto state = PlayablePropertyAuthoringV1::readProperty(
                const_cast<ProjectItem&>(*item),
                *locator,
                static_cast<uint8_t>(id),
                scope);
            if (state && state->value.isDouble()) {
                const double value = state->value.toDouble();
                if (std::isfinite(value)) {
                    *out = static_cast<float>(value);
                    return;
                }
            }
            *out = 0.0f;
            builder.addWarning(
                QStringLiteral("Bus '%1': authored %2 is not readable")
                    .arg(item->name, QString::fromUtf8(slotName)));
        };
        readMixSlot(harkenplayableproperties::PlayablePropertyIdV1::Volume,
                    &bd.volume, "volume");
        readMixSlot(harkenplayableproperties::PlayablePropertyIdV1::OutputBusVolume,
                    &bd.outputBusVolume, "outputBusVolume");
        readMixSlot(harkenplayableproperties::PlayablePropertyIdV1::MakeUpGain,
                    &bd.makeUpGain, "makeUpGain");
        builder.addBus(bd);
    });
}

bool ProjectPackExporter::CollectSends(const ProjectData& project,
                                       Bridge::PackBuildSession& builder,
                                       PlatformScope scope, QString* errorOut)
{
    bool valid = true;
    forEachBusItem(project, [&](const ProjectItem* item) {
        if (!valid) return;
        const auto locator = PlayablePropertyAuthoringV1::definitionLocator(*item);
        if (!locator) {
            if (errorOut) *errorOut = QStringLiteral("Bus '%1' sends are not readable")
                .arg(item->name);
            valid = false;
            return;
        }
        const auto state = PlayablePropertyAuthoringV1::readProperty(
            const_cast<ProjectItem&>(*item),
            *locator,
            static_cast<uint8_t>(harkenplayableproperties::PlayablePropertyIdV1::Sends),
            scope);
        if (!state || !state->value.isArray()) {
            if (errorOut) *errorOut = QStringLiteral("Bus '%1' sends are not readable")
                .arg(item->name);
            valid = false;
            return;
        }
        const harkenstudio::ObjectId from = item->runtimeId();
        for (qsizetype index = 0; index < state->value.toArray().size(); ++index) {
            const QJsonValue rowValue = state->value.toArray().at(index);
            const QJsonObject row = rowValue.toObject();
            const auto targetValue = row.value(QStringLiteral("targetBusID"));
            const auto levelValue = row.value(QStringLiteral("sendLevel"));
            const double targetNumber = targetValue.toDouble(-1.0);
            const double levelNumber = levelValue.toDouble(
                std::numeric_limits<double>::quiet_NaN());
            const float levelDb = static_cast<float>(levelNumber);
            const bool wellFormed = rowValue.isObject() && targetValue.isDouble() &&
                targetNumber >= 0.0 && targetNumber <= UINT32_MAX &&
                std::trunc(targetNumber) == targetNumber && levelValue.isDouble() &&
                std::isfinite(levelNumber) && std::isfinite(levelDb) &&
                levelDb >= -96.0f && levelDb <= 24.0f &&
                std::isfinite(VMixNode::dbToLinear(levelDb));
            const harkenstudio::ObjectId to = wellFormed
                ? busIdentityForHandle(project, static_cast<uint32_t>(targetNumber))
                : harkenstudio::ObjectId{};
            if (!wellFormed || harkenstudio::IsNull(to) || to == from) {
                if (errorOut) *errorOut = QStringLiteral("Bus '%1' send row %2 is invalid")
                    .arg(item->name).arg(index);
                valid = false;
                return;
            }
            Bridge::PackBusSend edge;
            edge.fromBusId = from;
            edge.toBusId = to;
            edge.levelDb = levelDb == 0.0f ? 0.0f : levelDb;
            builder.addBusSend(edge);
        }
    });
    return valid;
}

void ProjectPackExporter::CollectAttenuations(const ProjectData& project,
                                                      Bridge::PackBuildSession& builder)
{
    for (const auto* def : AttenuationStore::all(project))
    {
        if (def->hostItem()->name.isEmpty()) continue;

        Bridge::PackAttenuation att;
        att.id = AttenuationStore::runtimeId(project, *def);
        att.maxDistance = def->maxDistance;

        // Authored points and handles pass through verbatim: every load and
        // write path funnels through attenuationNormalizeCurve, so the domain
        // invariants packc hard-fails on are already guaranteed here.
        for (const auto& dimension : def->dimensions)
        {
            if (dimension.curve.isEmpty()) continue;
            if (dimension.type != AttenuationDimension::Volume) continue;
            Bridge::PackAttenuation::Slot s;
            s.slot = static_cast<uint8_t>(dimension.type);
            s.points.reserve(static_cast<size_t>(dimension.curve.size()));
            for (const auto& p : dimension.curve)
                s.points.push_back({p.x, p.value});
            s.handles.reserve(static_cast<size_t>(dimension.handles.size()));
            for (const auto& h : dimension.handles)
                s.handles.push_back({h.h1x, h.h1y, h.h2x, h.h2y});
            att.curveSlots.push_back(std::move(s));
        }

        builder.addAttenuation(att);
    }
}


ProjectPackExporter::ValidationResult
ProjectPackExporter::ValidateBeforeBuild(const ProjectData& project)
{
    const auto media = AssetRegistry::instance().buildSnapshot(
        QFileInfo(project.projectPath).absolutePath(), project.platforms);

    ValidationResult result;
    QString referenceError;
    if (!validatePlaybackReferences(project, &referenceError)) {
        result.issues.push_back({ValidationIssue::Error, referenceError, 0});
        result.errorCount = 1;
        result.valid = false;
        return result;
    }

    QString projectDir = QFileInfo(project.projectPath).absolutePath();
    QString AudioSourcesDir = PathUtils::getAudioSourcesDir(projectDir);

    std::set<uint32_t> allObjectIDs;
    std::set<uint32_t> referencedMedia;

    for (const auto* binding : project.allFileBindings()) {
        if (!binding || !binding->rootItem) continue;
        const auto* wu = binding;
        std::function<void(const ProjectItem*)> collectAndValidate =
            [&](const ProjectItem* item) {
            if (!item) return;

            if (item->objectID != 0) {
                if (allObjectIDs.count(item->objectID)) {
                    result.issues.push_back({ValidationIssue::Warning,
                        QString("Duplicate objectID 0x%1 for '%2'")
                            .arg(item->objectID, 8, 16, QChar('0'))
                            .arg(item->name),
                        item->objectID});
                    ++result.warningCount;
                }
                allObjectIDs.insert(item->objectID);
            }

            const uint32_t sourceId = item->type == ProjectItem::Type::Sound
                ? SoundSourceEditing::activeSourceId(
                      *item, ProjectPlatformId{}, {})
                : 0;
            if (sourceId != 0) {
                referencedMedia.insert(sourceId);

                bool found = false;
                const auto assetIt = media.assets.constFind(sourceId);
                const AssetInfo* assetInfo = assetIt == media.assets.cend() ? nullptr : &*assetIt;
                if (assetInfo && !assetInfo->absolutePath.isEmpty()) {
                    found = QFileInfo::exists(assetInfo->absolutePath);
                } else if (assetInfo && !assetInfo->relativePath.isEmpty()) {
                    found = QFileInfo::exists(AudioSourcesDir + "/" + assetInfo->relativePath);
                } else {
                    for (const auto& as : project.audioSources) {
                        if (as.sourceID == sourceId) {
                            found = QFileInfo::exists(AudioSourcesDir + "/" + as.relativePath);
                            break;
                        }
                    }
                }

                if (!found) {
                    result.issues.push_back({ValidationIssue::Error,
                        QString("Media file missing for Sound '%1' (sourceID=0x%2)")
                            .arg(item->name)
                            .arg(sourceId, 8, 16, QChar('0')),
                        item->objectID});
                    ++result.errorCount;
                    result.valid = false;
                }
            }

            for (const auto& child : item->children)
                collectAndValidate(child.get());
        };
        collectAndValidate(wu->rootItem.get());
    }

    for (const auto* binding : project.allFileBindings()) {
        if (!binding || !binding->rootItem) continue;
        const auto* wu = binding;
        std::function<void(const ProjectItem*)> validateRefs =
            [&](const ProjectItem* item) {
            if (!item) return;

            if (item->type == ProjectItem::Type::Action) {
                const auto* evtC = item->comp<VAction>();
                if (!evtC) { for (const auto& child : item->children) validateRefs(child.get()); return; }
                for (const auto& action : evtC->steps) {
                    if (action.targetObjectID != 0 &&
                        !allObjectIDs.count(action.targetObjectID)) {
                        result.issues.push_back({ValidationIssue::Warning,
                            QString("Action '%1' action targets unknown object 0x%2")
                                .arg(item->name)
                                .arg(action.targetObjectID, 8, 16, QChar('0')),
                            item->objectID});
                        ++result.warningCount;
                    }
                }
            }

            for (const auto& child : item->children)
                validateRefs(child.get());
        };
        validateRefs(wu->rootItem.get());
    }

    if (result.issues.empty()) {
        result.issues.push_back({ValidationIssue::Info,
            QString("Validation passed: %1 objects, %2 media files")
                .arg(allObjectIDs.size()).arg(referencedMedia.size()), 0});
    }

    return result;
}

ProjectPackExporter::ValidationResult
ProjectPackExporter::ValidatePackBuildReadiness(const ProjectData& project,
                                                const QString& outputDir,
                                                PackBuildContext* buildContext)
{
    if (PathUtils::isProtectedPackOutputPath(outputDir, project.projectPath)
        && !PathUtils::isProjectRootDirectory(outputDir, project.projectPath)) {
        ValidationResult result;
        result.issues.push_back({ValidationIssue::Error, PathUtils::packBuildOutputLocationError(), 0});
        result.errorCount = 1;
        result.valid = false;
        return result;
    }
    ValidationResult result;
    QString referenceError;
    if (!(buildContext ? buildContext->validatePlayback(&referenceError)
                       : validatePlaybackReferences(project, &referenceError))) {
        result.issues.push_back({ValidationIssue::Error, referenceError, 0});
        result.errorCount = 1;
        result.valid = false;
        return result;
    }


    {
        QString cur = QDir(outputDir).absolutePath();
        QFileInfo fi(cur);
        while (!fi.exists()) {
            const QString parent = fi.absolutePath();
            if (parent.isEmpty() || parent.length() >= cur.length())
                break;
            cur = parent;
            fi.setFile(cur);
        }

        if (fi.exists() && !fi.isDir()) {
            result.issues.push_back({ValidationIssue::Error,
                QStringLiteral("Output path is a file, not a directory: %1").arg(outputDir),
                0});
            ++result.errorCount;
            result.valid = false;
        } else if (!fi.exists() || !fi.isWritable()) {
            result.issues.push_back({ValidationIssue::Error,
                QStringLiteral("Output directory is not writable: %1").arg(outputDir),
                0});
            ++result.errorCount;
            result.valid = false;
        }
    }

    const auto sourceIndex = buildContext ? buildContext->capabilities->sourceIndex()
        : Bridge::canonicalRuntimeProjectSourceIndex(project);
    const auto& allActions = sourceIndex->actions;
    std::set<harkenstudio::ObjectId> actionStableIds;
    for (const ProjectItem* action : allActions)
        if (action->comp<VAction>()) actionStableIds.insert(action->runtimeId());

    std::unordered_map<harkenstudio::ObjectId, QString> memberOwner;
    std::set<QString> groupNames;
    for (const PackGroup& group : project.packGroups) {
        if (!groupNames.insert(group.name.toCaseFolded()).second) {
            result.issues.push_back({ValidationIssue::Error,
                QStringLiteral("Pack group name '%1' is duplicated (case-insensitive)")
                    .arg(group.name), 0});
            ++result.errorCount;
            result.valid = false;
        }
        for (harkenstudio::ObjectId memberId : group.actionIds) {
            if (!actionStableIds.count(memberId)) {
                result.issues.push_back({ValidationIssue::Warning,
                    QStringLiteral("Pack group '%1' member %2 does not resolve to an Action")
                        .arg(group.name)
                        .arg(VNX::objectIdString(memberId)),
                    0});
                ++result.warningCount;
            }

            auto owner = memberOwner.find(memberId);
            if (owner == memberOwner.end()) {
                memberOwner.emplace(memberId, group.name);
            } else {
                result.issues.push_back({ValidationIssue::Error,
                    QStringLiteral("Action %1 is assigned to multiple pack groups "
                                   "('%2' and '%3'); each Action may belong to at most one group")
                        .arg(VNX::objectIdString(memberId))
                        .arg(owner->second)
                        .arg(group.name),
                    0});
                ++result.errorCount;
                result.valid = false;
            }
        }
    }

    for (const ProjectItem* evt : allActions) {
        const auto* actionComp = evt->comp<VAction>();
        if (!actionComp) continue;
        for (const auto& action : actionComp->steps) {
            if (action.targetObjectID != 0 &&
                !sourceIndex->byObjectId.count(action.targetObjectID)) {
                result.issues.push_back({ValidationIssue::Warning,
                    QStringLiteral("Action '%1' step targets unknown object 0x%2")
                        .arg(evt->name)
                        .arg(action.targetObjectID, 8, 16, QChar('0')),
                    evt->objectID});
                ++result.warningCount;
            }
        }
    }

    if (result.issues.empty()) {
        result.issues.push_back({ValidationIssue::Info,
            QStringLiteral("Project packs are ready to build"), 0});
    }

    return result;
}

std::vector<ProjectPackExporter::DependencyNode>
ProjectPackExporter::GetDependencyTree(const ProjectData& project)
{
    std::vector<DependencyNode> roots;

    auto typeToString = [](ProjectItem::Type t) -> QString {
        return ItemTypeCatalog::packDependencyName(t);
    };

    std::function<DependencyNode(const ProjectItem*)> buildNode;
    buildNode = [&](const ProjectItem* item) -> DependencyNode {
        DependencyNode node;
        node.objectID = item->objectID;
        node.name     = item->name;
        node.type     = typeToString(item->type);

        for (const auto& child : item->children) {
            if (!child) continue;
            QString childType = typeToString(child->type);
            if (!childType.isEmpty()) {
                node.children.push_back(buildNode(child.get()));
            }
        }
        return node;
    };

    for (const auto* binding : project.allFileBindings()) {
        if (!binding || !binding->rootItem) continue;
        const auto* wu = binding;
        for (const auto& child : wu->rootItem->children) {
            if (!child) continue;
            QString t = typeToString(child->type);
            if (!t.isEmpty()) {
                roots.push_back(buildNode(child.get()));
            }
        }
    }

    return roots;
}

QJsonArray ProjectPackExporter::GetDependencyTreeJson(const ProjectData& project)
{
    auto tree = GetDependencyTree(project);

    std::function<QJsonObject(const DependencyNode&)> toJson;
    toJson = [&](const DependencyNode& node) -> QJsonObject {
        QJsonObject obj;
        obj["objectID"] = QString("0x%1").arg(node.objectID, 8, 16, QChar('0'));
        obj["name"]     = node.name;
        obj["type"]     = node.type;
        if (!node.children.empty()) {
            QJsonArray arr;
            for (const auto& child : node.children)
                arr.append(toJson(child));
            obj["children"] = arr;
        }
        return obj;
    };

    QJsonArray arr;
    for (const auto& root : tree)
        arr.append(toJson(root));
    return arr;
}


void ProjectPackExporter::CollectReferencedMedia(const ProjectData& project,
                                                   std::set<uint32_t>& referencedMedia)
{
    for (const auto* binding : project.allFileBindings()) {
        if (!binding || !binding->rootItem) continue;
        const auto* wu = binding;
        std::function<void(const ProjectItem*)> scan = [&](const ProjectItem* item) {
            if (!item) return;
            if (item->type == ProjectItem::Type::Sound) {
                const uint32_t sourceId = SoundSourceEditing::activeSourceId(
                    *item, ProjectPlatformId{}, {});
                if (sourceId != 0)
                    referencedMedia.insert(sourceId);
            }
            for (const auto& child : item->children)
                scan(child.get());
        };
        scan(wu->rootItem.get());
    }
}

std::vector<ProjectPackExporter::UnusedAsset>
ProjectPackExporter::FindUnusedAssets(const ProjectData& project)
{
    std::set<uint32_t> referencedMedia;
    CollectReferencedMedia(project, referencedMedia);

    std::vector<UnusedAsset> unused;

    for (const auto& as : project.audioSources) {
        if (referencedMedia.find(as.sourceID) == referencedMedia.end()) {
            UnusedAsset ua;
            ua.sourceID = as.sourceID;
            ua.name     = as.relativePath;
            ua.path     = as.relativePath;
            unused.push_back(ua);
        }
    }

    return unused;
}

QJsonArray ProjectPackExporter::FindUnusedAssetsJson(const ProjectData& project)
{
    auto unused = FindUnusedAssets(project);
    QJsonArray arr;
    for (const auto& u : unused) {
        QJsonObject obj;
        obj["sourceID"] = QString("0x%1").arg(u.sourceID, 8, 16, QChar('0'));
        obj["name"]     = u.name;
        obj["path"]     = u.path;
        arr.append(obj);
    }
    return arr;
}


QString ProjectPackExporter::ResolveAudioFilePath(const ProjectData& project,
                                                   uint32_t sourceID,
                                                   const QString& AudioSourcesDir,
                                                   const MediaBuildSnapshot* media)
{
    const auto captured = media ? MediaBuildSnapshot{} : AssetRegistry::instance().buildSnapshot(
        QFileInfo(project.projectPath).absolutePath(), project.platforms);
    if (!media) media = &captured;
    const auto asset = media->assets.constFind(sourceID);
    const AssetInfo* assetInfo = asset == media->assets.cend() ? nullptr : &*asset;
    if (assetInfo && !assetInfo->absolutePath.isEmpty())
        return assetInfo->absolutePath;
    if (assetInfo && !assetInfo->relativePath.isEmpty())
        return AudioSourcesDir + "/" + assetInfo->relativePath;

    for (const auto& as : project.audioSources) {
        if (as.sourceID == sourceID)
            return AudioSourcesDir + "/" + as.relativePath;
    }

    return {};
}

bool ProjectPackExporter::WriteHarkenPeakFile(const QString& outputPath,
                                              const WaveformPeakData& data)
{
    if (!data.isValid())
        return false;

    QFile file(outputPath);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "[PackExporter] Failed to create .harkenpeak file:" << outputPath;
        return false;
    }

    QDataStream out(&file);
    out.setByteOrder(QDataStream::LittleEndian);
    out.setFloatingPointPrecision(QDataStream::SinglePrecision);

    out.writeRawData("VPEK", 4);
    out << static_cast<uint16_t>(2);
    out << static_cast<uint16_t>(1);
    out << static_cast<uint32_t>(data.sampleRate);
    out << static_cast<uint32_t>(data.numPeaks);

    double durationSec = data.durationSec;
    if (durationSec <= 0.0 && data.sampleRate > 0)
        durationSec = static_cast<double>(data.numPeaks) * data.samplesPerPeak / data.sampleRate;
    out.setFloatingPointPrecision(QDataStream::DoublePrecision);
    out << durationSec;
    out.setFloatingPointPrecision(QDataStream::SinglePrecision);

    uint32_t ch0Floats = data.numPeaks * 2;
    for (uint32_t i = 0; i < ch0Floats && i < data.peaks.size(); ++i)
        out << data.peaks[i];

    file.close();
    return true;
}

int ProjectPackExporter::GenerateHarkenPeakFiles(const ProjectData& project,
                                                 const QString& outputDir)
{
    QString projectDir = QFileInfo(project.projectPath).absolutePath();
    QString AudioSourcesDir = PathUtils::getAudioSourcesDir(projectDir);
    int filesWritten = 0;

    struct ActionPeakInfo {
        QString actionName;
        std::vector<uint32_t> sourceIDs;
    };
    std::vector<ActionPeakInfo> actionPeaks;

    QHash<uint32_t, const ProjectItem*> objectLookup;
    std::function<void(const ProjectItem*)> buildLookup = [&](const ProjectItem* item) {
        if (!item) return;
        if (item->objectID != 0)
            objectLookup[item->objectID] = item;
        for (const auto& child : item->children)
            buildLookup(child.get());
    };
    for (const auto* binding : project.allFileBindings()) {
        if (binding && binding->rootItem)
            buildLookup(binding->rootItem.get());
    }

    std::function<void(const ProjectItem*, std::vector<uint32_t>&)> collectSourceIDs;
    collectSourceIDs = [&](const ProjectItem* item, std::vector<uint32_t>& ids) {
        if (!item) return;
        if (item->type == ProjectItem::Type::Sound) {
            const uint32_t sourceId = SoundSourceEditing::activeSourceId(
                *item, ProjectPlatformId{}, {});
            if (sourceId != 0)
                ids.push_back(sourceId);
        }
        for (const auto& child : item->children)
            collectSourceIDs(child.get(), ids);
    };

    for (const auto* binding : project.allFileBindings()) {
        if (!binding || !binding->rootItem) continue;
        const auto* wu = binding;
        std::function<void(const ProjectItem*)> scanActions = [&](const ProjectItem* item) {
            if (!item) return;
            auto* actionComp = item->comp<VAction>();
            if (item->type == ProjectItem::Type::Action && actionComp &&
                !actionComp->steps.empty())
            {
                ActionPeakInfo info;
                info.actionName = item->name;

                for (const auto& action : actionComp->steps) {
                    if (action.type != VActionStep::Type::Play &&
                        action.type != VActionStep::Type::PostAction)
                        continue;

                    auto it = objectLookup.find(action.targetObjectID);
                    if (it != objectLookup.end())
                        collectSourceIDs(it.value(), info.sourceIDs);
                }

                if (!info.sourceIDs.empty())
                    actionPeaks.push_back(std::move(info));
            }
            for (const auto& child : item->children)
                scanActions(child.get());
        };
        scanActions(wu->rootItem.get());
    }

    QDir dir(outputDir);
    if (!dir.exists())
        dir.mkpath(".");

    for (const auto& ep : actionPeaks) {
        bool written = false;
        for (uint32_t sourceID : ep.sourceIDs) {
            QString audioPath = ResolveAudioFilePath(project, sourceID, AudioSourcesDir);
            if (audioPath.isEmpty() || !QFileInfo::exists(audioPath))
                continue;

            WaveformPeakData peakData;

            if (!WaveformCache::load(audioPath, peakData)) {
                if (!WaveformCache::generate(audioPath)) {
                    qDebug() << "[PackExporter] HarkenPeak: failed to generate .vkd for"
                             << audioPath;
                    continue;
                }
                if (!WaveformCache::load(audioPath, peakData)) {
                    qDebug() << "[PackExporter] HarkenPeak: failed to load .vkd after generate for"
                             << audioPath;
                    continue;
                }
            }

            if (!peakData.isValid())
                continue;

            QString peakPath = dir.filePath(ep.actionName + ".harkenpeak");
            if (WriteHarkenPeakFile(peakPath, peakData)) {
                ++filesWritten;
                written = true;
                break;
            }
        }

        if (!written) {
            qDebug() << "[PackExporter] HarkenPeak: no valid waveform data for action"
                     << ep.actionName;
        }
    }

    return filesWritten;
}

}
