#include "FieldGroupProjectAdapter.h"
#include "CanonicalRuntimeProjectAdapterPrivate.h"
#include "FieldGroupEditing.h"
#include "FieldGroupDerivedView.h"

#include "ProjectManager.h"
#include "ProjectInstance.h"
#include "AssetPipeline/SourceFileSystem.h"
#include "AuthoringPolicy.h"
#include "ProjectLanguageSemantics.h"
#include "CanonicalRuntimeProjectAdapter.h"
#include "AssetPipeline/AssetGarbageCollector.h"
#include "AssetPipeline/AssetImporter.h"
#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/MediaPool.h"
#include "AssetPipeline/PathUtils.h"
#include "AttenuationStore.h"
#include "BindingContract.h"
#include "AudioEngine.h"
#include "BusPlacementRules.h"
#include "BusRouteGraph.h"
#include "BucketFolderStore.h"
#include "FolderSaveState.h"
#include "ProjectItemSaveState.h"
#include "PhysicalFolderOperations.h"
#include "PhysicalFolderReferenceEdits.h"
#include "RuntimeSyncCoordinator.h"
#include "HubClient/HubIntake.h"
#include "GridSemantics.h"
#include "ContainerBehaviorSemantics.h"
#include "IntegrityScanner.h"
#include "TransitionSemantics.h"
#include "MigrationManager.h"
#include "ProjectContract.h"
#include "ProjectDataPolicy.h"
#include "ProjectDocument.h"
#include "ProjectTransaction.h"
#include "ProjectUpgradeMessages.h"
#include "NameValidator.h"
#include "PlayablePropertyAuthoringV1.h"
#include "PlaybackReferenceValidation.h"
#include "ProjectPackExporter.h"
#include "PackGenerationRecord.h"
#include "PackArtifactCache.h"
#include "ProjectIndex.h"
#include "PropertyEditSession.h"
#include "ReferenceSlotResolution.h"
#include "TemplateApplyService.h"
#include "Widgets/LogPanel/LogPanel.h"
#include "UndoCommands.h"
#include "Components/VAttenuation.h"
#include "Components/VAudio.h"
#include "Components/VMixNode.h"
#include "Components/VSpatial.h"
#include "Components/VBus.h"
#include "Components/VAction.h"
#include "Components/VSelection.h"
#include "Components/VVariable.h"
#include "Components/VSound.h"
#include "LanguageContextService.h"
#include "PlatformContextService.h"
#include "SoundSourceEditing.h"
#include "SoundSourceSemantics.h"
#include "Components/VTrack.h"
#include "Components/VPiece.h"
#include "Timeline/TimelineEditing.h"
#include "ActionStepUtils.h"
#include "HarkenMath.h"
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QApplication>
#include <QDateTime>
#include <QDebug>
#include <QTimer>
#include <QThread>
#include <QTemporaryDir>
#include <QUuid>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSet>
#include <cmath>

#include <algorithm>
#include <array>
#include <functional>
#include <iterator>
#include <limits>
#include <unordered_map>
#include <utility>

namespace VNX {

ProjectItem* findItemByObjectIDInProject(ProjectData* project, uint32_t objectID);
std::vector<ProjectItem*> findItemsByObjectIDInProject(
    ProjectData* project, std::span<const uint32_t> objectIDs);
ProjectItem* findItemByGuidInProject(ProjectData* project, const QUuid& guid);
void applyStepsList(uint32_t objectID, const std::vector<VActionStep>& target);

namespace {

constexpr const char* kActionsFolder = ItemTypeCatalog::kActionsFolderName;

bool copyDirectoryTree(const QString& sourcePath, const QString& destinationPath)
{
    QDir source(sourcePath);
    if (!source.exists()) return true;
    if (!QDir().mkpath(destinationPath)) return false;

    const QFileInfoList entries = source.entryInfoList(
        QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo& entry : entries) {
        const QString destination = QDir(destinationPath).filePath(entry.fileName());
        if (entry.isDir()) {
            if (!copyDirectoryTree(entry.absoluteFilePath(), destination)) return false;
        } else if (!QFile::copy(entry.absoluteFilePath(), destination)) {
            return false;
        }
    }
    return true;
}

bool writePackAtomically(const QString& filePath, const QByteArray& bytes)
{
    constexpr int kMaxAttempts = 3;
    constexpr int kRetryDelayMs = 50;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        QSaveFile output(filePath);
        if (output.open(QIODevice::WriteOnly | QIODevice::Text)
            && output.write(bytes) == bytes.size() && output.commit())
            return true;
        if (attempt < kMaxAttempts)
            QThread::msleep(kRetryDelayMs);
    }
    return false;
}

QString normalizedFolderName(const QString& name)
{
    QString normalized = name.trimmed();
    while (normalized.endsWith(QChar('.')))
        normalized.chop(1);
    return normalized;
}

QString folderNameKey(const QString& name)
{
    return normalizedFolderName(name).toCaseFolded();
}

void startupTrace(const QString& message)
{
    if (!qEnvironmentVariableIsSet("HARKEN_STARTUP_TRACE"))
        return;
    QDir().mkpath(QStringLiteral(".temp"));
    QFile f(QStringLiteral(".temp/editor_startup_trace.log"));
    if (!f.open(QIODevice::Append | QIODevice::Text))
        return;
    const QString line = QStringLiteral("%1 %2\n")
        .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs), message);
    f.write(line.toUtf8());
}

QString actionsRootDir(const QString& projectRoot)
{
    return QDir(projectRoot).filePath(kActionsFolder);
}

QString legacyAudioSourcesDirName()
{
    return QStringLiteral("Original") + QStringLiteral("s");
}

QString legacyCacheDirName()
{
    return QStringLiteral("Ca") + QStringLiteral("che");
}

void migrateWaveformCachesToDotCache(const QString& projectRoot)
{
    const QString audioSourcesDir = QDir(projectRoot).filePath(
        QString::fromLatin1(PathUtils::AUDIO_SOURCES_DIR));
    if (!QDir(audioSourcesDir).exists())
        return;

    QDirIterator it(audioSourcesDir,
                    QStringList{ QStringLiteral("*.vkd") },
                    QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QString oldVkdPath = it.filePath();
        QString audioPath = oldVkdPath;
        audioPath.chop(QStringLiteral(".vkd").size());

        if (!QFileInfo::exists(audioPath)) {
            QFile::remove(oldVkdPath);
            continue;
        }

        const QString relativeAudioPath = PathUtils::toRelativePath(audioPath, audioSourcesDir);
        const QString newVkdPath = QDir(PathUtils::getWaveformCacheDir(projectRoot))
            .filePath(relativeAudioPath + QStringLiteral(".vkd"));
        QDir().mkpath(QFileInfo(newVkdPath).absolutePath());

        if (QFileInfo::exists(newVkdPath)) {
            QFile::remove(oldVkdPath);
        } else if (!QFile::rename(oldVkdPath, newVkdPath)) {
            qWarning() << "[ProjectManager] Failed to migrate waveform cache:"
                       << oldVkdPath << "->" << newVkdPath;
        }
    }
}

void migrateAudioSourceLayout(const QString& projectRoot)
{
    QDir projectDir(projectRoot);
    const QString legacyAudioDirName = legacyAudioSourcesDirName();
    const QString audioSourcesDirName = QString::fromLatin1(PathUtils::AUDIO_SOURCES_DIR);

    const QString legacyAudioDir = projectDir.filePath(legacyAudioDirName);
    const QString audioSourcesDir = projectDir.filePath(audioSourcesDirName);
    if (QDir(legacyAudioDir).exists() && !QDir(audioSourcesDir).exists()) {
        if (projectDir.rename(legacyAudioDirName, audioSourcesDirName)) {
            qInfo() << "[ProjectManager] Migrated project audio source directory to"
                    << audioSourcesDir;
        } else {
            qWarning() << "[ProjectManager] Failed to migrate project audio source directory:"
                       << legacyAudioDir << "->" << audioSourcesDir;
        }
    }

    migrateWaveformCachesToDotCache(projectRoot);

    const QString legacyCacheDir = projectDir.filePath(legacyCacheDirName());
    if (QDir(legacyCacheDir).exists()) {
        if (QDir(legacyCacheDir).removeRecursively()) {
            qInfo() << "[ProjectManager] Removed legacy top-level cache directory:"
                    << legacyCacheDir;
        } else {
            qWarning() << "[ProjectManager] Failed to remove legacy top-level cache directory:"
                       << legacyCacheDir;
        }
    }
}

std::shared_ptr<ProjectItem> loadOneActionFile(const QFileInfo& fileInfo,
                                              ProjectSerializer& serializer)
{
    QFile file(fileInfo.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "[ActionLoader] Failed to open" << fileInfo.absoluteFilePath();
        return nullptr;
    }
    const QByteArray data = file.readAll();
    file.close();

    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseErr);
    if (doc.isNull() || !doc.isObject()) {
        qWarning() << "[ActionLoader] JSON parse failed for"
                   << fileInfo.absoluteFilePath() << ":" << parseErr.errorString();
        return nullptr;
    }
    ProjectDocumentData document;
    QString documentError;
    if (!ProjectDocument::parse(doc.object(), QStringLiteral("project.action"),
                                &document, &documentError)) {
        qWarning() << "[ActionLoader]" << documentError << "in"
                   << fileInfo.absoluteFilePath();
        return nullptr;
    }
    const QJsonObject obj = document.content;

    auto ev = std::make_shared<ProjectItem>(QString(), ProjectItem::Type::Action);
    if (!serializer.actionPackFromJson(obj, *ev)) {
        qWarning() << "[ActionLoader] actionPackFromJson rejected payload in"
                   << fileInfo.absoluteFilePath();
        return nullptr;
    }

    ev->documentHeader = document.header;

    return ev;
}

bool loadActions(const QString& projectRoot, ProjectData& project,
                 ProjectSerializer& serializer, QString* errOut = nullptr)
{
    const QString rootDir = actionsRootDir(projectRoot);
    if (!QDir(rootDir).exists()) {
        return true;
    }

    auto folders = BucketFolderStore::scanFolderTree(
        rootDir, QString::fromLatin1(kActionsFolder));
    project.folders.insert(project.folders.end(),
                           std::make_move_iterator(folders.begin()),
                           std::make_move_iterator(folders.end()));

    QDirIterator it(rootDir,
                    QStringList() << QStringLiteral("*.vp"),
                    QDir::Files,
                    QDirIterator::Subdirectories);
    int loaded = 0;
    int failed = 0;
    while (it.hasNext()) {
        it.next();
        const QFileInfo fi = it.fileInfo();
        auto ev = loadOneActionFile(fi, serializer);
        if (!ev) {
            ++failed;
            continue;
        }
        ev->parent = nullptr;
        ev->parentRef = ParentRef{};

        ProjectFileBinding binding;
        binding.parentFolder = QString::fromLatin1(kActionsFolder);
        binding.rootItem     = std::move(ev);
        PhysicalFolder* folder = project.findFolderByPath(
            binding.parentFolder, QDir(rootDir).relativeFilePath(fi.absolutePath()));
        project.setObjectFileLocation(binding.rootItem.get(), folder, fi.absoluteFilePath());
        project.actions.push_back(std::move(binding));
        ++loaded;
    }

    qDebug() << "[ActionLoader] Loaded" << loaded << "actions as bindings under" << rootDir;
    if (failed > 0) {
        if (errOut)
            *errOut = QStringLiteral("Actions contains %1 unreadable or invalid Pack file(s)")
                          .arg(failed);
        return false;
    }
    return true;
}

bool saveOneAction(const QString& targetDirPath, const QString& projectId,
                  const ProjectItem& ev,
                  ProjectSerializer& serializer)
{
    if (ev.type != ProjectItem::Type::Action) {
        qWarning() << "[ActionLoader] saveOneAction called on non-Action:" << ev.name;
        return false;
    }

    if (!QDir(targetDirPath).exists()) {
        qWarning() << "[ActionLoader] Directory does not exist:" << targetDirPath;
        return false;
    }

    const QString filePath = QDir(targetDirPath).filePath(NameValidator::objectFileName(ev.name, ev.guid));

    const QJsonObject content = serializer.actionPackToJson(ev);
    if (content.isEmpty()) {
        qWarning() << "[ActionLoader] actionPackToJson returned empty payload for"
                   << ev.name << "-- refusing to write empty .vp";
        return false;
    }

    const QJsonObject document = ProjectDocument::create(
        QStringLiteral("project.action"),
        ev.guid.toString(QUuid::WithoutBraces), projectId, content,
        ev.documentHeader);
    const QByteArray bytes = QJsonDocument(document).toJson(QJsonDocument::Indented);
    if (!writePackAtomically(filePath, bytes)) {
        qWarning() << "[ActionLoader] Short write to:" << filePath;
        return false;
    }
    return true;
}

bool saveActionsBucket(const QString& projectRoot, const QString& projectId,
                       ProjectData& project,
                       std::vector<ProjectFileBinding>& bucket,

                       ProjectSerializer& serializer)
{
    QDir eventsDir(actionsRootDir(projectRoot));
    const QString eventsRootAbs = eventsDir.absolutePath();

    bool allOk = QDir().mkpath(eventsRootAbs);
    int savedCount = 0;
    QSet<QString> writtenAbsPaths;
    for (auto& binding : bucket) {
        if (!binding.rootItem || binding.rootItem->type != ProjectItem::Type::Action) continue;
        const ProjectItem& ev = *binding.rootItem;
        const QString targetDirPath = eventsDir.filePath(project.itemFolderPath(&ev));
        const QString fullPath =
            QDir(targetDirPath).filePath(NameValidator::objectFileName(ev.name, ev.guid));
        writtenAbsPaths.insert(BucketFolderStore::pathComparisonKey(fullPath));

        if (saveOneAction(targetDirPath, projectId, ev, serializer)) {
            project.setObjectFileLocation(&ev, project.objectFolder(&ev), fullPath);
            ++savedCount;
        } else {
            allOk = false;
        }
    }
    qDebug() << "[ActionLoader] Saved" << savedCount << "actions to disk; ok=" << allOk;

    if (!allOk) {
        qWarning() << "[ActionLoader] Refusing destructive prune after save failure under"
                   << eventsRootAbs;
        return false;
    }

    if (QDir(eventsRootAbs).exists()) {
        QDirIterator it(eventsRootAbs,
                        QStringList() << QStringLiteral("*.vp"),
                        QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            const QFileInfo fi = it.fileInfo();
            const QString abs = QDir::cleanPath(fi.absoluteFilePath());
            if (writtenAbsPaths.contains(
                    BucketFolderStore::pathComparisonKey(abs))) continue;

            QFile probe(abs);
            if (!probe.open(QIODevice::ReadOnly)) continue;
            QJsonParseError parseError{};
            const QJsonDocument document = QJsonDocument::fromJson(probe.readAll(),
                                                                   &parseError);
            probe.close();
            if (!document.isObject()
                || document.object().value(QStringLiteral("harkenDocument")).toObject()
                       .value(QStringLiteral("type")).toString()
                    != QStringLiteral("project.action")) {
                continue;
            }

            if (!QFile::remove(abs)) {
                qWarning() << "[ActionLoader] Failed to prune stale action file:" << abs;
                allOk = false;
            } else {
                qDebug() << "[ActionLoader] Pruned stale action Pack:" << abs;
            }
        }
    }

    if (!allOk) return false;

    return allOk;
}

}

namespace {

struct SingleObjHierarchySpec
{
    QString folderName;
    ProjectItem::Type topType;
};

const std::vector<SingleObjHierarchySpec>& s3Specs()
{
    static const std::vector<SingleObjHierarchySpec> specs = {
        { QStringLiteral("Attenuations"),    ProjectItem::Type::Attenuation },
        { QLatin1String(ItemTypeCatalog::kVariablesFolderName),
                                             ProjectItem::Type::Variable },
    };
    return specs;
}

QString s3RootDir(const QString& projectRoot, const SingleObjHierarchySpec& spec)
{
    return QDir(projectRoot).filePath(spec.folderName);
}

QString s3DocumentType(const SingleObjHierarchySpec& spec)
{
    return spec.topType == ProjectItem::Type::Attenuation
               ? QStringLiteral("project.attenuation")
               : QStringLiteral("project.variable");
}

void rebindOwner(ProjectItem& node, ProjectItem* parent)
{
    node.parent = parent;
    for (const auto& c : node.children) {
        if (c) rebindOwner(*c, &node);
    }
}

bool loadS3PayloadByType(const SingleObjHierarchySpec& spec,
                         const QJsonObject& obj,
                         ProjectItem& item,
                         ProjectSerializer& serializer)
{
    switch (spec.topType) {
    case ProjectItem::Type::Attenuation:
        return serializer.attenuationPackFromJson(obj, item);
    case ProjectItem::Type::Variable:
        return serializer.variablePackFromJson(obj, item);
    default:
        qWarning() << "[S3Loader] Unhandled topType in dispatch:"
                   << static_cast<int>(spec.topType);
        return false;
    }
}

std::shared_ptr<ProjectItem> loadOneS3File(const SingleObjHierarchySpec& spec,
                                           const QFileInfo& fileInfo,
                                           ProjectSerializer& serializer)
{
    QFile file(fileInfo.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "[S3Loader]" << spec.folderName << "open failed:" << fileInfo.absoluteFilePath();
        return nullptr;
    }
    const QByteArray data = file.readAll();
    file.close();

    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseErr);
    if (doc.isNull() || !doc.isObject()) {
        qWarning() << "[S3Loader]" << spec.folderName << "JSON parse failed for"
                   << fileInfo.absoluteFilePath() << ":" << parseErr.errorString();
        return nullptr;
    }
    ProjectDocumentData document;
    QString documentError;
    if (!ProjectDocument::parse(doc.object(), s3DocumentType(spec),
                                &document, &documentError)) {
        qWarning() << "[S3Loader]" << documentError << "in"
                   << fileInfo.absoluteFilePath();
        return nullptr;
    }
    const QJsonObject obj = document.content;

    const QString storedType = obj.value("type").toString();
    if (storedType != ItemTypeCatalog::canonicalName(spec.topType)) {
        qWarning() << "[S3Loader] Skipping wrong-typed vp under" << spec.folderName << ":"
                   << fileInfo.absoluteFilePath() << "type=" << storedType;
        return nullptr;
    }

    auto item = std::make_shared<ProjectItem>(QString(), spec.topType);
    if (!loadS3PayloadByType(spec, obj, *item, serializer)) {
        qWarning() << "[S3Loader] serializer rejected payload:" << fileInfo.absoluteFilePath();
        return nullptr;
    }

    item->documentHeader = document.header;

    rebindOwner(*item, nullptr);
    return item;
}

bool loadS3Hierarchy(const QString& projectRoot,
                     const SingleObjHierarchySpec& spec,
                     ProjectData& project,
                     ProjectSerializer& serializer,
                     QString* errOut = nullptr)
{
    const QString rootDir = s3RootDir(projectRoot, spec);
    if (!QDir(rootDir).exists()) {
        return true;
    }

    auto folders = BucketFolderStore::scanFolderTree(rootDir, spec.folderName);
    project.folders.insert(project.folders.end(),
                           std::make_move_iterator(folders.begin()),
                           std::make_move_iterator(folders.end()));

    auto* bucket = project.getBucketForFolder(spec.folderName);
    if (!bucket) {
        qCritical() << "[S3Loader] No bucket for folder" << spec.folderName
                    << "-- bindings would be dropped on the floor.";
        return false;
    }

    QDirIterator it(rootDir,
                    QStringList() << QStringLiteral("*.vp"),
                    QDir::Files,
                    QDirIterator::Subdirectories);
    int loaded = 0;
    int failed = 0;
    while (it.hasNext()) {
        it.next();
        const QFileInfo fi = it.fileInfo();
        auto top = loadOneS3File(spec, fi, serializer);
        if (!top) {
            ++failed;
            continue;
        }
        top->parent = nullptr;
        top->parentRef = ParentRef{};

        ProjectFileBinding binding;
        binding.parentFolder = spec.folderName;
        binding.rootItem     = std::move(top);
        PhysicalFolder* folder = project.findFolderByPath(
            binding.parentFolder, QDir(rootDir).relativeFilePath(fi.absolutePath()));
        project.setObjectFileLocation(binding.rootItem.get(), folder, fi.absoluteFilePath());
        bucket->push_back(std::move(binding));
        ++loaded;
    }
    qDebug() << "[S3Loader]" << spec.folderName << "loaded" << loaded << "top-level objects";
    if (failed > 0) {
        if (errOut)
            *errOut = QStringLiteral("%1 contains %2 unreadable or invalid Pack file(s)")
                          .arg(spec.folderName).arg(failed);
        return false;
    }
    return true;
}

QJsonObject writeS3PayloadByType(const ProjectItem& top, ProjectSerializer& serializer)
{
    switch (top.type) {
    case ProjectItem::Type::Attenuation: return serializer.attenuationPackToJson(top);
    case ProjectItem::Type::Variable:    return serializer.variablePackToJson(top);
    default:
        qWarning() << "[S3Saver] Unhandled top type in dispatch:"
                   << static_cast<int>(top.type);
        return {};
    }
}

bool saveOneS3File(const QString& targetDirPath,
                   const QString& projectId,
                   const SingleObjHierarchySpec& spec,
                   const ProjectItem& top,
                   ProjectSerializer& serializer)
{
    if (top.type != spec.topType) {
        qWarning() << "[S3Saver] type mismatch in" << spec.folderName << ":"
                   << top.name << "type=" << static_cast<int>(top.type);
        return false;
    }

    if (!QDir(targetDirPath).exists()) {
        qWarning() << "[S3Saver] Directory does not exist:" << targetDirPath;
        return false;
    }

    const QString filePath = QDir(targetDirPath).filePath(NameValidator::objectFileName(top.name, top.guid));
    const QJsonObject content = writeS3PayloadByType(top, serializer);
    if (content.isEmpty()) {
        qWarning() << "[S3Saver] serializer returned empty payload for:" << filePath
                   << serializer.lastError();
        return false;
    }

    const QJsonObject document = ProjectDocument::create(
        s3DocumentType(spec), top.guid.toString(QUuid::WithoutBraces),
        projectId, content, top.documentHeader);
    const QByteArray bytes = QJsonDocument(document).toJson(QJsonDocument::Indented);
    if (!writePackAtomically(filePath, bytes)) {
        qWarning() << "[S3Saver] short write:" << filePath;
        return false;
    }
    return true;
}

bool saveS3HierarchyBucket(const QString& projectRoot,
                           const QString& projectId,
                           ProjectData& project,
                           const SingleObjHierarchySpec& spec,
                           std::vector<ProjectFileBinding>& bucket,

                           ProjectSerializer& serializer)
{
    QDir hierDir(s3RootDir(projectRoot, spec));
    const QString hierRootAbs = hierDir.absolutePath();

    bool allOk = QDir().mkpath(hierRootAbs);
    int savedCount = 0;
    QSet<QString> writtenAbsPaths;
    for (auto& binding : bucket) {
        if (!binding.rootItem || binding.rootItem->type != spec.topType) continue;
        const ProjectItem& top = *binding.rootItem;
        const QString targetDirPath = hierDir.filePath(project.itemFolderPath(&top));
        const QString fullPath =
            QDir(targetDirPath).filePath(NameValidator::objectFileName(top.name, top.guid));
        writtenAbsPaths.insert(BucketFolderStore::pathComparisonKey(fullPath));

        if (saveOneS3File(targetDirPath, projectId, spec, top, serializer)) {
            project.setObjectFileLocation(&top, project.objectFolder(&top), fullPath);
            ++savedCount;
        } else {
            allOk = false;
        }
    }
    qDebug() << "[S3Saver]" << spec.folderName << "saved" << savedCount << "ok=" << allOk;

    if (!allOk) {
        qWarning() << "[S3Saver] Refusing destructive prune after save failure under"
                   << hierRootAbs;
        return false;
    }

    if (QDir(hierRootAbs).exists()) {
        QDirIterator it(hierRootAbs,
                        QStringList() << QStringLiteral("*.vp"),
                        QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            const QFileInfo fi = it.fileInfo();
            const QString abs = QDir::cleanPath(fi.absoluteFilePath());
            if (writtenAbsPaths.contains(
                    BucketFolderStore::pathComparisonKey(abs))) continue;

            QFile probe(abs);
            if (!probe.open(QIODevice::ReadOnly)) continue;
            const QByteArray bytes = probe.readAll();
            probe.close();

            QJsonParseError perr{};
            const QJsonDocument doc = QJsonDocument::fromJson(bytes, &perr);
            if (doc.isNull() || !doc.isObject()) continue;

            const QString storedType = doc.object()
                                           .value(QStringLiteral("harkenDocument")).toObject()
                                           .value(QStringLiteral("type")).toString();
            if (storedType != s3DocumentType(spec)) continue;

            if (!QFile::remove(abs)) {
                qWarning() << "[S3Saver]" << spec.folderName
                           << "failed to prune stale file:" << abs;
                allOk = false;
            } else {
                qDebug() << "[S3Saver]" << spec.folderName
                         << "pruned stale Pack:" << abs;
            }
        }
    }

    if (!allOk) return false;

    return allOk;
}

const SingleObjHierarchySpec* findS3Spec(const QString& folder)
{
    for (const auto& s : s3Specs()) if (s.folderName == folder) return &s;
    return nullptr;
}

}


void ensureDefaultMasterBus(ProjectData& project);

namespace {

constexpr const char* kBusFolder = ItemTypeCatalog::kBussesFolderName;

QString busFolderAbs(const QString& projectRoot)
{
    return QDir(projectRoot).filePath(kBusFolder);
}

QString busPackFileName(const ProjectItem& busItem)
{
    return NameValidator::objectFileName(busItem.name, busItem.guid);
}

bool saveOneBusPack(const QString& projectRoot,
                   const QString& projectId,
                   const ProjectItem& busItem,
                   ProjectSerializer& serializer)
{
    QDir dir(busFolderAbs(projectRoot));
    if (!dir.exists() && !QDir().mkpath(dir.absolutePath())) {
        qWarning() << "[BussesPack] Directory does not exist:" << dir.absolutePath();
        return false;
    }

    const QString filePath = dir.filePath(busPackFileName(busItem));

    const QJsonObject content = serializer.busPackToJson(busItem);
    if (content.isEmpty()) {
        qWarning() << "[BussesPack] Serializer returned empty payload for bus:"
                   << busItem.name << "id=" << busItem.objectID;
        return false;
    }

    const QJsonObject document = ProjectDocument::create(
        QStringLiteral("project.bus"),
        busItem.guid.toString(QUuid::WithoutBraces), projectId, content,
        busItem.documentHeader);
    const QByteArray bytes = QJsonDocument(document).toJson(QJsonDocument::Indented);
    if (!writePackAtomically(filePath, bytes)) {
        qWarning() << "[BussesPack] Short write to:" << filePath;
        return false;
    }
    return true;
}

bool saveBussesPacksImpl(const QString& projectRoot,
                              ProjectData& project,
                              ProjectSerializer& serializer,
                              QString* errOut)
{
    QDir dir(busFolderAbs(projectRoot));
    if (!dir.exists() && !QDir().mkpath(dir.absolutePath())) {
        if (errOut) *errOut = QStringLiteral("Cannot create Busses folder");
        return false;
    }

    QSet<QString> writtenNames;

    int wrote = 0;
    bool allOk = true;

    std::function<void(const ProjectItem*)> walkAndSave =
        [&](const ProjectItem* node)
    {
        if (!node) return;
        if (node->type == ProjectItem::Type::Bus) {
            const QString fileName = busPackFileName(*node);
            writtenNames.insert(fileName);
            if (saveOneBusPack(projectRoot, project.projectId, *node, serializer)) {
                project.setObjectFileLocation(node, nullptr, dir.filePath(fileName));
                ++wrote;
            } else {
                allOk = false;
            }
        }
        for (const auto& child : node->children) walkAndSave(child.get());
    };
    if (!project.busses.empty() && project.busses[0].rootItem)
        walkAndSave(project.busses[0].rootItem.get());

    const QFileInfoList existing =
        dir.entryInfoList(QStringList() << QStringLiteral("*.vp"), QDir::Files);
    for (const QFileInfo& fi : existing) {
        const QString name = fi.fileName();
        if (writtenNames.contains(name)) continue;

        QFile probe(fi.absoluteFilePath());
        if (!probe.open(QIODevice::ReadOnly)) continue;
        const QJsonDocument document = QJsonDocument::fromJson(probe.readAll());
        probe.close();
        const bool looksLikeOurs = document.isObject()
            && document.object().value(QStringLiteral("harkenDocument")).toObject()
                   .value(QStringLiteral("type")).toString()
                == QStringLiteral("project.bus");
        if (looksLikeOurs) {
            if (!QFile::remove(fi.absoluteFilePath())) {
                qWarning() << "[BussesPack] Failed to prune stale bus file:" << fi.absoluteFilePath();
            } else {
                qDebug() << "[BussesPack] Pruned stale per-bus Pack:" << name;
            }
        }
    }

    qDebug() << "[BussesPack] saveBussesPacks wrote" << wrote
             << "Bus files (allOk=" << allOk << ")";
    if (!allOk && errOut) *errOut = QStringLiteral("One or more bus Pack files failed to save");
    return allOk;
}

enum class LoadOneBusPackResult { Ok, Skip, Fail };

LoadOneBusPackResult loadOneBusPack(const QFileInfo& fileInfo,
                   ProjectItem& scratchRoot,
                   ProjectData& project,
                   ProjectSerializer& serializer,
                   std::unordered_map<uint32_t, uint32_t>& outParentEdges)
{
    QFile file(fileInfo.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "[BussesPack] open failed:" << fileInfo.absoluteFilePath();
        return LoadOneBusPackResult::Fail;
    }
    const QByteArray bytes = file.readAll();
    file.close();

    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &perr);
    if (doc.isNull() || !doc.isObject()) {
        qWarning() << "[BussesPack] JSON parse failed for"
                   << fileInfo.absoluteFilePath() << ":" << perr.errorString();
        return LoadOneBusPackResult::Fail;
    }
    ProjectDocumentData document;
    QString documentError;
    if (!ProjectDocument::parse(doc.object(), QStringLiteral("project.bus"),
                                &document, &documentError)) {
        qWarning() << "[BussesPack]" << documentError << "in"
                   << fileInfo.absoluteFilePath();
        return LoadOneBusPackResult::Fail;
    }
    const QJsonObject root = document.content;
    if (root.value(QStringLiteral("type")).toString() != QStringLiteral("Bus"))
        return LoadOneBusPackResult::Skip;

    ProjectItem* item = serializer.busPackFromJson(
        root, scratchRoot, outParentEdges);
    if (!item) {
        qWarning() << "[BussesPack] Serializer rejected payload:"
                   << fileInfo.absoluteFilePath();
        return LoadOneBusPackResult::Fail;
    }
    item->documentHeader = document.header;
    project.setObjectFileLocation(item, nullptr, fileInfo.absoluteFilePath());
    return LoadOneBusPackResult::Ok;
}

bool loadBussesPacksImpl(const QString& projectRoot,
                              ProjectData& project,
                              ProjectSerializer& serializer,
                              QString* errOut)
{
    project.busses.clear();

    QDir dir(busFolderAbs(projectRoot));
    if (!dir.exists()) {
        return true;
    }

    const QFileInfoList files =
        dir.entryInfoList(QStringList() << QStringLiteral("*.vp"), QDir::Files);

    auto scratchRoot = std::make_shared<ProjectItem>(
        QStringLiteral("__bus_scratch__"), ProjectItem::Type::None);

    std::unordered_map<uint32_t, uint32_t> parentEdges;

    int loaded = 0;
    int failed = 0;
    for (const QFileInfo& fi : files) {
        const LoadOneBusPackResult result =
            loadOneBusPack(fi, *scratchRoot, project, serializer, parentEdges);
        if (result == LoadOneBusPackResult::Fail) {
            if (errOut) {
                *errOut = QStringLiteral("Rejected bus Pack file: %1")
                              .arg(fi.absoluteFilePath());
            }
            return false;
        }
        if (result == LoadOneBusPackResult::Ok)
            ++loaded;
        else
            ++failed;
    }

    auto isEdgeRoot = [&parentEdges](const ProjectItem* p) -> bool {
        return p && parentEdges.find(p->objectID) == parentEdges.end();
    };

    std::unordered_map<uint32_t, std::shared_ptr<ProjectItem>> byObjectID;
    QSet<QString> seenGuids;
    std::shared_ptr<ProjectItem> masterPtr;
    int rootCount = 0;
    for (const auto& child : scratchRoot->children) {
        if (!child) continue;
        if (!child->comp<VNX::VBus>()) continue;
        if (child->objectID == 0 || !byObjectID.emplace(child->objectID, child).second) {
            ++failed;
            if (errOut)
                *errOut = QStringLiteral("Busses contains a missing or duplicate object ID");
            continue;
        }
        const QString guid = child->guid.toString(QUuid::WithoutBraces);
        if (child->guid.isNull() || seenGuids.contains(guid)) {
            ++failed;
            if (errOut)
                *errOut = QStringLiteral("Busses contains a missing or duplicate guid");
            continue;
        }
        seenGuids.insert(guid);
        if (isEdgeRoot(child.get())) {
            ++rootCount;
            if (!masterPtr) masterPtr = child;
        }
    }

    std::function<void(const ProjectItem*)> walkMaxObjId =
        [&](const ProjectItem* node)
    {
        if (!node) return;
        if (node->objectID >= project.nextObjectID)
            project.nextObjectID = node->objectID + 1;
        for (const auto& child : node->children) walkMaxObjId(child.get());
    };
    walkMaxObjId(scratchRoot.get());
    if (project.nextObjectID == 0) project.nextObjectID = 1;

    qDebug() << "[BussesPack] loadBussesPacks scanned" << files.size()
              << "files, loaded" << loaded << "Bus Packs;"
              << "rootCount=" << rootCount
              << "; nextObjectID=" << project.nextObjectID;

    if (loaded != files.size() || failed > 0) {
        if (errOut)
            *errOut = QStringLiteral("Busses contains %1 unreadable or invalid Pack file(s)")
                          .arg(files.size() - loaded + failed);
        return false;
    }

    if (rootCount != 1) {
        if (errOut)
            *errOut = QStringLiteral(
                "Busses has %1 root Bus Packs; exactly 1 expected.")
                .arg(rootCount);
        return false;
    }

    if (masterPtr) {
        std::vector<std::shared_ptr<ProjectItem>> packs(
            scratchRoot->children.begin(), scratchRoot->children.end());
        scratchRoot->children.clear();
        for (auto& pack : packs) {
            if (pack.get() == masterPtr.get()) continue;
            auto eit = parentEdges.find(pack->objectID);
            const uint32_t parentObjId = (eit != parentEdges.end()) ? eit->second : 0u;
            auto pit = byObjectID.find(parentObjId);
            const bool resolved =
                (pit != byObjectID.end() && pit->second.get() != pack.get());
            if (!resolved) {
                qWarning() << "[BussesPack] Bus" << pack->name
                           << "has unresolved parent objectID" << parentObjId;
                if (errOut)
                    *errOut = QStringLiteral("Busses contains an unresolved parent object ID");
                return false;
            }
            ProjectItem* parentItem = pit->second.get();
            pack->parent = parentItem;
            parentItem->children.push_back(pack);
        }
        masterPtr->parent = nullptr;

        ProjectFileBinding binding;
        binding.parentFolder = QStringLiteral("Busses");
        binding.rootItem     = masterPtr;
        project.busses.push_back(std::move(binding));
    }

    return true;
}

int countRootBuses(const ProjectData& project)
{
    if (!project.busses.empty() && project.busses[0].rootItem) {
        return 1;
    }
    return 0;
}

}

// System-seeding path: mutates a raw ProjectData that has not yet been
// installed as currentProject_, so it deliberately bypasses createItem's
// name validation, uniqueness check, undo stack, and lifecycle signals.
// The engine-side bus is materialized later when syncProjectToRuntime
// walks the tree via the T3 runtime hooks.
void ensureDefaultMasterBus(ProjectData& project)
{
    if (!project.busses.empty() && project.busses[0].rootItem) {
        return;
    }
    project.busses.clear();

    auto masterItem = std::make_shared<ProjectItem>(
        QStringLiteral("Master Bus"), ProjectItem::Type::Bus);
    masterItem->attachDefaultComponents();
    masterItem->objectID = project.generateObjectID();
    masterItem->guid = QUuid::createUuid();

    ProjectFileBinding binding;
    binding.parentFolder = QStringLiteral("Busses");
    binding.rootItem     = masterItem;
    project.setObjectFileLocation(masterItem.get(), nullptr, {});
    project.busses.push_back(std::move(binding));
}

namespace {

QString layersFolderAbs(const QString& projectRoot)
{
    return QDir(projectRoot).filePath(ItemTypeCatalog::kLayersFolderName);
}

QString layerPackFileName(const ProjectItem& layer)
{
    return NameValidator::objectFileName(layer.name, layer.guid);
}

bool isLayersBucketRootType(ProjectItem::Type t)
{
    return ItemTypeCatalog::bucketOf(t) == ItemTypeCatalog::Bucket::Layers;
}

bool saveOneLayerPack(const QString& directory,
                          const QString& projectId,
                          const ProjectItem& layer,
                          const QUuid& parentGuid,
                          int order,
                          ProjectSerializer& serializer)
{
    if (!isLayersBucketRootType(layer.type)) {
        qWarning() << "[LayersPack] saveOneLayerPack called on type not in"
                   << "Layers bucket:" << layer.name
                   << "type=" << static_cast<int>(layer.type);
        return false;
    }

    QDir dir(directory);
    if (!dir.exists()) {
        qWarning() << "[LayersPack] Directory does not exist:" << dir.absolutePath();
        return false;
    }

    const QString filePath = dir.filePath(layerPackFileName(layer));

    QJsonObject data = serializer.audioItemToJson(layer);
    if (data.isEmpty()) {
        qWarning() << "[LayersPack] Serializer rejected Layer payload for"
                   << filePath;
        return false;
    }
    if (!parentGuid.isNull()) {
        ParentRef pr;
        pr.kind = ParentRef::Kind::Item;
        pr.guid = parentGuid;
        pr.documentId = parentGuid;
        data["parentRef"] = pr.toJson();
    }
    data["order"] = order;

    const QJsonObject document = ProjectDocument::create(
        QStringLiteral("project.layer"),
        layer.guid.toString(QUuid::WithoutBraces), projectId, data,
        layer.documentHeader);
    const QByteArray bytes = QJsonDocument(document).toJson(QJsonDocument::Indented);
    if (!writePackAtomically(filePath, bytes)) {
        qWarning() << "[LayersPack] Short write to:" << filePath;
        return false;
    }
    return true;
}

void saveLayerRecursive(const QString& bucketDirectory,
                             const QString& projectId,
                             ProjectData& project,
                             const ProjectItem& node,
                             const QUuid& parentGuid,
                             int order,
                             ProjectSerializer& serializer,
                             QSet<QString>& writtenAbsPaths,
                             int& wroteCount,
                             bool& allOk)
{
    if (!isLayersBucketRootType(node.type)) return;

    const QString directory = QDir(bucketDirectory).filePath(project.itemFolderPath(&node));
    const QString filePath = QDir(directory).filePath(layerPackFileName(node));
    writtenAbsPaths.insert(BucketFolderStore::pathComparisonKey(
        filePath));
    if (saveOneLayerPack(directory, projectId, node, parentGuid, order, serializer)) {
        project.setObjectFileLocation(&node, project.objectFolder(&node), filePath);
        ++wroteCount;
    } else {
        allOk = false;
    }

    for (size_t i = 0; i < node.children.size(); ++i) {
        const auto& child = node.children[i];
        if (!child) continue;
        saveLayerRecursive(bucketDirectory, projectId, project, *child, node.guid, static_cast<int>(i),
                                serializer, writtenAbsPaths, wroteCount, allOk);
    }
}

bool saveLayersBucket(const QString& projectRoot,
                              const QString& projectId,
                              ProjectData& project,
                              std::vector<ProjectFileBinding>& bucket,

                              ProjectSerializer& serializer,
                              QString* errOut)
{
    QDir dir(layersFolderAbs(projectRoot));
    if (!dir.exists() && !QDir().mkpath(dir.absolutePath())) {
        if (errOut) *errOut = QStringLiteral("Cannot create Layers folder");
        return false;
    }

    QSet<QString> writtenAbsPaths;
    int wrote = 0;
    bool allOk = true;

    int rootOrder = 0;
    for (auto& binding : bucket) {
        const auto& layer = binding.rootItem;
        if (!layer || !isLayersBucketRootType(layer->type)) continue;
        if (layer->parentRef.kind != ParentRef::Kind::Root) continue;
        saveLayerRecursive(dir.absolutePath(), projectId, project, *layer, QUuid{}, rootOrder++, serializer,
                                writtenAbsPaths, wrote, allOk);
    }

    QDirIterator existing(dir.absolutePath(), QStringList() << QStringLiteral("*.vp"),
                          QDir::Files, QDirIterator::Subdirectories);
    while (allOk && existing.hasNext()) {
        existing.next();
        const QFileInfo fi = existing.fileInfo();
        const QString name = fi.fileName();
        if (writtenAbsPaths.contains(BucketFolderStore::pathComparisonKey(fi.absoluteFilePath())))
            continue;

        QFile probe(fi.absoluteFilePath());
        if (!probe.open(QIODevice::ReadOnly)) continue;
        const QJsonDocument document = QJsonDocument::fromJson(probe.readAll());
        probe.close();
        const bool looksLikeOurs = document.isObject()
            && document.object().value(QStringLiteral("harkenDocument")).toObject()
                   .value(QStringLiteral("type")).toString()
                == QStringLiteral("project.layer");
        if (looksLikeOurs) {
            if (!QFile::remove(fi.absoluteFilePath())) {
                allOk = false;
                qWarning() << "[LayersPack] Failed to prune stale Layers bucket file:"
                           << fi.absoluteFilePath();
            } else {
                qDebug() << "[LayersPack] Pruned stale Layers bucket Pack:" << name;
            }
        }
    }

    qDebug() << "[LayersPack] saveLayersPacks wrote" << wrote
             << "Layer files (allOk=" << allOk << ")";
    if (!allOk && errOut) *errOut = QStringLiteral("One or more Layer Pack files failed to save");
    return allOk;
}

bool loadOneLayerPack(const QFileInfo& fileInfo,
                          ProjectSerializer& serializer,
                          std::shared_ptr<ProjectItem>& outLayer,
                          QUuid& outParentGuid,
                          bool& outIsRoot,
                          int& outOrder)
{
    outLayer.reset();
    outParentGuid = QUuid{};
    outIsRoot = false;
    outOrder = std::numeric_limits<int>::max();

    QFile file(fileInfo.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "[LayersPack] open failed:" << fileInfo.absoluteFilePath();
        return false;
    }
    const QByteArray bytes = file.readAll();
    file.close();

    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &perr);
    if (doc.isNull() || !doc.isObject()) {
        qWarning() << "[LayersPack] JSON parse failed for"
                   << fileInfo.absoluteFilePath() << ":" << perr.errorString();
        return false;
    }
    ProjectDocumentData document;
    QString documentError;
    if (!ProjectDocument::parse(doc.object(), QStringLiteral("project.layer"),
                                &document, &documentError)) {
        qWarning() << "[LayersPack]" << documentError << "in"
                   << fileInfo.absoluteFilePath();
        return false;
    }
    const QJsonObject root = document.content;

    const auto headerType = ItemTypeCatalog::parseTypeName(
        root.value(QStringLiteral("type")).toString(), Qt::CaseSensitive, false);
    if (!headerType || !isLayersBucketRootType(*headerType)) {
        qWarning() << "[LayersPack] Skipping non-Layers-bucket Pack (type="
                   << root.value(QStringLiteral("type")).toString() << "):"
                   << fileInfo.absoluteFilePath();
        return false;
    }

    const QJsonObject dataObj = root;
    if (dataObj.isEmpty()) {
        qWarning() << "[LayersPack] Missing or empty `data` payload in"
                   << fileInfo.absoluteFilePath();
        return false;
    }

    auto layer = std::make_shared<ProjectItem>(QString{}, *headerType);
    if (!serializer.audioPackItemFromJson(dataObj, *layer)) {
        qWarning() << "[LayersPack] audioItemFromJson failed for"
                   << fileInfo.absoluteFilePath();
        return false;
    }

    layer->documentHeader = document.header;
    const auto payloadType = ItemTypeCatalog::parseTypeName(
        dataObj.value(QStringLiteral("type")).toString(),
        Qt::CaseSensitive, false);
    if (!payloadType || !isLayersBucketRootType(*payloadType)) {
        qWarning() << "[LayersPack] Bucket root payload type"
                   << dataObj.value(QStringLiteral("type")).toString()
                   << "is not a Layers bucket root in"
                   << fileInfo.absoluteFilePath() << "-- rejecting.";
        return false;
    }
    if (!root.value(QStringLiteral("guid")).isString()) {
        qWarning() << "[LayersPack] Missing Layer guid in"
                   << fileInfo.absoluteFilePath();
        return false;
    }
    layer->guid = QUuid::fromString(root.value(QStringLiteral("guid")).toString());
    if (layer->guid.isNull()) {
        qWarning() << "[LayersPack] Invalid Layer guid in"
                   << fileInfo.absoluteFilePath();
        return false;
    }
    outOrder = root.value(QStringLiteral("order"))
                   .toInt(std::numeric_limits<int>::max());

    const auto ref = ParentRef::fromJson(root.value(QStringLiteral("parentRef")).toObject());
    if (!ref) {
        qWarning() << "[LayersPack] Unknown parentRef.kind in"
                   << fileInfo.absoluteFilePath() << "-- rejecting.";
        return false;
    }
    if (ref->isRoot()) {
        outIsRoot = true;
    } else if (ref->kind == ParentRef::Kind::Item) {
        outParentGuid = ref->guid;
        if (outParentGuid.isNull()) {
            qWarning() << "[LayersPack] item parentRef missing/invalid id in"
                       << fileInfo.absoluteFilePath();
            return false;
        }
    } else {
        qWarning() << "[LayersPack] Unexpected parentRef.kind in"
                   << fileInfo.absoluteFilePath();
        return false;
    }

    if (outIsRoot) {
        layer->parentRef = ParentRef{};
        layer->parentRef.kind = ParentRef::Kind::Root;
    } else {
        layer->parentRef.kind = ParentRef::Kind::Item;
        layer->parentRef.guid = outParentGuid;
    }

    outLayer = layer;
    return true;
}

bool loadLayersPacksImpl(const QString& projectRoot,
                                ProjectData& project,
                                ProjectSerializer& serializer,
                                QString* errOut,
                                int* failedOut = nullptr)
{
    const QString rootDir = layersFolderAbs(projectRoot);
    if (!QDir(rootDir).exists()) {
        return true;
    }

    auto folders = BucketFolderStore::scanFolderTree(
        rootDir, QString::fromLatin1(ItemTypeCatalog::kLayersFolderName));
    project.folders.insert(project.folders.end(),
                           std::make_move_iterator(folders.begin()),
                           std::make_move_iterator(folders.end()));

    struct LoadedLayer
    {
        std::shared_ptr<ProjectItem> layer;
        QUuid parentGuid;
        bool isRoot = false;
        int order = std::numeric_limits<int>::max();
    };

    QFileInfoList files;
    QDirIterator iterator(rootDir, QStringList() << QStringLiteral("*.vp"),
                          QDir::Files, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        files.push_back(iterator.fileInfo());
    }

    std::vector<LoadedLayer> loaded;
    loaded.reserve(static_cast<size_t>(files.size()));
    QHash<QString, ProjectItem*> guidToItem;
    guidToItem.reserve(files.size() * 2);

    int failed = 0;
    for (const QFileInfo& fi : files) {
        LoadedLayer ll;
        if (!loadOneLayerPack(fi, serializer, ll.layer, ll.parentGuid, ll.isRoot,
                              ll.order)) {
            ++failed;
            continue;
        }
        if (!ll.layer) {
            ++failed;
            continue;
        }
        const QString key = ll.layer->guid.toString(QUuid::WithoutBraces);
        if (guidToItem.contains(key)) {
            qWarning() << "[LayersPack] Duplicate Layer GUID" << key
                       << "in" << fi.absoluteFilePath();
            ++failed;
            continue;
        }
        project.setObjectFileLocation(ll.layer.get(), project.findFolderByPath(
            QString::fromLatin1(ItemTypeCatalog::kLayersFolderName),
            QDir(rootDir).relativeFilePath(fi.absolutePath())), fi.absoluteFilePath());
        guidToItem.insert(key, ll.layer.get());
        loaded.push_back(std::move(ll));
    }

    int rootCount = 0;
    int childCount = 0;
    int orphanCount = 0;
    for (auto& ll : loaded) {
        if (ll.isRoot) {
            ll.layer->parent = nullptr;
            ++rootCount;
            continue;
        }
        const QString key = ll.parentGuid.toString(QUuid::WithoutBraces);
        auto it = guidToItem.find(key);
        if (it != guidToItem.end()) {
            ProjectItem* parent = it.value();
            ll.layer->parent = parent;
            parent->children.push_back(ll.layer);
            ++childCount;
        } else {
            qWarning() << "[LayersPack] Orphan Layer" << ll.layer->name
                       << "guid=" << ll.layer->guid.toString(QUuid::WithoutBraces)
                       << "missing parent_guid=" << key;
            ++orphanCount;
        }
    }

    QHash<const ProjectItem*, int> orderOf;
    orderOf.reserve(static_cast<int>(loaded.size()));
    for (const auto& ll : loaded)
        orderOf.insert(ll.layer.get(), ll.order);
    const auto byOrder = [&orderOf](const std::shared_ptr<ProjectItem>& a,
                                    const std::shared_ptr<ProjectItem>& b) {
        return orderOf.value(a.get(), std::numeric_limits<int>::max())
             < orderOf.value(b.get(), std::numeric_limits<int>::max());
    };
    for (auto& ll : loaded) {
        std::stable_sort(ll.layer->children.begin(), ll.layer->children.end(),
                         byOrder);
    }

    for (auto& ll : loaded) {
        rebindOwner(*ll.layer, ll.layer->parent);
    }

    std::vector<LoadedLayer*> rootPacks;
    rootPacks.reserve(loaded.size());
    for (auto& ll : loaded)
        if (ll.isRoot) rootPacks.push_back(&ll);
    std::stable_sort(rootPacks.begin(), rootPacks.end(),
                     [](const LoadedLayer* a, const LoadedLayer* b) {
                         return a->order < b->order;
                     });

    for (LoadedLayer* ll : rootPacks) {
        ProjectFileBinding binding;
        binding.parentFolder = QString::fromLatin1(ItemTypeCatalog::kLayersFolderName);
        binding.rootItem     = std::move(ll->layer);
        project.layers.push_back(std::move(binding));
    }

    qDebug() << "[LayersPack] loadLayersPacks scanned" << files.size()
             << "files, loaded" << loaded.size() << "Layers"
             << "(roots=" << rootCount << ", children=" << childCount
             << ", orphans=" << orphanCount << ")";

    if (failedOut)
        *failedOut = failed + orphanCount;
    if (failed > 0) {
        if (errOut)
            *errOut = QStringLiteral("Layers contains %1 unreadable or invalid Pack file(s)")
                          .arg(failed);
        return false;
    }
    if (orphanCount > 0) {
        if (errOut)
            *errOut = QStringLiteral("Layers has %1 orphan Layer(s) -- parent_guid not found.")
                          .arg(orphanCount);
        return false;
    }
    return true;
}

bool validateLoadedProjectIdentities(const ProjectData& project, QString* error)
{
    if (!validateProjectObjectIdentities(project, error))
        return false;
    QSet<QString> guids;
    QSet<uint32_t> objectIds;
    QSet<const ProjectItem*> visited;
    uint32_t maxObjectId = 0;
    QSet<harkenstudio::ObjectId> actionRuntimeIds;

    std::function<bool(const ProjectItem*, const ProjectItem*)> visit =
        [&](const ProjectItem* item, const ProjectItem* expectedParent) {
            if (!item || visited.contains(item)) {
                if (error) *error = QStringLiteral("Project contains a null or cyclic item");
                return false;
            }
            visited.insert(item);
            if (item->parent != expectedParent) {
                if (error) *error = QStringLiteral("Project item parent links are inconsistent");
                return false;
            }
            const QString guid = item->guid.toString(QUuid::WithoutBraces);
            if (item->guid.isNull() || guids.contains(guid)) {
                if (error) *error = QStringLiteral("Project contains a missing or duplicate guid");
                return false;
            }
            guids.insert(guid);
            if (item->objectID == 0 || objectIds.contains(item->objectID)) {
                if (error) *error = QStringLiteral("Project contains a missing or duplicate object ID");
                return false;
            }
            objectIds.insert(item->objectID);
            maxObjectId = std::max(maxObjectId, item->objectID);
            if (item->type == ProjectItem::Type::Action)
                if (!harkenstudio::IsNull(item->runtimeId()))
                    actionRuntimeIds.insert(item->runtimeId());
            for (const auto& child : item->children) {
                if (!visit(child.get(), item)) return false;
            }
            return true;
        };

    if (project.root && !visit(project.root.get(), nullptr)) return false;
    for (const ProjectFileBinding* binding : project.allFileBindings()) {
        if (!binding || !binding->rootItem) {
            if (error) *error = QStringLiteral("Project contains an empty file binding");
            return false;
        }
        if (!visit(binding->rootItem.get(), nullptr)) return false;
    }

    if (project.nextObjectID == 0 || project.nextObjectID <= maxObjectId) {
        if (error) *error = QStringLiteral("Project identity counters are behind loaded data");
        return false;
    }
    for (const PackGroup& group : project.packGroups) {
        for (const harkenstudio::ObjectId id : group.actionIds) {
            if (harkenstudio::IsNull(id) || !actionRuntimeIds.contains(id)) {
                if (error) *error = QStringLiteral("Project pack group references an unknown action");
                return false;
            }
        }
    }
    return true;
}



}

PreparedProject::PreparedProject() = default;

PreparedProject::PreparedProject(PreparedProject&& other) noexcept
    : instance(std::move(other.instance)),
      project(std::move(other.project)),
      folderSaveState(std::move(other.folderSaveState)),
      index(std::move(other.index)),
      integrityScanner(std::move(other.integrityScanner)),
      integrityReport(std::move(other.integrityReport)),
      assets(std::move(other.assets)),
      diagnostics(std::move(other.diagnostics)),
      error(std::move(other.error)),
      rebuildAssetRegistry(other.rebuildAssetRegistry),
      audioLoadFailures(other.audioLoadFailures),
      upgrade(std::move(other.upgrade)),
      upgradePending(other.upgradePending)
{
    other.upgradePending = false;
}

PreparedProject& PreparedProject::operator=(PreparedProject&& other) noexcept
{
    if (this == &other)
        return *this;
    rollbackUpgrade();
    project = std::move(other.project);
    folderSaveState = std::move(other.folderSaveState);
    index = std::move(other.index);
    integrityScanner = std::move(other.integrityScanner);
    integrityReport = std::move(other.integrityReport);
    assets = std::move(other.assets);
    diagnostics = std::move(other.diagnostics);
    error = std::move(other.error);
    rebuildAssetRegistry = other.rebuildAssetRegistry;
    audioLoadFailures = other.audioLoadFailures;
    upgrade = std::move(other.upgrade);
    upgradePending = other.upgradePending;
    other.upgradePending = false;
    instance = std::move(other.instance);
    return *this;
}

PreparedProject::~PreparedProject()
{
    rollbackUpgrade();
}

void PreparedProject::rollbackUpgrade()
{
    if (!upgradePending)
        return;
    QString rollbackError;
    if (!ProjectUpgradeManager().rollback(upgrade, &rollbackError))
        qCritical() << "[ProjectManager] Failed to roll back uncommitted project upgrade:"
                    << rollbackError;
    upgradePending = false;
}

ProjectManager& ProjectManager::instance()
{
    static ProjectManager instance;
    return instance;
}

ProjectManager::ProjectManager(QObject* parent)
    : QObject(parent)
{
    connect(&m_fileWatcher, &FileWatcher::filesAdded, this, &ProjectManager::externalFilesAdded);
    connect(&m_fileWatcher, &FileWatcher::filesRemoved, this, &ProjectManager::externalFilesRemoved);
    connect(&m_fileWatcher, &FileWatcher::filesModified, this, &ProjectManager::externalFilesModified);
    connect(&m_fileWatcher, &FileWatcher::packExternallyModified, this, &ProjectManager::packExternallyModified);
    const auto refreshGroups = [this] {
        if (m_currentProject) refreshFieldGroupDerivedViews(*m_currentProject);
    };
    connect(this, &ProjectManager::itemCreated, this, refreshGroups);
    connect(this, &ProjectManager::itemCreated, this, &ProjectManager::recordCreatedNameHistory);
    connect(this, &ProjectManager::itemMoved, this, refreshGroups);
    connect(this, &ProjectManager::itemDeleted, this, [this](ProjectItem* item) {
        recordDeletedNameHistory(item);
    });
    initializeRuntimeSync();
    connect(&UndoRedoManager::instance(), &UndoRedoManager::indexChanged, this, [this] {
        if (!m_currentProject || !m_itemSaveState) return;
        for (auto* item : m_itemSaveState->refresh(*m_currentProject))
            emit itemPropertyChanged(item, QStringLiteral("dirty"));
    });
}

ProjectManager::~ProjectManager() = default;

void ProjectManager::setStartupProgressCallback(
    std::function<void(StartupPhase)> callback)
{
    m_startupProgressCallback = std::move(callback);
}

QStringList ProjectManager::getStandardDirectories()
{
    QStringList dirs;
    for (ItemTypeCatalog::Bucket bucket : ItemTypeCatalog::allBuckets())
        dirs << ItemTypeCatalog::folderNameOf(bucket);
    return dirs;
}

QString ProjectManager::bucketDisplayName(const QString& folderName)
{
    return ItemTypeCatalog::bucketDisplayName(folderName);
}

static QStringList getDirectoryAliases(const QString& name)
{
    return {name};
}

static bool canProjectItemAcceptChildren(const ProjectItem* parent)
{
    return parent && ItemTypeCatalog::canAcceptAnyChild(parent->type);
}

bool ProjectManager::canAcceptImportedAudioParent(const ProjectItem* parent)
{
    return parent && ItemTypeCatalog::acceptsImportedAudio(parent->type);
}

bool ProjectManager::canBeChildOf(ProjectItem::Type childType, ProjectItem::Type parentType)
{
    return ItemTypeCatalog::canBeChildOf(childType, parentType);
}

bool ProjectManager::canAcceptAnyChild(ProjectItem::Type parentType)
{
    return ItemTypeCatalog::canAcceptAnyChild(parentType);
}

std::vector<ProjectItem::Type> ProjectManager::allowedChildTypesOf(ProjectItem::Type parentType)
{
    return ItemTypeCatalog::allowedChildTypesOf(parentType);
}

std::vector<ProjectItem::Type> ProjectManager::allowedTopLevelTypesForBucket(const QString& bucketFolderName)
{
    return ItemTypeCatalog::allowedTopLevelTypesForBucket(bucketFolderName);
}

bool ProjectManager::bucketAllowsFolder(const QString& bucketFolderName)
{
    return ItemTypeCatalog::bucketAllowsFolder(bucketFolderName);
}

ProjectManager::NameNamespace ProjectManager::namespaceForType(ProjectItem::Type type)
{
    return ItemTypeCatalog::nameNamespaceOf(type);
}
bool ProjectManager::isNameUnique(const QString& name,
                                  ProjectItem::Type type,
                                  const ProjectItem* excludeItem) const
{
    return findNameConflict(name, type, excludeItem) == nullptr;
}

const ProjectItem* ProjectManager::findNameConflict(const QString& name,
                                                    ProjectItem::Type type,
                                                    const ProjectItem* excludeItem) const
{
    if (!m_currentProject) return nullptr;
    const NameNamespace ns = namespaceForType(type);
    if (ns != NameNamespace::Action && ns != NameNamespace::Variable) return nullptr;

    const ProjectItem* conflict = nullptr;
    std::function<void(const ProjectItem*)> walk = [&](const ProjectItem* item) {
        if (conflict || !item) return;
        if (item != excludeItem
            && namespaceForType(item->type) == ns
            && item->name == name) {
            conflict = item;
            return;
        }
        for (const auto& child : item->children) {
            if (conflict) return;
            walk(child.get());
        }
    };

    for (auto* binding : m_currentProject->allFileBindings()) {
        if (conflict) break;
        if (!binding || !binding->rootItem) continue;
        walk(binding->rootItem.get());
    }
    return conflict;
}

QString ProjectManager::nameConflictMessage(const QString& name,
                                            ProjectItem::Type type,
                                            const ProjectItem& conflict)
{
    return QStringLiteral("The name \"%1\" is already used by the %2 \"%3\". "
                          "%4 must all have different names.")
               .arg(name,
                    ItemTypeCatalog::displayName(conflict.type),
                    conflict.objectPath,
                    ItemTypeCatalog::nameNamespaceDisplayName(namespaceForType(type)));
}

QString ProjectManager::suggestUniqueName(ProjectItem::Type type,
                                          const QString& baseName,
                                          const ProjectItem* excludeItem,
                                          const QSet<QString>& reservedNames,
                                          const ProjectItem* parent,
                                          const QString& folderPath) const
{
    const auto ns = namespaceForType(type);
    if (ns == NameNamespace::None) return baseName;
    const bool bucketUnique = ns == NameNamespace::Action || ns == NameNamespace::Variable;
    const auto sensitivity = ns == NameNamespace::Attenuation ? Qt::CaseInsensitive : Qt::CaseSensitive;
    const auto siblingConflict = [&](const QString& candidate) {
        const auto conflicts = [&](const ProjectItem* item) {
            return item && item != excludeItem && namespaceForType(item->type) == ns
                && item->name.compare(candidate, sensitivity) == 0;
        };
        if (parent) {
            return std::any_of(parent->children.begin(), parent->children.end(),
                [&](const auto& child) { return conflicts(child.get()); });
        }
        if (!m_currentProject) return false;
        const auto* bucket = m_currentProject->getBucketForType(type);
        if (!bucket) return false;
        return std::any_of(bucket->begin(), bucket->end(), [&](const auto& binding) {
            return conflicts(binding.rootItem.get()) && !binding.rootItem->parent
                && (ns == NameNamespace::AudioNode || !ItemTypeCatalog::isFolderAddressable(type)
                    || m_currentProject->itemFolderPath(binding.rootItem.get())
                           .compare(folderPath, Qt::CaseInsensitive) == 0);
        });
    };
    return NameValidator::makeUniqueName(baseName.normalized(QString::NormalizationForm_C), [&](const QString& candidate) {
        const QString key = type == ProjectItem::Type::Attenuation ? candidate.toCaseFolded() : candidate;
        return !reservedNames.contains(key)
            && (bucketUnique ? isNameUnique(candidate, type, excludeItem) : !siblingConflict(candidate));
    });
}

bool ProjectManager::isFolderNameUniqueAmongSiblings(
    const PhysicalFolder* parentFolderItem,
    const QString& bucketFolder,
    const QString& name,
    const PhysicalFolder* excludeItem) const
{
    if (!m_currentProject) return true;

    const QString key = folderNameKey(name);
    if (parentFolderItem) {
        if (m_currentProject->folderBucket(parentFolderItem) != bucketFolder)
            return false;
        for (const auto& child : parentFolderItem->children) {
            if (child && child.get() != excludeItem
                && folderNameKey(child->name) == key) {
                return false;
            }
        }
        return true;
    }

    if (bucketFolder.isEmpty()) return false;
    for (const PhysicalFolder* root : m_currentProject->folderRootsForBucket(bucketFolder)) {
        if (root != excludeItem && folderNameKey(root->name) == key)
            return false;
    }
    return true;
}

QString ProjectManager::suggestUniqueFolderName(
    const PhysicalFolder* parentFolderItem,
    const QString& bucketFolder,
    const QString& baseName,
    const PhysicalFolder* excludeItem) const
{
    if (!m_currentProject || (parentFolderItem && m_currentProject->folderBucket(parentFolderItem) != bucketFolder)
        || (!parentFolderItem && bucketFolder.isEmpty())) return {};
    return NameValidator::makeUniqueName(baseName.normalized(QString::NormalizationForm_C), [&](const QString& candidate) {
        return isFolderNameUniqueAmongSiblings(parentFolderItem, bucketFolder, candidate, excludeItem);
    });
}

ProjectItem* ProjectManager::findDirectoryItem(const QString& dirName) const
{
    if (!m_currentProject || !m_currentProject->root) return nullptr;
    for (auto& child : m_currentProject->root->children) {
        if (child->name == dirName) return child.get();
        for (auto& wuChild : child->children) {
            if (wuChild->name == dirName) return wuChild.get();
        }
    }
    return nullptr;
}


bool ProjectManager::addProjectLanguage(const QString& languageName)
{
    if (!m_currentProject) return false;
    const QString name = languageName.trimmed();
    if (name.isEmpty()) return false;
    QStringList languages = m_currentProject->projectLanguages;
    if (!languages.contains(name))
        languages.append(name);
    const auto normalized = ProjectLanguageSemantics::normalize(languages);
    if (!normalized.ok()) return false;
    setProjectLanguagesInternal(normalized.value);
    return true;
}

bool ProjectManager::removeProjectLanguage(const QString& languageName)
{
    if (!m_currentProject) return false;
    int idx = m_currentProject->projectLanguages.indexOf(languageName);
    if (idx < 0) return false;
    QStringList languages = m_currentProject->projectLanguages;
    languages.removeAt(idx);
    const auto normalized = ProjectLanguageSemantics::normalize(languages);
    if (!normalized.ok()) return false;
    setProjectLanguagesInternal(normalized.value);
    if (m_currentProject->defaultLanguage == languageName)
        m_currentProject->defaultLanguage = m_currentProject->projectLanguages.value(0);
    return true;
}

QStringList ProjectManager::getProjectLanguages() const
{
    if (!m_currentProject) return {};
    return m_currentProject->projectLanguages;
}

void ProjectManager::setProjectLanguagesInternal(const QStringList& value)
{
    if (!m_currentProject) return;
    if (m_currentProject->projectLanguages == value) return;
    m_currentProject->projectLanguages = value;
    emit projectLanguagesChanged(value);
}

void ProjectManager::setDefaultLanguage(const QString& languageName)
{
    if (!m_currentProject) return;
    m_currentProject->defaultLanguage = languageName;
}

QString ProjectManager::getDefaultLanguage() const
{
    if (!m_currentProject) return {};
    return m_currentProject->defaultLanguage;
}


QString ProjectManager::packBuildOutputDir() const
{
    if (!m_currentProject) return {};
    return m_currentProject->packBuildOutputDir;
}

bool ProjectManager::setPackBuildOutputDir(const QString& value)
{
    if (!m_currentProject || !PathUtils::isValidPackBuildOutputDir(value, m_currentProject->projectPath))
        return false;
    setPackBuildOutputDirInternal(PathUtils::normalizePackBuildOutputDir(value, m_currentProject->projectPath));
    return true;
}

void ProjectManager::setPackBuildOutputDirInternal(const QString& value)
{
    if (!m_currentProject || m_currentProject->packBuildOutputDir == value) return;
    m_currentProject->packBuildOutputDir = value;
    emit packBuildOutputDirChanged(value);
}

void ProjectManager::setBuildProjectDataExportInternal(bool enabled)
{
    if (!m_currentProject) return;
    m_currentProject->buildExportProjectData = enabled;
}

void ProjectManager::setBuildPackMetadataExportInternal(bool enabled)
{
    if (!m_currentProject) return;
    m_currentProject->buildExportPackMetadata = enabled;
}

void ProjectManager::setBuildWaveformSettingsInternal(bool enabled, const QString& directory)
{
    if (!m_currentProject) return;
    m_currentProject->buildExportWaveforms = enabled;
    m_currentProject->buildWaveformOutputDir = directory;
}

PackExportSelection ProjectManager::packExportSelection() const
{
    return m_currentProject ? PackExportSelectionStore::instance().selection() : PackExportSelection{};
}

void ProjectManager::setPackExportSelectionInternal(PackExportSelection selection)
{
    if (!m_currentProject) return;
    const auto before = packExportSelection();
    PackExportSelectionStore::instance().setSelection(*m_currentProject, std::move(selection));
    const auto after = packExportSelection();
    if (before.currentPlatformId != after.currentPlatformId)
        emit packExportCurrentPlatformChanged(after.currentPlatformId);
    if (before.selectedPlatformIds != after.selectedPlatformIds)
        emit packExportSelectedPlatformsChanged(after.selectedPlatformIds);
    if (before.actionTargeting != after.actionTargeting)
        emit actionPackTargetingChanged();
}

QString ProjectManager::getPackExportCurrentPlatform() const
{
    return packExportSelection().currentPlatformId;
}

void ProjectManager::setPackExportCurrentPlatformInternal(const QString& value)
{
    auto selection = packExportSelection();
    selection.currentPlatformId = value;
    setPackExportSelectionInternal(std::move(selection));
}

QStringList ProjectManager::getPackExportSelectedPlatforms() const
{
    return packExportSelection().selectedPlatformIds;
}

void ProjectManager::setPackExportSelectedPlatformsInternal(const QStringList& value)
{
    auto selection = packExportSelection();
    selection.selectedPlatformIds = value;
    setPackExportSelectionInternal(std::move(selection));
}

std::vector<PackGroup> ProjectManager::getPackGroups() const
{
    if (!m_currentProject) return {};
    return m_currentProject->packGroups;
}

void ProjectManager::setPackGroupsInternal(std::vector<PackGroup> value)
{
    if (!m_currentProject) return;
    if (m_currentProject->packGroups == value) return;
    m_currentProject->packGroups = std::move(value);
    emit packGroupsChanged();
}

std::vector<ActionPackTargeting> ProjectManager::getActionPackTargeting() const
{
    return packExportSelection().actionTargeting;
}

void ProjectManager::setActionPackTargetingInternal(std::vector<ActionPackTargeting> value)
{
    auto selection = packExportSelection();
    selection.actionTargeting = std::move(value);
    setPackExportSelectionInternal(std::move(selection));
}

QStringList ProjectManager::getTargetPlatforms() const
{
    if (!m_currentProject) return QStringList();
    return m_currentProject->platformIds();
}

void ProjectManager::setProjectPlatformsInternal(const QVector<ProjectPlatform>& value)
{
    if (!m_currentProject) return;
    PropertyEditSession::finishAllActive(true);
    m_currentProject->platforms = value;
    AssetRegistry::instance().setPlatforms(value);
    emit targetPlatformsChanged(m_currentProject->platformIds());
}

void ProjectManager::notifyProjectPlatformEdit(const QVector<ProjectItem*>& changedItems, const QString& platformId)
{
    notifyProjectPlatformEdits(changedItems, {platformId});
}

void ProjectManager::notifyProjectPlatformEdits(const QVector<ProjectItem*>& changedItems, const QStringList& platformIds)
{
    if (!m_currentProject) return;
    AssetRegistry::instance().setPlatforms(m_currentProject->platforms);
    const auto primary = PlatformContextService::instance().context().primary;
    for (auto* item : ProjectPlatformSemantics::referenceOwners(*m_currentProject)) {
        if (item->comp<VSound>())
            SoundSourceEditing::syncActiveProjection(*item, primary,
                SoundSourceEditing::previewLanguage(*item, LanguageContextService::instance().language()));
    }
    for (const auto& id : platformIds) {
        emit projectPlatformEdited(id);
    }
    emit targetPlatformsChanged(m_currentProject->platformIds());
    emit packExportCurrentPlatformChanged(getPackExportCurrentPlatform());
    emit packExportSelectedPlatformsChanged(getPackExportSelectedPlatforms());
    emit packGroupsChanged();
    emit actionPackTargetingChanged();
    for (auto* item : changedItems) {
        emit itemPropertyChanged(item, QStringLiteral("platformOverrides"), nullptr);
        if (item->comp<VSound>()) emit itemPropertyChanged(item, QStringLiteral("sources"), nullptr);
        resyncItemToRuntime(item);
    }
    AudioEngine::instance().runtimeSyncCoordinator().markCurrentWorldMutation();
}

void ProjectManager::refreshSoundSourceProjections(ProjectPlatformId primary, const QString& language)
{
    if (!m_currentProject) return;
    QVector<ProjectItem*> changed;
    for (auto* item : ProjectPlatformSemantics::referenceOwners(*m_currentProject)) {
        if (SoundSourceEditing::syncActiveProjection(*item, primary,
                SoundSourceEditing::previewLanguage(*item, language)))
            changed.append(item);
    }
    for (auto* item : changed) {
        emit itemPropertyChanged(item, QStringLiteral("sources"), nullptr);
    }
}

static bool wouldCreateHierarchyCycle(const ProjectItem* item, const ProjectItem* newParent)
{
    if (!item || !newParent) return false;
    if (item == newParent) return true;

    const ProjectItem* cursor = newParent;
    while (cursor) {
        if (cursor == item) {
            return true;
        }
        cursor = cursor->parent;
    }
    return false;
}

static int countContainerAncestors(const ProjectItem* node)
{
    int depth = 0;
    const ProjectItem* cursor = node;
    while (cursor) {
        if (cursor->type == ProjectItem::Type::Layer) {
            ++depth;
        }
        cursor = cursor->parent;
    }
    return depth;
}

static int computeMaxContainerDepthAfterAttach(const ProjectItem* node, int baseDepth)
{
    if (!node) return baseDepth;

    const int currentDepth = baseDepth + (node->type == ProjectItem::Type::Layer ? 1 : 0);
    int maxDepth = currentDepth;
    for (const auto& child : node->children) {
        maxDepth = std::max(maxDepth, computeMaxContainerDepthAfterAttach(child.get(), currentDepth));
    }
    return maxDepth;
}

// Shared placement rule core for both creation and movement: the coarse
// type-level containment rules plus container-depth. `instance` is the item
// being placed (the fresh item on create, the moved subtree on move); it lets
// the depth check account for the subtree's own nesting. Structural per-bus
// rules (fan-out / send loops) stay in validateStructuralPlacement, called
// alongside this by the move/paste/create-bus paths. Fills *reason on failure
// when a caller wants to surface it (move/paste); create passes nullptr to
// fail silently.
static bool validatePlacement(ProjectItem::Type childType,
                              const ProjectItem* parent,
                              const ProjectItem* instance,
                              QString* reason)
{
    auto fail = [reason](const QString& message) {
        if (reason) *reason = message;
        return false;
    };

    if (!parent) {
        return fail(QStringLiteral("Placement failed: no target parent."));
    }

    if (!canProjectItemAcceptChildren(parent)) {
        return fail(QString("'%1' cannot contain child items.").arg(parent->name));
    }

    if (!ItemTypeCatalog::canBeChildOf(childType, parent->type)) {
        return fail(instance
            ? QString("'%1' cannot be placed under '%2'.").arg(instance->name, parent->name)
            : QString("This item cannot be placed under '%1'.").arg(parent->name));
    }

    const int baseContainerDepth = countContainerAncestors(parent);
    const int maxDepthAfterAttach = instance
        ? computeMaxContainerDepthAfterAttach(instance, baseContainerDepth)
        : baseContainerDepth + (childType == ProjectItem::Type::Layer ? 1 : 0);
    if (maxDepthAfterAttach > MAX_CONTAINER_DEPTH) {
        return fail(QString("Container nesting depth would exceed limit (%1).")
                        .arg(MAX_CONTAINER_DEPTH));
    }

    return true;
}

namespace {

TimelineValidation validateNewAuthoring(const ProjectManager& manager,
                                       const ProjectItem& item,
                                       const ProjectItem* parent)
{
    QHash<QUuid, const ProjectItem*> subtree;
    const auto collect = [&](auto&& self, const ProjectItem* node) -> void {
        if (!node || subtree.contains(node->guid)) return;
        subtree.insert(node->guid, node);
        for (const auto& child : node->children)
            self(self, child.get());
    };
    collect(collect, &item);
    const auto resolve = [&](const QUuid& guid) -> const ProjectItem* {
        const auto found = subtree.constFind(guid);
        return found == subtree.cend() ? manager.findItemByGuid(guid) : found.value();
    };
    const auto resolveObject = [&manager](uint32_t objectID) {
        return manager.findItemByObjectID(objectID);
    };
    const auto validity = AuthoringPolicy::validateNewSubtree(
        item, parent, resolve, resolveObject);
    if (!validity) return validity;
    return TimelineValidation::success();
}

bool validateProjectTemplate(const ProjectConfig& config, QString* error)
{
    if (config.templateId.isEmpty() || config.templateData.isEmpty()) return true;
    ProjectData candidate;
    candidate.platforms = config.platforms;
    ensureDefaultMasterBus(candidate);
    return TemplateApplyService::seedTemplateInto(candidate, config.templateData, error);
}

}

bool ProjectManager::validateMoveConstraints(const ProjectItem* item, const ProjectItem* newParent, QString* reason) const
{
    if (!item || !newParent) {
        if (reason) *reason = "Move failed: invalid item or target parent.";
        return false;
    }

    if (wouldCreateHierarchyCycle(item, newParent)) {
        if (reason) {
            *reason = QString("Move failed: '%1' cannot be moved into itself or its descendants.")
                          .arg(item->name);
        }
        return false;
    }

    if (!validatePlacement(item->type, newParent, item, reason))
        return false;
    const auto authoring = validateNewAuthoring(*this, *item, newParent);
    if (!authoring) {
        if (reason) *reason = authoring.message;
        return false;
    }

    return validateStructuralPlacement(item->type, newParent, reason, item);
}

static QString computeObjectPath(const ProjectItem* parent, const QString& name)
{
    if (!parent)
        return QStringLiteral("\\") + name;
    return parent->objectPath + QStringLiteral("\\") + name;
}

static void recomputeObjectPathRecursive(ProjectItem* item)
{
    if (!item) return;
    item->objectPath = computeObjectPath(item->parent, item->name);
    for (auto& child : item->children) {
        recomputeObjectPathRecursive(child.get());
    }
}

bool ProjectManager::createProject(const ProjectConfig& config)
{
    QString platformError;
    if (!ProjectPlatformSemantics::validatePlatforms(config.platforms, &platformError)) {
        emit errorOccurred(platformError);
        return false;
    }
    if (!validateProjectTemplate(config, &platformError)) {
        emit errorOccurred(platformError);
        return false;
    }
    drainPendingDestruction();

    QDir dir(config.location);
    QString projectRoot = dir.filePath(config.name);
    auto acquisition = ProjectInstance::acquire(
        QDir(projectRoot).filePath(config.name + QStringLiteral(".hsp")));
    if (acquisition.status != ProjectInstance::Status::Acquired) {
        emit errorOccurred(acquisition.status == ProjectInstance::Status::Forwarded
                               ? tr("This project is already open in another Studio window.")
                               : acquisition.error);
        return false;
    }

    if (!createDirectoryStructure(projectRoot))
    {
        emit errorOccurred("Failed to create project directory structure.");
        return false;
    }

    if (m_currentProject && !closeProject())
        return false;
    AssetRegistry::instance().setProjectRoot(projectRoot);
    qDebug() << "[ProjectManager] New project root set to:" << projectRoot;

    if (!createMainProjectFile(config, projectRoot))
    {
        emit errorOccurred("Failed to create main project file.");
        return false;
    }

    return loadProject(QDir(projectRoot).filePath(config.name + ".hsp"));
}

bool ProjectManager::createDirectoryStructure(const QString& projectRoot)
{
    QDir dir(projectRoot);
    if (dir.exists())
    {
    }
    else
    {
        if (!dir.mkpath(".")) return false;
    }

    for (const QString& subDir : getStandardDirectories())
    {
        if (!dir.mkpath(subDir)) return false;
    }

    return true;
}

bool ProjectManager::createMainProjectFile(const ProjectConfig& config, const QString& projectRoot)
{
    QString platformError;
    if (!ProjectPlatformSemantics::validatePlatforms(config.platforms, &platformError)) {
        emit errorOccurred(platformError);
        return false;
    }
    ProjectData project;
    project.name = config.name;
    project.author = config.author;
    project.description = config.description;
    project.createdDate = QDateTime::currentDateTime().toString(Qt::ISODate);
    project.audioFolder = kLayers;
    project.packFolder = "Packs";
    project.projectPath = QDir(projectRoot).filePath(config.name + ".hsp");

    project.platforms = config.platforms;
    project.sampleRate        = config.sampleRate;
    project.channelConfig     = config.channelConfig;
    project.enableLFE         = config.enableLFE;
    project.projectTemplateId = config.templateId;

    m_projectIndex = std::make_unique<VNX::ProjectIndex>();
    if (!m_projectIndex->loadFromCache(projectRoot)) {
        qDebug() << "[ProjectManager] ProjectIndex cache miss/stale -- rebuilding from disk";
    }
    const auto scanResult = m_projectIndex->scanProjectRoot(projectRoot);
    if (scanResult != VNX::ProjectIndex::ScanResult::Ok) {
        qWarning() << "[ProjectManager] ProjectIndex scan returned non-Ok:" << static_cast<int>(scanResult);
    }
    m_projectIndex->saveToCache(projectRoot);
    qDebug() << "[ProjectManager] ProjectIndex entries:" << m_projectIndex->entryCount();

    {
        QString busErr;
        loadBussesPacksImpl(projectRoot, project, m_serializer, &busErr);
        if (countRootBuses(project) == 0) {
            ensureDefaultMasterBus(project);
            saveBussesPacksImpl(projectRoot, project, m_serializer, nullptr);
        }
    }

    loadActions(projectRoot, project, m_serializer);

    for (const auto& spec : s3Specs()) {
        loadS3Hierarchy(projectRoot, spec, project, m_serializer);
    }

    loadLayersPacksImpl(projectRoot, project, m_serializer, nullptr);

    if (!config.templateId.isEmpty() && !config.templateData.isEmpty()) {
        QString tmplErr;
        if (!TemplateApplyService::seedTemplateInto(
                project, config.templateData, &tmplErr)) {
            qWarning() << "[createMainProjectFile] template seed failed:" << tmplErr;
            emit errorOccurred(QStringLiteral("Template seed failed: %1").arg(tmplErr));
        } else {
            saveBussesPacksImpl(projectRoot, project, m_serializer, nullptr);
            for (const auto& spec : s3Specs()) {
                auto* bucket = project.getBucketForFolder(spec.folderName);
                if (!bucket || bucket->empty()) continue;
                if (!saveS3HierarchyBucket(
                        projectRoot, project.projectId, project, spec, *bucket,

                        m_serializer)) {
                    qWarning() << "[createMainProjectFile] saveS3HierarchyBucket failed for"
                               << spec.folderName;
                }
            }
        }
    }

    ensureUniqueObjectIds(project);
    QString identityError;
    if (!validateProjectObjectIdentities(project, &identityError)) {
        emit errorOccurred(identityError);
        return false;
    }
    if (project.projectId.isEmpty())
        project.projectId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!m_serializer.prepareReferenceWrite(project))
        return false;
    return m_serializer.saveProject(project.projectPath, project);
}

void ProjectManager::drainPendingDestruction()
{
    if (m_runtimeCloseRevision)
        return;
    if (m_pendingDestruction) {
        m_pendingDestruction.reset();
        qDebug() << "[ProjectManager] Pending project destruction drained at"
                    " create/load entry.";
    }
    m_closing = false;
}

void ProjectManager::clearRuntimeCloseState()
{
    auto& audio = AudioEngine::instance();
    audio.clearRuntimeObjects();
    audio.clearRuntimeMedia();
    audio.resetListenerTransformCache();
    m_runtimeCloseRevision.reset();
    m_pendingDestruction.reset();
    m_projectInstance.reset();
    m_closing = false;
}

void ProjectManager::commitQueuedPreparedProject()
{
    if (!m_queuedPreparedProject)
        return;
    PreparedProject prepared = std::move(*m_queuedPreparedProject);
    m_queuedPreparedProject.reset();
    if (!commitPreparedProject(std::move(prepared)))
        return;
    startWatchingCurrentProject();
    publishProjectOpened();
}

void ProjectManager::cancelQueuedPreparedProject(const QString& projectPath)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_queuedPreparedProject && m_queuedPreparedProject->project
        && m_queuedPreparedProject->project->projectPath == projectPath) {
        // Drop the replacement, but retain the old world's close barrier and lease.
        // PreparedProject destruction also rolls back an uncommitted upgrade.
        m_queuedPreparedProject.reset();
    }
}

void ProjectManager::finishRuntimeCloseIfReady()
{
    if (!m_runtimeCloseRevision)
        return;
    const auto barrier = AudioEngine::instance().runtimeSyncCoordinator()
        .barrier(*m_runtimeCloseRevision);
    if (barrier.state == RuntimeSyncCoordinator::RevisionBarrierState::Pending)
        return;

    // Every other barrier state is terminal for this close, so the close must
    // complete here: the only re-entry is a queued barrierChanged, and a
    // terminal barrier never changes again.
    const bool rejected =
        barrier.state == RuntimeSyncCoordinator::RevisionBarrierState::Failed
        || barrier.state
            == RuntimeSyncCoordinator::RevisionBarrierState::Superseded;
    clearRuntimeCloseState();
    if (rejected) {
        emit errorOccurred(QStringLiteral(
            "Runtime rejected the project-close Current World barrier"));
        AudioEngine::instance().stopOutput();
    }
    commitQueuedPreparedProject();
}

void ProjectManager::finishRuntimePreparationIfReady()
{
    if (!m_pendingRuntimePreparation)
        return;
    auto& coordinator = AudioEngine::instance().runtimeSyncCoordinator();
    const auto barrier = coordinator.barrier(
        m_pendingRuntimePreparation->revision);
    if (barrier.projectSessionId
            != m_pendingRuntimePreparation->projectSessionId
        || barrier.runtimeEpoch
            != m_pendingRuntimePreparation->runtimeEpoch) {
        m_runtimePrepared = false;
        m_pendingRuntimePreparation.reset();
        return;
    }
    if (barrier.state
        == RuntimeSyncCoordinator::RevisionBarrierState::RuntimeUnavailable) {
        m_runtimePrepared = false;
        auto finished = std::move(m_pendingRuntimePreparation->finished);
        const QPointer<QObject> context =
            m_pendingRuntimePreparation->context;
        m_pendingRuntimePreparation.reset();
        if (context && finished)
            finished();
        return;
    }
    if (barrier.state
            == RuntimeSyncCoordinator::RevisionBarrierState::Failed
        || barrier.state
            == RuntimeSyncCoordinator::RevisionBarrierState::Superseded) {
        m_runtimePrepared = false;
        auto finished = std::move(m_pendingRuntimePreparation->finished);
        const QPointer<QObject> context =
            m_pendingRuntimePreparation->context;
        m_pendingRuntimePreparation.reset();
        if (context && finished)
            finished();
        return;
    }
    if (barrier.state != RuntimeSyncCoordinator::RevisionBarrierState::Ready)
        return;

    m_runtimePrepared = true;
    auto finished = std::move(m_pendingRuntimePreparation->finished);
    const QPointer<QObject> context = m_pendingRuntimePreparation->context;
    m_pendingRuntimePreparation.reset();
    if (context && finished)
        finished();
}

PreparedProject ProjectManager::prepareProject(const ProjectConfig& config)
{
    PreparedProject failed;
    if (!ProjectPlatformSemantics::validatePlatforms(config.platforms, &failed.error))
        return failed;
    if (!validateProjectTemplate(config, &failed.error))
        return failed;
    const QString projectRoot = QDir(config.location).filePath(config.name);
    QDir root(projectRoot);
    if (root.exists()) {
        failed.error = QStringLiteral("A folder with this project name already exists");
        return failed;
    }
    if (!root.mkpath(QStringLiteral("."))) {
        failed.error = QStringLiteral("Failed to create project directory structure");
        return failed;
    }
    for (const QString& directory : getStandardDirectories()) {
        if (!root.mkpath(directory)) {
            failed.error = QStringLiteral("Failed to create project directory structure");
            return failed;
        }
    }

    ProjectSerializer serializer;
    ProjectData project;
    project.name = config.name;
    project.author = config.author;
    project.description = config.description;
    project.createdDate = QDateTime::currentDateTime().toString(Qt::ISODate);
    project.audioFolder = kLayers;
    project.packFolder = QStringLiteral("Packs");
    project.projectPath = root.filePath(config.name + QStringLiteral(".hsp"));
    project.projectId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    project.platforms = config.platforms;
    project.sampleRate = config.sampleRate;
    project.channelConfig = config.channelConfig;
    project.enableLFE = config.enableLFE;
    project.projectTemplateId = config.templateId;
    ensureDefaultMasterBus(project);

    if (!config.templateId.isEmpty() && !config.templateData.isEmpty()) {
        QString templateError;
        if (!TemplateApplyService::seedTemplateInto(
                project, config.templateData, &templateError)) {
            failed.error = templateError;
            return failed;
        }
    }

    QString busError;
    if (!serializer.prepareReferenceWrite(project)
        || !saveBussesPacksImpl(projectRoot, project, serializer, &busError)
        || !serializer.saveProject(project.projectPath, project)) {
        failed.error = !serializer.lastError().isEmpty()
                           ? serializer.lastError()
                           : busError.isEmpty()
                           ? QStringLiteral("Failed to create project files")
                           : busError;
        return failed;
    }
    for (const auto& spec : s3Specs()) {
        auto* bucket = project.getBucketForFolder(spec.folderName);
        if (bucket && !bucket->empty())
            saveS3HierarchyBucket(projectRoot, project.projectId, project, spec, *bucket,

                                  serializer);
    }
    return prepareProject(project.projectPath);
}

PreparedProject ProjectManager::prepareProject(const QString& filePath,
                                               bool allowUpgrade,
                                               ProjectUpgradeProgressCallback upgradeProgress,
                                               ProjectUpgradeApprovalCallback upgradeApproval)
{
    PreparedProject prepared;
    prepared.project = std::make_unique<ProjectData>();
    ProjectSerializer serializer;
    if (!serializer.loadProject(filePath, *prepared.project, allowUpgrade,
                                std::move(upgradeProgress),
                                std::move(upgradeApproval))) {
        prepared.error = serializer.lastError();
        prepared.project.reset();
        return prepared;
    }
    prepared.upgrade = serializer.upgradeResult();
    prepared.upgradePending = prepared.upgrade.ok && prepared.upgrade.changed;

    const QString projectRoot = QFileInfo(prepared.project->projectPath).absolutePath();
    const bool registryLoaded =
        AssetRegistry::loadSnapshotFromFile(projectRoot, prepared.assets);
    prepared.rebuildAssetRegistry = AssetRegistry::reconcileSnapshot(
        projectRoot, prepared.project->sourceAssets, prepared.assets) || !registryLoaded;

    prepared.index = std::make_unique<ProjectIndex>();
    prepared.index->loadFromCache(projectRoot);
    const auto scanResult = prepared.index->scanProjectRoot(projectRoot);
    if (scanResult != ProjectIndex::ScanResult::Ok) {
        qWarning() << "[ProjectManager] ProjectIndex scan returned non-Ok:"
                   << static_cast<int>(scanResult);
    }

    QString busError;
    if (!loadBussesPacksImpl(projectRoot, *prepared.project, serializer, &busError)) {
        prepared.error = busError.isEmpty()
                             ? QStringLiteral("Failed to load Busses")
                             : busError;
        prepared.project.reset();
        return prepared;
    }
    QString actionError;
    if (!loadActions(projectRoot, *prepared.project, serializer, &actionError)) {
        prepared.error = actionError.isEmpty()
                             ? QStringLiteral("Failed to load Actions")
                             : actionError;
        prepared.project.reset();
        return prepared;
    }
    for (const auto& spec : s3Specs()) {
        QString hierarchyError;
        if (!loadS3Hierarchy(projectRoot, spec, *prepared.project, serializer,
                             &hierarchyError)) {
            prepared.error = hierarchyError.isEmpty()
                                 ? QStringLiteral("Failed to load %1").arg(spec.folderName)
                                 : hierarchyError;
            prepared.project.reset();
            return prepared;
        }
    }

    QString audioError;
    loadLayersPacksImpl(projectRoot, *prepared.project, serializer,
                        &audioError, &prepared.audioLoadFailures);
    prepared.diagnostics.append(serializer.warnings());
    if (!serializer.lastError().isEmpty() || !audioError.isEmpty()) {
        prepared.error = !serializer.lastError().isEmpty()
            ? serializer.lastError() : audioError;
        prepared.project.reset();
        return prepared;
    }

    QString identityError;
    if (!validateProjectObjectIdentities(*prepared.project, &prepared.error)) {
        prepared.project.reset();
        return prepared;
    }
    if (!validateLoadedProjectIdentities(*prepared.project, &identityError)
        || !ProjectPlatformSemantics::validateProject(*prepared.project, &identityError)) {
        prepared.error = identityError;
        prepared.project.reset();
        return prepared;
    }
    prepared.diagnostics.append(VariableSemantics::restoreLoadedLabelReferences(*prepared.project));
    if (!validatePlaybackReferences(*prepared.project, &prepared.error)) {
        prepared.error = projectIssueCode(ProjectIssue::InvalidPlaybackReference)
            + QStringLiteral(": ") + prepared.error;
        prepared.project.reset();
        return prepared;
    }
    const auto groupValidation = validateFieldGroupProject(*prepared.project);
    if (!groupValidation.ok()) {
        prepared.error = groupValidation.diagnostics.front().message;
        prepared.project.reset();
        return prepared;
    }
    prepared.folderSaveState = FolderSaveState::capture(*prepared.project, &prepared.error);
    if (!prepared.folderSaveState) {
        prepared.project.reset();
        return prepared;
    }
    refreshFieldGroupDerivedViews(*prepared.project);
    for (AssetInfo& asset : prepared.assets) {
        if (asset.metadata.fileSize == 0 && !asset.absolutePath.isEmpty())
            asset.metadata.fileSize = static_cast<uint64_t>(QFileInfo(asset.absolutePath).size());
    }


    {
        QHash<uint32_t, AssetInfo*> assets;
        for (auto& asset : prepared.assets) assets.insert(asset.sourceID, &asset);
        QSet<uint32_t> referenced;
        std::function<void(const ProjectItem*, const QString&)> collect =
            [&](const ProjectItem* item, const QString& ownerGuid) {
                if (!item) return;
                const auto* sound = item->type == ProjectItem::Type::Sound
                    ? item->comp<VSound>() : nullptr;
                if (sound) {
                    for (const auto* set : SoundSourceSemantics::sourceSets(sound->sources))
                    for (const SoundSourceDef& source : set->entries) {
                        if (source.sourceId == 0) continue;
                        referenced.insert(source.sourceId);
                        auto* asset = assets.value(source.sourceId);
                        if (asset && asset->ownerObjectGuid.isEmpty())
                            asset->ownerObjectGuid = ownerGuid;
                    }
                }
                for (const auto& child : item->children)
                    collect(child.get(), ownerGuid);
            };
        for (const auto* binding : prepared.project->allFileBindings()) {
            if (!binding || !binding->rootItem)
                continue;
            collect(binding->rootItem.get(),
                    binding->rootItem->guid.toString(QUuid::WithoutBraces));
        }
        QList<uint32_t> sourceIds = referenced.values();
        std::sort(sourceIds.begin(), sourceIds.end());
        for (const uint32_t sourceId : sourceIds) {
            if (!assets.contains(sourceId))
                prepared.diagnostics.append(QStringLiteral(
                    "No persisted file reference for audio source %1. "
                    "Its reference and audio files were preserved.").arg(sourceId));
        }
    }

    prepared.integrityScanner = std::make_unique<IntegrityScanner>();
    IntegrityScanner::Report cached;
    const bool hasCached = prepared.integrityScanner->loadFromCache(projectRoot, cached);
    const QByteArray indexHash = prepared.index->contentHash();
    if (!prepared.rebuildAssetRegistry && hasCached
        && !indexHash.isEmpty() && cached.indexHash == indexHash) {
        prepared.integrityReport = std::move(cached);
    } else {
        prepared.integrityReport =
            prepared.integrityScanner->scan(*prepared.project, prepared.assets);
        prepared.integrityReport.indexHash = indexHash;
    }

    return prepared;
}

bool ProjectManager::commitPreparedProject(PreparedProject&& prepared)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!prepared.project || !prepared.folderSaveState)
        return false;

    auto itemSaveState = std::make_unique<ProjectItemSaveState>();
    if (!itemSaveState->markSaved(*prepared.project)) {
        emit errorOccurred(QStringLiteral("Cannot capture saved project object content"));
        return false;
    }

    QString referenceError;
    if (!validatePlaybackReferences(*prepared.project, &referenceError)) {
        emit errorOccurred(referenceError);
        return false;
    }

    drainPendingDestruction();
    if (m_runtimeCloseRevision) {
        m_queuedPreparedProject.emplace(std::move(prepared));
        return true;
    }
    if (m_currentProject) {
        if (!closeProject())
            return false;
        if (m_runtimeCloseRevision) {
            m_queuedPreparedProject.emplace(std::move(prepared));
            return true;
        }
        drainPendingDestruction();
    }
    const QString projectRoot = QFileInfo(prepared.project->projectPath).absolutePath();
    AssetRegistry& assetRegistry = AssetRegistry::instance();
    assetRegistry.setProjectRoot(projectRoot);
    if (assetRegistry.projectRoot() != projectRoot) {
        emit errorOccurred(QStringLiteral("Failed to initialize project asset directories."));
        return false;
    }
    if (AudioEngine::instance().runtimeSyncCoordinator().beginProjectSession()
        == RuntimeSyncCoordinator::AllocationState::IdentityExhausted) {
        prepared.rollbackUpgrade();
        emit errorOccurred(ProjectUpgradeMessages::userMessage(QStringLiteral(
            "Runtime project-session identity exhausted"), true));
        return false;
    }
    if (prepared.upgradePending) {
        ProjectUpgradeManager upgradeManager;
        QString upgradeError;
        if (!upgradeManager.finalize(prepared.upgrade, &upgradeError)) {
            prepared.rollbackUpgrade();
            emit errorOccurred(ProjectUpgradeMessages::userMessage(upgradeError, true));
            return false;
        }
        prepared.releaseUpgradeRecovery();
    }
    m_currentProject = std::move(prepared.project);
    m_projectInstance = std::move(prepared.instance);
    m_folderSaveState = std::move(prepared.folderSaveState);
    m_itemSaveState = std::move(itemSaveState);
    m_projectIndex = std::move(prepared.index);
    m_integrityScanner = std::move(prepared.integrityScanner);
    m_closing = false;
    m_runtimePrepared = false;
    PackExportSelectionStore::instance().openProject(*m_currentProject);
    PackArtifactCache::instance().openProject(*m_currentProject);
    PackGenerationRecordStore::instance().clear();

    assetRegistry.replaceSnapshot(projectRoot, prepared.assets);
    assetRegistry.setPlatforms(m_currentProject->platforms);
    assetRegistry.markAllCommitted();

    if (!prepared.integrityReport.issues.empty()) {
        LogPanel::logWarning(LogCategory::File,
            IntegrityScanner::summarize(prepared.integrityReport));
    }

    for (const QString& diagnostic : prepared.diagnostics)
        emit warningOccurred(ProjectUpgradeMessages::userMessage(diagnostic));
    return true;
}

void ProjectManager::publishProjectOpened()
{
    if (!m_currentProject)
        return;
    emit projectOpened(m_currentProject.get());
    HubIntake::recordProjectOpened(m_currentProject->projectPath);
}

void ProjectManager::startWatchingCurrentProject()
{
    if (!m_currentProject)
        return;
    m_fileWatcher.startWatching(QFileInfo(m_currentProject->projectPath).absolutePath());
}

bool ProjectManager::loadProject(const QString& filePath, bool allowUpgrade,
                                 QString* error, bool* forwarded)
{
    if (error)
        error->clear();
    if (forwarded)
        *forwarded = false;
    auto acquisition = ProjectInstance::acquire(filePath);
    if (acquisition.status != ProjectInstance::Status::Acquired) {
        if (forwarded)
            *forwarded = acquisition.status == ProjectInstance::Status::Forwarded;
        if (error)
            *error = acquisition.error;
        return false;
    }
    if (m_startupProgressCallback)
        m_startupProgressCallback(StartupPhase::ReadingProject);
    PreparedProject prepared = prepareProject(filePath, allowUpgrade);
    prepared.instance = std::move(acquisition.instance);
    if (!prepared.project) {
        if (error)
            *error = prepared.error;
        if (!prepared.error.isEmpty())
            emit errorOccurred(ProjectUpgradeMessages::userMessage(prepared.error, true));
        return false;
    }
    if (m_startupProgressCallback)
        m_startupProgressCallback(StartupPhase::LoadingProjectContent);
    if (!commitPreparedProject(std::move(prepared)))
        return false;
    startWatchingCurrentProject();
    if (m_startupProgressCallback)
        m_startupProgressCallback(StartupPhase::PreparingRuntime);
    publishProjectOpened();
    return true;
}

void ProjectManager::reconcileAssetsOnOpen(int bindingLoadFailures)
{
    if (!m_currentProject) return;

    // A failed .vp load would make that binding's assets look unreferenced;
    // destroying them here would be wrong, so skip the whole pass.
    if (bindingLoadFailures > 0) {
        qWarning() << "[ProjectManager] Skipping asset reconcile:"
                   << bindingLoadFailures << "binding file(s) failed to load.";
        return;
    }

    const QString projectRoot = AssetRegistry::instance().projectRoot();
    if (projectRoot.isEmpty()) return;

    QSet<uint32_t> referenced;
    QSet<QString> referencedPaths;
    for (auto* binding : m_currentProject->allFileBindings()) {
        if (!binding || !binding->rootItem) continue;
        for (const MediaFileRef& ref : collectMediaFiles(binding->rootItem.get())) {
            if (!ProjectDataPolicy::isSafeRelativePath(ref.relativePath)) {
                qWarning() << "[ProjectManager] Skipping asset reconcile: unresolved audio source"
                           << ref.sourceID;
                return;
            }
            referenced.insert(ref.sourceID);
            referencedPaths.insert(PathUtils::normalizeToUnix(ref.relativePath));
        }
    }

    QList<QString> reclaim;
    for (uint32_t id : AssetRegistry::instance().getAllSourceIDs()) {
        if (referenced.contains(id)) continue;
        if (const AssetInfo* info = AssetRegistry::instance().getAsset(id)) {
            reclaim.append(info->relativePath);
        }
    }
    if (!reclaim.isEmpty()) {
        AssetRegistry::instance().cleanupOrphaned(reclaim);
        qInfo() << "[ProjectManager] Reconcile: dropped" << reclaim.size()
                << "unreferenced asset record(s).";
    }

    AssetGarbageCollector gc;
    gc.setProjectRoot(projectRoot);
    const QString audioSourcesDir = PathUtils::getAudioSourcesDir(projectRoot);
    int trashedFiles = 0;
    for (const QString& relativePath : gc.scanForOrphans()) {
        // A file a tree node still points to, but whose record was lost (e.g. a
        // crash between writing the project and assets.json), must survive as an
        // honest Missing rather than be quarantined here.
        if (referencedPaths.contains(PathUtils::normalizeToUnix(relativePath))) continue;
        const QString absPath = PathUtils::toAbsolutePath(relativePath, audioSourcesDir);
        if (PathUtils::moveToTrash(projectRoot, absPath, relativePath)) ++trashedFiles;
    }
    if (trashedFiles > 0) {
        qInfo() << "[ProjectManager] Reconcile: quarantined" << trashedFiles
                << "orphaned AudioSources file(s) into" << PathUtils::TRASH_DIR;
    }

    if (!reclaim.isEmpty() || trashedFiles > 0) {
        LogPanel::logInfo(LogCategory::File,
            tr("Asset cleanup: %1 unused asset(s) and %2 orphaned file(s) moved to %3/")
                .arg(reclaim.size()).arg(trashedFiles)
                .arg(QLatin1String(PathUtils::TRASH_DIR)));
    }

    if (!reclaim.isEmpty()) {
        AssetRegistry::instance().saveToFile();
    }

    const int purged = gc.purgeTrash();
    if (purged > 0) {
        qInfo() << "[ProjectManager] Reconcile: purged" << purged
                << "expired trash entries.";
    }
}

bool ProjectManager::saveProject()
{
    if (!m_currentProject) return false;
    QString sourceRecoveryError;
    if (!SourceFileSystem::recoverDiscard(&sourceRecoveryError)) {
        emit errorOccurred(sourceRecoveryError);
        return false;
    }
    m_fileWatcher.refreshSources();
    if (!PathUtils::isValidPackBuildOutputDir(m_currentProject->packBuildOutputDir,
                                             m_currentProject->projectPath)) {
        emit errorOccurred(PathUtils::packBuildOutputLocationError());
        return false;
    }

    QString referenceError;
    if (!validatePlaybackReferences(*m_currentProject, &referenceError)) {
        emit errorOccurred(referenceError);
        return false;
    }

    PropertyEditSession::finishAllActive(true);

    QString identityError;
    if (!validateProjectObjectIdentities(*m_currentProject, &identityError)) {
        emit errorOccurred(identityError);
        return false;
    }

    QString platformError;
    if (!ProjectPlatformSemantics::validateProject(*m_currentProject, &platformError)) {
        emit errorOccurred(platformError);
        return false;
    }

    QTemporaryDir stagedDirectory;
    if (!stagedDirectory.isValid()) {
        emit errorOccurred(QStringLiteral("Cannot prepare project save directory"));
        return false;
    }
    const QDir projectRoot(QFileInfo(m_currentProject->projectPath).absolutePath());
    const QDir stagedRoot(stagedDirectory.path());
    const QString projectFileName = QFileInfo(m_currentProject->projectPath).fileName();
    const QString stagedProjectPath = stagedRoot.filePath(projectFileName);
    ProjectData stagedProject = *m_currentProject;
    stagedProject.sourceAssets.clear();
    for (const auto& asset : AssetRegistry::instance().snapshot())
        stagedProject.sourceAssets.insert(asset.sourceID, asset.relativePath);

    QString saveError;
    if (!m_folderSaveState->stage(stagedProject, stagedRoot.path(), &saveError)) {
        emit errorOccurred(saveError);
        return false;
    }
    const QString registryDirectory = QStringLiteral(".cache/asset_registry");
    if (!copyDirectoryTree(projectRoot.filePath(registryDirectory),
                           stagedRoot.filePath(registryDirectory))) {
        emit errorOccurred(QStringLiteral("Cannot prepare the asset registry for saving"));
        return false;
    }
    if (!m_serializer.prepareProjectSave(m_currentProject->projectPath, stagedProject, stagedProjectPath)) {
        emit errorOccurred(m_serializer.lastError());
        return false;
    }
    const auto cancelPreparedSave = qScopeGuard([&] { m_serializer.cancelProjectSave(); });

    bool hierarchySaveOk = saveActionsBucket(stagedRoot.path(), stagedProject.projectId,
        stagedProject, stagedProject.actions, m_serializer);
    for (const auto& spec : s3Specs()) {
        auto* bucket = stagedProject.getBucketForFolder(spec.folderName);
        if (bucket && !saveS3HierarchyBucket(stagedRoot.path(), stagedProject.projectId,
                stagedProject, spec, *bucket, m_serializer))
            hierarchySaveOk = false;
    }
    if (!saveLayersBucket(stagedRoot.path(), stagedProject.projectId, stagedProject,
                          stagedProject.layers, m_serializer, &saveError))
        hierarchySaveOk = false;
    if (!saveBussesPacksImpl(stagedRoot.path(), stagedProject, m_serializer, &saveError))
        hierarchySaveOk = false;
    if (!AssetRegistry::instance().saveToFile(
            stagedRoot.filePath(registryDirectory + QStringLiteral("/assets.json"))))
        hierarchySaveOk = false;
    if (!hierarchySaveOk) {
        emit errorOccurred(saveError.isEmpty()
            ? QStringLiteral("Failed to prepare project files for saving") : saveError);
        return false;
    }
    if (!m_serializer.commitProjectSave()) return false;

    QFile stagedFile(stagedProjectPath);
    QJsonParseError parseError;
    const QJsonDocument committedDocument = QJsonDocument::fromJson(
        stagedFile.open(QIODevice::ReadOnly) ? stagedFile.readAll() : QByteArray(), &parseError);
    stagedFile.close();
    const QStringList validationErrors =
        (!committedDocument.isObject() || parseError.error != QJsonParseError::NoError)
            ? QStringList{QStringLiteral("Prepared project file is invalid JSON")}
            : ProjectContractRegistry::instance().validateTree(
                stagedRoot.path(), committedDocument.object(), true);
    if (!validationErrors.isEmpty()) {
        emit errorOccurred(validationErrors.first());
        return false;
    }

    auto itemSaveState = std::make_unique<ProjectItemSaveState>();
    if (!itemSaveState->markSaved(stagedProject)) {
        emit errorOccurred(QStringLiteral("Cannot capture saved project object content"));
        return false;
    }

    QStringList directoryRoots = getStandardDirectories();
    directoryRoots.append(registryDirectory);
    QHash<QString, QString> writePaths;
    writePaths.insert(BucketFolderStore::pathComparisonKey(projectFileName), projectFileName);
    for (const auto& root : {projectRoot, stagedRoot}) {
        for (const QString& directory : directoryRoots) {
            QDirIterator entries(root.filePath(directory),
                QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                QDirIterator::Subdirectories);
            while (entries.hasNext()) {
                const QString path = entries.next();
                if (entries.fileInfo().isSymLink()) {
                    emit errorOccurred(QStringLiteral("Cannot save a project containing a symbolic link: %1").arg(path));
                    return false;
                }
                const QString relative = root.relativeFilePath(path);
                writePaths.insert(BucketFolderStore::pathComparisonKey(relative), relative);
            }
        }
    }
    auto projectTransaction = ProjectTransaction::begin(
        m_currentProject->projectPath, writePaths.values(), &saveError);
    if (!projectTransaction) {
        emit errorOccurred(saveError);
        return false;
    }
    const bool resumeWatcher = m_fileWatcher.isWatching();
    m_fileWatcher.stopWatching();
    const auto restoreWatcher = qScopeGuard([&] {
        if (resumeWatcher) m_fileWatcher.startWatching(projectRoot.path());
    });
    if (!projectTransaction->replaceFromDirectory(stagedRoot.path(), directoryRoots, &saveError)
        || !projectTransaction->commit(false, &saveError)) {
        QString rollbackError;
        if (!projectTransaction->rollback(&rollbackError))
            emit errorOccurred(rollbackError);
        emit errorOccurred(saveError);
        return false;
    }
    for (auto* binding : stagedProject.allDocumentBindings()) {
        const QString relative = stagedRoot.relativeFilePath(binding->filePath);
        if (!binding->filePath.isEmpty() && ProjectDataPolicy::isSafeRelativePath(relative))
            binding->filePath = projectRoot.filePath(relative);
    }
    m_currentProject->documentBindings = std::move(stagedProject.documentBindings);
    m_currentProject->sourceAssets = std::move(stagedProject.sourceAssets);

    AssetRegistry::instance().markAllCommitted();

    m_currentProject->documentHeader =
        committedDocument.object().value(QStringLiteral("harkenDocument")).toObject();
    const auto updateDocumentHeaders = [&](auto&& self, ProjectItem& item) -> void {
        QString type;
        switch (ItemTypeCatalog::bucketOf(item.type)) {
        case ItemTypeCatalog::Bucket::Layers:
            type = QStringLiteral("project.layer");
            break;
        case ItemTypeCatalog::Bucket::Actions:
            type = QStringLiteral("project.action");
            break;
        case ItemTypeCatalog::Bucket::Attenuations:
            type = QStringLiteral("project.attenuation");
            break;
        case ItemTypeCatalog::Bucket::Busses:
            type = QStringLiteral("project.bus");
            break;
        case ItemTypeCatalog::Bucket::Variables:
            type = QStringLiteral("project.variable");
            break;
        default:
            break;
        }
        if (!type.isEmpty()) {
            item.documentHeader = ProjectDocument::createHeader(
                type, item.guid.toString(QUuid::WithoutBraces),
                m_currentProject->projectId, item.documentHeader);
        }
        for (const auto& child : item.children)
            if (child) self(self, *child);
    };
    for (auto* binding : m_currentProject->allFileBindings())
    {
        if (binding && binding->rootItem) {
            updateDocumentHeaders(updateDocumentHeaders, *binding->rootItem);
            binding->rootItem->clearDirty();
        }
    }

    SourceFileSystem::commitChanges();
    m_folderSaveState->markSaved(*m_currentProject);
    m_itemSaveState = std::move(itemSaveState);
    UndoRedoManager::instance().setClean();

    emit projectSaved();
    return true;
}

bool ProjectManager::closeProject()
{
    if (m_currentProject)
    {
        PropertyEditSession::finishAllActive(false);
        PackGenerationRecordStore::instance().cancelExport();
        if (ProjectPackExporter::hasBackgroundExportInFlight())
        {
            qInfo() << "[ProjectManager] closeProject: waiting for in-flight"
                       " pack export to finish...";
            if (!ProjectPackExporter::waitForBackgroundExports(30000))
            {
                qWarning() << "[ProjectManager] closeProject: pack export"
                              " still running after 30s -- closing anyway"
                              " (export result will be discarded)";
                AudioEngine::instance().addExecError(
                    AudioEngine::ExecErrorDomain::Lifecycle, 1, 0, 0,
                    QString(), QStringLiteral("CLOSE_EXPORT_TIMEOUT"),
                    QStringLiteral("pack export still running after 30s -- "
                                   "project force-closed, export result "
                                   "discarded"));
            }
        }

        if (AudioEngine::instance().isInitialized())
        {
            auto& audio = AudioEngine::instance();
            audio.stopAll(0.0f);
        }

        auto& mediaPool = MediaPool::instance();
        mediaPool.beginSourceFileChange();
        const auto resumeReaders = qScopeGuard([&] { mediaPool.endSourceFileChange(); });
        const bool resumeWatcher = m_fileWatcher.isWatching();
        m_fileWatcher.stopWatching();
        const auto restoreWatcher = qScopeGuard([&] {
            if (resumeWatcher && m_currentProject)
                m_fileWatcher.startWatching(QFileInfo(m_currentProject->projectPath).absolutePath());
        });
        QString discardError;
        bool rollbackFailed = false;
        if (!SourceFileSystem::discardChanges(&discardError, &rollbackFailed)) {
            if (rollbackFailed) {
                UndoRedoManager::instance().clear();
                UndoRedoManager::instance().undoStack()->resetClean();
            }
            emit errorOccurred(discardError);
            return false;
        }
        AssetRegistry::instance().discardUncommittedCaches();
        m_closing = true;
        m_runtimePrepared = false;
        m_itemSaveState.reset();
        UndoRedoManager::instance().clear();
        m_folderSaveState.reset();
        AssetRegistry::instance().clear();
        qDebug() << "[ProjectManager] AssetRegistry cleared.";

        m_projectIndex.reset();
        m_integrityScanner.reset();

        m_pendingResyncs.clear();
        m_resyncTimerScheduled = false;

        m_auditionOverlay.clearAll();
        m_variableRehearsal.clearAll();
        m_transitionRehearsal.clearAll();
        clearClipboard();

        PackExportSelectionStore::instance().closeProject();
        PackArtifactCache::instance().closeProject();
        PackGenerationRecordStore::instance().clear();
        m_pendingDestruction = std::shared_ptr<ProjectData>(std::move(m_currentProject));
        m_pendingRuntimePreparation.reset();
        emit projectClosed();
        auto& coordinator = AudioEngine::instance().runtimeSyncCoordinator();
        if (coordinator.beginProjectSession()
            == RuntimeSyncCoordinator::AllocationState::IdentityExhausted) {
            qCritical() << "[CurrentWorldSync] project session identity exhausted";
            auto& audio = AudioEngine::instance();
            audio.clearRuntimeObjects();
            audio.clearRuntimeMedia();
            audio.resetListenerTransformCache();
            m_pendingDestruction.reset();
            m_projectInstance.reset();
            m_closing = false;
            return true;
        }
        const auto allocation = coordinator.markCurrentWorldMutation();
        if (allocation.state
            == RuntimeSyncCoordinator::AllocationState::IdentityExhausted) {
            qCritical() << "[CurrentWorldSync] authoring revision identity exhausted";
            auto& audio = AudioEngine::instance();
            audio.clearRuntimeObjects();
            audio.clearRuntimeMedia();
            audio.resetListenerTransformCache();
            m_pendingDestruction.reset();
            m_projectInstance.reset();
            m_closing = false;
            return true;
        }
        m_runtimeCloseRevision = allocation.revision;
        coordinator.flushPendingMutationNow();
        finishRuntimeCloseIfReady();
    }
    else
    {
        AudioEngine::instance().addExecError(
            AudioEngine::ExecErrorDomain::Lifecycle, 1, 0, 0,
            QString(), QStringLiteral("CLOSE_NO_PROJECT"),
            QStringLiteral("closeProject called with no open project"));
    }
    return true;
}


namespace {
bool initializeRootFieldGroups(ProjectData& project, ProjectItem& item)
{
    if (!PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item.type))
        return true;
    const auto* master = project.busses.empty()
        ? nullptr : project.busses.front().rootItem.get();
    if (!master || master->type != ProjectItem::Type::Bus || master->guid.isNull())
        return false;
    item.fieldGroups = MakeRootFieldGroups(toFieldGroupGuid(master->guid));
    return true;
}

auto guardFieldGroupIdentities(const std::shared_ptr<ProjectData>& project,
                                 ProjectItem::Type type)
{
    const uint32_t nextObject = project ? project->nextObjectID : 0;
    return qScopeGuard([project, type, nextObject] {
        if (project && PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(type)) {
            project->nextObjectID = nextObject;
        }
    });
}

void appendPublishedNameHistory(ProjectData* project,
                                      const ProjectItem& item,
                                      const QString& event,
                                      const QString& priorName = {})
{
    if (!project || item.type != ProjectItem::Type::Action) return;
    ActionPublishedNameHistoryRow row;
    row.name = item.name;
    row.objectGuid = item.guid;
    row.nameBindingRevision = item.nameBindingRevision;
    row.event = event;
    row.priorName = priorName;
    project->actionPublishedNameHistory.push_back(std::move(row));
}

void recordNamedObjectCreated(ProjectData* project, ProjectItem& item)
{
    if (!project || item.type != ProjectItem::Type::Action) return;
    item.nameBindingRevision = 1;
    appendPublishedNameHistory(project, item, QStringLiteral("created"));
}

void recordNamedObjectRenamed(ProjectData* project, ProjectItem& item, const QString& fromName)
{
    if (item.type != ProjectItem::Type::Action || fromName == item.name) return;
    if (item.nameBindingRevision < std::numeric_limits<uint64_t>::max())
        ++item.nameBindingRevision;
    appendPublishedNameHistory(
        project, item, QStringLiteral("renamed_from"), fromName);
}

void recordNamedObjectDeleted(ProjectData* project, ProjectItem& item)
{
    if (!project || item.type != ProjectItem::Type::Action) return;
    appendPublishedNameHistory(project, item, QStringLiteral("deleted"));
}

}

PhysicalFolder* ProjectManager::createFolder(PhysicalFolder* parent, const QString& name,
                                              const QString& bucket)
{
    if (!m_currentProject) {
        emit errorOccurred("No project loaded; cannot create Folder.");
        return nullptr;
    }
    if (!bucketAllowsFolder(bucket)) {
        emit errorOccurred(QString("Bucket '%1' does not accept Folder creation.").arg(bucket));
        return nullptr;
    }
    if (parent && m_currentProject->folderBucket(parent) != bucket) return nullptr;
    NameValidator::Result validation;
    if (!NameValidator::validateFileName(name, &validation)) {
        emit errorOccurred(QString("Invalid Folder name '%1': %2")
                               .arg(name, NameValidator::resultMessage(validation)));
        return nullptr;
    }
    const QString resolvedName = suggestUniqueFolderName(parent, bucket, name);
    if (resolvedName.isEmpty()) return nullptr;
    auto folder = std::make_shared<PhysicalFolder>();
    folder->name = resolvedName;
    folder->parent = parent;
    if (!createPhysicalFolder(parent, folder, bucket)) return nullptr;
    return folder.get();
}

bool ProjectManager::isFolderDirty(const PhysicalFolder* folder) const
{
    return folder && m_currentProject && m_folderSaveState
        && m_folderSaveState->isFolderDirty(*m_currentProject, folder);
}

QSet<QString> ProjectManager::bucketsWithPendingFileChanges() const
{
    return m_currentProject && m_folderSaveState
        ? m_folderSaveState->bucketsWithPendingFileChanges(*m_currentProject) : QSet<QString>{};
}

bool ProjectManager::validateFolderRename(PhysicalFolder* folder, const QString& name,
                                           QString* error) const
{
    if (error) error->clear();
    if (!m_currentProject || m_currentProject->folderBucket(folder).isEmpty()) {
        if (error) *error = tr("The folder no longer exists.");
        return false;
    }
    NameValidator::Result validation;
    if (!NameValidator::validateFileName(name, &validation)) {
        if (error) *error = QString("Invalid Folder name '%1': %2")
                               .arg(name, NameValidator::resultMessage(validation));
        return false;
    }
    return true;
}

bool ProjectManager::renameFolder(PhysicalFolder* folder, const QString& name)
{
    QString error;
    if (!validateFolderRename(folder, name, &error)) {
        emit errorOccurred(error);
        return false;
    }
    const QString resolvedName = suggestUniqueFolderName(
        folder->parent, m_currentProject->folderBucket(folder), name, folder);
    if (resolvedName.isEmpty()) return false;
    const QString oldName = folder->name;
    if (oldName == resolvedName) return true;
    return renamePhysicalFolder(folder, resolvedName);
}

bool ProjectManager::moveFolder(PhysicalFolder* folder, PhysicalFolder* newParent,
                                 const QString& bucket)
{
    if (!m_currentProject || !bucketAllowsFolder(bucket)
        || m_currentProject->folderBucket(folder) != bucket
        || (newParent && m_currentProject->folderBucket(newParent) != bucket)) return false;
    if (folder->parent == newParent) return true;
    for (const PhysicalFolder* ancestor = newParent; ancestor; ancestor = ancestor->parent)
        if (ancestor == folder) return false;
    if (!isFolderNameUniqueAmongSiblings(newParent, bucket, folder->name, folder)) {
        emit errorOccurred(QString("A Folder named \"%1\" already exists in this folder.")
                               .arg(folder->name));
        return false;
    }
    for (const ProjectFileBinding* binding : m_currentProject->fileBindingsUnderFolder(folder)) {
        if (!binding->rootItem) continue;
        const auto validity = validateNewAuthoring(*this, *binding->rootItem, nullptr);
        if (!validity) {
            emit errorOccurred(validity.message);
            return false;
        }
    }
    return executePhysicalMoves({}, {{folder, newParent, bucket}});
}

bool ProjectManager::moveEntriesToFolder(const QList<ProjectItem*>& items,
    const QList<PhysicalFolder*>& folders, const QString& bucket, const QString& relativePath)
{
    if (!m_currentProject || !bucketAllowsFolder(bucket)) return false;
    auto* destination = relativePath.isEmpty()
        ? nullptr : m_currentProject->findFolderByPath(bucket, relativePath);
    if (!relativePath.isEmpty() && !destination) return false;
    const auto unique = uniqueItemsByGuid(items);
    const auto allowed = allowedTopLevelTypesForBucket(bucket);
    std::vector<PhysicalItemMove> itemMoves;
    std::vector<PhysicalFolderMove> folderMoves;
    for (auto* item : unique) {
        const auto* binding = findFileBindingForItem(*m_currentProject, item);
        if (!binding || binding->parentFolder != bucket
            || !ItemTypeCatalog::isFolderAddressable(item->type)) return false;
        if (item->parent) {
            if (std::find(allowed.begin(), allowed.end(), item->type) == allowed.end()) return false;
            const auto authoring = validateNewAuthoring(*this, *item, nullptr);
            if (!authoring) {
                emit errorOccurred(authoring.message);
                return false;
            }
        }
        itemMoves.push_back({item, destination});
    }
    for (auto* folder : folders) folderMoves.push_back({folder, destination, bucket});
    std::vector<std::unique_ptr<TransactionalUndoCommand>> commands;
    for (auto* item : unique)
        if (item->parent) commands.push_back(std::make_unique<MoveItemCommand>(item, bucket));
    return executePhysicalMoves(itemMoves, folderMoves,
        makeMoveCommand(std::move(commands), tr("Move items")));
}

ProjectItem* ProjectManager::createItem(ProjectItem* parent, const QString& name, ProjectItem::Type type,
                                        const QString& path, const QString& bucketFolderHint,
                                        QUndoCommand* undoParent, VariableKind variableKind)
{
    return createItemImpl(parent, name, type, path, bucketFolderHint, undoParent, nullptr, variableKind);
}

ProjectItem* ProjectManager::createTrackWithClip(ProjectItem* piece, const QString& name, Clip clip)
{
    if (!m_currentProject || !piece || piece->type != ProjectItem::Type::Piece) {
        emit errorOccurred(QStringLiteral("Track creation with a clip requires a Piece in an open project."));
        return nullptr;
    }
    clip.clipGuid = QUuid::createUuid();
    return createItemImpl(piece, name, ProjectItem::Type::Track, {}, {}, nullptr, &clip);
}

ProjectItem* ProjectManager::createItemImpl(ProjectItem* parent, const QString& name,
                                            ProjectItem::Type type, const QString& path,
                                            const QString& bucketFolderHint,
                                            QUndoCommand* undoParent,
                                            const Clip* initialClip, VariableKind variableKind)
{
    if (type == ProjectItem::Type::Bus && !parent)
    {
        emit errorOccurred("Bus cannot be created at top level; pass the parent Bus ProjectItem.");
        return nullptr;
    }

    const bool isTopLevelCreate = (parent == nullptr);
    std::vector<ProjectFileBinding>* targetBucket = nullptr;
    QString targetFolderName;
    if (isTopLevelCreate) {
        if (!m_currentProject) {
            emit errorOccurred("No project loaded; cannot create top-level item.");
            return nullptr;
        }
        const ItemTypeCatalog::TypeFacts* facts = ItemTypeCatalog::factsOrNull(type);
        targetBucket = (facts && facts->topLevelCreatable)
            ? m_currentProject->getBucketForType(type) : nullptr;
        if (!targetBucket) {
            emit errorOccurred(QString("A %1 cannot be created at the top level of the project.")
                                   .arg(ItemTypeCatalog::displayName(type)));
            return nullptr;
        }
        targetFolderName = m_currentProject->folderNameForBucket(targetBucket);
    } else {
        QString placementError;
        if (!validatePlacement(type, parent, nullptr, &placementError)) {
            emit errorOccurred(placementError);
            return nullptr;
        }
    }

    // Extension point ②: fine-grained per-instance placement rules (bus depth /
    // fan-out). No-op for non-bus types.
    if (type == ProjectItem::Type::Bus) {
        QString structuralErr;
        if (!validateStructuralPlacement(type, parent, &structuralErr)) {
            emit errorOccurred(structuralErr);
            return nullptr;
        }
    }

    if (!path.isEmpty()) {
        const ItemTypeCatalog::TypeFacts* facts = ItemTypeCatalog::factsOrNull(type);
        const QStringList segments = path.split(QChar('/'), Qt::KeepEmptyParts);
        const bool invalidSegment = std::any_of(
            segments.cbegin(), segments.cend(), [](const QString& segment) {
                NameValidator::Result result;
                return segment == QStringLiteral(".")
                    || segment == QStringLiteral("..")
                    || !NameValidator::validateFileName(segment, &result);
            });
        if (!facts || !facts->folderAddressable || !isTopLevelCreate) {
            emit errorOccurred(QString("A %1 cannot be created at a folder address.")
                                   .arg(ItemTypeCatalog::displayName(type)));
            return nullptr;
        }
        if (QDir::isAbsolutePath(path)
            || path.contains(QChar('\\'))
            || invalidSegment) {
            emit errorOccurred(QString("Invalid folder address '%1'.").arg(path));
            return nullptr;
        }
    }

    NameValidator::Result vr;
    if (!NameValidator::validateItemName(name, type, &vr)) {
        emit errorOccurred(QString("Invalid name '%1': %2")
                               .arg(name, NameValidator::resultMessage(vr)));
        return nullptr;
    }
    const QString createName = suggestUniqueName(type, name, nullptr, {}, parent, path);
    if (createName.isEmpty()) return nullptr;

    auto newItem = std::make_shared<ProjectItem>(createName, type);
    newItem->attachDefaultComponents();
    if (auto* variable = newItem->comp<VVariable>())
        VariableSemantics::initializeNewVariable(*variable, variableKind);
    if (auto* selection = newItem->comp<VSelection>())
        selection->transitions = AuthoringPolicy::defaultTransitionTable();
    if (initialClip) {
        auto* timeline = newItem->comp<VTrack>();
        if (!timeline) return nullptr;
        Clip clip = *initialClip;
        if (!timeline->clips.mintOccurrenceOrdinal(&clip)) return nullptr;
        timeline->clips.items.push_back(std::move(clip));
        const auto targets = timeline->clips.validateTargets(
            *newItem, [this](const QUuid& guid) { return findItemByGuid(guid); });
        if (!targets) {
            emit errorOccurred(targets.message);
            return nullptr;
        }
        newItem->parent = parent;
        const auto placement = TimelineEditing::validateClipPlacement(this, newItem.get(), timeline->clips);
        newItem->parent = nullptr;
        if (!placement) {
            emit errorOccurred(placement.message);
            return nullptr;
        }
    }
    const auto authoring = validateNewAuthoring(*this, *newItem, parent);
    if (!authoring) {
        emit errorOccurred(authoring.message);
        return nullptr;
    }
    auto identityGuard = guardFieldGroupIdentities(m_currentProject, type);

    if (m_currentProject) {
        newItem->objectID = m_currentProject->generateObjectID();
    }

    newItem->guid = QUuid::createUuid();

    if (isTopLevelCreate) {
        if (!initializeRootFieldGroups(*m_currentProject, *newItem))
            return nullptr;
        newItem->parent = nullptr;
        if (isLayersBucketRootType(type)) {
            newItem->parentRef = ParentRef{};
            newItem->parentRef.kind = ParentRef::Kind::Root;
        }

        ProjectFileBinding binding;
        binding.parentFolder = targetFolderName;
        binding.rootItem = newItem;
        PhysicalFolder* parentFolder = nullptr;
        if (!path.isEmpty()) {
            const QString normalized = QDir::fromNativeSeparators(path);
            if (QDir::isAbsolutePath(normalized)) return nullptr;
            const QStringList segments = normalized.split(QChar('/'), Qt::KeepEmptyParts);
            for (const QString& segment : segments) {
                NameValidator::Result result;
                if (segment == QStringLiteral(".") || segment == QStringLiteral("..")
                    || segment.trimmed() != segment || segment.endsWith(QChar('.'))
                    || !NameValidator::validateFileName(segment, &result)) {
                    return nullptr;
                }
            }
            QString relativePath;
            for (const QString& segment : segments) {
                relativePath = relativePath.isEmpty()
                    ? segment : relativePath + QChar('/') + segment;
                auto* folder = m_currentProject->findFolderByPath(targetFolderName, relativePath);
                if (!folder) folder = createFolder(parentFolder, segment, targetFolderName);
                if (!folder) return nullptr;
                parentFolder = folder;
            }
        }
        targetBucket->push_back(std::move(binding));
        if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(type)
            && (!validateFieldGroupProject(*m_currentProject).ok()
                || !refreshFieldGroupDerivedViews(*m_currentProject))) {
            targetBucket->pop_back();
            return nullptr;
        }

        identityGuard.dismiss();
        m_currentProject->setObjectFileLocation(newItem.get(), parentFolder, {});
        newItem->markDirty();
        newItem->objectPath = computeObjectPath(nullptr, newItem->name);
        emit itemCreated(newItem.get());
        auto* command = new CreateProjectItemCommand(nullptr, newItem, undoParent);
        if (!undoParent)
            UndoRedoManager::instance().push(command);
        return newItem.get();
    }

    if (!restoreItemInternal(parent, newItem)) {
        return nullptr;
    }
    m_currentProject->setObjectFileLocation(newItem.get(), m_currentProject->objectFolder(parent), {});
    identityGuard.dismiss();
    newItem->objectPath = computeObjectPath(parent, newItem->name);
    emit itemCreated(newItem.get());

    auto* command = new CreateProjectItemCommand(parent, newItem, undoParent);
    if (!undoParent)
        UndoRedoManager::instance().push(command);

    return newItem.get();
}

ProjectItem* ProjectManager::createAudioItem(ProjectItem* parent, const QString& name,
                                              const QString& relativePath, uint32_t sourceID)
{
    if (!parent) return nullptr;

    // Import runs a per-file loop with its own LogPanel summary; fail silently
    // here instead of raising one errorOccurred dialog per rejected file.
    if (!validatePlacement(ProjectItem::Type::Sound, parent, nullptr, nullptr))
        return nullptr;

    {
        NameValidator::Result vr;
        if (!NameValidator::validateItemName(name, ProjectItem::Type::Sound, &vr)) {
            emit errorOccurred(QString("Invalid name '%1': %2")
                                   .arg(name, NameValidator::resultMessage(vr)));
            return nullptr;
        }
    }

    const QString createName = suggestUniqueName(ProjectItem::Type::Sound, name, nullptr, {}, parent);
    auto newItem = std::make_shared<ProjectItem>(createName, ProjectItem::Type::Sound, relativePath, sourceID);
    newItem->attachDefaultComponents();
    const auto authoring = validateNewAuthoring(*this, *newItem, parent);
    if (!authoring) {
        emit errorOccurred(authoring.message);
        return nullptr;
    }
    auto identityGuard = guardFieldGroupIdentities(m_currentProject, ProjectItem::Type::Sound);
    if (auto* sound = newItem->comp<VSound>(); sound && sourceID != 0) {
        SoundSourceDef source;
        source.entryId = QUuid::createUuid();
        source.sourceId = sourceID;
        sound->sources.plain.entries.push_back(source);
        sound->sources.plain.active = source.entryId;
        SoundSourceEditing::syncActiveProjection(
            *newItem, PlatformContextService::instance().context().primary,
            SoundSourceEditing::previewLanguage(
                *newItem, LanguageContextService::instance().language()));
    }

    if (m_currentProject) {
        newItem->objectID = m_currentProject->generateObjectID();
    }

    newItem->guid = QUuid::createUuid();

    if (!restoreItemInternal(parent, newItem)) {
        return nullptr;
    }
    m_currentProject->setObjectFileLocation(newItem.get(), m_currentProject->objectFolder(parent), {});
    identityGuard.dismiss();
    newItem->objectPath = computeObjectPath(parent, newItem->name);

    QString ownerName = QStringLiteral("NULL");
    if (m_currentProject) {
        if (auto* binding = findFileBindingForItem(*m_currentProject, newItem.get())) {
            if (binding->rootItem) ownerName = binding->rootItem->name;
        }
    }
    qDebug() << "[ProjectManager::createAudioItem] Created item:" << name
             << "sourceID:" << sourceID
             << "parent:" << parent->name
             << "ownerPack:" << ownerName
             << "parent->children.size():" << parent->children.size();
    
    emit itemCreated(newItem.get());
    return newItem.get();
}

ProjectItem* ProjectManager::createRootAudioItem(const QString& name,
                                                  const QString& relativePath,
                                                  uint32_t sourceID)
{
    if (!m_currentProject) return nullptr;

    NameValidator::Result vr;
    if (!NameValidator::validateItemName(name, ProjectItem::Type::Sound, &vr)) {
        emit errorOccurred(QString("Invalid name '%1': %2")
                               .arg(name, NameValidator::resultMessage(vr)));
        return nullptr;
    }

    std::vector<ProjectFileBinding>* targetBucket =
        m_currentProject->getBucketForType(ProjectItem::Type::Sound);
    if (!targetBucket) return nullptr;
    const QString targetFolderName = m_currentProject->folderNameForBucket(targetBucket);

    const QString createName = suggestUniqueName(ProjectItem::Type::Sound, name);
    auto newItem = std::make_shared<ProjectItem>(createName, ProjectItem::Type::Sound, relativePath, sourceID);
    newItem->attachDefaultComponents();
    if (!initializeRootFieldGroups(*m_currentProject, *newItem))
        return nullptr;
    auto identityGuard = guardFieldGroupIdentities(m_currentProject, ProjectItem::Type::Sound);
    if (auto* sound = newItem->comp<VSound>(); sound && sourceID != 0) {
        SoundSourceDef source;
        source.entryId = QUuid::createUuid();
        source.sourceId = sourceID;
        sound->sources.plain.entries.push_back(source);
        sound->sources.plain.active = source.entryId;
        SoundSourceEditing::syncActiveProjection(
            *newItem, PlatformContextService::instance().context().primary,
            SoundSourceEditing::previewLanguage(
                *newItem, LanguageContextService::instance().language()));
    }

    newItem->objectID = m_currentProject->generateObjectID();
    newItem->guid = QUuid::createUuid();

    newItem->parent = nullptr;
    newItem->markDirty();
    newItem->parentRef = ParentRef{};
    newItem->parentRef.kind = ParentRef::Kind::Root;

    ProjectFileBinding binding;
    binding.parentFolder = targetFolderName;
    binding.rootItem = newItem;
    targetBucket->push_back(std::move(binding));
    if (!validateFieldGroupProject(*m_currentProject).ok()
        || !refreshFieldGroupDerivedViews(*m_currentProject)) {
        targetBucket->pop_back();
        return nullptr;
    }

    identityGuard.dismiss();
    m_currentProject->setObjectFileLocation(newItem.get(), nullptr, {});
    newItem->objectPath = computeObjectPath(nullptr, newItem->name);
    emit itemCreated(newItem.get());
    return newItem.get();
}

void ProjectManager::applyRenameSideEffects(ProjectItem* item,
                                            const QString& fromName,
                                            const QString& toName)
{
    if (!item) return;

    auto& self = ProjectManager::instance();

    recomputeObjectPathRecursive(item);

    // Extension point ④: buses rename inside the engine BusTree; other runtime-
    // bearing types re-register under the new name.
    if (item->type == ProjectItem::Type::Bus) {
        AudioEngine::instance().renameBus(self.busRuntimeHandle(item), toName);
    } else if (hasRuntimePresence(item)) {
        unregisterItemFromRuntime(item);
        resyncItemToRuntime(item);
    }

    if (fromName != toName)
    {
        ProjectData* project = self.currentProject();
        if (project)
        {
            auto leafOf = [](const QString& name) {
                const QString trimmed = name.trimmed();
                const int lastSlash = trimmed.lastIndexOf('/');
                return lastSlash >= 0 ? trimmed.mid(lastSlash + 1) : trimmed;
            };
            std::vector<ProjectItem::Type> residualClaimants;
            std::function<void(ProjectItem*)> collectClaimants = [&](ProjectItem* pi) {
                if (!pi) return;
                if (pi != item && pi->name == fromName)
                    residualClaimants.push_back(pi->type);
                for (auto& child : pi->children) collectClaimants(child.get());
            };
            for (auto* binding : project->allFileBindings())
                if (binding && binding->rootItem)
                    collectClaimants(binding->rootItem.get());
            auto nameStillResolvesFor = [&](VActionStep::Type stepType) {
                for (ProjectItem::Type ct : residualClaimants)
                    if (VNX::EditorUtils::canActionStepReferenceObject(stepType, ct))
                        return true;
                return false;
            };
            std::function<void(ProjectItem*)> relinkActions = [&](ProjectItem* pi) {
                if (!pi) return;
                if (auto* ec = pi->comp<VAction>()) {
                    bool changed = false;
                    for (auto& action : ec->steps) {
                        const auto* stepCatalog =
                            VNX::EditorUtils::actionStepCatalogEntry(action.type);
                        if (stepCatalog && stepCatalog->supportsQuantize
                            && ItemTypeCatalog::isContentObject(item->type)
                            && action.quantize.referenceObjectID == item->objectID
                            && leafOf(action.quantize.referenceName) == fromName) {
                            action.quantize.referenceName = toName;
                            changed = true;
                        }
                        if (!VNX::EditorUtils::canActionStepReferenceObject(
                                action.type, item->type, item->objectID,
                                pi->objectID))
                            continue;
                        if (action.targetObjectID == item->objectID) {
                            if (leafOf(action.targetName) == fromName) {
                                action.targetName = toName;
                                changed = true;
                            }
                            continue;
                        }
                        if (action.targetObjectID != 0 || nameStillResolvesFor(action.type))
                            continue;
                        if (leafOf(action.targetName) == fromName) {
                            action.targetName = toName;
                            changed = true;
                        }
                    }
                    if (changed) {
                        pi->markDirty();
                        if (hasRuntimePresence(pi))
                            resyncItemToRuntime(pi);
                    }
                }
                for (auto& child : pi->children) relinkActions(child.get());
            };
            for (auto* binding : project->allFileBindings())
                if (binding && binding->rootItem)
                    relinkActions(binding->rootItem.get());
        }
    }

    if (item->type == ProjectItem::Type::Attenuation && fromName != toName)
    {
        ProjectData* project = self.currentProject();
        if (project)
            AttenuationStore::notifyReferencesChanged(
                project, item->guid, &self, {QStringLiteral("attenuationRef")});
    }

    if (fromName != toName)
        recordNamedObjectRenamed(self.currentProject(), *item, fromName);
}

void ProjectManager::recordCreatedNameHistory(ProjectItem* item)
{
    if (!m_currentProject || !item ||
        item->type != ProjectItem::Type::Action) return;
    const auto& history = m_currentProject->actionPublishedNameHistory;
    if (std::none_of(history.begin(), history.end(),
        [&](const auto& row) { return row.objectGuid == item->guid; }))
        recordNamedObjectCreated(m_currentProject.get(), *item);
}

void ProjectManager::recordDeletedNameHistory(ProjectItem* item)
{
    if (!item || !m_currentProject) return;
    recordNamedObjectDeleted(m_currentProject.get(), *item);
}

bool ProjectManager::renameItem(ProjectItem* item, const QString& newName, QString* error)
{
    if (!item) return false;
    const QString oldName = item->name;
    if (!renameItemInternal(item, newName, RenamePolicy::ResolveConflicts, error)) return false;
    if (oldName != item->name)
        UndoRedoManager::instance().push(new RenameProjectItemCommand(item, oldName, item->name));
    return true;
}

bool ProjectManager::validateItemRename(ProjectItem* item, const QString& newName,
                                        QString* error) const
{
    QString resolvedName;
    return prepareItemRename(item, newName, RenamePolicy::ResolveConflicts, resolvedName, error);
}

bool ProjectManager::prepareItemRename(ProjectItem* item, const QString& requestedName,
                                       RenamePolicy policy, QString& newName, QString* error) const
{
    if (error) error->clear();
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (!item || !m_currentProject || findItemByGuid(item->guid) != item)
        return fail(tr("The object no longer exists."));
    const bool resolveConflicts = policy == RenamePolicy::ResolveConflicts;
    newName = requestedName.normalized(QString::NormalizationForm_C);
    if (item->name == newName) return true;

    NameValidator::Result vr;
    if (!NameValidator::validateItemName(newName, item->type, &vr)) {
        return fail(QString("Invalid name '%1': %2")
                        .arg(newName, NameValidator::resultMessage(vr)));
    }
    if (resolveConflicts && (item->type == ProjectItem::Type::Action
                            || item->type == ProjectItem::Type::Variable)) {
        newName = suggestUniqueName(item->type, newName, item);
    }
    if (item->name == newName) return true;

    if (const ProjectItem* conflict = findNameConflict(newName, item->type, item)) {
        return fail(nameConflictMessage(newName, item->type, *conflict));
    }
    if (item->type == ProjectItem::Type::Action) {
        const auto lookup = EditorUtils::makeProjectTargetLookup(*m_currentProject);
        auto renamedLookup = lookup;
        renamedLookup.renameObject(item->objectID, newName);
        std::vector<const ProjectItem*> pending;
        for (const auto* binding : m_currentProject->allFileBindings())
            if (binding && binding->rootItem)
                pending.push_back(binding->rootItem.get());
        while (!pending.empty()) {
            const ProjectItem* current = pending.back();
            pending.pop_back();
            const auto& before = lookup.postActionSteps(current->objectID);
            const auto& after = renamedLookup.postActionSteps(current->objectID);
            for (size_t index = 0; index < after.size(); ++index) {
                const auto previousTarget = EditorUtils::resolveActionStepTarget(
                    before[index].type, before[index].targetObjectID,
                    before[index].targetName, lookup);
                const auto target = EditorUtils::resolveActionStepTarget(
                    after[index].type, after[index].targetObjectID,
                    after[index].targetName, renamedLookup);
                if (target.state != EditorUtils::ActionTargetState::Resolved
                    || (previousTarget.state == EditorUtils::ActionTargetState::Resolved
                        && previousTarget.objectID == target.objectID))
                    continue;
                const QString cycleError = EditorUtils::postActionCycleError(
                    current->objectID, {after[index]}, renamedLookup);
                if (!cycleError.isEmpty()) return fail(cycleError);
            }
            for (const auto& child : current->children)
                if (child) pending.push_back(child.get());
        }
    }
    return true;
}

bool ProjectManager::renameItemInternal(ProjectItem* item, const QString& requestedName,
                                       RenamePolicy policy, QString* error)
{
    if (!item || !m_currentProject || findItemByGuid(item->guid) != item) return false;
    QString newName;
    QString reason;
    if (!prepareItemRename(item, requestedName, policy, newName, &reason)) {
        if (error) *error = reason;
        else emit errorOccurred(reason);
        return false;
    }
    if (error) error->clear();
    if (item->name == newName) return true;

    QString oldName = item->name;
    item->name = newName;

    applyRenameSideEffects(item, oldName, newName);

    item->markDirty();

    emit itemRenamed(item, oldName);

    return true;
}

bool ProjectManager::isReferencedAttenuation(ProjectItem* item, int* refCount, QString* error)
{
    if (refCount) *refCount = 0;
    if (error) error->clear();
    if (!item || item->type != ProjectItem::Type::Attenuation)
        return false;
    ProjectData* project = ProjectManager::instance().currentProject();
    if (!project)
        return false;
    const VAttenuation* def =
        AttenuationStore::find(project, item->guid, item->name);
    if (!def)
        return false;
    const int refs = AttenuationStore::referenceCount(project, *def->hostItem(), error);
    if (refCount) *refCount = refs;
    return refs > 0;
}

namespace {
std::vector<DeletedSubtreeRefLog::FieldGroupRef> fieldGroupReferenceRemovals(
    ProjectData& project, ProjectItem* deletedRoot);
}

QList<ProjectItem*> ProjectManager::uniqueItemsByGuid(const QList<ProjectItem*>& items) const
{
    QList<ProjectItem*> unique;
    QSet<QUuid> seen;
    for (ProjectItem* item : items) {
        if (!item || item->guid.isNull() || findItemByGuid(item->guid) != item)
            return {};
        if (seen.contains(item->guid))
            continue;
        seen.insert(item->guid);
        unique.append(item);
    }
    return unique;
}

QList<ProjectItem*> ProjectManager::normalizeSubtreeItems(const QList<ProjectItem*>& items) const
{
    const QList<ProjectItem*> unique = uniqueItemsByGuid(items);
    QSet<ProjectItem*> selected(unique.begin(), unique.end());
    QList<ProjectItem*> roots;
    for (ProjectItem* item : unique) {
        bool covered = false;
        for (ProjectItem* parent = item->parent; parent; parent = parent->parent) {
            if (selected.contains(parent)) {
                covered = true;
                break;
            }
        }
        if (!covered) roots.append(item);
    }
    return roots;
}

QList<PhysicalFolder*> ProjectManager::normalizeFolderEntries(
    const QList<PhysicalFolder*>& folders) const
{
    if (!m_currentProject) return {};
    QSet<PhysicalFolder*> selected;
    for (PhysicalFolder* folder : folders) {
        if (m_currentProject->folderBucket(folder).isEmpty()) return {};
        selected.insert(folder);
    }
    QList<PhysicalFolder*> roots;
    for (PhysicalFolder* folder : folders) {
        if (roots.contains(folder)) continue;
        bool covered = false;
        for (PhysicalFolder* parent = folder->parent; parent; parent = parent->parent) {
            if (selected.contains(parent)) {
                covered = true;
                break;
            }
        }
        if (!covered) roots.append(folder);
    }
    return roots;
}

bool ProjectManager::canDeleteEntries(const QList<ProjectItem*>& items,
                                      const QList<PhysicalFolder*>& folders) const
{
    if (!m_currentProject) return false;
    const auto folderRoots = normalizeFolderEntries(folders);
    if (!folders.isEmpty() && folderRoots.isEmpty()) return false;
    QList<ProjectItem*> targets = uniqueItemsByGuid(items);
    if (!items.isEmpty() && targets.isEmpty()) return false;
    for (PhysicalFolder* folder : folderRoots)
        for (const auto* binding : m_currentProject->documentBindingsUnderFolder(folder))
            if (auto* item = findItemByGuid(binding->guid)) targets.append(item);
    return targets.isEmpty() ? !folderRoots.isEmpty() : canDeleteItems(targets);
}

namespace {
class DeleteFoldersCommand final : public TransactionalUndoCommand
{
public:
    DeleteFoldersCommand(const QList<PhysicalFolder*>& folders,
                         const QList<ProjectItem*>& items)
        : TransactionalUndoCommand(QObject::tr("Delete folders"), nullptr, false)
        , project_(ProjectManager::instance().currentProject()), items_(items)
    {
        for (auto* folder : folders) {
            Entry entry;
            entry.parent = folder->parent;
            if (entry.parent) {
                const auto& siblings = entry.parent->children;
                auto it = std::find_if(siblings.begin(), siblings.end(),
                    [folder](const auto& value) { return value.get() == folder; });
                entry.index = static_cast<int>(it - siblings.begin());
                entry.folder = *it;
            } else {
                const auto* binding = ProjectManager::findRootFolderBinding(project_, folder);
                entry.root.binding = *binding;
                entry.root.index = static_cast<int>(binding - project_->folders.data());
                entry.index = entry.root.index;
                entry.folder = binding->rootFolder;
            }
            folders_.push_back(std::move(entry));
        }
        std::stable_sort(folders_.begin(), folders_.end(),
            [](const auto& left, const auto& right) { return left.index < right.index; });
    }

private:
    struct Entry {
        std::shared_ptr<PhysicalFolder> folder;
        PhysicalFolder* parent = nullptr;
        RootFolderSlot root;
        int index = -1;
    };

    bool apply(bool redo) override
    {
        auto& manager = ProjectManager::instance();
        if (!project_ || manager.currentProject() != project_) return false;
        if (redo) {
            std::vector<std::pair<ProjectItem*, bool>> dirtyState;
            const auto capture = [&](const auto& self, ProjectItem* item) -> void {
                dirtyState.emplace_back(item, item->dirtyBit);
                for (const auto& child : item->children) self(self, child.get());
            };
            for (const auto* binding : project_->allFileBindings())
                capture(capture, binding->rootItem.get());
            size_t applied = 0;
            const auto revert = [&] {
                while (applied > 0) deletions_[--applied]->undo();
                for (const auto& [item, dirty] : dirtyState) {
                    if (item->dirtyBit == dirty) continue;
                    item->dirtyBit = dirty;
                    emit manager.itemPropertyChanged(item, QStringLiteral("dirty"));
                }
            };
            for (size_t index = 0; index < static_cast<size_t>(items_.size()); ++index) {
                auto* item = items_.at(static_cast<qsizetype>(index));
                if (manager.findItemByGuid(item->guid) != item) {
                    revert();
                    return false;
                }
                if (index == deletions_.size())
                    deletions_.push_back(std::make_unique<DeleteProjectItemCommand>(item));
                deletions_[index]->redo();
                if (manager.findItemByGuid(item->guid)) {
                    revert();
                    return false;
                }
                ++applied;
            }
            for (const auto& entry : folders_) {
                if (entry.parent) {
                    std::erase(entry.parent->children, entry.folder);
                } else {
                    RootFolderSlot ignored;
                    ProjectManager::detachRootFolderBinding(project_, entry.folder.get(), &ignored);
                }
                emit manager.folderDeleted(entry.folder.get());
            }
        } else {
            for (const auto& entry : folders_) {
                if (entry.parent) {
                    auto& siblings = entry.parent->children;
                    siblings.insert(siblings.begin() + std::min(entry.index, static_cast<int>(siblings.size())), entry.folder);
                } else {
                    ProjectManager::attachRootFolderBinding(project_, entry.root);
                }
                emit manager.folderCreated(entry.folder.get());
            }
            for (auto it = deletions_.rbegin(); it != deletions_.rend(); ++it) (*it)->undo();
        }
        return true;
    }

    ProjectData* project_;
    QList<ProjectItem*> items_;
    std::vector<Entry> folders_;
    std::vector<std::unique_ptr<DeleteProjectItemCommand>> deletions_;
};
}

bool ProjectManager::deleteEntries(const QList<ProjectItem*>& items,
                                  const QList<PhysicalFolder*>& folders)
{
    if (!canDeleteEntries(items, folders)) return false;
    const auto folderRoots = normalizeFolderEntries(folders);
    if (folderRoots.isEmpty()) {
        const auto roots = normalizeSubtreeItems(items);
        return deleteItems(roots) == roots.size();
    }
    auto& project = *m_currentProject;
    QList<ProjectItem*> selected = uniqueItemsByGuid(items);
    QSet<QUuid> contained;
    for (PhysicalFolder* folder : folderRoots) {
        for (const auto* binding : project.documentBindingsUnderFolder(folder)) {
            if (auto* item = findItemByGuid(binding->guid)) {
                selected.append(item);
                contained.insert(item->guid);
            }
        }
    }
    const auto gather = [&](const ProjectItem* item, QSet<QUuid>& guids, const auto& self) -> void {
        guids.insert(item->guid);
        for (const auto& child : item->children) self(child.get(), guids, self);
    };
    for (auto* item : items) gather(item, contained, gather);
    const auto roots = normalizeSubtreeItems(selected);
    QSet<QUuid> deletedGuids;
    for (auto* root : roots) gather(root, deletedGuids, gather);
    if (deletedGuids != contained) {
        emit errorOccurred(tr("A folder contains a parent whose children are stored outside the selected folders. Move those children or select their folders before deleting."));
        return false;
    }
    if (!roots.isEmpty() && !canDeleteItems(roots)) return false;
    auto command = std::make_unique<DeleteFoldersCommand>(folderRoots, roots);
    if (!command->tryReplay(true)) return false;
    UndoRedoManager::instance().push(command.release());
    return true;
}

bool ProjectManager::canDeleteItems(const QList<ProjectItem*>& items) const
{
    if (!m_currentProject) return false;
    const QList<ProjectItem*> roots = normalizeSubtreeItems(items);
    if (roots.isEmpty())
        return false;

    for (ProjectItem* item : roots) {
        if (isMonoRootBucketRoot(item))
            return false;

        QString referenceError;
        isReferencedAttenuation(item, nullptr, &referenceError);
        if (!referenceError.isEmpty())
            return false;

        if (m_currentProject) {
            std::vector<FieldGroupDefinitionSnapshot> candidates;
            for (const auto& change : fieldGroupReferenceRemovals(*m_currentProject, item))
                candidates.push_back({change.definition, change.after});
            if (!validateFieldGroupProject(*m_currentProject, candidates).ok())
                return false;
        }
    }
    return true;
}

bool ProjectManager::deleteItem(ProjectItem* item)
{
    return item && deleteItems({item}) == 1;
}

int ProjectManager::deleteItems(const QList<ProjectItem*>& items)
{
    const QList<ProjectItem*> roots = normalizeSubtreeItems(items);
    if (roots.isEmpty() || !canDeleteItems(items))
        return 0;

    const bool macro = roots.size() > 1;
    if (macro) UndoRedoManager::instance().beginMacro(tr("Delete %1 items").arg(roots.size()));
    for (ProjectItem* item : roots)
        UndoRedoManager::instance().push(new DeleteProjectItemCommand(item));
    if (macro) UndoRedoManager::instance().endMacro();
    return static_cast<int>(roots.size());
}

bool ProjectManager::moveItem(ProjectItem* item, ProjectItem* newParent)
{
    return item && moveItems({item}, newParent);
}

bool ProjectManager::moveItems(const QList<ProjectItem*>& items, ProjectItem* newParent)
{
    if (!m_currentProject || !newParent || findItemByGuid(newParent->guid) != newParent)
        return false;
    const auto unique = uniqueItemsByGuid(items);
    if (!items.isEmpty() && unique.isEmpty()) return false;
    QList<ProjectItem*> movedItems;
    std::vector<PhysicalItemMove> physicalMoves;
    for (auto* item : unique) {
        if (isMonoRootBucketRoot(item)) {
            emit errorOccurred(QString("Cannot move '%1': it is the root of a single-root bucket").arg(item->name));
            return false;
        }
        if (item->parent == newParent) continue;
        QString reason;
        if (!validateMoveConstraints(item, newParent, &reason)) {
            emit errorOccurred(reason);
            return false;
        }
        movedItems.push_back(item);
        if (ItemTypeCatalog::isFolderAddressable(item->type))
            physicalMoves.push_back({item, m_currentProject->objectFolder(newParent)});
    }
    std::vector<std::unique_ptr<TransactionalUndoCommand>> commands;
    for (auto* item : movedItems)
        commands.push_back(std::make_unique<MoveItemCommand>(item, newParent));
    return executePhysicalMoves(physicalMoves, {},
        makeMoveCommand(std::move(commands), tr("Move items")));
}

bool ProjectManager::moveItemToBucketRoot(ProjectItem* item, const QString& bucketFolder)
{
    if (!item) return false;
    if (!m_currentProject) { emit errorOccurred("No project loaded."); return false; }
    const auto authoring = validateNewAuthoring(*this, *item, nullptr);
    if (!authoring) {
        emit errorOccurred(authoring.message);
        return false;
    }

    if (isMonoRootBucketRoot(item)) {
        emit errorOccurred(QString("Cannot move '%1': it is the root of a single-root bucket").arg(item->name));
        return false;
    }

    const std::vector<ProjectItem::Type> allowed = allowedTopLevelTypesForBucket(bucketFolder);
    if (std::find(allowed.begin(), allowed.end(), item->type) == allowed.end()) {
        emit errorOccurred(QString("'%1' cannot be placed at the top level of '%2'.")
                               .arg(item->name, bucketFolder));
        return false;
    }

    ProjectFileBinding* owner = findFileBindingForItem(*m_currentProject, item);
    if (!owner || owner->parentFolder != bucketFolder) {
        emit errorOccurred(QString("'%1' belongs to a different section than '%2'.")
                               .arg(item->name, bucketFolder));
        return false;
    }
    if (!item->parent) return true;

    if (ItemTypeCatalog::isFolderAddressable(item->type))
        return moveEntriesToFolder({item}, {}, bucketFolder, {});
    std::vector<std::unique_ptr<TransactionalUndoCommand>> commands;
    commands.push_back(std::make_unique<MoveItemCommand>(item, bucketFolder));
    return pushMoveCommands(std::move(commands), tr("Move items"));
}

bool ProjectManager::setItemColor(ProjectItem* item, uint32_t color)
{
    if (!item) return false;
    return setItemsColor({item}, color);
}

bool ProjectManager::setItemsColor(const QList<ProjectItem*>& items, uint32_t color)
{
    const auto unique = uniqueItemsByGuid(items);
    if (unique.isEmpty()) return false;
    QList<ProjectItem*> changed;
    for (ProjectItem* item : unique) {
        if (item && item->color != color)
            changed.append(item);
    }
    if (changed.isEmpty())
        return true;

    const bool macro = changed.size() > 1;
    if (macro)
        UndoRedoManager::instance().beginMacro(tr("Set Color"));
    for (ProjectItem* item : changed) {
        const uint32_t oldColor = item->color;
        item->color = color;
        item->markDirty();
        emit itemPropertyChanged(item, QStringLiteral("color"));
        UndoRedoManager::instance().push(new SetItemColorCommand(item, oldColor, color));
    }
    if (macro)
        UndoRedoManager::instance().endMacro();
    return true;
}

bool ProjectManager::canCreateItem(ProjectItem* parent, ProjectItem::Type type,
                                    const QString& bucketFolderHint) const
{
    if (!m_currentProject) return false;
    if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(type)
        && !validateFieldGroupProject(*m_currentProject).ok()) return false;
    if (!parent) {
        const auto* facts = ItemTypeCatalog::factsOrNull(type);
        return facts && facts->topLevelCreatable && m_currentProject->getBucketForType(type);
    }
    if (findItemByGuid(parent->guid) != parent
        || !validatePlacement(type, parent, nullptr, nullptr)
        || !validateStructuralPlacement(type, parent)) return false;
    if (parent->type == ProjectItem::Type::Track || parent->type == ProjectItem::Type::Piece) {
        return static_cast<bool>(AuthoringPolicy::validateTimeline(*parent,
            [this](const QUuid& guid) { return findItemByGuid(guid); },
            [this](uint32_t id) { return findItemByObjectID(id); }));
    }
    return true;
}

bool ProjectManager::canCreateParentContainer(const QList<ProjectItem*>& selection,
                                              ProjectItem::Type containerType,
                                              QString* error) const
{
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (!m_currentProject || selection.isEmpty())
        return fail(QStringLiteral("Select objects to group in an open project."));
    ProjectItem* commonParent = selection.front() ? selection.front()->parent : nullptr;
    if (!commonParent || !canCreateItem(commonParent, containerType))
        return fail(QStringLiteral("That parent cannot be created here."));
    for (ProjectItem* sel : selection) {
        if (!sel || findItemByGuid(sel->guid) != sel || sel->parent != commonParent
            || !canBeChildOf(sel->type, containerType))
            return fail(QStringLiteral("Select compatible sibling objects under one parent to group."));
        const auto authoring = validateNewAuthoring(*this, *sel, commonParent);
        if (!authoring) return fail(authoring.message);
        if (!validatePlaybackReferences(*sel, error)) return false;
        const int depth = countContainerAncestors(commonParent)
            + (containerType == ProjectItem::Type::Layer ? 1 : 0);
        if (computeMaxContainerDepthAfterAttach(sel, depth) > MAX_CONTAINER_DEPTH)
            return fail(QStringLiteral("Container nesting depth would exceed its limit."));
        if (containerType == ProjectItem::Type::Bus) {
            std::function<bool(const ProjectItem*, int)> fits = [&](const ProjectItem* node, int level) {
                if (level > 8) return false;
                for (const auto& child : node->children)
                    if (!fits(child.get(), level + 1)) return false;
                return true;
            };
            int level = 2;
            for (auto* parent = commonParent; parent; parent = parent->parent) ++level;
            if (!fits(sel, level))
                return fail(QStringLiteral("Bus hierarchy depth limit (8) exceeded."));
        }
    }
    return true;
}

ProjectItem* ProjectManager::createParentContainer(const QList<ProjectItem*>& selection,
                                                   ProjectItem::Type containerType,
                                                   const QString& name)
{
    QString error;
    if (!canCreateParentContainer(selection, containerType, &error)) {
        emit errorOccurred(error);
        return nullptr;
    }
    const auto targets = uniqueItemsByGuid(selection);
    ProjectItem* commonParent = targets.front()->parent;
    NameValidator::Result validity;
    if (!NameValidator::validateItemName(name, containerType, &validity)) {
        emit errorOccurred(NameValidator::resultMessage(validity));
        return nullptr;
    }
    std::vector<PhysicalItemMove> physicalMoves;
    for (auto* item : targets)
        if (ItemTypeCatalog::isFolderAddressable(item->type))
            physicalMoves.push_back({item, m_currentProject->objectFolder(commonParent)});
    auto command = std::make_unique<CreateParentContainerCommand>(targets, containerType, name);
    const auto* creation = command.get();
    if (!executePhysicalMoves(physicalMoves, {}, std::move(command))) return nullptr;
    return creation->container();
}

ProjectItem* ProjectManager::createActionTargeting(ProjectItem* target,
                                                  VActionStep::Type actionType,
                                                  const QString& actionName)
{
    if (!m_currentProject) { emit errorOccurred("No project loaded."); return nullptr; }
    if (!target || target->objectID == 0) {
        emit errorOccurred("Select an object for the new Action to target.");
        return nullptr;
    }
    const auto authoring = validateNewAuthoring(*this, *target, nullptr);
    if (!authoring) {
        emit errorOccurred(authoring.message);
        return nullptr;
    }

    if (target->type == ProjectItem::Type::Action) {
        const auto lookup = EditorUtils::makeProjectTargetLookup(*m_currentProject);
        const QString cycleError = EditorUtils::postActionCycleError(
            target->objectID, lookup.postActionSteps(target->objectID), lookup);
        if (!cycleError.isEmpty()) {
            emit errorOccurred(cycleError);
            return nullptr;
        }
    }

    if (!VNX::EditorUtils::canActionStepReferenceObject(
            actionType, target->type, target->objectID)) {
        VActionStep::Type defaultType = VActionStep::Type::Play;
        if (!VNX::EditorUtils::defaultActionStepTypeForTarget(target->type, defaultType)) {
            emit errorOccurred("Selected object cannot be targeted by a new Action.");
            return nullptr;
        }
        actionType = defaultType;
    }

    UndoRedoManager::instance().beginMacro(tr("New Action"));
    const QString uniqueName = suggestUniqueName(ProjectItem::Type::Action, actionName);
    ProjectItem* actionItem = createItem(nullptr, uniqueName, ProjectItem::Type::Action);
    if (!actionItem) {
        UndoRedoManager::instance().endMacro();
        emit errorOccurred("Failed to create Action.");
        return nullptr;
    }
    VActionStep action;
    action.type = actionType;
    action.targetObjectID = target->objectID;
    action.targetName = target->name;

    const uint32_t actionObjectID = actionItem->objectID;
    const std::vector<VActionStep> newSteps{action};
    applyStepsList(actionObjectID, newSteps);

    UndoRedoManager::instance().push(new RuntimeActionCommand(
        tr("New Action"),
        [actionObjectID]() { applyStepsList(actionObjectID, {}); },
        [actionObjectID, newSteps]() { applyStepsList(actionObjectID, newSteps); }));
    UndoRedoManager::instance().endMacro();
    return actionItem;
}

bool ProjectManager::canCreateActionsTargeting(const QList<ProjectItem*>& targets,
                                               VActionStep::Type actionType) const
{
    const QList<ProjectItem*> unique = uniqueItemsByGuid(targets);
    if (unique.isEmpty())
        return false;
    for (ProjectItem* target : unique) {
        if (!target || target->objectID == 0)
            return false;
        if (!ItemTypeCatalog::isUserFacingType(target->type))
            return false;
        if (!VNX::EditorUtils::canActionStepReferenceObject(
                actionType, target->type, target->objectID)) {
            return false;
        }
        if (!validateNewAuthoring(*this, *target, nullptr))
            return false;
        if (target->type == ProjectItem::Type::Action) {
            const auto lookup = EditorUtils::makeProjectTargetLookup(*m_currentProject);
            if (!EditorUtils::postActionCycleError(
                    target->objectID, lookup.postActionSteps(target->objectID), lookup).isEmpty())
                return false;
        }
    }
    return true;
}

std::vector<ProjectItem*> ProjectManager::createActionsTargeting(
    const QList<ProjectItem*>& targets, VActionStep::Type actionType)
{
    std::vector<ProjectItem*> result;
    if (!m_currentProject) {
        emit errorOccurred("No project loaded.");
        return result;
    }
    const QList<ProjectItem*> unique = uniqueItemsByGuid(targets);
    if (!canCreateActionsTargeting(unique, actionType))
        return result;

    auto actionNamePrefix = [](VActionStep::Type type) {
        switch (type) {
            case VActionStep::Type::Play:       return QStringLiteral("Play");
            case VActionStep::Type::Stop:       return QStringLiteral("Stop");
            case VActionStep::Type::SetVariable:   return QStringLiteral("SetVariable");
            case VActionStep::Type::PostAction: return QStringLiteral("TriggerAction");
            default: {
                VActionStep action;
                action.type = type;
                QString prefix = action.typeString();
                prefix.remove(QChar(' '));
                prefix.remove(QChar('('));
                prefix.remove(QChar(')'));
                return prefix;
            }
        }
    };

    const bool macro = unique.size() > 1;
    if (macro) {
        UndoRedoManager::instance().beginMacro(
            tr("New Action (%1)").arg(unique.size()));
    }
    for (ProjectItem* target : unique) {
        QString name = QStringLiteral("%1_%2")
                                 .arg(actionNamePrefix(actionType), target->name)
                                 .left(NameValidator::MAX_NAME_LENGTH - 5);
        name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_]+")), QStringLiteral("_"));
        while (name.endsWith(QLatin1Char('_'))) name.chop(1);
        ProjectItem* created = createActionTargeting(target, actionType, name);
        if (!created) {
            if (macro)
                UndoRedoManager::instance().endMacro();
            emit errorOccurred("Failed to create Action.");
            return result;
        }
        result.push_back(created);
    }
    if (macro)
        UndoRedoManager::instance().endMacro();
    return result;
}



bool ProjectManager::isMonoRootBucketRoot(const ProjectItem* item) const
{
    if (!item) return false;
    if (item->parent != nullptr) return false;
    if (!m_currentProject) return false;
    if (m_currentProject->busses.empty()) return false;
    return item == m_currentProject->busses[0].rootItem.get();
}

bool ProjectManager::ensureTrackChildClip(ProjectItem* track, ProjectItem* child)
{
    const auto* timeline = track->comp<VTrack>();
    if (!timeline || std::any_of(timeline->clips.items.begin(), timeline->clips.items.end(),
            [child](const Clip& clip) { return clip.targetGuid == child->guid; }))
        return true;
    Clip clip;
    clip.clipGuid = QUuid::createUuid();
    clip.targetGuid = child->guid;
    clip.duration = 1000000000LL;
    const auto result = TimelineEditing::addClip(this, track, std::move(clip));
    if (!result.success)
        emit errorOccurred(result.errorMessage);
    return result.success;
}

bool ProjectManager::restoreItemInternal(
    ProjectItem* parent,
    const std::shared_ptr<ProjectItem>& item,
    int insertIndex,
    const std::optional<SelectionEntry>& restoredOccurrence,
    const std::vector<SelectionEntry>* restoredOccurrences,
    bool createDefaultClip)
{
    if (!parent || !item) return false;
    if (!validatePlaybackReferences(*item)) return false;
    if (!PlayablePropertyAuthoringV1::validateContainerOccurrences(*parent))
        return false;

    // Silent last-line placement guard shared with the move path: rejects
    // structurally illegal parent/child (e.g. a Sound under a Bus). The three
    // creation/copy funnels (createItem child, createAudioItem, paste-copy) all
    // return this false without emitting, so an illegal placement simply does
    // not create the item — no dialog, no half-built node.
    if (!validatePlacement(item->type, parent, item.get(), nullptr)) return false;

    if (parent->type != ProjectItem::Type::Container
        && (restoredOccurrence || restoredOccurrences))
        return false;
    const auto* parentSelection = parent->comp<VSelection>();
    const auto oldSelections = parentSelection ? std::optional(parentSelection->selections) : std::nullopt;
    ProjectItem* oldParent = item->parent;
    const ParentRef oldParentRef = item->parentRef;
    item->parent = parent;
    const int childCount = static_cast<int>(parent->children.size());
    const int installedIndex = insertIndex >= 0 && insertIndex <= childCount
        ? insertIndex : childCount;
    parent->children.insert(parent->children.begin() + installedIndex, item);
    const auto rollback = [&] {
        parent->children.erase(parent->children.begin() + installedIndex);
        if (oldSelections) parent->comp<VSelection>()->selections = *oldSelections;
        item->parent = oldParent;
        item->parentRef = oldParentRef;
    };
    if (parent->type == ProjectItem::Type::Container) {
        std::vector<SelectionEntry> rows;
        bool restoreExactRows = false;
        if (restoredOccurrences) {
            rows = *restoredOccurrences;
            restoreExactRows = true;
        } else if (restoredOccurrence) {
            rows = {*restoredOccurrence};
            restoreExactRows = true;
        }
        if (rows.empty()) {
            if (!restoreExactRows
                && !PlayablePropertyAuthoringV1::addContainerChildOccurrence(
                    *parent, item->guid)) {
                rollback();
                return false;
            }
        } else {
            for (const auto& row : rows) {
                if (!PlayablePropertyAuthoringV1::addContainerChildOccurrence(
                        *parent, item->guid, row)) {
                    rollback();
                    return false;
                }
            }
        }
    }

    const bool hasFieldGroups =
        PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item->type);
    if (hasFieldGroups) {
        item->parentRef = ParentRef{};
        if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(parent->type)) {
            item->parentRef.kind = ParentRef::Kind::Item;
            item->parentRef.guid = parent->guid;
            item->parentRef.documentId = parent->guid;
        }
    }

    if (!PlayablePropertyAuthoringV1::validateContainerOccurrences(*parent)
        || (hasFieldGroups && (!m_currentProject
            || !validateFieldGroupProject(*m_currentProject).ok()
            || !refreshFieldGroupDerivedViews(*m_currentProject)))) {
        rollback();
        return false;
    }

    if (createDefaultClip && !ensureTrackChildClip(parent, item.get())) {
        rollback();
        return false;
    }

    item->markDirty();
    parent->markDirty();

    // Extension point ④: buses mirror into the engine BusTree at insertion time,
    // covering both fresh creation and undo/redo restore (which both funnel
    // through here). Other types are driven by the item* runtime-sync signals,
    // so they are left untouched.
    if (item->type == ProjectItem::Type::Bus) {
        materializeBusSubtree(item.get(), busRuntimeHandle(parent));
    }
    return true;
}

ProjectItem* findItemByGuidInProject(ProjectData* project, const QUuid& guid)
{
    if (!project || guid.isNull()) return nullptr;
    ProjectItem* found = nullptr;
    std::function<void(ProjectItem*)> walk = [&](ProjectItem* pi) {
        if (!pi || found) return;
        if (pi->guid == guid) { found = pi; return; }
        for (auto& child : pi->children) walk(child.get());
    };
    for (auto* binding : project->allFileBindings())
        if (binding && binding->rootItem && !found)
            walk(binding->rootItem.get());
    return found;
}

ProjectItem* ProjectManager::findItemByGuid(const QUuid& guid) const
{
    return findItemByGuidInProject(m_currentProject.get(), guid);
}

ProjectItem* ProjectManager::findItemByObjectID(uint32_t objectID) const
{
    return findItemByObjectIDInProject(m_currentProject.get(), objectID);
}

std::vector<ProjectItem*> ProjectManager::findItemsByObjectID(
    std::span<const uint32_t> objectIDs) const
{
    return findItemsByObjectIDInProject(m_currentProject.get(), objectIDs);
}

ProjectItem* ProjectManager::findItemByRuntimeStableId(harkenstudio::ObjectId runtimeStableId) const
{
    if (!m_currentProject || harkenstudio::IsNull(runtimeStableId)) return nullptr;
    ProjectItem* found = nullptr;
    bool ambiguous = false;
    std::function<void(ProjectItem*)> walk = [&](ProjectItem* item) {
        if (!item || ambiguous) return;
        if (item->runtimeId() == runtimeStableId) {
            if (found) {
                ambiguous = true;
                return;
            }
            found = item;
        }
        for (auto& child : item->children) walk(child.get());
    };
    for (auto* binding : m_currentProject->allFileBindings())
        if (binding && binding->rootItem && !ambiguous)
            walk(binding->rootItem.get());
    return ambiguous ? nullptr : found;
}


bool ProjectManager::isClipboardEligible(const ProjectItem* it)
{
    if (!it) return false;
    if (it->type == ProjectItem::Type::Bus) return false;
    if (it->isSpatialObject()) return false;
    if (!ItemTypeCatalog::isUserFacingType(it->type)) return false;
    return true;
}

bool ProjectManager::canSetClipboard(const QList<ProjectItem*>& items) const
{
    if (!m_currentProject) return false;
    const QList<ProjectItem*> roots = normalizeSubtreeItems(items);
    if (roots.isEmpty())
        return false;
    for (ProjectItem* it : roots) {
        if (!isClipboardEligible(it) || it->guid.isNull())
            return false;
        if (!validateNewAuthoring(*this, *it, nullptr))
            return false;
        if (!validatePlaybackReferences(*it, nullptr))
            return false;
    }
    return true;
}

int ProjectManager::setClipboard(const QList<ProjectItem*>& items, ClipboardOp op)
{
    if (op == ClipboardOp::None) { clearClipboard(); return 0; }
    if (!canSetClipboard(items))
        return 0;

    const QList<ProjectItem*> roots = normalizeSubtreeItems(items);
    std::vector<QUuid> guids;
    std::vector<ClipboardCopy> copies;
    std::unique_ptr<ProjectSerializer> serializer;
    if (op == ClipboardOp::Copy) {
        serializer = std::make_unique<ProjectSerializer>();
        if (!serializer->prepareReferenceWrite(*m_currentProject)) {
            emit errorOccurred(serializer->lastError());
            return 0;
        }
        copies.reserve(static_cast<size_t>(roots.size()));
    }
    guids.reserve(static_cast<size_t>(roots.size()));
    for (ProjectItem* it : roots) {
        guids.push_back(it->guid);
        if (serializer) {
            const auto json = serializer->projectItemToJson(*it);
            auto snapshot = std::make_shared<ProjectItem>();
            if (json.isEmpty() || !serializer->projectItemFromJson(json, *snapshot)) {
                emit errorOccurred(serializer->lastError());
                return 0;
            }
            auto groups = PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(it->type)
                ? captureRootFieldGroups(it) : std::nullopt;
            copies.push_back({std::move(snapshot), std::move(groups)});
        }
    }

    m_clipboardOp    = op;
    m_clipboardGuids = std::move(guids);
    m_clipboardCopies = std::move(copies);
    m_clipboardSerializer = std::move(serializer);
    return static_cast<int>(m_clipboardGuids.size());
}

void ProjectManager::clearClipboard()
{
    m_clipboardOp = ClipboardOp::None;
    m_clipboardGuids.clear();
    m_clipboardCopies.clear();
    m_clipboardSerializer.reset();
}

std::vector<const ProjectItem*> ProjectManager::resolveClipboardSources() const
{
    std::vector<const ProjectItem*> out;
    if (!m_currentProject) return out;
    out.reserve(m_clipboardGuids.size());
    if (m_clipboardOp == ClipboardOp::Copy) {
        for (const auto& copy : m_clipboardCopies)
            out.push_back(copy.item.get());
        return out;
    }
    for (const QUuid& g : m_clipboardGuids) {
        if (ProjectItem* it = findItemByGuidInProject(m_currentProject.get(), g))
            out.push_back(it);
    }
    return out;
}

ProjectItem* ProjectManager::resolvePasteParent(ProjectItem* target, ProjectItem::Type childType) const
{
    if (!target) return nullptr;
    if (canBeChildOf(childType, target->type)) return target;
    if (target->parent && canBeChildOf(childType, target->parent->type))
        return target->parent;
    return nullptr;
}

std::shared_ptr<ProjectItem> ProjectManager::cloneSubtreeDetached(
    const ProjectItem* src, const ProjectItem* parent, const QString& folderPath,
    const ProjectSerializer* snapshotSerializer)
{
    if (!src || !m_currentProject) return nullptr;
    const auto authoring = validateNewAuthoring(*this, *src, nullptr);
    if (!authoring) {
        emit errorOccurred(authoring.message);
        return nullptr;
    }
    QString referenceError;
    if (!validatePlaybackReferences(*src, &referenceError)) {
        emit errorOccurred(referenceError);
        return nullptr;
    }
    if (!snapshotSerializer && !m_serializer.prepareReferenceWrite(*m_currentProject)) {
        emit errorOccurred(m_serializer.lastError());
        return nullptr;
    }

    const auto& serializer = snapshotSerializer ? *snapshotSerializer : m_serializer;
    const QJsonObject j = serializer.projectItemToJson(*src);
    if (j.isEmpty()) {
        emit errorOccurred(serializer.lastError());
        return nullptr;
    }
    auto clone = std::make_shared<ProjectItem>();
    if (!serializer.projectItemFromJson(j, *clone)) {
        emit errorOccurred(serializer.lastError());
        return nullptr;
    }

    QHash<QUuid, QUuid> guidMap;
    std::function<void(const ProjectItem*, ProjectItem*)> remint =
        [&](const ProjectItem* source, ProjectItem* node) {
        if (!source || !node) return;
        const QUuid oldGuid = source->guid;
        node->guid     = QUuid::createUuid();
        guidMap.insert(oldGuid, node->guid);
        node->objectID = m_currentProject->generateObjectID();
        if (node->type == ProjectItem::Type::Action || node->type == ProjectItem::Type::Variable)
            node->nameBindingRevision = 1;
        node->objectPath.clear();
        const size_t count = std::min(source->children.size(), node->children.size());
        for (size_t i = 0; i < count; ++i)
            remint(source->children[i].get(), node->children[i].get());
    };
    remint(src, clone.get());
    clone->name = suggestUniqueName(src->type, src->name, nullptr, {}, parent, folderPath);
    std::function<void(ProjectItem*)> remintInternalEntities = [&](ProjectItem* node) {
        if (!node) return;
        if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(node->type)) {
            node->parentRef = ParentRef{};
            if (node->parent
                && PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(node->parent->type)) {
                node->parentRef.kind = ParentRef::Kind::Item;
                node->parentRef.guid = node->parent->guid;
                node->parentRef.documentId = node->parent->guid;
            }
        }
        if (node->type == ProjectItem::Type::Container) {
            for (auto* table : node->comp<VSelection>()->selections.tables())
            for (auto& row : table->entries) {
                const auto mapped = guidMap.constFind(row.childGuid);
                if (mapped != guidMap.cend()) row.childGuid = mapped.value();
            }
        }
        if (auto* selection = node->comp<VSelection>()) {
            QHash<QUuid, QUuid> rowGuids;
            for (auto* table : selection->selections.tables()) {
                for (auto& axis : table->matchAxes) axis = guidMap.value(axis, axis);
                for (auto& row : table->mixingRows) {
                    if (!rowGuids.contains(row.guid)) rowGuids.insert(row.guid, QUuid::createUuid());
                    row.guid = rowGuids.value(row.guid);
                    row.variable = guidMap.value(row.variable, row.variable);
                }
                for (auto& entry : table->entries)
                    for (auto& curve : entry.curves) curve.rowGuid = rowGuids.value(curve.rowGuid);
            }
            for (TransitionRuleDef& rule : selection->transitions.rules) {
                for (auto* reference : {&rule.source, &rule.target, &rule.bridge}) {
                    if (!*reference) continue;
                    const auto mapped = guidMap.constFind(**reference);
                    if (mapped != guidMap.cend())
                        *reference = mapped.value();
                }
            }
        }
        if (auto* sound = node->comp<VSound>())
            SoundSourceSemantics::remintEntries(*sound);
        if (auto* mix = node->comp<VMixNode>()) {
            auto bindings = mix->bindings.entries();
            for (auto& binding : bindings) binding.guid = QUuid::createUuid();
            mix->bindings.replace(std::move(bindings));
            auto effects = mix->effects.entries();
            for (auto& effect : effects) effect.guid = QUuid::createUuid();
            mix->effects.replace(std::move(effects));
        }
        if (auto* timeline = node->comp<VTrack>()) {
            for (Clip& clip : timeline->clips.items) {
                clip.clipGuid = QUuid::createUuid();
                const auto target = guidMap.constFind(clip.targetGuid);
                if (target != guidMap.cend()) clip.targetGuid = target.value();
            }
        }
        if (auto* timeline = node->comp<VPiece>())
            for (Marker& marker : timeline->markers.items) {
                marker.markerGuid = QUuid::createUuid();
            }
        for (auto& child : node->children)
            remintInternalEntities(child.get());
    };
    remintInternalEntities(clone.get());
    if (!validatePlaybackReferences(*clone, &referenceError)) {
        emit errorOccurred(referenceError);
        return nullptr;
    }
    clone->parent = nullptr;
    if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(clone->type))
        clone->parentRef = ParentRef{};
    PhysicalFolder* folder = parent ? m_currentProject->objectFolder(parent)
        : m_currentProject->findFolderByPath(
            ItemTypeCatalog::folderNameOf(ItemTypeCatalog::bucketOf(clone->type)), folderPath);
    const auto locate = [&](auto&& self, const ProjectItem* item) -> void {
        m_currentProject->setObjectFileLocation(item, folder, {});
        for (const auto& child : item->children)
            if (child) self(self, child.get());
    };
    locate(locate, clone.get());
    return clone;
}

bool ProjectManager::canPasteClipboard(ProjectItem* target) const
{
    if (!m_currentProject || !target || clipboardEmpty())
        return false;
    const auto sources = resolveClipboardSources();
    if (sources.empty() || sources.size() != m_clipboardGuids.size())
        return false;

    std::vector<FieldGroupParentSnapshot> placements;
    std::vector<FieldGroupCopySnapshot> copies;
    for (const ProjectItem* src : sources) {
        if (!src || !ItemTypeCatalog::isUserFacingType(src->type))
            return false;
        ProjectItem* parent = resolvePasteParent(target, src->type);
        if (!parent)
            return false;
        if (!PlayablePropertyAuthoringV1::validateContainerOccurrences(*parent)) return false;
        if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(src->type)) {
            if (m_clipboardOp == ClipboardOp::Cut)
                placements.push_back({src->guid, parent->guid});
            else
                copies.push_back({src, parent->guid});
        }
        if (m_clipboardOp == ClipboardOp::Cut) {
            QString reason;
            if (!validateMoveConstraints(src, parent, &reason))
                return false;
        } else {
            if (!validatePlacement(src->type, parent, src, nullptr)
                || !validatePlaybackReferences(*src, nullptr))
                return false;
            if (!validateNewAuthoring(*this, *src, parent))
                return false;
            if (src->type == ProjectItem::Type::Bus) {
                QString reason;
                if (!validateStructuralPlacement(src->type, parent, &reason, nullptr))
                    return false;
            }
        }
    }
    return (placements.empty() && copies.empty())
        || validateFieldGroupProject(*m_currentProject, {}, placements, copies).ok();
}

bool ProjectManager::canPasteClipboardAtBucketRoot(const QString& bucketFolder) const
{
    if (!m_currentProject || clipboardEmpty())
        return false;
    if (m_clipboardOp == ClipboardOp::Cut)
        return false;
    if (!m_currentProject->getBucketForFolder(bucketFolder))
        return false;
    if (m_clipboardCopies.empty())
        return false;
    const std::vector<ProjectItem::Type> allowed = allowedTopLevelTypesForBucket(bucketFolder);
    std::vector<FieldGroupCopySnapshot> copies;
    for (const auto& copy : m_clipboardCopies) {
        const ProjectItem* src = copy.item.get();
        if (!src || !ItemTypeCatalog::isUserFacingType(src->type))
            return false;
        if (!validateNewAuthoring(*this, *src, nullptr))
            return false;
        if (!validatePlaybackReferences(*src, nullptr))
            return false;
        if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(src->type)) {
            if (!copy.rootGroups) return false;
            copies.push_back({src, {}, &*copy.rootGroups});
        }
        if (std::find(allowed.begin(), allowed.end(), src->type) == allowed.end())
            return false;
    }
    return copies.empty() || validateFieldGroupProject(*m_currentProject, {}, {}, copies).ok();
}

std::vector<ProjectItem*> ProjectManager::pasteClipboard(ProjectItem* target)
{
    std::vector<ProjectItem*> result;
    if (!m_currentProject) { emit errorOccurred("No project loaded."); return result; }
    if (!target)           { emit errorOccurred("Select a destination to paste into."); return result; }
    if (clipboardEmpty())  { emit errorOccurred("Clipboard is empty."); return result; }
    if (!canPasteClipboard(target)) {
        emit errorOccurred("Cannot paste here.");
        return result;
    }

    const auto sources = resolveClipboardSources();
    if (sources.empty()) {
        emit errorOccurred("Clipboard sources no longer exist.");
        clearClipboard();
        return result;
    }

    if (m_clipboardOp == ClipboardOp::Cut) {
        struct PastePair { ProjectItem* src; ProjectItem* parent; };
        std::vector<PastePair> pairs;
        pairs.reserve(sources.size());
        for (const ProjectItem* source : sources) {
            ProjectItem* src = findItemByGuid(source->guid);
            ProjectItem* parent = resolvePasteParent(target, src->type);
            QString reason;
            if (!parent || !validateMoveConstraints(src, parent, &reason)) {
                emit errorOccurred(reason.isEmpty()
                    ? QString("Cannot paste '%1' here.").arg(src->name)
                    : reason);
                return result;
            }
            pairs.push_back({ src, parent });
        }
        std::vector<PhysicalItemMove> physicalMoves;
        for (const PastePair& p : pairs) {
            if (p.src->parent != p.parent && ItemTypeCatalog::isFolderAddressable(p.src->type))
                physicalMoves.push_back({p.src, m_currentProject->objectFolder(p.parent)});
        }
        std::vector<std::unique_ptr<TransactionalUndoCommand>> moves;
        for (const PastePair& p : pairs)
            if (p.src->parent != p.parent)
                moves.push_back(std::make_unique<MoveItemCommand>(p.src, p.parent));
        if (!executePhysicalMoves(physicalMoves, {},
                makeMoveCommand(std::move(moves), tr("Cut / Paste")))) return result;
        for (const auto& pair : pairs) result.push_back(pair.src);
        if (!result.empty()) clearClipboard();
        return result;
    }

    struct PastePair { const ProjectItem* src; ProjectItem* parent; };
    std::vector<PastePair> pairs;
    pairs.reserve(sources.size());
    for (const ProjectItem* src : sources) {
        ProjectItem* parent = resolvePasteParent(target, src->type);
        const auto authoring = validateNewAuthoring(*this, *src, parent);
        if (!authoring) {
            emit errorOccurred(authoring.message);
            return result;
        }
        if (!parent) {
            emit errorOccurred(QString("Cannot paste '%1' here.").arg(src->name));
            return result;
        }
        // Bus fan-out / depth limits apply to a pasted clone exactly as they do
        // to a moved bus; the send-loop sub-check is a move-only concern (a fresh
        // clone has no incoming edges yet), so instance is null here.
        if (src->type == ProjectItem::Type::Bus) {
            QString reason;
            if (!validateStructuralPlacement(src->type, parent, &reason, nullptr)) {
                emit errorOccurred(reason);
                return result;
            }
        }
        pairs.push_back({ src, parent });
    }
    if (pairs.empty()) {
        emit errorOccurred("Nothing could be pasted into the selected destination.");
        return result;
    }

    bool pasteMacroStarted = false;
    for (const PastePair& p : pairs) {
        auto identityGuard = guardFieldGroupIdentities(m_currentProject, p.src->type);
        auto clone = cloneSubtreeDetached(p.src, p.parent, {}, m_clipboardSerializer.get());
        if (!clone || !restoreItemInternal(p.parent, clone)) {
            emit errorOccurred(QString("Failed to paste '%1'; the batch was stopped.").arg(p.src->name));
            break;
        }
        identityGuard.dismiss();

        recomputeObjectPathRecursive(clone.get());

        if (!pasteMacroStarted) {
            UndoRedoManager::instance().beginMacro(tr("Paste"));
            pasteMacroStarted = true;
        }
        emit itemCreated(clone.get());
        UndoRedoManager::instance().push(new CreateProjectItemCommand(p.parent, clone));
        requestResync(clone.get());
        result.push_back(clone.get());
    }
    if (pasteMacroStarted) UndoRedoManager::instance().endMacro();
    return result;
}

std::vector<ProjectItem*> ProjectManager::pasteClipboardAtBucketRoot(const QString& bucketFolder)
{
    std::vector<ProjectItem*> result;
    if (!m_currentProject) { emit errorOccurred("No project loaded."); return result; }
    if (clipboardEmpty())  { emit errorOccurred("Clipboard is empty."); return result; }
    if (m_clipboardOp == ClipboardOp::Cut) {
        emit errorOccurred("Cut objects need an object destination; top-level paste only accepts copied objects.");
        return result;
    }

    auto* bucket = m_currentProject->getBucketForFolder(bucketFolder);
    if (!bucket) {
        emit errorOccurred(QString("Unknown bucket folder '%1'.").arg(bucketFolder));
        return result;
    }

    if (!canPasteClipboardAtBucketRoot(bucketFolder)) {
        emit errorOccurred("Nothing could be pasted at the top level of this section.");
        return result;
    }

    bool pasteMacroStarted = false;
    for (const auto& copy : m_clipboardCopies) {
        const ProjectItem* src = copy.item.get();
        auto identityGuard = guardFieldGroupIdentities(m_currentProject, src->type);
        auto clone = cloneSubtreeDetached(src, nullptr, {}, m_clipboardSerializer.get());
        if (!clone) {
            emit errorOccurred(QString("Failed to paste '%1'; the batch was stopped.").arg(src->name));
            break;
        }
        if (copy.rootGroups) clone->fieldGroups = *copy.rootGroups;

        if (isLayersBucketRootType(clone->type)) {
            clone->parentRef = ParentRef{};
            clone->parentRef.kind = ParentRef::Kind::Root;
        }
        ProjectFileBinding binding;
        binding.parentFolder = bucketFolder;
        binding.rootItem     = clone;
        bucket->push_back(std::move(binding));
        if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(clone->type)
            && (!validateFieldGroupProject(*m_currentProject).ok()
                || !refreshFieldGroupDerivedViews(*m_currentProject))) {
            bucket->pop_back();
            emit errorOccurred(QString("Failed to paste '%1'; the batch was stopped.").arg(src->name));
            break;
        }

        identityGuard.dismiss();
        clone->markDirty();
        recomputeObjectPathRecursive(clone.get());

        if (!pasteMacroStarted) {
            UndoRedoManager::instance().beginMacro(tr("Paste"));
            pasteMacroStarted = true;
        }
        emit itemCreated(clone.get());
        UndoRedoManager::instance().push(new CreateProjectItemCommand(nullptr, clone));
        requestResync(clone.get());
        result.push_back(clone.get());
    }
    if (pasteMacroStarted) UndoRedoManager::instance().endMacro();
    return result;
}

std::optional<DefinitionFieldGroups> ProjectManager::captureRootFieldGroups(
    const ProjectItem* item, QString* error,
    std::span<const FieldGroupDefinitionSnapshot> replacements) const
{
    const auto fail = [error](const QString& message) -> std::optional<DefinitionFieldGroups> {
        if (error) *error = message;
        return {};
    };
    if (!m_currentProject || !item
        || !PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item->type))
        return fail(QStringLiteral("A field group definition is required"));
    if (findItemByGuid(item->guid) != item)
        return fail(QStringLiteral("Field group definition is not in the project"));
    const auto validation = validateFieldGroupProject(*m_currentProject, replacements);
    if (!validation.ok()) return fail(validation.diagnostics.front().message);
    const auto groupsFor = [&](const ProjectItem& definition) {
        const auto replacement = std::find_if(replacements.begin(), replacements.end(),
            [&](const auto& snapshot) { return snapshot.definition == definition.guid; });
        return replacement == replacements.end() ? &definition.fieldGroups : &replacement->groups;
    };
    const auto view = buildFieldGroupProjectView(*m_currentProject, groupsFor);
    const auto identity = toFieldGroupGuid(item->guid);
    const auto found = std::find_if(view.definitions.begin(), view.definitions.end(),
        [&](const auto& definition) {
            return definition.guid == identity;
        });
    if (found == view.definitions.end())
        return fail(QStringLiteral("Field group definition is not in the project"));
    const FieldGroupResolver resolver(view.definitions,
        toFieldGroupGuid(m_currentProject->busses.front().rootItem->guid));
    std::vector<FieldGroupScope> scopes{std::nullopt};
    for (const auto& platform : m_currentProject->platforms)
        scopes.push_back(toFieldGroupGuid(platform.id));
    const auto& authored = *groupsFor(*item);
    auto result = authored;
    const auto capture = [&]<typename Payload>(const AuthoredGroup<Payload>& before,
                                               AuthoredGroup<Payload>& after,
                                               FieldGroupId group) {
        std::vector<Payload> values(scopes.size());
        for (std::size_t i = 0; i < scopes.size(); ++i) {
            const auto resolved = resolver.Resolve(identity, scopes[i], group);
            if (!resolved.ok()) return false;
            values[i] = std::get<Payload>(resolved.value->payload);
        }
        after.shared.localHistory = values[0];
        after.shared.localFields = FieldGroupPropertyMask(Payload::groupId);
        after.platforms.clear();
        for (std::size_t i = 1; i < scopes.size(); ++i) {
            const auto platform = *scopes[i];
            const auto original = before.platforms.find(platform);
            auto mode = original == before.platforms.end()
                ? FieldGroupMode::UseShared : original->second.mode;
            if (mode == after.shared.mode) mode = FieldGroupMode::UseShared;
            const auto fields = FieldGroupDifferenceMask(values[i], values[0]);
            if (fields != 0) {
                Payload payload;
                CopyFieldGroupFields(payload, values[i], fields);
                after.platforms.emplace(platform, GroupState<Payload>{mode, std::move(payload), fields});
            } else if (mode != FieldGroupMode::UseShared) {
                after.platforms.emplace(platform, GroupState<Payload>{mode, {}});
            }
        }
        return true;
    };
    if (!capture(authored.mainOutput, result.mainOutput, FieldGroupId::MainOutput)
        || !capture(authored.send, result.send, FieldGroupId::Send)
        || !capture(authored.spatialRating, result.spatialRating, FieldGroupId::SpatialRating))
        return fail(QStringLiteral("Cannot preserve the current field group values"));
    return result;
}

bool ProjectManager::isDuplicable(const ProjectItem* item)
{
    return isClipboardEligible(item);
}

bool ProjectManager::canDuplicateItems(const QList<ProjectItem*>& items) const
{
    if (!m_currentProject)
        return false;
    const QList<ProjectItem*> roots = normalizeSubtreeItems(items);
    if (roots.isEmpty())
        return false;
    for (ProjectItem* src : roots) {
        if (!isDuplicable(src) || src->guid.isNull())
            return false;
        if (!validateNewAuthoring(*this, *src, src->parent))
            return false;
        if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(src->type)
            && !validateFieldGroupProject(*m_currentProject).ok()) return false;
        if (!validatePlaybackReferences(*src, nullptr)) return false;
        if (src->parent && !validatePlacement(src->type, src->parent, src, nullptr))
            return false;
        if (!src->parent) {
            const ProjectFileBinding* owner = findFileBindingForItem(*m_currentProject, src);
            if (!owner || !m_currentProject->getBucketForFolder(owner->parentFolder))
                return false;
        } else if (src->type == ProjectItem::Type::Bus) {
            QString reason;
            if (!validateStructuralPlacement(src->type, src->parent, &reason, nullptr))
                return false;
        }
    }
    return true;
}

std::vector<ProjectItem*> ProjectManager::duplicateItems(const QList<ProjectItem*>& items)
{
    std::vector<ProjectItem*> result;
    if (!m_currentProject) {
        emit errorOccurred("No project loaded.");
        return result;
    }
    const QList<ProjectItem*> roots = normalizeSubtreeItems(items);
    if (!canDuplicateItems(roots))
        return result;

    const bool macro = roots.size() > 1;
    if (macro)
        UndoRedoManager::instance().beginMacro(tr("Duplicate %1 items").arg(roots.size()));
    for (ProjectItem* src : roots) {
        ProjectItem* clone = duplicateItem(src);
        if (!clone) {
            if (macro)
                UndoRedoManager::instance().endMacro();
            return result;
        }
        result.push_back(clone);
    }
    if (macro)
        UndoRedoManager::instance().endMacro();
    return result;
}

ProjectItem* ProjectManager::duplicateItem(ProjectItem* src)
{
    if (!m_currentProject) { emit errorOccurred("No project loaded."); return nullptr; }
    if (!src) return nullptr;
    const auto authoring = validateNewAuthoring(*this, *src, src->parent);
    if (!authoring) {
        emit errorOccurred(authoring.message);
        return nullptr;
    }

    auto identityGuard = guardFieldGroupIdentities(m_currentProject, src->type);
    auto clone = cloneSubtreeDetached(src, src->parent,
                                     m_currentProject->itemFolderPath(src));
    if (!clone) return nullptr;

    if (ProjectItem* parent = src->parent) {
        if (src->type == ProjectItem::Type::Bus) {
            QString reason;
            if (!validateStructuralPlacement(src->type, parent, &reason, nullptr)) {
                emit errorOccurred(reason);
                return nullptr;
            }
        }
        if (!restoreItemInternal(parent, clone))
            return nullptr;
        identityGuard.dismiss();

        recomputeObjectPathRecursive(clone.get());

        emit itemCreated(clone.get());
        UndoRedoManager::instance().push(new CreateProjectItemCommand(parent, clone));
        requestResync(clone.get());
        return clone.get();
    }

    ProjectFileBinding* owner = findFileBindingForItem(*m_currentProject, src);
    auto* bucket = owner ? m_currentProject->getBucketForFolder(owner->parentFolder)
                         : nullptr;
    if (!bucket) {
        emit errorOccurred(QString("Cannot duplicate '%1': it has no owning section.").arg(src->name));
        return nullptr;
    }

    if (isLayersBucketRootType(clone->type)) {
        clone->parentRef = ParentRef{};
        clone->parentRef.kind = ParentRef::Kind::Root;
    }
    ProjectFileBinding binding;
    binding.parentFolder = owner->parentFolder;
    binding.rootItem     = clone;
    bucket->push_back(std::move(binding));
    if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(clone->type)
        && (!validateFieldGroupProject(*m_currentProject).ok()
            || !refreshFieldGroupDerivedViews(*m_currentProject))) {
        bucket->pop_back();
        return nullptr;
    }

    identityGuard.dismiss();
    clone->markDirty();
    recomputeObjectPathRecursive(clone.get());

    emit itemCreated(clone.get());
    UndoRedoManager::instance().push(new CreateProjectItemCommand(nullptr, clone));
    requestResync(clone.get());
    return clone.get();
}

bool ProjectManager::pasteProperties(const ProjectItem* source, const QList<ProjectItem*>& targets)
{
    if (!canPasteProperties(source, targets)) return false;
    const auto unique = uniqueItemsByGuid(targets);
    const std::vector<ProjectItem*> applicable(unique.begin(), unique.end());

    auto* cmd = new PastePropertiesCommand(applicable, source);
    if (cmd->appliedCount() == 0) {
        delete cmd;
        return true;
    }
    UndoRedoManager::instance().push(cmd);
    return true;
}

bool ProjectManager::canPasteProperties(const ProjectItem* source, const QList<ProjectItem*>& targets) const
{
    if (!m_currentProject || !source || !source->comp<VMixNode>() || targets.isEmpty())
        return false;
    if (findItemByGuid(source->guid) != source
        && std::none_of(m_clipboardCopies.begin(), m_clipboardCopies.end(),
            [source](const auto& copy) { return copy.item.get() == source; }))
        return false;
    for (ProjectItem* target : targets) {
        if (!target || target->type != source->type || !target->comp<VMixNode>()
            || findItemByGuid(target->guid) != target)
            return false;
    }
    return true;
}

bool ProjectManager::canPastePropertiesFromClipboard(const QList<ProjectItem*>& targets) const
{
    if (clipboardCount() != 1) return false;
    const auto sources = resolveClipboardSources();
    return sources.size() == 1 && canPasteProperties(sources.front(), targets);
}

bool ProjectManager::pastePropertiesFromClipboard(const QList<ProjectItem*>& targets)
{
    if (!m_currentProject) { emit errorOccurred("No project loaded."); return false; }

    const auto sources = resolveClipboardSources();
    if (sources.size() != 1) {
        emit errorOccurred("Paste Properties needs exactly one copied source.");
        return false;
    }
    return pasteProperties(sources.front(), targets);
}

namespace {

std::vector<DeletedSubtreeRefLog::FieldGroupRef> fieldGroupReferenceRemovals(
    ProjectData& project, ProjectItem* deletedRoot)
{
    std::set<harkenprojectschema::AuthoringGuid> deleted;
    std::function<void(ProjectItem*)> collect = [&](ProjectItem* item) {
        if (!item) return;
        deleted.insert(toFieldGroupGuid(item->guid));
        for (const auto& child : item->children) collect(child.get());
    };
    collect(deletedRoot);
    const auto source = HarkenEditor::Bridge::canonicalRuntimeProjectSourceIndex(project);
    const auto* fallback = deletedRoot->parent && deletedRoot->parent->type == ProjectItem::Type::Bus
        ? deletedRoot->parent : (project.busses.empty() ? nullptr : project.busses.front().rootItem.get());
    const auto fallbackGuid = fallback ? toFieldGroupGuid(fallback->guid) : harkenprojectschema::AuthoringGuid{};
    std::vector<DeletedSubtreeRefLog::FieldGroupRef> result;
    const auto editActive = [](auto& group, bool root, const auto& edit) {
        bool sharedActive = root || group.shared.mode == FieldGroupMode::Local;
        for (auto& [platform, state] : group.platforms) {
            const auto local = ResolveFieldGroupLocal(group, platform);
            if (!root && local.mode != FieldGroupMode::Local) continue;
            sharedActive = true;
            if (state.localHistory) {
                auto value = *state.localHistory;
                edit(value);
                CopyFieldGroupFields(*state.localHistory, value, state.localFields);
            }
        }
        if (sharedActive && group.shared.localHistory) edit(*group.shared.localHistory);
    };
    for (auto* item : source->definitions) {
        if (deleted.contains(toFieldGroupGuid(item->guid))) continue;
        const bool root = source->parentDefinition.at(item) == nullptr;
        auto after = item->fieldGroups;
        editActive(after.mainOutput, root, [&](MainOutputFieldGroup& output) {
            if (deleted.contains(output.mainOutputBusRef)) output.mainOutputBusRef = fallbackGuid;
        });
        editActive(after.send, root, [&](SendFieldGroup& send) {
            std::erase_if(send.sends, [&](const auto& row) { return deleted.contains(row.targetBusID); });
        });
        const auto clearSpatialReference = [&](auto& state) {
            if (state.localHistory && deleted.contains(state.localHistory->spatialAttenuationRef))
                state.localHistory->spatialAttenuationRef = {};
        };
        clearSpatialReference(after.spatialRating.shared);
        for (auto& [platform, state] : after.spatialRating.platforms)
            clearSpatialReference(state);
        if (after != item->fieldGroups)
            result.push_back({item->guid, item->fieldGroups, std::move(after)});
    }
    return result;
}

}

bool ProjectManager::deleteItemInternal(ProjectItem* parent, const std::shared_ptr<ProjectItem>& item, int* removedIndex,
                                        DeletedSubtreeRefLog* outLog)
{
    if (!parent || !item) return false;
    if (!PlayablePropertyAuthoringV1::validateContainerOccurrences(*parent))
        return false;

    auto it = std::find(parent->children.begin(), parent->children.end(), item);
    if (it == parent->children.end()) return false;

    if (auto* project = currentProject()) {
        std::vector<FieldGroupDefinitionSnapshot> candidates;
        for (const auto& change : fieldGroupReferenceRemovals(*project, item.get()))
            candidates.push_back({change.definition, change.after});
        const auto validation = validateFieldGroupProject(*project, candidates);
        if (!validation.ok()) {
            emit errorOccurred(validation.diagnostics.front().message);
            return false;
        }
    }

    const auto* parentSelection = parent->comp<VSelection>();
    const auto previousSelections = parentSelection
        ? std::optional(parentSelection->selections) : std::nullopt;
    std::vector<SelectionEntry> removedOccurrences;
    if (parent->type == ProjectItem::Type::Container) {
        removedOccurrences =
            PlayablePropertyAuthoringV1::removeContainerChildOccurrences(
                *parent, item->guid);
    }

    if (removedIndex) {
        *removedIndex = static_cast<int>(std::distance(parent->children.begin(), it));
    }

    // Extension point ④: detach the bus subtree from the engine BusTree while
    // the items are still valid and parented. removeBus does not cascade to
    // child buses, so tear the whole subtree down post-order. Non-bus types are
    // removed from the runtime via the itemDeleted signal path instead.
    if (item->type == ProjectItem::Type::Bus) {
        std::function<void(ProjectItem*)> demat = [&](ProjectItem* node) {
            if (!node) return;
            for (auto& child : node->children) demat(child.get());
            if (node->type == ProjectItem::Type::Bus)
                dematerializeRuntime(node);
        };
        demat(item.get());
    }

    parent->children.erase(it);
    parent->markDirty();

    ProjectData* project = currentProject();
    if (project)
    {
        DeletedSubtreeRefLog log = cleanupReferencesToDeletedSubtree(item.get(), project);
        if (previousSelections)
            log.selections.push_back({parent->objectID, *previousSelections});
        log.containerChildOccurrences = std::move(removedOccurrences);
        if (outLog) *outLog = std::move(log);
    } else if (outLog) {
        if (previousSelections)
            outLog->selections.push_back({parent->objectID, *previousSelections});
        outLog->containerChildOccurrences = std::move(removedOccurrences);
    }

    return true;
}

namespace {
bool replaceSends(ProjectItem& owner, VMixNode& mix, std::vector<Send> next);
}

DeletedSubtreeRefLog ProjectManager::cleanupIllegalPlaybackReferences(ProjectData* project)
{
    DeletedSubtreeRefLog log;
    if (!project) return log;
    auto& self = ProjectManager::instance();
    auto resolve = [project](const QUuid& guid) -> const ProjectItem* {
        return findItemByGuidInProject(project, guid);
    };
    auto clipTargetIllegal = [&](const ProjectItem* owner, const Clip& clip) {
        const ProjectItem* target = resolve(clip.targetGuid);
        return !target || target->parent != owner
            || !ItemTypeCatalog::isTimelineClipTarget(target->type);
    };
    auto gridTargetIllegal = [](const ProjectItem& owner, const QUuid& target) {
        return !GridSemantics::isGridTargetCandidate(owner, target);
    };
    auto transitionEndpointIllegal = [&](const ProjectItem& owner,
                                         const std::optional<QUuid>& endpoint) {
        if (!endpoint) return false;
        const ProjectItem* candidate = resolve(*endpoint);
        return !candidate
            || !TransitionSemantics::isEndpointCandidate(owner, *candidate);
    };

    std::function<void(ProjectItem*)> scrubPlaybackRefs = [&](ProjectItem* owner) {
        if (!owner) return;
        if (auto* timeline = owner->comp<VTrack>()) {
            std::vector<Clip> kept;
            kept.reserve(timeline->clips.items.size());
            for (size_t i = 0; i < timeline->clips.items.size(); ++i) {
                const Clip& clip = timeline->clips.items[i];
                if (clipTargetIllegal(owner, clip))
                    log.clips.push_back(
                        {owner->objectID, static_cast<int>(i), clip});
                else
                    kept.push_back(clip);
            }
            if (kept.size() != timeline->clips.items.size()) {
                timeline->clips.items = std::move(kept);
                owner->markDirty();
                emit self.itemPropertyChanged(owner, QStringLiteral("clips"));
            }
        }
        if (auto* selection = owner->comp<VSelection>()) {
            SelectionData next = selection->selections;
            for (auto* table : next.tables())
                std::erase_if(table->entries, [&](const auto& entry) {
                    return gridTargetIllegal(*owner, entry.childGuid);
                });
            if (next != selection->selections) {
                log.selections.push_back({owner->objectID, selection->selections});
                selection->selections = std::move(next);
                owner->markDirty();
                emit self.itemPropertyChanged(owner, QStringLiteral("selections"));
            }

            TransitionTableDef table = selection->transitions;
            std::erase_if(table.rules, [&](const TransitionRuleDef& rule) {
                return transitionEndpointIllegal(*owner, rule.source)
                    || transitionEndpointIllegal(*owner, rule.target);
            });
            if (table.rules.size() != selection->transitions.rules.size()) {
                log.transitions.push_back({owner->objectID, selection->transitions});
                selection->transitions = std::move(table);
                owner->markDirty();
                emit self.itemPropertyChanged(owner, QStringLiteral("transitions"));
            }
        }
        for (auto& child : owner->children) scrubPlaybackRefs(child.get());
    };
    for (auto* binding : project->allFileBindings())
        if (binding && binding->rootItem)
            scrubPlaybackRefs(binding->rootItem.get());
    return log;
}

namespace {

QJsonObject folderComponentReferences(const ProjectItem& item)
{
    QJsonObject components;
    if (const auto* mix = item.comp<VMixNode>())
        components.insert(QStringLiteral("mixnode"), QJsonObject{{QStringLiteral("bindings"), mix->bindings.toJson()}});
    if (const auto* selection = item.comp<VSelection>())
        components.insert(QStringLiteral("selection"), QJsonObject{
            {QStringLiteral("selections"), selection->selections.toJson()},
            {QStringLiteral("transitions"), selection->transitions.toJson()}});
    return {{QStringLiteral("components"), components}};
}

void applyFolderComponentReferences(ProjectItem& item, const QJsonObject& state)
{
    const auto components = state.value(QStringLiteral("components")).toObject();
    QStringList changedProperties;
    if (auto* mix = item.comp<VMixNode>()) {
        const auto bindings = components.value(QStringLiteral("mixnode"))
            .toObject().value(QStringLiteral("bindings")).toArray();
        if (mix->bindings.toJson() != bindings) {
            mix->bindings = BindingLedger::fromJson(bindings);
            changedProperties.append(QStringLiteral("bindings"));
        }
    }
    if (auto* selection = item.comp<VSelection>()) {
        const auto value = components.value(QStringLiteral("selection")).toObject();
        if (const auto selections = value.value(QStringLiteral("selections")).toObject();
            selection->selections.toJson() != selections) {
            if (const auto parsed = SelectionData::fromJson(selections)) {
                selection->selections = *parsed;
                changedProperties.append(QStringLiteral("selections"));
            }
        }
        if (const auto transitions = value.value(QStringLiteral("transitions")).toObject();
            selection->transitions.toJson() != transitions) {
            if (const auto parsed = TransitionTableDef::fromJson(transitions)) {
                selection->transitions = *parsed;
                changedProperties.append(QStringLiteral("transitions"));
            }
        }
    }
    if (changedProperties.isEmpty()) return;
    item.markDirty();
    for (const auto& property : changedProperties)
        emit ProjectManager::instance().itemPropertyChanged(&item, property);
}

QJsonObject folderProjectReferences(const ProjectData& project)
{
    QJsonArray groups;
    for (const auto& group : project.packGroups) groups.append(group.toJson());
    QJsonArray targeting;
    for (const auto& target : ProjectManager::instance().getActionPackTargeting()) targeting.append(target.toJson());
    return {{QStringLiteral("packGroups"), groups}, {QStringLiteral("actionPackTargeting"), targeting}};
}

void applyFolderProjectReferences(ProjectData& project, const QJsonObject& state)
{
    project.packGroups.clear();
    for (const auto& group : state.value(QStringLiteral("packGroups")).toArray())
        project.packGroups.push_back(PackGroup::fromJson(group.toObject()));
    std::vector<ActionPackTargeting> targeting;
    for (const auto& target : state.value(QStringLiteral("actionPackTargeting")).toArray())
        targeting.push_back(ActionPackTargeting::fromJson(target.toObject()));
    ProjectManager::instance().setActionPackTargetingInternal(std::move(targeting));
}

}

DeletedSubtreeRefLog ProjectManager::cleanupReferencesToDeletedSubtree(ProjectItem* deletedRoot,
                                                                       ProjectData* project)
{
    DeletedSubtreeRefLog log;
    if (!deletedRoot || !project) return log;

    // Static entry point: reach the singleton for member operations and signals.
    auto& self = ProjectManager::instance();

    QHash<uint32_t, const ProjectItem*> deletedObjects;
    QSet<QUuid> deletedGuids;
    {
        std::function<void(ProjectItem*)> gather = [&](ProjectItem* pi) {
            if (!pi) return;
            deletedGuids.insert(pi->guid);
            if (pi->objectID != 0) deletedObjects.insert(pi->objectID, pi);
            for (auto& child : pi->children) gather(child.get());
        };
        gather(deletedRoot);
    }

    log.fieldGroups = fieldGroupReferenceRemovals(*project, deletedRoot);
    const auto groupSource = HarkenEditor::Bridge::canonicalRuntimeProjectSourceIndex(*project);
    for (const auto& change : log.fieldGroups) {
        auto* item = groupSource->byQtGuid.at(change.definition);
        item->fieldGroups = change.after;
        item->markDirty();
    }
    refreshFieldGroupDerivedViews(*project);
    for (const auto& change : log.fieldGroups)
        emit self.itemPropertyChanged(groupSource->byQtGuid.at(change.definition), QStringLiteral("fieldGroups"));

    if (deletedObjects.isEmpty())
        return log;

    // Where items whose output routing pointed into the deleted subtree get
    // re-indexed: the deleted root's parent bus (guaranteed to survive), or the
    // master bus when the root has no parent. Only meaningful for bus deletes,
    // but harmless otherwise since no output ref will match a non-bus id set.
    const uint32_t fallbackBusID =
        deletedRoot->parent ? deletedRoot->parent->objectID : kMasterBusID;

    std::function<void(ProjectItem*)> scrub = [&](ProjectItem* pi) {
        if (!pi) return;

        // A surviving item whose output routing pointed at a now-deleted bus is
        // re-indexed to the deleted root's parent bus.
        if (auto* mix = pi->comp<VMixNode>();
            mix && !PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(pi->type)
            && deletedObjects.contains(mix->outputBusId))
        {
            log.outputBusRefs.push_back({pi->objectID, mix->outputBusId});
            self.setItemOutputBusRefInternal(pi, fallbackBusID);
        }

        if (auto* action = pi->comp<VAction>();
            action && !deletedObjects.contains(pi->objectID)) {
            bool changed = false;
            for (size_t index = 0; index < action->steps.size(); ++index) {
                auto& step = action->steps[index];
                if (deletedObjects.contains(step.quantize.referenceObjectID)) {
                    log.actionTimingRefs.push_back(
                        {pi->objectID, index, step.quantize.referenceObjectID, step.quantize.referenceName});
                    step.quantize.referenceObjectID = 0;
                    step.quantize.referenceName.clear();
                    changed = true;
                }
                const ProjectItem* target = deletedObjects.value(step.targetObjectID, nullptr);
                if (!target || !EditorUtils::canActionStepReferenceObject(
                        step.type, target->type, target->objectID, pi->objectID))
                    continue;
                log.actionTargetRefs.push_back(
                    {pi->objectID, index, step.targetObjectID, step.targetName});
                step.targetObjectID = 0;
                step.targetName.clear();
                changed = true;
            }
            if (changed) {
                pi->markDirty();
                emit self.itemPropertyChanged(pi, QStringLiteral("steps"));
            }
        }

        {
            const auto before = folderComponentReferences(*pi);
            const auto after = PhysicalFolderReferenceEdits::removeReferences(before, deletedGuids);
            if (before != after) {
                log.folderComponentRefs.push_back({pi->objectID, before, after});
                applyFolderComponentReferences(*pi, after);
            }
        }

        for (auto& child : pi->children) scrub(child.get());
    };

    for (auto* binding : project->allFileBindings())
        if (binding && binding->rootItem)
            scrub(binding->rootItem.get());

    {
        const auto before = folderProjectReferences(*project);
        auto after = PhysicalFolderReferenceEdits::removeReferences(before, deletedGuids);
        QJsonArray targeting;
        for (const auto& value : after.value(QStringLiteral("actionPackTargeting")).toArray()) {
            const auto entry = ActionPackTargeting::fromJson(value.toObject());
            const bool removed = std::any_of(deletedObjects.cbegin(), deletedObjects.cend(),
                [&](const auto* item) { return item->runtimeId() == entry.stableId; });
            if (!removed) targeting.append(value);
        }
        after.insert(QStringLiteral("actionPackTargeting"), targeting);
        if (before != after) {
            log.folderProjectReferencesBefore = before;
            log.folderProjectReferencesAfter = after;
            applyFolderProjectReferences(*project, after);
        }
    }

    // Surviving mix nodes' sends targeting any deleted bus become dangling;
    // strip them and record for undo.
    self.forEachMixNodeItem([&](ProjectItem* owner, VMixNode* mix) {
        if (deletedObjects.contains(owner->objectID)
            || PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(owner->type)) return;
        const bool ownerIsBus = owner->type == ProjectItem::Type::Bus;
        std::vector<Send> kept;
        std::vector<Send> stripped;
        kept.reserve(mix->sends.size());
        for (const auto& s : mix->sends) {
            if (deletedObjects.contains(s.targetBusID))
                stripped.push_back(s);
            else
                kept.push_back(s);
        }
        if (stripped.empty()) return;
        if (!replaceSends(*owner, *mix, std::move(kept)))
            return;
        for (const auto& s : stripped) {
            log.sends.push_back({owner->objectID, s});
        }
        owner->markDirty();
        if (ownerIsBus) {
            AudioEngine::instance().runtimeSyncCoordinator().markCurrentWorldMutation();
            emit self.busChanged(owner->objectID, QStringLiteral("send"));
        } else {
            self.propagateItemSendsLive(owner);
            emit self.itemPropertyChanged(owner, QStringLiteral("send"));
        }
    });

    DeletedSubtreeRefLog playback = cleanupIllegalPlaybackReferences(project);
    log.clips = std::move(playback.clips);
    log.selections.insert(log.selections.end(), playback.selections.begin(), playback.selections.end());
    log.transitions = std::move(playback.transitions);

    return log;
}

void ProjectManager::restoreReferencesFromLog(const DeletedSubtreeRefLog& log,
                                              ProjectData* project)
{
    if (!project) return;
    auto& self = ProjectManager::instance();

    auto findById = [project](uint32_t objectID) -> ProjectItem* {
        if (objectID == 0) return nullptr;
        for (auto* binding : project->allFileBindings()) {
            if (!binding || !binding->rootItem) continue;
            std::vector<ProjectItem*> stack{binding->rootItem.get()};
            while (!stack.empty()) {
                ProjectItem* item = stack.back();
                stack.pop_back();
                if (!item) continue;
                if (item->objectID == objectID) return item;
                for (auto& child : item->children)
                    stack.push_back(child.get());
            }
        }
        return nullptr;
    };

    if (!log.folderProjectReferencesBefore.isEmpty()) {
        auto current = folderProjectReferences(*project);
        if (PhysicalFolderReferenceEdits::mergeReferences(current,
                log.folderProjectReferencesAfter, log.folderProjectReferencesBefore))
            applyFolderProjectReferences(*project, current);
    }
    for (const auto& ref : log.folderComponentRefs) {
        auto* item = findById(ref.ownerObjectID);
        if (!item) continue;
        auto current = folderComponentReferences(*item);
        if (PhysicalFolderReferenceEdits::mergeReferences(current, ref.after, ref.before))
            applyFolderComponentReferences(*item, current);
    }

    std::vector<FieldGroupDefinitionSnapshot> groups;
    const auto groupSource = HarkenEditor::Bridge::canonicalRuntimeProjectSourceIndex(*project);
    bool matching = true;
    for (const auto& ref : log.fieldGroups) {
        const auto item = groupSource->byQtGuid.find(ref.definition);
        matching = matching && item != groupSource->byQtGuid.end()
            && item->second->fieldGroups == ref.after;
        groups.push_back({ref.definition, ref.before});
    }
    if (matching && validateFieldGroupProject(*project, groups).ok()) {
        for (const auto& ref : log.fieldGroups) {
            auto* item = groupSource->byQtGuid.at(ref.definition);
            item->fieldGroups = ref.before;
            item->markDirty();
        }
        refreshFieldGroupDerivedViews(*project);
        for (const auto& ref : log.fieldGroups)
            emit self.itemPropertyChanged(groupSource->byQtGuid.at(ref.definition), QStringLiteral("fieldGroups"));
    }

    for (const auto& ref : log.outputBusRefs) {
        if (ProjectItem* item = findById(ref.itemObjectID))
            self.setItemOutputBusRefInternal(item, ref.oldBusID);
    }

    for (const auto& ref : log.sends) {
        self.upsertSendInternal(ref.ownerObjectID, ref.send.targetBusID,
                                   ref.send.sendLevel);
    }

    for (const auto& ref : log.selections) {
        auto* owner = findById(ref.ownerObjectID);
        auto* selection = owner ? owner->comp<VSelection>() : nullptr;
        if (!selection) continue;
        selection->selections = ref.selections;
        owner->markDirty();
        emit self.itemPropertyChanged(owner, QStringLiteral("selections"));
    }

    QSet<uint32_t> changedActionIDs;
    for (const auto& ref : log.actionTimingRefs) {
        ProjectItem* owner = findById(ref.ownerObjectID);
        auto* action = owner ? owner->comp<VAction>() : nullptr;
        if (!action || ref.stepIndex >= action->steps.size()) continue;
        auto& quantize = action->steps[ref.stepIndex].quantize;
        if (quantize.referenceObjectID != 0 || !quantize.referenceName.isEmpty()) continue;
        quantize.referenceObjectID = ref.targetObjectID;
        quantize.referenceName = ref.targetName;
        changedActionIDs.insert(ref.ownerObjectID);
    }
    for (const auto& ref : log.actionTargetRefs) {
        ProjectItem* owner = findById(ref.ownerObjectID);
        auto* action = owner ? owner->comp<VAction>() : nullptr;
        if (!action || ref.stepIndex >= action->steps.size()) continue;
        auto& step = action->steps[ref.stepIndex];
        if (step.targetObjectID != 0 || !step.targetName.isEmpty()) continue;
        step.targetObjectID = ref.targetObjectID;
        step.targetName = ref.targetName;
        changedActionIDs.insert(ref.ownerObjectID);
    }
    for (uint32_t objectID : changedActionIDs) {
        if (ProjectItem* owner = findById(objectID)) {
            owner->markDirty();
            emit self.itemPropertyChanged(owner, QStringLiteral("steps"));
        }
    }

    QSet<uint32_t> changedTrackIDs;
    for (const auto& ref : log.clips) {
        ProjectItem* owner = findById(ref.ownerObjectID);
        auto* timeline = owner ? owner->comp<VTrack>() : nullptr;
        if (!timeline) continue;
        const int index = std::clamp(
            ref.originalIndex, 0, static_cast<int>(timeline->clips.items.size()));
        timeline->clips.items.insert(timeline->clips.items.begin() + index, ref.clip);
        changedTrackIDs.insert(ref.ownerObjectID);
    }
    for (uint32_t objectID : changedTrackIDs) {
        if (ProjectItem* owner = findById(objectID)) {
            owner->markDirty();
            emit self.itemPropertyChanged(owner, QStringLiteral("clips"));
        }
    }


    for (const auto& ref : log.transitions) {
        ProjectItem* owner = findById(ref.ownerObjectID);
        auto* selection = owner ? owner->comp<VSelection>() : nullptr;
        if (!selection) continue;
        selection->transitions = ref.table;
        owner->markDirty();
        emit self.itemPropertyChanged(owner, QStringLiteral("transitions"));
    }
}

ProjectFileBinding* ProjectManager::findRootBinding(ProjectData* project, const ProjectItem* item)
{
    if (!project || !item) return nullptr;
    // Bindings register by bucket folder, so scan the file buckets instead of
    // inferring ownership from the root type.
    std::vector<ProjectFileBinding>* buckets[] = {
        &project->layers, &project->actions, &project->attenuationItems,
        &project->busses, &project->variables };
    for (auto* bucket : buckets) {
        for (auto& binding : *bucket) {
            if (binding.rootItem.get() == item) return &binding;
        }
    }
    return nullptr;
}

bool ProjectManager::detachRootBinding(ProjectData* project, const ProjectItem* item, RootBindingSlot* out)
{
    if (!project || !item || !out) return false;
    std::vector<ProjectFileBinding>* buckets[] = {
        &project->layers, &project->actions, &project->attenuationItems,
        &project->busses, &project->variables };
    for (auto* bucket : buckets) {
        for (size_t i = 0; i < bucket->size(); ++i) {
            if ((*bucket)[i].rootItem.get() == item) {
                out->binding = (*bucket)[i];
                out->index = static_cast<int>(i);
                bucket->erase(bucket->begin() + i);
                return true;
            }
        }
    }
    return false;
}

bool ProjectManager::attachRootBinding(ProjectData* project, const RootBindingSlot& slot)
{
    if (!project || !slot.valid()) return false;
    auto* bucket = project->getBucketForFolder(slot.binding.parentFolder);
    if (!bucket) return false;
    const auto& root = *slot.binding.rootItem;
    if (root.type == ProjectItem::Type::Action || root.type == ProjectItem::Type::Variable) {
        if (!NameValidator::validateItemName(root.name, root.type)
            || !project->findItemsBySelector(root.name, root.type).empty())
            return false;
    }
    const int insertAt = slot.index < 0 ? static_cast<int>(bucket->size())
        : std::min(slot.index, static_cast<int>(bucket->size()));
    ProjectItem* item = slot.binding.rootItem.get();
    ProjectItem* oldParent = item->parent;
    const ParentRef oldParentRef = item->parentRef;
    bucket->insert(bucket->begin() + insertAt, slot.binding);
    item->parent = nullptr;
    if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item->type)) {
        item->parentRef = ParentRef{};
        if (!validateFieldGroupProject(*project).ok()
            || !refreshFieldGroupDerivedViews(*project)) {
            bucket->erase(bucket->begin() + insertAt);
            item->parent = oldParent;
            item->parentRef = oldParentRef;
            return false;
        }
    }
    item->markDirty();
    return true;
}

ProjectFolderBinding* ProjectManager::findRootFolderBinding(ProjectData* project, const PhysicalFolder* folder)
{
    if (!project || !folder) return nullptr;
    for (auto& binding : project->folders) {
        if (binding.rootFolder.get() == folder) return &binding;
    }
    return nullptr;
}

bool ProjectManager::detachRootFolderBinding(ProjectData* project, const PhysicalFolder* folder, RootFolderSlot* out)
{
    if (!project || !folder || !out) return false;
    for (size_t i = 0; i < project->folders.size(); ++i) {
        if (project->folders[i].rootFolder.get() == folder) {
            out->binding = project->folders[i];
            out->index = static_cast<int>(i);
            project->folders.erase(project->folders.begin() + i);
            return true;
        }
    }
    return false;
}

bool ProjectManager::attachRootFolderBinding(ProjectData* project, const RootFolderSlot& slot)
{
    if (!project || !slot.valid()) return false;
    const int insertAt = std::min(slot.index, static_cast<int>(project->folders.size()));
    if (insertAt < 0) {
        project->folders.push_back(slot.binding);
    } else {
        project->folders.insert(project->folders.begin() + insertAt, slot.binding);
    }
    slot.binding.rootFolder->parent = nullptr;
    return true;
}

bool ProjectManager::moveItemInternal(
    ProjectItem* item,
    ProjectItem* newParent,
    int insertIndex,
    const std::optional<SelectionEntry>& restoredDestination,
    std::optional<SelectionEntry>* removedSource,
    std::optional<SelectionEntry>* installedDestination,
    std::vector<SelectionEntry>* removedSources,
    bool restoreEmptySourceOccurrences,
    DeletedSubtreeRefLog* outLog,
    bool createDefaultClip)
{
    if (!item || !newParent) return false;
    if (item->parent == newParent) return true;

    QString reason;
    if (!validateMoveConstraints(item, newParent, &reason)) {
        emit errorOccurred(reason);
        return false;
    }

    ProjectItem* oldParent = item->parent;
    if (!oldParent) return false;
    if (newParent->type != ProjectItem::Type::Container
        && restoredDestination)
        return false;
    if (!PlayablePropertyAuthoringV1::validateContainerOccurrences(*oldParent)
        || !PlayablePropertyAuthoringV1::validateContainerOccurrences(*newParent))
        return false;

    auto it = std::find_if(oldParent->children.begin(), oldParent->children.end(),
                           [item](const std::shared_ptr<ProjectItem>& ptr) { return ptr.get() == item; });
    if (it == oldParent->children.end()) return false;

    std::shared_ptr<ProjectItem> itemRef = *it;
    const auto originalIndex = std::distance(oldParent->children.begin(), it);
    const ParentRef oldParentRef = item->parentRef;
    const auto* oldSelection = oldParent->comp<VSelection>();
    const auto* newSelection = newParent->comp<VSelection>();
    const auto oldSelections = oldSelection ? std::optional(oldSelection->selections) : std::nullopt;
    const auto newSelections = newSelection ? std::optional(newSelection->selections) : std::nullopt;
    std::vector<SelectionEntry> removedRows;
    std::optional<SelectionEntry> installed;
    if (oldParent->type == ProjectItem::Type::Container) {
        removedRows =
            PlayablePropertyAuthoringV1::removeContainerChildOccurrences(
                *oldParent, item->guid);
    }

    // Extension point ④: buses commit the reparent to the engine BusTree
    // first — a runtime refusal aborts the move with the model untouched.
    if (item->type == ProjectItem::Type::Bus) {
        if (!reparentRuntime(item, busRuntimeHandle(newParent))) {
            if (oldSelections) oldParent->comp<VSelection>()->selections = *oldSelections;
            if (newSelections) newParent->comp<VSelection>()->selections = *newSelections;
            return false;
        }
    }

    oldParent->children.erase(it);
    itemRef->parent = newParent;
    const bool hasFieldGroups =
        PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item->type);
    if (hasFieldGroups) {
        itemRef->parentRef = ParentRef{};
        if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(newParent->type)) {
            itemRef->parentRef.kind = ParentRef::Kind::Item;
            itemRef->parentRef.guid = newParent->guid;
            itemRef->parentRef.documentId = newParent->guid;
        }
    }
    const int childCount = static_cast<int>(newParent->children.size());
    const int installedIndex = insertIndex >= 0 && insertIndex <= childCount
        ? insertIndex : childCount;
    newParent->children.insert(newParent->children.begin() + installedIndex, itemRef);
    const auto rollback = [&] {
        newParent->children.erase(newParent->children.begin() + installedIndex);
        oldParent->children.insert(oldParent->children.begin() + originalIndex, itemRef);
        itemRef->parent = oldParent;
        itemRef->parentRef = oldParentRef;
        if (oldSelections) oldParent->comp<VSelection>()->selections = *oldSelections;
        if (newSelections) newParent->comp<VSelection>()->selections = *newSelections;
    };
    if (newParent->type == ProjectItem::Type::Container
        && (!restoreEmptySourceOccurrences || restoredDestination)) {
        SelectionEntry row;
        if (!PlayablePropertyAuthoringV1::addContainerChildOccurrence(
            *newParent, item->guid, restoredDestination, &row)) {
            rollback();
            return false;
        }
        installed = std::move(row);
    }

    if (!PlayablePropertyAuthoringV1::validateContainerOccurrences(*oldParent)
        || !PlayablePropertyAuthoringV1::validateContainerOccurrences(*newParent)
        || (hasFieldGroups && (!m_currentProject
            || !validateFieldGroupProject(*m_currentProject).ok()
            || !refreshFieldGroupDerivedViews(*m_currentProject)))) {
        rollback();
        return false;
    }
    if (createDefaultClip && !ensureTrackChildClip(newParent, item)) {
        rollback();
        return false;
    }
    recomputeObjectPathRecursive(itemRef.get());

    oldParent->markDirty();
    newParent->markDirty();
    itemRef->markDirty();
    if (removedSource) removedSource->reset();
    if (installedDestination) installedDestination->reset();
    if (removedSources) removedSources->clear();
    if (removedSource && !removedRows.empty())
        *removedSource = removedRows.front();
    if (removedSources) *removedSources = std::move(removedRows);
    if (installedDestination) *installedDestination = std::move(installed);
    auto playbackLog = cleanupIllegalPlaybackReferences(currentProject());
    if (outLog && oldSelections)
        playbackLog.selections.push_back({oldParent->objectID, *oldSelections});
    if (outLog) *outLog = std::move(playbackLog);
    return true;
}

bool ProjectManager::mutateItemInternal(ProjectItem* item, const std::function<void(ProjectItem*)>& mutator,
                                        const QString& propertyName)
{
    if (!item || !mutator) return false;

    mutator(item);

    item->markDirty();
    if (!propertyName.isEmpty()) {
        emit itemPropertyChanged(item, propertyName);
    }
    return true;
}

namespace {

const PropertyDescriptor* findDescriptorByName(const PropertySchema& schema, const QString& name)
{
    for (const auto& d : schema) {
        if (d.field == name) return &d;
        for (const auto& alias : d.aliases)
            if (alias == name) return &d;
    }
    return nullptr;
}

QString canonicalAudioProperty(const QString& propertyName)
{
    static const QHash<QString, QString> kAliases = {
        {QStringLiteral("makeup_gain"),       QStringLiteral("makeUpGain")},
        {QStringLiteral("loop_count"),        QStringLiteral("loopCount")},
        {QStringLiteral("streaming_mode"),    QStringLiteral("streamingMode")},
        {QStringLiteral("outputBus"),         QStringLiteral("outputBusId")},
        {QStringLiteral("output_bus"),        QStringLiteral("outputBusId")},
        {QStringLiteral("makeupgain"),        QStringLiteral("makeUpGain")},
    };
    return kAliases.value(propertyName, propertyName);
}

}

bool ProjectManager::restoreFieldGroupsInternal(
    ProjectItem* item, const DefinitionFieldGroups& replacement,
    const std::optional<DefinitionFieldGroups>& expectedBefore,
    const QObject* originator)
{
    if (!item || !m_currentProject) return false;
    const auto source = HarkenEditor::Bridge::canonicalRuntimeProjectSourceIndex(*m_currentProject);
    const auto found = source->byQtGuid.find(item->guid);
    if (found == source->byQtGuid.end() || found->second != item) return false;
    const auto prepared = prepareFieldGroupRestore(*m_currentProject, item->guid,
                                                     replacement, expectedBefore);
    if (!prepared.ok()) return false;
    if (!prepared.changed) return true;
    item->fieldGroups = *prepared.after;
    if (!refreshFieldGroupDerivedViews(*m_currentProject)) {
        item->fieldGroups = *prepared.before;
        return false;
    }
    item->markDirty();
    emit itemPropertyChanged(item, QStringLiteral("fieldGroups"), originator);
    return true;
}

bool ProjectManager::writeAudioPropertySilent(ProjectItem* item, const QString& propertyName, const QVariant& value)
{
    if (!item) return false;
    const QString canonical = canonicalAudioProperty(propertyName);
    if (canonical == QLatin1String("scheduleOffsetSec")) {
        auto* sound = item->comp<VSound>();
        bool valid = false;
        const double offset = value.toDouble(&valid);
        if (!sound || !valid || !std::isfinite(offset) || offset < 0.0) return false;
        sound->scheduleOffsetSec = offset;
        return true;
    }
    if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item->type)) {
        const auto* field = PlayablePropertyAuthoringV1::fieldByName(canonical);
        if ((field && FieldGroupForProperty(static_cast<harkenplayableproperties::PlayablePropertyIdV1>(field->id)))
            || canonical.compare(QStringLiteral("outputBusId"), Qt::CaseInsensitive) == 0)
            return false;
    }
    if (const PropertyDescriptor* m = findDescriptorByName(VMixNode::schema(), canonical)) {
        if (!m->set) return false;
        auto* mix = item->compOrCreate<VMixNode>();
        mix->setHostItem(item);
        return m->set(mix, m->coerce ? m->coerce(value) : value);
    }
    if (const PropertyDescriptor* d = findDescriptorByName(VAudio::schema(), canonical)) {
        if (!d->set) return false;
        if (!ItemTypeCatalog::isContentObject(item->type)) return false;
        return d->set(item->compOrCreate<VAudio>(), d->coerce ? d->coerce(value) : value);
    }
    const PropertyDescriptor* s = findDescriptorByName(VSpatial::schema(), canonical);
    if (!s || !s->set) return false;
    if (!ItemTypeCatalog::hasSpatialCapability(item->type)) return false;
    auto* spatial = item->comp<VSpatial>();
    if (!spatial) return false;
    return s->set(spatial, s->coerce ? s->coerce(value) : value);
}

uint32_t ProjectManager::resolveEffectiveOutputBusId(const ProjectItem* item)
{
    return ReferenceSlotResolution::resolveEffectiveOutputBusId(item);
}

namespace {

FieldGroupEffectiveSnapshot currentFieldGroupSnapshot(const ProjectData* project)
{
    if (!project) return {};
    return buildFieldGroupEffectiveSnapshot(
        *project,
        toFieldGroupScope(currentWorldPlatformScope(PlatformContextService::instance().context())));
}

uint32_t outputBusIdFromSnapshot(
    const ProjectItem* item, const FieldGroupEffectiveSnapshot& snapshot)
{
    if (!item || !snapshot.ok()) return 0;
    const auto guid = toFieldGroupGuid(item->guid);
    const auto owner = snapshot.objects.find(guid);
    const auto effective = snapshot.definitions.find(guid);
    if (owner == snapshot.objects.end() || owner->second != item
        || effective == snapshot.definitions.end())
        return 0;
    return snapshot.objects.at(effective->second.mainOutput.mainOutputBusRef)->objectID;
}

}

float ProjectManager::aggregatePitchForItem(const ProjectItem* item) const
{
    return aggregatePitchForItem(item, findBusItem(resolveEffectiveOutputBusId(item)));
}

float ProjectManager::aggregatePitchForItem(
    const ProjectItem* item, const ProjectItem* outputBus) const
{
    float accum = 0.0f;
    for (const ProjectItem* p = item; p; p = p->parent) {
        if (const auto* m = p->comp<VMixNode>())
            accum += m->pitch;
    }
    const ProjectItem* bus = outputBus;
    while (bus && bus->comp<VBus>()) {
        if (const auto* m = bus->comp<VMixNode>())
            accum += m->pitch;
        bus = bus->parent;
    }
    return accum;
}

void ProjectManager::pushItemPitchLive(ProjectItem* item)
{
    if (!item || item->type != ProjectItem::Type::Sound) return;
    AudioEngine::instance().setVoicePitchByName(item->name, aggregatePitchForItem(item));
}

void ProjectManager::pushItemPitchLive(
    ProjectItem* item, const FieldGroupEffectiveSnapshot& snapshot)
{
    if (!item || item->type != ProjectItem::Type::Sound) return;
    AudioEngine::instance().setVoicePitchByName(
        item->name, aggregatePitchForItem(item, findBusItem(outputBusIdFromSnapshot(item, snapshot))));
}

void ProjectManager::propagateItemPitchLive(ProjectItem* item)
{
    if (!item) return;
    const auto snapshot = currentFieldGroupSnapshot(m_currentProject.get());
    const auto visit = [&](ProjectItem* node, const auto& self) -> void {
        pushItemPitchLive(node, snapshot);
        for (const auto& child : node->children)
            self(child.get(), self);
    };
    visit(item, visit);
}

float ProjectManager::aggregateVolumeDbForItem(const ProjectItem* item) const
{
    float accum = 0.0f;
    for (const ProjectItem* p = item; p; p = p->parent) {
        if (p->type == ProjectItem::Type::Bus) break;
        if (const auto* m = p->comp<VMixNode>())
            accum += m->volume + m->makeUpGain;
    }
    return accum;
}

float ProjectManager::effectiveOutputVolumeDbForItem(const ProjectItem* item) const
{
    if (!item)
        return 0.0f;
    if (item->type == ProjectItem::Type::Bus) {
        const auto* mix = item->comp<VMixNode>();
        return mix ? mix->outputBusVolume : 0.0f;
    }
    if (!m_currentProject)
        return 0.0f;
    return effectiveOutputVolumeDbForItem(item, currentFieldGroupSnapshot(m_currentProject.get()));
}

float ProjectManager::effectiveOutputVolumeDbForItem(
    const ProjectItem* item, const FieldGroupEffectiveSnapshot& snapshot) const
{
    const auto guid = toFieldGroupGuid(item->guid);
    const auto owner = snapshot.objects.find(guid);
    const auto effective = snapshot.definitions.find(guid);
    if (!snapshot.ok() || owner == snapshot.objects.end() || owner->second != item
        || effective == snapshot.definitions.end())
        return 0.0f;
    return effective->second.mainOutput.outputBusVolume;
}

float ProjectManager::effectiveBusVolumeDb(const ProjectItem* busItem) const
{
    const auto* mix = busItem ? busItem->comp<VMixNode>() : nullptr;
    return mix ? mix->volume + mix->makeUpGain : 0.0f;
}

void ProjectManager::pushItemVolumeLive(ProjectItem* item)
{
    if (!item) return;
    if (item->type != ProjectItem::Type::Sound &&
        item->type != ProjectItem::Type::Layer) return;
    pushItemVolumeLive(item, currentFieldGroupSnapshot(m_currentProject.get()));
}

void ProjectManager::pushItemVolumeLive(
    ProjectItem* item, const FieldGroupEffectiveSnapshot& snapshot)
{
    if (!item || (item->type != ProjectItem::Type::Sound
        && item->type != ProjectItem::Type::Layer)) return;
    AudioEngine::instance().setVoiceVolumeByName(item->name, aggregateVolumeDbForItem(item));
    AudioEngine::instance().setVoiceOutputVolumeByName(
        item->name, effectiveOutputVolumeDbForItem(item, snapshot));
}

void ProjectManager::propagateItemVolumeLive(ProjectItem* item)
{
    if (!item || item->type == ProjectItem::Type::Bus) return;
    const auto snapshot = currentFieldGroupSnapshot(m_currentProject.get());
    const auto visit = [&](ProjectItem* node, const auto& self) -> void {
        if (!node || node->type == ProjectItem::Type::Bus) return;
        pushItemVolumeLive(node, snapshot);
        for (const auto& child : node->children)
            self(child.get(), self);
    };
    visit(item, visit);
}

std::vector<Send> ProjectManager::aggregateVoiceSendsForItem(const ProjectItem* item) const
{
    if (!item || !m_currentProject)
        return {};
    return aggregateVoiceSendsForItem(item, currentFieldGroupSnapshot(m_currentProject.get()));
}

std::vector<Send> ProjectManager::aggregateVoiceSendsForItem(
    const ProjectItem* item, const FieldGroupEffectiveSnapshot& snapshot) const
{
    std::vector<Send> out;
    const auto guid = toFieldGroupGuid(item->guid);
    const auto owner = snapshot.objects.find(guid);
    const auto effective = snapshot.definitions.find(guid);
    if (!snapshot.ok() || owner == snapshot.objects.end() || owner->second != item
        || effective == snapshot.definitions.end())
        return out;
    for (const auto& send : effective->second.send.sends) {
        out.push_back({snapshot.objects.at(send.targetBusID)->objectID,
                       VMixNode::dbToLinear(send.sendLevel)});
    }
    return out;
}

void ProjectManager::pushItemSendsLive(ProjectItem* item)
{
    if (!item || item->type != ProjectItem::Type::Sound) return;
    pushItemSendsLive(item, currentFieldGroupSnapshot(m_currentProject.get()));
}

void ProjectManager::pushItemSendsLive(
    ProjectItem* item, const FieldGroupEffectiveSnapshot& snapshot)
{
    if (!item || item->type != ProjectItem::Type::Sound) return;
    std::vector<std::pair<harkenstudio::ObjectId, float>> runtimeSends;
    for (const Send& s : aggregateVoiceSendsForItem(item, snapshot)) {
        if (!findVBus(s.targetBusID)) continue;
        runtimeSends.emplace_back(busRuntimeHandleForId(s.targetBusID),
                                  VMixNode::linearToDb(s.sendLevel));
    }
    AudioEngine::instance().setVoiceSendsByName(item->name, runtimeSends);
}

void ProjectManager::propagateItemSendsLive(ProjectItem* item)
{
    if (!item || item->type == ProjectItem::Type::Bus) return;
    const auto snapshot = currentFieldGroupSnapshot(m_currentProject.get());
    const auto visit = [&](ProjectItem* node, const auto& self) -> void {
        if (!node || node->type == ProjectItem::Type::Bus) return;
        if (node->type == ProjectItem::Type::Sound) {
            pushItemSendsLive(node, snapshot);
            return;
        }
        for (const auto& child : node->children)
            self(child.get(), self);
    };
    visit(item, visit);
}

float ProjectManager::runtimeSpatMixForValue(float spatializationMix)
{
    return qBound(0.0f, spatializationMix, 1.0f);
}

void ProjectManager::pushItemSpatMixLive(ProjectItem* item)
{
    if (!item || item->type != ProjectItem::Type::Sound) return;
    const auto* sp = item->comp<VSpatial>();
    AudioEngine::instance().setVoiceSpatMixByName(
        item->name, runtimeSpatMixForValue(sp ? sp->spatializationMix : 1.0f));
}

void ProjectManager::propagateItemSpatMixLive(ProjectItem* item)
{
    if (!item || item->type == ProjectItem::Type::Bus) return;
    pushItemSpatMixLive(item);
    for (auto& child : item->children)
        propagateItemSpatMixLive(child.get());
}

float ProjectManager::runtimeDistanceScaleForValue(float distanceScaling)
{
    return qBound(0.01f, distanceScaling, 100.0f);
}

void ProjectManager::pushItemDistanceScaleLive(ProjectItem* item)
{
    if (!item || item->type != ProjectItem::Type::Sound || !m_currentProject) return;
    pushItemDistanceScaleLive(item, currentFieldGroupSnapshot(m_currentProject.get()));
}

void ProjectManager::pushItemDistanceScaleLive(
    ProjectItem* item, const FieldGroupEffectiveSnapshot& snapshot)
{
    if (!item || item->type != ProjectItem::Type::Sound) return;
    const auto effective = snapshot.definitions.find(toFieldGroupGuid(item->guid));
    if (!snapshot.ok() || effective == snapshot.definitions.end())
        return;
    AudioEngine::instance().setVoiceDistanceScaleByName(
        item->name,
        runtimeDistanceScaleForValue(
            effective->second.spatialRating.spatialAttenuationDistanceScaling));
}

void ProjectManager::propagateItemDistanceScaleLive(ProjectItem* item)
{
    if (!item || item->type == ProjectItem::Type::Bus || !m_currentProject) return;
    const auto snapshot = currentFieldGroupSnapshot(m_currentProject.get());
    const auto visit = [&](ProjectItem* node, const auto& self) -> void {
        if (!node || node->type == ProjectItem::Type::Bus) return;
        pushItemDistanceScaleLive(node, snapshot);
        for (const auto& child : node->children)
            self(child.get(), self);
    };
    visit(item, visit);
}

void ProjectManager::pushAudioPropertyLive(ProjectItem* item, const QString& propertyName)
{
    if (!item) return;

    const QString canonical = canonicalAudioProperty(propertyName);
    if (canonical == QStringLiteral("pitch")) {
        pushItemPitchLive(item);
        return;
    }
    if (canonical == QStringLiteral("spatializationMix")) {
        return;
    }
    if (canonical == QStringLiteral("attenuationDistanceScaling")) {
        propagateItemDistanceScaleLive(item);
        return;
    }
    if (canonical == QStringLiteral("volume") ||
        canonical == QStringLiteral("makeUpGain") ||
        canonical == QStringLiteral("outputBusVolume"))
    {
        propagateItemVolumeLive(item);
    }
}

bool ProjectManager::applyItemPropertyLive(ProjectItem* item, const QString& propertyName, const QVariant& value)
{
    if (!writeAudioPropertySilent(item, propertyName, value)) return false;
    pushAudioPropertyLive(item, propertyName);
    return true;
}

bool ProjectManager::setAudioPropertyInternal(ProjectItem* item, const QString& propertyName, const QVariant& value,
                                              const QObject* originator)
{
    if (!writeAudioPropertySilent(item, propertyName, value)) return false;
    item->markDirty();
    emit itemPropertyChanged(item, propertyName, originator);
    return true;
}

bool ProjectManager::setItemVolumeInternal(ProjectItem* item, float volume, const QObject* originator)
{
    return setAudioPropertyInternal(item, QStringLiteral("volume"), volume, originator);
}

bool ProjectManager::setItemPitchInternal(ProjectItem* item, float pitch, const QObject* originator)
{
    return setAudioPropertyInternal(item, QStringLiteral("pitch"), pitch, originator);
}

bool ProjectManager::setItemMakeUpGainInternal(ProjectItem* item, float makeUpGain, const QObject* originator)
{
    return setAudioPropertyInternal(item, QStringLiteral("makeUpGain"), makeUpGain, originator);
}

bool ProjectManager::setItemLoopCountInternal(ProjectItem* item, uint32_t loopCount, const QObject* originator)
{
    return setAudioPropertyInternal(
        item, QStringLiteral("loopCount"),
        QVariant::fromValue(static_cast<qulonglong>(loopCount)), originator);
}

bool ProjectManager::setItemOutputBusRefInternal(ProjectItem* item, uint32_t busID, const QObject* originator)
{
    return setAudioPropertyInternal(item, QStringLiteral("outputBus"), busID, originator);
}

bool ProjectManager::reorderItemChildrenInternal(
    ProjectItem* parent,
    const std::vector<std::shared_ptr<ProjectItem>>& orderedChildren)
{
    if (!parent) return false;
    if (!PlayablePropertyAuthoringV1::validateContainerOccurrences(*parent))
        return false;
    if (parent->children.size() != orderedChildren.size()) return false;

    std::vector<ProjectItem*> remaining;
    remaining.reserve(parent->children.size());
    for (const auto& child : parent->children) {
        remaining.push_back(child.get());
    }

    for (const auto& child : orderedChildren) {
        if (!child || child->parent != parent) return false;
        auto it = std::find(remaining.begin(), remaining.end(), child.get());
        if (it == remaining.end()) return false;
        remaining.erase(it);
    }

    if (!remaining.empty()) return false;

    parent->children = orderedChildren;
    parent->markDirty();
    emit itemPropertyChanged(parent, QStringLiteral("childOrder"));
    return true;
}

ProjectItem* ProjectManager::getMasterBusItem() const
{
    if (!m_currentProject) return nullptr;
    if (m_currentProject->busses.empty()) return nullptr;
    return m_currentProject->busses[0].rootItem.get();
}

ProjectItem* ProjectManager::findBusItem(uint32_t objectID) const
{
    ProjectItem* master = getMasterBusItem();
    if (!master) return nullptr;
    if (objectID == kMasterBusID) return master;
    std::function<ProjectItem*(ProjectItem*)> walk = [&](ProjectItem* node) -> ProjectItem* {
        if (!node) return nullptr;
        if (node->objectID == objectID) return node;
        for (auto& child : node->children) {
            if (auto* found = walk(child.get())) return found;
        }
        return nullptr;
    };
    return walk(master);
}

harkenstudio::ObjectId ProjectManager::busRuntimeHandle(const ProjectItem* item) const
{
    if (!item)
        return {};
    return item->runtimeId();
}

harkenstudio::ObjectId ProjectManager::busRuntimeHandleForId(uint32_t objectID) const
{
    return busRuntimeHandle(findBusItem(objectID));
}

VBus* ProjectManager::findVBus(uint32_t busID)
{
    ProjectItem* item = findBusItem(busID);
    return item ? item->comp<VBus>() : nullptr;
}

const VBus* ProjectManager::findVBus(uint32_t busID) const
{
    ProjectItem* item = findBusItem(busID);
    return item ? item->comp<VBus>() : nullptr;
}

std::vector<ProjectItem*> ProjectManager::collectBusItems() const
{
    std::vector<ProjectItem*> out;
    ProjectItem* master = getMasterBusItem();
    if (!master) return out;
    std::function<void(ProjectItem*)> walk = [&](ProjectItem* node) {
        if (!node) return;
        out.push_back(node);
        for (auto& child : node->children) walk(child.get());
    };
    walk(master);
    return out;
}


uint32_t ProjectManager::masterBusID() const
{
    return m_currentProject ? kMasterBusID : 0u;
}

ProjectItem* ProjectManager::findBusItemByName(const QString& name) const
{
    return m_currentProject ? m_currentProject->findItemBySelector(name, ProjectItem::Type::Bus) : nullptr;
}

std::vector<ProjectItem*> ProjectManager::getChildBusItems(uint32_t parentBusID) const
{
    std::vector<ProjectItem*> out;
    ProjectItem* parent = findBusItem(parentBusID);
    if (!parent) return out;
    for (const auto& child : parent->children) {
        if (child && child->comp<VBus>())
            out.push_back(child.get());
    }
    return out;
}

void ProjectManager::forEachMixNodeItem(const std::function<void(ProjectItem*, VMixNode*)>& fn) const
{
    if (!m_currentProject) return;
    QSet<const ProjectItem*> seen;
    std::function<void(ProjectItem*)> walk = [&](ProjectItem* item) {
        if (!item || seen.contains(item)) return;
        seen.insert(item);
        if (auto* mix = item->comp<VMixNode>()) fn(item, mix);
        for (auto& child : item->children) walk(child.get());
    };
    for (auto* binding : m_currentProject->allFileBindings())
        if (binding && binding->rootItem)
            walk(binding->rootItem.get());
    walk(getMasterBusItem());
}

bool ProjectManager::busHasIncomingSends(uint32_t busId) const
{
    return !collectIncomingSends(busId).empty();
}

std::vector<IncomingSendInfo> ProjectManager::collectIncomingSends(uint32_t busId) const
{
    std::vector<IncomingSendInfo> out;
    if (!m_currentProject) return out;
    const auto snapshot = buildFieldGroupEffectiveSnapshot(
        *m_currentProject,
        toFieldGroupScope(currentWorldPlatformScope(PlatformContextService::instance().context())));
    forEachMixNodeItem([&](ProjectItem* item, VMixNode* mix) {
        if (item->comp<VBus>()) {
            for (const auto& send : mix->sends) {
                if (send.targetBusID == busId)
                    out.push_back({item->objectID, item->name, send.sendLevel});
            }
            return;
        }
        const auto guid = toFieldGroupGuid(item->guid);
        const auto effective = snapshot.definitions.find(guid);
        if (effective == snapshot.definitions.end()
            || effective->second.sendSource.definition != guid)
            return;
        for (const auto& send : effective->second.send.sends) {
            if (snapshot.objects.at(send.targetBusID)->objectID == busId)
                out.push_back({item->objectID, item->name, VMixNode::dbToLinear(send.sendLevel)});
        }
    });
    return out;
}

std::vector<MainInputInfo> ProjectManager::collectMainInputs(uint32_t busId) const
{
    std::vector<MainInputInfo> out;
    if (!m_currentProject) return out;
    for (const auto* bus : getChildBusItems(busId)) {
        const auto* mix = bus->comp<VMixNode>();
        out.push_back({bus->objectID, mix->outputBusVolume});
    }
    const auto snapshot = buildFieldGroupEffectiveSnapshot(
        *m_currentProject,
        toFieldGroupScope(currentWorldPlatformScope(PlatformContextService::instance().context())));
    forEachMixNodeItem([&](ProjectItem* item, VMixNode*) {
        const auto guid = toFieldGroupGuid(item->guid);
        const auto effective = snapshot.definitions.find(guid);
        if (effective == snapshot.definitions.end()
            || effective->second.mainOutputSource.definition != guid)
            return;
        const auto& output = effective->second.mainOutput;
        if (snapshot.objects.at(output.mainOutputBusRef)->objectID == busId)
            out.push_back({item->objectID, output.outputBusVolume});
    });
    return out;
}

QString ProjectManager::getBusPath(uint32_t busID) const
{
    QStringList path;
    ProjectItem* master  = getMasterBusItem();
    ProjectItem* current = findBusItem(busID);
    while (current) {
        path.prepend(current->name);
        if (current == master || !current->parent) break;
        current = current->parent;
    }
    return path.join("/");
}

bool ProjectManager::isDescendantOfBus(uint32_t busID, uint32_t ancestorID) const
{
    ProjectItem* master  = getMasterBusItem();
    ProjectItem* current = findBusItem(busID);
    while (current && current != master && current->parent) {
        current = current->parent;
        if (current && current->objectID == ancestorID) return true;
    }
    return false;
}

namespace {

BusRouteSnapshot buildBusRouteSnapshot(const ProjectManager& pm)
{
    BusRouteSnapshot g;
    const std::vector<ProjectItem*> buses = pm.collectBusItems();
    const ProjectItem* master = pm.getMasterBusItem();

    QSet<uint32_t> busIDs;
    for (const ProjectItem* item : buses) {
        if (item && item->objectID != 0) busIDs.insert(item->objectID);
    }

    for (const ProjectItem* item : buses) {
        if (!item || item->objectID == 0) continue;
        const bool hasBusParent =
            item != master && item->parent && busIDs.contains(item->parent->objectID);
        g.parentOf.insert(item->objectID, hasBusParent ? item->parent->objectID : 0u);

        const auto* mix = item->comp<VMixNode>();
        if (!mix) continue;
        QVector<uint32_t> targets;
        for (const auto& send : mix->sends) {
            if (send.targetBusID != 0 && busIDs.contains(send.targetBusID))
                targets.push_back(send.targetBusID);
        }
        if (!targets.isEmpty()) g.sendTargets.insert(item->objectID, targets);
    }
    return g;
}

}

bool ProjectManager::sendWouldCycle(uint32_t fromBusID, uint32_t toBusID) const
{
    if (fromBusID == 0 || toBusID == 0) return false;
    if (fromBusID == toBusID) return true;
    return busFlowReaches(buildBusRouteSnapshot(*this), toBusID, fromBusID);
}

bool ProjectManager::busMoveWouldCycle(const ProjectItem* moved, const ProjectItem* newParent) const
{
    if (!moved || !newParent) return false;
    if (moved == newParent) return true;
    // A cycle exists iff, in the post-move graph, audio from newParent flows
    // back into moved. Reaching moved terminates the search, so moved's own
    // (replaced) parent edge is never traversed and the current snapshot is
    // sufficient.
    return busFlowReaches(buildBusRouteSnapshot(*this), newParent->objectID, moved->objectID);
}

bool ProjectManager::validateStructuralPlacement(ProjectItem::Type type,
                                                 const ProjectItem* parent,
                                                 QString* err,
                                                 const ProjectItem* instance) const
{
    auto fail = [err](const QString& message) {
        if (err) *err = message;
        return false;
    };

    // Only buses currently carry per-instance placement rules beyond the
    // type-level canBeChildOf() gate; every other type passes through.
    if (type != ProjectItem::Type::Bus)
        return true;

    if (!validateBusHierarchyPlacement(parent, getMasterBusItem(), err))
        return false;

    if (instance && busMoveWouldCycle(instance, parent))
        return fail(QStringLiteral("Cannot move '%1' under '%2': the move would close a send loop "
                                   "(audio would feed back into itself through a send).")
                        .arg(instance->name, parent->name));

    return true;
}

void ProjectManager::materializeRuntime(ProjectItem* item, harkenstudio::ObjectId parentRuntimeId)
{
    if (!item) return;
    using T = ProjectItem::Type;
    if (item->type == T::Bus) {
        Q_UNUSED(parentRuntimeId);
        AudioEngine::instance().runtimeSyncCoordinator().markCurrentWorldMutation();
        return;
    }
    if (item->type == T::Action) {
        syncActionNameForPreview(item);
        return;
    }
    // Non-bus objects live in the runtime object registry; the resync path
    // recomputes their runtime parent itself, so the bus-oriented
    // parentRuntimeId argument is unused for them.
    resyncItemToRuntime(item);
}

void ProjectManager::materializeBusSubtree(ProjectItem* item, harkenstudio::ObjectId parentRuntimeId)
{
    Q_UNUSED(item);
    Q_UNUSED(parentRuntimeId);
    AudioEngine::instance().runtimeSyncCoordinator().markCurrentWorldMutation();
}

void ProjectManager::dematerializeRuntime(ProjectItem* item)
{
    if (!item) return;
    using T = ProjectItem::Type;
    if (item->type == T::Bus) {
        AudioEngine::instance().runtimeSyncCoordinator().markCurrentWorldMutation();
        return;
    }
    unregisterItemFromRuntime(item);
}

bool ProjectManager::reparentRuntime(ProjectItem* item, harkenstudio::ObjectId newParentRuntimeId)
{
    if (!item) return false;
    using T = ProjectItem::Type;
    if (item->type == T::Bus) {
        Q_UNUSED(newParentRuntimeId);
        return true;
    }
    resyncItemToRuntime(item);
    return true;
}

void ProjectManager::applyBusRuntimeState(ProjectItem* item)
{
    if (!item || item->type != ProjectItem::Type::Bus)
        return;

    AudioEngine::instance().runtimeSyncCoordinator().markCurrentWorldMutation();
}

void ProjectManager::syncBusTreeToRuntime(
    ProjectData* project,
    std::shared_ptr<const harkenbuscompiler::CompiledBusResultV1> compiledBus)
{
    Q_UNUSED(project);
    Q_UNUSED(compiledBus);
    AudioEngine::instance().runtimeSyncCoordinator().markCurrentWorldMutation();
}

namespace {

bool writePlayableDefinitionOverride(ProjectItem& owner,
                                    const QString& propertyName,
                                    const QVariant& value)
{
    const auto* field = PlayablePropertyAuthoringV1::fieldByName(propertyName);
    if (!field || !PlayablePropertyAuthoringV1::isDefinitionType(owner.type)
        || !PlayablePropertyAuthoringV1::isSlotApplicable(owner.type, field->id))
        return false;
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(owner);
    if (!locator) return false;
    const auto wire = PlayablePropertyAuthoringV1::wireValueFromVariant(field->id, value);
    if (!wire) return false;
    return PlayablePropertyAuthoringV1::setOverride(
        owner, *locator, field->id, *wire, std::nullopt);
}

bool replaceSends(ProjectItem& owner, VMixNode& mix, std::vector<Send> next)
{
    if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(owner.type)) return false;
    const auto id = static_cast<uint8_t>(
        harkenplayableproperties::PlayablePropertyIdV1::Sends);
    if (PlayablePropertyAuthoringV1::isDefinitionType(owner.type)) {
        const auto locator = PlayablePropertyAuthoringV1::definitionLocator(owner);
        if (!locator
            || !PlayablePropertyAuthoringV1::isSlotApplicable(owner.type, id))
            return false;
        QJsonArray rows;
        for (const Send& send : next) {
            rows.append(QJsonObject{
                {QStringLiteral("targetBusID"),
                 static_cast<qint64>(send.targetBusID)},
                {QStringLiteral("sendLevel"),
                 VMixNode::linearToDb(send.sendLevel)},
            });
        }
        return PlayablePropertyAuthoringV1::setOverride(
            owner, *locator, id, rows, std::nullopt);
    }
    mix.sends = std::move(next);
    return true;
}

}

bool ProjectManager::writeBusPropertySilent(ProjectItem* busItem, const QString& propertyName, const QVariant& value)
{
    if (!busItem) return false;
    const auto* field = PlayablePropertyAuthoringV1::fieldByName(propertyName);
    if (busItem->type == ProjectItem::Type::Bus
        && field
        && field->id == static_cast<uint8_t>(
               harkenplayableproperties::PlayablePropertyIdV1::OutputBusVolume)) {
        const PropertyDescriptor* mixField =
            findDescriptorByName(VMixNode::schema(), QStringLiteral("outputBusVolume"));
        if (!mixField || !mixField->set) return false;
        auto* mix = busItem->compOrCreate<VMixNode>();
        mix->setHostItem(busItem);
        if (!mixField->set(mix, mixField->coerce ? mixField->coerce(value) : value))
            return false;
        busItem->playablePropertyPresence.Set(
            harkenplayableproperties::PlayablePropertyIdV1::OutputBusVolume);
        return true;
    }
    if (field
        && PlayablePropertyAuthoringV1::isDefinitionType(busItem->type)
        && PlayablePropertyAuthoringV1::isSlotApplicable(busItem->type, field->id))
        return writePlayableDefinitionOverride(*busItem, propertyName, value);
    if (!busItem->comp<VBus>()) return false;
    if (const PropertyDescriptor* m = findDescriptorByName(VMixNode::schema(), propertyName)) {
        if (!m->set) return false;
        auto* mix = busItem->compOrCreate<VMixNode>();
        mix->setHostItem(busItem);
        return m->set(mix, m->coerce ? m->coerce(value) : value);
    }
    const PropertyDescriptor* d = findDescriptorByName(VBus::schema(), propertyName);
    if (!d || !d->set) return false;
    return d->set(busItem->comp<VBus>(), d->coerce ? d->coerce(value) : value);
}

void ProjectManager::pushBusPropertyLive(ProjectItem* busItem, const QString& propertyName)
{
    auto* mix = busItem ? busItem->comp<VMixNode>() : nullptr;
    if (!mix) return;
    if (propertyName == QStringLiteral("volume")
        || propertyName == QStringLiteral("makeUpGain")
        || propertyName == QStringLiteral("outputBusVolume"))
        AudioEngine::instance().runtimeSyncCoordinator().markCurrentWorldMutation();
    else if (propertyName == QStringLiteral("pitch"))
        propagateBusPitchLive(busItem);
}

void ProjectManager::propagateBusPitchLive(ProjectItem* busItem)
{
    if (!busItem || !m_currentProject) return;
    const uint32_t changedBusID = busItem->objectID;
    const auto snapshot = currentFieldGroupSnapshot(m_currentProject.get());
    std::function<void(ProjectItem*)> visit = [&](ProjectItem* item) {
        if (!item) return;
        if (item->type == ProjectItem::Type::Sound) {
            const ProjectItem* routed = findBusItem(outputBusIdFromSnapshot(item, snapshot));
            if (routed && (routed == busItem ||
                           isDescendantOfBus(routed->objectID, changedBusID)))
                AudioEngine::instance().setVoicePitchByName(
                    item->name, aggregatePitchForItem(item, routed));
        }
        for (auto& child : item->children)
            visit(child.get());
    };
    if (m_currentProject->root)
        visit(m_currentProject->root.get());
    for (auto* binding : m_currentProject->allFileBindings())
        if (binding && binding->rootItem)
            visit(binding->rootItem.get());
}

bool ProjectManager::applyBusPropertyLive(ProjectItem* busItem, const QString& propertyName, const QVariant& value)
{
    if (!writeBusPropertySilent(busItem, propertyName, value)) return false;
    pushBusPropertyLive(busItem, propertyName);
    return true;
}

bool ProjectManager::setBusVolumeInternal(uint32_t busID, float volumeDb, const QObject* originator)
{
    ProjectItem* item = findBusItem(busID);
    if (!writeBusPropertySilent(item, QStringLiteral("volume"), volumeDb)) return false;
    pushBusPropertyLive(item, QStringLiteral("volume"));
    emit busChanged(busID, QStringLiteral("volume"), originator);
    return true;
}

bool ProjectManager::setBusMakeUpGainInternal(uint32_t busID, float makeUpGainDb,
                                              const QObject* originator)
{
    ProjectItem* item = findBusItem(busID);
    if (!writeBusPropertySilent(item, QStringLiteral("makeUpGain"), makeUpGainDb))
        return false;
    pushBusPropertyLive(item, QStringLiteral("makeUpGain"));
    emit busChanged(busID, QStringLiteral("makeUpGain"), originator);
    return true;
}

bool ProjectManager::setBusPanInternal(uint32_t busID, float pan, const QObject* originator)
{
    ProjectItem* item = findBusItem(busID);
    if (!writeBusPropertySilent(item, QStringLiteral("pan"), pan)) return false;
    emit busChanged(busID, QStringLiteral("pan"), originator);
    return true;
}

bool ProjectManager::setBusChannelConfigInternal(uint32_t busID, uint8_t channelConfig)
{
    auto* bc = findVBus(busID);
    if (!bc) return false;
    bc->channelConfig = channelConfig;

    emit busChanged(busID, QStringLiteral("channelConfig"));
    return true;
}

bool ProjectManager::setBusListenerRelativeInternal(uint32_t busID, bool enabled)
{
    auto* bc = findVBus(busID);
    if (!bc) return false;
    bc->listenerRelativeRouting = enabled;

    emit busChanged(busID, QStringLiteral("listenerRelativeRouting"));
    return true;
}

bool ProjectManager::setBusMuteInternal(uint32_t busID, bool mute, const QObject* originator)
{
    ProjectItem* item = findBusItem(busID);
    if (!item || !item->comp<VBus>()) return false;
    const harkenstudio::ObjectId busId = busRuntimeHandle(item);
    m_auditionOverlay.setMuted(busId, mute);
    (void)AudioEngine::instance().applyInstalledBusMuteOverlay(busId, mute, 0);
    emit busChanged(busID, QStringLiteral("mute"), originator);
    return true;
}

bool ProjectManager::setBusSoloInternal(uint32_t busID, bool solo, const QObject* originator)
{
    ProjectItem* item = findBusItem(busID);
    if (!item || !item->comp<VBus>()) return false;
    const harkenstudio::ObjectId busId = busRuntimeHandle(item);
    m_auditionOverlay.setSoloed(busId, solo);
    (void)AudioEngine::instance().applyInstalledBusSoloOverlay(busId, solo, 0);
    emit busChanged(busID, QStringLiteral("solo"), originator);
    return true;
}

bool ProjectManager::setBusPitchInternal(uint32_t busID, float cents, const QObject* originator)
{
    ProjectItem* item = findBusItem(busID);
    if (!writeBusPropertySilent(item, QStringLiteral("pitch"), cents)) return false;
    pushBusPropertyLive(item, QStringLiteral("pitch"));
    emit busChanged(busID, QStringLiteral("pitch"), originator);
    return true;
}

bool ProjectManager::setBusOutputBusVolumeInternal(uint32_t busID, float outputBusVolumeDb, const QObject* originator)
{
    ProjectItem* item = findBusItem(busID);
    if (!writeBusPropertySilent(item, QStringLiteral("outputBusVolume"), outputBusVolumeDb)) return false;
    pushBusPropertyLive(item, QStringLiteral("outputBusVolume"));
    emit busChanged(busID, QStringLiteral("outputBusVolume"), originator);
    return true;
}

ProjectItem* ProjectManager::findSendOwnerItem(uint32_t objectID) const
{
    if (ProjectItem* bus = findBusItem(objectID)) return bus;
    return findItemByObjectIDInProject(m_currentProject.get(), objectID);
}

bool ProjectManager::sendTargetSelectable(uint32_t srcObjectID, uint32_t targetBusID) const
{
    if (!sendTargetStructurallySelectable(srcObjectID, targetBusID)) return false;
    ProjectItem* src = findSendOwnerItem(srcObjectID);
    const auto* mix = src ? src->comp<VMixNode>() : nullptr;

    for (const auto& send : mix->sends)
        if (send.targetBusID == targetBusID) return false;

    return true;
}

bool ProjectManager::sendTargetStructurallySelectable(
    uint32_t srcObjectID, uint32_t targetBusID) const
{
    if (targetBusID == 0 || targetBusID == srcObjectID) return false;
    if (!findVBus(targetBusID)) return false;

    ProjectItem* src = findSendOwnerItem(srcObjectID);
    const auto* mix = src ? src->comp<VMixNode>() : nullptr;
    if (!mix) return false;

    const ProjectItem* mainOut = src->type == ProjectItem::Type::Bus
        ? src->parent
        : findBusItem(resolveEffectiveOutputBusId(src));
    if (mainOut && mainOut->objectID == targetBusID) return false;

    if (src->type == ProjectItem::Type::Bus && sendWouldCycle(srcObjectID, targetBusID))
        return false;
    return true;
}

bool ProjectManager::outputBusSelectable(uint32_t srcObjectID, uint32_t candidateBusID) const
{
    if (candidateBusID == 0 || candidateBusID == srcObjectID) return false;
    if (!findVBus(candidateBusID)) return false;

    ProjectItem* src = findSendOwnerItem(srcObjectID);
    const auto* mix = src ? src->comp<VMixNode>() : nullptr;
    if (!mix) return true;

    for (const auto& send : mix->sends)
        if (send.targetBusID == candidateBusID) return false;
    return true;
}

bool ProjectManager::upsertSendInternal(uint32_t srcObjectID, uint32_t targetBusID, float sendLevel)
{
    ProjectItem* srcItem = findSendOwnerItem(srcObjectID);
    auto* srcMix = srcItem ? srcItem->comp<VMixNode>() : nullptr;
    auto* targetBC = findVBus(targetBusID);
    if (!srcMix || !targetBC) return false;

    const bool srcIsBus = srcItem->type == ProjectItem::Type::Bus;
    std::vector<Send> next = srcMix->sends;
    bool existed = false;
    for (auto& send : next) {
        if (send.targetBusID == targetBusID) {
            send.sendLevel = sendLevel;
            existed = true;
            break;
        }
    }

    if (!existed) {
        if (srcIsBus && sendWouldCycle(srcObjectID, targetBusID)) {
            qWarning() << "[ProjectManager] Rejected send" << srcObjectID << "->" << targetBusID
                       << ": would close a send loop";
            return false;
        }
        next.push_back(Send{targetBusID, sendLevel});
    }

    if (!replaceSends(*srcItem, *srcMix, std::move(next)))
        return false;

    if (srcIsBus) {
        AudioEngine::instance().runtimeSyncCoordinator().markCurrentWorldMutation();
        emit busChanged(srcObjectID, QStringLiteral("send"));
    } else {
        propagateItemSendsLive(srcItem);
        srcItem->markDirty();
        emit itemPropertyChanged(srcItem, QStringLiteral("send"));
    }
    return true;
}

bool ProjectManager::removeSendInternal(uint32_t srcObjectID, uint32_t targetBusID)
{
    ProjectItem* srcItem = findSendOwnerItem(srcObjectID);
    auto* srcMix = srcItem ? srcItem->comp<VMixNode>() : nullptr;
    if (!srcMix) return false;

    std::vector<Send> next = srcMix->sends;
    const auto erased = std::remove_if(
        next.begin(), next.end(),
        [targetBusID](const Send& s) { return s.targetBusID == targetBusID; });
    if (erased == next.end()) return false;
    next.erase(erased, next.end());

    if (!replaceSends(*srcItem, *srcMix, std::move(next)))
        return false;

    if (srcItem->type == ProjectItem::Type::Bus) {
        AudioEngine::instance().runtimeSyncCoordinator().markCurrentWorldMutation();
        emit busChanged(srcObjectID, QStringLiteral("send"));
    } else {
        propagateItemSendsLive(srcItem);
        srcItem->markDirty();
        emit itemPropertyChanged(srcItem, QStringLiteral("send"));
    }
    return true;
}



namespace {

QString projectItemTypeString(ProjectItem::Type type)
{
    return ItemTypeCatalog::blueprintChainName(type);
}

SignalChainNode itemToChainNode(const ProjectItem* item,
                               const FieldGroupEffectiveSnapshot* groups,
                               const QString& AudioSourcesDir = QString())
{
    SignalChainNode node;
    if (!item) return node;
    node.objectID   = item->objectID;
    node.name       = item->name;
    node.type       = projectItemTypeString(item->type);
    node.itemType   = item->type;
    if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item->type)) {
        if (groups && groups->ok()) {
            const auto guid = toFieldGroupGuid(item->guid);
            const auto owner = groups->objects.find(guid);
            const auto definition = groups->definitions.find(guid);
            if (owner != groups->objects.end() && owner->second == item
                && definition != groups->definitions.end()) {
                const auto& source = definition->second.mainOutputSource;
                node.outputBusOverride = !source.isDefault && source.definition == guid;
            }
        }
    } else {
        node.outputBusOverride = item->isOutputBusOverride();
    }
    if (auto* mc = item->comp<VMixNode>()) {
        node.volume = mc->volume;
        node.pitch  = mc->pitch;
    }
    node.childCount = static_cast<int>(item->children.size());

    const QString mediaPath = item->type == ProjectItem::Type::Sound
        ? SoundSourceEditing::activeRelativePath(
              *item, PlatformContextService::instance().context().primary,
              SoundSourceEditing::previewLanguage(
                  *item, LanguageContextService::instance().language()))
        : QString{};
    if (!mediaPath.isEmpty() && !AudioSourcesDir.isEmpty()) {
        node.mediaPath = QDir(AudioSourcesDir).filePath(mediaPath);
    }
    return node;
}

const ProjectItem* findBusItemInProject(const ProjectData* project, uint32_t busID)
{
    if (project->busses.empty() || !project->busses[0].rootItem) return nullptr;
    const ProjectItem* root = project->busses[0].rootItem.get();
    if (busID == 0) return root;
    std::function<const ProjectItem*(const ProjectItem*)> walk =
        [&](const ProjectItem* node) -> const ProjectItem* {
            if (!node) return nullptr;
            if (node->objectID == busID) return node;
            for (const auto& child : node->children) {
                if (auto* found = walk(child.get())) return found;
            }
            return nullptr;
        };
    return walk(root);
}

void appendBusChain(const ProjectData* project, const ProjectItem* busItem,
                    SignalChainView& view)
{
    int busDepthGuard = 0;
    constexpr int kMaxBusDepth = 32;
    while (busItem && busDepthGuard < kMaxBusDepth) {
        ++busDepthGuard;
        const auto* bc  = busItem->comp<VBus>();
        const auto* mix = busItem->comp<VMixNode>();
        if (!bc || !mix) break;

        BusChainNode bNode;
        bNode.busID  = busItem->objectID;
        bNode.name   = busItem->name;
        bNode.volume = mix->volume;

        if (!busItem->parent)
            bNode.type = QStringLiteral("Master");
        else if (ProjectManager::instance().busHasIncomingSends(busItem->objectID))
            bNode.type = QStringLiteral("Receive");
        else
            bNode.type = QStringLiteral("Standard");

        for (const auto& send : mix->sends) {
            SendViewInfo info;
            info.targetBusID = send.targetBusID;
            info.sendLevel   = send.sendLevel;
            const ProjectItem* targetItem = findBusItemInProject(project, send.targetBusID);
            info.targetBusName = targetItem ? targetItem->name : QStringLiteral("(unknown)");
            bNode.sends.push_back(info);
        }

        view.busChain.push_back(bNode);

        if (!busItem->parent) break;

        busItem = busItem->parent;
    }
}

void appendSlotSource(SignalChainView& view, const QString& slot,
                      const ProjectItem* owner,
                      ReferenceSlotResolution::ResolutionSource source,
                      const QString& via)
{
    if (!owner) return;
    view.slotSources.push_back({
        slot,
        owner->objectID,
        owner->name,
        ReferenceSlotResolution::resolutionSourceName(source),
        via,
    });
}

SignalChainView buildSignalChainViewFromScope(
    const ProjectData* project, const ProjectItem* item,
    const ReferenceSlotResolution::ResolutionScope& scope)
{
    SignalChainView view;
    if (!item || !project) return view;

    QString AudioSourcesDir;
    if (!project->projectPath.isEmpty()) {
        AudioSourcesDir = QDir(QFileInfo(project->projectPath).absolutePath()).filePath("AudioSources");
    }

    view.self = itemToChainNode(item, scope.effectiveGroups.get(), AudioSourcesDir);

    for (const auto& child : item->children)
        view.children.push_back(itemToChainNode(child.get(), scope.effectiveGroups.get(), AudioSourcesDir));

    for (const ProjectItem* parent : scope.placementChain)
        view.placementChain.push_back(itemToChainNode(parent, scope.effectiveGroups.get()));
    for (const ProjectItem* parent : scope.organizationChain)
        view.organizationChain.push_back(itemToChainNode(parent, scope.effectiveGroups.get()));

    std::vector<QString> crumbs;
    for (const ProjectItem* current = item; current; current = current->parent)
        crumbs.push_back(current->name);
    for (auto it = crumbs.rbegin(); it != crumbs.rend(); ++it)
        view.breadcrumb.push_back(*it);

    view.placementContextValid = scope.placementContextValid;
    view.placementContextError = scope.placementContextError;

    const auto output = ReferenceSlotResolution::resolveOutputBusSlot(scope);
    if (output.owner) {
        view.busSlotOwnerObjectID = output.owner->objectID;
        appendSlotSource(view, QStringLiteral("outputBus"), output.owner,
                         output.source, output.via);
    }

    const auto attenuation =
        ReferenceSlotResolution::resolveEffectiveAttenuation(scope);
    appendSlotSource(view, QStringLiteral("attenuation"), attenuation.owner,
                     attenuation.source, attenuation.via);

    const auto sends = ReferenceSlotResolution::resolveEffectiveSends(scope);
    appendSlotSource(view, QStringLiteral("sends"), sends.owner,
                     sends.source, sends.via);

    const BindingAddress voiceVolume = BindingContractSemantics::addressOf(
        static_cast<uint8_t>(harkenplayableproperties::PlayablePropertyIdV1::Volume),
        item->type);
    const BindingAddress voicePitch = BindingContractSemantics::addressOf(
        static_cast<uint8_t>(harkenplayableproperties::PlayablePropertyIdV1::Pitch),
        item->type);
    for (const auto& binding : ReferenceSlotResolution::resolveParameterBindings(
             scope, voiceVolume)) {
        appendSlotSource(view, QStringLiteral("binding.volume"), binding.owner,
                         binding.source, binding.via);
    }
    for (const auto& binding : ReferenceSlotResolution::resolveParameterBindings(
             scope, voicePitch)) {
        appendSlotSource(view, QStringLiteral("binding.pitch"), binding.owner,
                         binding.source, binding.via);
    }

    appendBusChain(
        project,
        findBusItemInProject(
            project, ReferenceSlotResolution::resolveEffectiveOutputBusId(scope)),
        view);
    return view;
}

}

SignalChainView ProjectManager::buildSignalChainView(const ProjectItem* item) const
{
    return buildSignalChainView(m_currentProject.get(), item);
}

SignalChainView ProjectManager::buildSignalChainView(
    const ProjectItem* item, const PlacementContext& placement) const
{
    return buildSignalChainView(m_currentProject.get(), item, placement);
}

SignalChainView ProjectManager::buildSignalChainView(const ProjectData* project,
                                                     const ProjectItem* item)
{
    if (!project)
        return {};
    return buildSignalChainViewFromScope(
        project, item, ReferenceSlotResolution::buildResolutionScope(
            *project, item,
            toFieldGroupScope(currentWorldPlatformScope(PlatformContextService::instance().context()))));
}

SignalChainView ProjectManager::buildSignalChainView(
    const ProjectData* project, const ProjectItem* item,
    const PlacementContext& placement)
{
    if (!project)
        return {};
    const auto scope = ReferenceSlotResolution::buildResolutionScope(
        *project, item, placement, [project](const QUuid& guid) {
            return findItemByGuidInProject(const_cast<ProjectData*>(project), guid);
        }, toFieldGroupScope(currentWorldPlatformScope(PlatformContextService::instance().context())));
    return buildSignalChainViewFromScope(project, item, scope);
}

SignalChainView ProjectManager::buildSignalChainViewForBus(uint32_t busID) const
{
    SignalChainView view;
    const ProjectData* project = m_currentProject.get();
    if (!project) return view;

    const ProjectItem* busItem = findBusItem(busID);
    if (!busItem || !busItem->comp<VBus>()) return view;

    appendBusChain(project, busItem, view);
    view.incomingSends = collectIncomingSends(busID);
    return view;
}


std::vector<ProjectManager::ActionReference>
ProjectManager::findReferencingActions(uint32_t objectID) const
{
    return findReferencingActions(m_currentProject.get(), objectID);
}

std::vector<ProjectManager::ActionReference>
ProjectManager::findReferencingActions(const ProjectData* project, uint32_t objectID)
{
    std::vector<ActionReference> refs;
    if (!project || objectID == 0) return refs;

    std::function<void(const ProjectItem*)> traverse = [&](const ProjectItem* item) {
        if (!item) return;

        if (item->type == ProjectItem::Type::Action) {
            if (const auto* actionComponent = item->comp<VAction>()) {
                for (const auto& action : actionComponent->steps) {
                    if (action.targetObjectID == objectID) {
                        ActionReference ref;
                        ref.actionName = item->name;
                        ref.actionObjectID = item->objectID;
                        ref.actionType = action.typeString();
                        refs.push_back(ref);
                        break;
                    }
                }
            }
        }

        for (const auto& child : item->children) {
            traverse(child.get());
        }
    };

    for (const auto* binding : project->allFileBindings()) {
        if (binding && binding->rootItem) {
            traverse(binding->rootItem.get());
        }
    }

    return refs;
}

}
