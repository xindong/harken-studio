#include "Tooling/Harkenizer/Core/PackBuildState.h"

#include "AssetPipeline/BuildSourceInputs.h"
#include "AssetPipeline/SourceFileState.h"
#include "AssetPipeline/PathUtils.h"
#include "AssetPipeline/PlatformProfile.h"
#include "ProjectSerializer.h"
#include "Components/VAction.h"
#include "Components/VEffect.h"
#include "Components/VMixNode.h"
#include "PlayablePropertyAuthoringV1.h"
#include "SoundSourceSemantics.h"
#include "Tooling/Harkenizer/Core/PackBuildPlan.h"
#include "Tooling/Harkenizer/Core/SoundSourceDerivative.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>

namespace VNX {
namespace {

using ExportResult = ProjectPackExporter::ExportResult;
using BuildRecord = ProjectPackExporter::PackBuildRecord;

QString digest(const QJsonObject& object)
{
    return QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(object).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}

QString companionPath(const QString& path)
{
    const QFileInfo file(path);
    return QDir::cleanPath(QDir(file.path()).filePath(file.completeBaseName() + QStringLiteral(".json")));
}

QString fileState(const QString& path)
{
    return QString::fromLatin1(packFileState(path).toHex());
}

bool matches(const QString& path, const QJsonValue& state)
{
    return !state.toString().isEmpty() && fileState(path) == state.toString();
}

}

PackBuildState::PackBuildState(std::shared_ptr<const ProjectBuildSnapshot> snapshot, const QByteArray& compiler)
    : snapshot_(std::move(snapshot)), compiler_(QString::fromLatin1(compiler.toHex()))
{
    cachePath_ = QDir(PathUtils::getDotCacheDir(snapshot_->media.projectRoot))
        .filePath(QStringLiteral("pack/builds.json"));
    QFile file(cachePath_);
    if (file.open(QIODevice::ReadOnly)) {
        const auto state = QJsonDocument::fromJson(file.readAll()).object();
        if (state.value(QStringLiteral("version")).toInt() == 2) {
            legacyCleanup_ = false;
            targets_ = state.value(QStringLiteral("targets")).toObject();
            waveforms_ = state.value(QStringLiteral("waveforms")).toObject();
        }
    }
    persisted_ = targets_;
    persistedWaveforms_ = waveforms_;
    for (const auto& path : targets_.keys()) {
        const auto platform = targets_.value(path).toObject().value(QStringLiteral("platform")).toString();
        if (!ProjectPlatformSemantics::find(snapshot_->project.platforms, QUuid(platform))) targets_.remove(path);
    }
}

void PackBuildState::select(const ProjectPackExporter::ExportOptions& options, const QString& outputDirectory)
{
    options_ = options;
    options_.packBuildState.reset();
    outputDirectory_ = QDir(outputDirectory).absolutePath();
    packs_ = targets_.value(outputDirectory_).toObject().value(QStringLiteral("packs")).toObject();
    inputs_.clear();
    packMatches_.clear();
    companionMatches_.clear();
    current_.clear();
    resources_.clear();
    records_.clear();
    objectCount_ = mediaCount_ = actionCount_ = emptyActionCount_ = 0;
    inputsReady_ = false;
    reusable_ = false;
    if (!options.forceReexport && !packs_.isEmpty()) deriveInputs();
}

void PackBuildState::deriveInputs(const QSet<QString>* successfulPaths)
{
    const auto& options = options_;
    reusable_ = !compiler_.isEmpty() && options.currentWorldOutputPath.isEmpty()
        && !options.resourceReuse && options.buildPlan && options.buildContext;
    if (!reusable_) return;
    reusable_ = options.buildPlan->diagnostics.empty();
    const auto& project = snapshot_->project;
    const auto* platform = ProjectPlatformSemantics::find(project.platforms, QUuid(options.platform));
    if (!platform) { reusable_ = false; return; }
    ProjectSerializer serializer;
    if (!serializer.prepareReferenceWrite(project)) { reusable_ = false; return; }
    const auto itemDigest = [&](const ProjectItem* item) {
        const auto cached = objectDigests_.constFind(item);
        if (cached != objectDigests_.cend()) return *cached;
        auto json = serializer.projectItemToJson(*item, false);
        if (json.isEmpty()) return QString();
        if (const auto* mix = item->comp<VMixNode>()) {
            QJsonObject effects;
            bool valid = true;
            const auto addEffects = [&](const EffectChainLedger& chain) {
                for (const auto& entry : chain.entries()) {
                    if (entry.mode == EffectMode::Custom) continue;
                    const auto* effect = EffectChainSemantics::resolve(entry, project);
                    if (!effect) { valid = false; continue; }
                    effects.insert(entry.effectGuid.toString(QUuid::WithoutBraces), effect->toJson());
                }
            };
            if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item->type)) {
                VisitAuthoredEffectChains(item->fieldGroups, [&](const auto& chain, auto) {
                    addEffects(chain);
                });
            } else {
                addEffects(mix->effects);
                for (const auto& [platform, chain] : mix->effectPlatforms)
                    addEffects(chain);
            }
            if (!valid) return QString();
            if (!effects.isEmpty()) json.insert(QStringLiteral("effectAssets"), effects);
        }
        json.remove(QStringLiteral("notes"));
        json.remove(QStringLiteral("color"));
        QJsonArray children;
        for (const auto& child : item->children)
            if (child) children.append(child->guid.toString(QUuid::WithoutBraces));
        json.insert(QStringLiteral("children"), children);
        const auto fingerprint = digest(json);
        objectDigests_.insert(item, fingerprint);
        return fingerprint;
    };
    QJsonObject globals;
    QSet<const ProjectItem*> visited;
    QSet<harkenstudio::ObjectId> objects, actions, emptyActions;
    QSet<QString> media;
    const auto visit = [&](const auto& self, const ProjectItem* item) -> void {
        if (!item || visited.contains(item)) return;
        visited.insert(item);
        if (item->type == ProjectItem::Type::Bus || item->type == ProjectItem::Type::Variable
            || item->type == ProjectItem::Type::Attenuation
            || item->type == ProjectItem::Type::Effect) {
            const auto fingerprint = itemDigest(item);
            if (fingerprint.isEmpty()) reusable_ = false;
            globals.insert(objectIdString(item->runtimeId()), fingerprint);
            objects.insert(item->runtimeId());
        }
        for (const auto& child : item->children) self(self, child.get());
    };
    if (!successfulPaths) {
        visit(visit, project.root.get());
        for (const auto* binding : project.allFileBindings()) visit(visit, binding->rootItem.get());
    }
    QJsonObject common{
        {QStringLiteral("compiler"), compiler_},
        {QStringLiteral("project"), project.projectId},
        {QStringLiteral("platform"), platform->toJson()},
        {QStringLiteral("sampleRate"), project.sampleRate},
        {QStringLiteral("channels"), static_cast<int>(project.channelConfig)},
        {QStringLiteral("packMetadata"), project.buildExportPackMetadata},
        {QStringLiteral("waveforms"), project.buildExportWaveforms}};
    if (project.buildExportWaveforms)
        common.insert(QStringLiteral("waveformDirectory"), options.waveformOutputDir.isEmpty()
            ? project.buildWaveformOutputDir : options.waveformOutputDir);
    const auto commonDigest = digest(common);
    QJsonObject unitInputs;
    BuildRecord init;
    init.packName = QStringLiteral("Init");
    init.unitKey = QStringLiteral("Init");
    init.source = BuildRecord::Source::Init;
    init.outputPath = QDir(outputDirectory_).filePath(QStringLiteral("Init.bytes"));
    records_.push_back(init);
    for (const auto& unit : options.buildPlan->units) {
        const auto path = PackBuildPlanning::packFileName(unit.variant.language, unit.name, QStringLiteral(".bytes"));
        if (successfulPaths && !successfulPaths->contains(path)) continue;
        if (!successfulPaths && options.cancelCheck && options.cancelCheck()) { reusable_ = false; return; }
        QJsonObject definitions, sources;
        bool valid = true;
        bool hasSteps = false;
        uint32_t objectId = 0;
        const auto addMember = [&](const ActionClosureMember& member) {
            const auto* item = options.buildContext->closures->findObject(member.objectId);
            if (!item) { valid = false; return; }
            if (item->type == ProjectItem::Type::Action) {
                actions.insert(member.stableId);
                const auto* action = item->comp<VAction>();
                if (action && !action->steps.empty()) hasSteps = true;
                else emptyActions.insert(member.stableId);
                if (!unit.grouped && member.stableId == unit.actionStableIds.front()) objectId = item->objectID;
            } else {
                objects.insert(member.stableId);
            }
            for (; item && item->type != ProjectItem::Type::None; item = item->parent) {
                if (item->type != ProjectItem::Type::Action) objects.insert(item->runtimeId());
                const auto fingerprint = itemDigest(item);
                valid = valid && !fingerprint.isEmpty();
                definitions.insert(objectIdString(item->runtimeId()), fingerprint);
            }
        };
        for (const auto& variant : options.buildPlan->variants) {
            if (variant.variant != unit.variant
                || std::find(unit.actionStableIds.begin(), unit.actionStableIds.end(), variant.actionStableId)
                    == unit.actionStableIds.end()) continue;
            valid = valid && variant.closure.ready() && variant.capability.permitted();
            for (const auto* members : {&variant.closure.actions, &variant.closure.objects, &variant.closure.controlObjects})
                for (const auto& member : *members) addMember(member);
            for (const auto* entries : {&variant.closure.media, &variant.closure.controlMedia}) {
                for (const auto& entry : *entries) {
                    const auto id = QString::number(entry.authoringSourceId);
                    media.insert(entry.artifactId);
                    if (sources.contains(id)) continue;
                    const auto input = snapshot_->media.inspectRequest(entry.authoringSourceId, platform->id);
                    if (!input) { valid = false; continue; }
                    sources.insert(id, input->fingerprint());
                }
            }
        }
        const auto key = unit.grouped ? QStringLiteral("g:") + unit.artifactKey
            : QStringLiteral("a:") + objectIdString(unit.actionStableIds.front());
        const QJsonObject identity{
            {QStringLiteral("common"), commonDigest}, {QStringLiteral("key"), key},
            {QStringLiteral("language"), unit.variant.language},
            {QStringLiteral("definitions"), definitions}, {QStringLiteral("sources"), sources}};
        const auto input = valid ? digest(identity) : QString();
        unitInputs.insert(path, input);
        if (hasSteps) inputs_.insert(path, input);
        BuildRecord record;
        record.packName = unit.name;
        record.objectID = objectId;
        record.source = unit.grouped ? BuildRecord::Source::Group : BuildRecord::Source::Action;
        record.outputPath = QDir(outputDirectory_).filePath(path);
        record.language = unit.variant.language;
        record.unitKey = key;
        record.omitted = !hasSteps;
        if (record.omitted)
            record.warnings.append(unit.grouped
                ? QStringLiteral("Pack group has no Actions with steps; nothing to pack")
                : QStringLiteral("Action has no steps; nothing to pack"));
        records_.push_back(std::move(record));
        reusable_ = reusable_ && valid;
    }
    if (!successfulPaths)
        inputs_.insert(QStringLiteral("Init.bytes"), digest(QJsonObject{
            {QStringLiteral("common"), commonDigest}, {QStringLiteral("globals"), globals},
            {QStringLiteral("units"), unitInputs}, {QStringLiteral("partial"), options.updateSelectedOnly},
            {QStringLiteral("language"), options.language},
            {QStringLiteral("defaultLanguage"), project.defaultLanguage}}));
    for (auto& record : records_) {
        record.platform = options.platform;
        record.platformName = platform->name;
        record.basePlatform = platform->basePlatform;
        record.platformDefinition = platform->toJson();
    }
    objectCount_ = objects.size();
    mediaCount_ = media.size();
    actionCount_ = actions.size();
    emptyActionCount_ = emptyActions.size();
    inputsReady_ = true;
    reusable_ = reusable_ && !successfulPaths;
}

bool PackBuildState::savedPackMatches(const QString& path) const
{
    const auto found = packMatches_.constFind(path);
    if (found != packMatches_.cend()) return *found;
    const auto current = matches(QDir(outputDirectory_).filePath(path),
        packs_.value(path).toObject().value(QStringLiteral("state")));
    packMatches_.insert(path, current);
    return current;
}

bool PackBuildState::savedCompanionMatches(const QString& path) const
{
    const auto found = companionMatches_.constFind(path);
    if (found != companionMatches_.cend()) return *found;
    const auto current = matches(QDir(outputDirectory_).filePath(companionPath(path)),
        packs_.value(path).toObject().value(QStringLiteral("companionState")));
    companionMatches_.insert(path, current);
    return current;
}

bool PackBuildState::isCurrent(const QString& path) const
{
    const auto found = current_.constFind(path);
    if (found != current_.cend()) return *found;
    const auto check = [&] {
        const auto input = inputs_.value(path);
        const auto row = packs_.value(path).toObject();
        if (input.isEmpty() || row.value(QStringLiteral("input")).toString() != input) return false;
        if (!savedPackMatches(path)
            || !savedCompanionMatches(path)) return false;
        for (const auto& waveform : row.value(QStringLiteral("waveforms")).toArray())
            if (!isWaveformCurrent(path, waveform.toString())) return false;
        return true;
    };
    const auto current = check();
    current_.insert(path, current);
    return current;
}

bool PackBuildState::isWaveformCurrent(const QString& packPath, const QString& waveformPath) const
{
    const auto path = QDir::cleanPath(QFileInfo(QDir(outputDirectory_).filePath(packPath))
        .dir().filePath(waveformPath));
    const auto expected = waveforms_.value(path).toString();
    if (expected.isEmpty()) return false;
    auto found = waveformStates_.constFind(path);
    if (found == waveformStates_.cend()) {
        const auto current = fileState(path);
        found = waveformStates_.insert(path, current == expected ? current : QString());
    }
    return *found == expected;
}

void PackBuildState::recordWaveformOutput(const QString& absolutePath)
{
    const auto path = QDir::cleanPath(absolutePath);
    const auto state = fileState(path);
    waveforms_.insert(path, state);
    waveformStates_.insert(path, state);
}

void PackBuildState::applyCounts(ExportResult& result) const
{
    if (!inputsReady_) return;
    result.objectCount = objectCount_;
    result.mediaCount = mediaCount_;
    result.actionCount = actionCount_;
}

bool PackBuildState::restore(ExportResult* result, std::vector<BuildRecord>* records) const
{
    if (!inputsReady_ || !reusable_ || !result || !records || (options_.cancelCheck && options_.cancelCheck())) return false;
    for (auto it = inputs_.cbegin(); it != inputs_.cend(); ++it)
        if (!isCurrent(it.key())) return false;
    ExportResult restored;
    auto restoredRecords = records_;
    restored.outputPath = outputDirectory_;
    applyCounts(restored);
    for (auto& record : restoredRecords) {
        record.success = true;
        record.skipped = true;
        record.elapsedMs = 0;
        if (record.omitted) continue;
        const QFileInfo file(record.outputPath);
        record.fileSize = file.size();
        PackArtifactMetadata artifact;
        artifact.unitKey = record.unitKey;
        artifact.platform = record.platform;
        artifact.language = record.language;
        artifact.packName = record.packName;
        artifact.outputPath = record.outputPath;
        artifact.bytes = record.fileSize;
        artifact.version = HARKEN_PACK_SCHEMA_VERSION;
        artifact.modified = file.lastModified();
        restored.artifacts.append(artifact);
        restored.fileSize += record.fileSize;
        if (!options_.updateSelectedOnly || record.source != BuildRecord::Source::Init)
            ++restored.skippedPackCount;
        const auto path = QDir(outputDirectory_).relativeFilePath(record.outputPath);
        const auto row = packs_.value(path).toObject();
        QJsonArray waveforms;
        for (const auto& path : row.value(QStringLiteral("waveforms")).toArray())
            waveforms.append(QJsonObject{{QStringLiteral("file"), path}});
        restored.outputs.append(QJsonObject{{QStringLiteral("path"), path},
            {QStringLiteral("key"), record.unitKey},
            {QStringLiteral("companion"), QJsonObject{{QStringLiteral("waveforms"), waveforms}}}});
    }
    if (emptyActionCount_ != 0)
        restored.warnings.append(QStringLiteral("%1 action(s) have no steps and were not packed")
            .arg(emptyActionCount_));
    if (options_.updateSelectedOnly)
        std::erase_if(restoredRecords, [](const auto& record) { return record.source == BuildRecord::Source::Init; });
    restored.success = true;
    *result = std::move(restored);
    *records = std::move(restoredRecords);
    return true;
}

QString PackBuildState::resourceFingerprint(const QString& path) const
{
    const auto row = packs_.value(path).toObject();
    return savedPackMatches(path)
        && savedCompanionMatches(path)
        ? row.value(QStringLiteral("resourceFingerprint")).toString() : QString();
}

void PackBuildState::setResourceFingerprint(const QString& path, const QString& fingerprint)
{
    resources_.insert(path, fingerprint);
}

void PackBuildState::save(const ExportResult& result, const std::vector<BuildRecord>& records)
{
    if (!inputsReady_ && !result.canceled && !result.outputs.isEmpty()) {
        QSet<QString> successfulPaths;
        if (!result.success)
            for (const auto& output : result.outputs)
                successfulPaths.insert(output.toObject().value(QStringLiteral("path")).toString());
        deriveInputs(result.success ? nullptr : &successfulPaths);
    }
    const QDir directory(outputDirectory_);
    for (const auto& value : result.outputs) {
        const auto output = value.toObject();
        const auto path = output.value(QStringLiteral("path")).toString();
        const auto previous = packs_.value(path).toObject();
        QJsonObject row;
        row.insert(QStringLiteral("key"), output.value(QStringLiteral("key")));
        row.insert(QStringLiteral("state"), fileState(directory.filePath(path)));
        const bool unchanged = previous.value(QStringLiteral("state")) == row.value(QStringLiteral("state"));
        row.insert(QStringLiteral("input"), path == QStringLiteral("Init.bytes") && !result.success
            ? QString() : inputs_.value(path, inputsReady_ || unchanged
                ? previous.value(QStringLiteral("input")).toString() : QString()));
        row.insert(QStringLiteral("companionState"), fileState(directory.filePath(companionPath(path))));
        const auto fingerprint = resources_.value(path,
            unchanged
                ? previous.value(QStringLiteral("resourceFingerprint")).toString() : QString());
        if (!fingerprint.isEmpty()) row.insert(QStringLiteral("resourceFingerprint"), fingerprint);
        QJsonArray waveforms;
        for (const auto& value : output.value(QStringLiteral("companion")).toObject().value(QStringLiteral("waveforms")).toArray()) {
            const auto file = value.toObject().value(QStringLiteral("file")).toString();
            if (!waveforms.contains(file)) waveforms.append(file);
        }
        if (!waveforms.isEmpty()) row.insert(QStringLiteral("waveforms"), waveforms);
        packs_.insert(path, row);
    }
    for (const auto& record : records)
        if (!record.success || record.omitted) packs_.remove(directory.relativeFilePath(record.outputPath));
    targets_.insert(outputDirectory_, QJsonObject{
        {QStringLiteral("platform"), options_.platform}, {QStringLiteral("packs"), packs_}});
}

void PackBuildState::retainOutputs(const QString& outputRoot, const QMap<QString, QJsonArray>& outputs)
{
    const QDir root(outputRoot);
    for (const auto& target : targets_.keys()) {
        const auto relative = root.relativeFilePath(target);
        if (!relative.startsWith(QStringLiteral("../")) && !QDir::isAbsolutePath(relative)
            && !outputs.contains(target)) targets_.remove(target);
    }
    for (auto target = outputs.cbegin(); target != outputs.cend(); ++target) {
        auto state = targets_.value(target.key()).toObject();
        auto packs = state.value(QStringLiteral("packs")).toObject();
        QSet<QString> paths;
        for (const auto& output : target.value()) paths.insert(output.toObject().value(QStringLiteral("path")).toString());
        for (const auto& path : packs.keys())
            if (!paths.contains(path)) packs.remove(path);
        state.insert(QStringLiteral("packs"), packs);
        targets_.insert(target.key(), state);
    }
}

bool PackBuildState::flush()
{
    if (targets_ == persisted_ && waveforms_ == persistedWaveforms_) return true;
    for (const auto& path : targets_.keys())
        if (!QDir(path).exists()) targets_.remove(path);
    if (!waveforms_.isEmpty()) {
        QSet<QString> referenced;
        for (auto target = targets_.begin(); target != targets_.end(); ++target) {
            const auto packs = target.value().toObject().value(QStringLiteral("packs")).toObject();
            for (auto pack = packs.begin(); pack != packs.end(); ++pack)
                for (const auto& file : pack.value().toObject().value(QStringLiteral("waveforms")).toArray())
                    referenced.insert(QDir::cleanPath(QFileInfo(QDir(target.key()).filePath(pack.key()))
                        .dir().filePath(file.toString())));
        }
        for (const auto& path : waveforms_.keys())
            if (!referenced.contains(path)) waveforms_.remove(path);
    }
    if (!QDir().mkpath(QFileInfo(cachePath_).absolutePath())) return false;
    QJsonObject state{{QStringLiteral("version"), 2}, {QStringLiteral("targets"), targets_}};
    if (!waveforms_.isEmpty()) state.insert(QStringLiteral("waveforms"), waveforms_);
    const auto bytes = QJsonDocument(state).toJson(QJsonDocument::Compact);
    QSaveFile file(cachePath_);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) return false;
    persisted_ = targets_;
    persistedWaveforms_ = waveforms_;
    return true;
}

void PackBuildState::prune() const
{
    const auto& snapshot = *snapshot_;
    const auto assets = snapshot.media.sourceInputs->assets();
    QSet<QString> keys;
    QSet<const ProjectItem*> visited;
    const auto visit = [&](const auto& self, const ProjectItem* item) -> void {
        if (!item || visited.contains(item)) return;
        visited.insert(item);
        if (const auto* sound = item->comp<VSound>()) {
            QStringList languages = sound->distinguishLanguage ? snapshot.project.projectLanguages : QStringList{QString()};
            for (const auto& platform : snapshot.project.platforms) {
                for (const auto& language : languages) {
                    const auto selected = SoundSourceSemantics::resolveSourceSelection(
                        *sound, ProjectPlatformSemantics::idString(platform.id), language);
                    if (!selected) continue;
                    const auto asset = assets.constFind(selected.source->sourceId);
                    if (asset == assets.cend() || asset->contentHash.isEmpty()) continue;
                    const auto override = snapshot.media.fileOverrides.constFind(selected.source->sourceId);
                    const auto settings = PlatformProfileManager::effectiveSettingsForAsset(
                        *asset, platform, override == snapshot.media.fileOverrides.cend() ? nullptr : &*override);
                    const auto source = SoundSourceSemantics::authoredView(*selected.source, &*asset);
                    keys.insert(SoundSourceDerivative::cacheKey(asset->contentHash, source,
                        sound->scheduleOffsetSec, settings, asset->metadata));
                }
            }
        }
        for (const auto& child : item->children) self(self, child.get());
    };
    visit(visit, snapshot.project.root.get());
    for (const auto* binding : snapshot.project.allFileBindings()) visit(visit, binding->rootItem.get());
    const QDir cache(PathUtils::getDotCacheDir(snapshot.media.projectRoot));
    const QDir conversions(cache.filePath(QStringLiteral("pack/media")));
    for (const auto& file : conversions.entryInfoList(
            {QStringLiteral("*.pcm"), QStringLiteral("*.pcm.??????")}, QDir::Files))
        if ((file.suffix() != QStringLiteral("pcm") || !keys.contains(file.completeBaseName()))
            && !QFile::remove(file.absoluteFilePath()))
            qWarning().noquote() << QStringLiteral("Could not remove unused Pack media: %1").arg(file.absoluteFilePath());
    if (!legacyCleanup_) return;
    for (const auto& legacy : {QStringLiteral("pack/targets"), QStringLiteral("pack-build"),
            QStringLiteral("pack-inputs"), QStringLiteral("media/pack-derivatives")}) {
        const auto path = cache.filePath(legacy);
        const QFileInfo info(path);
        if (info.isDir() && !info.isSymLink() && !QDir(path).removeRecursively())
            qWarning().noquote() << QStringLiteral("Could not remove obsolete Pack cache: %1").arg(path);
    }
    for (const auto& legacy : {QStringLiteral("pack/sources.json"), QStringLiteral("pack-artifacts.json")}) {
        QFile file(cache.filePath(legacy));
        if (file.exists() && !file.remove())
            qWarning().noquote() << QStringLiteral("Could not remove obsolete Pack cache: %1").arg(file.fileName());
    }
    QDirIterator waveforms(PathUtils::getWaveformCacheDir(snapshot.media.projectRoot),
        {QStringLiteral("*.vkd.source.json")}, QDir::Files, QDirIterator::Subdirectories);
    while (waveforms.hasNext()) {
        const auto path = waveforms.next();
        if (!QFile::remove(path))
            qWarning().noquote() << QStringLiteral("Could not remove obsolete waveform state: %1").arg(path);
    }
}

}
