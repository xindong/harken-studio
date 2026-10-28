#include "CanonicalPackSetExport.h"
#include "ProjectLanguageSemantics.h"

#include "ProjectBuildSnapshot.h"
#include "Components/VAction.h"
#include "Components/VSound.h"
#include "Components/VVariable.h"
#include "CurrentWorldProjectAdapter.h"
#include "CanonicalRuntimeProjectAdapter.h"
#include "CurrentWorldArtifactWriter.h"
#include "PackBuildPlan.h"
#include "NameValidator.h"
#include "ItemTypeCatalog.h"
#include "ProjectTypes.h"
#include "ProjectSchemaGenerated.h"
#include "ProjectSchemaValidator.h"
#include "harken_buildinfo.h"
#include "BindingRuntimeLowering.h"
#include "SoundSourceEditing.h"
#include "SoundSourceSemantics.h"
#include "VariableConsumerSemantics.h"
#include "AssetPipeline/PathUtils.h"
#include "AssetPipeline/WaveformCache.h"

#include "RuntimeBridge/CurrentWorldSyncBridgeInternal.h"
#include "RuntimeBridge/PackagingBridge.h"

#include <harken_studio_build/WorldCodec.h>
#include <harken_studio_build/MediaContentRevision.h>
#include <harken_studio_build/VariableExecutionFingerprint.h>
#include <harken_studio_build/WorldValidation.h>
#include <harken_studio_build/BusSnapshot.h>
#include <harken_studio_build/PackFormat.h>
#include <harken_studio_build/PackFormatV2.h>
#include <harken_studio_build/PackImage.h>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCryptographicHash>
#include <QLockFile>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QUuid>

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace VNX {

namespace {

namespace Bridge = HarkenEditor::Bridge;

constexpr char kStagingPrefix[] = ".harken-pack-staging-";
constexpr char kPreviousPrefix[] = ".harken-pack-previous-";
constexpr char kNextPrefix[] = ".harken-pack-next-";
constexpr char kPublishingMarker[] = ".harken-pack-publishing";
constexpr char kWaveformLock[] = ".harken-pack-waveforms.lock";

std::uint64_t fnvName(const QString& name)
{
    const QByteArray utf8 = name.toUtf8();
    return harkenstudiobuild::Fnv1a64(utf8.constData(), static_cast<std::size_t>(utf8.size()));
}

std::vector<std::uint8_t> requirementPayload(std::uint64_t packStableId,
                                             std::uint32_t ownedResources,
                                             std::uint32_t ownedDefinitions,
                                             std::uint64_t partitionRevision,
                                             std::uint64_t semanticFingerprint,
                                             std::uint64_t lineageFingerprint,
                                             std::uint64_t contentRevision,
                                             harkenstudiobuild::CanonicalBusTriple busTriple)
{
    harkenstudiobuild::PackRequirementBlock block;
    block.schemaVersion = 2;
    block.storageModelVersion = 2;
    block.packStableId = packStableId;
    block.packContentRevision = contentRevision;
    block.definitionPartitionRevision = partitionRevision;
    block.resourceClosureRevision =
        ownedResources != 0 || ownedDefinitions != 0 ? 1 : 0;
    block.semanticFingerprint = semanticFingerprint;
    block.lineageFingerprint = lineageFingerprint;
    block.ownedDefinitionCount = ownedDefinitions;
    block.ownedResourceCount = ownedResources;
    harkenstudiobuild::ApplyCanonicalBusTriple(&block, busTriple);
    std::vector<std::uint8_t> payload(harkenstudiobuild::kPackRequirementBlockV2Bytes, 0);
    harkenstudiobuild::SerializePackRequirementBlockV2(block, payload.data());
    harkenstudiobuild::StampRequirementCoverage(&payload);
    return payload;
}

std::vector<std::uint8_t> dependencyPayload(std::uint64_t prerequisiteId,
                                            std::uint32_t edgeKind,
                                            std::uint64_t expectedRevision,
                                            harkenstudiobuild::CanonicalBusTriple busTriple)
{
    harkenstudiobuild::PackDependencyEdge edge;
    edge.prerequisitePackStableId = prerequisiteId;
    edge.edgeKind = edgeKind;
    edge.required01 = 1;
    edge.expectedPrerequisiteContentRevision = expectedRevision;
    if (edgeKind == harkenstudiobuild::kPackDependencyEdgeKindInit)
        harkenstudiobuild::ApplyCanonicalBusTriple(&edge, busTriple);
    std::vector<std::uint8_t> payload(harkenstudiobuild::kPackDependencyEdgeV2Bytes, 0);
    harkenstudiobuild::SerializePackDependencyEdgeV2(edge, payload.data());
    return payload;
}

bool readAll(const QString& path, QByteArray* bytes, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("Cannot read '%1'").arg(path);
        return false;
    }
    *bytes = file.readAll();
    return true;
}

bool collectEntries(const QString& path,
                    std::vector<Bridge::PackBuildSession::CanonicalPackEntry>* entries,
                    QString* error, bool resourcesOnly = false, bool publicationMetadataOnly = false)
{
    QByteArray bytes;
    if (!readAll(path, &bytes, error))
        return false;
    if (bytes.size() < static_cast<int>(harkenstudiobuild::kPackV2HeaderBytes)) {
        if (error)
            *error = QStringLiteral("Pack '%1' is too small").arg(path);
        return false;
    }
    harkenstudiobuild::PackV2Header header;
    if (!harkenstudiobuild::ParsePackV2Header(
            reinterpret_cast<const std::uint8_t*>(bytes.constData()), &header)) {
        if (error)
            *error = QStringLiteral("Pack '%1' is not a v2 container").arg(path);
        return false;
    }
    for (std::uint32_t i = 0; i < header.entryCount; ++i) {
        const harkenstudiobuild::PackV2DirectoryEntry record = harkenstudiobuild::ParsePackV2DirectoryEntry(
            reinterpret_cast<const std::uint8_t*>(bytes.constData()) +
            header.directoryOffset + i * harkenstudiobuild::kPackV2DirectoryEntryBytes);
        if (publicationMetadataOnly
            && record.kind != harkenstudiobuild::kPackEntryKindCanonicalDefinitionPartition
            && record.kind != harkenstudiobuild::kPackEntryKindPackRequirementBlock
            && record.kind != harkenstudiobuild::kPackEntryKindPackDependencyEdge
            && record.kind != harkenstudiobuild::kPackEntryKindVariablePolicyOverlay
            && record.kind != harkenstudiobuild::kPackEntryKindVariableRequirements) continue;
        if (resourcesOnly && record.kind != harkenstudiobuild::kPackEntryKindMedia
            && record.kind != harkenstudiobuild::kPackEntryKindBus
            && record.kind != harkenstudiobuild::kPackEntryKindBusSend
            && record.kind != harkenstudiobuild::kPackEntryKindAttenuation
            && record.kind != harkenstudiobuild::kPackEntryKindBusEffect
            && record.kind != harkenstudiobuild::kPackEntryKindEffectDynamicRoute) continue;
        Bridge::PackBuildSession::CanonicalPackEntry entry;
        entry.kind = record.kind;
        entry.vintage = record.vintage;
        entry.id = record.id;
        entry.relationOrdinal = record.relationOrdinal;
        if (record.kind == harkenstudiobuild::kPackEntryKindMedia) {
            const harkenstudiobuild::PackTocEntry toc = harkenstudiobuild::ParsePackTocEntry(
                reinterpret_cast<const std::uint8_t*>(bytes.constData()) + record.offset);
            entry.payload.resize(static_cast<std::size_t>(
                harkenstudiobuild::kPackV2MediaPayloadBytes + toc.blobBytes));
            std::memcpy(entry.payload.data(),
                        reinterpret_cast<const std::uint8_t*>(bytes.constData()) +
                            record.offset,
                        harkenstudiobuild::kPackV2MediaPayloadBytes);
            harkenstudiobuild::StoreLE64(entry.payload.data() + 32, 0);
            if (toc.blobBytes != 0) {
                std::memcpy(entry.payload.data() + harkenstudiobuild::kPackV2MediaPayloadBytes,
                            reinterpret_cast<const std::uint8_t*>(bytes.constData()) +
                                toc.blobOffset,
                            static_cast<std::size_t>(toc.blobBytes));
            }
        } else {
            entry.payload.assign(
                reinterpret_cast<const std::uint8_t*>(bytes.constData()) + record.offset,
                reinterpret_cast<const std::uint8_t*>(bytes.constData()) + record.offset +
                    record.size);
        }
        const auto existing = std::find_if(entries->begin(), entries->end(),
            [&](const auto& candidate) {
                return candidate.kind == entry.kind && candidate.id == entry.id
                    && candidate.relationOrdinal == entry.relationOrdinal;
            });
        if (existing != entries->end()) {
            const bool samePayload = entry.kind == harkenstudiobuild::kPackEntryKindMedia
                ? existing->payload.size() == entry.payload.size()
                    && std::equal(entry.payload.begin(), entry.payload.begin() + 32,
                                  existing->payload.begin())
                    && std::equal(entry.payload.begin() + 40, entry.payload.end(),
                                  existing->payload.begin() + 40)
                : existing->payload == entry.payload;
            if (existing->vintage != entry.vintage || !samePayload) {
                if (error)
                    *error = QStringLiteral("Conflicting Pack entry %1 in '%2'")
                                 .arg(VNX::objectIdString(entry.id), path);
                return false;
            }
            continue;
        }
        entries->push_back(std::move(entry));
    }
    return true;
}

QString entriesFingerprint(const std::vector<Bridge::PackBuildSession::CanonicalPackEntry>& entries,
                           std::uint32_t kind)
{
    std::vector<const Bridge::PackBuildSession::CanonicalPackEntry*> sorted;
    for (const auto& entry : entries) sorted.push_back(&entry);
    std::sort(sorted.begin(), sorted.end(), [](const auto* a, const auto* b) {
        if (a->kind != b->kind) return a->kind < b->kind;
        if (a->id != b->id) return a->id < b->id;
        return a->relationOrdinal < b->relationOrdinal;
    });
    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::array<std::uint8_t, 40> header{};
    harkenstudiobuild::StoreLE32(header.data(), kind);
    harkenstudiobuild::StoreLE32(header.data() + 4, harkenstudiobuild::kPackFormatVersion2);
    hash.addData(QByteArrayView(reinterpret_cast<const char*>(header.data()), 8));
    for (const auto* entry : sorted) {
        harkenstudiobuild::StoreLE32(header.data(), entry->kind);
        harkenstudiobuild::StoreLE32(header.data() + 4, entry->vintage);
        harkenstudiobuild::StoreObjectId(header.data() + 8, entry->id);
        harkenstudiobuild::StoreLE64(header.data() + 24, entry->relationOrdinal);
        harkenstudiobuild::StoreLE64(header.data() + 32, entry->payload.size());
        hash.addData(QByteArrayView(reinterpret_cast<const char*>(header.data()), header.size()));
        const auto* bytes = reinterpret_cast<const char*>(entry->payload.data());
        if (entry->kind == harkenstudiobuild::kPackEntryKindMedia && entry->payload.size() >= 40) {
            hash.addData(QByteArrayView(bytes, 32));
            hash.addData(QByteArray(8, '\0'));
            hash.addData(QByteArrayView(bytes + 40, entry->payload.size() - 40));
        } else {
            hash.addData(QByteArrayView(bytes, entry->payload.size()));
        }
    }
    return QString::fromLatin1(hash.result().toHex());
}

void removeMatchingDirs(const QDir& parent, const QString& prefix)
{
    const QFileInfoList entries =
        parent.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const QFileInfo& info : entries) {
        if (info.fileName().startsWith(prefix))
            QDir(info.absoluteFilePath()).removeRecursively();
    }
}

bool safePublicationPath(const QDir& dir, const QString& relative)
{
    if (QFileInfo(dir.absolutePath()).isSymLink() || relative.isEmpty() || QDir::isAbsolutePath(relative)
        || relative.contains(QLatin1Char('\\')) || relative.contains(QLatin1Char(':')))
        return false;
    QString current = dir.absolutePath();
    for (const QString& part : relative.split(QLatin1Char('/'))) {
        if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral("..")
            || (current == dir.absolutePath() && (part == QString::fromLatin1(kPublishingMarker)
                || part == QStringLiteral(".harken-pack-platform.lock")
                || part == QStringLiteral(".harken-pack-build.lock")
                || part == QStringLiteral(".harken-pack-index.lock")))
            || part == QString::fromLatin1(kWaveformLock)
            || part.endsWith(QLatin1Char('.')) || part.endsWith(QLatin1Char(' ')))
            return false;
        current = QDir(current).filePath(part);
        const QFileInfo info(current);
        if (info.isSymLink())
            return false;
        if (info.exists() && !info.canonicalFilePath().startsWith(
                dir.canonicalPath() + QLatin1Char('/'), Qt::CaseInsensitive))
            return false;
    }
    return true;
}

bool containsPublicationFile(const QDir& dir, const QStringList& names, const QString& relative)
{
    if (names.contains(relative)) return true;
    const QFileInfo target(dir.filePath(relative));
    if (!target.exists()) return false;
    return std::any_of(names.cbegin(), names.cend(), [&](const QString& name) {
        return QFileInfo(dir.filePath(name)) == target;
    });
}

bool exportCanceled(const std::function<bool()>& cancelCheck, QString* error)
{
    if (!cancelCheck || !cancelCheck()) return false;
    if (error) *error = QStringLiteral("Export canceled");
    return true;
}

bool isWaveformOutputFile(const QFileInfo& file)
{
    const auto hash = file.completeBaseName();
    return file.suffix() == QStringLiteral("vkd") && hash.size() == 64
        && std::all_of(hash.begin(), hash.end(), [](QChar ch) {
            return (ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))
                || (ch >= QLatin1Char('a') && ch <= QLatin1Char('f'));
        });
}

bool preserveBusyWaveformOutputs(const QDir& root, const QStringList& liveNames,
                                 QStringList* publishedNames, QStringList* preservedNames,
                                 std::vector<std::unique_ptr<QLockFile>>* locks,
                                 QStringList* warnings, QString* error,
                                 const std::function<bool()>& cancelCheck,
                                 const QString& lockedWaveformRoot = {})
{
    QSet<QString> checkedDirectories;
    QSet<QString> busyDirectories;
    for (const auto& name : liveNames) {
        if (exportCanceled(cancelCheck, error)) return false;
        if (publishedNames->contains(name) || !isWaveformOutputFile(QFileInfo(name))) continue;
        const QString directory = QFileInfo(root.filePath(name)).absolutePath();
        if (!lockedWaveformRoot.isEmpty() && QFileInfo(directory) == QFileInfo(lockedWaveformRoot)) continue;
        if (!checkedDirectories.contains(directory)) {
            checkedDirectories.insert(directory);
            auto lock = std::make_unique<QLockFile>(QDir(directory).filePath(QString::fromLatin1(kWaveformLock)));
            lock->setStaleLockTime(0);
            if (lock->tryLock()) {
                locks->push_back(std::move(lock));
            } else {
                busyDirectories.insert(directory);
                warnings->append(QStringLiteral("Waveform cleanup skipped because the output is being used by another build or cannot be locked: %1")
                    .arg(directory));
            }
        }
        if (busyDirectories.contains(directory)) {
            publishedNames->append(name);
            preservedNames->append(name);
        }
    }
    return true;
}

bool scanOutputFiles(const QDir& dir, QStringList* names, QStringList* packs, QString* error,
                     bool includeStagingDirectories = false,
                     const std::function<bool()>& cancelCheck = {})
{
    if (exportCanceled(cancelCheck, error)) return false;
    if (QFileInfo(dir.absolutePath()).isSymLink()) {
        *error = QStringLiteral("Output directory is a symbolic link: %1").arg(dir.absolutePath());
        return false;
    }
    QStringList directories{QString()};
    while (!directories.isEmpty()) {
        const QDir current(dir.filePath(directories.takeLast()));
        for (const auto& entry : current.entryInfoList(
                 QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
            if (exportCanceled(cancelCheck, error)) return false;
            const QString relative = dir.relativeFilePath(entry.absoluteFilePath());
            if (entry.isSymLink()) {
                *error = QStringLiteral("Output contains a symbolic link: %1").arg(entry.absoluteFilePath());
                return false;
            }
            if (!safePublicationPath(dir, relative)) continue;
            if (entry.isDir()) {
                if ((!includeStagingDirectories && entry.fileName().startsWith(QString::fromLatin1(kStagingPrefix)))
                    || entry.fileName().startsWith(QString::fromLatin1(kPreviousPrefix))
                    || entry.fileName().startsWith(QString::fromLatin1(kNextPrefix))) continue;
                directories.append(relative);
                continue;
            }
            names->append(relative);
            if (!packs || isWaveformOutputFile(entry)) continue;
            QFile file(entry.absoluteFilePath());
            if (!file.open(QIODevice::ReadOnly)) {
                *error = QStringLiteral("Cannot inspect export output: %1").arg(entry.absoluteFilePath());
                return false;
            }
            const auto magic = file.read(sizeof(harkenstudiobuild::kPackMagic));
            if (magic.size() == sizeof(harkenstudiobuild::kPackMagic)
                && std::memcmp(magic.constData(), harkenstudiobuild::kPackMagic, magic.size()) == 0)
                packs->append(relative);
        }
    }
    return true;
}

bool scanCleanupFiles(const QDir& root, const QStringList& selectedPlatforms,
                      QStringList* names, QStringList* packs, QString* error,
                      const std::function<bool()>& cancelCheck = {})
{
    if (!scanOutputFiles(root, names, packs, error, false, cancelCheck)) return false;
    for (const auto& directory : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)) {
        if (directory.fileName().startsWith(QString::fromLatin1(kStagingPrefix))
            || directory.fileName().startsWith(QString::fromLatin1(kPreviousPrefix))
            || directory.fileName().startsWith(QString::fromLatin1(kNextPrefix))
            || selectedPlatforms.contains(directory.fileName())) continue;
        QStringList retiredFiles, retiredPacks;
        if (!scanOutputFiles(QDir(directory.absoluteFilePath()), &retiredFiles,
                packs ? &retiredPacks : nullptr, error, true, cancelCheck)) return false;
        for (const auto& name : retiredFiles) names->append(directory.fileName() + QLatin1Char('/') + name);
        if (packs)
            for (const auto& name : retiredPacks) packs->append(directory.fileName() + QLatin1Char('/') + name);
    }
    names->removeDuplicates();
    if (packs) packs->removeDuplicates();
    return true;
}

bool copyRelativeFile(const QDir& fromDir, const QDir& toDir, const QString& relative)
{
    const QString source = fromDir.filePath(relative);
    if (!QFile::exists(source))
        return false;
    const QString destination = toDir.filePath(relative);
    QDir().mkpath(QFileInfo(destination).absolutePath());
    QFile::remove(destination);
    return QFile::copy(source, destination);
}

bool restoreFiles(const QString& fromDir, const QDir& liveDir, const QStringList& names)
{
    for (const QString& name : names) {
        if (!copyRelativeFile(QDir(fromDir), liveDir, name))
            return false;
    }
    return true;
}

void removeRelativeFile(const QDir& dir, const QString& relative)
{
    QFile::remove(dir.filePath(relative));
    QDir parent(QFileInfo(dir.filePath(relative)).absolutePath());
    while (parent.absolutePath() != dir.absolutePath()
           && parent.absolutePath().startsWith(dir.absolutePath())
           && parent.isEmpty()) {
        const QString current = parent.absolutePath();
        if (!parent.cdUp())
            break;
        QDir().rmdir(current);
    }
}

void removeEmptyOutputDirs(const QDir& dir)
{
    for (const auto& entry : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks)) {
        const QDir child(entry.absoluteFilePath());
        removeEmptyOutputDirs(child);
        if (child.isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System))
            QDir().rmdir(child.absolutePath());
    }
}

void removePublicationCopy(const QDir& dir, const QStringList& names)
{
    QStringList companions;
    bool removed = true;
    for (const QString& name : names) {
        if (QFileInfo(name).suffix() == QStringLiteral("json")) {
            companions.prepend(name);
            continue;
        }
        removeRelativeFile(dir, name);
        removed = removed && !QFileInfo::exists(dir.filePath(name));
    }
    if (!removed) return;
    for (const QString& name : companions) {
        removeRelativeFile(dir, name);
        if (QFileInfo::exists(dir.filePath(name))) return;
    }
    QDir().rmdir(dir.absolutePath());
}

void cleanupPublicationCopies(const QDir& liveDir)
{
    for (const QFileInfo& info : liveDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)) {
        if (info.isSymLink() || (!info.fileName().startsWith(QString::fromLatin1(kPreviousPrefix))
            && !info.fileName().startsWith(QString::fromLatin1(kNextPrefix)))) continue;
        QStringList names;
        QString error;
        const QDir copy(info.absoluteFilePath());
        if (scanOutputFiles(copy, &names, nullptr, &error)) {
            removePublicationCopy(copy, names);
        }
    }
}

bool recoverInterruptedPublish(const QDir& liveDir, QString* error)
{
    const QString marker = liveDir.filePath(QString::fromLatin1(kPublishingMarker));
    if (!QFile::exists(marker)) {
        cleanupPublicationCopies(liveDir);
        return true;
    }
    const auto reject = [&](const QString& detail) {
        *error = QStringLiteral("Cannot recover interrupted pack publish: %1").arg(detail);
        return false;
    };
    QFile markerFile(marker);
    if (!markerFile.open(QIODevice::ReadOnly)) return reject(QStringLiteral("marker is unreadable"));
    const QByteArray markerBytes = markerFile.readAll();
    markerFile.close();
    QStringList previousNames, nextNames;
    const QJsonObject transaction = QJsonDocument::fromJson(markerBytes).object();
    const QString previousName = transaction.value(QStringLiteral("previous")).toString();
    const QString nextName = transaction.value(QStringLiteral("next")).toString();
    if (transaction.value(QStringLiteral("version")).toInt() != 1
        || !previousName.startsWith(QString::fromLatin1(kPreviousPrefix))
        || previousName.contains(QLatin1Char('/')) || previousName.contains(QLatin1Char('\\')) || previousName.contains(QLatin1Char(':'))
        || !nextName.startsWith(QString::fromLatin1(kNextPrefix))
        || nextName.contains(QLatin1Char('/')) || nextName.contains(QLatin1Char('\\')) || nextName.contains(QLatin1Char(':')))
        return reject(QStringLiteral("invalid transaction record"));
    const QString previous = liveDir.filePath(previousName);
    const QString next = liveDir.filePath(nextName);
    const auto readNames = [&](const QString& key, QStringList* names) {
        if (!transaction.value(key).isArray()) return false;
        for (const QJsonValue& value : transaction.value(key).toArray()) {
            const QString name = value.toString();
            if (!safePublicationPath(liveDir, name)
                || !safePublicationPath(QDir(previous), name)
                || !safePublicationPath(QDir(next), name)) return false;
            names->append(name);
        }
        names->removeDuplicates();
        return true;
    };
    if (!readNames(QStringLiteral("previousFiles"), &previousNames)
        || !readNames(QStringLiteral("nextFiles"), &nextNames))
        return reject(QStringLiteral("unsafe transaction inventory"));
    if (!QDir(previous).exists()) return reject(QStringLiteral("previous pack set is missing"));
    for (const QString& name : previousNames) {
        if (!QFileInfo(QDir(previous).filePath(name)).isFile())
            return reject(QStringLiteral("previous file is missing: %1").arg(name));
    }
    for (const QString& name : nextNames) {
        if (containsPublicationFile(liveDir, previousNames, name)) continue;
        removeRelativeFile(liveDir, name);
        if (QFileInfo::exists(liveDir.filePath(name)))
            return reject(QStringLiteral("cannot remove incomplete file: %1").arg(name));
    }
    if (!restoreFiles(previous, liveDir, previousNames) || !QFile::remove(marker))
        return reject(QStringLiteral("previous pack set could not be restored"));
    removePublicationCopy(QDir(previous), previousNames);
    if (!next.isEmpty()) removePublicationCopy(QDir(next), nextNames);
    return true;
}

bool writeJson(const QString& path, const QJsonObject& object, QString* error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("Failed to open '%1'").arg(path);
        return false;
    }
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (error)
            *error = QStringLiteral("Failed to write '%1'").arg(path);
        return false;
    }
    return true;
}

bool validateWriteAccess(QStringList paths, QString* error,
                         const std::function<bool()>& cancelCheck = {}, QString* summary = nullptr)
{
    paths.removeDuplicates();
    paths.sort();
    QStringList blocked;
    QSet<QString> checkedDirectories;
    for (const QString& path : paths) {
        if (exportCanceled(cancelCheck, error)) return false;
        const QFileInfo entry(path);
        if (entry.isFile()) {
            bool accessible = entry.isWritable();
#ifdef Q_OS_WIN
            const QString nativePath = QDir::toNativeSeparators(entry.absoluteFilePath());
            const HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(nativePath.utf16()),
                GENERIC_READ | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            accessible = accessible && handle != INVALID_HANDLE_VALUE;
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
#else
            QFile file(entry.absoluteFilePath());
            accessible = accessible && file.open(QIODevice::ReadOnly);
            struct stat parentStat{};
            if (::stat(QFile::encodeName(entry.absolutePath()).constData(), &parentStat) != 0
                || ((parentStat.st_mode & S_ISVTX) && ::geteuid() != 0
                    && ::geteuid() != parentStat.st_uid && ::geteuid() != entry.ownerId()))
                accessible = false;
#endif
            if (!accessible)
                blocked.append(QDir::toNativeSeparators(entry.absoluteFilePath()));
        }
        QFileInfo parent(entry.isDir() ? entry.absoluteFilePath() : entry.absolutePath());
        while (!parent.exists() && parent.absoluteFilePath() != parent.absolutePath())
            parent.setFile(parent.absolutePath());
        if (checkedDirectories.contains(parent.absoluteFilePath())) continue;
        checkedDirectories.insert(parent.absoluteFilePath());
        bool accessible = parent.isDir() && parent.isWritable();
#ifdef Q_OS_WIN
        const QString nativePath = QDir::toNativeSeparators(parent.absoluteFilePath());
        const HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(nativePath.utf16()),
            FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        accessible = accessible && handle != INVALID_HANDLE_VALUE;
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
#else
        accessible = accessible && ::access(QFile::encodeName(parent.absoluteFilePath()).constData(), W_OK | X_OK) == 0;
#endif
        if (!accessible)
            blocked.append(QDir::toNativeSeparators(parent.absoluteFilePath()));
    }
    blocked.removeDuplicates();
    if (blocked.isEmpty()) return true;
    const QString message = ProjectPackExporter::tr(
        "Some paths needed for Pack export are read-only, locked, or cannot be replaced.\n"
        "Check file write protection, access permissions, and other programs using these files. If your version control workflow "
        "requires checkout or unlocking, do that first, then retry.");
    if (summary) *summary = message;
    *error = message + ProjectPackExporter::tr("\n\nAffected paths:\n%1")
        .arg(blocked.join(QLatin1Char('\n')));
    return false;
}

bool commitPublication(const QDir& liveDir, const QString& nextDir,
                       const QStringList& liveNames, const QStringList& publishedNames,
                       const QStringList& preservedNames, QString* error,
                       const std::function<bool()>& cancelCheck = {})
{
    const auto refuse = [&](const QString& message) {
        *error = message;
        return false;
    };
    for (const QString& name : publishedNames) {
        if (exportCanceled(cancelCheck, error)) return false;
        if (!safePublicationPath(liveDir, name)
            || (QFileInfo::exists(liveDir.filePath(name)) && !containsPublicationFile(liveDir, liveNames, name)))
            return refuse(QStringLiteral("Publication path conflicts with an unowned file: %1").arg(name));
    }
    QStringList previousNames;
    for (const auto& name : liveNames)
        if (!preservedNames.contains(name)) previousNames.append(name);
    QStringList nextNames;
    for (const auto& name : publishedNames)
        if (!preservedNames.contains(name)) nextNames.append(name);
    if (previousNames.isEmpty() && nextNames.isEmpty()) return true;
    QStringList writePaths;
    for (const auto& name : previousNames + nextNames)
        writePaths.append(liveDir.filePath(name));
    if (!validateWriteAccess(writePaths, error, cancelCheck)) return false;
    QTemporaryDir previous(liveDir.filePath(QString::fromLatin1(kPreviousPrefix) + QStringLiteral("XXXXXX")));
    const QString previousDir = previous.path();
    if (!previous.isValid())
        return refuse(QStringLiteral("Cannot retain previous pack set"));
    for (const QString& name : previousNames) {
        if (exportCanceled(cancelCheck, error)) return false;
        if (!copyRelativeFile(liveDir, QDir(previousDir), name))
            return refuse(QStringLiteral("Cannot retain previous '%1'").arg(name));
    }
    if (exportCanceled(cancelCheck, error)) return false;

    const QString marker = liveDir.filePath(QString::fromLatin1(kPublishingMarker));
    QJsonObject transaction;
    transaction.insert(QStringLiteral("version"), 1);
    transaction.insert(QStringLiteral("previous"), QFileInfo(previousDir).fileName());
    transaction.insert(QStringLiteral("next"), QFileInfo(nextDir).fileName());
    transaction.insert(QStringLiteral("previousFiles"), QJsonArray::fromStringList(previousNames));
    transaction.insert(QStringLiteral("nextFiles"), QJsonArray::fromStringList(nextNames));
    QString transactionError;
    if (!writeJson(marker, transaction, &transactionError))
        return refuse(QStringLiteral("Cannot start live-set publish: %1").arg(transactionError));
    previous.setAutoRemove(false);

    auto restorePrevious = [&]() -> bool {
        bool orphansCleared = true;
        for (const QString& published : nextNames) {
            if (!containsPublicationFile(liveDir, previousNames, published)) {
                removeRelativeFile(liveDir, published);
                if (QFileInfo(liveDir.filePath(published)).isFile())
                    orphansCleared = false;
            }
        }
        return restoreFiles(previousDir, liveDir, previousNames) && orphansCleared;
    };

    for (const QString& name : previousNames) {
        removeRelativeFile(liveDir, name);
        if (QFile::exists(liveDir.filePath(name))) {
            restorePrevious();
            return refuse(QStringLiteral("Cannot retire previous '%1'").arg(name));
        }
    }
    for (const QString& name : nextNames) {
        if (!copyRelativeFile(QDir(nextDir), liveDir, name)) {
            restorePrevious();
            return refuse(QStringLiteral("Cannot publish '%1'").arg(name));
        }
    }

    if (!QFile::remove(marker)) {
        restorePrevious();
        return refuse(QStringLiteral("Cannot commit live-set publish"));
    }
    previous.setAutoRemove(true);
    return true;
}

struct PackMeta
{
    QString name;
    QString unitKey;
    QString fileName;
    QString language;
    QString platform;
    std::vector<harkenstudio::ObjectId> actionStableIds;
    bool grouped = false;
    std::uint64_t packStableId = 0;
    std::uint32_t packKind = 0;
    std::uint64_t contentRevision = 1;
    std::uint64_t requiredInitRevision = 0;
    std::vector<std::uint64_t> dependencies;
    std::uint32_t ownedResources = 0;
    std::uint32_t ownedDefinitions = 0;
    std::uint64_t semanticFingerprint = 0;
    std::uint64_t lineageFingerprint = 0;
    std::vector<std::uint8_t> partitionPayload;
    std::vector<std::uint8_t> variablePolicyOverlayPayload;
    QJsonObject companion;
    std::vector<std::uint8_t> variableRequirementsPayload;
    std::unordered_map<harkenstudio::ObjectId, std::uint64_t> variableExecutionFingerprints;
    std::unordered_map<harkenstudio::ObjectId, std::uint64_t> variableLabelMappingFingerprints;
    std::vector<Bridge::PackBuildSession::CanonicalPackEntry> resourceEntries;
    QJsonObject previousRow;
    std::vector<Bridge::PackBuildSession::CanonicalPackEntry> previousEntries;
    QString resourceFingerprint;
    QString previousResourceFingerprint;
    QString resourceCacheKey;
    bool previousValid = false;
    harkenstudiobuild::WorldCapacitySummaryV1 previousCapacity;
    bool retained = false;
    bool metadataUpdated = false;
    bool skipped = false;
    qint64 elapsedNanoseconds = 0;
    quint32 formatVersion = 0;
    qint64 fileSize = 0;
    std::unordered_map<harkenstudio::ObjectId, std::uint64_t> soundContentRevisions;
    ProjectPackExporter::ExportResult mediaResult;
    std::set<harkenstudio::ObjectId> ownedPlayableIds;
    std::vector<std::vector<harkenstudiobuild::SendV1>> ownedSends;
    std::vector<std::vector<harkenstudiobuild::LayerAncestorRefV1>> ownedLayerAncestors;
    std::vector<std::vector<harkenstudiobuild::StructuralAncestorRefV1>> ownedStructuralAncestors;
    std::vector<std::vector<harkenstudiobuild::ScheduleClipPlacementV1>> ownedPlacements;
    std::vector<std::vector<harkenstudiobuild::ScheduleTrackRefV1>> ownedTrackRefs;
    std::vector<std::vector<harkenstudiobuild::ContainerChildProgramViewV2>> ownedContainerChildren;
    std::vector<std::vector<harkenstudiobuild::ActionStepProgramViewV1>> ownedActionSteps;
    std::vector<harkenstudiobuild::PlayablePropertyProgramViewV1> slicedPlayables;
    std::vector<harkenstudiobuild::PlayableOccurrencePropertyViewV1> slicedOccurrences;
    std::vector<harkenstudiobuild::TrackScheduleProgramViewV1> slicedTracks;
    std::vector<harkenstudiobuild::SegmentScheduleProgramViewV1> slicedSegments;
    std::vector<harkenstudiobuild::ContainerProgramViewV3> slicedContainers;
    std::vector<harkenstudiobuild::TransitionProgramViewV1> slicedTransitions;
    std::vector<harkenstudiobuild::ActionProgramViewV1> slicedActions;
    std::vector<harkenstudiobuild::VariableDefinitionV1> slicedVariables;
};

struct IdentityOwner
{
    std::set<std::uint64_t> packStableIds;
    std::uint32_t identityClass = 0;
};

bool isPlayableType(ProjectItem::Type type)
{
    return type == ProjectItem::Type::Sound || type == ProjectItem::Type::Container
        || type == ProjectItem::Type::Track || type == ProjectItem::Type::Segment
        || type == ProjectItem::Type::Layer;
}

const ProjectItem* findAction(const ProjectData& project, harkenstudio::ObjectId stableId)
{
    const auto units = ProjectPackExporter::CollectActionPackUnits(project);
    uint32_t objectId = 0;
    for (const auto& unit : units) {
        if (unit.stableId == stableId)
            objectId = unit.objectID;
    }
    if (objectId == 0)
        return nullptr;
    const ProjectItem* found = nullptr;
    for (const auto* binding : project.allFileBindings()) {
        if (!binding || !binding->rootItem)
            continue;
        std::function<void(const ProjectItem*)> walk = [&](const ProjectItem* item) {
            if (!item || found)
                return;
            if (item->objectID == objectId)
                found = item;
            for (const auto& child : item->children)
                walk(child.get());
        };
        walk(binding->rootItem.get());
    }
    return found;
}

const ProjectItem* findItemByGuid(const ProjectData& project, const QUuid& guid)
{
    const ProjectItem* found = nullptr;
    std::function<void(const ProjectItem*)> walk = [&](const ProjectItem* item) {
        if (!item || found)
            return;
        if (item->guid == guid)
            found = item;
        for (const auto& child : item->children)
            walk(child.get());
    };
    if (project.root)
        walk(project.root.get());
    for (const auto* binding : project.allFileBindings()) {
        if (binding && binding->rootItem)
            walk(binding->rootItem.get());
    }
    return found;
}

const ProjectItem* findItemByStableId(const ProjectData& project, harkenstudio::ObjectId stableId)
{
    const ProjectItem* found = nullptr;
    std::function<void(const ProjectItem*)> walk = [&](const ProjectItem* item) {
        if (!item || found)
            return;
        const harkenstudio::ObjectId itemId =
            item->runtimeId();
        if (itemId == stableId)
            found = item;
        for (const auto& child : item->children)
            walk(child.get());
    };
    if (project.root)
        walk(project.root.get());
    for (const auto* binding : project.allFileBindings()) {
        if (binding && binding->rootItem)
            walk(binding->rootItem.get());
    }
    return found;
}

harkenstudio::ObjectId itemStableId(const ProjectItem& item)
{
    return item.runtimeId();
}

std::vector<std::int32_t> discreteAnchorsFor(const ProjectItem* item)
{
    std::vector<std::int32_t> anchors;
    const auto* source = item ? item->comp<VVariable>() : nullptr;
    if (!source || source->kind != VariableKind::Step)
        return anchors;
    anchors.reserve(source->labels.size());
    for (const auto& label : VariableSemantics::effectiveLabels(*source))
        anchors.push_back(label.anchor);
    std::sort(anchors.begin(), anchors.end());
    anchors.erase(std::unique(anchors.begin(), anchors.end()), anchors.end());
    return anchors;
}

bool exactIntegerDeclaration(float value)
{
    return std::isfinite(value) && value == std::floor(value) &&
           value >= -16777216.0f && value <= 16777216.0f;
}

bool validateVariableDeclarationForExport(const ProjectItem* item,
                                          QString* error)
{
    const auto* source = item ? item->comp<VVariable>() : nullptr;
    if (!source) {
        if (error)
            *error = QStringLiteral("Variable overlay is missing writers");
        return false;
    }
    const harkenstudio::ObjectId id = itemStableId(*item);
    QString labelsError;
    if (source->rangeMin < 0.0f || !VariableSemantics::validateLabels(source->labels, &labelsError)) {
        if (error) *error = QStringLiteral("Variable '%1': %2").arg(item->name,
            labelsError.isEmpty() ? QStringLiteral("rangeMin must be non-negative") : labelsError);
        return false;
    }
    if (source->writers < 1 || source->writers > 3) {
        if (error)
            *error = QStringLiteral("Variable %1 writers must be 1, 2, or 3")
                         .arg(VNX::objectIdString(id));
        return false;
    }
    if (source->kind == VariableKind::Step &&
        (!exactIntegerDeclaration(source->rangeMin) ||
         !exactIntegerDeclaration(source->rangeMax) ||
         !exactIntegerDeclaration(source->defaultValue))) {
        if (error)
            *error = QStringLiteral(
                         "Variable %1 Discrete range and default must be "
                         "exact integers")
                         .arg(VNX::objectIdString(id));
        return false;
    }
    if (!std::isfinite(source->rangeMin) || !std::isfinite(source->rangeMax) ||
        !std::isfinite(source->defaultValue) ||
        source->defaultValue < source->rangeMin ||
        source->defaultValue > source->rangeMax) {
        if (error)
            *error = QStringLiteral(
                         "Variable %1 defaultValue is outside [rangeMin, "
                         "rangeMax]")
                         .arg(VNX::objectIdString(id));
        return false;
    }
    return true;
}

bool overlayWritersFor(const ProjectItem* item, std::uint8_t* writers,
                       QString* error)
{
    if (!validateVariableDeclarationForExport(item, error))
        return false;
    if (writers == nullptr) {
        if (error)
            *error = QStringLiteral("Variable overlay is missing writers");
        return false;
    }
    *writers = item->comp<VVariable>()->writers;
    return true;
}

void stripBindingEntries(
    std::vector<Bridge::PackBuildSession::CanonicalPackEntry>* entries)
{
    entries->erase(std::remove_if(entries->begin(), entries->end(),
        [](const Bridge::PackBuildSession::CanonicalPackEntry& entry) {
            return entry.kind == harkenstudiobuild::kPackEntryKindBindingDefinition
                || entry.kind == harkenstudiobuild::kPackEntryKindBindingAttachment;
        }), entries->end());
}

bool encodeBindingDefinitionEntry(
    const BindingRuntimeProgram& program,
    Bridge::PackBuildSession::CanonicalPackEntry* out)
{
    if (!out || harkenstudio::IsNull(program.bindingId) || harkenstudio::IsNull(program.variableId)
        || program.points.size() < 2
        || program.points.size() > harkenstudio::kCurveMaxPoints)
        return false;
    harkenstudiobuild::BindingDefinition desc{};
    desc.variableId = program.variableId;
    desc.targetScopeKind = program.targetScopeKind;
    desc.targetId = program.targetId;
    desc.transitionMs = program.transitionMs;
    desc.curve.pointCount = static_cast<std::uint32_t>(program.points.size());
    for (int i = 0; i < program.points.size(); ++i) {
        desc.curve.points[i].x = program.points[i].x;
        desc.curve.points[i].y = program.points[i].value;
    }
    const int handleCount = std::min(static_cast<int>(program.handles.size()),
                                     static_cast<int>(program.points.size()) - 1);
    for (int i = 0; i < handleCount; ++i) {
        desc.curve.handles[i].h1x = program.handles[i].h1x;
        desc.curve.handles[i].h1y = program.handles[i].h1y;
        desc.curve.handles[i].h2x = program.handles[i].h2x;
        desc.curve.handles[i].h2y = program.handles[i].h2y;
    }
    out->kind = harkenstudiobuild::kPackEntryKindBindingDefinition;
    out->vintage = harkenstudiobuild::kPackV2SchemaVintage;
    out->id = program.bindingId;
    out->payload.assign(harkenstudiobuild::kPackV2BindingDefinitionPayloadBytes, 0);
    harkenstudiobuild::SerializePackV2BindingDefinition(program.bindingId, desc,
                                            out->payload.data());
    return true;
}

bool appendCanonicalBindingEntries(
    const BindingRuntimeLowering& lowering,
    const PackMeta& meta,
    std::vector<Bridge::PackBuildSession::CanonicalPackEntry>* entries,
    QString* error)
{
    if (!entries)
        return false;
    stripBindingEntries(entries);
    const bool initPack = meta.packKind == harkenstudiobuild::kPackKindInit;
    std::set<harkenstudio::ObjectId> localObjectIds;
    for (const auto& entry : *entries) {
        if (entry.kind == harkenstudiobuild::kPackEntryKindMedia
            || entry.kind == harkenstudiobuild::kPackEntryKindBus)
            localObjectIds.insert(entry.id);
    }
    localObjectIds.insert(meta.ownedPlayableIds.begin(),
                          meta.ownedPlayableIds.end());

    std::vector<BindingRuntimeAttachment> kept;
    std::set<harkenstudio::ObjectId> keptBindingIds;
    for (const BindingRuntimeAttachment& attachment : lowering.attachments) {
        const bool global =
            attachment.scopeKind == harkenstudio::kTargetScopeBus
            || attachment.scopeKind == harkenstudio::kTargetScopeBusEffectParam;
        if (global != initPack)
            continue;
        if (localObjectIds.count(attachment.objectId) == 0)
            continue;
        kept.push_back(attachment);
        keptBindingIds.insert(attachment.bindingId);
    }
    for (const BindingRuntimeProgram& program : lowering.programs) {
        if (keptBindingIds.count(program.bindingId) == 0)
            continue;
        Bridge::PackBuildSession::CanonicalPackEntry entry;
        if (!encodeBindingDefinitionEntry(program, &entry)) {
            if (error)
                *error = QStringLiteral("Pack '%1' cannot encode Binding %2")
                             .arg(meta.fileName)
                             .arg(VNX::objectIdString(program.bindingId));
            return false;
        }
        entries->push_back(std::move(entry));
    }
    std::uint64_t ordinal = 0;
    for (const BindingRuntimeAttachment& attachment : kept) {
        Bridge::PackBuildSession::CanonicalPackEntry entry;
        entry.kind = harkenstudiobuild::kPackEntryKindBindingAttachment;
        entry.vintage = harkenstudiobuild::kPackV2SchemaVintage;
        entry.relationOrdinal = ordinal++;
        harkenstudiobuild::PackBindingAttachmentEntry wire;
        wire.bindingId = attachment.bindingId;
        wire.scopeKind = attachment.scopeKind;
        wire.objectId = attachment.objectId;
        entry.payload.assign(harkenstudiobuild::kPackV2BindingAttachmentPayloadBytes, 0);
        harkenstudiobuild::SerializePackV2BindingAttachment(wire, entry.payload.data());
        entries->push_back(std::move(entry));
    }
    return true;
}

bool encodeVariablePolicyOverlay(
    const harkenstudiobuild::ContainerWorldInstallViewV1& world,
    const ProjectData& project,
    std::vector<std::uint8_t>* outPayload,
    std::unordered_map<harkenstudio::ObjectId, std::uint64_t>* fingerprints,
    std::unordered_map<harkenstudio::ObjectId, std::uint64_t>* labelFingerprints,
    QString* error)
{
    struct Row {
        harkenstudio::ObjectId variableId = {};
        std::uint8_t writers = 1;
        std::uint64_t fingerprint = 0;
        std::vector<std::int32_t> anchors;
        std::vector<harkenstudiobuild::VariableLabelDefinitionV1> labels;
        std::uint64_t labelMappingFingerprint = 0;
    };
    std::vector<Row> rows;
    rows.reserve(world.variableCount);
    for (std::uint32_t i = 0; i < world.variableCount; ++i) {
        const auto& def = world.variables[i];
        const ProjectItem* item = findItemByStableId(project, def.variableId);
        if (!item || !NameValidator::validateItemName(item->name, ProjectItem::Type::Variable)) {
            if (error) *error = QStringLiteral("Variable has an invalid SDK name");
            return false;
        }
        std::uint8_t writers = 1;
        if (!overlayWritersFor(item, &writers, error))
            return false;
        auto anchors = discreteAnchorsFor(item);
        const std::int32_t* pointer =
            anchors.empty() ? nullptr : anchors.data();
        std::uint64_t fingerprint = 0;
        if (!harkenstudiobuild::ComputeVariableExecutionFingerprintV1(
                def.variableId, static_cast<std::uint8_t>(def.kind),
                1, writers, def.rangeMin,
                def.rangeMax, def.defaultValue, pointer,
                static_cast<std::uint32_t>(anchors.size()), &fingerprint)) {
            if (error)
                *error = QStringLiteral(
                    "Variable %1 failed execution fingerprint")
                             .arg(VNX::objectIdString(def.variableId));
            return false;
        }
        (*fingerprints)[def.variableId] = fingerprint;
        std::vector<harkenstudiobuild::VariableLabelDefinitionV1> labels;
        for (const auto& label : VariableSemantics::effectiveLabels(*item->comp<VVariable>()))
            labels.push_back({label.anchor, {}, runtimeObjectId(label.id)});
        const auto mapping = harkenstudiobuild::ComputeVariableLabelMappingFingerprintV1(def.variableId, labels);
        (*labelFingerprints)[def.variableId] = mapping;
        rows.push_back({def.variableId, writers, fingerprint, std::move(anchors), std::move(labels), mapping});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        return a.variableId < b.variableId;
    });
    std::size_t bytes = 16 + rows.size() * 32;
    for (const auto& row : rows) {
        bytes += 4 + row.anchors.size() * 4;
        bytes += 16 + row.labels.size() * 24;
    }
    std::vector<std::uint8_t> payload(bytes, 0);
    harkenstudiobuild::StoreLE32(payload.data(), static_cast<std::uint32_t>(rows.size()));
    harkenstudiobuild::StoreLE32(payload.data() + 4, harkenstudiobuild::kPackVariablePolicyOverlayFormat);
    std::size_t offset = 16;
    for (const auto& row : rows) {
        harkenstudiobuild::StoreObjectId(payload.data() + offset, row.variableId);
        payload[offset + 16] = row.writers;
        harkenstudiobuild::StoreLE64(payload.data() + offset + 24, row.fingerprint);
        offset += 32;
    }
    for (const auto& row : rows) {
        harkenstudiobuild::StoreLE32(payload.data() + offset,
                         static_cast<std::uint32_t>(row.anchors.size()));
        offset += 4;
        for (std::int32_t anchor : row.anchors) {
            harkenstudiobuild::StoreLE32(payload.data() + offset,
                             static_cast<std::uint32_t>(anchor));
            offset += 4;
        }
    }
    for (const auto& row : rows) {
        harkenstudiobuild::StoreLE64(payload.data() + offset, row.labelMappingFingerprint);
        harkenstudiobuild::StoreLE32(payload.data() + offset + 8, static_cast<std::uint32_t>(row.labels.size()));
        offset += 16;
        for (const auto& label : row.labels) {
            harkenstudiobuild::StoreObjectId(payload.data() + offset, label.labelId);
            harkenstudiobuild::StoreLE32(payload.data() + offset + 16, static_cast<std::uint32_t>(label.anchor));
            offset += 24;
        }
    }
    *outPayload = std::move(payload);
    return true;
}

bool fillInitCompanion(const harkenstudiobuild::ContainerWorldInstallViewV1& world,
                       const ProjectData& project, QJsonObject* companion, QString* error)
{
    QJsonArray variables;
    QSet<QString> names;
    for (std::uint32_t i = 0; i < world.variableCount; ++i) {
        const auto* item = findItemByStableId(project, world.variables[i].variableId);
        if (!item || item->type != ProjectItem::Type::Variable
            || !NameValidator::validateItemName(item->name, item->type)
            || names.contains(item->name)) {
            if (error) *error = QStringLiteral("Invalid Variable publication identity or name");
            return false;
        }
        names.insert(item->name);
        QJsonArray labels;
        for (const auto& label : VariableSemantics::effectiveLabels(*item->comp<VVariable>())) {
            if (label.name.isEmpty()) continue;
            labels.append(QJsonObject{{QStringLiteral("name"), label.name},
                {QStringLiteral("guid"), objectIdString(runtimeObjectId(label.id))}});
        }
        variables.append(QJsonObject{{QStringLiteral("name"), item->name},
            {QStringLiteral("guid"), objectIdString(item->runtimeId())},
            {QStringLiteral("labels"), labels}});
    }
    *companion = QJsonObject{{QStringLiteral("schemaVersion"), HARKEN_PACK_SCHEMA_VERSION},
        {QStringLiteral("variables"), variables}};
    return true;
}

QStringList companionWaveformFiles(const QJsonObject& companion,
                                    const QString& companionPath, const QDir& root)
{
    QStringList files;
    const QDir companionDir(QFileInfo(companionPath).absolutePath());
    for (const auto& value : companion.value(QStringLiteral("waveforms")).toArray()) {
        const QString absolute = QDir::cleanPath(companionDir.absoluteFilePath(
            value.toObject().value(QStringLiteral("file")).toString()));
        const QString relative = root.relativeFilePath(absolute);
        if (safePublicationPath(root, relative) && QFileInfo(absolute).isFile())
            files.append(relative);
    }
    files.removeDuplicates();
    return files;
}

std::set<harkenstudio::ObjectId> packMetadataObjectIds(
    const harkenstudiobuild::PackCanonicalImage& image,
    const harkenstudiobuild::ContainerWorldInstallViewV1& world)
{
    std::set<harkenstudio::ObjectId> ids;
    const auto collectPayload = [&](const harkenstudiobuild::PlayablePropertyPayloadViewV1& payload) {
        constexpr auto slot = static_cast<std::size_t>(
            harkenstudiobuild::playableproperties::PlayablePropertyIdV1::SpatialAttenuationRef) - 1;
        if (payload.presentMask & (UINT64_C(1) << slot))
            ids.insert(payload.values[slot].objectId);
    };
    for (std::uint32_t i = 0; i < world.variableCount; ++i)
        ids.insert(world.variables[i].variableId);
    for (std::uint32_t i = 0; i < world.actionCount; ++i) {
        const auto& action = world.actions[i];
        ids.insert(action.actionStableId);
        for (std::uint32_t j = 0; j < action.stepCount; ++j) {
            ids.insert(action.steps[j].target.stableId);
            ids.insert(action.steps[j].quantizeReference.stableId);
        }
    }
    for (std::uint32_t i = 0; i < world.playablePropertyCount; ++i) {
        const auto& playable = world.playableProperties[i];
        ids.insert(playable.stableId);
        collectPayload(playable.payload);
        for (std::uint32_t j = 0; j < playable.organizationLayerAncestorCount; ++j)
            ids.insert(playable.organizationLayerAncestors[j].layerStableId);
        for (std::uint32_t j = 0; j < playable.structuralAncestorCount; ++j) {
            ids.insert(playable.structuralAncestors[j].stableId);
            collectPayload(playable.structuralAncestors[j].payload);
        }
    }
    for (std::uint32_t i = 0; i < world.playableOccurrenceCount; ++i) {
        const auto& occurrence = world.playableOccurrences[i];
        ids.insert(occurrence.parentStableId);
        ids.insert(occurrence.childPlayable.stableId);
        collectPayload(occurrence.payload);
    }
    for (std::uint32_t i = 0; i < world.containerCount; ++i) {
        const auto& container = world.containers[i];
        for (std::uint32_t j = 0; j < container.discreteAxisCount; ++j)
            ids.insert(container.discreteAxes[j].variableId);
        for (std::uint32_t j = 0; j < container.continuousAxisCount; ++j)
            ids.insert(container.continuousAxes[j].variableId);
    }
    for (const auto& media : image.media)
        ids.insert(media.soundId);
    for (const auto& attenuation : image.attenuations)
        ids.insert(attenuation.id);
    for (const auto& binding : image.bindingDefinitions)
        ids.insert(binding.desc.variableId);
    for (const auto& requirement : image.variableRequirements)
        ids.insert(requirement.stableId);
    if (image.hasBusDefinitionSnapshot) {
        for (const auto& bus : image.busDefinitionBusesV2)
            ids.insert(bus.busId);
    }
    ids.erase(harkenstudio::ObjectId{});
    ids.erase(harkenstudio::kMasterBusObjectId);
    return ids;
}

bool fillPackMetadata(const ProjectData& project, const PackMeta& pack,
                      const harkenstudiobuild::PackCanonicalImage& image,
                      const std::unordered_map<harkenstudio::ObjectId, const ProjectItem*>& items,
                      QJsonObject* companion, QString* error)
{
    harkenstudiobuild::CurrentWorldBodyImage body;
    if (!body.Decode(image.partitionBody.data(), image.partitionBody.size(),
            harkenstudiobuild::kCanonicalDefinitionPartitionBodyVintage)) {
        *error = QStringLiteral("Cannot read Pack metadata from '%1'").arg(pack.fileName);
        return false;
    }
    const auto* platform = ProjectPlatformSemantics::find(project.platforms, QUuid(pack.platform));
    if (!platform) {
        *error = QStringLiteral("Pack metadata platform is unavailable: %1").arg(pack.platform);
        return false;
    }
    QJsonArray objects;
    for (const auto id : packMetadataObjectIds(image, *body.ProductionView())) {
        const auto found = items.find(id);
        if (found == items.end() || !ItemTypeCatalog::factsOrNull(found->second->type)) {
            *error = QStringLiteral("Pack '%1' metadata object is unavailable: %2")
                .arg(pack.fileName, objectIdString(id));
            return false;
        }
        const auto& item = *found->second;
        objects.append(QJsonObject{
            {QStringLiteral("guid"), objectIdString(id)},
            {QStringLiteral("name"), item.name},
            {QStringLiteral("type"), ItemTypeCatalog::canonicalName(item.type)},
            {QStringLiteral("path"), item.objectPath}});
    }
    companion->insert(QStringLiteral("metadata"), QJsonObject{
        {QStringLiteral("packStableId"), QString::number(image.packStableId)},
        {QStringLiteral("platform"), platform->name},
        {QStringLiteral("language"), pack.language},
        {QStringLiteral("objects"), objects}});
    return true;
}

bool fillWaveformCompanion(const CanonicalPackSetExportRequest& request,
                           const PackMeta& pack, const QString& outputRoot,
                           const QString& stagingRoot,
                           QHash<uint32_t, QString>* exportedSources,
                           QJsonObject* companion, QString* error)
{
    QJsonArray waveforms;
    std::set<harkenstudio::ObjectId> mediaIds;
    for (const auto& entry : pack.resourceEntries)
        if (entry.kind == harkenstudiobuild::kPackEntryKindMedia)
            mediaIds.insert(entry.id);
    const auto& snapshot = *request.options.snapshot;
    const auto& project = *request.project;
    const QDir waveformDir(outputRoot);
    const QDir companionDir(QFileInfo(QDir(request.outputDir).filePath(pack.fileName)).absolutePath());
    for (const auto& variant : request.plan->variants) {
        if (variant.variant.platform != pack.platform || variant.variant.language != pack.language
            || std::find(pack.actionStableIds.begin(), pack.actionStableIds.end(), variant.actionStableId)
                == pack.actionStableIds.end()) continue;
        for (const auto& media : variant.closure.media) {
            if (!mediaIds.erase(media.soundStableId)) continue;
            if (request.options.cancelCheck && request.options.cancelCheck()) {
                *error = QStringLiteral("Export canceled");
                return false;
            }
            const auto* item = findItemByStableId(project, media.soundStableId);
            const auto* sound = item ? item->comp<VSound>() : nullptr;
            const auto* source = sound ? SoundSourceSemantics::findEntry(*sound, media.sourceEntryId) : nullptr;
            const auto conversion = snapshot.media.request(media.authoringSourceId,
                QUuid(pack.platform), error);
            if (!source || !conversion) {
                *error = QStringLiteral("Waveform source is unavailable for Sound %1")
                    .arg(objectIdString(media.soundStableId));
                return false;
            }
            const auto range = SoundSourceSemantics::effectiveRange(*source, &conversion->asset);
            if (!range || range->trimEndFrame <= range->trimStartFrame
                || range->trimEndFrame > SoundSourceSemantics::kMaxSourceFrame) {
                *error = QStringLiteral("Invalid waveform source range for Sound %1")
                    .arg(objectIdString(media.soundStableId));
                return false;
            }
            QString fileName = exportedSources->value(media.authoringSourceId);
            if (fileName.isEmpty()) {
                const auto& sourcePath = conversion->asset.absolutePath;
                if (!conversion->sourceMatches()) {
                    *error = QStringLiteral("Waveform source changed before export: %1").arg(sourcePath);
                    return false;
                }
                WaveformPeakData peaks;
                if (!WaveformCache::load(sourcePath, peaks)
                    && (!WaveformCache::generate(sourcePath) || !WaveformCache::load(sourcePath, peaks))) {
                    *error = QStringLiteral("Cannot generate waveform cache for '%1'").arg(sourcePath);
                    return false;
                }
                QByteArray bytes;
                if (!readAll(WaveformCache::cachePath(sourcePath), &bytes, error)) return false;
                if (!conversion->sourceMatches()) {
                    *error = QStringLiteral("Source changed while exporting waveform '%1'").arg(sourcePath);
                    return false;
                }
                fileName = QString::fromLatin1(QCryptographicHash::hash(bytes,
                    QCryptographicHash::Sha256).toHex()) + QStringLiteral(".vkd");
                if (exportCanceled(request.options.cancelCheck, error)) return false;
                if (!QDir().mkpath(stagingRoot)) {
                    *error = QStringLiteral("Cannot stage waveform '%1'").arg(sourcePath);
                    return false;
                }
                QSaveFile file(QDir(stagingRoot).filePath(fileName));
                if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
                    *error = QStringLiteral("Cannot stage waveform '%1'").arg(sourcePath);
                    return false;
                }
                exportedSources->insert(media.authoringSourceId, fileName);
            }
            waveforms.append(QJsonObject{
                {QStringLiteral("soundGuid"), objectIdString(media.soundStableId)},
                {QStringLiteral("file"), companionDir.relativeFilePath(waveformDir.filePath(fileName))},
                {QStringLiteral("trimStartFrame"), static_cast<qint64>(range->trimStartFrame)},
                {QStringLiteral("trimEndFrame"), static_cast<qint64>(range->trimEndFrame)}});
        }
    }
    if (!mediaIds.empty()) {
        *error = QStringLiteral("Pack '%1' contains media without a waveform source").arg(pack.fileName);
        return false;
    }
    companion->insert(QStringLiteral("waveforms"), waveforms);
    return true;
}

bool publishWaveformCompanion(const QJsonObject& companion, const QString& companionPath,
                              const QString& stagingRoot, const QString& outputRoot,
                              QStringList* newFiles, QString* error,
                              const std::function<bool()>& cancelCheck)
{
    const QDir output(outputRoot);
    const QDir companionDir(QFileInfo(companionPath).absolutePath());
    for (const auto& value : companion.value(QStringLiteral("waveforms")).toArray()) {
        if (exportCanceled(cancelCheck, error)) return false;
        const QString path = QDir::cleanPath(companionDir.absoluteFilePath(
            value.toObject().value(QStringLiteral("file")).toString()));
        const QString fileName = QFileInfo(path).fileName();
        if (!safePublicationPath(output, fileName) || !QDir().mkpath(outputRoot)) {
            *error = QStringLiteral("Cannot use waveform output directory '%1'").arg(outputRoot);
            return false;
        }
        const QString staged = QDir(stagingRoot).filePath(fileName);
        QByteArray expected;
        if (!readAll(staged, &expected, error)) return false;
        if (!QFileInfo::exists(path)) {
            QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(expected) != expected.size() || !file.commit()) {
                *error = QStringLiteral("Cannot publish waveform '%1'").arg(path);
                return false;
            }
            newFiles->append(path);
            continue;
        }
        QByteArray existing;
        if (!readAll(path, &existing, error)) return false;
        if (existing != expected) {
            *error = QStringLiteral("Waveform output content conflicts with '%1'").arg(path);
            return false;
        }
    }
    return !exportCanceled(cancelCheck, error);
}

std::vector<std::uint8_t> encodeVariableRequirements(
    const std::vector<std::pair<harkenstudio::ObjectId, std::uint64_t>>& rows,
    const std::unordered_map<harkenstudio::ObjectId, std::uint64_t>& labelFingerprints)
{
    std::vector<std::uint8_t> payload(16 + rows.size() * 40, 0);
    harkenstudiobuild::StoreLE32(payload.data(), static_cast<std::uint32_t>(rows.size()));
    harkenstudiobuild::StoreLE32(payload.data() + 4, 1);
    std::size_t offset = 16;
    for (const auto& row : rows) {
        payload[offset] = 1;
        harkenstudiobuild::StoreObjectId(payload.data() + offset + 8, row.first);
        harkenstudiobuild::StoreLE64(payload.data() + offset + 24, row.second);
        harkenstudiobuild::StoreLE64(payload.data() + offset + 32, labelFingerprints.at(row.first));
        offset += 40;
    }
    return payload;
}

bool claimIdentity(std::unordered_map<harkenstudio::ObjectId, IdentityOwner>* owners,
                   harkenstudio::ObjectId stableId, std::uint64_t packStableId,
                   std::uint32_t identityClass, QString* error)
{
    const auto existing = owners->find(stableId);
    if (existing != owners->end()) {
        if (existing->second.identityClass == identityClass) {
            existing->second.packStableIds.insert(packStableId);
            return true;
        }
        if (error)
            *error = QStringLiteral("Duplicate ownership for identity %1")
                         .arg(VNX::objectIdString(stableId));
        return false;
    }
    (*owners)[stableId] = {{packStableId}, identityClass};
    return true;
}

bool assignOwnership(
    const CanonicalPackSetExportRequest& request,
    const std::vector<PackMeta>& packs, const QString& language,
    std::uint64_t initId,
    const harkenstudiobuild::ContainerWorldInstallViewV1& world,
    std::unordered_map<harkenstudio::ObjectId, IdentityOwner>* owners, QString* error)
{
    for (const PackMeta& pack : packs) {
        if (pack.actionStableIds.empty()
            || (!pack.language.isEmpty() && pack.language != language))
            continue;
        for (const PackBuildVariant& variant : request.plan->variants) {
            if (std::find(pack.actionStableIds.begin(), pack.actionStableIds.end(),
                          variant.actionStableId) == pack.actionStableIds.end()
                || variant.variant.platform != pack.platform
                || variant.variant.language != pack.language)
                continue;
            for (const ActionClosureMember& action : variant.closure.actions)
                if (!claimIdentity(owners, action.stableId, pack.packStableId,
                                   harkenstudiobuild::kInitCatalogIdentityClassAction, error))
                    return false;
            for (const ActionClosureMember& member : variant.closure.objects) {
                if (!isPlayableType(member.type) || harkenstudio::IsNull(member.stableId)
                    || member.type == ProjectItem::Type::Layer)
                    continue;
                if (!claimIdentity(owners, member.stableId, pack.packStableId,
                                   harkenstudiobuild::kInitCatalogIdentityClassPlayable, error))
                    return false;
            }
        }
    }

    for (std::uint32_t i = 0; i < world.variableCount; ++i) {
        if (!claimIdentity(owners, world.variables[i].variableId, initId,
                           harkenstudiobuild::kInitCatalogIdentityClassVariable, error))
            return false;
    }
    for (std::uint32_t i = 0; i < world.playablePropertyCount; ++i) {
        const auto& property = world.playableProperties[i];
        if (property.contentPropertyKind
                != harkenstudiobuild::playableproperties::ContentPropertyKindV1::Layer)
            continue;
        if (!claimIdentity(owners, property.stableId, initId,
                           harkenstudiobuild::kInitCatalogIdentityClassPlayable, error))
            return false;
    }
    return true;
}

bool ownedBy(const std::unordered_map<harkenstudio::ObjectId, IdentityOwner>& owners,
             harkenstudio::ObjectId stableId, std::uint64_t packStableId)
{
    const auto it = owners.find(stableId);
    return it != owners.end() && it->second.packStableIds.count(packStableId) != 0;
}

bool hostOwnedByPack(const ProjectItem& host, const PackMeta& pack,
                     const std::unordered_map<harkenstudio::ObjectId, IdentityOwner>& owners)
{
    if (pack.actionStableIds.empty())
        return false;
    const harkenstudio::ObjectId id = itemStableId(host);
    return ownedBy(owners, id, pack.packStableId);
}

bool prepareOwnedPayload(harkenstudiobuild::PlayablePropertyPayloadViewV1* payload,
                         std::vector<harkenstudiobuild::SendV1>* ownedSends,
                         QString* error)
{
    if (payload == nullptr)
        return true;
    constexpr auto kMainOutputSlot =
        static_cast<std::size_t>(harkenstudiobuild::playableproperties::PlayablePropertyIdV1::MainOutputBusRef)
        - 1;
    const auto mainOutput = payload->values[kMainOutputSlot].objectId;
    if (payload->sendCount == 0 || payload->sends == nullptr)
        return true;
    if (ownedSends == nullptr) {
        if (error)
            *error = QStringLiteral("Send payload is missing owned storage");
        return false;
    }
    ownedSends->assign(payload->sends, payload->sends + payload->sendCount);
    for (auto& send : *ownedSends) {
        if (send.targetBusId == mainOutput) {
            if (error) {
                *error = QStringLiteral(
                    "Send target equals MainOutput (%1); export refuses send==MainOutput")
                    .arg(VNX::objectIdString(mainOutput));
            }
            return false;
        }
    }
    payload->sends = ownedSends->data();
    return true;
}

const harkenstudiobuild::PlayablePropertyProgramViewV1* findSlicedPlayable(
    const std::vector<harkenstudiobuild::PlayablePropertyProgramViewV1>& playables,
    harkenstudio::ObjectId stableId)
{
    for (const auto& playable : playables) {
        if (playable.stableId == stableId)
            return &playable;
    }
    return nullptr;
}

void rebindPlayableRef(harkenstudiobuild::PlayableRefV1* ref,
                       const std::vector<harkenstudiobuild::PlayablePropertyProgramViewV1>& playables)
{
    if (ref == nullptr)
        return;
    if (const auto* playable = findSlicedPlayable(playables, ref->stableId)) {
        ref->expectedProgramSemanticFingerprint = playable->programSemanticFingerprint;
        ref->contentRevision = playable->contentRevision;
        ref->routeStableId = playable->routeStableId;
        ref->compatibilityFingerprint = playable->compatibilityFingerprint;
    }
}

const harkenstudiobuild::TrackScheduleProgramViewV1* findSlicedTrack(
    const std::vector<harkenstudiobuild::TrackScheduleProgramViewV1>& tracks,
    harkenstudio::ObjectId stableId)
{
    for (const auto& track : tracks) {
        if (track.rootDefinition.stableId == stableId)
            return &track;
    }
    return nullptr;
}

const harkenstudiobuild::SegmentScheduleProgramViewV1* findSlicedSegment(
    const std::vector<harkenstudiobuild::SegmentScheduleProgramViewV1>& segments,
    harkenstudio::ObjectId stableId)
{
    for (const auto& segment : segments) {
        if (segment.rootDefinition.stableId == stableId)
            return &segment;
    }
    return nullptr;
}

void remapActionObjectRef(
    harkenstudiobuild::ActionObjectRefV1* ref,
    const std::vector<harkenstudiobuild::PlayablePropertyProgramViewV1>& playables)
{
    if (ref == nullptr || ref->stableId == harkenstudio::ObjectId{})
        return;
    if (const auto* playable = findSlicedPlayable(playables, ref->stableId))
        ref->expectedProgramSemanticFingerprint = playable->programSemanticFingerprint;
}

bool preparePlayableAfterOwnedCopy(
    harkenstudiobuild::PlayablePropertyProgramViewV1* playable,
    PackMeta* pack,
    QString* error)
{
    if (playable == nullptr || pack == nullptr)
        return true;
    pack->ownedSends.emplace_back();
    if (!prepareOwnedPayload(&playable->payload,
                                    &pack->ownedSends.back(), error))
        return false;
    if (playable->organizationLayerAncestorCount != 0
        && playable->organizationLayerAncestors != nullptr) {
        pack->ownedLayerAncestors.emplace_back(
            playable->organizationLayerAncestors,
            playable->organizationLayerAncestors + playable->organizationLayerAncestorCount);
        playable->organizationLayerAncestors = pack->ownedLayerAncestors.back().data();
    }
    if (playable->structuralAncestorCount != 0 && playable->structuralAncestors != nullptr) {
        pack->ownedStructuralAncestors.emplace_back(
            playable->structuralAncestors,
            playable->structuralAncestors + playable->structuralAncestorCount);
        playable->structuralAncestors = pack->ownedStructuralAncestors.back().data();
    }
    return true;
}

const harkenstudiobuild::PlayablePropertyProgramViewV1* findLiveSlicedPlayable(
    std::vector<PackMeta>& packs,
    const QString& language,
    harkenstudio::ObjectId stableId)
{
    for (PackMeta& pack : packs) {
        if (!pack.language.isEmpty() && pack.language != language)
            continue;
        for (const auto& playable : pack.slicedPlayables) {
            if (playable.stableId == stableId)
                return &playable;
        }
    }
    return nullptr;
}

const harkenstudiobuild::TrackScheduleProgramViewV1* findLiveSlicedTrack(
    std::vector<PackMeta>& packs,
    const QString& language,
    harkenstudio::ObjectId stableId)
{
    for (PackMeta& pack : packs) {
        if (!pack.language.isEmpty() && pack.language != language)
            continue;
        for (const auto& track : pack.slicedTracks) {
            if (track.rootDefinition.stableId == stableId)
                return &track;
        }
    }
    return nullptr;
}

const harkenstudiobuild::SegmentScheduleProgramViewV1* findLiveSlicedSegment(
    std::vector<PackMeta>& packs,
    const QString& language,
    harkenstudio::ObjectId stableId)
{
    for (PackMeta& pack : packs) {
        if (!pack.language.isEmpty() && pack.language != language)
            continue;
        for (const auto& segment : pack.slicedSegments) {
            if (segment.rootDefinition.stableId == stableId)
                return &segment;
        }
    }
    return nullptr;
}

const harkenstudiobuild::ActionProgramViewV1* findLiveSlicedAction(
    std::vector<PackMeta>& packs,
    const QString& language,
    harkenstudio::ObjectId stableId)
{
    for (PackMeta& pack : packs) {
        if (!pack.language.isEmpty() && pack.language != language)
            continue;
        for (const auto& action : pack.slicedActions) {
            if (action.actionStableId == stableId)
                return &action;
        }
    }
    return nullptr;
}

void rebindPlayableRefLive(
    harkenstudiobuild::PlayableRefV1* ref,
    std::vector<PackMeta>& packs,
    const QString& language)
{
    if (ref == nullptr)
        return;
    if (const auto* playable = findLiveSlicedPlayable(packs, language, ref->stableId)) {
        ref->expectedProgramSemanticFingerprint = playable->programSemanticFingerprint;
        ref->contentRevision = playable->contentRevision;
        ref->routeStableId = playable->routeStableId;
        ref->compatibilityFingerprint = playable->compatibilityFingerprint;
    }
}

void remapActionObjectRefLive(
    harkenstudiobuild::ActionObjectRefV1* ref,
    std::vector<PackMeta>& packs,
    const QString& language)
{
    if (ref == nullptr || ref->stableId == harkenstudio::ObjectId{})
        return;
    if (const auto* playable = findLiveSlicedPlayable(packs, language, ref->stableId)) {
        ref->expectedProgramSemanticFingerprint = playable->programSemanticFingerprint;
        return;
    }
    if (const auto* action = findLiveSlicedAction(packs, language, ref->stableId))
        ref->expectedProgramSemanticFingerprint = action->programSemanticFingerprint;
}

void rebindPlayableCrossPackRefs(
    harkenstudiobuild::PlayablePropertyProgramViewV1* playable,
    std::vector<PackMeta>& packs,
    const QString& language)
{
    if (playable == nullptr)
        return;
    if (playable->organizationLayerAncestorCount != 0
        && playable->organizationLayerAncestors != nullptr) {
        auto* ancestors = const_cast<harkenstudiobuild::LayerAncestorRefV1*>(
            playable->organizationLayerAncestors);
        for (std::uint32_t i = 0; i < playable->organizationLayerAncestorCount; ++i) {
            if (const auto* layer = findLiveSlicedPlayable(
                    packs, language, ancestors[i].layerStableId))
                ancestors[i].layerProgramSemanticFingerprint =
                    layer->programSemanticFingerprint;
        }
    }
    if (playable->structuralAncestorCount != 0
        && playable->structuralAncestors != nullptr) {
        auto* ancestors = const_cast<harkenstudiobuild::StructuralAncestorRefV1*>(
            playable->structuralAncestors);
        for (std::uint32_t i = 0; i < playable->structuralAncestorCount; ++i) {
            const auto* current = findLiveSlicedPlayable(packs, language, ancestors[i].stableId);
            if (current == nullptr)
                continue;
            ancestors[i].programSemanticFingerprint = current->programSemanticFingerprint;
            ancestors[i].payload =
                harkenstudiobuild::MakeStructuralAncestorPayloadV1(current->payload);
        }
    }
    playable->programSemanticFingerprint =
        harkenstudiobuild::ComputePlayablePropertyFingerprintV1(*playable);
}

void rebindLayerAncestorFingerprints(
    std::vector<harkenstudiobuild::PlayablePropertyProgramViewV1>* playables)
{
    if (playables == nullptr)
        return;
    for (auto& playable : *playables) {
        if (playable.organizationLayerAncestorCount != 0
            && playable.organizationLayerAncestors != nullptr) {
            auto* ancestors = const_cast<harkenstudiobuild::LayerAncestorRefV1*>(
                playable.organizationLayerAncestors);
            for (std::uint32_t i = 0; i < playable.organizationLayerAncestorCount; ++i) {
                if (const auto* layer = findSlicedPlayable(*playables, ancestors[i].layerStableId))
                    ancestors[i].layerProgramSemanticFingerprint =
                        layer->programSemanticFingerprint;
            }
        }
        playable.programSemanticFingerprint =
            harkenstudiobuild::ComputePlayablePropertyFingerprintV1(playable);
    }
}

bool remapScheduleReferenceHintLive(
    harkenstudiobuild::ChildTimingHintV2* hint,
    std::vector<PackMeta>& packs,
    const QString& language,
    QString* error)
{
    if (hint == nullptr
        || hint->kind != harkenstudiobuild::ChildTimingHintKindV2::ScheduleReference)
        return true;
    auto& key = hint->payload.schedule;
    if (key.kind == harkenstudiobuild::PlayableKindV1::Track) {
        const auto* track = findLiveSlicedTrack(packs, language, key.stableId);
        if (track == nullptr) {
            if (error)
                *error = QStringLiteral(
                    "Schedule reference track %1 is missing from the sliced canonical set")
                    .arg(VNX::objectIdString(key.stableId));
            return false;
        }
        key.scheduleSemanticFingerprint = track->scheduleSemanticFingerprint;
        return true;
    }
    if (key.kind == harkenstudiobuild::PlayableKindV1::Segment) {
        const auto* segment = findLiveSlicedSegment(packs, language, key.stableId);
        if (segment == nullptr) {
            if (error)
                *error = QStringLiteral(
                    "Schedule reference segment %1 is missing from the sliced canonical set")
                    .arg(VNX::objectIdString(key.stableId));
            return false;
        }
        key.scheduleSemanticFingerprint = segment->scheduleSemanticFingerprint;
        return true;
    }
    if (error)
        *error = QStringLiteral(
            "Schedule reference %1 has unsupported kind %2")
            .arg(VNX::objectIdString(key.stableId))
            .arg(static_cast<std::uint32_t>(key.kind));
    return false;
}

bool sliceWorld(
    const harkenstudiobuild::ContainerWorldInstallViewV1& world, std::uint64_t packStableId,
    const std::unordered_map<harkenstudio::ObjectId, IdentityOwner>& owners,
    std::vector<harkenstudiobuild::VariableDefinitionV1>* variables,
    std::vector<harkenstudiobuild::PlayablePropertyProgramViewV1>* playables,
    std::vector<harkenstudiobuild::PlayableOccurrencePropertyViewV1>* occurrences,
    std::vector<harkenstudiobuild::TrackScheduleProgramViewV1>* tracks,
    std::vector<harkenstudiobuild::SegmentScheduleProgramViewV1>* segments,
    std::vector<harkenstudiobuild::ContainerProgramViewV3>* containers,
    std::vector<harkenstudiobuild::TransitionProgramViewV1>* transitions,
    std::vector<harkenstudiobuild::ActionProgramViewV1>* actions,
    PackMeta* pack,
    QString* error)
{
    harkenstudiobuild::ContainerWorldInstallViewV1 slice = world;
    pack->ownedSends.reserve(
        static_cast<std::size_t>(world.playablePropertyCount) + world.playableOccurrenceCount);
    pack->ownedLayerAncestors.reserve(world.playablePropertyCount);
    pack->ownedStructuralAncestors.reserve(world.playablePropertyCount);
    pack->ownedPlacements.reserve(world.trackScheduleCount);
    pack->ownedTrackRefs.reserve(world.segmentScheduleCount);
    pack->ownedContainerChildren.reserve(world.containerCount);
    pack->ownedActionSteps.reserve(world.actionCount);
    for (std::uint32_t i = 0; i < world.variableCount; ++i) {
        if (ownedBy(owners, world.variables[i].variableId, packStableId))
            variables->push_back(world.variables[i]);
    }
    for (std::uint32_t i = 0; i < world.playablePropertyCount; ++i) {
        if (!ownedBy(owners, world.playableProperties[i].stableId, packStableId))
            continue;
        auto playable = world.playableProperties[i];
        if (!preparePlayableAfterOwnedCopy(&playable, pack, error))
            return false;
        playable.programSemanticFingerprint =
            harkenstudiobuild::ComputePlayablePropertyFingerprintV1(playable);
        playables->push_back(playable);
    }
    rebindLayerAncestorFingerprints(playables);
    for (std::uint32_t i = 0; i < world.playableOccurrenceCount; ++i) {
        if (!ownedBy(owners, world.playableOccurrences[i].parentStableId, packStableId))
            continue;
        auto occurrence = world.playableOccurrences[i];
        pack->ownedSends.emplace_back();
        if (!prepareOwnedPayload(&occurrence.payload,
                                        &pack->ownedSends.back(), error))
            return false;
        if (const auto* parent = findSlicedPlayable(*playables, occurrence.parentStableId))
            occurrence.parentProgramSemanticFingerprint = parent->programSemanticFingerprint;
        rebindPlayableRef(&occurrence.childPlayable, *playables);
        occurrence.occurrenceSemanticFingerprint =
            harkenstudiobuild::ComputePlayableOccurrenceFingerprintV1(occurrence);
        occurrences->push_back(occurrence);
    }
    for (std::uint32_t i = 0; i < world.trackScheduleCount; ++i) {
        if (!ownedBy(owners, world.trackSchedules[i].rootDefinition.stableId, packStableId))
            continue;
        auto track = world.trackSchedules[i];
        rebindPlayableRef(&track.rootDefinition, *playables);
        if (track.placementCount != 0 && track.placements != nullptr) {
            pack->ownedPlacements.emplace_back(
                track.placements, track.placements + track.placementCount);
            auto& placements = pack->ownedPlacements.back();
            for (auto& placement : placements)
                rebindPlayableRef(&placement.content, *playables);
            track.placements = placements.data();
        }
        track.scheduleSemanticFingerprint =
            harkenstudiobuild::ComputeTrackScheduleFingerprintV1(track);
        tracks->push_back(track);
    }
    for (std::uint32_t i = 0; i < world.segmentScheduleCount; ++i) {
        if (!ownedBy(owners, world.segmentSchedules[i].rootDefinition.stableId, packStableId))
            continue;
        auto segment = world.segmentSchedules[i];
        rebindPlayableRef(&segment.rootDefinition, *playables);
        if (segment.trackRefCount != 0 && segment.trackRefs != nullptr) {
            pack->ownedTrackRefs.emplace_back(
                segment.trackRefs, segment.trackRefs + segment.trackRefCount);
            auto& trackRefs = pack->ownedTrackRefs.back();
            for (auto& trackRef : trackRefs) {
                rebindPlayableRef(&trackRef.trackDefinition, *playables);
                if (const auto* track = findSlicedTrack(*tracks, trackRef.trackDefinition.stableId))
                    trackRef.trackScheduleSemanticFingerprint = track->scheduleSemanticFingerprint;
            }
            segment.trackRefs = trackRefs.data();
        }
        segment.scheduleSemanticFingerprint =
            harkenstudiobuild::ComputeSegmentScheduleFingerprintV1(segment);
        segments->push_back(segment);
    }
    for (std::uint32_t i = 0; i < world.transitionCount; ++i) {
        if (ownedBy(owners, world.transitions[i].ownerContainerStableId,
                    packStableId))
            transitions->push_back(world.transitions[i]);
    }
    for (std::uint32_t i = 0; i < world.containerCount; ++i) {
        if (!ownedBy(owners, world.containers[i].containerStableId,
                     packStableId)) {
            continue;
        }
        harkenstudiobuild::ContainerProgramViewV3 container = world.containers[i];
        if (container.childCount != 0 && container.children != nullptr) {
            pack->ownedContainerChildren.emplace_back(
                container.children, container.children + container.childCount);
            auto& children = pack->ownedContainerChildren.back();
            for (auto& child : children) {
                rebindPlayableRef(&child.playable, *playables);
            }
            container.children = children.data();
        }
        container.transitionProgramIndex = UINT32_MAX;
        const auto transition = std::find_if(
            transitions->begin(), transitions->end(),
            [&](const harkenstudiobuild::TransitionProgramViewV1& candidate) {
                return candidate.ownerContainerStableId ==
                       container.containerStableId;
            });
        if (transition != transitions->end()) {
            container.transitionProgramIndex = static_cast<std::uint32_t>(
                transition - transitions->begin());
        }
        containers->push_back(container);
    }
    for (std::uint32_t i = 0; i < world.actionCount; ++i) {
        if (!ownedBy(owners, world.actions[i].actionStableId, packStableId))
            continue;
        auto action = world.actions[i];
        if (action.stepCount != 0 && action.steps != nullptr) {
            pack->ownedActionSteps.emplace_back(action.steps, action.steps + action.stepCount);
            auto& steps = pack->ownedActionSteps.back();
            for (auto& step : steps) {
                remapActionObjectRef(&step.target, *playables);
                remapActionObjectRef(&step.quantizeReference, *playables);
            }
            action.steps = steps.data();
            action.programSemanticFingerprint =
                harkenstudiobuild::ComputeActionProgramFingerprintV1(action);
        }
        actions->push_back(action);
    }

    slice.variables = variables->empty() ? nullptr : variables->data();
    slice.variableCount = static_cast<std::uint32_t>(variables->size());
    slice.playableProperties = playables->empty() ? nullptr : playables->data();
    slice.playablePropertyCount = static_cast<std::uint32_t>(playables->size());
    slice.playableOccurrences = occurrences->empty() ? nullptr : occurrences->data();
    slice.playableOccurrenceCount = static_cast<std::uint32_t>(occurrences->size());
    slice.trackSchedules = tracks->empty() ? nullptr : tracks->data();
    slice.trackScheduleCount = static_cast<std::uint32_t>(tracks->size());
    slice.segmentSchedules = segments->empty() ? nullptr : segments->data();
    slice.segmentScheduleCount = static_cast<std::uint32_t>(segments->size());
    slice.containers = containers->empty() ? nullptr : containers->data();
    slice.containerCount = static_cast<std::uint32_t>(containers->size());
    slice.transitions = transitions->empty() ? nullptr : transitions->data();
    slice.transitionCount = static_cast<std::uint32_t>(transitions->size());
    slice.actions = actions->empty() ? nullptr : actions->data();
    slice.actionCount = static_cast<std::uint32_t>(actions->size());
    slice.identity.capabilityMask =
        harkenstudiobuild::detail::ComputeCurrentWorldLocalCapabilityMaskV1(slice);
    slice.identity.runtimeSemanticFingerprint =
        harkenstudiobuild::ComputeCurrentWorldFingerprintV5(slice);
    return true;
}

bool encodePartition(std::uint64_t packStableId,
                     const harkenstudiobuild::ContainerWorldInstallViewV1& slice,
                     std::uint32_t ownedDefinitions,
                     std::vector<std::uint8_t>* payload, QString* error,
                     harkenstudiobuild::CanonicalBusTriple busTriple,
                     std::uint64_t partitionRevision = 1)
{
    std::vector<std::uint8_t> body;
    if (!harkenstudiobuild::EncodeCurrentWorldShippingBody(slice, body)) {
        if (error)
            *error = QStringLiteral("Failed to encode definition partition");
        return false;
    }
    harkenstudiobuild::PackCanonicalPartitionHeader header;
    header.headerBytes = harkenstudiobuild::kCanonicalDefinitionPartitionHeaderV2Bytes;
    header.bodyVintage = harkenstudiobuild::kCanonicalDefinitionPartitionBodyVintage;
    header.bodyBytes = body.size();
    header.bodyChecksum = harkenstudiobuild::Fnv1a64(body.data(), body.size());
    header.packStableId = packStableId;
    header.partitionRevision = partitionRevision;
    header.ownedDefinitionCount = ownedDefinitions;
    harkenstudiobuild::ApplyCanonicalBusTriple(&header, busTriple);
    payload->assign(harkenstudiobuild::kCanonicalDefinitionPartitionHeaderV2Bytes + body.size(), 0);
    harkenstudiobuild::SerializePackCanonicalPartitionHeaderV2(header, payload->data());
    header.headerChecksum = harkenstudiobuild::Fnv1a64(payload->data(), 80);
    harkenstudiobuild::SerializePackCanonicalPartitionHeaderV2(header, payload->data());
    if (!body.empty()) {
        std::memcpy(payload->data() + harkenstudiobuild::kCanonicalDefinitionPartitionHeaderV2Bytes,
                    body.data(), body.size());
    }
    return true;
}

std::uint64_t lineageFingerprint(
    const std::unordered_map<harkenstudio::ObjectId, IdentityOwner>& owners,
    std::uint64_t packStableId)
{
    std::vector<harkenstudio::ObjectId> ids;
    for (const auto& [stableId, owner] : owners) {
        if (owner.packStableIds.count(packStableId) != 0)
            ids.push_back(stableId);
    }
    std::sort(ids.begin(), ids.end());
    std::vector<std::uint8_t> bytes;
    bytes.reserve(ids.size() * 16);
    for (const auto id : ids) {
        const auto encoded = harkenstudio::ObjectIdBytes(id);
        bytes.insert(bytes.end(), encoded.begin(), encoded.end());
    }
    return harkenstudiobuild::Fnv1a64(bytes.data(), bytes.size());
}

std::uint32_t ownedDefinitionCount(
    const std::unordered_map<harkenstudio::ObjectId, IdentityOwner>& owners,
    std::uint64_t packStableId)
{
    std::uint32_t count = 0;
    for (const auto& [stableId, owner] : owners) {
        (void)stableId;
        if (owner.packStableIds.count(packStableId) != 0)
            ++count;
    }
    return count;
}

bool readPreviousPack(const QDir& output, PackMeta* pack, QByteArray* bytes,
                      harkenstudiobuild::CurrentWorldBodyImage* body,
                      bool publicationMetadataOnly = false)
{
    const auto path = pack->previousRow.value(QStringLiteral("path")).toString();
    QString error;
    harkenstudiobuild::PackCanonicalImage image;
    return !path.isEmpty() && safePublicationPath(output, path)
        && readAll(output.filePath(path), bytes, &error)
        && (!pack->previousRow.contains(QStringLiteral("contentHash"))
            || harkenstudiobuild::Fnv1a64(bytes->constData(), bytes->size())
                == pack->previousRow.value(QStringLiteral("contentHash")).toString().toULongLong())
        && harkenstudiobuild::ParsePackCanonicalImage(
            reinterpret_cast<const std::uint8_t*>(bytes->constData()), bytes->size(), &image) == harkenstudiobuild::kPackOk
        && image.packStableId == pack->packStableId && image.packKind == pack->packKind
        && collectEntries(output.filePath(path), &pack->previousEntries, &error,
                          false, publicationMetadataOnly)
        && body->Decode(image.partitionBody.data(), image.partitionBody.size(),
            harkenstudiobuild::kCanonicalDefinitionPartitionBodyVintage);
}

struct PublicationAuthoredOrders
{
    QString language;
    std::map<harkenstudio::ObjectId, std::uint32_t> properties;
    std::map<harkenstudio::ObjectId, std::uint32_t> containers;
};

std::vector<PublicationAuthoredOrders> readPublicationAuthoredOrders(
    const QDir& output, const QJsonArray& publication,
    std::set<std::uint32_t>* reservedProperties,
    std::set<std::uint32_t>* reservedContainers)
{
    std::vector<PublicationAuthoredOrders> result;
    for (const auto& value : publication) {
        PackMeta pack;
        pack.previousRow = value.toObject();
        pack.language = pack.previousRow.value(QStringLiteral("language")).toString();
        pack.packStableId = pack.previousRow.value(QStringLiteral("packStableId")).toString().toULongLong();
        pack.packKind = pack.previousRow.value(QStringLiteral("packKind")).toInt();
        QByteArray bytes;
        harkenstudiobuild::CurrentWorldBodyImage body;
        if (!readPreviousPack(output, &pack, &bytes, &body, true)) continue;
        PublicationAuthoredOrders orders;
        orders.language = pack.language;
        const auto& world = *body.ProductionView();
        for (std::uint32_t i = 0; i < world.playablePropertyCount; ++i) {
            const auto& property = world.playableProperties[i];
            reservedProperties->insert(property.authoredStableOrder);
            orders.properties.emplace(property.stableId, property.authoredStableOrder);
        }
        for (std::uint32_t i = 0; i < world.containerCount; ++i) {
            const auto& container = world.containers[i];
            reservedContainers->insert(container.authoredStableOrder);
            orders.containers.emplace(container.containerStableId, container.authoredStableOrder);
        }
        result.push_back(std::move(orders));
    }
    return result;
}

bool mergePublicationAuthoredOrders(const std::vector<PublicationAuthoredOrders>& publication,
                                    const QString& language,
                                    const std::set<harkenstudio::ObjectId>& neutralDefinitions,
                                    std::map<harkenstudio::ObjectId, std::uint32_t>* properties,
                                    std::map<harkenstudio::ObjectId, std::uint32_t>* containers,
                                    QString* error)
{
    std::map<std::uint32_t, harkenstudio::ObjectId> propertyOwners, containerOwners;
    const auto merge = [&](const auto& source, auto& orders, auto& owners, bool compatible) {
        for (const auto& [id, order] : source) {
            if (!compatible && neutralDefinitions.count(id) == 0) continue;
            const auto [definition, newDefinition] = orders.emplace(id, order);
            const auto [owner, newOrder] = owners.emplace(order, id);
            if ((!newDefinition && definition->second != order)
                || (!newOrder && owner->second != id)) {
                *error = QStringLiteral("Published definitions have conflicting authored orders for %1")
                    .arg(objectIdString(id));
                return false;
            }
        }
        return true;
    };
    for (const auto& published : publication) {
        const bool compatible = published.language.isEmpty() || published.language == language;
        if (!merge(published.properties, *properties, propertyOwners, compatible)
            || !merge(published.containers, *containers, containerOwners, compatible)) return false;
    }
    const auto validateNeutral = [&](const auto& source, const auto& owners) {
        for (const auto& [id, order] : source) {
            const auto owner = owners.find(order);
            if (owner != owners.end() && owner->second != id
                && neutralDefinitions.count(owner->second) != 0) {
                *error = QStringLiteral("Published definitions have conflicting authored orders for %1")
                    .arg(objectIdString(owner->second));
                return false;
            }
        }
        return true;
    };
    for (const auto& published : publication) {
        if (!validateNeutral(published.properties, propertyOwners)
            || !validateNeutral(published.containers, containerOwners)) return false;
    }
    return true;
}

struct CapacityFields
{
    std::vector<std::uint32_t*> values;
    void U32(std::uint32_t& value) { values.push_back(&value); }
};

bool mergeCapacity(harkenstudiobuild::WorldCapacitySummaryV1* target,
                   harkenstudiobuild::WorldCapacitySummaryV1 source)
{
    CapacityFields destinationFields, sourceFields;
    harkenstudiobuild::VisitWorldCapacitySummaryV1(*target, destinationFields);
    harkenstudiobuild::VisitWorldCapacitySummaryV1(source, sourceFields);
    bool changed = false;
    for (std::size_t i = 0; i < destinationFields.values.size(); ++i) {
        auto& value = *destinationFields.values[i];
        const auto required = *sourceFields.values[i];
        changed = changed || value < required;
        value = std::max(value, required);
    }
    return changed;
}

bool harmonizePublicationCapacity(std::vector<PackMeta>* packs,
                                  const QDir& liveDir, const QDir& resourceCacheDir,
                                  QString* error)
{
    std::vector<std::unique_ptr<harkenstudiobuild::CurrentWorldBodyImage>> bodies;
    std::vector<harkenstudiobuild::PackCanonicalPartitionHeader> headers;
    harkenstudiobuild::WorldCapacitySummaryV1 common;
    QStringList languages{QString()};
    for (auto& pack : *packs) {
        auto body = std::make_unique<harkenstudiobuild::CurrentWorldBodyImage>();
        if (pack.retained) {
            QByteArray bytes;
            if (!readPreviousPack(liveDir, &pack, &bytes, body.get(), true)) {
                *error = QStringLiteral("Cannot read retained Pack '%1'").arg(pack.fileName);
                return false;
            }
            pack.previousValid = true;
            QFile cache(resourceCacheDir.filePath(QString::fromLatin1(
                QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())));
            if (cache.open(QIODevice::ReadOnly))
                pack.resourceFingerprint = QString::fromLatin1(cache.readAll());
            for (const auto& entry : pack.previousEntries) {
                if (entry.kind == harkenstudiobuild::kPackEntryKindCanonicalDefinitionPartition)
                    pack.partitionPayload = entry.payload;
                else if (entry.kind == harkenstudiobuild::kPackEntryKindVariablePolicyOverlay)
                    pack.variablePolicyOverlayPayload = entry.payload;
                else if (entry.kind == harkenstudiobuild::kPackEntryKindVariableRequirements)
                    pack.variableRequirementsPayload = entry.payload;
                else if (entry.kind == harkenstudiobuild::kPackEntryKindPackRequirementBlock) {
                    harkenstudiobuild::PackRequirementBlock requirement;
                    if (!harkenstudiobuild::ParsePackRequirementBlockV2(
                            entry.payload.data(), entry.payload.size(), &requirement)) {
                        *error = QStringLiteral("Invalid retained Pack requirement");
                        return false;
                    }
                    pack.ownedDefinitions = requirement.ownedDefinitionCount;
                    pack.ownedResources = requirement.ownedResourceCount;
                    pack.semanticFingerprint = requirement.semanticFingerprint;
                    pack.lineageFingerprint = requirement.lineageFingerprint;
                }
            }
        }
        harkenstudiobuild::PackCanonicalPartitionHeader header;
        if (!harkenstudiobuild::ParsePackCanonicalPartitionHeaderV2(
                pack.partitionPayload.data(), pack.partitionPayload.size(), &header)
            || header.headerBytes > pack.partitionPayload.size()
            || header.bodyBytes != pack.partitionPayload.size() - header.headerBytes
            || (!pack.retained && !body->Decode(
                pack.partitionPayload.data() + header.headerBytes,
                static_cast<std::size_t>(header.bodyBytes), header.bodyVintage))) {
            *error = QStringLiteral("Cannot decode Pack capacity for '%1'").arg(pack.fileName);
            return false;
        }
        mergeCapacity(&common, body->ProductionView()->capacity);
        if (pack.previousValid) mergeCapacity(&common, pack.previousCapacity);
        if (!languages.contains(pack.language)) languages.append(pack.language);
        headers.push_back(header);
        bodies.push_back(std::move(body));
    }
    for (const auto& language : languages) {
        std::vector<harkenstudiobuild::VariableDefinitionV1> variables;
        std::vector<harkenstudiobuild::PlayablePropertyProgramViewV1> properties;
        std::vector<harkenstudiobuild::PlayableOccurrencePropertyViewV1> occurrences;
        std::vector<harkenstudiobuild::ContainerProgramViewV3> containers;
        std::vector<harkenstudiobuild::ActionProgramViewV1> actions;
        std::vector<harkenstudiobuild::TrackScheduleProgramViewV1> tracks;
        std::vector<harkenstudiobuild::SegmentScheduleProgramViewV1> segments;
        std::set<harkenstudio::ObjectId> variableIds, propertyIds, containerIds, actionIds;
        std::set<harkenstudio::ObjectId> trackIds, segmentIds;
        std::set<std::tuple<std::uint32_t, harkenstudio::ObjectId, std::uint32_t>> occurrenceIds;
        const auto append = [](auto& rows, auto& ids, const auto* source,
                               std::uint32_t count, auto identity) {
            for (std::uint32_t i = 0; i < count; ++i)
                if (ids.insert(identity(source[i])).second) rows.push_back(source[i]);
        };
        for (std::size_t i = 0; i < packs->size(); ++i) {
            const auto& pack = (*packs)[i];
            if (!pack.language.isEmpty() && pack.language != language) continue;
            const auto& world = *bodies[i]->ProductionView();
            append(variables, variableIds, world.variables, world.variableCount,
                   [](const auto& row) { return row.variableId; });
            append(properties, propertyIds, world.playableProperties, world.playablePropertyCount,
                   [](const auto& row) { return row.stableId; });
            append(occurrences, occurrenceIds, world.playableOccurrences, world.playableOccurrenceCount,
                   [](const auto& row) { return std::make_tuple(
                       static_cast<std::uint32_t>(row.parentKind),
                       row.parentStableId, row.stableOccurrenceOrdinal); });
            append(containers, containerIds, world.containers, world.containerCount,
                   [](const auto& row) { return row.containerStableId; });
            append(actions, actionIds, world.actions, world.actionCount,
                   [](const auto& row) { return row.actionStableId; });
            append(tracks, trackIds, world.trackSchedules, world.trackScheduleCount,
                   [](const auto& row) { return row.rootDefinition.stableId; });
            append(segments, segmentIds, world.segmentSchedules, world.segmentScheduleCount,
                   [](const auto& row) { return row.rootDefinition.stableId; });
        }
        if (variables.size() > harkenstudiobuild::kHardVariableDefinitionCapacity
            || properties.size() > harkenstudiobuild::kHardPlayablePropertyProgramCapacity
            || occurrences.size() > harkenstudiobuild::kHardPlayableOccurrencePropertyCapacity
            || containers.size() > harkenstudiobuild::kHardContainerProgramCapacity
            || actions.size() > harkenstudiobuild::kHardActionDefinitionCapacity
            || tracks.size() > harkenstudiobuild::kHardTrackScheduleProgramCapacity
            || segments.size() > harkenstudiobuild::kHardSegmentScheduleProgramCapacity) {
            *error = QStringLiteral("Published Pack set exceeds World representation limits");
            return false;
        }
        harkenstudiobuild::ContainerWorldInstallViewV1 combined;
        combined.variables = variables.empty() ? nullptr : variables.data();
        combined.variableCount = static_cast<std::uint32_t>(variables.size());
        combined.playableProperties = properties.empty() ? nullptr : properties.data();
        combined.playablePropertyCount = static_cast<std::uint32_t>(properties.size());
        combined.playableOccurrences = occurrences.empty() ? nullptr : occurrences.data();
        combined.playableOccurrenceCount = static_cast<std::uint32_t>(occurrences.size());
        combined.containers = containers.empty() ? nullptr : containers.data();
        combined.containerCount = static_cast<std::uint32_t>(containers.size());
        combined.actions = actions.empty() ? nullptr : actions.data();
        combined.actionCount = static_cast<std::uint32_t>(actions.size());
        combined.trackSchedules = tracks.empty() ? nullptr : tracks.data();
        combined.trackScheduleCount = static_cast<std::uint32_t>(tracks.size());
        combined.segmentSchedules = segments.empty() ? nullptr : segments.data();
        combined.segmentScheduleCount = static_cast<std::uint32_t>(segments.size());
        if (!harkenstudiobuild::GrowAuthoredWorldCapacityV1(combined, &common)) {
            *error = QStringLiteral("Published Pack set exceeds World content capacity");
            return false;
        }
    }
    for (std::size_t i = 0; i < packs->size(); ++i) {
        auto& pack = (*packs)[i];
        auto world = *bodies[i]->ProductionView();
        if (!mergeCapacity(&world.capacity, common)) continue;
        world.identity.runtimeSemanticFingerprint =
            harkenstudiobuild::ComputeCurrentWorldFingerprintV5(world);
        if (!encodePartition(pack.packStableId, world, pack.ownedDefinitions,
                &pack.partitionPayload, error,
                harkenstudiobuild::PartitionBusTriple(headers[i]),
                headers[i].partitionRevision))
            return false;
        pack.semanticFingerprint = harkenstudiobuild::Fnv1a64(
            pack.partitionPayload.data() + harkenstudiobuild::kCanonicalDefinitionPartitionHeaderV2Bytes,
            pack.partitionPayload.size() - harkenstudiobuild::kCanonicalDefinitionPartitionHeaderV2Bytes);
        pack.metadataUpdated = pack.retained;
    }
    return true;
}

bool collectPlatformWritePaths(const ProjectData& project, const QDir& output,
                               const PackBuildPlan& plan,
                               const ProjectPackExporter::ExportOptions& options,
                               QStringList* paths, QString* error)
{
    paths->append(output.absolutePath());
    QStringList files, binaries;
    if (!scanOutputFiles(output, &files, &binaries, error, false, options.cancelCheck)) return false;
    QJsonArray publication;
    QStringList managed;
    if (!ReadPackPublication(output.path(), project.projectId, options.platform,
            &publication, &managed, error, false, {}, !options.updateSelectedOnly, options.cancelCheck)) return false;
    QMap<QString, QJsonObject> previous;
    QStringList previousCompanions;
    for (const auto& value : publication) {
        const auto row = value.toObject();
        const QString path = row.value(QStringLiteral("path")).toString();
        previous.insert(path, row);
        const QFileInfo file(path);
        previousCompanions.append(QDir::cleanPath(QDir(file.path()).filePath(
            file.completeBaseName() + QStringLiteral(".json"))));
    }
    QStringList plannedFiles;
    const auto appendPack = [&](const QString& relative, std::uint64_t id, std::uint32_t kind) {
        if (exportCanceled(options.cancelCheck, error)) return false;
        if (!safePublicationPath(output, relative)) {
            *error = QStringLiteral("Invalid Pack output path: %1").arg(output.filePath(relative));
            return false;
        }
        const QFileInfo file(relative);
        const auto companion = QDir::cleanPath(QDir(file.path()).filePath(
            file.completeBaseName() + QStringLiteral(".json")));
        plannedFiles.append(relative);
        plannedFiles.append(companion);
        paths->append(QFileInfo(output.filePath(relative)).absolutePath());
        PackMeta pack;
        pack.packStableId = id;
        pack.packKind = kind;
        pack.previousRow = previous.value(relative);
        if (pack.previousRow.isEmpty() && files.contains(relative))
            pack.previousRow.insert(QStringLiteral("path"), relative);
        QByteArray bytes;
        harkenstudiobuild::CurrentWorldBodyImage body;
        const bool valid = readPreviousPack(output, &pack, &bytes, &body);
        const bool retained = options.updateSelectedOnly && kind == harkenstudiobuild::kPackKindInit
            && valid && previous.contains(relative);
        if (!retained && (options.forceReexport || !valid))
            paths->append(output.filePath(relative));
        if (!retained && QFileInfo(output.filePath(companion)).isFile()) {
            QByteArray companionBytes;
            if (!readAll(output.filePath(companion), &companionBytes, error)) return false;
            const auto document = QJsonDocument::fromJson(companionBytes);
            const auto object = document.object();
            if (!document.isObject()
                || !ProjectSchemaValidator::validatePackManifest(HARKEN_PACK_SCHEMA_VERSION, object, companion).isEmpty()
                || object.contains(QStringLiteral("metadata")) != project.buildExportPackMetadata
                || (!project.buildExportWaveforms && object.contains(QStringLiteral("waveforms"))))
                paths->append(output.filePath(companion));
        }
        return true;
    };
    if (!appendPack(QStringLiteral("Init") + Bridge::packArtifactExtension(),
            fnvName(options.platform + QStringLiteral("/Init")), harkenstudiobuild::kPackKindInit)) return false;
    for (const auto& unit : plan.units) {
        const bool hasContent = std::any_of(unit.actionStableIds.begin(), unit.actionStableIds.end(),
            [&](auto id) {
                const ProjectItem* action = findAction(project, id);
                return action && action->comp<VAction>() && !action->comp<VAction>()->steps.empty();
            });
        if (hasContent && !appendPack(PackBuildPlanning::packFileName(
                unit.variant.language, unit.name, Bridge::packArtifactExtension()),
                fnvName(PackBuildPlanning::relativePackPath(unit.variant.platform,
                    unit.variant.language, unit.artifactKey)), harkenstudiobuild::kPackKindCanonicalContent)) return false;
    }
    for (const auto& name : files) {
        if (exportCanceled(options.cancelCheck, error)) return false;
        if (containsPublicationFile(output, plannedFiles, name) || isWaveformOutputFile(QFileInfo(name)))
            continue;
        if ((options.selfCleaning && !options.updateSelectedOnly)
            || (managed.contains(name) && !binaries.contains(name) && !previousCompanions.contains(name)))
            paths->append(output.filePath(name));
    }
    return true;
}

} // namespace

bool ProjectPackExporter::ValidatePackBuildWriteAccess(
    const ProjectData& project, const QString& baseDir, const QStringList& platforms,
    const ExportOptions& options, QString* error, QString* summary)
{
    if (summary) summary->clear();
    const auto reject = [&](const QString& message) {
        if (summary) *summary = message;
        return false;
    };
    if (PathUtils::isProtectedPackOutputPath(baseDir, project.projectPath)) {
        if (error) *error = PathUtils::packBuildOutputLocationError();
        return reject(PathUtils::packBuildOutputLocationError());
    }
    const QDir root(baseDir);
    QStringList paths{root.absolutePath()};
    QStringList platformNames;
    for (const auto& id : platforms) {
        const auto* platform = ProjectPlatformSemantics::find(project.platforms, QUuid(id));
        if (!platform) {
            *error = QStringLiteral("Unknown project platform ID: %1").arg(id);
            return reject(tr("A selected export platform is no longer in the project. Check the platform selection, then retry."));
        }
        platformNames.append(platform->name);
        auto platformOptions = options;
        platformOptions.platform = id;
        const QDir output(platformOutputDir(baseDir, *platform));
        std::unique_ptr<QLockFile> platformLock;
        if (output.exists()) {
            platformLock = std::make_unique<QLockFile>(output.filePath(QStringLiteral(".harken-pack-platform.lock")));
            platformLock->setStaleLockTime(0);
            if (!platformLock->tryLock()) {
                *error = QStringLiteral("Platform output is being built by another request: %1").arg(output.path());
                return reject(tr("Another export is using a selected platform's output directory. Wait for it to finish, then retry."));
            }
            if (!recoverInterruptedPublish(output, error))
                return reject(tr("Pack export could not recover an interrupted publication. Check the previous output and recovery files before retrying."));
        }
        if (!collectPlatformWritePaths(project, output,
                createBuildPlan(project, platformOptions), platformOptions, &paths, error))
            return reject(tr("Pack export could not inspect the existing packs. Check the output files and publication metadata before retrying."));
    }
    if (options.selfCleaning && !options.updateSelectedOnly) {
        QStringList files;
        if (!scanCleanupFiles(root, platformNames, &files, nullptr, error, options.cancelCheck))
            return reject(tr("Pack export could not inspect the output files for cleanup. Check the output directory and file access before retrying."));
        for (const auto& name : files) {
            if (std::any_of(platformNames.begin(), platformNames.end(), [&](const auto& platform) {
                    return name.startsWith(platform + QLatin1Char('/'));
                }) || isWaveformOutputFile(QFileInfo(name))) continue;
            paths.append(root.filePath(name));
        }
    } else {
        for (const auto& entry : root.entryInfoList(QDir::Files | QDir::Hidden | QDir::System)) {
            if (entry.fileName().compare(QStringLiteral("Project.json"), Qt::CaseInsensitive) == 0
                || entry.fileName().compare(QStringLiteral("ProjectData.json"), Qt::CaseInsensitive) == 0)
                paths.append(entry.absoluteFilePath());
        }
    }
    if (project.buildExportProjectData) paths.append(root.filePath(QStringLiteral("Project.json")));
    return !exportCanceled(options.cancelCheck, error)
        && validateWriteAccess(paths, error, options.cancelCheck, summary);
}

bool ReadPackPublication(const QString& directory, const QString& projectId,
                         const QString& platformId, QJsonArray* packs,
                         QStringList* managedFiles, QString* error, bool requireComplete,
                         const QStringList& expectedPackPaths, bool allowPlatformReplacement,
                         const std::function<bool()>& cancelCheck)
{
    *packs = {};
    managedFiles->clear();
    const QDir dir(directory);
    const auto reject = [&](const QString& message) {
        if (error) *error = message;
        return false;
    };
    QStringList files, binaries;
    QString detail;
    if (!scanOutputFiles(dir, &files, &binaries, &detail, false, cancelCheck)) return reject(detail);
    for (const auto& path : expectedPackPaths)
        if (!safePublicationPath(dir, path))
            return reject(QStringLiteral("Unsafe requested Pack path '%1'").arg(path));
    if (!expectedPackPaths.isEmpty())
        std::stable_sort(binaries.begin(), binaries.end(), [&](const auto& left, const auto& right) {
            return expectedPackPaths.contains(left) && !expectedPackPaths.contains(right);
        });
    harkenstudio::ObjectId expectedProject = runtimeObjectId(QUuid(projectId));
    std::uint64_t initId = 0;
    bool selectedInit = false;
    QSet<QString> selectedPaths;
    QSet<qulonglong> ids;
    std::unordered_map<std::uint64_t, std::uint32_t> kinds;
    std::vector<harkenstudiobuild::PackDependencyEdge> dependencyEdges;
    QSet<QString> names;
    QSet<QString> actionIds;
    QSet<QString> localizedNames;
    QSet<QString> localizedActionIds;
    for (const auto& path : binaries) {
        if (exportCanceled(cancelCheck, error)) return false;
        const auto info = QFileInfo(path);
        const bool selected = expectedPackPaths.isEmpty() || expectedPackPaths.contains(path);
        const QString companionPath = QDir::cleanPath(QDir(info.path()).filePath(info.completeBaseName() + QStringLiteral(".json")));
        const bool hasCompanion = files.contains(companionPath);
        QByteArray bytes;
        harkenstudiobuild::PackCanonicalImage image;
        harkenstudiobuild::CurrentWorldBodyImage body;
        if (!readAll(dir.filePath(path), &bytes, &detail)
            || harkenstudiobuild::ParsePackCanonicalImage(reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                bytes.size(), &image) != harkenstudiobuild::kPackOk
            || !body.Decode(image.partitionBody.data(), image.partitionBody.size(),
                harkenstudiobuild::kCanonicalDefinitionPartitionBodyVintage)) {
            if (requireComplete && selected && hasCompanion)
                return reject(QStringLiteral("Invalid published Pack '%1'").arg(path));
            continue;
        }
        const auto& world = *body.ProductionView();
        const auto pairProject = world.identity.projectId;
        if (!harkenstudio::IsNull(pairProject) && !harkenstudio::IsNull(expectedProject)
            && pairProject != expectedProject)
            return reject(QStringLiteral("Pack publication contains another project's content: %1").arg(path));
        if (selected && !platformId.isEmpty() && (requireComplete || !allowPlatformReplacement)) {
            const auto expectedInit = fnvName(platformId + QStringLiteral("/Init"));
            if (image.packKind == harkenstudiobuild::kPackKindInit && image.packStableId != expectedInit)
                return reject(QStringLiteral("Pack publication contains another platform's content: %1").arg(path));
            for (const auto& edge : image.dependencyEdges)
                if (edge.edgeKind == harkenstudiobuild::kPackDependencyEdgeKindInit
                    && edge.prerequisitePackStableId != expectedInit
                    && harkenstudiobuild::ValidateCanonicalInitDependency(
                        image.dependencyEdges, edge.prerequisitePackStableId) == harkenstudiobuild::kPackOk)
                    return reject(QStringLiteral("Pack publication contains another platform's content: %1").arg(path));
        }
        if (!hasCompanion) continue;
        const auto hash = harkenstudiobuild::Fnv1a64(bytes.constData(), bytes.size());
        QJsonObject row;
        QString pairError;
        const auto inspectPair = [&] {
            const auto rejectPair = [&](const QString& message) {
                pairError = message;
                return false;
            };
            QJsonObject companion;
            QByteArray companionBytes;
            if (!readAll(dir.filePath(companionPath), &companionBytes, &detail)) return rejectPair(detail);
            QJsonParseError parseError;
            const auto document = QJsonDocument::fromJson(companionBytes, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject())
                return rejectPair(QStringLiteral("Invalid Pack companion '%1'").arg(companionPath));
            companion = document.object();
            const auto companionErrors = ProjectSchemaValidator::validatePackManifest(
                HARKEN_PACK_SCHEMA_VERSION, companion, companionPath);
            if (!companionErrors.isEmpty()) return rejectPair(companionErrors.join(QLatin1Char('\n')));
            if (harkenstudio::IsNull(pairProject))
                return rejectPair(QStringLiteral("Pack publication contains an invalid project identity: %1").arg(path));
            if (requireComplete && selected && ids.contains(image.packStableId))
                return rejectPair(QStringLiteral("Pack publication repeats a Pack identity: %1").arg(path));
            QString name;
            if (image.packKind == harkenstudiobuild::kPackKindInit) {
                if ((selected && initId != 0) || path != QStringLiteral("Init.bytes")
                    || (!platformId.isEmpty() && image.packStableId != fnvName(platformId + QStringLiteral("/Init")))
                    || companion.size() != 2 + companion.contains(QStringLiteral("waveforms"))
                        + companion.contains(QStringLiteral("metadata"))
                    || !companion.value(QStringLiteral("variables")).isArray())
                    return rejectPair(QStringLiteral("Invalid Init publication in '%1'").arg(directory));
                name = QStringLiteral("Init");
                const auto variables = companion.value(QStringLiteral("variables")).toArray();
                if (variables.size() != world.variableCount)
                    return rejectPair(QStringLiteral("Init companion does not match its Variable definitions"));
                QSet<QString> variableNames;
                std::set<harkenstudio::ObjectId> variableIds;
                for (const auto& value : variables) {
                    const auto variable = value.toObject();
                    const QString variableName = variable.value(QStringLiteral("name")).toString();
                    const QString guid = variable.value(QStringLiteral("guid")).toString();
                    const auto id = runtimeObjectId(QUuid(guid));
                    if (variable.size() != 3 || !NameValidator::validateItemName(variableName, ProjectItem::Type::Variable)
                        || harkenstudio::IsNull(id) || objectIdString(id) != guid
                        || variableNames.contains(variableName) || !variableIds.insert(id).second
                        || !variable.value(QStringLiteral("labels")).isArray()
                        || std::none_of(world.variables, world.variables + world.variableCount,
                            [&](const auto& definition) { return definition.variableId == id; }))
                        return rejectPair(QStringLiteral("Invalid Variable in Init companion"));
                    variableNames.insert(variableName);
                    const auto overlay = std::find_if(image.variablePolicyOverlays.begin(), image.variablePolicyOverlays.end(),
                        [&](const auto& row) { return row.variableId == id; });
                    const auto labels = variable.value(QStringLiteral("labels")).toArray();
                    if (overlay == image.variablePolicyOverlays.end())
                        return rejectPair(QStringLiteral("Init companion does not match its Label definitions"));
                    QSet<QString> labelNames;
                    std::set<harkenstudio::ObjectId> labelIds;
                    for (const auto& labelValue : labels) {
                        const auto label = labelValue.toObject();
                        const QString labelName = label.value(QStringLiteral("name")).toString();
                        const QString labelGuid = label.value(QStringLiteral("guid")).toString();
                        const auto labelId = runtimeObjectId(QUuid(labelGuid));
                        if (label.size() != 2 || labelName.isEmpty() || harkenstudio::IsNull(labelId)
                            || objectIdString(labelId) != labelGuid || labelNames.contains(labelName)
                            || !labelIds.insert(labelId).second
                            || std::none_of(overlay->labels.begin(), overlay->labels.end(),
                                [&](const auto& row) { return row.labelId == labelId; }))
                            return rejectPair(QStringLiteral("Invalid Label in Init companion"));
                        labelNames.insert(labelName);
                    }
                }
            } else {
                name = companion.value(QStringLiteral("name")).toString();
                const QString guid = companion.value(QStringLiteral("guid")).toString();
                const auto id = runtimeObjectId(QUuid(guid));
                const QString scope = info.path() == QStringLiteral(".") ? QString() : info.path();
                if (image.packKind != harkenstudiobuild::kPackKindCanonicalContent
                    || companion.size() != 3 + companion.contains(QStringLiteral("waveforms"))
                        + companion.contains(QStringLiteral("metadata"))
                    || !NameValidator::validateItemName(name, ProjectItem::Type::Action)
                    || harkenstudio::IsNull(id) || objectIdString(id) != guid || world.actionCount == 0
                    || std::none_of(world.actions, world.actions + world.actionCount,
                        [&](const auto& action) { return action.actionStableId == id; })
                    || (!platformId.isEmpty() && image.packStableId != fnvName(PackBuildPlanning::relativePackPath(
                        platformId, scope, PackBuildPlanning::stableArtifactKey(id))))
                    || (requireComplete && selected && (names.contains(scope + QLatin1Char('/') + name)
                    || actionIds.contains(scope + QLatin1Char('/') + guid)
                    || (!scope.isEmpty() && (names.contains(QLatin1Char('/') + name)
                        || actionIds.contains(QLatin1Char('/') + guid)))
                    || (scope.isEmpty() && (localizedNames.contains(name) || localizedActionIds.contains(guid))))))
                    return rejectPair(QStringLiteral("Invalid Action companion '%1'").arg(companionPath));
            }
            if (companion.contains(QStringLiteral("metadata"))) {
                const auto metadata = companion.value(QStringLiteral("metadata")).toObject();
                const auto language = info.path() == QStringLiteral(".") ? QString() : info.path();
                if (metadata.value(QStringLiteral("packStableId")).toString() != QString::number(image.packStableId)
                    || metadata.value(QStringLiteral("language")).toString() != language)
                    return rejectPair(QStringLiteral("Pack metadata identity does not match '%1'").arg(companionPath));
                auto expectedObjects = packMetadataObjectIds(image, world);
                for (const auto& value : metadata.value(QStringLiteral("objects")).toArray()) {
                    const auto object = value.toObject();
                    const auto id = runtimeObjectId(QUuid(object.value(QStringLiteral("guid")).toString()));
                    if (expectedObjects.erase(id) != 1
                        || (!image.hasBusDefinitionSnapshot
                            && object.value(QStringLiteral("type")).toString() == QStringLiteral("Bus")))
                        return rejectPair(QStringLiteral("Invalid metadata object in '%1'").arg(companionPath));
                }
                if (!expectedObjects.empty())
                    return rejectPair(QStringLiteral("Pack metadata does not cover the dependencies in '%1'").arg(companionPath));
            }
            std::set<harkenstudio::ObjectId> waveformSounds;
            for (const auto& value : companion.value(QStringLiteral("waveforms")).toArray()) {
                const auto waveform = value.toObject();
                const auto soundId = runtimeObjectId(QUuid(waveform.value(QStringLiteral("soundGuid")).toString()));
                const QString file = waveform.value(QStringLiteral("file")).toString();
                QString filePath = file;
#if defined(Q_OS_WIN)
                if (filePath.size() >= 3 && filePath.at(0).toLower() >= QLatin1Char('a')
                    && filePath.at(0).toLower() <= QLatin1Char('z')
                    && filePath.at(1) == QLatin1Char(':') && filePath.at(2) == QLatin1Char('/'))
                    filePath.remove(0, 2);
#endif
                if (!waveformSounds.insert(soundId).second
                    || std::none_of(image.media.begin(), image.media.end(),
                        [&](const auto& media) { return media.soundId == soundId; })
                    || file.contains(QLatin1Char('\\')) || PathUtils::hasUnsafeCharacters(filePath)
                    || !file.endsWith(QStringLiteral(".vkd"))
                    || QDir::cleanPath(file) != file
                    || waveform.value(QStringLiteral("trimEndFrame")).toInteger()
                        <= waveform.value(QStringLiteral("trimStartFrame")).toInteger())
                    return rejectPair(QStringLiteral("Invalid waveform index in '%1'").arg(companionPath));
            }
            if (companion.contains(QStringLiteral("waveforms")) && waveformSounds.size() != image.media.size())
                return rejectPair(QStringLiteral("Waveform index does not cover the Pack media in '%1'").arg(companionPath));
            QJsonArray dependencies;
            for (const auto& edge : image.dependencyEdges)
                dependencies.append(QString::number(edge.prerequisitePackStableId));
            row = QJsonObject{{QStringLiteral("name"), name},
                {QStringLiteral("path"), path}, {QStringLiteral("packStableId"), QString::number(image.packStableId)},
                {QStringLiteral("packKind"), static_cast<int>(image.packKind)},
                {QStringLiteral("contentHash"), QString::number(hash)},
                {QStringLiteral("contentRevision"), static_cast<qint64>(image.requirement.packContentRevision)},
                {QStringLiteral("dependencies"), dependencies},
                {QStringLiteral("language"), info.path() == QStringLiteral(".") ? QString() : info.path()},
                {QStringLiteral("projectId"), objectIdString(pairProject)},
                {QStringLiteral("platformId"), platformId}, {QStringLiteral("companion"), companion}};
            return true;
        };
        if (!inspectPair()) {
            if (requireComplete && selected) return reject(pairError);
            continue;
        }
        if (harkenstudio::IsNull(expectedProject) && selected) expectedProject = pairProject;
        if (selected && image.packKind == harkenstudiobuild::kPackKindInit) {
            initId = image.packStableId;
            selectedInit = true;
        } else if (selected) {
            const QString name = row.value(QStringLiteral("name")).toString();
            const QString guid = row.value(QStringLiteral("companion")).toObject().value(QStringLiteral("guid")).toString();
            const QString scope = row.value(QStringLiteral("language")).toString();
            names.insert(scope + QLatin1Char('/') + name);
            actionIds.insert(scope + QLatin1Char('/') + guid);
            if (!scope.isEmpty()) {
                localizedNames.insert(name);
                localizedActionIds.insert(guid);
            }
        }
        managedFiles->append(path);
        managedFiles->append(companionPath);
        if (!selected) continue;
        selectedPaths.insert(path);
        ids.insert(image.packStableId);
        kinds.emplace(image.packStableId, image.packKind);
        dependencyEdges.insert(dependencyEdges.end(), image.dependencyEdges.begin(), image.dependencyEdges.end());
        packs->append(row);
    }
    if (requireComplete && !selectedInit) return reject(QStringLiteral("Pack publication is missing Init.bytes and Init.json; re-export all Packs with the current Studio version"));
    if (requireComplete && initId != 0) {
        for (const auto& value : *packs) {
            const auto row = value.toObject();
            const auto id = row.value(QStringLiteral("packStableId")).toString().toULongLong();
            const auto dependencies = row.value(QStringLiteral("dependencies")).toArray();
            if (id != initId && !dependencies.contains(QString::number(initId)))
                return reject(QStringLiteral("Pack does not depend on this publication's Init"));
            for (const auto& dependency : dependencies)
                if (!ids.contains(dependency.toString().toULongLong()))
                    return reject(QStringLiteral("Pack publication is missing a dependency"));
        }
    } else if (requireComplete && !packs->isEmpty()) {
        return reject(QStringLiteral("Pack publication contains Actions without Init"));
    }
    if (requireComplete) {
        for (const auto& path : expectedPackPaths)
            if (!selectedPaths.contains(path))
                return reject(QStringLiteral("Pack publication is missing requested Pack: %1").arg(path));
        for (const auto& relative : files) {
            if (exportCanceled(cancelCheck, error)) return false;
            if (!relative.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)
                || managedFiles->contains(relative)) continue;
            const QFileInfo info(relative);
            const auto binary = QDir::cleanPath(QDir(info.path()).filePath(info.completeBaseName() + QStringLiteral(".bytes")));
            if (!expectedPackPaths.isEmpty() && !expectedPackPaths.contains(binary)) continue;
            QByteArray bytes;
            if (!readAll(dir.filePath(relative), &bytes, &detail)) return reject(detail);
            const auto document = QJsonDocument::fromJson(bytes);
            if (!document.isObject() || !ProjectSchemaValidator::validatePackManifest(
                    HARKEN_PACK_SCHEMA_VERSION, document.object(), relative).isEmpty()) continue;
            if (!document.object().contains(QStringLiteral("guid"))
                && !document.object().contains(QStringLiteral("variables"))) continue;
            return reject(QStringLiteral("Pack companion has no valid matching binary: %1").arg(relative));
        }
        for (const auto& edge : dependencyEdges) {
            const auto prerequisite = kinds.find(edge.prerequisitePackStableId);
            if (prerequisite == kinds.end()
                || (edge.edgeKind == harkenstudiobuild::kPackDependencyEdgeKindInit)
                    != (prerequisite->second == harkenstudiobuild::kPackKindInit))
                return reject(QStringLiteral("Pack publication contains an incompatible dependency"));
        }
    }
    if (initId != 0) {
        for (const auto& path : binaries) {
            if (exportCanceled(cancelCheck, error)) return false;
            if (managedFiles->contains(path)) continue;
            const QFileInfo info(path);
            const auto companion = QDir::cleanPath(QDir(info.path()).filePath(info.completeBaseName() + QStringLiteral(".json")));
            if (files.contains(companion)) continue;
            QByteArray bytes;
            harkenstudiobuild::PackCanonicalImage image;
            harkenstudiobuild::CurrentWorldBodyImage body;
            if (!readAll(dir.filePath(path), &bytes, &detail)
                || harkenstudiobuild::ParsePackCanonicalImage(reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                    bytes.size(), &image) != harkenstudiobuild::kPackOk
                || !body.Decode(image.partitionBody.data(), image.partitionBody.size(),
                    harkenstudiobuild::kCanonicalDefinitionPartitionBodyVintage)
                || body.ProductionView()->identity.projectId != expectedProject) continue;
            if ((image.packKind == harkenstudiobuild::kPackKindCanonicalContent
                    && harkenstudiobuild::ValidateCanonicalInitDependency(image.dependencyEdges, initId)
                        == harkenstudiobuild::kPackOk)
                || (image.packKind == harkenstudiobuild::kPackKindInit && image.packStableId == initId))
                managedFiles->append(path);
        }
    }
    managedFiles->removeDuplicates();
    return !exportCanceled(cancelCheck, error);
}

ProjectPackExporter::ExportResult ExportCanonicalPackSet(
    ProjectPackExporter& exporter, const CanonicalPackSetExportRequest& request,
    std::vector<ProjectPackExporter::PackBuildRecord>* outRecords)
{
    ProjectPackExporter::ExportResult result;
    ProjectData& project = const_cast<ProjectData&>(*request.project);
    if (PathUtils::isProtectedPackOutputPath(request.outputDir, project.projectPath)
        && (request.options.selfCleaning
            || !PathUtils::isProjectRootDirectory(request.outputDir, project.projectPath))) {
        result.error = PathUtils::packBuildOutputLocationError();
        return result;
    }
    if (!validateProjectObjectIdentities(project, &result.error)) {
        if (result.error.isEmpty())
            result.error = QStringLiteral("Action identity registration failed");
        return result;
    }
    for (const auto& diagnostic : request.plan->diagnostics) {
        if (diagnostic.code == ActionClosureDiagnosticCode::InvalidPackName
            && request.includedActionIds.count(diagnostic.actionStableId) != 0) {
            result.error = diagnostic.message;
            return result;
        }
    }
    const BindingRuntimeLowering lowering =
        BindingRuntimeLoweringSemantics::lowerProject(project);
    if (lowering.failed) {
        result.error = lowering.error;
        return result;
    }
    std::vector<Bridge::ProjectInputDiagnostic> busDiagnostics;
    const auto busProjection = Bridge::compileStudioBusProjectionForPlatform(
        project, QUuid::fromString(request.options.platform), &busDiagnostics);
    if (!busDiagnostics.empty() || busProjection.fingerprint == 0) {
        result.error = busDiagnostics.empty()
            ? QStringLiteral("Bus projection is unavailable for target platform %1")
                  .arg(request.options.platform)
            : QStringLiteral("Bus projection on %1: %2 (%3)")
                  .arg(request.options.platform, busDiagnostics.front().message,
                       busDiagnostics.front().field);
        return result;
    }
    const auto busSnapshotPayload = harkenstudiobuild::VolumeBusSnapshotPayload(busProjection);
    harkenstudiobuild::PackBusDefinitionSnapshotHeaderV1 busSnapshotHeader;
    if (busSnapshotPayload.empty() ||
        !harkenstudiobuild::ParsePackBusDefinitionSnapshotHeaderV2(
            busSnapshotPayload.data(), busSnapshotPayload.size(), &busSnapshotHeader)) {
        result.error = QStringLiteral("Bus snapshot cannot represent target platform %1")
                           .arg(request.options.platform);
        return result;
    }
    const auto busTriple = harkenstudiobuild::SnapshotBusTriple(busSnapshotHeader);
    QDir liveDir(request.outputDir);
    if (!liveDir.exists())
        liveDir.mkpath(".");
    QLockFile outputLock(liveDir.filePath(QStringLiteral(".harken-pack-platform.lock")));
    outputLock.setStaleLockTime(0);
    if (!outputLock.tryLock()) {
        result.error = QStringLiteral("Platform output is being built by another request: %1").arg(request.outputDir);
        return result;
    }
    if (!recoverInterruptedPublish(liveDir, &result.error))
        return result;
    QStringList writePaths;
    if (!collectPlatformWritePaths(project, liveDir, *request.plan,
            request.options, &writePaths, &result.error)
        || !validateWriteAccess(writePaths, &result.error, request.options.cancelCheck)) {
        result.canceled = request.options.cancelCheck && request.options.cancelCheck();
        return result;
    }
    QStringList liveNames, allPackNames;
    if (!scanOutputFiles(liveDir, &liveNames, &allPackNames, &result.error, false,
            request.options.cancelCheck)) {
        result.canceled = request.options.cancelCheck && request.options.cancelCheck();
        return result;
    }
    const QDir resourceCacheDir(QDir(PathUtils::getDotCacheDir(QFileInfo(project.projectPath).absolutePath()))
        .filePath(QStringLiteral("pack-inputs")));
    QMap<QString, QJsonObject> previousPacks;
    QHash<qulonglong, QJsonObject> publishedPacks;
    QJsonArray previousPublication;
    QStringList previousManaged;
    if (!ReadPackPublication(request.outputDir, project.projectId, request.options.platform,
            &previousPublication, &previousManaged, &result.error, false, {},
            !request.options.updateSelectedOnly, request.options.cancelCheck)) {
        result.canceled = request.options.cancelCheck && request.options.cancelCheck();
        return result;
    }
    for (const auto& value : previousPublication) {
        const auto row = value.toObject();
        previousPacks.insert(row.value(QStringLiteral("path")).toString(), row);
        publishedPacks.insert(row.value(QStringLiteral("packStableId")).toString().toULongLong(), row);
    }
    QStringList previousPackNames;
    QStringList previousCompanionNames;
    for (const auto& name : previousManaged)
        if (allPackNames.contains(name)) previousPackNames.append(name);
    for (const auto& value : previousPublication) {
        const QFileInfo file(value.toObject().value(QStringLiteral("path")).toString());
        previousCompanionNames.append(QDir::cleanPath(QDir(file.path()).filePath(
            file.completeBaseName() + QStringLiteral(".json"))));
    }

    const QString stagingDir = liveDir.filePath(
        QString::fromLatin1(kStagingPrefix) +
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!QDir().mkpath(stagingDir)) {
        result.error = QStringLiteral("Cannot create staging directory");
        return result;
    }
    QString nextDir;
    QStringList newWaveformFiles;
    std::unique_ptr<QLockFile> waveformLock;
    std::vector<std::unique_ptr<QLockFile>> waveformCleanupLocks;
    bool keepPreviousOnFail = false;
    const std::size_t firstRecord = outRecords ? outRecords->size() : 0;

    auto fail = [&](QString error) {
        QDir(stagingDir).removeRecursively();
        if (!nextDir.isEmpty() && !keepPreviousOnFail)
            QDir(nextDir).removeRecursively();
        removeMatchingDirs(liveDir, QString::fromLatin1(kStagingPrefix));

        if (!keepPreviousOnFail)
            QFile::remove(liveDir.filePath(QString::fromLatin1(kPublishingMarker)));
        if (!keepPreviousOnFail)
            for (const auto& path : newWaveformFiles) QFile::remove(path);
        result.success = false;
        result.canceled = result.canceled || error == QStringLiteral("Export canceled")
            || (request.options.cancelCheck && request.options.cancelCheck());
        result.error = std::move(error);
        if (outRecords) {
            for (std::size_t i = firstRecord; i < outRecords->size(); ++i) {
                auto& record = (*outRecords)[i];
                record.success = false;
                record.error = result.error;
            }
        }
        return result;
    };

    ProjectPackExporter::ExportOptions stagingOpts = request.options;
    const std::uint64_t initId = fnvName(request.options.platform + QStringLiteral("/Init"));

    std::vector<PackMeta> packs;
    {
        PackMeta init;
        init.name = QStringLiteral("Init");
        init.unitKey = QStringLiteral("Init");
        init.fileName = QStringLiteral("Init") + Bridge::packArtifactExtension();
        init.platform = request.options.platform;
        init.packStableId = initId;
        init.packKind = harkenstudiobuild::kPackKindInit;
        packs.push_back(std::move(init));
    }
    for (const PackBuildUnit& planned : request.plan->units) {
        PackMeta content;
        content.name = planned.name;
        content.unitKey = planned.grouped ? QStringLiteral("g:") + planned.artifactKey
            : QStringLiteral("a:") + objectIdString(planned.actionStableIds.front());
        content.fileName = PackBuildPlanning::packFileName(
            planned.variant.language, planned.name, Bridge::packArtifactExtension());
        content.language = planned.variant.language;
        content.platform = planned.variant.platform;
        content.grouped = planned.grouped;
        for (harkenstudio::ObjectId actionId : planned.actionStableIds) {
            if (request.includedActionIds.count(actionId) == 0)
                continue;
            const ProjectItem* action = findAction(project, actionId);
            if (action && action->comp<VAction>() && !action->comp<VAction>()->steps.empty())
                content.actionStableIds.push_back(actionId);
        }
        if (content.actionStableIds.empty())
            continue;
        content.packStableId = fnvName(PackBuildPlanning::relativePackPath(
            planned.variant.platform, planned.variant.language, planned.artifactKey));
        content.packKind = harkenstudiobuild::kPackKindCanonicalContent;
        content.dependencies = {initId};
        packs.push_back(std::move(content));
    }
    for (PackMeta& content : packs) {
        for (const PackBuildVariant& variant : request.plan->variants) {
            if (std::find(content.actionStableIds.begin(), content.actionStableIds.end(),
                          variant.actionStableId) == content.actionStableIds.end()
                || variant.variant.platform != content.platform
                || variant.variant.language != content.language)
                continue;
            for (const ActionClosureDiagnostic& diagnostic : variant.closure.diagnostics) {
                if (diagnostic.code != ActionClosureDiagnosticCode::AmbiguousSourceSelection)
                    continue;
                const ProjectItem* item = findItemByStableId(project, diagnostic.objectStableId);
                const auto* sound = item ? item->comp<VSound>() : nullptr;
                if (sound && sound->distinguishLanguage)
                    return fail(QStringLiteral("Pack '%1' on %2/%3: %4")
                        .arg(content.name, content.platform, content.language, diagnostic.message));
            }
        }
    }

    std::set<harkenstudio::ObjectId> previousInitLayerIds;
    for (auto& pack : packs) {
        pack.previousRow = previousPacks.value(pack.fileName);
        if (pack.previousRow.isEmpty() && liveNames.contains(pack.fileName))
            pack.previousRow.insert(QStringLiteral("path"), pack.fileName);
        QByteArray bytes;
        harkenstudiobuild::CurrentWorldBodyImage body;
        pack.previousValid = readPreviousPack(liveDir, &pack, &bytes, &body);
        if (pack.previousValid) {
            pack.previousCapacity = body.ProductionView()->capacity;
            if (request.options.updateSelectedOnly && pack.packKind == harkenstudiobuild::kPackKindInit) {
                const auto* world = body.ProductionView();
                for (std::uint32_t i = 0; i < world->playablePropertyCount; ++i) {
                    const auto& property = world->playableProperties[i];
                    if (property.contentPropertyKind == harkenstudiobuild::playableproperties::ContentPropertyKindV1::Layer)
                        previousInitLayerIds.insert(property.stableId);
                }
            }
            QFile cache(resourceCacheDir.filePath(QString::fromLatin1(
                QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())));
            if (cache.open(QIODevice::ReadOnly))
                pack.previousResourceFingerprint = QString::fromLatin1(cache.readAll());
        }
    }

    {
        std::unordered_set<std::uint64_t> seenPackIds;
        std::unordered_set<QString> seenLocators;
        for (const PackMeta& pack : packs) {
            if (!seenPackIds.insert(pack.packStableId).second)
                return fail(QStringLiteral("Duplicate packStableId %1")
                                .arg(pack.packStableId));
            if (!seenLocators.insert(pack.fileName.toCaseFolded()).second)
                return fail(QStringLiteral("Pack file name collision at '%1' (case-insensitive); rename the conflicting Actions.")
                                .arg(pack.fileName));
            const QFileInfo file(pack.fileName);
            const auto companion = QDir::cleanPath(QDir(file.path()).filePath(file.completeBaseName() + QStringLiteral(".json")));
            for (const auto& path : {pack.fileName, companion})
                if (QFileInfo::exists(liveDir.filePath(path))
                    && !containsPublicationFile(liveDir, liveNames, path))
                    return fail(QStringLiteral("Pack output conflicts with an output directory: %1").arg(path));
        }
    }
    {
        std::unordered_map<std::uint64_t, const PackMeta*> packsById;
        for (const PackMeta& pack : packs)
            packsById.emplace(pack.packStableId, &pack);
        std::set<std::uint64_t> visited;
        std::set<std::uint64_t> active;
        std::function<bool(std::uint64_t)> visit = [&](std::uint64_t id) {
            if (active.count(id) != 0)
                return false;
            if (visited.count(id) != 0)
                return true;
            active.insert(id);
            for (std::uint64_t dependency : packsById.at(id)->dependencies)
                if (!visit(dependency))
                    return false;
            active.erase(id);
            visited.insert(id);
            return true;
        };
        for (const PackMeta& pack : packs)
            if (!visit(pack.packStableId))
                return fail(QStringLiteral("Pack dependency cycle involving '%1'; revise the Action dependencies")
                                .arg(pack.name));
    }

    auto recordUnit = [&](const PackMeta& pack, const ProjectPackExporter::ExportResult& unit) {
        if (!outRecords || pack.retained) return;
        ProjectPackExporter::PackBuildRecord record;
        record.packName = pack.name;
        record.unitKey = pack.unitKey;
        record.source = pack.actionStableIds.empty()
            ? ProjectPackExporter::PackBuildRecord::Source::Init
            : pack.grouped ? ProjectPackExporter::PackBuildRecord::Source::Group
                           : ProjectPackExporter::PackBuildRecord::Source::Action;
        record.success = unit.success;
        record.skipped = pack.skipped;
        record.outputPath = liveDir.filePath(pack.fileName);
        record.fileSize = unit.fileSize;
        record.platform = request.options.platform;
        const auto& definition = *ProjectPlatformSemantics::find(project.platforms, QUuid(record.platform));
        record.platformName = definition.name;
        record.basePlatform = definition.basePlatform;
        record.platformDefinition = definition.toJson();
        record.warnings = unit.warnings;
        record.error = unit.error;
        record.elapsedMs = pack.elapsedNanoseconds / 1000000;
        record.language = pack.language;
        if (!pack.grouped && !pack.actionStableIds.empty()) {
            const ProjectItem* action = findAction(project, pack.actionStableIds.front());
            if (action) record.objectID = action->objectID;
        }
        outRecords->push_back(std::move(record));
    };
    const auto reportProgress = [&](const PackMeta& pack, int percent, PackGenerationStage stage) {
        if (!request.options.progressCallback) return;
        request.options.progressCallback({request.options.platform, pack.language,
            pack.unitKey, percent, stage});
    };

    auto prepareUnit = [&](PackMeta& meta) -> ProjectPackExporter::ExportResult {
        const QString path = QDir(stagingDir).filePath(meta.fileName);
        QDir().mkpath(QFileInfo(path).absolutePath());
        ProjectPackExporter::ExportResult unit;
        QStringList resourcePaths;
        QSet<QString> reusedPaths;
        auto resourceOptions = stagingOpts;
        resourceOptions.forceReexport = request.options.forceReexport && !meta.retained;
        resourceOptions.resourceReuse = ProjectPackExporterInternal::PackResourceReuse{
            meta.previousValid && !meta.grouped
                ? liveDir.filePath(meta.previousRow.value(QStringLiteral("path")).toString()) : QString(),
            meta.previousResourceFingerprint};
        if (meta.packKind == harkenstudiobuild::kPackKindInit) {
            const QString residentPath = path + QStringLiteral(".resident");
            unit = exporter.ExportInitPack(project, residentPath, resourceOptions);
            if (!unit.success)
                return unit;
            resourcePaths = {unit.outputPath};
            if (unit.reusedResources) reusedPaths.insert(unit.outputPath);
            meta.resourceFingerprint = unit.resourceFingerprint;
        } else {
            ProjectPackExporter::ExportOptions contentOpts = resourceOptions;
            contentOpts.language = meta.language;
            for (harkenstudio::ObjectId actionId : meta.actionStableIds) {
                const ProjectItem* action = findAction(project, actionId);
                if (!action) {
                    unit.error = QStringLiteral("Missing Action %1 in Pack '%2'")
                                     .arg(VNX::objectIdString(actionId), meta.name);
                    return unit;
                }
                const QString mediaPath = QDir(QFileInfo(path).absolutePath()).filePath(
                    VNX::objectIdString(actionId) + QStringLiteral(".media"));
                auto member = exporter.ExportActionPack(
                    project, action->name, mediaPath, contentOpts);
                if (!member.success)
                    return member;
                for (const auto& [soundId, revision] : member.controlSoundContentRevisions) {
                    const auto [found, inserted] = meta.soundContentRevisions.emplace(soundId, revision);
                    if (!inserted && found->second != revision) {
                        unit.error = QStringLiteral("Conflicting control media revision for Sound %1")
                            .arg(VNX::objectIdString(soundId));
                        return unit;
                    }
                }
                unit.warnings += member.warnings;
                unit.objectCount += member.objectCount;
                unit.actionCount += member.actionCount;
                unit.mediaCacheHits += member.mediaCacheHits;
                unit.mediaConversions += member.mediaConversions;
                unit.mediaFallbacks += member.mediaFallbacks;
                resourcePaths.append(member.outputPath);
                if (member.reusedResources) reusedPaths.insert(member.outputPath);
                meta.resourceFingerprint = member.resourceFingerprint;
            }
            unit.success = true;
        }
        for (const QString& resourcePath : resourcePaths) {
            if (exportCanceled(request.options.cancelCheck, &unit.error)) {
                unit.success = false;
                unit.canceled = true;
                return unit;
            }
            if (!collectEntries(resourcePath, &meta.resourceEntries, &unit.error, reusedPaths.contains(resourcePath))) {
                unit.success = false;
                return unit;
            }
            if (!reusedPaths.contains(resourcePath)) QFile::remove(resourcePath);
        }
        for (const auto& entry : meta.resourceEntries) {
            if (exportCanceled(request.options.cancelCheck, &unit.error)) {
                unit.success = false;
                unit.canceled = true;
                return unit;
            }
            if (entry.kind == harkenstudiobuild::kPackEntryKindContainer) {
                unit.success = false;
                unit.error = QStringLiteral(
                    "Pack '%1' emitted pack entry kind 9; CurrentWorld is the "
                    "playback program and v2 Container entries are refused")
                                 .arg(meta.fileName);
                return unit;
            }
            if (entry.kind == harkenstudiobuild::kPackEntryKindVoiceRecipe) {
                unit.success = false;
                unit.error = QStringLiteral(
                    "Pack '%1' emitted pack entry kind 11; CurrentWorld is the "
                    "playback program and v2 VoiceRecipe entries are refused")
                                 .arg(meta.fileName);
                return unit;
            }
            if (entry.kind != harkenstudiobuild::kPackEntryKindMedia)
                continue;
            unit.success = false;
            unit.error = QStringLiteral("Pack '%1' contains invalid PCM media %2")
                .arg(meta.fileName).arg(VNX::objectIdString(entry.id));
            if (entry.payload.size() < harkenstudiobuild::kPackV2MediaPayloadBytes)
                return unit;
            const auto media = harkenstudiobuild::ParsePackTocEntry(entry.payload.data());
            const auto pcmBytes = entry.payload.size() - harkenstudiobuild::kPackV2MediaPayloadBytes;
            if (media.sampleFormat != harkenstudiobuild::kPackSampleFormatF32Planar
                || media.blobBytes != pcmBytes || pcmBytes % sizeof(float) != 0
                || media.channelCount == 0 || media.frameCount == 0)
                return unit;
            const auto sampleCount = pcmBytes / sizeof(float);
            if (sampleCount % media.channelCount != 0
                || sampleCount / media.channelCount != media.frameCount)
                return unit;
            std::vector<float> planar(sampleCount);
            std::memcpy(planar.data(),
                        entry.payload.data() + harkenstudiobuild::kPackV2MediaPayloadBytes, pcmBytes);
            std::vector<const float*> channels(media.channelCount);
            for (std::uint16_t channel = 0; channel < media.channelCount; ++channel)
                channels[channel] = planar.data() + channel * media.frameCount;
            std::uint64_t revision = 0;
            const bool loopable =
                (media.flags & harkenstudiobuild::kPackFlagLoopable) != 0;
            if (!harkenstudiobuild::ComputeMediaPcmContentRevisionV1(
                    media.sampleRate, media.channelCount, media.frameCount,
                    channels.data(), 1, &revision,
                    loopable ? media.loopStartFrame : 0,
                    loopable ? media.loopEndFrame : 0))
                return unit;
            const auto [found, inserted] = meta.soundContentRevisions.emplace(media.soundId, revision);
            if (!inserted && found->second != revision) {
                unit.error = QStringLiteral("Control and packed media revisions disagree for Sound %1")
                    .arg(VNX::objectIdString(media.soundId));
                return unit;
            }
            ++meta.ownedResources;
            unit.success = true;
            unit.error.clear();
        }
        unit.mediaCount = static_cast<int>(meta.ownedResources);
        return unit;
    };
    for (PackMeta& pack : packs) {
        reportProgress(pack, 5, PackGenerationStage::Preparing);
        if (exportCanceled(request.options.cancelCheck, &result.error)) return fail(result.error);
        QElapsedTimer timer;
        timer.start();
        pack.mediaResult = prepareUnit(pack);
        pack.elapsedNanoseconds += timer.nsecsElapsed();
        result.mediaCacheHits += pack.mediaResult.mediaCacheHits;
        result.mediaConversions += pack.mediaResult.mediaConversions;
        result.mediaFallbacks += pack.mediaResult.mediaFallbacks;
        if (!pack.mediaResult.success) {
            recordUnit(pack, pack.mediaResult);
            return fail(pack.mediaResult.error);
        }
    }

    for (const auto& pack : packs)
        reportProgress(pack, 40, PackGenerationStage::Compiling);
    if (exportCanceled(request.options.cancelCheck, &result.error)) return fail(result.error);
    QString compileError;
    std::set<harkenstudio::ObjectId> requiredDefinitions;
    for (const auto& variant : request.plan->variants)
        for (const auto* members : {&variant.closure.objects, &variant.closure.controlObjects})
            for (const auto& member : *members) requiredDefinitions.insert(member.stableId);
    std::set<harkenstudio::ObjectId> sharedLayerIds;
    std::function<bool(const ProjectItem*)> collectLayers = [&](const ProjectItem* item) {
        if (!item) return false;
        bool required = requiredDefinitions.count(item->runtimeId()) != 0
            || previousInitLayerIds.count(item->runtimeId()) != 0;
        for (const auto& child : item->children)
            required = collectLayers(child.get()) || required;
        if (required && item->type == ProjectItem::Type::Layer)
            sharedLayerIds.insert(item->runtimeId());
        return required;
    };
    for (const auto* binding : project.allFileBindings()) collectLayers(binding->rootItem.get());
    QStringList languages;
    for (const auto& language : ProjectLanguageSemantics::buildLanguages(project))
        if (std::any_of(packs.begin(), packs.end(), [&](const auto& pack) { return pack.language == language; }))
            languages.append(language);
    if (languages.isEmpty()) languages.append(QString());
    const QString preferredLanguage = stagingOpts.language.trimmed().isEmpty()
        ? project.defaultLanguage : stagingOpts.language.trimmed();
    const QString selectedLanguage = languages.contains(preferredLanguage)
        ? preferredLanguage : languages.front();
    std::map<harkenstudio::ObjectId, std::uint32_t> newPropertyOrders, newContainerOrders;
    std::set<std::uint32_t> reservedPropertyOrders, reservedContainerOrders;
    std::vector<PublicationAuthoredOrders> publicationOrders;
    std::set<harkenstudio::ObjectId> neutralDefinitionIds = sharedLayerIds;
    if (request.options.updateSelectedOnly) {
        for (const auto& variant : request.plan->variants) {
            if (!variant.variant.language.isEmpty()) continue;
            for (const auto* members : {&variant.closure.objects, &variant.closure.controlObjects})
                for (const auto& member : *members) neutralDefinitionIds.insert(member.stableId);
        }
        publicationOrders = readPublicationAuthoredOrders(liveDir, previousPublication,
            &reservedPropertyOrders, &reservedContainerOrders);
    }
    for (const QString& language : languages) {
        if (exportCanceled(request.options.cancelCheck, &result.error)) return fail(result.error);
        std::set<harkenstudio::ObjectId> packedRootActionIds;
        std::set<harkenstudio::ObjectId> packedActionIds;
        std::set<harkenstudio::ObjectId> packedDefinitionIds = sharedLayerIds;
        for (const PackMeta& pack : packs) {
            if (!pack.language.isEmpty() && pack.language != language) continue;
            packedRootActionIds.insert(pack.actionStableIds.begin(), pack.actionStableIds.end());
        }
        for (const auto& variant : request.plan->variants) {
            if ((!variant.variant.language.isEmpty() && variant.variant.language != language)
                || packedRootActionIds.count(variant.actionStableId) == 0)
                continue;
            for (const auto& member : variant.closure.actions) packedActionIds.insert(member.stableId);
            for (const auto& member : variant.closure.objects) packedDefinitionIds.insert(member.stableId);
            for (const auto& member : variant.closure.controlObjects) packedDefinitionIds.insert(member.stableId);
        }
        CurrentWorldBuildOptions buildOptions;
        buildOptions.includedActionIds = &packedActionIds;
        buildOptions.includedDefinitionIds = &packedDefinitionIds;
        buildOptions.skipEmptyActionSteps = true;
        buildOptions.language = language;
        auto worldRequest = buildCurrentWorldSyncRequest(
            project, QUuid::fromString(request.options.platform),
            &compileError, nullptr, &buildOptions);
        if (!worldRequest)
            return fail(compileError);
        if (request.options.updateSelectedOnly) {
            std::map<harkenstudio::ObjectId, std::uint32_t> containerOrders;
            if (!mergePublicationAuthoredOrders(publicationOrders, language, neutralDefinitionIds,
                    &worldRequest->propertyAuthoredOrders, &containerOrders,
                    &compileError))
                return fail(compileError);
            worldRequest->propertyAuthoredOrders.insert(newPropertyOrders.begin(), newPropertyOrders.end());
            worldRequest->reservedPropertyAuthoredOrders = reservedPropertyOrders;
            containerOrders.insert(newContainerOrders.begin(), newContainerOrders.end());
            std::uint64_t nextOrder = 0;
            for (auto& container : worldRequest->containers) {
                const auto previous = containerOrders.find(container.containerStableId);
                if (previous != containerOrders.end()) {
                    container.authoredStableOrder = previous->second;
                    continue;
                }
                while (nextOrder <= UINT32_MAX
                       && reservedContainerOrders.count(static_cast<std::uint32_t>(nextOrder)) != 0)
                    ++nextOrder;
                if (nextOrder > UINT32_MAX)
                    return fail(QStringLiteral("Published container authored orders are exhausted"));
                container.authoredStableOrder = static_cast<std::uint32_t>(nextOrder++);
                reservedContainerOrders.insert(container.authoredStableOrder);
                newContainerOrders.emplace(container.containerStableId, container.authoredStableOrder);
            }
        }
        if (exportCanceled(request.options.cancelCheck, &result.error)) return fail(result.error);
        for (const PackMeta& pack : packs) {
            if (!pack.language.isEmpty() && pack.language != language)
                continue;
            for (const auto& [soundId, revision] : pack.soundContentRevisions) {
                const auto [found, inserted] =
                    worldRequest->soundContentRevisions.emplace(soundId, revision);
                if (!inserted && found->second != revision)
                    return fail(QStringLiteral("Conflicting packed media for Sound %1")
                                    .arg(VNX::objectIdString(soundId)));
            }
        }
        for (const auto& source : worldRequest->soundSources) {
            if (worldRequest->soundContentRevisions.count(source.stableId) == 0)
                return fail(QStringLiteral("Missing packed media for Sound %1 on %2/%3")
                                .arg(VNX::objectIdString(source.stableId)).arg(request.options.platform, language));
        }
        auto compiled = Bridge::compileCurrentWorldRequest(*worldRequest);
        if (exportCanceled(request.options.cancelCheck, &result.error)) return fail(result.error);
        if (!compiled.ok() || !compiled.world) {
            return fail(QStringLiteral(
                            "Current World project compilation failed (%1, stable id %2, language '%3')")
                            .arg(static_cast<std::uint32_t>(compiled.status))
                            .arg(VNX::objectIdString(compiled.relatedStableId)).arg(language));
        }
        const harkenstudiobuild::ContainerWorldInstallViewV1& world = compiled.world->View();
        if (request.options.updateSelectedOnly) {
            for (std::uint32_t i = 0; i < world.playablePropertyCount; ++i) {
                const auto& property = world.playableProperties[i];
                if (worldRequest->propertyAuthoredOrders.count(property.stableId) != 0) continue;
                newPropertyOrders.emplace(property.stableId, property.authoredStableOrder);
                reservedPropertyOrders.insert(property.authoredStableOrder);
            }
        }
        if (!request.options.currentWorldOutputPath.isEmpty()
            && language == selectedLanguage) {
            if (!WriteCurrentWorldArtifactV1(request.options.currentWorldOutputPath, world, &compileError))
                return fail(compileError);
            result.currentWorldSemanticFingerprint = world.identity.runtimeSemanticFingerprint;
        }

        std::unordered_map<harkenstudio::ObjectId, IdentityOwner> owners;
        QString ownershipError;
        if (!assignOwnership(request, packs, language, initId,
                             world, &owners, &ownershipError))
            return fail(ownershipError);

        // Normalize control-only definitions through the same slice/rebind path as
        // published providers. This temporary owner is removed before encoding.
        auto compilationOwners = owners;
        PackMeta controlInputs;
        controlInputs.language = language;
        for (std::uint32_t i = 0; i < world.playablePropertyCount; ++i) {
            const auto id = world.playableProperties[i].stableId;
            if (owners.count(id) == 0
                && !claimIdentity(&compilationOwners, id, controlInputs.packStableId,
                    harkenstudiobuild::kInitCatalogIdentityClassPlayable, &compileError))
                return fail(compileError);
        }
        packs.push_back(std::move(controlInputs));
        for (PackMeta& pack : packs) {
            if (!pack.language.isEmpty() && pack.language != language)
                continue;
            if (pack.language.isEmpty() && !pack.partitionPayload.empty())
                continue;
            pack.ownedSends.clear();
            pack.ownedLayerAncestors.clear();
            pack.ownedStructuralAncestors.clear();
            pack.ownedPlacements.clear();
            pack.ownedTrackRefs.clear();
            pack.ownedContainerChildren.clear();
            pack.ownedActionSteps.clear();
            pack.slicedPlayables.clear();
            pack.slicedOccurrences.clear();
            pack.slicedTracks.clear();
            pack.slicedSegments.clear();
            pack.slicedContainers.clear();
            pack.slicedTransitions.clear();
            pack.slicedActions.clear();
            pack.slicedVariables.clear();
            if (!sliceWorld(world, pack.packStableId,
                            compilationOwners, &pack.slicedVariables, &pack.slicedPlayables, &pack.slicedOccurrences,
                            &pack.slicedTracks, &pack.slicedSegments, &pack.slicedContainers,
                            &pack.slicedTransitions, &pack.slicedActions, &pack, &compileError))
                return fail(compileError);
            pack.ownedPlayableIds.clear();
            for (const auto& playable : pack.slicedPlayables) {
                pack.ownedPlayableIds.insert(playable.stableId);
                for (std::uint32_t i = 0;
                     i < playable.organizationLayerAncestorCount; ++i) {
                    if (playable.organizationLayerAncestors == nullptr) {
                        break;
                    }
                    pack.ownedPlayableIds.insert(
                        playable.organizationLayerAncestors[i].layerStableId);
                }
            }
            pack.ownedDefinitions = ownedDefinitionCount(owners, pack.packStableId);
            if (!pack.actionStableIds.empty() && pack.ownedDefinitions == 0)
                return fail(QStringLiteral("Content Pack '%1' is missing owned definitions")
                                .arg(pack.name));
        }
        const auto packNeedsEncode = [&](const PackMeta& pack) {
            if (!pack.language.isEmpty() && pack.language != language)
                return false;
            if (pack.language.isEmpty() && !pack.partitionPayload.empty())
                return false;
            return true;
        };
        const auto rebindOwnedPlayables = [&](bool initOnly, bool* changed) {
            for (PackMeta& pack : packs) {
                if (!packNeedsEncode(pack))
                    continue;
                if (initOnly != (pack.packKind == harkenstudiobuild::kPackKindInit))
                    continue;
                for (auto& playable : pack.slicedPlayables) {
                    const auto before = playable.programSemanticFingerprint;
                    rebindPlayableCrossPackRefs(&playable, packs, language);
                    if (changed != nullptr
                        && playable.programSemanticFingerprint != before)
                        *changed = true;
                }
            }
        };
        bool fingerprintsChanged = true;
        for (int pass = 0; fingerprintsChanged && pass < 32; ++pass) {
            fingerprintsChanged = false;
            rebindOwnedPlayables(true, &fingerprintsChanged);
            rebindOwnedPlayables(false, &fingerprintsChanged);
        }
        for (PackMeta& pack : packs) {
            if (!packNeedsEncode(pack))
                continue;
            for (auto& occurrence : pack.slicedOccurrences) {
                if (const auto* parent = findLiveSlicedPlayable(
                        packs, language, occurrence.parentStableId))
                    occurrence.parentProgramSemanticFingerprint = parent->programSemanticFingerprint;
                rebindPlayableRefLive(&occurrence.childPlayable, packs, language);
                occurrence.occurrenceSemanticFingerprint =
                    harkenstudiobuild::ComputePlayableOccurrenceFingerprintV1(occurrence);
            }
            for (auto& track : pack.slicedTracks) {
                rebindPlayableRefLive(&track.rootDefinition, packs, language);
                if (track.placements != nullptr) {
                    auto* placements = const_cast<harkenstudiobuild::ScheduleClipPlacementV1*>(
                        track.placements);
                    for (std::uint32_t i = 0; i < track.placementCount; ++i)
                        rebindPlayableRefLive(&placements[i].content,
                                             packs, language);
                }
                track.scheduleSemanticFingerprint =
                    harkenstudiobuild::ComputeTrackScheduleFingerprintV1(track);
            }
        }
        for (PackMeta& pack : packs) {
            if (!packNeedsEncode(pack))
                continue;
            for (auto& segment : pack.slicedSegments) {
                rebindPlayableRefLive(&segment.rootDefinition, packs, language);
                if (segment.trackRefs != nullptr) {
                    auto* trackRefs = const_cast<harkenstudiobuild::ScheduleTrackRefV1*>(
                        segment.trackRefs);
                    for (std::uint32_t i = 0; i < segment.trackRefCount; ++i) {
                        rebindPlayableRefLive(&trackRefs[i].trackDefinition,
                                             packs, language);
                        const auto* track = findLiveSlicedTrack(
                            packs, language, trackRefs[i].trackDefinition.stableId);
                        if (track == nullptr)
                            return fail(QStringLiteral(
                                "Segment %1 references missing sliced Track %2")
                                .arg(VNX::objectIdString(segment.rootDefinition.stableId),
                                     VNX::objectIdString(trackRefs[i].trackDefinition.stableId)));
                        trackRefs[i].trackScheduleSemanticFingerprint =
                            track->scheduleSemanticFingerprint;
                    }
                }
                segment.scheduleSemanticFingerprint =
                    harkenstudiobuild::ComputeSegmentScheduleFingerprintV1(segment);
            }
            for (auto& container : pack.slicedContainers) {
                if (container.children != nullptr) {
                    auto* children = const_cast<harkenstudiobuild::ContainerChildProgramViewV2*>(
                        container.children);
                    for (std::uint32_t i = 0; i < container.childCount; ++i) {
                        rebindPlayableRefLive(&children[i].playable,
                                             packs, language);
                        if (!remapScheduleReferenceHintLive(&children[i].timingHint,
                                                            packs, language, &compileError))
                            return fail(compileError);
                    }
                }
                const auto* definition = findLiveSlicedPlayable(
                    packs, language, container.containerStableId);
                if (definition == nullptr)
                    return fail(QStringLiteral(
                        "Container %1 is missing from the sliced canonical set")
                        .arg(VNX::objectIdString(container.containerStableId)));
                container.programSemanticFingerprint =
                    definition->programSemanticFingerprint;
            }
        }
        bool actionFingerprintsChanged = true;
        // Live rebind can walk nested Action->Action refs. Runtime admits
        // maxActionTriggerDepth up to 64; keep the same fail-closed ceiling
        // here so a legal chain is not reported as non-convergent.
        constexpr int kMaxLiveActionFingerprintPasses = 64;
        for (int actionPass = 0;
             actionFingerprintsChanged && actionPass < kMaxLiveActionFingerprintPasses;
             ++actionPass) {
            actionFingerprintsChanged = false;
            for (PackMeta& pack : packs) {
                if (!packNeedsEncode(pack))
                    continue;
                for (auto& action : pack.slicedActions) {
                    if (action.steps != nullptr) {
                        auto* steps = const_cast<harkenstudiobuild::ActionStepProgramViewV1*>(
                            action.steps);
                        for (std::uint32_t i = 0; i < action.stepCount; ++i) {
                            remapActionObjectRefLive(&steps[i].target, packs, language);
                            remapActionObjectRefLive(&steps[i].quantizeReference, packs, language);
                        }
                    }
                    const auto before = action.programSemanticFingerprint;
                    action.programSemanticFingerprint =
                        harkenstudiobuild::ComputeActionProgramFingerprintV1(action);
                    if (action.programSemanticFingerprint != before)
                        actionFingerprintsChanged = true;
                }
            }
        }
        if (actionFingerprintsChanged)
            return fail(QStringLiteral(
                "Action program fingerprints did not converge after live rebind"));
        packs.pop_back();
        for (PackMeta& pack : packs) {
            if (!packNeedsEncode(pack))
                continue;
            harkenstudiobuild::ContainerWorldInstallViewV1 slice = world;

            slice.variables = pack.slicedVariables.empty() ? nullptr : pack.slicedVariables.data();
            slice.variableCount = static_cast<std::uint32_t>(pack.slicedVariables.size());
            slice.playableProperties = pack.slicedPlayables.empty() ? nullptr : pack.slicedPlayables.data();
            slice.playablePropertyCount = static_cast<std::uint32_t>(pack.slicedPlayables.size());
            slice.playableOccurrences = pack.slicedOccurrences.empty() ? nullptr : pack.slicedOccurrences.data();
            slice.playableOccurrenceCount = static_cast<std::uint32_t>(pack.slicedOccurrences.size());
            slice.trackSchedules = pack.slicedTracks.empty() ? nullptr : pack.slicedTracks.data();
            slice.trackScheduleCount = static_cast<std::uint32_t>(pack.slicedTracks.size());
            slice.segmentSchedules = pack.slicedSegments.empty() ? nullptr : pack.slicedSegments.data();
            slice.segmentScheduleCount = static_cast<std::uint32_t>(pack.slicedSegments.size());
            slice.containers = pack.slicedContainers.empty() ? nullptr : pack.slicedContainers.data();
            slice.containerCount = static_cast<std::uint32_t>(pack.slicedContainers.size());
            slice.transitions = pack.slicedTransitions.empty() ? nullptr : pack.slicedTransitions.data();
            slice.transitionCount = static_cast<std::uint32_t>(pack.slicedTransitions.size());
            slice.actions = pack.slicedActions.empty() ? nullptr : pack.slicedActions.data();
            slice.actionCount = static_cast<std::uint32_t>(pack.slicedActions.size());
            slice.identity.capabilityMask =
                harkenstudiobuild::detail::ComputeCurrentWorldLocalCapabilityMaskV1(slice);
            slice.identity.runtimeSemanticFingerprint =
                harkenstudiobuild::ComputeCurrentWorldFingerprintV5(slice);
            if (!encodePartition(pack.packStableId, slice, pack.ownedDefinitions,
                                 &pack.partitionPayload, &compileError, busTriple))
                return fail(compileError);
            pack.semanticFingerprint = harkenstudiobuild::Fnv1a64(
                pack.partitionPayload.data() + harkenstudiobuild::kCanonicalDefinitionPartitionHeaderV2Bytes,
                pack.partitionPayload.size()
                    - harkenstudiobuild::kCanonicalDefinitionPartitionHeaderV2Bytes);
            pack.lineageFingerprint = lineageFingerprint(owners, pack.packStableId);
            if (pack.packKind == harkenstudiobuild::kPackKindInit &&
                !fillInitCompanion(world, project, &pack.companion, &compileError))
                return fail(compileError);
            if (pack.packKind == harkenstudiobuild::kPackKindInit
                && pack.variablePolicyOverlayPayload.empty()
                && world.variableCount > 0) {
                if (!encodeVariablePolicyOverlay(
                        world, project, &pack.variablePolicyOverlayPayload,
                        &pack.variableExecutionFingerprints, &pack.variableLabelMappingFingerprints,
                        &compileError))
                    return fail(compileError);
            }
            if (!pack.actionStableIds.empty()
                && pack.variableRequirementsPayload.empty()) {
                const auto& fingerprints =
                    packs.front().variableExecutionFingerprints;
                std::set<harkenstudio::ObjectId> referenced;
                for (std::uint32_t i = 0; i < world.variableCount; ++i) {
                    const harkenstudio::ObjectId variableId = world.variables[i].variableId;
                    const ProjectItem* variable =
                        findItemByStableId(project, variableId);
                    if (!variable)
                        continue;
                    for (const auto& consumer :
                         VariableConsumerSemantics::consumersOf(
                             &project, variable->guid)) {
                        const ProjectItem* host =
                            findItemByGuid(project, consumer.hostGuid);
                        if (host && hostOwnedByPack(*host, pack, owners))
                            referenced.insert(variableId);
                    }
                }
                std::vector<std::pair<harkenstudio::ObjectId, std::uint64_t>> rows;
                for (harkenstudio::ObjectId variableId : referenced) {
                    const auto found = fingerprints.find(variableId);
                    if (found == fingerprints.end())
                        continue;
                    rows.push_back({variableId, found->second});
                }
                if (!rows.empty())
                    pack.variableRequirementsPayload =
                        encodeVariableRequirements(rows, packs.front().variableLabelMappingFingerprints);
            }
        }
    }

    if (request.options.updateSelectedOnly) {
        packs.erase(std::remove_if(packs.begin() + 1, packs.end(), [&](const auto& pack) {
            if (!request.options.onlyPackPaths.isEmpty())
                return !request.options.onlyPackPaths.contains(
                    PackBuildPlanning::packFileName(pack.language, pack.name, {}));
            if (!request.options.onlyActionStableIds.empty())
                return std::none_of(pack.actionStableIds.begin(), pack.actionStableIds.end(),
                    [&](auto id) { return request.options.onlyActionStableIds.count(id) != 0; });
            return !request.options.onlyUnits.isEmpty() && !request.options.onlyUnits.contains(pack.name);
        }), packs.end());
        for (auto it = publishedPacks.cbegin(); it != publishedPacks.cend(); ++it) {
            if (it.key() != initId && std::any_of(packs.begin(), packs.end(),
                    [&](const auto& pack) {
                        return pack.packStableId == it.key()
                            || pack.fileName.compare(it.value().value(QStringLiteral("path")).toString(),
                                                     Qt::CaseInsensitive) == 0;
                    }))
                continue;
            PackMeta retained;
            retained.retained = true;
            retained.previousRow = it.value();
            retained.companion = it.value().value(QStringLiteral("companion")).toObject();
            retained.packStableId = it.key();
            retained.packKind = it.value().value(QStringLiteral("packKind")).toInt();
            retained.name = it.value().value(QStringLiteral("name")).toString();
            retained.fileName = it.value().value(QStringLiteral("path")).toString();
            retained.language = it.value().value(QStringLiteral("language")).toString();
            retained.platform = it.value().value(QStringLiteral("platformId")).toString();
            retained.contentRevision = it.value().value(QStringLiteral("contentRevision")).toVariant().toULongLong();
            for (const auto& dependency : it.value().value(QStringLiteral("dependencies")).toArray())
                retained.dependencies.push_back(dependency.toString().toULongLong());
            if (it.key() == initId) {
                retained.unitKey = QStringLiteral("Init");
                packs.front() = std::move(retained);
            } else {
                retained.actionStableIds.push_back(runtimeObjectId(QUuid(
                    retained.companion.value(QStringLiteral("guid")).toString())));
                if (!retained.actionStableIds.empty())
                    retained.unitKey = QStringLiteral("a:") + objectIdString(retained.actionStableIds.front());
                packs.push_back(std::move(retained));
            }
        }
    }
    if (!harmonizePublicationCapacity(&packs, liveDir, resourceCacheDir, &compileError))
        return fail(compileError);
    for (PackMeta& pack : packs) {
        if (pack.retained && !pack.metadataUpdated) continue;
        auto globals = pack.resourceEntries;
        if (exportCanceled(request.options.cancelCheck, &result.error)) return fail(result.error);
        if (pack.retained) {
            if (!collectEntries(liveDir.filePath(pack.fileName), &globals, &compileError))
                return fail(compileError);
            std::erase_if(globals, [](const auto& entry) {
                return entry.kind == harkenstudiobuild::kPackEntryKindCanonicalDefinitionPartition
                    || entry.kind == harkenstudiobuild::kPackEntryKindPackRequirementBlock
                    || entry.kind == harkenstudiobuild::kPackEntryKindPackDependencyEdge
                    || entry.kind == harkenstudiobuild::kPackEntryKindVariablePolicyOverlay
                    || entry.kind == harkenstudiobuild::kPackEntryKindVariableRequirements
                    || entry.kind == harkenstudiobuild::kPackEntryKindBusDefinitionSnapshot;
            });
        } else if (!appendCanonicalBindingEntries(lowering, pack, &globals, &compileError)) {
            return fail(compileError);
        }
        std::vector<std::uint8_t> semantics = pack.partitionPayload;
        for (const auto& entry : globals) {
            const std::size_t offset = semantics.size();
            semantics.resize(offset + 32);
            harkenstudiobuild::StoreLE32(semantics.data() + offset, entry.kind);
            harkenstudiobuild::StoreLE32(semantics.data() + offset + 4, entry.vintage);
            harkenstudiobuild::StoreObjectId(semantics.data() + offset + 8, entry.id);
            harkenstudiobuild::StoreLE64(semantics.data() + offset + 24, entry.relationOrdinal);
            semantics.insert(semantics.end(), entry.payload.begin(), entry.payload.end());
        }
        for (const auto& row : {std::make_pair(harkenstudiobuild::kPackEntryKindVariablePolicyOverlay,
                                              &pack.variablePolicyOverlayPayload),
                                std::make_pair(harkenstudiobuild::kPackEntryKindVariableRequirements,
                                              &pack.variableRequirementsPayload)}) {
            if (row.second->empty()) continue;
            const std::size_t offset = semantics.size();
            semantics.resize(offset + 16);
            harkenstudiobuild::StoreLE32(semantics.data() + offset, row.first);
            harkenstudiobuild::StoreLE32(semantics.data() + offset + 4,
                                       harkenstudiobuild::kPackCanonicalEntryVintage);
            harkenstudiobuild::StoreLE64(semantics.data() + offset + 8, 0);
            semantics.insert(semantics.end(), row.second->begin(), row.second->end());
        }
        pack.contentRevision = harkenstudiobuild::Fnv1a64(semantics.data(), semantics.size()) & INT64_MAX;
        if (pack.contentRevision == 0) pack.contentRevision = 1;
    }
    QStringList capacityWritePaths;
    for (auto& pack : packs) {
        pack.requiredInitRevision = packs.front().contentRevision;
        if (pack.retained) {
            for (const auto& entry : pack.previousEntries) {
                if (entry.kind != harkenstudiobuild::kPackEntryKindPackDependencyEdge) continue;
                harkenstudiobuild::PackDependencyEdge edge;
                if (!harkenstudiobuild::ParsePackDependencyEdgeV2(
                        entry.payload.data(), entry.payload.size(), &edge))
                    return fail(QStringLiteral("Invalid retained Pack dependency"));
                if (edge.prerequisitePackStableId == initId
                    && edge.expectedPrerequisiteContentRevision != pack.requiredInitRevision)
                    pack.metadataUpdated = true;
            }
        }
        if (pack.metadataUpdated) capacityWritePaths.append(liveDir.filePath(pack.fileName));
    }
    if (!validateWriteAccess(capacityWritePaths, &compileError)) return fail(compileError);
    QSet<QString> currentPaths;
    for (const auto& pack : packs) {
        if (currentPaths.contains(pack.fileName.toCaseFolded()))
            return fail(QStringLiteral("The pack path '%1' conflicts with another published pack").arg(pack.fileName));
        currentPaths.insert(pack.fileName.toCaseFolded());
    }

    auto writeUnit = [&](PackMeta& meta) -> ProjectPackExporter::ExportResult {
        const QString path = QDir(stagingDir).filePath(meta.fileName);
        auto unit = std::move(meta.mediaResult);
        const auto reuse = [&] {
            const auto oldPath = liveDir.filePath(meta.previousRow.value(QStringLiteral("path")).toString());
            QDir().mkpath(QFileInfo(path).absolutePath());
            unit.success = QFile::copy(oldPath, path);
            if (!unit.success) unit.error = QStringLiteral("Cannot stage existing pack: %1").arg(oldPath);
            meta.skipped = unit.success;
            unit.outputPath = path;
            unit.fileSize = QFileInfo(path).size();
            return unit;
        };
        if (meta.retained && !meta.metadataUpdated) return reuse();
        if (meta.retained) {
            std::vector<Bridge::PackBuildSession::CanonicalPackEntry> entries;
            if (!collectEntries(liveDir.filePath(meta.fileName), &entries, &unit.error)) return unit;
            for (auto& entry : entries) {
                if (entry.kind == harkenstudiobuild::kPackEntryKindCanonicalDefinitionPartition) {
                    entry.payload = meta.partitionPayload;
                } else if (entry.kind == harkenstudiobuild::kPackEntryKindPackRequirementBlock) {
                    harkenstudiobuild::PackRequirementBlock requirement;
                    if (!harkenstudiobuild::ParsePackRequirementBlockV2(
                            entry.payload.data(), entry.payload.size(), &requirement)) {
                        unit.error = QStringLiteral("Invalid retained Pack requirement");
                        return unit;
                    }
                    requirement.semanticFingerprint = meta.semanticFingerprint;
                    requirement.packContentRevision = meta.contentRevision;
                    harkenstudiobuild::SerializePackRequirementBlockV2(requirement, entry.payload.data());
                    harkenstudiobuild::StampRequirementCoverage(&entry.payload);
                } else if (entry.kind == harkenstudiobuild::kPackEntryKindPackDependencyEdge) {
                    harkenstudiobuild::PackDependencyEdge edge;
                    if (!harkenstudiobuild::ParsePackDependencyEdgeV2(
                            entry.payload.data(), entry.payload.size(), &edge)) {
                        unit.error = QStringLiteral("Invalid retained Pack dependency");
                        return unit;
                    }
                    if (edge.prerequisitePackStableId == initId) {
                        edge.expectedPrerequisiteContentRevision = meta.requiredInitRevision;
                        harkenstudiobuild::SerializePackDependencyEdgeV2(edge, entry.payload.data());
                    }
                }
            }
            unit.success = Bridge::PackBuildSession::writeCanonicalPackFile(
                path, meta.packKind == harkenstudiobuild::kPackKindInit
                    ? Bridge::PackBuildSession::CanonicalPackKind::Init
                    : Bridge::PackBuildSession::CanonicalPackKind::CanonicalContent,
                entries, &unit.error);
            unit.outputPath = path;
            unit.fileSize = QFileInfo(path).size();
            return unit;
        }
        auto entries = meta.resourceEntries;
        if (!appendCanonicalBindingEntries(lowering, meta, &entries, &unit.error)) {
            unit.success = false;
            return unit;
        }
        Bridge::PackBuildSession::CanonicalPackEntry partition;
        partition.kind = harkenstudiobuild::kPackEntryKindCanonicalDefinitionPartition;
        partition.vintage = harkenstudiobuild::kPackCanonicalRequirementVintage;
        partition.relationOrdinal = 0;
        partition.payload = meta.partitionPayload;
        entries.push_back(std::move(partition));
        if (meta.packKind == harkenstudiobuild::kPackKindInit
            && !meta.variablePolicyOverlayPayload.empty()) {
            Bridge::PackBuildSession::CanonicalPackEntry overlay;
            overlay.kind = harkenstudiobuild::kPackEntryKindVariablePolicyOverlay;
            overlay.vintage = harkenstudiobuild::kPackCanonicalEntryVintage;
            overlay.id = {};
            overlay.payload = meta.variablePolicyOverlayPayload;
            entries.push_back(std::move(overlay));
        }
        if (meta.packKind == harkenstudiobuild::kPackKindCanonicalContent
            && !meta.variableRequirementsPayload.empty()) {
            Bridge::PackBuildSession::CanonicalPackEntry requirements;
            requirements.kind = harkenstudiobuild::kPackEntryKindVariableRequirements;
            requirements.vintage = harkenstudiobuild::kPackCanonicalEntryVintage;
            requirements.id = {};
            requirements.payload = meta.variableRequirementsPayload;
            entries.push_back(std::move(requirements));
        }

        std::uint32_t edgeOrdinal = 0;
        for (std::uint64_t dependency : meta.dependencies) {
            Bridge::PackBuildSession::CanonicalPackEntry edge;
            edge.kind = harkenstudiobuild::kPackEntryKindPackDependencyEdge;
            edge.vintage = harkenstudiobuild::kPackCanonicalRequirementVintage;
            edge.relationOrdinal = edgeOrdinal++;
            edge.payload = dependencyPayload(
                dependency,
                dependency == initId ? harkenstudiobuild::kPackDependencyEdgeKindInit
                    : harkenstudiobuild::kPackDependencyEdgeKindContent,
                dependency == initId ? meta.requiredInitRevision : 0,
                busTriple);
            entries.push_back(std::move(edge));
        }
        Bridge::PackBuildSession::CanonicalPackEntry requirement;
        requirement.kind = harkenstudiobuild::kPackEntryKindPackRequirementBlock;
        requirement.vintage = harkenstudiobuild::kPackCanonicalRequirementVintage;
        requirement.relationOrdinal = 0;
        requirement.payload = requirementPayload(
            meta.packStableId, meta.ownedResources, meta.ownedDefinitions, 1,
            meta.semanticFingerprint, meta.lineageFingerprint, meta.contentRevision,
            busTriple);
        entries.push_back(std::move(requirement));

        if (meta.packKind == harkenstudiobuild::kPackKindInit) {
            Bridge::PackBuildSession::CanonicalPackEntry snapshot;
            snapshot.kind = harkenstudiobuild::kPackEntryKindBusDefinitionSnapshot;
            snapshot.vintage = harkenstudiobuild::kPackBusDefinitionSnapshotVintage2;
            snapshot.id = {};
            snapshot.relationOrdinal = 1;
            snapshot.payload = busSnapshotPayload;
            entries.push_back(std::move(snapshot));
        }

        const bool matches = meta.previousValid
            && entriesFingerprint(entries, meta.packKind) == entriesFingerprint(meta.previousEntries, meta.packKind);
        if (matches && !request.options.forceReexport) return reuse();
        if (!validateWriteAccess({liveDir.filePath(meta.fileName)}, &unit.error)) {
            unit.success = false;
            return unit;
        }
        if (!Bridge::PackBuildSession::writeCanonicalPackFile(
                path,
                meta.packKind == harkenstudiobuild::kPackKindInit
                    ? Bridge::PackBuildSession::CanonicalPackKind::Init
                    : Bridge::PackBuildSession::CanonicalPackKind::CanonicalContent,
                entries, &unit.error)) {
            unit.success = false;
            return unit;
        }
        unit.success = true;
        unit.outputPath = path;
        unit.fileSize = QFileInfo(path).size();
        if (meta.grouped)
            unit.objectCount = static_cast<int>(meta.ownedDefinitions);
        return unit;
    };

    for (PackMeta& pack : packs) {
        reportProgress(pack, 60, PackGenerationStage::Writing);
        if (exportCanceled(request.options.cancelCheck, &result.error)) return fail(result.error);
        QElapsedTimer timer;
        timer.start();
        auto unit = writeUnit(pack);
        pack.elapsedNanoseconds += timer.nsecsElapsed();
        recordUnit(pack, unit);
        if (!unit.success)
            return fail(unit.error);
        if (exportCanceled(request.options.cancelCheck, &result.error)) return fail(result.error);
        result.warnings += unit.warnings;
        if (!pack.retained || pack.packKind == harkenstudiobuild::kPackKindInit) {
            result.fileSize += unit.fileSize;
            result.mediaCount += unit.mediaCount;
            result.objectCount += unit.objectCount;
            result.actionCount += unit.actionCount;
        }
        if (!pack.retained) {
            if (pack.skipped) ++result.skippedPackCount;
            else ++result.builtPackCount;
        }
    }

    for (const auto& pack : packs)
        reportProgress(pack, 85, PackGenerationStage::Publishing);
    std::vector<PackMeta*> allPacks;
    QStringList companionNames;
    QHash<uint32_t, QString> exportedWaveformSources;
    const QString waveformStagingRoot = QDir(stagingDir).filePath(QStringLiteral(".waveforms"));
    const auto warnWaveform = [&](PackMeta& pack, const QString& detail) {
        const QString warning = QStringLiteral("Waveforms omitted for Pack '%1': %2").arg(pack.name, detail);
        result.warnings.append(warning);
        pack.companion.remove(QStringLiteral("waveforms"));
        if (outRecords) {
            for (std::size_t i = firstRecord; i < outRecords->size(); ++i) {
                auto& record = (*outRecords)[i];
                if (record.unitKey == pack.unitKey && record.language == pack.language)
                    record.warnings.append(warning);
            }
        }
    };
    std::unordered_map<harkenstudio::ObjectId, const ProjectItem*> metadataItems;
    if (project.buildExportPackMetadata) {
        std::unordered_set<const ProjectItem*> visited;
        const auto indexItem = [&](auto&& self, const ProjectItem* item) -> void {
            if (!item || !visited.insert(item).second) return;
            metadataItems.emplace(item->runtimeId(), item);
            for (const auto& child : item->children)
                self(self, child.get());
        };
        indexItem(indexItem, project.root.get());
        for (const auto* binding : project.allFileBindings())
            if (binding) indexItem(indexItem, binding->rootItem.get());
    }
    QString waveformOutputRoot;
    QString waveformOutputError;
    if (project.buildExportWaveforms) {
        if (request.options.waveformOutputDir.isEmpty()) {
            if (!PathUtils::isValidWaveformBuildOutputDir(project.buildWaveformOutputDir, project.projectPath))
                waveformOutputError = QStringLiteral("Waveform output must use a valid directory outside the project root and object directories.");
            waveformOutputRoot = PathUtils::waveformBuildOutputRoot(project.projectPath, project.buildWaveformOutputDir);
        } else {
            waveformOutputRoot = request.options.waveformOutputDir;
            if (!QDir::isAbsolutePath(waveformOutputRoot))
                waveformOutputError = QStringLiteral("Waveform output override must be an absolute directory");
            else if (PathUtils::isProtectedPackOutputPath(waveformOutputRoot, project.projectPath))
                waveformOutputError = PathUtils::packBuildOutputLocationError();
        }
        if (waveformOutputError.isEmpty()) {
            if (!QDir().mkpath(waveformOutputRoot)) {
                waveformOutputError = QStringLiteral("Cannot use waveform output directory '%1'")
                    .arg(waveformOutputRoot);
            } else {
                waveformLock = std::make_unique<QLockFile>(QDir(waveformOutputRoot)
                    .filePath(QString::fromLatin1(kWaveformLock)));
                waveformLock->setStaleLockTime(0);
                if (!waveformLock->tryLock())
                    waveformOutputError = QStringLiteral("Waveform output is being used by another build or cannot be locked: %1")
                        .arg(waveformOutputRoot);
            }
        }
    }
    QString error;
    for (auto& pack : packs) {
        if (exportCanceled(request.options.cancelCheck, &result.error)) return fail(result.error);
        allPacks.push_back(&pack);
        if (!pack.retained && pack.packKind == harkenstudiobuild::kPackKindCanonicalContent) {
            if (pack.actionStableIds.size() != 1)
                return fail(QStringLiteral("An Action Pack must contain exactly one Action"));
            const auto* action = findAction(project, pack.actionStableIds.front());
            if (!action) return fail(QStringLiteral("Action publication identity is unavailable"));
            pack.companion = QJsonObject{{QStringLiteral("schemaVersion"), HARKEN_PACK_SCHEMA_VERSION},
                {QStringLiteral("name"), action->name},
                {QStringLiteral("guid"), objectIdString(action->runtimeId())}};
        }
        if (!pack.retained && project.buildExportWaveforms) {
            QString waveformError = waveformOutputError;
            if (!waveformError.isEmpty()
                || !fillWaveformCompanion(request, pack, waveformOutputRoot, waveformStagingRoot,
                    &exportedWaveformSources, &pack.companion, &waveformError)) {
                if (exportCanceled(request.options.cancelCheck, &result.error)) return fail(result.error);
                warnWaveform(pack, waveformError);
            }
        }
    }

    for (PackMeta* item : allPacks) {
        if (exportCanceled(request.options.cancelCheck, &result.error)) return fail(result.error);
        PackMeta& pack = *item;
        QByteArray stagedBytes;
        QString stagedError;
        const QString stagedPath = QDir(stagingDir).filePath(pack.fileName);
        if (!readAll(stagedPath, &stagedBytes, &stagedError))
            return fail(stagedError);
        pack.resourceCacheKey = QString::fromLatin1(
            QCryptographicHash::hash(stagedBytes, QCryptographicHash::Sha256).toHex());
        harkenstudiobuild::PackCanonicalImage decoded;
        if (harkenstudiobuild::ParsePackCanonicalImage(
                reinterpret_cast<const std::uint8_t*>(stagedBytes.constData()),
                static_cast<std::uint64_t>(stagedBytes.size()), &decoded)
            != harkenstudiobuild::kPackOk ||
            (pack.packKind == harkenstudiobuild::kPackKindCanonicalContent &&
             harkenstudiobuild::ValidateCanonicalInitDependency(decoded.dependencyEdges, initId)
                 != harkenstudiobuild::kPackOk))
            return fail(QStringLiteral("Staged Pack '%1' failed validation")
                            .arg(pack.fileName));
        if (pack.packKind == harkenstudiobuild::kPackKindInit && decoded.hasCatalog)
            return fail(QStringLiteral("Init Pack still contains a routing catalog"));
        if (decoded.hasGameplayCatalog || decoded.hasVariableNameCatalog)
            return fail(QStringLiteral("Pack still contains a name publication directory"));
        if (!pack.retained && project.buildExportPackMetadata
            && !fillPackMetadata(project, pack, decoded, metadataItems, &pack.companion, &error))
            return fail(error);
        if (!pack.retained && pack.companion.contains(QStringLiteral("waveforms"))) {
            QString waveformError;
            if (!publishWaveformCompanion(pack.companion, liveDir.filePath(pack.fileName),
                    waveformStagingRoot, waveformOutputRoot, &newWaveformFiles,
                    &waveformError, request.options.cancelCheck)) {
                if (exportCanceled(request.options.cancelCheck, &result.error)) return fail(result.error);
                warnWaveform(pack, waveformError);
            }
        }
        const QFileInfo file(pack.fileName);
        const QString relative = QDir::cleanPath(QDir(file.path()).filePath(
            file.completeBaseName() + QStringLiteral(".json")));
        const auto schemaErrors = ProjectSchemaValidator::validatePackManifest(
            HARKEN_PACK_SCHEMA_VERSION, pack.companion, relative);
        if (!schemaErrors.isEmpty()) return fail(schemaErrors.join(QLatin1Char('\n')));
        if (pack.retained) {
            if (!copyRelativeFile(liveDir, QDir(stagingDir), relative))
                return fail(QStringLiteral("Cannot retain Pack companion '%1'").arg(relative));
        } else if (!writeJson(QDir(stagingDir).filePath(relative), pack.companion, &error)) {
            return fail(error);
        }
        companionNames.append(relative);
        harkenstudiobuild::PackV2Header stagedHeader;
        if (!harkenstudiobuild::ParsePackV2Header(
                reinterpret_cast<const std::uint8_t*>(stagedBytes.constData()),
                &stagedHeader))
            return fail(QStringLiteral("Staged Pack '%1' is not a v2 container")
                            .arg(pack.fileName));
        pack.formatVersion = stagedHeader.formatVersion;
        pack.fileSize = stagedBytes.size();
        for (std::uint32_t i = 0; i < stagedHeader.entryCount; ++i) {
            const harkenstudiobuild::PackV2DirectoryEntry record =
                harkenstudiobuild::ParsePackV2DirectoryEntry(
                    reinterpret_cast<const std::uint8_t*>(stagedBytes.constData())
                    + stagedHeader.directoryOffset
                    + i * harkenstudiobuild::kPackV2DirectoryEntryBytes);
            if (record.kind == harkenstudiobuild::kPackEntryKindContainer)
                return fail(QStringLiteral(
                    "Pack '%1' still contains pack entry kind 9")
                                .arg(pack.fileName));
            if (record.kind == harkenstudiobuild::kPackEntryKindVoiceRecipe)
                return fail(QStringLiteral(
                    "Pack '%1' still contains pack entry kind 11")
                                .arg(pack.fileName));
        }
    }

    QSet<QString> referencedWaveformFiles;
    for (const auto* pack : allPacks) {
        const QDir companionDir(QFileInfo(liveDir.filePath(pack->fileName)).absolutePath());
        for (const auto& value : pack->companion.value(QStringLiteral("waveforms")).toArray())
            referencedWaveformFiles.insert(QDir::cleanPath(companionDir.absoluteFilePath(
                value.toObject().value(QStringLiteral("file")).toString())));
    }
    for (qsizetype i = newWaveformFiles.size(); i-- > 0;) {
        if (referencedWaveformFiles.contains(newWaveformFiles.at(i))) continue;
        QFile::remove(newWaveformFiles.at(i));
        newWaveformFiles.removeAt(i);
    }

    if (request.options.cancelCheck && request.options.cancelCheck()) {
        result.canceled = true;
        return fail(QStringLiteral("Export canceled"));
    }

    QJsonArray stagedPublication;
    QStringList stagedFiles;
    if (!ReadPackPublication(stagingDir, project.projectId, request.options.platform,
            &stagedPublication, &stagedFiles, &error, true, {}, false,
            request.options.cancelCheck)) return fail(error);
    QStringList publishedNames = companionNames;
    for (const PackMeta* pack : allPacks)
        publishedNames.append(pack->fileName);
    QStringList preservedNames;
    for (const auto* pack : allPacks) {
        const auto waveforms = companionWaveformFiles(pack->companion,
            liveDir.filePath(pack->fileName), liveDir);
        for (const auto& path : waveforms) {
            if (!liveNames.contains(path)) liveNames.append(path);
            if (!publishedNames.contains(path)) publishedNames.append(path);
            if (!preservedNames.contains(path)) preservedNames.append(path);
        }
    }
    for (const auto& pack : packs) {
        if (pack.skipped && pack.fileName == pack.previousRow.value(QStringLiteral("path")).toString())
            preservedNames.append(pack.fileName);
    }
    for (const auto& name : liveNames) {
        if (containsPublicationFile(liveDir, publishedNames, name)) continue;
        if (!request.options.selfCleaning
            && (!previousManaged.contains(name)
                || previousPackNames.contains(name) || previousCompanionNames.contains(name))) {
            publishedNames.append(name);
            preservedNames.append(name);
        }
    }
    for (const auto& name : companionNames) {
        if (!liveNames.contains(name)) continue;
        QByteArray before, after;
        if (readAll(liveDir.filePath(name), &before, &error)
            && readAll(QDir(stagingDir).filePath(name), &after, &error) && before == after)
            preservedNames.append(name);
    }
    if (!preserveBusyWaveformOutputs(liveDir, liveNames, &publishedNames, &preservedNames,
            &waveformCleanupLocks, &result.warnings, &error, request.options.cancelCheck,
            waveformLock && waveformLock->isLocked() ? waveformOutputRoot : QString()))
        return fail(error);

    nextDir = liveDir.filePath(QString::fromLatin1(kNextPrefix)
                               + QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!QDir().mkpath(nextDir))
        return fail(QStringLiteral("Cannot create next pack set"));
    for (const QString& name : publishedNames) {
        if (exportCanceled(request.options.cancelCheck, &result.error)) return fail(result.error);
        if (preservedNames.contains(name)) continue;
        if (!copyRelativeFile(QDir(stagingDir), QDir(nextDir), name))
            return fail(QStringLiteral("Cannot stage next '%1'").arg(name));
    }

    if (exportCanceled(request.options.cancelCheck, &result.error)) return fail(result.error);
    if (!commitPublication(liveDir, nextDir, liveNames, publishedNames, preservedNames,
            &error, request.options.cancelCheck)) {
        QString recoveryError;
        if (!recoverInterruptedPublish(liveDir, &recoveryError)) {
            keepPreviousOnFail = true;
            error += QLatin1Char('\n') + recoveryError;
        }
        return fail(error);
    }
    for (const auto& pack : packs) {
        if (pack.retained && pack.packKind != harkenstudiobuild::kPackKindInit) continue;
        const QString outputPath = liveDir.filePath(pack.fileName);
        result.artifacts.append({pack.unitKey, request.options.platform, pack.language,
            pack.name, outputPath, pack.fileSize, QFileInfo(outputPath).lastModified(),
            pack.formatVersion});
    }
    for (const auto& name : previousPackNames)
        if (!QFileInfo::exists(liveDir.filePath(name)))
            ++result.removedPackCount;
    for (const auto& pack : packs) {
        if (pack.grouped || (pack.retained && !pack.metadataUpdated)
            || pack.resourceFingerprint.isEmpty()) continue;
        const auto fingerprint = pack.resourceFingerprint.toLatin1();
        QFile existing(resourceCacheDir.filePath(pack.resourceCacheKey));
        if (existing.open(QIODevice::ReadOnly) && existing.readAll() == fingerprint) continue;
        existing.close();
        QSaveFile cache(resourceCacheDir.filePath(pack.resourceCacheKey));
        if (!QDir().mkpath(resourceCacheDir.path()) || !cache.open(QIODevice::WriteOnly)
            || cache.write(fingerprint) != fingerprint.size() || !cache.commit())
            result.warnings.append(QStringLiteral("Cannot cache pack build inputs for '%1'").arg(pack.name));
    }
    QDir(nextDir).removeRecursively();
    nextDir.clear();
    if (!QDir(stagingDir).removeRecursively())
        QDir(stagingDir).removeRecursively();
    removeMatchingDirs(liveDir, QString::fromLatin1(kStagingPrefix));

    removeMatchingDirs(QDir(request.outputDir), QString::fromLatin1(kStagingPrefix));
    if (request.options.selfCleaning) removeEmptyOutputDirs(liveDir);
    result.success = true;
    result.outputPath = request.outputDir;
    return result;
}

bool RecoverPackOutput(const QString& outputDir, QString* error)
{
    QString detail;
    const bool recovered = recoverInterruptedPublish(QDir(outputDir), &detail);
    if (!recovered && error) *error = detail;
    return recovered;
}

ProjectPackExporter::ExportResult ProjectPackExporter::FinalizePackExport(
    const QString& baseDir, const ProjectData& project,
    const QStringList& selectedPlatformIds, bool selfCleaning,
    const QVector<PackArtifactMetadata>& artifacts,
    const std::function<bool()>& cancelCheck)
{
    ExportResult result;
    const auto isCanceled = [&] {
        if (!exportCanceled(cancelCheck, &result.error)) return false;
        result.canceled = true;
        return true;
    };
    if (isCanceled()) return result;
    if (PathUtils::isProtectedPackOutputPath(baseDir, project.projectPath)) {
        result.error = PathUtils::packBuildOutputLocationError();
        return result;
    }
    const auto& platforms = project.platforms;
    if (selectedPlatformIds.isEmpty()) {
        result.error = QStringLiteral("Select at least one platform to export");
        return result;
    }
    QVector<ProjectPlatform> selectedPlatforms;
    for (const auto& id : selectedPlatformIds) {
        if (isCanceled()) return result;
        const auto* platform = ProjectPlatformSemantics::find(platforms, QUuid(id));
        if (!platform) {
            result.error = QStringLiteral("Unknown project platform ID: %1").arg(id);
            return result;
        }
        selectedPlatforms.append(*platform);
    }
    if (!FinalizePackOutputs(baseDir, platforms, selectedPlatformIds, selectedPlatformIds,
            &result.error, artifacts, isCanceled)) return result;
    const QDir liveDir(baseDir);
    QLockFile publicationLock(liveDir.filePath(QStringLiteral(".harken-pack-index.lock")));
    publicationLock.setStaleLockTime(0);
    if (!publicationLock.tryLock(2000)) {
        result.error = QStringLiteral("Pack output is being finalized by another build");
        return result;
    }
    QString projectId = project.projectId;
    QStringList selectedFiles;
    for (const auto& platform : selectedPlatforms) {
        if (isCanceled()) return result;
        QJsonArray rows;
        QStringList names;
        const QString platformId = ProjectPlatformSemantics::idString(platform.id);
        const QDir platformDir(platformOutputDir(baseDir, platform));
        QStringList expectedPaths;
        for (const auto& artifact : artifacts)
            if (artifact.platform == platformId)
                expectedPaths.append(platformDir.relativeFilePath(artifact.outputPath));
        if (!artifacts.isEmpty() && expectedPaths.isEmpty()) {
            result.error = QStringLiteral("No generated Pack artifacts for platform: %1").arg(platform.name);
            return result;
        }
        if (!ReadPackPublication(platformDir.path(), projectId, platformId, &rows, &names,
                &result.error, true, expectedPaths, false, isCanceled)) return result;
        if (!rows.isEmpty()) projectId = rows.first().toObject().value(QStringLiteral("projectId")).toString();
        for (const auto& value : rows) {
            if (isCanceled()) return result;
            const QString path = value.toObject().value(QStringLiteral("path")).toString();
            selectedFiles.append(platform.name + QLatin1Char('/') + path);
            const QFileInfo file(path);
            selectedFiles.append(platform.name + QLatin1Char('/')
                + QDir::cleanPath(QDir(file.path()).filePath(file.completeBaseName() + QStringLiteral(".json"))));
            selectedFiles.append(companionWaveformFiles(value.toObject().value(QStringLiteral("companion")).toObject(),
                platformDir.filePath(path), liveDir));
        }
    }
    QStringList liveNames, previousPackNames;
    const QString metadataName = QStringLiteral("Project.json");
    const QStringList metadataNames{metadataName, QStringLiteral("ProjectData.json")};
    if (selfCleaning) {
        QStringList platformNames;
        for (const auto& platform : selectedPlatforms) platformNames.append(platform.name);
        if (!scanCleanupFiles(liveDir, platformNames, &liveNames, &previousPackNames,
                &result.error, isCanceled)) return result;
    } else {
        if (QFileInfo(liveDir.absolutePath()).isSymLink()) {
            result.error = QStringLiteral("Output directory is a symbolic link: %1").arg(liveDir.absolutePath());
            return result;
        }
        for (const auto& entry : liveDir.entryInfoList(QDir::Files | QDir::Hidden | QDir::System)) {
            if (isCanceled()) return result;
            if (!metadataNames.contains(entry.fileName(), Qt::CaseInsensitive)) continue;
            if (entry.isSymLink()) {
                result.error = QStringLiteral("Project data output is a symbolic link: %1").arg(entry.absoluteFilePath());
                return result;
            }
            liveNames.append(entry.fileName());
        }
    }
    QStringList publishedNames;
    for (const auto& name : liveNames)
        if (!metadataNames.contains(name, Qt::CaseInsensitive)
            && (!selfCleaning || selectedFiles.contains(name))) publishedNames.append(name);
    QStringList preservedNames = publishedNames;
    std::vector<std::unique_ptr<QLockFile>> waveformCleanupLocks;
    if (!preserveBusyWaveformOutputs(liveDir, liveNames, &publishedNames, &preservedNames,
            &waveformCleanupLocks, &result.warnings, &result.error, isCanceled)) return result;
    QTemporaryDir next(liveDir.filePath(QString::fromLatin1(kNextPrefix) + QStringLiteral("XXXXXX")));
    if (!next.isValid()) {
        result.error = QStringLiteral("Cannot stage the Pack output publication");
        return result;
    }
    if (project.buildExportProjectData) {
        QJsonArray platformRows;
        for (const auto& platform : platforms) {
            platformRows.append(QJsonObject{
                {QStringLiteral("guid"), ProjectPlatformSemantics::idString(platform.id)},
                {QStringLiteral("name"), platform.name},
                {QStringLiteral("basePlatform"), ProjectPlatformSemantics::basePlatformName(platform.basePlatform)}});
        }
        const QJsonObject metadata{
            {QStringLiteral("schemaVersion"), HARKEN_PACK_SCHEMA_VERSION},
            {QStringLiteral("projectGuid"), QUuid(project.projectId).toString(QUuid::WithoutBraces)},
            {QStringLiteral("projectName"), project.name},
            {QStringLiteral("studioVersion"), QString::fromUtf8(Harken::BuildInfo::Version.data(),
                static_cast<qsizetype>(Harken::BuildInfo::Version.size()))},
            {QStringLiteral("studioSchemaVersion"), ProjectSchema::kProjectSchemaVersion},
            {QStringLiteral("platforms"), platformRows},
            {QStringLiteral("languages"), QJsonArray::fromStringList(ProjectLanguageSemantics::buildLanguages(project))}};
        const auto errors = ProjectSchemaValidator::validatePackManifest(
            HARKEN_PACK_SCHEMA_VERSION, metadata, metadataName);
        if (!errors.isEmpty()) {
            result.error = errors.join(QLatin1Char('\n'));
            return result;
        }
        if (!writeJson(QDir(next.path()).filePath(metadataName), metadata, &result.error)) return result;
        publishedNames.append(metadataName);
    }
    if (isCanceled()) return result;
    if (!commitPublication(liveDir, next.path(), liveNames, publishedNames, preservedNames,
            &result.error, isCanceled)) {
        QString detail;
        if (!recoverInterruptedPublish(liveDir, &detail)) {
            next.setAutoRemove(false);
            result.error += QLatin1Char('\n') + detail;
        }
        return result;
    }
    for (const auto& name : previousPackNames)
        if (!QFileInfo::exists(liveDir.filePath(name))) ++result.removedPackCount;
    if (selfCleaning) removeEmptyOutputDirs(liveDir);
    result.outputPath = baseDir;
    result.success = true;
    return result;
}

} // namespace VNX
