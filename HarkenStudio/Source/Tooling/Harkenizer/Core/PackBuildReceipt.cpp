#include "Tooling/Harkenizer/Core/PackBuildReceipt.h"

#include "Tooling/Harkenizer/Core/ProjectBuildSnapshot.h"
#include "AssetPipeline/BuildSourceInputs.h"
#include "AssetPipeline/PathUtils.h"
#include "ProjectSchemaGenerated.h"
#include "ProjectSerializer.h"
#include "harken_buildinfo.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
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

QString fileDigest(const QString& path, const std::function<bool()>& canceled)
{
    QFile file(path);
    if (!QFileInfo(path).isFile() || QFileInfo(path).isSymLink() || !file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        if (canceled && canceled()) return {};
        const QByteArray bytes = file.read(1024 * 1024);
        if (bytes.isEmpty() && file.error() != QFile::NoError) return {};
        hash.addData(bytes);
    }
    if (file.error() != QFile::NoError) return {};
    return QString::fromLatin1(hash.result().toHex());
}

QJsonArray sortedStrings(const QSet<QString>& values)
{
    QStringList strings = values.values();
    strings.sort();
    return QJsonArray::fromStringList(strings);
}

QJsonObject requestIdentity(const ProjectPackExporter::ExportOptions& options)
{
    QJsonArray actions;
    for (const auto& action : options.selection.actionTargeting) actions.append(action.toJson());
    QJsonArray onlyActions;
    for (const auto& id : options.onlyActionStableIds) onlyActions.append(objectIdString(id));
    return {
        {QStringLiteral("platform"), options.platform},
        {QStringLiteral("language"), options.language},
        {QStringLiteral("debugStrings"), options.debugStrings},
        {QStringLiteral("selfCleaning"), options.selfCleaning},
        {QStringLiteral("updateSelectedOnly"), options.updateSelectedOnly},
        {QStringLiteral("includeUnchecked"), options.includeUnchecked},
        {QStringLiteral("waveformOutputDirectory"), options.waveformOutputDir},
        {QStringLiteral("platformSelection"), QJsonArray::fromStringList(options.selection.selectedPlatformIds)},
        {QStringLiteral("actionSelection"), actions},
        {QStringLiteral("onlyUnits"), sortedStrings(options.onlyUnits)},
        {QStringLiteral("onlyPackPaths"), sortedStrings(options.onlyPackPaths)},
        {QStringLiteral("onlyActions"), onlyActions}};
}

std::optional<QJsonObject> projectIdentity(const ProjectBuildSnapshot& snapshot,
                                         QMap<uint32_t, QString>* objectGuids,
                                         const std::function<bool()>& canceled)
{
    const auto& project = snapshot.project;
    ProjectSerializer serializer;
    if (!serializer.prepareReferenceWrite(project)) return std::nullopt;
    QJsonArray items;
    QSet<const ProjectItem*> visited;
    const auto visit = [&](const auto& self, const ProjectItem* item) -> bool {
        if (!item || visited.contains(item)) return true;
        if (canceled && canceled()) return false;
        visited.insert(item);
        auto object = serializer.projectItemToJson(*item, false);
        if (object.isEmpty()) return false;
        object.insert(QStringLiteral("documentHeader"), item->documentHeader);
        object.insert(QStringLiteral("physicalPath"), project.itemPath(item));
        object.insert(QStringLiteral("documentPath"), project.objectFilePath(item));
        object.insert(QStringLiteral("treeParent"), item->parent
            ? item->parent->guid.toString(QUuid::WithoutBraces) : QString());
        QJsonArray children;
        for (const auto& child : item->children)
            children.append(child ? child->guid.toString(QUuid::WithoutBraces) : QString());
        object.insert(QStringLiteral("children"), children);
        items.append(object);
        if (item->objectID != 0)
            objectGuids->insert(item->objectID, item->guid.toString(QUuid::WithoutBraces));
        for (const auto& child : item->children)
            if (!self(self, child.get())) return false;
        return true;
    };
    if (!visit(visit, project.root.get())) return std::nullopt;
    QJsonArray bindings;
    for (const auto* binding : project.allFileBindings()) {
        if (!binding || !binding->rootItem || !visit(visit, binding->rootItem.get())) return std::nullopt;
        bindings.append(QJsonObject{
            {QStringLiteral("root"), binding->rootItem->guid.toString(QUuid::WithoutBraces)},
            {QStringLiteral("bucket"), binding->parentFolder}});
    }
    QJsonArray groups;
    for (const auto& group : project.packGroups) groups.append(group.toJson());
    QJsonObject media;
    for (auto it = snapshot.media.assets.cbegin(); it != snapshot.media.assets.cend(); ++it) {
        const auto& asset = it.value();
        media.insert(QString::number(it.key()), QJsonObject{
            {QStringLiteral("path"), asset.absolutePath},
            {QStringLiteral("relativePath"), asset.relativePath},
            {QStringLiteral("settings"), asset.settings.toJson()}});
    }
    QJsonObject overrides;
    for (auto it = snapshot.media.fileOverrides.cbegin(); it != snapshot.media.fileOverrides.cend(); ++it)
        overrides.insert(QString::number(it.key()), it.value().toJson());
    return QJsonObject{
        {QStringLiteral("projectId"), project.projectId},
        {QStringLiteral("name"), project.name},
        {QStringLiteral("author"), project.author},
        {QStringLiteral("description"), project.description},
        {QStringLiteral("header"), project.documentHeader},
        {QStringLiteral("extensions"), project.extensions},
        {QStringLiteral("projectPath"), project.projectPath},
        {QStringLiteral("languages"), QJsonArray::fromStringList(project.projectLanguages)},
        {QStringLiteral("defaultLanguage"), project.defaultLanguage},
        {QStringLiteral("platforms"), ProjectPlatformSemantics::platformsToJson(project.platforms)},
        {QStringLiteral("sampleRate"), project.sampleRate},
        {QStringLiteral("channelConfig"), static_cast<int>(project.channelConfig)},
        {QStringLiteral("exportProjectData"), project.buildExportProjectData},
        {QStringLiteral("exportPackMetadata"), project.buildExportPackMetadata},
        {QStringLiteral("exportWaveforms"), project.buildExportWaveforms},
        {QStringLiteral("waveformOutputDirectory"), project.buildWaveformOutputDir},
        {QStringLiteral("bindings"), bindings},
        {QStringLiteral("items"), items},
        {QStringLiteral("groups"), groups},
        {QStringLiteral("media"), media},
        {QStringLiteral("mediaOverrides"), overrides}};
}

bool outputInventory(const QString& outputDirectory, QJsonObject* outputs,
                     const std::function<bool()>& canceled)
{
    QStringList files;
    const auto scan = [&](const auto& self, const QDir& directory) -> bool {
        if (!directory.exists() || QFileInfo(directory.absolutePath()).isSymLink()) return false;
        for (const auto& entry : directory.entryInfoList(
                 QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name)) {
            if (canceled && canceled()) return false;
            if (entry.isSymLink()) return false;
            const QString name = entry.fileName();
            if (name == QStringLiteral(".harken-pack-publishing")) return false;
            if (name == QStringLiteral(".harken-pack-build.lock")
                || name == QStringLiteral(".harken-pack-platform.lock")
                || name == QStringLiteral(".harken-pack-index.lock")
                || name == QStringLiteral(".harken-pack-waveforms.lock")
                || (entry.isDir() && (name.startsWith(QStringLiteral(".harken-pack-staging-"))
                    || name.startsWith(QStringLiteral(".harken-pack-previous-"))
                    || name.startsWith(QStringLiteral(".harken-pack-next-"))))) continue;
            if (entry.isDir()) {
                if (!self(self, QDir(entry.absoluteFilePath()))) return false;
            } else if (entry.isFile()) {
                files.append(entry.absoluteFilePath());
            } else {
                return false;
            }
        }
        return true;
    };
    if (!scan(scan, QDir(outputDirectory))) return false;
    const auto platformFiles = files;
    for (const auto& path : platformFiles) {
        if (!path.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) continue;
        const QFileInfo info(path);
        if (!QFileInfo::exists(info.dir().filePath(info.completeBaseName() + QStringLiteral(".bytes")))) continue;
        QFile companion(path);
        if (!companion.open(QIODevice::ReadOnly)) return false;
        const auto object = QJsonDocument::fromJson(companion.readAll()).object();
        for (const auto& value : object.value(QStringLiteral("waveforms")).toArray()) {
            const QString waveform = value.toObject().value(QStringLiteral("file")).toString();
            if (waveform.isEmpty()) return false;
            files.append(QDir::cleanPath(info.dir().absoluteFilePath(waveform)));
        }
    }
    files.removeDuplicates();
    files.sort();
    for (const auto& path : files) {
        const auto hash = fileDigest(path, canceled);
        if (hash.isEmpty()) return false;
        outputs->insert(path, hash);
    }
    return !outputs->isEmpty();
}

QJsonObject resultJson(const ExportResult& result)
{
    QJsonObject revisions;
    for (const auto& [id, revision] : result.soundContentRevisions)
        revisions.insert(objectIdString(id), QString::number(revision));
    QJsonArray artifacts;
    for (const auto& artifact : result.artifacts)
        artifacts.append(QJsonObject{
            {QStringLiteral("unitKey"), artifact.unitKey},
            {QStringLiteral("platform"), artifact.platform},
            {QStringLiteral("language"), artifact.language},
            {QStringLiteral("name"), artifact.packName},
            {QStringLiteral("path"), artifact.outputPath},
            {QStringLiteral("bytes"), artifact.bytes},
            {QStringLiteral("version"), static_cast<qint64>(artifact.version)}});
    QJsonObject object{
        {QStringLiteral("outputPath"), result.outputPath},
        {QStringLiteral("fileSize"), result.fileSize},
        {QStringLiteral("objectCount"), result.objectCount},
        {QStringLiteral("mediaCount"), result.mediaCount},
        {QStringLiteral("actionCount"), result.actionCount},
        {QStringLiteral("warnings"), QJsonArray::fromStringList(result.warnings)},
        {QStringLiteral("worldFingerprint"), QString::number(result.currentWorldSemanticFingerprint)},
        {QStringLiteral("soundRevisions"), revisions},
        {QStringLiteral("resourceFingerprint"), result.resourceFingerprint},
        {QStringLiteral("artifacts"), artifacts}};
    if (result.publication) {
        const auto& publication = *result.publication;
        object.insert(QStringLiteral("publication"), QJsonObject{
            {QStringLiteral("directory"), publication.directory},
            {QStringLiteral("projectId"), publication.projectId},
            {QStringLiteral("platformId"), publication.platformId},
            {QStringLiteral("packs"), publication.packs},
            {QStringLiteral("files"), QJsonArray::fromStringList(publication.managedFiles)}});
    }
    return object;
}

QJsonObject recordJson(const BuildRecord& record, const QMap<uint32_t, QString>& objectGuids)
{
    return {
        {QStringLiteral("name"), record.packName},
        {QStringLiteral("objectGuid"), objectGuids.value(record.objectID)},
        {QStringLiteral("source"), static_cast<int>(record.source)},
        {QStringLiteral("path"), record.outputPath},
        {QStringLiteral("bytes"), record.fileSize},
        {QStringLiteral("platform"), record.platform},
        {QStringLiteral("warnings"), QJsonArray::fromStringList(record.warnings)},
        {QStringLiteral("language"), record.language},
        {QStringLiteral("platformName"), record.platformName},
        {QStringLiteral("basePlatform"), static_cast<int>(record.basePlatform)},
        {QStringLiteral("platformDefinition"), record.platformDefinition},
        {QStringLiteral("unitKey"), record.unitKey},
        {QStringLiteral("omitted"), record.omitted}};
}

QStringList stringList(const QJsonValue& value)
{
    QStringList strings;
    for (const auto& entry : value.toArray()) strings.append(entry.toString());
    return strings;
}

}

std::optional<PackBuildReceipt> PackBuildReceipt::prepare(
    std::shared_ptr<const ProjectBuildSnapshot> snapshot,
    const ProjectPackExporter::ExportOptions& options,
    const QString& outputDirectory, const QByteArray& buildIdentity)
{
    if (!snapshot || !snapshot->media.sourceInputs || buildIdentity.isEmpty()
        || options.forceReexport || !options.currentWorldOutputPath.isEmpty()
        || options.resourceReuse || outputDirectory.isEmpty()
        || (options.cancelCheck && options.cancelCheck())) return std::nullopt;
    PackBuildReceipt receipt;
    receipt.snapshot_ = std::move(snapshot);
    receipt.canceled_ = options.cancelCheck;
    receipt.outputDirectory_ = QDir(outputDirectory).absolutePath();
    receipt.platform_ = options.platform;
    receipt.platformLock_ = options.platformLocks.value(options.platform);
    const auto project = projectIdentity(*receipt.snapshot_, &receipt.objectGuids_, receipt.canceled_);
    if (!project) return std::nullopt;
    auto identity = requestIdentity(options);
    identity.insert(QStringLiteral("outputDirectory"), receipt.outputDirectory_);
    receipt.scope_ = digest(identity);
    identity.insert(QStringLiteral("project"), *project);
    identity.insert(QStringLiteral("buildIdentity"), QString::fromLatin1(buildIdentity.toHex()));
    identity.insert(QStringLiteral("packSchema"), HARKEN_PACK_SCHEMA_VERSION);
    identity.insert(QStringLiteral("projectSchema"), ProjectSchema::kProjectSchemaVersion);
    identity.insert(QStringLiteral("version"), QString::fromUtf8(Harken::BuildInfo::Version.data(),
        static_cast<qsizetype>(Harken::BuildInfo::Version.size())));
    receipt.fingerprint_ = digest(identity);
    receipt.cachePath_ = QDir(PathUtils::getDotCacheDir(
        QFileInfo(receipt.snapshot_->project.projectPath).absolutePath()))
        .filePath(QStringLiteral("pack-build/") + receipt.scope_ + QStringLiteral(".json"));
    return receipt;
}

bool PackBuildReceipt::restore(ExportResult* result, std::vector<BuildRecord>* records) const
{
    if (!result || !records || (canceled_ && canceled_())) return false;
    QFile file(cachePath_);
    if (!file.open(QIODevice::ReadOnly)) return false;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
    const auto envelope = document.object();
    const auto payload = envelope.value(QStringLiteral("payload")).toObject();
    if (payload.value(QStringLiteral("version")).toInt() != 1
        || payload.value(QStringLiteral("scope")).toString() != scope_
        || payload.value(QStringLiteral("fingerprint")).toString() != fingerprint_
        || envelope.value(QStringLiteral("hash")).toString() != digest(payload)
        || !payload.value(QStringLiteral("sources")).isObject()
        || !payload.value(QStringLiteral("outputs")).isObject()
        || !payload.value(QStringLiteral("result")).isObject()
        || !payload.value(QStringLiteral("records")).isArray()) return false;
    const auto sources = payload.value(QStringLiteral("sources")).toObject();
    BuildSourceInputs probes;
    for (auto it = sources.constBegin(); it != sources.constEnd(); ++it) {
        if (canceled_ && canceled_()) return false;
        bool valid = false;
        const uint32_t id = it.key().toUInt(&valid);
        const auto asset = snapshot_->media.assets.constFind(id);
        if (!valid || it.value().toString().isEmpty()
            || asset == snapshot_->media.assets.cend()
            || probes.sourceFingerprint(*asset) != it.value().toString()) return false;
    }
    QJsonObject outputs;
    if (!outputInventory(outputDirectory_, &outputs, canceled_)
        || outputs != payload.value(QStringLiteral("outputs")).toObject()) return false;
    const auto saved = payload.value(QStringLiteral("result")).toObject();
    ExportResult restored;
    restored.outputPath = saved.value(QStringLiteral("outputPath")).toString();
    if (QDir(restored.outputPath).absolutePath() != outputDirectory_) return false;
    restored.fileSize = saved.value(QStringLiteral("fileSize")).toInteger();
    restored.objectCount = saved.value(QStringLiteral("objectCount")).toInt();
    restored.mediaCount = saved.value(QStringLiteral("mediaCount")).toInt();
    restored.actionCount = saved.value(QStringLiteral("actionCount")).toInt();
    restored.warnings = stringList(saved.value(QStringLiteral("warnings")));
    restored.currentWorldSemanticFingerprint = saved.value(QStringLiteral("worldFingerprint")).toString().toULongLong();
    restored.resourceFingerprint = saved.value(QStringLiteral("resourceFingerprint")).toString();
    const auto revisions = saved.value(QStringLiteral("soundRevisions")).toObject();
    for (auto it = revisions.constBegin(); it != revisions.constEnd(); ++it) {
        harkenstudio::ObjectId id;
        bool valid = false;
        const auto revision = it.value().toString().toULongLong(&valid);
        if (!parseObjectIdJson(it.key(), &id) || !valid) return false;
        restored.soundContentRevisions.emplace(id, revision);
    }
    for (const auto& value : saved.value(QStringLiteral("artifacts")).toArray()) {
        const auto row = value.toObject();
        PackArtifactMetadata artifact;
        artifact.unitKey = row.value(QStringLiteral("unitKey")).toString();
        artifact.platform = row.value(QStringLiteral("platform")).toString();
        artifact.language = row.value(QStringLiteral("language")).toString();
        artifact.packName = row.value(QStringLiteral("name")).toString();
        artifact.outputPath = row.value(QStringLiteral("path")).toString();
        artifact.bytes = row.value(QStringLiteral("bytes")).toInteger(-1);
        artifact.version = static_cast<quint32>(row.value(QStringLiteral("version")).toInteger());
        const QFileInfo info(artifact.outputPath);
        if (artifact.platform != platform_ || artifact.unitKey.isEmpty()
            || !outputs.contains(info.absoluteFilePath()) || artifact.bytes != info.size()
            || artifact.version == 0) return false;
        artifact.modified = info.lastModified();
        restored.artifacts.append(std::move(artifact));
    }
    if (restored.artifacts.isEmpty()) return false;
    if (saved.contains(QStringLiteral("publication"))) {
        const auto publication = saved.value(QStringLiteral("publication")).toObject();
        PackPublicationProof proof;
        proof.directory = publication.value(QStringLiteral("directory")).toString();
        proof.projectId = publication.value(QStringLiteral("projectId")).toString();
        proof.platformId = publication.value(QStringLiteral("platformId")).toString();
        proof.packs = publication.value(QStringLiteral("packs")).toArray();
        proof.managedFiles = stringList(publication.value(QStringLiteral("files")));
        proof.outputLock = platformLock_;
        if (proof.directory != outputDirectory_ || proof.platformId != platform_
            || QUuid(proof.projectId) != QUuid(snapshot_->project.projectId)
            || proof.packs.isEmpty() || proof.managedFiles.isEmpty()) return false;
        for (const auto& path : proof.managedFiles)
            if (!outputs.contains(QDir(outputDirectory_).absoluteFilePath(path))) return false;
        restored.publication = std::move(proof);
    }
    QMap<QString, uint32_t> objectIds;
    for (auto it = objectGuids_.cbegin(); it != objectGuids_.cend(); ++it)
        objectIds.insert(it.value(), it.key());
    std::vector<BuildRecord> restoredRecords;
    for (const auto& value : payload.value(QStringLiteral("records")).toArray()) {
        const auto row = value.toObject();
        BuildRecord record;
        record.packName = row.value(QStringLiteral("name")).toString();
        const auto objectGuid = row.value(QStringLiteral("objectGuid")).toString();
        if (!objectGuid.isEmpty() && !objectIds.contains(objectGuid)) return false;
        record.objectID = objectIds.value(objectGuid);
        const int source = row.value(QStringLiteral("source")).toInt(-1);
        const int basePlatform = row.value(QStringLiteral("basePlatform")).toInt(-1);
        if (source < static_cast<int>(BuildRecord::Source::Group)
            || source > static_cast<int>(BuildRecord::Source::Shared)
            || basePlatform < static_cast<int>(BasePlatform::Windows)
            || basePlatform > static_cast<int>(BasePlatform::iOS)) return false;
        record.source = static_cast<BuildRecord::Source>(source);
        record.basePlatform = static_cast<BasePlatform>(basePlatform);
        record.outputPath = row.value(QStringLiteral("path")).toString();
        record.fileSize = row.value(QStringLiteral("bytes")).toInteger(-1);
        record.platform = row.value(QStringLiteral("platform")).toString();
        record.warnings = stringList(row.value(QStringLiteral("warnings")));
        record.language = row.value(QStringLiteral("language")).toString();
        record.platformName = row.value(QStringLiteral("platformName")).toString();
        record.platformDefinition = row.value(QStringLiteral("platformDefinition")).toObject();
        record.unitKey = row.value(QStringLiteral("unitKey")).toString();
        record.omitted = row.value(QStringLiteral("omitted")).toBool();
        if (record.platform != platform_ || record.unitKey.isEmpty() || record.fileSize < 0
            || (!record.omitted && !outputs.contains(QFileInfo(record.outputPath).absoluteFilePath()))) return false;
        record.success = true;
        record.skipped = true;
        record.elapsedMs = 0;
        if (!record.omitted) ++restored.skippedPackCount;
        restoredRecords.push_back(std::move(record));
    }
    if (restoredRecords.empty() || (canceled_ && canceled_())) return false;
    if (!snapshot_->media.sourceInputs->adopt(probes)) return false;
    restored.success = true;
    *result = std::move(restored);
    *records = std::move(restoredRecords);
    return true;
}

bool PackBuildReceipt::save(const ExportResult& result, const std::vector<BuildRecord>& records,
                           bool cacheable, const QMap<uint32_t, QString>& sourceDependencies) const
{
    if (!cacheable || !result.success || result.canceled || !result.error.isEmpty()
        || result.mediaFallbacks != 0 || result.artifacts.isEmpty() || records.empty()
        || (canceled_ && canceled_())) return false;
    QJsonArray savedRecords;
    for (const auto& record : records) {
        if (!record.success || !record.error.isEmpty()
            || (record.objectID != 0 && !objectGuids_.contains(record.objectID))) return false;
        savedRecords.append(recordJson(record, objectGuids_));
    }
    QJsonObject outputs;
    if (!outputInventory(outputDirectory_, &outputs, canceled_)) return false;
    QJsonObject sources;
    for (auto it = sourceDependencies.cbegin(); it != sourceDependencies.cend(); ++it) {
        if (it.value().isEmpty()) return false;
        sources.insert(QString::number(it.key()), it.value());
    }
    const QJsonObject payload{
        {QStringLiteral("version"), 1},
        {QStringLiteral("scope"), scope_},
        {QStringLiteral("fingerprint"), fingerprint_},
        {QStringLiteral("sources"), sources},
        {QStringLiteral("outputs"), outputs},
        {QStringLiteral("result"), resultJson(result)},
        {QStringLiteral("records"), savedRecords}};
    const auto bytes = QJsonDocument(QJsonObject{
        {QStringLiteral("payload"), payload}, {QStringLiteral("hash"), digest(payload)}})
        .toJson(QJsonDocument::Compact);
    if ((canceled_ && canceled_()) || !QDir().mkpath(QFileInfo(cachePath_).absolutePath())) return false;
    QSaveFile file(cachePath_);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

}
