#include "Tooling/Harkenizer/Core/PackBuildState.h"

#include "AssetPipeline/BuildSourceInputs.h"
#include "AssetPipeline/SourceContentCache.h"
#include "AssetPipeline/PathUtils.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>

namespace VNX {
namespace {

using ExportResult = ProjectPackExporter::ExportResult;
using BuildRecord = ProjectPackExporter::PackBuildRecord;

QString digest(const QByteArray& bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QString digest(const QJsonObject& object)
{
    return digest(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

QJsonArray sortedStrings(const QSet<QString>& values)
{
    QStringList strings = values.values();
    strings.sort();
    return QJsonArray::fromStringList(strings);
}

QStringList stringList(const QJsonValue& value)
{
    QStringList strings;
    for (const auto& entry : value.toArray()) strings.append(entry.toString());
    return strings;
}

QJsonObject readState(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto state = QJsonDocument::fromJson(file.readAll()).object();
    return state.value(QStringLiteral("version")).toInt() == 1 ? state : QJsonObject{};
}

QJsonObject requestIdentity(const ProjectPackExporter::ExportOptions& options)
{
    QJsonArray actions;
    for (const auto& action : options.selection.actionTargeting) actions.append(action.toJson());
    QJsonArray onlyActions;
    for (const auto& id : options.onlyActionStableIds) onlyActions.append(objectIdString(id));
    return {
        {QStringLiteral("language"), options.language},
        {QStringLiteral("debugStrings"), options.debugStrings},
        {QStringLiteral("updateSelectedOnly"), options.updateSelectedOnly},
        {QStringLiteral("includeUnchecked"), options.includeUnchecked},
        {QStringLiteral("waveformOutputDirectory"), options.waveformOutputDir},
        {QStringLiteral("actionSelection"), actions},
        {QStringLiteral("onlyUnits"), sortedStrings(options.onlyUnits)},
        {QStringLiteral("onlyPackPaths"), sortedStrings(options.onlyPackPaths)},
        {QStringLiteral("onlyActions"), onlyActions}};
}

QString companionPath(const QString& path)
{
    const QFileInfo file(path);
    return QDir::cleanPath(QDir(file.path()).filePath(file.completeBaseName() + QStringLiteral(".json")));
}

QString packLanguage(const QString& path)
{
    const auto directory = QFileInfo(path).path();
    return directory == QStringLiteral(".") ? QString() : directory;
}

QString fileState(const QString& path)
{
    if (QFileInfo(path).isSymLink()) return {};
    return QString::fromLatin1(packFileState(path).toHex());
}

QJsonObject sourceIdentity(const QMap<uint32_t, QString>& sources)
{
    QJsonObject identity;
    for (auto it = sources.cbegin(); it != sources.cend(); ++it)
        identity.insert(QString::number(it.key()), it.value());
    return identity;
}

}

std::shared_ptr<PackBuildState> PackBuildState::prepare(
    std::shared_ptr<const ProjectBuildSnapshot> snapshot,
    const ProjectPackExporter::ExportOptions& options,
    const QString& outputDirectory, const QByteArray& buildIdentity,
    std::optional<ProjectBuildIdentity>& sharedIdentity)
{
    if (!snapshot || !snapshot->media.sourceInputs || !snapshot->sourceContentCache
        || buildIdentity.isEmpty() || !options.currentWorldOutputPath.isEmpty()
        || options.resourceReuse || outputDirectory.isEmpty()
        || (options.cancelCheck && options.cancelCheck())) return {};
    const auto* platform = ProjectPlatformSemantics::find(snapshot->project.platforms, QUuid(options.platform));
    if (!platform) return {};
    const auto projectIdentity = snapshot->buildIdentity(options.platform, sharedIdentity, options.cancelCheck);
    if (!projectIdentity) return {};
    auto state = std::make_shared<PackBuildState>();
    state->snapshot_ = std::move(snapshot);
    state->canceled_ = options.cancelCheck;
    state->outputDirectory_ = QDir(outputDirectory).absolutePath();
    state->platform_ = *platform;
    state->platformLock_ = options.platformLocks.value(options.platform);
    state->objectGuids_ = projectIdentity->objectGuids;
    auto identity = requestIdentity(options);
    identity.insert(QStringLiteral("project"), projectIdentity->fingerprint);
    identity.insert(QStringLiteral("compiler"), QString::fromLatin1(buildIdentity.toHex()));
    state->fingerprint_ = digest(identity);
    state->cachePath_ = QDir(PathUtils::getDotCacheDir(state->snapshot_->media.projectRoot))
        .filePath(QStringLiteral("pack/targets/")
            + digest(state->outputDirectory_.toUtf8()) + QStringLiteral(".json"));
    state->saved_ = readState(state->cachePath_);
    if (state->saved_.value(QStringLiteral("directory")).toString() != state->outputDirectory_)
        state->saved_ = {};
    state->resources_ = state->saved_.value(QStringLiteral("packs")).toObject();
    return state;
}

QString PackBuildState::resourceFingerprint(const QString& path, const QString& hash) const
{
    const auto row = resources_.value(path).toObject();
    return row.value(QStringLiteral("contentHash")).toString() == hash
        ? row.value(QStringLiteral("resourceFingerprint")).toString() : QString();
}

void PackBuildState::setResourceFingerprint(const QString& path, const QString& hash, const QString& fingerprint)
{
    auto row = resources_.value(path).toObject();
    row.insert(QStringLiteral("contentHash"), hash);
    row.insert(QStringLiteral("resourceFingerprint"), fingerprint);
    resources_.insert(path, row);
}

void PackBuildState::setMediaArtifacts(const QString& path, QStringList artifacts)
{
    artifacts.removeDuplicates();
    artifacts.sort();
    auto row = resources_.value(path).toObject();
    row.insert(QStringLiteral("media"), QJsonArray::fromStringList(artifacts));
    resources_.insert(path, row);
}

bool PackBuildState::restore(ExportResult* result, std::vector<BuildRecord>* records) const
{
    if (!result || !records || (canceled_ && canceled_())
        || saved_.value(QStringLiteral("identity")).toString() != fingerprint_
        || !saved_.value(QStringLiteral("sources")).isArray()
        || !saved_.value(QStringLiteral("units")).isArray()) return false;
    BuildSourceInputs probes(snapshot_->sourceContentCache);
    if (!probes.adopt(*snapshot_->media.sourceInputs)) return false;
    QMap<uint32_t, QString> dependencies;
    for (const auto& value : saved_.value(QStringLiteral("sources")).toArray()) {
        if (canceled_ && canceled_()) return false;
        const auto id = static_cast<uint32_t>(value.toInteger());
        const auto asset = snapshot_->media.assets.constFind(id);
        if (asset == snapshot_->media.assets.cend()) return false;
        const auto hash = probes.sourceFingerprint(*asset);
        if (hash.isEmpty()) return false;
        dependencies.insert(id, hash);
    }
    if (digest(sourceIdentity(dependencies)) != saved_.value(QStringLiteral("sourceIdentity")).toString()) return false;
    const QDir directory(outputDirectory_);
    const auto packs = saved_.value(QStringLiteral("packs")).toObject();
    if (packs.isEmpty()) return false;
    PackPublicationProof publication;
    publication.directory = outputDirectory_;
    publication.projectId = snapshot_->project.projectId;
    publication.platformId = ProjectPlatformSemantics::idString(platform_.id);
    publication.outputLock = platformLock_;
    for (auto it = packs.begin(); it != packs.end(); ++it) {
        if (canceled_ && canceled_()) return false;
        auto row = it.value().toObject();
        const QString path = directory.filePath(it.key());
        const QString companion = companionPath(it.key());
        if (row.value(QStringLiteral("state")).toString().isEmpty()
            || fileState(path) != row.value(QStringLiteral("state")).toString()
            || row.value(QStringLiteral("companionState")).toString().isEmpty()
            || fileState(directory.filePath(companion)) != row.value(QStringLiteral("companionState")).toString()) return false;
        QJsonArray waveforms;
        const auto waveformStates = row.take(QStringLiteral("waveforms")).toObject();
        for (auto waveform = waveformStates.begin(); waveform != waveformStates.end(); ++waveform) {
            if (waveform.value().toString().isEmpty()
                || fileState(QFileInfo(path).dir().filePath(waveform.key())) != waveform.value().toString()) return false;
            waveforms.append(QJsonObject{{QStringLiteral("file"), waveform.key()}});
        }
        row.remove(QStringLiteral("state"));
        row.remove(QStringLiteral("companionState"));
        row.remove(QStringLiteral("resourceFingerprint"));
        row.remove(QStringLiteral("media"));
        row.insert(QStringLiteral("path"), it.key());
        row.insert(QStringLiteral("language"), packLanguage(it.key()));
        row.insert(QStringLiteral("projectId"), publication.projectId);
        row.insert(QStringLiteral("platformId"), publication.platformId);
        row.insert(QStringLiteral("companion"), QJsonObject{{QStringLiteral("waveforms"), waveforms}});
        publication.packs.append(row);
        publication.managedFiles.append(it.key());
        publication.managedFiles.append(companion);
    }
    QMap<QString, uint32_t> objectIds;
    for (auto it = objectGuids_.cbegin(); it != objectGuids_.cend(); ++it) objectIds.insert(it.value(), it.key());
    ExportResult restored;
    restored.outputPath = outputDirectory_;
    restored.warnings = stringList(saved_.value(QStringLiteral("warnings")));
    const auto counts = saved_.value(QStringLiteral("counts")).toObject();
    restored.objectCount = counts.value(QStringLiteral("objects")).toInt();
    restored.mediaCount = counts.value(QStringLiteral("media")).toInt();
    restored.actionCount = counts.value(QStringLiteral("actions")).toInt();
    std::vector<BuildRecord> restoredRecords;
    bool hasInit = false;
    for (const auto& value : saved_.value(QStringLiteral("units")).toArray()) {
        const auto unit = value.toObject();
        BuildRecord record;
        const QString path = unit.value(QStringLiteral("path")).toString();
        const auto pack = packs.value(path).toObject();
        record.omitted = unit.value(QStringLiteral("omitted")).toBool();
        if (!record.omitted && pack.isEmpty()) return false;
        record.packName = (record.omitted ? unit : pack).value(QStringLiteral("name")).toString();
        record.language = packLanguage(path);
        const auto guid = unit.value(QStringLiteral("object")).toString();
        if (!guid.isEmpty() && !objectIds.contains(guid)) return false;
        record.objectID = objectIds.value(guid);
        const int source = unit.value(QStringLiteral("source")).toInt(-1);
        if (source < static_cast<int>(BuildRecord::Source::Group)
            || source > static_cast<int>(BuildRecord::Source::Shared)) return false;
        record.source = static_cast<BuildRecord::Source>(source);
        record.outputPath = directory.filePath(path);
        const QFileInfo output(record.outputPath);
        record.fileSize = record.omitted ? 0 : output.size();
        record.platform = publication.platformId;
        record.platformName = platform_.name;
        record.basePlatform = platform_.basePlatform;
        record.platformDefinition = platform_.toJson();
        record.unitKey = unit.value(QStringLiteral("key")).toString();
        record.warnings = stringList(unit.value(QStringLiteral("warnings")));
        record.success = true;
        record.skipped = true;
        record.elapsedMs = 0;
        if (record.unitKey.isEmpty()) return false;
        if (!record.omitted) {
            PackArtifactMetadata artifact;
            artifact.unitKey = record.unitKey;
            artifact.platform = record.platform;
            artifact.language = record.language;
            artifact.packName = record.packName;
            artifact.outputPath = record.outputPath;
            artifact.bytes = record.fileSize;
            artifact.version = HARKEN_PACK_SCHEMA_VERSION;
            artifact.modified = output.lastModified();
            restored.artifacts.append(artifact);
            hasInit = hasInit || path == QStringLiteral("Init.bytes");
            restored.fileSize += record.fileSize;
            ++restored.skippedPackCount;
        }
        restoredRecords.push_back(std::move(record));
    }
    if (!hasInit && packs.contains(QStringLiteral("Init.bytes"))) {
        const QFileInfo output(directory.filePath(QStringLiteral("Init.bytes")));
        PackArtifactMetadata artifact;
        artifact.unitKey = QStringLiteral("Init");
        artifact.platform = publication.platformId;
        artifact.packName = QStringLiteral("Init");
        artifact.outputPath = output.filePath();
        artifact.bytes = output.size();
        artifact.version = HARKEN_PACK_SCHEMA_VERSION;
        artifact.modified = output.lastModified();
        restored.fileSize += artifact.bytes;
        restored.artifacts.prepend(artifact);
    }
    if (restored.artifacts.isEmpty() || !snapshot_->media.sourceInputs->adopt(probes)) return false;
    restored.publication = std::move(publication);
    restored.success = true;
    *result = std::move(restored);
    *records = std::move(restoredRecords);
    return true;
}

bool PackBuildState::save(const ExportResult& result, const std::vector<BuildRecord>& records,
                          const QMap<uint32_t, QString>& sourceDependencies)
{
    if (!result.success || result.canceled || !result.publication || records.empty()
        || (canceled_ && canceled_())) return false;
    const QDir directory(outputDirectory_);
    QJsonObject packs;
    for (const auto& value : result.publication->packs) {
        auto row = value.toObject();
        const QString path = row.take(QStringLiteral("path")).toString();
        const auto companion = row.take(QStringLiteral("companion")).toObject();
        row.remove(QStringLiteral("projectId"));
        row.remove(QStringLiteral("platformId"));
        row.remove(QStringLiteral("contentRevision"));
        row.remove(QStringLiteral("language"));
        row.insert(QStringLiteral("state"), fileState(directory.filePath(path)));
        row.insert(QStringLiteral("companionState"), fileState(directory.filePath(companionPath(path))));
        const auto fingerprint = resourceFingerprint(path, row.value(QStringLiteral("contentHash")).toString());
        if (!fingerprint.isEmpty()) row.insert(QStringLiteral("resourceFingerprint"), fingerprint);
        else row.remove(QStringLiteral("contentHash"));
        const auto media = resources_.value(path).toObject().value(QStringLiteral("media")).toArray();
        if (!media.isEmpty()) row.insert(QStringLiteral("media"), media);
        QJsonObject waveforms;
        for (const auto& value : companion.value(QStringLiteral("waveforms")).toArray()) {
            const QString file = value.toObject().value(QStringLiteral("file")).toString();
            if (file.isEmpty()) return false;
            waveforms.insert(file, fileState(QFileInfo(directory.filePath(path)).dir().filePath(file)));
        }
        if (!waveforms.isEmpty()) row.insert(QStringLiteral("waveforms"), waveforms);
        packs.insert(path, row);
    }
    QJsonArray units;
    for (const auto& record : records) {
        if (!record.success || (record.objectID && !objectGuids_.contains(record.objectID))) return false;
        QJsonObject unit{
            {QStringLiteral("key"), record.unitKey},
            {QStringLiteral("path"), directory.relativeFilePath(record.outputPath)},
            {QStringLiteral("source"), static_cast<int>(record.source)}};
        if (record.objectID) unit.insert(QStringLiteral("object"), objectGuids_.value(record.objectID));
        if (!record.warnings.isEmpty()) unit.insert(QStringLiteral("warnings"), QJsonArray::fromStringList(record.warnings));
        if (record.omitted) {
            unit.insert(QStringLiteral("omitted"), true);
            unit.insert(QStringLiteral("name"), record.packName);
        }
        units.append(unit);
    }
    QJsonArray sources;
    for (auto it = sourceDependencies.cbegin(); it != sourceDependencies.cend(); ++it)
        sources.append(static_cast<qint64>(it.key()));
    QJsonObject state{
        {QStringLiteral("version"), 1},
        {QStringLiteral("directory"), outputDirectory_},
        {QStringLiteral("platform"), ProjectPlatformSemantics::idString(platform_.id)},
        {QStringLiteral("identity"), result.reusableBuild && !result.mediaFallbacks ? fingerprint_ : QString()},
        {QStringLiteral("sources"), sources},
        {QStringLiteral("sourceIdentity"), digest(sourceIdentity(sourceDependencies))},
        {QStringLiteral("packs"), packs},
        {QStringLiteral("units"), units},
        {QStringLiteral("counts"), QJsonObject{
            {QStringLiteral("objects"), result.objectCount},
            {QStringLiteral("media"), result.mediaCount},
            {QStringLiteral("actions"), result.actionCount}}}};
    if (!result.warnings.isEmpty()) state.insert(QStringLiteral("warnings"), QJsonArray::fromStringList(result.warnings));
    if (state == saved_) return true;
    if (!QDir().mkpath(QFileInfo(cachePath_).absolutePath())) return false;
    const auto bytes = QJsonDocument(state).toJson(QJsonDocument::Compact);
    QSaveFile file(cachePath_);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) return false;
    saved_ = std::move(state);
    return true;
}

void PackBuildState::prune(const ProjectBuildSnapshot& snapshot)
{
    const QDir cache(PathUtils::getDotCacheDir(snapshot.media.projectRoot));
    const QDir targets(cache.filePath(QStringLiteral("pack/targets")));
    QSet<QString> media;
    for (const auto& file : targets.entryInfoList({QStringLiteral("*.json")}, QDir::Files)) {
        const auto state = readState(file.absoluteFilePath());
        const auto directory = state.value(QStringLiteral("directory")).toString();
        if (state.isEmpty() || directory.isEmpty() || !QDir(directory).exists()
            || !ProjectPlatformSemantics::find(snapshot.project.platforms,
                QUuid(state.value(QStringLiteral("platform")).toString()))) {
            QFile::remove(file.absoluteFilePath());
            continue;
        }
        for (const auto& pack : state.value(QStringLiteral("packs")).toObject())
            for (const auto& value : pack.toObject().value(QStringLiteral("media")).toArray())
                media.insert(value.toString());
    }
    const QDir conversions(cache.filePath(QStringLiteral("pack/media")));
    for (const auto& file : conversions.entryInfoList(
             {QStringLiteral("*.pcm"), QStringLiteral("*.pcm.??????")}, QDir::Files))
        if (file.suffix() != QStringLiteral("pcm") || !media.contains(file.completeBaseName()))
            QFile::remove(file.absoluteFilePath());
    for (const auto& legacy : {QStringLiteral("pack-build"), QStringLiteral("pack-inputs"),
                              QStringLiteral("media/pack-derivatives")}) {
        const auto path = cache.filePath(legacy);
        if (QFileInfo(path).isDir() && !QFileInfo(path).isSymLink()) QDir(path).removeRecursively();
    }
}

}
