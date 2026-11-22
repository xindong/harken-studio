#include "ConversionResults.h"

#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/BuildSourceInputs.h"
#include "AssetPipeline/MediaBuildSnapshot.h"
#include "AssetPipeline/PathUtils.h"
#include "FieldGroupProjectAdapter.h"
#include "ReferenceSlotResolution.h"
#include "SoundSourceSemantics.h"
#include "Tooling/Harkenizer/Core/PackMediaPreparation.h"
#include "Tooling/Harkenizer/Core/SoundSourceDerivative.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <algorithm>
#include <atomic>
#include <exception>

namespace harken::studio::ConversionResults {
namespace {

QString cacheDirectory(const ProjectData& project)
{
    return QDir(PathUtils::getDotCacheDir(QFileInfo(project.projectPath).absolutePath()))
        .filePath(QStringLiteral("pack/media"));
}

QVector<const ProjectItem*> projectItems(const ProjectData& project)
{
    QVector<const ProjectItem*> items;
    QSet<const ProjectItem*> visited;
    const auto visit = [&](const auto& self, const ProjectItem* item) -> void {
        if (!item || visited.contains(item)) return;
        visited.insert(item);
        items.push_back(item);
        for (const auto& child : item->children) self(self, child.get());
    };
    visit(visit, project.root.get());
    for (const auto* binding : project.allFileBindings()) visit(visit, binding->rootItem.get());
    return items;
}

struct SelectedSource
{
    const ProjectItem* sound;
    const SoundSourceDef* source;
    QString language;
};

QVector<SelectedSource> selectedSources(const ProjectData& project, const QUuid& presetGuid,
    ProjectPlatformId platform, QString* error)
{
    if (error) error->clear();
    if (!ProjectPlatformSemantics::find(project.platforms, platform)) {
        if (error) *error = QStringLiteral("Conversion platform does not belong to the project");
        return {};
    }
    const auto items = projectItems(project);
    const auto preset = std::find_if(items.begin(), items.end(), [&](const auto* item) {
        return item->guid == presetGuid && item->type == ProjectItem::Type::Conversion;
    });
    if (preset == items.end()) {
        if (error) *error = QStringLiteral("Conversion preset is unavailable");
        return {};
    }
    QVector<SelectedSource> selected;
    const auto effective = std::make_shared<const FieldGroupEffectiveSnapshot>(
        buildFieldGroupEffectiveSnapshot(project, toFieldGroupGuid(platform)));
    for (const auto* item : items) {
        const auto* sound = item->comp<VSound>();
        if (!sound) continue;
        const auto resolved = ReferenceSlotResolution::resolveEffectiveConversion(
            ReferenceSlotResolution::buildResolutionScope(item, effective));
        if (!resolved.valid) {
            if (error) *error = resolved.error;
            return {};
        }
        if (!resolved.target || resolved.target->guid != presetGuid) continue;
        const auto languages = sound->distinguishLanguage ? project.projectLanguages : QStringList{QString()};
        for (const auto& language : languages) {
            const auto* sourceSet = SoundSourceSemantics::sourceSet(*sound, language);
            if (!sourceSet) continue;
            const auto platformKey = platform.toString(QUuid::WithoutBraces);
            const auto override = sourceSet->platforms.constFind(platformKey);
            const auto active = override == sourceSet->platforms.cend() ? sourceSet->active : override->active;
            if (active.isNull()) continue;
            const auto selection = SoundSourceSemantics::resolveSourceSelection(
                *sound, platformKey, language);
            if (!selection) {
                if (!selection.error.isEmpty()) {
                    if (error) *error = selection.error;
                    return {};
                }
                continue;
            }
            selected.push_back({item, selection.source, language});
        }
    }
    return selected;
}

}

FileProperties FileProperties::fromSource(const AssetInfo& asset)
{
    const auto& metadata = asset.metadata;
    return {asset.absolutePath, metadata.codec, metadata.sampleRate, metadata.channels,
        metadata.totalSamples, metadata.fileSize};
}

QJsonObject FileProperties::toJson() const
{
    return {{QStringLiteral("path"), path}, {QStringLiteral("format"), format},
        {QStringLiteral("sample_rate"), static_cast<qint64>(sampleRate)},
        {QStringLiteral("channels"), channels}, {QStringLiteral("frames"), static_cast<qint64>(frames)},
        {QStringLiteral("bytes"), static_cast<qint64>(bytes)},
        {QStringLiteral("duration_seconds"), sampleRate ? static_cast<double>(frames) / sampleRate : 0.0}};
}

QJsonObject SourceResult::toJson() const
{
    QJsonObject result{{QStringLiteral("sound"), soundGuid.toString(QUuid::WithoutBraces)},
        {QStringLiteral("sound_name"), soundName}, {QStringLiteral("source_id"), static_cast<qint64>(source.sourceId)},
        {QStringLiteral("source_entry"), source.entryId.toString(QUuid::WithoutBraces)},
        {QStringLiteral("source_path"), sourcePath}, {QStringLiteral("language"), language},
        {QStringLiteral("conversion"), presetGuid.isNull() ? QJsonValue(QJsonValue::Null)
            : QJsonValue(presetGuid.toString(QUuid::WithoutBraces))},
        {QStringLiteral("original"), original.toJson()},
        {QStringLiteral("converted"), converted ? QJsonValue(converted->toJson()) : QJsonValue(QJsonValue::Null)}};
    if (converted && original.bytes != 0)
        result.insert(QStringLiteral("compression_ratio"), static_cast<double>(converted->bytes) / original.bytes);
    return result;
}

std::optional<SourceResult> inspectSource(const ProjectData& project,
    const ProjectItem& sound, const SoundSourceDef& source, const AssetInfo& asset,
    ProjectPlatformId platform, const QString& language, double scheduleOffsetSec, QString* error)
{
    QString resolutionError;
    const auto conversion = resolveConversion(project, sound, platform, asset, &resolutionError);
    if (!resolutionError.isEmpty()) {
        if (error) *error = resolutionError;
        return std::nullopt;
    }
    if (error) error->clear();
    SourceResult result;
    result.soundGuid = sound.guid;
    result.source = source;
    result.soundName = sound.name;
    result.sourcePath = asset.relativePath;
    result.language = language;
    result.original = FileProperties::fromSource(asset);
    if (!conversion) return result;
    result.presetGuid = conversion->presetGuid;
    const auto key = SoundSourceDerivative::cacheKey(asset.contentHash,
        SoundSourceSemantics::authoredView(source, &asset), scheduleOffsetSec, conversion, asset.metadata);
    const auto artifact = SoundSourceDerivative::findCachedArtifact(key, cacheDirectory(project), conversion->format);
    if (artifact) {
        const auto frames = artifact->encodedMedia ? artifact->encodedMedia->frameCount
            : artifact->samples->size() / artifact->channels;
        result.converted = FileProperties{artifact->path,
            artifact->format == ConversionFormat::Vorbis ? QStringLiteral("Vorbis") : QStringLiteral("PCM"),
            artifact->sampleRate, artifact->channels, frames, artifact->outputBytes};
    }
    return result;
}

QVector<SourceResult> query(const ProjectData& project, const QUuid& presetGuid,
    ProjectPlatformId platform, QString* error)
{
    const auto sources = selectedSources(project, presetGuid, platform, error);
    QVector<SourceResult> results;
    for (const auto& selected : sources) {
        const auto* asset = AssetRegistry::instance().getAsset(selected.source->sourceId);
        if (!asset) {
            if (error) *error = QStringLiteral("Conversion source %1 is unavailable").arg(selected.source->sourceId);
            return {};
        }
        const auto row = inspectSource(project, *selected.sound, *selected.source, *asset, platform,
            selected.language, selected.sound->comp<VSound>()->scheduleOffsetSec, error);
        if (!row) return {};
        results.push_back(*row);
    }
    return results;
}

bool hasCachedSource(const ProjectData& project, const ProjectItem& sound,
    const SoundSourceDef& source, const AssetInfo& asset, ProjectPlatformId platform,
    double scheduleOffsetSec)
{
    const auto conversion = resolveConversion(project, sound, platform, asset);
    if (!conversion) return false;
    const auto key = SoundSourceDerivative::cacheKey(asset.contentHash,
        SoundSourceSemantics::authoredView(source, &asset), scheduleOffsetSec, conversion, asset.metadata);
    if (key.isEmpty()) return false;
    return QFileInfo::exists(QDir(cacheDirectory(project)).filePath(key
        + (conversion->format == ConversionFormat::Vorbis ? QStringLiteral(".vcm") : QStringLiteral(".pcm"))));
}

QJsonArray toJson(const QVector<SourceResult>& results)
{
    QJsonArray rows;
    for (const auto& result : results) rows.append(result.toJson());
    return rows;
}

QJsonObject BatchResult::toJson() const
{
    return {{QStringLiteral("canceled"), canceled}, {QStringLiteral("converted"), converted},
        {QStringLiteral("cached"), cached}, {QStringLiteral("errors"), QJsonArray::fromStringList(errors)}};
}

struct Batch::Impl
{
    struct Work
    {
        harken::studio::types::ObjectId soundId;
        SoundSourceDef source;
        double scheduleOffsetSec;
    };

    MediaBuildSnapshot snapshot;
    QVector<Work> work;
    ProjectPlatformId platform;
    QString directory;
    std::atomic<bool> canceled{false};
};

Batch::Batch() : impl_(std::make_unique<Impl>()) {}
Batch::~Batch() = default;

std::shared_ptr<Batch> Batch::create(const ProjectData& project,
    const QUuid& presetGuid, ProjectPlatformId platform, QString* error)
{
    QString selectionError;
    const auto sources = selectedSources(project, presetGuid, platform, &selectionError);
    if (!selectionError.isEmpty()) {
        if (error) *error = selectionError;
        return {};
    }
    auto batch = std::shared_ptr<Batch>(new Batch);
    auto& state = *batch->impl_;
    state.platform = platform;
    state.directory = cacheDirectory(project);
    const auto projectRoot = QFileInfo(project.projectPath).absolutePath();
    state.snapshot = AssetRegistry::instance().buildSnapshot(projectRoot, project.platforms, false);
    if (!state.snapshot.captureConversions(project, error)) return {};
    state.snapshot.sourceInputs = std::make_shared<BuildSourceInputs>(projectRoot, state.snapshot.assets);
    for (const auto& selected : sources)
        state.work.push_back({selected.sound->runtimeId(), *selected.source,
            selected.sound->comp<VSound>()->scheduleOffsetSec});
    return batch;
}

BatchResult Batch::run(const std::function<bool()>& cancelCheck)
{
    BatchResult result;
    const auto canceled = [&] { return impl_->canceled.load(std::memory_order_acquire)
        || (cancelCheck && cancelCheck()); };
    auto cache = std::make_shared<SoundSourceDerivative::MaterializationCache>();
    PackMediaPreparation preparation(impl_->snapshot, cache, impl_->platform, impl_->directory);
    for (const auto& work : impl_->work) {
        if (canceled()) { result.canceled = true; return result; }
        preparation.add(work.soundId, work.source, work.scheduleOffsetSec);
    }
    preparation.start();
    for (const auto& work : impl_->work) {
        QString failure;
        try {
            const auto prepared = preparation.take(work.soundId, work.source.entryId, canceled);
            if (canceled()) { result.canceled = true; break; }
            failure = prepared.error;
            if (failure.isEmpty() && prepared.artifact) {
                if (prepared.artifact->cacheHit) ++result.cached;
                else ++result.converted;
            }
        } catch (const std::exception& exception) {
            failure = QString::fromUtf8(exception.what());
        } catch (...) {
            failure = QStringLiteral("Source conversion failed");
        }
        if (!failure.isEmpty()) {
            result.errors.push_back(failure);
            qWarning().noquote() << failure;
        }
    }
    return result;
}

void Batch::cancel()
{
    impl_->canceled.store(true, std::memory_order_release);
}

}
