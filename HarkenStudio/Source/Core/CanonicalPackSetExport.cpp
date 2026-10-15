#include "CanonicalPackSetExport.h"

#include "ProjectBuildSnapshot.h"
#include "Components/VAction.h"
#include "Components/VSound.h"
#include "Components/VVariable.h"
#include "CurrentWorldProjectAdapter.h"
#include "CanonicalRuntimeProjectAdapter.h"
#include "CurrentWorldArtifactWriter.h"
#include "PackBuildPlan.h"
#include "NameValidator.h"
#include "ProjectTypes.h"
#include "PackSchemaGenerated.h"
#include "ProjectSchemaValidator.h"
#include "BindingRuntimeLowering.h"
#include "SoundSourceEditing.h"
#include "VariableConsumerSemantics.h"
#include "EditorWaveformPublication.h"

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
#include <harken_project_schema/VariableContract.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
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
                    QString* error)
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

void removeMatchingDirs(const QDir& parent, const QString& prefix)
{
    const QFileInfoList entries =
        parent.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const QFileInfo& info : entries) {
        if (info.fileName().startsWith(prefix))
            QDir(info.absoluteFilePath()).removeRecursively();
    }
}

QString uniqueMatchingDir(const QDir& parent, const QString& prefix)
{
    QString found;
    for (const QFileInfo& info : parent.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)) {
        if (!info.fileName().startsWith(prefix)) continue;
        if (!found.isEmpty() || info.isSymLink()) return {};
        found = info.absoluteFilePath();
    }
    return found;
}

bool safePublicationPath(const QDir& dir, const QString& relative)
{
    if (QFileInfo(dir.absolutePath()).isSymLink() || relative.isEmpty() || QDir::isAbsolutePath(relative)
        || relative.contains(QLatin1Char('\\')) || relative.contains(QLatin1Char(':'))
        || relative.endsWith(QStringLiteral(".meta"), Qt::CaseInsensitive))
        return false;
    QString current = dir.absolutePath();
    for (const QString& part : relative.split(QLatin1Char('/'))) {
        if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral("..")
            || part == QString::fromLatin1(kPublishingMarker)
            || part == QStringLiteral(".harken-pack-platform.lock")
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

bool publishedRelativeNames(const QDir& dir, QStringList* names, QString* error, bool requireComplete = false)
{
    names->clear();
    const QString manifestName = Bridge::packManifestFileName();
    if (!QFileInfo::exists(dir.filePath(manifestName)))
        return true;
    QStringList manifests{manifestName};
    for (qsizetype index = 0; index < manifests.size(); ++index) {
        const QString relative = manifests[index];
        QFile file(dir.filePath(relative));
        if (!safePublicationPath(dir, relative) || !file.open(QIODevice::ReadOnly)) {
            *error = QStringLiteral("Cannot read previous publication '%1'").arg(relative);
            return false;
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
        const QJsonObject manifest = document.object();
        const QStringList errors = ProjectSchemaValidator::validatePackManifest(
            PackSchema::kPackManifestSchemaVersion, manifest, relative);
        if (parseError.error != QJsonParseError::NoError || !document.isObject() || !errors.isEmpty()
            || manifest.value(QStringLiteral("packs")).toArray().isEmpty()) {
            *error = QStringLiteral("Cannot establish publication ownership from '%1': invalid manifest").arg(relative);
            return false;
        }
        names->append(relative);
        const QString companion = QDir::cleanPath(QDir(QFileInfo(relative).path()).filePath(
            QString::fromLatin1(harkenstudiobuild::tooling::kEditorWaveformFileName)));
        if (QFileInfo::exists(dir.filePath(companion))) {
            if (!safePublicationPath(dir, companion)) {
                *error = QStringLiteral("Unsafe publication path '%1'").arg(companion);
                return false;
            }
            names->append(companion);
        }
        for (const QJsonValue& value : manifest.value(QStringLiteral("packs")).toArray()) {
            const QJsonObject row = value.toObject();
            QString path = row.value(QStringLiteral("path")).toString();
            if (!safePublicationPath(dir, path)) {
                *error = QStringLiteral("Unsafe publication path '%1'").arg(path);
                return false;
            }
            const QString wrapped = path + QStringLiteral(".bytes");
            if (!path.endsWith(QStringLiteral(".bytes"), Qt::CaseInsensitive)
                && QFileInfo::exists(dir.filePath(wrapped))) {
                if (QFileInfo::exists(dir.filePath(path)) || !safePublicationPath(dir, wrapped)) {
                    *error = QStringLiteral("Ambiguous previous Pack '%1'").arg(path);
                    return false;
                }
                path = wrapped;
            }
            if (QFileInfo::exists(dir.filePath(path)))
                names->append(path);
            else if (requireComplete) {
                *error = QStringLiteral("Previous publication file is missing: %1").arg(path);
                return false;
            }
            if (index == 0) {
                const QString language = row.value(QStringLiteral("language")).toString();
                if (language.isEmpty()) continue;
                const QString selected = language + QLatin1Char('/') + manifestName;
                if (!safePublicationPath(dir, selected)) {
                    *error = QStringLiteral("Unsafe language publication '%1'").arg(selected);
                    return false;
                }
                if (QFileInfo::exists(dir.filePath(selected)) && !manifests.contains(selected))
                    manifests.append(selected);
            }
        }
    }
    names->removeDuplicates();
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

void removePublicationCopy(const QDir& dir, const QStringList& names)
{
    QStringList manifests;
    bool removed = true;
    for (const QString& name : names) {
        if (QFileInfo(name).fileName() == Bridge::packManifestFileName()) {
            manifests.prepend(name);
            continue;
        }
        removeRelativeFile(dir, name);
        removed = removed && !QFileInfo::exists(dir.filePath(name));
    }
    if (!removed) return;
    for (const QString& name : manifests) {
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
        if (publishedRelativeNames(copy, &names, &error))
            removePublicationCopy(copy, names);
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
    QString previous = uniqueMatchingDir(liveDir, QString::fromLatin1(kPreviousPrefix));
    QString next = uniqueMatchingDir(liveDir, QString::fromLatin1(kNextPrefix));
    QStringList previousNames, nextNames;
    if (!markerBytes.isEmpty()) {
        const QJsonObject transaction = QJsonDocument::fromJson(markerBytes).object();
        const QString previousName = transaction.value(QStringLiteral("previous")).toString();
        const QString nextName = transaction.value(QStringLiteral("next")).toString();
        if (transaction.value(QStringLiteral("version")).toInt() != 1
            || !previousName.startsWith(QString::fromLatin1(kPreviousPrefix))
            || previousName.contains(QLatin1Char('/')) || previousName.contains(QLatin1Char('\\')) || previousName.contains(QLatin1Char(':'))
            || !nextName.startsWith(QString::fromLatin1(kNextPrefix))
            || nextName.contains(QLatin1Char('/')) || nextName.contains(QLatin1Char('\\')) || nextName.contains(QLatin1Char(':')))
            return reject(QStringLiteral("invalid transaction record"));
        previous = liveDir.filePath(previousName);
        next = liveDir.filePath(nextName);
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
    } else {
        QString detail;
        if (previous.isEmpty() || !publishedRelativeNames(QDir(previous), &previousNames, &detail, true))
            return reject(detail.isEmpty() ? QStringLiteral("previous pack set is missing") : detail);
        // Older failed rollbacks deleted next. Restore the proven previous set
        // and retain any unproven live files instead of guessing their ownership.
        if (!next.isEmpty() && !publishedRelativeNames(QDir(next), &nextNames, &detail))
            nextNames.clear();
        if (previousNames.isEmpty() && (!QDir(previous).isEmpty() || nextNames.isEmpty()))
            return reject(QStringLiteral("previous publication ownership is unavailable"));
    }
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

struct PackMeta
{
    QString name;
    QString fileName;
    QString language;
    QString platform;
    std::vector<harkenstudio::ObjectId> actionStableIds;
    bool grouped = false;
    std::uint64_t packStableId = 0;
    std::uint32_t packKind = 0;
    std::uint64_t contentHash = 0;
    std::uint64_t contentRevision = 1;
    std::vector<std::uint64_t> dependencies;
    std::uint32_t ownedResources = 0;
    std::uint32_t ownedDefinitions = 0;
    std::uint64_t semanticFingerprint = 0;
    std::uint64_t lineageFingerprint = 0;
    std::vector<std::uint8_t> partitionPayload;
    std::vector<std::uint8_t> variablePolicyOverlayPayload;
    harkenstudiobuild::PackVariableNameCatalog variableNameCatalog;
    std::vector<std::uint8_t> variableNameCatalogPayload;
    std::vector<std::uint8_t> variableRequirementsPayload;
    std::unordered_map<harkenstudio::ObjectId, std::uint64_t> variableExecutionFingerprints;
    std::unordered_map<harkenstudio::ObjectId, std::uint64_t> variableLabelMappingFingerprints;
    QStringList resourcePaths;
    std::unordered_map<harkenstudio::ObjectId, std::uint64_t> soundContentRevisions;
    ProjectPackExporter::ExportResult mediaResult;
    QJsonObject gameplayCatalog;
    std::vector<std::uint8_t> gameplayCatalogPayload;
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
    std::vector<harkenstudiobuild::PieceScheduleProgramViewV1> slicedPieces;
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
        || type == ProjectItem::Type::Track || type == ProjectItem::Type::Piece
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

std::array<std::uint8_t, 16> guidRfc4122(const QUuid& guid)
{
    const QByteArray bytes = guid.toRfc4122();
    std::array<std::uint8_t, 16> out{};
    if (bytes.size() == 16)
        std::memcpy(out.data(), bytes.constData(), 16);
    return out;
}

std::uint64_t providerClosureFingerprint(const PackMeta& pack,
                                         std::uint64_t initId,
                                         std::uint64_t initRevision)
{
    std::vector<std::pair<std::uint64_t, std::uint64_t>> edges;
    edges.push_back({pack.packStableId, pack.contentRevision});
    for (std::uint64_t dependency : pack.dependencies) {
        const std::uint64_t revision =
            dependency == initId ? initRevision : 0;
        edges.push_back({dependency, revision});
    }
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    std::vector<std::uint8_t> bytes(edges.size() * 16, 0);
    for (std::size_t i = 0; i < edges.size(); ++i) {
        harkenstudiobuild::StoreLE64(bytes.data() + i * 16, edges[i].first);
        harkenstudiobuild::StoreLE64(bytes.data() + i * 16 + 8, edges[i].second);
    }
    return harkenstudiobuild::Fnv1a64(bytes.data(), bytes.size());
}

void visitActions(const ProjectData& project,
                  const std::function<void(const ProjectItem*)>& fn)
{
    for (const auto* binding : project.allFileBindings()) {
        if (!binding || !binding->rootItem)
            continue;
        std::function<void(const ProjectItem*)> walk = [&](const ProjectItem* item) {
            if (!item)
                return;
            if (item->type == ProjectItem::Type::Action)
                fn(item);
            for (const auto& child : item->children)
                walk(child.get());
        };
        walk(binding->rootItem.get());
    }
}

QString escapeActionSymbol(QString name)
{
    if (name.startsWith(QStringLiteral("A_"))) return QStringLiteral("A_") + name;
    for (QChar& c : name) {
        if (c == QLatin1Char(' ') || c == QLatin1Char('-') || c == QLatin1Char('.'))
            c = QLatin1Char('_');
    }
    if (!name.isEmpty() && name.front().isDigit())
        name.prepend(QLatin1Char('A'));
    // Closed C# / C identifier set from the gameplay contract.
    static const QStringList reserved = QStringLiteral(
        "abstract as base bool break byte case catch char checked class const continue "
        "decimal default delegate do double else enum event explicit extern false finally "
        "fixed float for foreach goto if implicit in int interface internal is lock long "
        "namespace new null object operator out override params private protected public "
        "readonly ref return sbyte sealed short sizeof stackalloc static string struct "
        "switch this throw true try typeof uint ulong unchecked unsafe ushort using "
        "virtual void volatile while "
        "add alias and ascending args async await by descending dynamic equals from "
        "get global group init into join let managed nameof nint not notnull nuint on "
        "or orderby partial record remove required scoped select set unmanaged value "
        "var when where with yield "
        "auto inline register restrict signed typedef union unsigned _Alignas _Alignof "
        "_Atomic _Bool _Complex _Generic _Imaginary _Noreturn _Static_assert _Thread_local"
    ).split(QLatin1Char(' '));
    if (reserved.contains(name))
        name.prepend(QStringLiteral("A_"));
    return name;
}

bool fillInitGameplayCatalog(const ProjectData& project,
                             const std::vector<PackMeta>& packs,
                             PackMeta& init, const QString& language,
                             QString* error)
{
    if (packs.empty()) {
        if (error)
            *error = QStringLiteral("Init Pack is missing");
        return false;
    }
    const QUuid projectGuid = QUuid::fromString(project.projectId);
    if (projectGuid.isNull()) {
        if (error)
            *error = QStringLiteral("Project is missing a project UUID");
        return false;
    }
    std::set<QString> liveNames;
    std::set<QString> liveSymbols;
    bool validNames = true;
    visitActions(project, [&](const ProjectItem* item) {
        if (!validNames) return;
        QString problem;
        if (!NameValidator::validateItemName(item->name, ProjectItem::Type::Action))
            problem = QStringLiteral("Invalid Action name '%1'").arg(item->name);
        else if (!liveNames.insert(item->name).second)
            problem = QStringLiteral("Duplicate Action name '%1'").arg(item->name);
        else if (!liveSymbols.insert(escapeActionSymbol(item->name)).second)
            problem = QStringLiteral("Action '%1' collides with another generated symbol").arg(item->name);
        else if (harkenstudio::IsNull(item->runtimeId()) || item->guid.isNull() ||
                 item->nameBindingRevision == 0)
            problem = QStringLiteral("Action '%1' is missing publication identity").arg(item->name);
        else if (actionNameTakeoverForbidden(project, item->name, item->guid))
            problem = QStringLiteral("Action name '%1' requires explicit published-name reuse").arg(item->name);
        if (!problem.isEmpty()) {
            validNames = false;
            if (error) *error = problem;
        }
    });
    if (!validNames) return false;
    std::vector<QByteArray> nameBytes;
    std::vector<harkenstudiobuild::PackGameplayCatalogActiveDraft> active;
    for (const PackMeta& pack : packs) {
        if (pack.packKind != harkenstudiobuild::kPackKindCanonicalContent ||
            pack.actionStableIds.empty() ||
            (!pack.language.isEmpty() && pack.language != language))
            continue;
        for (harkenstudio::ObjectId actionId : pack.actionStableIds) {
            const ProjectItem* action = findAction(project, actionId);
            if (!action)
                continue;
            nameBytes.push_back(action->name.toUtf8());
            harkenstudiobuild::PackGameplayCatalogActiveDraft row;
            row.actionStableId = !harkenstudio::IsNull(action->runtimeId())
                ? action->runtimeId()
                : actionId;
            row.nameBindingRevision = action->nameBindingRevision;
            row.callableFingerprint = harkenstudiobuild::GameplayCallableFingerprint(
                row.actionStableId, row.nameBindingRevision,
                std::string_view(nameBytes.back().constData(), nameBytes.back().size()));
            row.ownerPackStableId = pack.packStableId;
            row.providerClosureFingerprint = providerClosureFingerprint(
                pack, init.packStableId, init.contentRevision);
            row.name = std::string_view(nameBytes.back().constData(),
                                        static_cast<std::size_t>(nameBytes.back().size()));
            active.push_back(row);
        }
    }
    harkenstudiobuild::PackGameplayCatalogHeader identity;
    identity.projectGuid = guidRfc4122(projectGuid);
    identity.initPackStableId = init.packStableId;
    identity.initContentRevision = init.contentRevision;
    if (!harkenstudiobuild::EncodeGameplayCatalog(identity, active, {},
                                      &init.gameplayCatalogPayload)) {
        if (error) {
            *error = QStringLiteral(
                "Gameplay catalog exceeds 4096 entries or 1 MiB");
        }
        return false;
    }
    const auto digest = harkenstudiobuild::LoadLE64(init.gameplayCatalogPayload.data() + 24);
    auto catalogSort = [](const QByteArray& a, harkenstudio::ObjectId aId,
                          const QByteArray& b, harkenstudio::ObjectId bId) {
        const int c = harkenstudiobuild::CompareGameplayCatalogNameBytes(
            a.constData(), static_cast<std::uint16_t>(a.size()),
            b.constData(), static_cast<std::uint16_t>(b.size()));
        if (c != 0)
            return c < 0;
        return aId < bId;
    };
    std::sort(active.begin(), active.end(),
              [&](const auto& a, const auto& b) {
                  return catalogSort(QByteArray(a.name.data(),
                                                static_cast<int>(a.name.size())),
                                     a.actionStableId,
                                     QByteArray(b.name.data(),
                                                static_cast<int>(b.name.size())),
                                     b.actionStableId);
              });
    QJsonObject catalog;
    catalog.insert(QStringLiteral("schema"), 2);
    catalog.insert(QStringLiteral("projectId"), project.projectId);
    catalog.insert(QStringLiteral("digest"), QString::number(digest));
    catalog.insert(QStringLiteral("initPackStableId"),
                   QString::number(init.packStableId));
    catalog.insert(QStringLiteral("initContentRevision"),
                   static_cast<qint64>(init.contentRevision));
    QJsonArray actionsJson;
    for (const auto& row : active) {
        QJsonObject item;
        item.insert(QStringLiteral("name"),
                    QString::fromUtf8(row.name.data(),
                                      static_cast<int>(row.name.size())));
        item.insert(QStringLiteral("actionStableId"),
                    objectIdString(row.actionStableId));
        item.insert(QStringLiteral("nameBindingRevision"),
                    QString::number(row.nameBindingRevision));
        item.insert(QStringLiteral("callableFingerprint"),
                    QString::number(row.callableFingerprint));
        item.insert(QStringLiteral("ownerPackStableId"),
                    QString::number(row.ownerPackStableId));
        item.insert(QStringLiteral("providerClosureFingerprint"),
                    QString::number(row.providerClosureFingerprint));
        actionsJson.append(item);
    }
    catalog.insert(QStringLiteral("actions"), actionsJson);
    catalog.insert(QStringLiteral("retired"), QJsonArray{});
    init.gameplayCatalog = std::move(catalog);
    return true;
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
    const harkenprojectschema::VariableContractArtifactV1& contract,
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
        std::uint64_t catalogFingerprint = 0;
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
        const auto record = std::find_if(contract.variables.begin(), contract.variables.end(),
            [&](const auto& candidate) { return candidate.execution.variableId == def.variableId; });
        if (record == contract.variables.end()) return false;
        const auto mapping = harkenstudiobuild::ComputeVariableLabelMappingFingerprintV1(
            def.variableId, record->catalog.labels);
        (*labelFingerprints)[def.variableId] = mapping;
        rows.push_back({def.variableId, writers, fingerprint, std::move(anchors), record->catalog.labels,
            mapping, harkenprojectschema::ComputeVariableCatalogFingerprintV1(def.variableId, record->catalog)});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        return a.variableId < b.variableId;
    });
    std::size_t bytes = 16 + rows.size() * 32;
    for (const auto& row : rows) {
        bytes += 4 + row.anchors.size() * 4;
        bytes += 24;
        for (const auto& label : row.labels)
            bytes += 24 + ((label.name.size() + 3) & ~std::size_t{3});
    }
    std::vector<std::uint8_t> payload(bytes, 0);
    harkenstudiobuild::StoreLE32(payload.data(), static_cast<std::uint32_t>(rows.size()));
    harkenstudiobuild::StoreLE32(payload.data() + 4, 1);
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
        harkenstudiobuild::StoreLE64(payload.data() + offset + 8, row.catalogFingerprint);
        harkenstudiobuild::StoreLE32(payload.data() + offset + 16, static_cast<std::uint32_t>(row.labels.size()));
        offset += 24;
        for (const auto& label : row.labels) {
            harkenstudiobuild::StoreObjectId(payload.data() + offset, label.labelId);
            harkenstudiobuild::StoreLE32(payload.data() + offset + 16, static_cast<std::uint32_t>(label.anchor));
            harkenstudiobuild::StoreLE32(payload.data() + offset + 20, static_cast<std::uint32_t>(label.name.size()));
            offset += 24;
            std::memcpy(payload.data() + offset, label.name.data(), label.name.size());
            offset += (label.name.size() + 3) & ~std::size_t{3};
        }
    }
    *outPayload = std::move(payload);
    return true;
}

bool fillVariableNameCatalog(
    const harkenstudiobuild::ContainerWorldInstallViewV1& world,
    const ProjectData& project, PackMeta& init, QString* error)
{
    auto& catalog = init.variableNameCatalog;
    catalog.projectGuid = guidRfc4122(QUuid::fromString(project.projectId));
    catalog.initPackStableId = init.packStableId;
    catalog.initContentRevision = 1;
    catalog.rows.clear();
    std::set<QString> liveNames;
    for (std::uint32_t i = 0; i < world.variableCount; ++i) {
        const auto* item = findItemByStableId(project, world.variables[i].variableId);
        if (!item || item->type != ProjectItem::Type::Variable ||
            !NameValidator::validateItemName(item->name, item->type) ||
            !liveNames.insert(item->name).second) {
            if (error) *error = QStringLiteral("Invalid Variable publication identity or name");
            return false;
        }
        catalog.rows.push_back({item->runtimeId(), item->name.toStdString()});
    }
    if (!harkenstudiobuild::EncodeVariableNameCatalog(catalog, &init.variableNameCatalogPayload)) {
        if (error) *error = QStringLiteral("Invalid or oversized Variable name catalog");
        return false;
    }
    return true;
}

bool fillVariableContractArtifact(
    const harkenstudiobuild::ContainerWorldInstallViewV1& world,
    const ProjectData& project, std::uint64_t initPackStableId,
    harkenprojectschema::VariableContractArtifactV1* artifact, QString* error)
{
    if (artifact == nullptr)
        return false;
    harkenprojectschema::VariableContractArtifactV1 built;
    built.schemaVersion =
        harkenprojectschema::kVariableContractSchemaVersionV1;
    built.variables.reserve(world.variableCount);
    for (std::uint32_t i = 0; i < world.variableCount; ++i) {
        const auto& def = world.variables[i];
        const ProjectItem* item = findItemByStableId(project, def.variableId);
        std::uint8_t writers = 1;
        if (!overlayWritersFor(item, &writers, error))
            return false;
        const auto* source = item->comp<VVariable>();
        harkenprojectschema::VariableContractRecordV1 record;
        record.execution.variableId = def.variableId;
        record.execution.kind =
            static_cast<harkenprojectschema::ControlValueKindV1>(def.kind);
        record.execution.scopePolicy =
            harkenprojectschema::VariableScopePolicyV1::GlobalWithEmitterOverride;
        record.execution.writers =
            static_cast<harkenprojectschema::VariableWriterMaskV1>(writers);
        record.execution.range = {def.rangeMin, def.rangeMax, def.defaultValue};
        record.catalog.codeName = item->name.toStdString();
        record.catalog.displayName = item->name.toStdString();
        record.catalog.description = source->description.toStdString();
        record.catalog.unit = source->unit.toStdString();
        record.catalog.precision = source->precision;
        record.catalog.displayScale = source->displayScale;
        record.catalog.ownerInitPackId = initPackStableId;
        record.catalog.labels.reserve(source->labels.size());
        for (const auto& label : VariableSemantics::effectiveLabels(*source)) {
            record.catalog.labels.push_back(
                {label.anchor, label.name.toStdString(), runtimeObjectId(label.id)});
        }
        built.variables.push_back(std::move(record));
    }
    built.capacity.definitionCount =
        static_cast<std::uint32_t>(built.variables.size());
    *artifact = std::move(built);
    return true;
}

bool encodeVariableCatalogJson(
    const harkenprojectschema::VariableContractArtifactV1& artifact,
    QJsonObject* outCatalog, std::string* outExactJson, QString* error)
{
    if (outCatalog == nullptr || outExactJson == nullptr)
        return false;
    std::string json;
    if (!harkenprojectschema::EncodeVariableContractArtifactJsonV1(artifact,
                                                                  &json)) {
        if (error)
            *error = QStringLiteral("Variable catalog failed to encode");
        return false;
    }
    const QJsonDocument document =
        QJsonDocument::fromJson(QByteArray::fromStdString(json));
    if (!document.isObject()) {
        if (error)
            *error = QStringLiteral("Variable catalog JSON is not an object");
        return false;
    }
    *outCatalog = document.object();
    *outExactJson = std::move(json);
    return true;
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
        for (harkenstudio::ObjectId actionId : pack.actionStableIds)
            if (!claimIdentity(owners, actionId, pack.packStableId,
                               harkenstudiobuild::kInitCatalogIdentityClassAction, error))
                return false;
    }

    std::unordered_map<harkenstudio::ObjectId, std::vector<std::uint64_t>> playableRequirers;
    for (const PackMeta& pack : packs) {
        if (pack.actionStableIds.empty()
            || (!pack.language.isEmpty() && pack.language != language))
            continue;
        const std::uint64_t actionOwnerPack = pack.packStableId;
        for (const PackBuildVariant& variant : request.plan->variants) {
            if (std::find(pack.actionStableIds.begin(), pack.actionStableIds.end(),
                          variant.actionStableId) == pack.actionStableIds.end()
                || variant.variant.platform != pack.platform
                || variant.variant.language != pack.language)
                continue;
            std::set<harkenstudio::ObjectId> dependencyActionIds;
            for (const ActionClosureMember& action : variant.closure.actions)
                if (action.stableId != variant.actionStableId)
                    dependencyActionIds.insert(action.stableId);
            for (const ActionClosureMember& member : variant.closure.objects) {
                if (!isPlayableType(member.type) || harkenstudio::IsNull(member.stableId)
                    || member.type == ProjectItem::Type::Layer)
                    continue;
                if (std::any_of(member.referencePath.begin(),
                                member.referencePath.end(), [&](harkenstudio::ObjectId id) {
                                    return dependencyActionIds.count(id) != 0;
                                }))
                    continue;
                auto& requirers = playableRequirers[member.stableId];
                if (std::find(requirers.begin(), requirers.end(), actionOwnerPack)
                    == requirers.end())
                    requirers.push_back(actionOwnerPack);
            }
        }
    }

    for (const auto& [playableId, requirers] : playableRequirers) {
        if (requirers.empty())
            continue;
        for (const std::uint64_t ownerId : requirers) {
            if (!claimIdentity(owners, playableId, ownerId,
                               harkenstudiobuild::kInitCatalogIdentityClassPlayable, error))
                return false;
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

const harkenstudiobuild::PieceScheduleProgramViewV1* findSlicedPiece(
    const std::vector<harkenstudiobuild::PieceScheduleProgramViewV1>& pieces,
    harkenstudio::ObjectId stableId)
{
    for (const auto& piece : pieces) {
        if (piece.rootDefinition.stableId == stableId)
            return &piece;
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

const harkenstudiobuild::PieceScheduleProgramViewV1* findLiveSlicedPiece(
    std::vector<PackMeta>& packs,
    const QString& language,
    harkenstudio::ObjectId stableId)
{
    for (PackMeta& pack : packs) {
        if (!pack.language.isEmpty() && pack.language != language)
            continue;
        for (const auto& piece : pack.slicedPieces) {
            if (piece.rootDefinition.stableId == stableId)
                return &piece;
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
    if (key.kind == harkenstudiobuild::PlayableKindV1::Piece) {
        const auto* piece = findLiveSlicedPiece(packs, language, key.stableId);
        if (piece == nullptr) {
            if (error)
                *error = QStringLiteral(
                    "Schedule reference piece %1 is missing from the sliced canonical set")
                    .arg(VNX::objectIdString(key.stableId));
            return false;
        }
        key.scheduleSemanticFingerprint = piece->scheduleSemanticFingerprint;
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
    std::vector<harkenstudiobuild::PieceScheduleProgramViewV1>* pieces,
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
    pack->ownedTrackRefs.reserve(world.pieceScheduleCount);
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
    for (std::uint32_t i = 0; i < world.pieceScheduleCount; ++i) {
        if (!ownedBy(owners, world.pieceSchedules[i].rootDefinition.stableId, packStableId))
            continue;
        auto piece = world.pieceSchedules[i];
        rebindPlayableRef(&piece.rootDefinition, *playables);
        if (piece.trackRefCount != 0 && piece.trackRefs != nullptr) {
            pack->ownedTrackRefs.emplace_back(
                piece.trackRefs, piece.trackRefs + piece.trackRefCount);
            auto& trackRefs = pack->ownedTrackRefs.back();
            for (auto& trackRef : trackRefs) {
                rebindPlayableRef(&trackRef.trackDefinition, *playables);
                if (const auto* track = findSlicedTrack(*tracks, trackRef.trackDefinition.stableId))
                    trackRef.trackScheduleSemanticFingerprint = track->scheduleSemanticFingerprint;
            }
            piece.trackRefs = trackRefs.data();
        }
        piece.scheduleSemanticFingerprint =
            harkenstudiobuild::ComputePieceScheduleFingerprintV1(piece);
        pieces->push_back(piece);
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
    slice.pieceSchedules = pieces->empty() ? nullptr : pieces->data();
    slice.pieceScheduleCount = static_cast<std::uint32_t>(pieces->size());
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
                     harkenstudiobuild::CanonicalBusTriple busTriple)
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
    header.partitionRevision = 1;
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

} // namespace

ProjectPackExporter::ExportResult ExportCanonicalPackSet(
    ProjectPackExporter& exporter, const CanonicalPackSetExportRequest& request,
    std::vector<ProjectPackExporter::PackBuildRecord>* outRecords)
{
    ProjectPackExporter::ExportResult result;
    ProjectData& project = const_cast<ProjectData&>(*request.project);
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

    const QString stagingDir = liveDir.filePath(
        QString::fromLatin1(kStagingPrefix) +
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!QDir().mkpath(stagingDir)) {
        result.error = QStringLiteral("Cannot create staging directory");
        return result;
    }
    QString nextDir;
    QString previousDir;
    bool keepPreviousOnFail = false;
    const std::size_t firstRecord = outRecords ? outRecords->size() : 0;

    auto fail = [&](QString error) {
        QDir(stagingDir).removeRecursively();
        if (!nextDir.isEmpty() && !keepPreviousOnFail)
            QDir(nextDir).removeRecursively();
        if (!previousDir.isEmpty() && !keepPreviousOnFail)
            QDir(previousDir).removeRecursively();
        removeMatchingDirs(liveDir, QString::fromLatin1(kStagingPrefix));

        if (!keepPreviousOnFail)
            QFile::remove(liveDir.filePath(QString::fromLatin1(kPublishingMarker)));
        result.success = false;
        result.canceled = result.canceled || (request.options.cancelCheck && request.options.cancelCheck());
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
        init.fileName = QStringLiteral("Init") + Bridge::packArtifactExtension();
        init.packStableId = initId;
        init.packKind = harkenstudiobuild::kPackKindInit;
        packs.push_back(std::move(init));
    }
    for (const PackBuildUnit& planned : request.plan->units) {
        PackMeta content;
        content.name = planned.name;
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
            for (harkenstudio::ObjectId dependencyId : variant.dependencyOrder) {
                if (std::find(content.actionStableIds.begin(), content.actionStableIds.end(),
                              dependencyId) != content.actionStableIds.end())
                    continue;
                const auto dependency = std::find_if(packs.begin(), packs.end(),
                    [&](const PackMeta& candidate) {
                        return candidate.platform == content.platform
                            && (candidate.language.isEmpty() || candidate.language == content.language)
                            && std::find(candidate.actionStableIds.begin(), candidate.actionStableIds.end(),
                                         dependencyId) != candidate.actionStableIds.end();
                    });
                if (dependency == packs.end())
                    return fail(QStringLiteral("Missing Pack for Action %1 on %2/%3")
                                    .arg(VNX::objectIdString(dependencyId), content.platform,
                                         content.language));
                if (std::find(content.dependencies.begin(), content.dependencies.end(),
                              dependency->packStableId) == content.dependencies.end())
                    content.dependencies.push_back(dependency->packStableId);
            }
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
                return fail(QStringLiteral("Pack file name collision at '%1' (case-insensitive); rename the conflicting Actions or groups.")
                                .arg(pack.fileName));
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
                return fail(QStringLiteral("Pack dependency cycle involving '%1'; revise the Action groups")
                                .arg(pack.name));
    }

    auto prepareUnit = [&](PackMeta& meta) -> ProjectPackExporter::ExportResult {
        const QString path = QDir(stagingDir).filePath(meta.fileName);
        QDir().mkpath(QFileInfo(path).absolutePath());
        ProjectPackExporter::ExportResult unit;
        if (meta.packKind == harkenstudiobuild::kPackKindInit) {
            const QString residentPath = path + QStringLiteral(".resident");
            unit = exporter.ExportInitPack(project, residentPath, stagingOpts);
            if (!unit.success)
                return unit;
            meta.resourcePaths = {residentPath};
        } else {
            ProjectPackExporter::ExportOptions contentOpts = stagingOpts;
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
                meta.resourcePaths.append(mediaPath);
            }
            unit.success = true;
        }
        std::vector<Bridge::PackBuildSession::CanonicalPackEntry> entries;
        for (const QString& resourcePath : meta.resourcePaths) {
            if (!collectEntries(resourcePath, &entries, &unit.error)) {
                unit.success = false;
                return unit;
            }
        }
        for (const auto& entry : entries) {
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
        pack.mediaResult = prepareUnit(pack);
        result.mediaCacheHits += pack.mediaResult.mediaCacheHits;
        result.mediaConversions += pack.mediaResult.mediaConversions;
        result.mediaFallbacks += pack.mediaResult.mediaFallbacks;
        if (!pack.mediaResult.success)
            return fail(pack.mediaResult.error);
    }

    QString compileError;
    std::set<harkenstudio::ObjectId> packedActionIds;
    std::set<harkenstudio::ObjectId> packedDefinitionIds;
    QStringList languages;
    for (const PackMeta& pack : packs) {
        packedActionIds.insert(pack.actionStableIds.begin(), pack.actionStableIds.end());
        if (!pack.language.isEmpty() && !languages.contains(pack.language))
            languages.append(pack.language);
    }
    if (languages.isEmpty())
        languages.append(QString());
    const bool localized = languages != QStringList{QString()};
    const QString selectedLanguage = localized
        ? (stagingOpts.language.trimmed().isEmpty() ? project.defaultLanguage
                                                   : stagingOpts.language.trimmed())
        : QString();
    if (localized && !languages.contains(selectedLanguage))
        return fail(QStringLiteral("Selected language has no exported Pack variant: %1")
                        .arg(selectedLanguage));
    for (const PackBuildVariant& variant : request.plan->variants) {
        if (packedActionIds.count(variant.actionStableId) == 0)
            continue;
        for (const ActionClosureMember& member : variant.closure.objects)
            packedDefinitionIds.insert(member.stableId);
        for (const ActionClosureMember& member : variant.closure.controlObjects)
            packedDefinitionIds.insert(member.stableId);
    }
    harkenprojectschema::VariableContractArtifactV1 variableContract;
    variableContract.schemaVersion =
        harkenprojectschema::kVariableContractSchemaVersionV1;
    CurrentWorldBuildOptions buildOptions;
    buildOptions.includedActionIds = &packedActionIds;
    buildOptions.includedDefinitionIds = &packedDefinitionIds;
    buildOptions.skipEmptyActionSteps = true;
    for (const QString& language : languages) {
        buildOptions.language = language;
        auto worldRequest = buildCurrentWorldSyncRequest(
            project, QUuid::fromString(request.options.platform),
            &compileError, nullptr, &buildOptions);
        if (!worldRequest)
            return fail(compileError);
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
        if (!compiled.ok() || !compiled.world) {
            return fail(QStringLiteral(
                            "Current World project compilation failed (%1, stable id %2, language '%3')")
                            .arg(static_cast<std::uint32_t>(compiled.status))
                            .arg(VNX::objectIdString(compiled.relatedStableId)).arg(language));
        }
        const harkenstudiobuild::ContainerWorldInstallViewV1& world = compiled.world->View();
        if (!request.options.currentWorldOutputPath.isEmpty()
            && (language.isEmpty() || language == project.defaultLanguage)) {
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
            pack.slicedPieces.clear();
            pack.slicedContainers.clear();
            pack.slicedTransitions.clear();
            pack.slicedActions.clear();
            pack.slicedVariables.clear();
            if (!sliceWorld(world, pack.packStableId,
                            compilationOwners, &pack.slicedVariables, &pack.slicedPlayables, &pack.slicedOccurrences,
                            &pack.slicedTracks, &pack.slicedPieces, &pack.slicedContainers,
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
            for (auto& piece : pack.slicedPieces) {
                rebindPlayableRefLive(&piece.rootDefinition, packs, language);
                if (piece.trackRefs != nullptr) {
                    auto* trackRefs = const_cast<harkenstudiobuild::ScheduleTrackRefV1*>(
                        piece.trackRefs);
                    for (std::uint32_t i = 0; i < piece.trackRefCount; ++i) {
                        rebindPlayableRefLive(&trackRefs[i].trackDefinition,
                                             packs, language);
                        const auto* track = findLiveSlicedTrack(
                            packs, language, trackRefs[i].trackDefinition.stableId);
                        if (track == nullptr)
                            return fail(QStringLiteral(
                                "Piece %1 references missing sliced Track %2")
                                .arg(VNX::objectIdString(piece.rootDefinition.stableId),
                                     VNX::objectIdString(trackRefs[i].trackDefinition.stableId)));
                        trackRefs[i].trackScheduleSemanticFingerprint =
                            track->scheduleSemanticFingerprint;
                    }
                }
                piece.scheduleSemanticFingerprint =
                    harkenstudiobuild::ComputePieceScheduleFingerprintV1(piece);
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
            slice.pieceSchedules = pack.slicedPieces.empty() ? nullptr : pack.slicedPieces.data();
            slice.pieceScheduleCount = static_cast<std::uint32_t>(pack.slicedPieces.size());
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
                !fillVariableNameCatalog(world, project, pack, &compileError))
                return fail(compileError);
            if (pack.packKind == harkenstudiobuild::kPackKindInit
                && pack.variablePolicyOverlayPayload.empty()
                && world.variableCount > 0) {
                if (!fillVariableContractArtifact(world, project, initId,
                                                  &variableContract,
                                                  &compileError))
                    return fail(compileError);
                if (!encodeVariablePolicyOverlay(
                        world, project, variableContract, &pack.variablePolicyOverlayPayload,
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

    for (PackMeta& pack : packs) {
        std::vector<Bridge::PackBuildSession::CanonicalPackEntry> globals;
        for (const QString& resourcePath : pack.resourcePaths) {
            if (!collectEntries(resourcePath, &globals, &compileError))
                return fail(compileError);
        }
        if (!appendCanonicalBindingEntries(lowering, pack, &globals, &compileError))
            return fail(compileError);
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
        if (!pack.variablePolicyOverlayPayload.empty()) {
            const auto& payload = pack.variablePolicyOverlayPayload;
            const std::size_t offset = semantics.size();
            semantics.resize(offset + 16);
            harkenstudiobuild::StoreLE32(semantics.data() + offset,
                             harkenstudiobuild::kPackEntryKindVariablePolicyOverlay);
            harkenstudiobuild::StoreLE32(semantics.data() + offset + 4,
                             harkenstudiobuild::kPackCanonicalEntryVintage);
            harkenstudiobuild::StoreLE64(semantics.data() + offset + 8, 0);
            semantics.insert(semantics.end(), payload.begin(), payload.end());
        }
        if (!pack.variableRequirementsPayload.empty()) {
            const auto& payload = pack.variableRequirementsPayload;
            const std::size_t offset = semantics.size();
            semantics.resize(offset + 16);
            harkenstudiobuild::StoreLE32(semantics.data() + offset,
                             harkenstudiobuild::kPackEntryKindVariableRequirements);
            harkenstudiobuild::StoreLE32(semantics.data() + offset + 4,
                             harkenstudiobuild::kPackCanonicalEntryVintage);
            harkenstudiobuild::StoreLE64(semantics.data() + offset + 8, 0);
            semantics.insert(semantics.end(), payload.begin(), payload.end());
        }
        semantics.insert(semantics.end(), pack.variableNameCatalogPayload.begin(),
                         pack.variableNameCatalogPayload.end());
        pack.contentRevision = harkenstudiobuild::Fnv1a64(semantics.data(), semantics.size()) & INT64_MAX;
        if (pack.contentRevision == 0)
            pack.contentRevision = 1;
        if (pack.packKind == harkenstudiobuild::kPackKindInit) {
            pack.variableNameCatalog.initContentRevision = pack.contentRevision;
            if (!harkenstudiobuild::EncodeVariableNameCatalog(
                    pack.variableNameCatalog, &pack.variableNameCatalogPayload))
                return fail(QStringLiteral("Variable name catalog failed to encode"));
        }
    }

    auto writeUnit = [&](PackMeta& meta) -> ProjectPackExporter::ExportResult {
        const QString path = QDir(stagingDir).filePath(meta.fileName);
        auto unit = std::move(meta.mediaResult);
        std::vector<Bridge::PackBuildSession::CanonicalPackEntry> entries;
        for (const QString& resourcePath : meta.resourcePaths) {
            if (!collectEntries(resourcePath, &entries, &unit.error)) {
                unit.success = false;
                return unit;
            }
            QFile::remove(resourcePath);
        }
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
                dependency == initId ? packs.front().contentRevision : 0,
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
            Bridge::PackBuildSession::CanonicalPackEntry variableNames;
            variableNames.kind = harkenstudiobuild::kPackEntryKindVariableNameCatalog;
            variableNames.vintage = harkenstudiobuild::kPackCanonicalEntryVintage;
            variableNames.payload = meta.variableNameCatalogPayload;
            entries.push_back(std::move(variableNames));
            if (!fillInitGameplayCatalog(project, packs, meta, selectedLanguage, &unit.error)) {
                unit.success = false;
                return unit;
            }
            Bridge::PackBuildSession::CanonicalPackEntry catalog;
            catalog.kind = harkenstudiobuild::kPackEntryKindGameplayCatalog;
            catalog.vintage = harkenstudiobuild::kPackCanonicalEntryVintage;
            catalog.id = {};
            catalog.payload = packs.front().gameplayCatalogPayload;
            entries.push_back(std::move(catalog));
            meta.gameplayCatalog = packs.front().gameplayCatalog;
            meta.gameplayCatalogPayload = packs.front().gameplayCatalogPayload;
            Bridge::PackBuildSession::CanonicalPackEntry snapshot;
            snapshot.kind = harkenstudiobuild::kPackEntryKindBusDefinitionSnapshot;
            snapshot.vintage = harkenstudiobuild::kPackBusDefinitionSnapshotVintage2;
            snapshot.id = {};
            snapshot.relationOrdinal = 1;
            snapshot.payload = busSnapshotPayload;
            entries.push_back(std::move(snapshot));
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
        auto unit = writeUnit(pack);
        if (!unit.success)
            return fail(unit.error);
        result.warnings += unit.warnings;
        result.fileSize += unit.fileSize;
        result.mediaCount += unit.mediaCount;
        result.objectCount += unit.objectCount;
        result.actionCount += unit.actionCount;
        if (outRecords) {
            ProjectPackExporter::PackBuildRecord record;
            record.packName = pack.name;
            record.source = pack.actionStableIds.empty()
                ? ProjectPackExporter::PackBuildRecord::Source::Init
                : pack.grouped ? ProjectPackExporter::PackBuildRecord::Source::Group
                               : ProjectPackExporter::PackBuildRecord::Source::Action;
            record.success = true;
            record.outputPath = liveDir.filePath(pack.fileName);
            record.fileSize = unit.fileSize;
            record.platform = request.options.platform;
            const auto& definition = *ProjectPlatformSemantics::find(project.platforms, QUuid::fromString(record.platform));
            record.platformName = definition.name;
            record.basePlatform = definition.basePlatform;
            record.platformDefinition = definition.toJson();
            record.warnings = unit.warnings;
            record.language = pack.language;
            if (!pack.grouped && !pack.actionStableIds.empty()) {
                const ProjectItem* action = findAction(project, pack.actionStableIds.front());
                if (action)
                    record.objectID = action->objectID;
            }
            outRecords->push_back(record);
        }
    }

    // Init resources and dependency identity are shared; each session catalog
    // binds exactly one language's Content owners plus the common owners.
    std::vector<PackMeta> languageInits;
    if (localized) {
        std::vector<Bridge::PackBuildSession::CanonicalPackEntry> initEntries;
        if (!collectEntries(QDir(stagingDir).filePath(packs.front().fileName),
                            &initEntries, &compileError))
            return fail(compileError);
        for (const QString& language : languages) {
            PackMeta init = packs.front();
            init.language = language;
            init.fileName = language + QLatin1Char('/') + packs.front().fileName;
            if (!fillInitGameplayCatalog(project, packs, init, language, &compileError))
                return fail(compileError);
            for (auto& entry : initEntries)
                if (entry.kind == harkenstudiobuild::kPackEntryKindGameplayCatalog)
                    entry.payload = init.gameplayCatalogPayload;
            const QString path = QDir(stagingDir).filePath(init.fileName);
            QDir().mkpath(QFileInfo(path).absolutePath());
            if (!Bridge::PackBuildSession::writeCanonicalPackFile(
                    path, Bridge::PackBuildSession::CanonicalPackKind::Init,
                    initEntries, &compileError))
                return fail(compileError);
            result.fileSize += QFileInfo(path).size();
            languageInits.push_back(std::move(init));
        }
    }
    std::vector<PackMeta*> allPacks;
    for (auto& pack : packs) allPacks.push_back(&pack);
    for (auto& pack : languageInits) allPacks.push_back(&pack);
    for (PackMeta* item : allPacks) {
        PackMeta& pack = *item;
        QByteArray bytes;
        QString error;
        if (!readAll(QDir(stagingDir).filePath(pack.fileName), &bytes, &error))
            return fail(error);
        pack.contentHash =
            harkenstudiobuild::Fnv1a64(bytes.constData(), static_cast<std::size_t>(bytes.size()));
    }

    QString error;
    QJsonObject wrapper;
    wrapper.insert(QStringLiteral("schema_version"),
                   PackSchema::kPackManifestSchemaVersion);
    const auto& platform = *ProjectPlatformSemantics::find(project.platforms, QUuid(request.options.platform));
    wrapper.insert(QStringLiteral("platformId"), request.options.platform);
    wrapper.insert(QStringLiteral("platformName"), platform.name);
    wrapper.insert(QStringLiteral("basePlatform"), ProjectPlatformSemantics::basePlatformName(platform.basePlatform));
    wrapper.insert(QStringLiteral("language"), selectedLanguage);
    wrapper.insert(QStringLiteral("initPackStableId"), QString::number(initId));
    QJsonArray packArray;
    std::vector<const PackMeta*> manifestPacks;
    manifestPacks.reserve(packs.size());
    for (const PackMeta& pack : packs)
        manifestPacks.push_back(&pack);
    std::stable_sort(manifestPacks.begin(), manifestPacks.end(),
                     [](const PackMeta* a, const PackMeta* b) {
                         const auto rank = [](const PackMeta& pack) {
                             if (pack.packKind == harkenstudiobuild::kPackKindInit)
                                 return 0;
                             return 1;
                         };
                         const int left = rank(*a);
                         const int right = rank(*b);
                         if (left != right)
                             return left < right;
                         if (left == 1 && a->packStableId != b->packStableId)
                             return a->packStableId < b->packStableId;
                         return false;
                     });
    for (const PackMeta* pack : manifestPacks) {
        QJsonObject row;
        row.insert(QStringLiteral("name"), pack->name);
        row.insert(QStringLiteral("packStableId"), QString::number(pack->packStableId));
        row.insert(QStringLiteral("packKind"), static_cast<int>(pack->packKind));
        row.insert(QStringLiteral("path"), pack->fileName);
        row.insert(QStringLiteral("platformId"),
                   pack->platform.isEmpty() ? request.options.platform
                                            : pack->platform);
        row.insert(QStringLiteral("language"), pack->language);
        row.insert(QStringLiteral("contentHash"), QString::number(pack->contentHash));
        row.insert(QStringLiteral("contentRevision"), static_cast<qint64>(pack->contentRevision));
        row.insert(QStringLiteral("variantKey"),
                   pack->packKind == harkenstudiobuild::kPackKindCanonicalContent
                           && !pack->actionStableIds.empty()
                       ? QString::number(static_cast<std::uint32_t>(pack->packStableId))
                       : QString::number(0));
        QJsonArray deps;
        for (std::uint64_t dependency : pack->dependencies)
            deps.append(QString::number(dependency));
        row.insert(QStringLiteral("dependencies"), deps);
        packArray.append(row);
    }
    wrapper.insert(QStringLiteral("packs"), packArray);
    QJsonArray groups;
    for (const PackGroup& group : project.packGroups) {
        if (!group.includesPlatform(request.options.platform))
            continue;
        QJsonArray actions;
        for (harkenstudio::ObjectId actionId : group.actionIds) {
            if (request.includedActionIds.count(actionId) == 0)
                continue;
            const ProjectItem* action = findAction(project, actionId);
            if (action)
                actions.append(action->name);
        }
        if (actions.isEmpty())
            continue;
        QJsonObject row;
        row.insert(QStringLiteral("name"), group.name);
        row.insert(QStringLiteral("actions"), actions);
        groups.append(row);
    }
    wrapper.insert(QStringLiteral("groups"), groups);
    wrapper.insert(QStringLiteral("gameplayCatalog"), packs.front().gameplayCatalog);
    QJsonObject variableCatalog;
    std::string variableCatalogJson;
    if (!encodeVariableCatalogJson(variableContract, &variableCatalog,
                                   &variableCatalogJson, &error))
        return fail(error);
    // Hash and publish the same lossless Variable representation.
    const auto encodeManifest = [&](const QJsonObject& root) {
        QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
        bytes.chop(1);
        if (bytes.size() > 1)
            bytes += ',';
        bytes += QByteArrayLiteral("\"variableCatalog\":");
        bytes += QByteArray::fromStdString(variableCatalogJson);
        bytes += '}';
        return bytes;
    };
    QStringList waveformNames;
    const auto writeManifest = [&](QJsonObject root, const QString& relative) {
        root.remove(QStringLiteral("catalogChecksum"));
        const QByteArray catalogChecksumBytes = encodeManifest(root);
        root.insert(QStringLiteral("catalogChecksum"),
                    QString::number(harkenstudiobuild::Fnv1a64(
                        catalogChecksumBytes.constData(),
                        static_cast<std::size_t>(catalogChecksumBytes.size()))));
        QJsonObject validated = root;
        validated.insert(QStringLiteral("variableCatalog"), variableCatalog);
        const QStringList schemaErrors = ProjectSchemaValidator::validatePackManifest(
            PackSchema::kPackManifestSchemaVersion, validated,
            QStringLiteral("pack manifest"));
        if (!schemaErrors.isEmpty()) {
            error = schemaErrors.join(QLatin1Char('\n'));
            return false;
        }
        const QByteArray manifestBytes = encodeManifest(root);
        QSaveFile manifestFile(QDir(stagingDir).filePath(relative));
        if (!manifestFile.open(QIODevice::WriteOnly | QIODevice::Text) ||
            manifestFile.write(manifestBytes) != manifestBytes.size() ||
            !manifestFile.commit()) {
            error = QStringLiteral("Failed to write pack manifest");
            return false;
        }
        if (!localized || relative != Bridge::packManifestFileName()) {
            std::vector<harkenstudiobuild::tooling::EditorMetadataPackBytes> metadataPacks;
            for (const QJsonValue& value : root.value(QStringLiteral("packs")).toArray()) {
                const QJsonObject row = value.toObject();
                QByteArray bytes;
                if (!readAll(QDir(stagingDir).filePath(row.value(QStringLiteral("path")).toString()),
                        &bytes, &error)) return false;
                const auto* begin = reinterpret_cast<const std::uint8_t*>(bytes.constData());
                metadataPacks.push_back({row.value(QStringLiteral("packStableId")).toString().toULongLong(),
                    std::vector<std::uint8_t>(begin, begin + bytes.size())});
            }
            std::string companion, metadataError;
            if (!harkenstudiobuild::tooling::BuildEditorWaveformPublication(
                    std::string_view(manifestBytes.constData(), manifestBytes.size()),
                    metadataPacks, &companion, &metadataError)) {
                error = QStringLiteral("Failed to build editor waveform metadata: %1")
                    .arg(QString::fromStdString(metadataError));
                return false;
            }
            const QString companionName = QDir::cleanPath(QDir(QFileInfo(relative).path()).filePath(
                QString::fromLatin1(harkenstudiobuild::tooling::kEditorWaveformFileName)));
            QSaveFile companionFile(QDir(stagingDir).filePath(companionName));
            if (!companionFile.open(QIODevice::WriteOnly)
                || companionFile.write(companion.data(), companion.size()) != static_cast<qint64>(companion.size())
                || !companionFile.commit()) {
                error = QStringLiteral("Failed to write editor waveform metadata");
                return false;
            }
            waveformNames.append(companionName);
        }
        return true;
    };
    QStringList manifestNames{Bridge::packManifestFileName()};
    if (!writeManifest(wrapper, manifestNames.front()))
        return fail(error);
    for (const PackMeta& init : languageInits) {
        QJsonObject selected = wrapper;
        selected.insert(QStringLiteral("language"), init.language);
        selected.insert(QStringLiteral("gameplayCatalog"), init.gameplayCatalog);
        QJsonArray selectedPacks;
        for (const QJsonValue& value : packArray) {
            QJsonObject row = value.toObject();
            if (row.value(QStringLiteral("packStableId")).toString() == QString::number(initId)) {
                row.insert(QStringLiteral("path"), init.fileName);
                row.insert(QStringLiteral("contentHash"), QString::number(init.contentHash));
                row.insert(QStringLiteral("language"), init.language);
            } else if (!row.value(QStringLiteral("language")).toString().isEmpty()
                       && row.value(QStringLiteral("language")).toString() != init.language) {
                continue;
            }
            selectedPacks.append(row);
        }
        selected.insert(QStringLiteral("packs"), selectedPacks);
        const QString relative = init.language + QLatin1Char('/') + Bridge::packManifestFileName();
        if (!writeManifest(selected, relative))
            return fail(error);
        manifestNames.append(relative);
    }

    for (const PackMeta* item : allPacks) {
        const PackMeta& pack = *item;
        QByteArray stagedBytes;
        QString stagedError;
        const QString stagedPath = QDir(stagingDir).filePath(pack.fileName);
        if (!readAll(stagedPath, &stagedBytes, &stagedError))
            return fail(stagedError);
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
        if (pack.packKind == harkenstudiobuild::kPackKindInit) {
            if (!decoded.hasGameplayCatalog ||
                pack.gameplayCatalogPayload.size() < harkenstudiobuild::kGameplayCatalogHeaderBytes)
                return fail(QStringLiteral("Init Pack is missing a gameplay catalog"));
            const auto expectedDigest =
                harkenstudiobuild::LoadLE64(pack.gameplayCatalogPayload.data() + 24);
            if (decoded.gameplayCatalog.header.digest != expectedDigest)
                return fail(QStringLiteral(
                    "Init Pack gameplay catalog digest does not match"));
        }
        harkenstudiobuild::PackV2Header stagedHeader;
        if (!harkenstudiobuild::ParsePackV2Header(
                reinterpret_cast<const std::uint8_t*>(stagedBytes.constData()),
                &stagedHeader))
            return fail(QStringLiteral("Staged Pack '%1' is not a v2 container")
                            .arg(pack.fileName));
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

    if (request.options.cancelCheck && request.options.cancelCheck()) {
        result.canceled = true;
        return fail(QStringLiteral("Export canceled"));
    }

    QStringList publishedNames = manifestNames;
    publishedNames.append(waveformNames);
    for (const PackMeta* pack : allPacks)
        publishedNames.append(pack->fileName);

    nextDir = liveDir.filePath(QString::fromLatin1(kNextPrefix)
                               + QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!QDir().mkpath(nextDir))
        return fail(QStringLiteral("Cannot create next pack set"));
    for (const QString& name : publishedNames) {
        if (!copyRelativeFile(QDir(stagingDir), QDir(nextDir), name))
            return fail(QStringLiteral("Cannot stage next '%1'").arg(name));
    }

    previousDir = liveDir.filePath(QString::fromLatin1(kPreviousPrefix)
                                   + QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!QDir().mkpath(previousDir))
        return fail(QStringLiteral("Cannot retain previous pack set"));
    QStringList liveNames;
    QString ownershipError;
    if (!publishedRelativeNames(liveDir, &liveNames, &ownershipError))
        return fail(ownershipError);
    for (const QString& name : publishedNames) {
        if (!safePublicationPath(liveDir, name)
            || (QFileInfo::exists(liveDir.filePath(name)) && !containsPublicationFile(liveDir, liveNames, name)))
            return fail(QStringLiteral("Publication path conflicts with an unowned file: %1").arg(name));
    }
    QStringList previousNames;
    for (const QString& name : liveNames) {
        if (!copyRelativeFile(liveDir, QDir(previousDir), name))
            return fail(QStringLiteral("Cannot retain previous '%1'").arg(name));
        previousNames.append(name);
    }

    const QString marker = liveDir.filePath(QString::fromLatin1(kPublishingMarker));
    QJsonObject transaction;
    transaction.insert(QStringLiteral("version"), 1);
    transaction.insert(QStringLiteral("previous"), QFileInfo(previousDir).fileName());
    transaction.insert(QStringLiteral("next"), QFileInfo(nextDir).fileName());
    transaction.insert(QStringLiteral("previousFiles"), QJsonArray::fromStringList(previousNames));
    transaction.insert(QStringLiteral("nextFiles"), QJsonArray::fromStringList(publishedNames));
    QString transactionError;
    if (!writeJson(marker, transaction, &transactionError))
        return fail(QStringLiteral("Cannot start live-set publish: %1").arg(transactionError));

    auto restorePrevious = [&]() -> bool {
        bool orphansCleared = true;
        for (const QString& published : publishedNames) {
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
            if (!restorePrevious())
                keepPreviousOnFail = true;
            return fail(QStringLiteral("Cannot retire previous '%1'").arg(name));
        }
    }
    for (const QString& name : publishedNames) {
        if (!copyRelativeFile(QDir(nextDir), liveDir, name)) {
            if (!restorePrevious())
                keepPreviousOnFail = true;
            return fail(QStringLiteral("Cannot publish '%1'").arg(name));
        }
    }

    if (!QFile::remove(marker)) {
        restorePrevious();
        keepPreviousOnFail = true;
        return fail(QStringLiteral("Cannot commit live-set publish"));
    }
    QDir(nextDir).removeRecursively();
    nextDir.clear();
    QDir(previousDir).removeRecursively();
    previousDir.clear();
    if (!QDir(stagingDir).removeRecursively())
        QDir(stagingDir).removeRecursively();
    removeMatchingDirs(liveDir, QString::fromLatin1(kStagingPrefix));

    removeMatchingDirs(QDir(request.outputDir), QString::fromLatin1(kStagingPrefix));


    result.success = true;
    result.outputPath = request.outputDir;
    return result;
}

} // namespace VNX
