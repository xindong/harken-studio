#include "Tooling/Harkenizer/Core/CanonicalPackSetExport.h"
#include "ProjectLanguageSemantics.h"

#include "Tooling/Harkenizer/Core/ProjectBuildSnapshot.h"
#include "Tooling/Harkenizer/Core/PackBuildState.h"
#include "Tooling/Harkenizer/Core/SoundSourceDerivative.h"
#include "Tooling/Harkenizer/ResourceCompiler/ResourceCompiler.h"
#include <QScopeGuard>
#include "Components/VAction.h"
#include "Components/VSound.h"
#include "Components/VVariable.h"
#include "Tooling/Harkenizer/Core/CurrentWorldProjectAdapter.h"
#include "Tooling/Harkenizer/Core/CanonicalRuntimeProjectAdapter.h"
#include "Tooling/Harkenizer/Core/CanonicalRuntimeProjectAdapterPrivate.h"
#include "Tooling/Harkenizer/Core/CurrentWorldArtifactWriter.h"
#include "Tooling/Harkenizer/Core/PackBuildPlan.h"
#include "Tooling/Harkenizer/Core/PackMediaPreparation.h"
#include "NameValidator.h"
#include "ItemTypeCatalog.h"
#include "ProjectTypes.h"
#include "ProjectSchemaGenerated.h"
#include "ProjectSchemaValidator.h"
#include "harken_buildinfo.h"
#include "BindingRuntimeLowering.h"
#include "RuntimeCapabilityMatrix.h"
#include "SoundSourceEditing.h"
#include "SoundSourceSemantics.h"
#include "VariableConsumerSemantics.h"
#include "AssetPipeline/PathUtils.h"
#include "AssetPipeline/WaveformCache.h"

#include "Core/Build/CurrentWorldCompile.h"
#include "RuntimeBridge/CurrentWorldSyncBridgeInternal.h"
#include "Core/Build/PackagingBridge.h"

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
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace harken::studio {

namespace {


constexpr char kWaveformLock[] = ".harken-pack-waveforms.lock";

std::uint64_t fnvName(const QString& name)
{
    const QByteArray utf8 = name.toUtf8();
    return harken::studio::build::Fnv1a64(utf8.constData(), static_cast<std::size_t>(utf8.size()));
}

std::vector<std::uint8_t> requirementPayload(std::uint64_t packStableId,
                                             std::uint32_t ownedResources,
                                             std::uint32_t ownedDefinitions,
                                             std::uint64_t partitionRevision,
                                             std::uint64_t semanticFingerprint,
                                             std::uint64_t lineageFingerprint,
                                             std::uint64_t contentRevision,
                                             harken::studio::build::CanonicalBusTriple busTriple)
{
    harken::studio::build::PackRequirementBlock block;
    block.schemaVersion = HK_PACK_PACK_REQUIREMENT_BLOCK_VINTAGE;
    block.storageModelVersion = HK_PACK_PACK_REQUIREMENT_BLOCK_VINTAGE;
    block.packStableId = packStableId;
    block.packContentRevision = contentRevision;
    block.definitionPartitionRevision = partitionRevision;
    block.resourceClosureRevision =
        ownedResources != 0 || ownedDefinitions != 0 ? 1 : 0;
    block.semanticFingerprint = semanticFingerprint;
    block.lineageFingerprint = lineageFingerprint;
    block.ownedDefinitionCount = ownedDefinitions;
    block.ownedResourceCount = ownedResources;
    harken::studio::build::ApplyCanonicalBusTriple(&block, busTriple);
    std::vector<std::uint8_t> payload(harken::studio::build::kPackBytes<HkPackRequirementBlock>, 0);
    harken::studio::build::SerializePackRow<HkPackRequirementBlock>(block, payload.data());
    harken::studio::build::StampRequirementCoverage(&payload);
    return payload;
}

std::vector<std::uint8_t> dependencyPayload(std::uint64_t prerequisiteId,
                                            std::uint32_t edgeKind,
                                            std::uint64_t expectedRevision,
                                            harken::studio::build::CanonicalBusTriple busTriple)
{
    harken::studio::build::PackDependencyEdge edge;
    edge.prerequisitePackStableId = prerequisiteId;
    edge.edgeKind = edgeKind;
    edge.required01 = 1;
    edge.expectedPrerequisiteContentRevision = expectedRevision;
    if (edgeKind == HK_PACK_DEPENDENCY_EDGE_KIND_INIT)
        harken::studio::build::ApplyCanonicalBusTriple(&edge, busTriple);
    std::vector<std::uint8_t> payload(harken::studio::build::kPackBytes<HkPackDependencyEdge>, 0);
    harken::studio::build::SerializePackRow<HkPackDependencyEdge>(edge, payload.data());
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

bool sameMedia(const bridge::PackBuildSession::CanonicalPackEntry& left,
               const bridge::PackBuildSession::CanonicalPackEntry& right)
{
    const auto bytes = left.mediaBytes();
    return bytes == right.mediaBytes() && (bytes == 0 || left.mediaData() == right.mediaData()
        || std::memcmp(left.mediaData(), right.mediaData(), bytes) == 0);
}

bool appendEntry(bridge::PackBuildSession::CanonicalPackEntry entry, const QString& path,
                 std::vector<bridge::PackBuildSession::CanonicalPackEntry>* entries, QString* error)
{
    const auto existing = std::find_if(entries->begin(), entries->end(),
        [&](const auto& candidate) {
            return candidate.kind == entry.kind && candidate.id == entry.id
                && candidate.relationOrdinal == entry.relationOrdinal;
        });
    if (existing != entries->end()) {
        const bool samePayload = entry.kind == HK_PACK_ENTRY_KIND_MEDIA
            ? existing->payload.size() == entry.payload.size()
                && std::equal(entry.payload.begin(), entry.payload.begin() + offsetof(HkPackMedia, blobOffset),
                              existing->payload.begin())
                && std::equal(entry.payload.begin() + offsetof(HkPackMedia, blobBytes), entry.payload.end(),
                              existing->payload.begin() + offsetof(HkPackMedia, blobBytes))
                && sameMedia(*existing, entry)
            : existing->payload == entry.payload;
        if (existing->vintage != entry.vintage || !samePayload) {
            if (error)
                *error = QStringLiteral("Conflicting Pack entry %1 in '%2'")
                             .arg(harken::studio::objectIdString(entry.id), path);
            return false;
        }
        return true;
    }
    entries->push_back(std::move(entry));
    return true;
}

bool collectEntries(const QByteArray& bytes, const QString& path,
                    std::vector<bridge::PackBuildSession::CanonicalPackEntry>* entries,
                    QString* error, bool resourcesOnly, bool publicationMetadataOnly,
                    harkenizer::resourcecompiler::MediaCache* mediaCache = nullptr)
{
    if (bytes.size() < static_cast<int>(harken::studio::build::kPackBytes<HkPackFileHeader>)) {
        if (error)
            *error = QStringLiteral("Pack '%1' is too small").arg(path);
        return false;
    }
    harken::studio::build::PackFileHeader header;
    if (!harken::studio::build::ReadPackFileHeader(
            reinterpret_cast<const std::uint8_t*>(bytes.constData()), &header)) {
        if (error)
            *error = QStringLiteral("Pack '%1' is not a v2 container").arg(path);
        return false;
    }
    for (std::uint32_t i = 0; i < header.entryCount; ++i) {
        const harken::studio::build::PackDirectoryEntry record = harken::studio::build::ReadPackDirectoryEntry(
            reinterpret_cast<const std::uint8_t*>(bytes.constData()) +
            header.directoryOffset + i * harken::studio::build::kPackBytes<HkPackDirectoryEntry>);
        if (publicationMetadataOnly
            && record.kind != HK_PACK_ENTRY_KIND_CANONICAL_DEFINITION_PARTITION
            && record.kind != HK_PACK_ENTRY_KIND_PACK_REQUIREMENT_BLOCK
            && record.kind != HK_PACK_ENTRY_KIND_PACK_DEPENDENCY_EDGE
            && record.kind != HK_PACK_ENTRY_KIND_VARIABLE_POLICY_OVERLAY
            && record.kind != HK_PACK_ENTRY_KIND_VARIABLE_REQUIREMENTS) continue;
        if (resourcesOnly && record.kind != HK_PACK_ENTRY_KIND_MEDIA
            && record.kind != HK_PACK_ENTRY_KIND_BUS
            && record.kind != HK_PACK_ENTRY_KIND_BUS_SEND
            && record.kind != HK_PACK_ENTRY_KIND_ATTENUATION
            && record.kind != HK_PACK_ENTRY_KIND_EFFECT_PRESET
            && record.kind != HK_PACK_ENTRY_KIND_BUS_EFFECT
            && record.kind != HK_PACK_ENTRY_KIND_EFFECT_DYNAMIC_ROUTE) continue;
        bridge::PackBuildSession::CanonicalPackEntry entry;
        entry.kind = record.kind;
        entry.vintage = record.vintage;
        entry.id = record.id;
        entry.relationOrdinal = record.relationOrdinal;
        if (record.kind == HK_PACK_ENTRY_KIND_MEDIA) {
            const harken::studio::build::PackTocEntry toc = harken::studio::build::ReadPackMedia(
                reinterpret_cast<const std::uint8_t*>(bytes.constData()) + record.offset);
            entry.payload.resize(harken::studio::build::kPackBytes<HkPackMedia>);
            std::memcpy(entry.payload.data(),
                        reinterpret_cast<const std::uint8_t*>(bytes.constData()) +
                            record.offset,
                        harken::studio::build::kPackBytes<HkPackMedia>);
            harken::studio::build::StorePackMediaBlobOffset(entry.payload.data(), 0);
            if (toc.blobBytes != 0) {
                const auto* blob = reinterpret_cast<const std::uint8_t*>(bytes.constData()) + toc.blobOffset;
                if (toc.sampleFormat == HK_PACK_SAMPLE_FORMAT_VORBIS_RESIDENT) {
                    entry.encodedBytes = std::make_shared<const std::vector<uint8_t>>(blob, blob + toc.blobBytes);
                } else if (toc.sampleFormat != HK_PACK_SAMPLE_FORMAT_F32_PLANAR || toc.blobBytes % sizeof(float) != 0) {
                    if (error) *error = QStringLiteral("Pack '%1' contains invalid media").arg(path);
                    return false;
                } else if (mediaCache) {
                    entry.media = mediaCache->intern(blob, static_cast<std::size_t>(toc.blobBytes), toc.blobChecksum);
                } else {
                    auto samples = std::make_shared<std::vector<float>>(static_cast<size_t>(toc.blobBytes / sizeof(float)));
                    std::memcpy(samples->data(), blob, static_cast<size_t>(toc.blobBytes));
                    entry.media = std::move(samples);
                }
            }
        } else {
            entry.payload.assign(
                reinterpret_cast<const std::uint8_t*>(bytes.constData()) + record.offset,
                reinterpret_cast<const std::uint8_t*>(bytes.constData()) + record.offset +
                    record.size);
        }
        if (!appendEntry(std::move(entry), path, entries, error)) return false;
    }
    return true;
}

bool safePublicationPath(const QDir& dir, const QString& relative)
{
    if (QFileInfo(dir.absolutePath()).isSymLink() || relative.isEmpty() || QDir::isAbsolutePath(relative)
        || relative.contains(QLatin1Char('\\')) || relative.contains(QLatin1Char(':')))
        return false;
    QString current = dir.absolutePath();
    for (const QString& part : relative.split(QLatin1Char('/'))) {
        if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral("..")
            || part == QStringLiteral(".harken-pack-platform.lock")
            || part == QStringLiteral(".harken-pack-build.lock")
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
                     const std::function<bool()>& cancelCheck = {},
                     QStringList* directoryNames = nullptr)
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
                if (directoryNames) directoryNames->append(relative);
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
            const auto magic = file.read(sizeof(HkPackFilePrefix::magic));
            if (magic.size() == sizeof(HkPackFilePrefix::magic)
                && std::memcmp(magic.constData(), HK_PACK_MAGIC, magic.size()) == 0)
                packs->append(relative);
        }
    }
    return true;
}

bool preserveOutputFileSpelling(const QString& path, QString* error)
{
#ifdef Q_OS_WIN
    const QString nativePath = QDir::toNativeSeparators(path);
    WIN32_FIND_DATAW found;
    const HANDLE handle = FindFirstFileW(reinterpret_cast<LPCWSTR>(nativePath.utf16()), &found);
    if (handle == INVALID_HANDLE_VALUE) {
        *error = QStringLiteral("Cannot inspect output file name: %1").arg(path);
        return false;
    }
    FindClose(handle);
    const QFileInfo file(path);
    const QString existingName = QString::fromWCharArray(found.cFileName);
    if (existingName != file.fileName()) {
        const QString existingPath = QDir::toNativeSeparators(file.dir().filePath(existingName));
        if (!MoveFileExW(reinterpret_cast<LPCWSTR>(existingPath.utf16()),
                         reinterpret_cast<LPCWSTR>(nativePath.utf16()), MOVEFILE_REPLACE_EXISTING)) {
            *error = QStringLiteral("Cannot update output file name: %1").arg(path);
            return false;
        }
    }
#else
    Q_UNUSED(path);
    Q_UNUSED(error);
#endif
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
    QString unitKey;
    QString fileName;
    QString language;
    QString platform;
    std::vector<harken::studio::types::ObjectId> actionStableIds;
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
    QJsonObject packMetadata;
    std::vector<std::uint8_t> variableRequirementsPayload;
    std::unordered_map<harken::studio::types::ObjectId, std::uint64_t> variableExecutionFingerprints;
    std::unordered_map<harken::studio::types::ObjectId, std::uint64_t> variableLabelMappingFingerprints;
    std::vector<bridge::PackBuildSession::CanonicalPackEntry> resourceEntries;
    QJsonObject previousRow;
    std::vector<bridge::PackBuildSession::CanonicalPackEntry> previousEntries;
    QString resourceFingerprint;
    QString previousResourceFingerprint;
    bool previousValid = false;
    harken::studio::build::WorldCapacitySummaryV1 previousCapacity;
    bool retained = false;
    bool metadataUpdated = false;
    bool skipped = false;
    bool failed = false;
    bool current = false;
    bool finished = false;
    qint64 elapsedNanoseconds = 0;
    quint32 formatVersion = 0;
    qint64 fileSize = 0;
    std::unordered_map<harken::studio::types::ObjectId, std::uint64_t> soundContentRevisions;
    ProjectPackExporter::ExportResult mediaResult;
    std::set<harken::studio::types::ObjectId> ownedPlayableIds;
    std::vector<std::vector<harken::studio::build::SendV1>> ownedSends;
    std::vector<std::vector<harken::studio::build::LayerAncestorRefV1>> ownedLayerAncestors;
    std::vector<std::vector<harken::studio::build::StructuralAncestorRefV1>> ownedStructuralAncestors;
    std::vector<std::vector<harken::studio::build::ScheduleClipPlacementV1>> ownedPlacements;
    std::vector<std::vector<harken::studio::build::ScheduleTrackRefV1>> ownedTrackRefs;
    std::vector<std::vector<harken::studio::build::ContainerChildProgramViewV2>> ownedContainerChildren;
    std::vector<std::vector<harken::studio::build::ActionStepProgramViewV1>> ownedActionSteps;
    std::vector<harken::studio::build::PlayablePropertyProgramViewV1> slicedPlayables;
    std::vector<harken::studio::build::PlayableOccurrencePropertyViewV1> slicedOccurrences;
    std::vector<harken::studio::build::TrackScheduleProgramViewV1> slicedTracks;
    std::vector<harken::studio::build::SegmentScheduleProgramViewV1> slicedSegments;
    std::vector<harken::studio::build::ContainerProgramViewV3> slicedContainers;
    std::vector<harken::studio::build::TransitionProgramViewV1> slicedTransitions;
    std::vector<harken::studio::build::ActionProgramViewV1> slicedActions;
    std::vector<harken::studio::build::VariableDefinitionV1> slicedVariables;
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

const ProjectItem* findAction(const ProjectData& project, harken::studio::types::ObjectId stableId)
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

const ProjectItem* findItemByStableId(const ProjectData& project, harken::studio::types::ObjectId stableId)
{
    const ProjectItem* found = nullptr;
    std::function<void(const ProjectItem*)> walk = [&](const ProjectItem* item) {
        if (!item || found)
            return;
        const harken::studio::types::ObjectId itemId =
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

harken::studio::types::ObjectId itemStableId(const ProjectItem& item)
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
    const harken::studio::types::ObjectId id = itemStableId(*item);
    QString labelsError;
    if (source->rangeMin < 0.0f || !VariableSemantics::validateLabels(source->labels, &labelsError)) {
        if (error) *error = QStringLiteral("Variable '%1': %2").arg(item->name,
            labelsError.isEmpty() ? QStringLiteral("rangeMin must be non-negative") : labelsError);
        return false;
    }
    if (source->writers < 1 || source->writers > 3) {
        if (error)
            *error = QStringLiteral("Variable %1 writers must be 1, 2, or 3")
                         .arg(harken::studio::objectIdString(id));
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
                         .arg(harken::studio::objectIdString(id));
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
                         .arg(harken::studio::objectIdString(id));
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
    std::vector<bridge::PackBuildSession::CanonicalPackEntry>* entries)
{
    entries->erase(std::remove_if(entries->begin(), entries->end(),
        [](const bridge::PackBuildSession::CanonicalPackEntry& entry) {
            return entry.kind == HK_PACK_ENTRY_KIND_BINDING_DEFINITION
                || entry.kind == HK_PACK_ENTRY_KIND_BINDING_ATTACHMENT;
        }), entries->end());
}

bool encodeBindingDefinitionEntry(
    const BindingRuntimeProgram& program,
    bridge::PackBuildSession::CanonicalPackEntry* out)
{
    if (!out || harken::studio::types::IsNull(program.bindingId) || harken::studio::types::IsNull(program.variableId)
        || program.points.size() < 2
        || program.points.size() > harken::studio::types::kCurveMaxPoints)
        return false;
    harken::studio::build::BindingDefinition desc{};
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
    out->kind = HK_PACK_ENTRY_KIND_BINDING_DEFINITION;
    out->vintage = HK_PACK_BINDING_DEFINITION_VINTAGE;
    out->id = program.bindingId;
    out->payload.assign(harken::studio::build::kPackBytes<HkPackBindingDefinition>, 0);
    harken::studio::build::WritePackBindingDefinition(program.bindingId, desc,
                                            out->payload.data());
    return true;
}

bool appendCanonicalBindingEntries(
    const BindingRuntimeLowering& lowering,
    const PackMeta& meta,
    std::vector<bridge::PackBuildSession::CanonicalPackEntry>* entries,
    QString* error)
{
    if (!entries)
        return false;
    stripBindingEntries(entries);
    const bool initPack = meta.packKind == HK_PACK_KIND_INIT;
    std::set<harken::studio::types::ObjectId> localObjectIds;
    for (const auto& entry : *entries) {
        if (entry.kind == HK_PACK_ENTRY_KIND_MEDIA
            || entry.kind == HK_PACK_ENTRY_KIND_BUS)
            localObjectIds.insert(entry.id);
    }
    localObjectIds.insert(meta.ownedPlayableIds.begin(),
                          meta.ownedPlayableIds.end());

    std::vector<BindingRuntimeAttachment> kept;
    std::set<harken::studio::types::ObjectId> keptBindingIds;
    for (const BindingRuntimeAttachment& attachment : lowering.attachments) {
        const bool global =
            attachment.scopeKind == harken::studio::types::kTargetScopeBus
            || attachment.scopeKind == harken::studio::types::kTargetScopeBusEffectParam;
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
        bridge::PackBuildSession::CanonicalPackEntry entry;
        if (!encodeBindingDefinitionEntry(program, &entry)) {
            if (error)
                *error = QStringLiteral("Pack '%1' cannot encode Binding %2")
                             .arg(meta.fileName)
                             .arg(harken::studio::objectIdString(program.bindingId));
            return false;
        }
        entries->push_back(std::move(entry));
    }
    std::uint64_t ordinal = 0;
    for (const BindingRuntimeAttachment& attachment : kept) {
        bridge::PackBuildSession::CanonicalPackEntry entry;
        entry.kind = HK_PACK_ENTRY_KIND_BINDING_ATTACHMENT;
        entry.vintage = HK_PACK_BINDING_ATTACHMENT_VINTAGE;
        entry.relationOrdinal = ordinal++;
        harken::studio::build::PackBindingAttachmentEntry wire;
        wire.bindingId = attachment.bindingId;
        wire.scopeKind = attachment.scopeKind;
        wire.objectId = attachment.objectId;
        entry.payload.assign(harken::studio::build::kPackBytes<HkPackBindingAttachment>, 0);
        harken::studio::build::SerializePackRow<HkPackBindingAttachment>(wire, entry.payload.data());
        entries->push_back(std::move(entry));
    }
    return true;
}

bool encodeVariablePolicyOverlay(
    const harken::studio::build::ContainerWorldInstallViewV1& world,
    const ProjectData& project,
    std::vector<std::uint8_t>* outPayload,
    std::unordered_map<harken::studio::types::ObjectId, std::uint64_t>* fingerprints,
    std::unordered_map<harken::studio::types::ObjectId, std::uint64_t>* labelFingerprints,
    QString* error)
{
    struct Row {
        harken::studio::types::ObjectId variableId = {};
        std::uint8_t writers = 1;
        std::uint64_t fingerprint = 0;
        std::vector<std::int32_t> anchors;
        std::vector<harken::studio::build::VariableLabelDefinitionV1> labels;
        std::uint64_t labelMappingFingerprint = 0;
    };
    std::vector<Row> rows;
    rows.reserve(world.variableCount);
    for (std::uint32_t i = 0; i < world.variableCount; ++i) {
        const auto& def = world.variables[i];
        const ProjectItem* item = findItemByStableId(project, def.variableId);
        if (!item || !NameValidator::validateItemName(item->name, ProjectItem::Type::Variable)) {
            if (error) *error = QStringLiteral("Variable has an invalid callable name");
            return false;
        }
        std::uint8_t writers = 1;
        if (!overlayWritersFor(item, &writers, error))
            return false;
        auto anchors = discreteAnchorsFor(item);
        const std::int32_t* pointer =
            anchors.empty() ? nullptr : anchors.data();
        std::uint64_t fingerprint = 0;
        if (!harken::studio::build::ComputeVariableExecutionFingerprintV1(
                def.variableId, static_cast<std::uint8_t>(def.kind),
                1, writers, def.rangeMin,
                def.rangeMax, def.defaultValue, pointer,
                static_cast<std::uint32_t>(anchors.size()), &fingerprint)) {
            if (error)
                *error = QStringLiteral(
                    "Variable %1 failed execution fingerprint")
                             .arg(harken::studio::objectIdString(def.variableId));
            return false;
        }
        (*fingerprints)[def.variableId] = fingerprint;
        std::vector<harken::studio::build::VariableLabelDefinitionV1> labels;
        for (const auto& label : VariableSemantics::effectiveLabels(*item->comp<VVariable>()))
            labels.push_back({label.anchor, {}, runtimeObjectId(label.id)});
        const auto mapping = harken::studio::build::ComputeVariableLabelMappingFingerprintV1(def.variableId, labels);
        (*labelFingerprints)[def.variableId] = mapping;
        rows.push_back({def.variableId, writers, fingerprint, std::move(anchors), std::move(labels), mapping});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        return a.variableId < b.variableId;
    });
    using namespace harken::studio::build;
    struct PolicyRecord {
        const Row& source;
        PackVariablePolicyRow Wire() const noexcept { return {source.variableId, source.writers, source.fingerprint}; }
        bool Finish() const noexcept { return true; }
    };
    struct AnchorVisitor {
        const Row& source;
        PackVariableAnchorHeader header;
        const auto& Value() const noexcept { return header; }
        const auto& Header() const noexcept { return header; }
        bool PrepareAnchors(std::uint64_t count) const noexcept { return count == source.anchors.size(); }
        auto Anchors(std::uint64_t index) const noexcept { return source.anchors[static_cast<std::size_t>(index)]; }
        bool Finish() const noexcept { return true; }
    };
    struct LabelRecord {
        PackVariableLabel wire;
        const auto& Wire() const noexcept { return wire; }
        bool Finish() const noexcept { return true; }
    };
    struct LabelVisitor {
        const Row& source;
        PackVariableLabelHeader header;
        const auto& Value() const noexcept { return header; }
        const auto& Header() const noexcept { return header; }
        bool PrepareLabels(std::uint64_t count) const noexcept { return count == source.labels.size(); }
        LabelRecord Labels(std::uint64_t index) const noexcept {
            const auto& label = source.labels[static_cast<std::size_t>(index)];
            return {{label.labelId, label.anchor}};
        }
        bool Finish() const noexcept { return true; }
    };
    struct Visitor {
        const std::vector<Row>& rows;
        PackVariablePolicyHeader header;
        const auto& Value() const noexcept { return header; }
        const auto& Header() const noexcept { return header; }
        bool PrepareRows(std::uint64_t count) const noexcept { return count == rows.size(); }
        PolicyRecord Rows(std::uint64_t index) const noexcept { return {rows[static_cast<std::size_t>(index)]}; }
        bool PrepareAnchorGroups(std::uint64_t count) const noexcept { return count == rows.size(); }
        AnchorVisitor AnchorGroups(std::uint64_t index) const noexcept {
            const auto& row = rows[static_cast<std::size_t>(index)];
            return {row, {static_cast<std::uint32_t>(row.anchors.size())}};
        }
        bool PrepareLabelGroups(std::uint64_t count) const noexcept { return count == rows.size(); }
        LabelVisitor LabelGroups(std::uint64_t index) const noexcept {
            const auto& row = rows[static_cast<std::size_t>(index)];
            return {row, {row.labelMappingFingerprint, static_cast<std::uint32_t>(row.labels.size())}};
        }
        bool Finish() const noexcept { return true; }
    } visitor{rows, {static_cast<std::uint32_t>(rows.size())}};
    std::uint64_t bytes = 0;
    if (!MeasurePackVariablePolicyPayload(visitor, bytes)) return false;
    std::vector<std::uint8_t> payload(static_cast<std::size_t>(bytes), 0);
    PackWireBuffer<std::uint8_t> buffer{payload.data(), payload.size()};
    if (!AppendPackVariablePolicyPayload(buffer, visitor) || buffer.offset != bytes) return false;
    *outPayload = std::move(payload);
    return true;
}

bool fillInitPackMetadata(const harken::studio::build::ContainerWorldInstallViewV1& world,
                       const ProjectData& project, QJsonObject* packMetadata, QString* error)
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
    *packMetadata = QJsonObject{{QStringLiteral("packMetadataVersion"), HARKEN_PACK_METADATA_VERSION},
        {QStringLiteral("variables"), variables}};
    return true;
}

QStringList packMetadataWaveformFiles(const QJsonObject& packMetadata,
                                    const QString& packMetadataPath, const QDir& root)
{
    QStringList files;
    const QDir packMetadataDir(QFileInfo(packMetadataPath).absolutePath());
    for (const auto& value : packMetadata.value(QStringLiteral("waveforms")).toArray()) {
        const QString absolute = QDir::cleanPath(packMetadataDir.absoluteFilePath(
            value.toObject().value(QStringLiteral("file")).toString()));
        const QString relative = root.relativeFilePath(absolute);
        if (safePublicationPath(root, relative) && QFileInfo(absolute).isFile())
            files.append(relative);
    }
    files.removeDuplicates();
    return files;
}

std::set<harken::studio::types::ObjectId> worldMetadataObjectIds(
    const harken::studio::build::ContainerWorldInstallViewV1& world)
{
    std::set<harken::studio::types::ObjectId> ids;
    const auto collectPayload = [&](const harken::studio::build::PlayablePropertyPayloadViewV1& payload) {
        constexpr auto slot = static_cast<std::size_t>(
            harken::studio::build::playableproperties::PlayablePropertyIdV1::SpatialAttenuationRef) - 1;
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
    ids.erase(harken::studio::types::ObjectId{});
    ids.erase(harken::studio::types::kMasterBusObjectId);
    return ids;
}

std::set<harken::studio::types::ObjectId> packMetadataObjectIds(
    const harken::studio::build::PackCanonicalImage& image,
    const harken::studio::build::ContainerWorldInstallViewV1& world)
{
    auto ids = worldMetadataObjectIds(world);
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
    ids.erase(harken::studio::types::ObjectId{});
    ids.erase(harken::studio::types::kMasterBusObjectId);
    return ids;
}

bool fillPackMetadata(const ProjectData& project, const PackMeta& pack,
                      const std::set<harken::studio::types::ObjectId>& ids,
                      const std::unordered_map<harken::studio::types::ObjectId, const ProjectItem*>& items,
                      QJsonObject* packMetadata, QString* error)
{
    const auto* platform = ProjectPlatformSemantics::find(project.platforms, QUuid(pack.platform));
    if (!platform) {
        *error = QStringLiteral("Pack metadata platform is unavailable: %1").arg(pack.platform);
        return false;
    }
    QJsonArray objects;
    for (const auto id : ids) {
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
            {QStringLiteral("path"), item.objectPath()}});
    }
    packMetadata->insert(QStringLiteral("metadata"), QJsonObject{
        {QStringLiteral("packStableId"), QString::number(pack.packStableId)},
        {QStringLiteral("platform"), platform->name},
        {QStringLiteral("language"), pack.language},
        {QStringLiteral("objects"), objects}});
    return true;
}

bool fillWaveformPackMetadata(const CanonicalPackSetExportRequest& request,
                           const PackMeta& pack, const QString& outputRoot,
                           QHash<uint32_t, QString>* exportedSources,
                           QJsonObject* packMetadata, QString* error)
{
    QJsonArray waveforms;
    std::set<harken::studio::types::ObjectId> mediaIds;
    for (const auto& entry : pack.resourceEntries)
        if (entry.kind == HK_PACK_ENTRY_KIND_MEDIA)
            mediaIds.insert(entry.id);
    const auto& snapshot = *request.options.snapshot;
    const auto& project = *request.project;
    const QDir waveformDir(outputRoot);
    const QDir packMetadataDir(QFileInfo(QDir(request.outputDir).filePath(pack.fileName)).absolutePath());
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
            const auto conversion = snapshot.media.request(
                QUuid(objectIdString(media.soundStableId)), media.authoringSourceId, QUuid(pack.platform), error);
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
                const auto sourcePath = conversion->inputPath();
                QByteArray bytes;
                QByteArray waveformState;
                const auto& inputs = snapshot.media.sourceInputs;
                if (!WaveformCache::readForBuild(sourcePath, conversion->asset.absolutePath,
                        conversion->asset.contentHash, bytes,
                        inputs->waveformState(media.authoringSourceId), &waveformState)) {
                    *error = QStringLiteral("Cannot generate waveform cache for '%1'").arg(sourcePath);
                    return false;
                }
                inputs->setWaveformState(media.authoringSourceId, waveformState);
                fileName = QString::fromLatin1(QCryptographicHash::hash(bytes,
                    QCryptographicHash::Sha256).toHex()) + QStringLiteral(".vkd");
                if (exportCanceled(request.options.cancelCheck, error)) return false;
                const QString path = waveformDir.filePath(fileName);
                if (!request.options.packBuildState || !request.options.packBuildState->isWaveformCurrent(
                        pack.fileName, packMetadataDir.relativeFilePath(path))) {
                    QSaveFile file(path);
                    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
                        *error = QStringLiteral("Cannot write waveform '%1'").arg(path);
                        return false;
                    }
                    if (request.options.packBuildState)
                        request.options.packBuildState->recordWaveformOutput(path);
                }
                exportedSources->insert(media.authoringSourceId, fileName);
            }
            waveforms.append(QJsonObject{
                {QStringLiteral("soundGuid"), objectIdString(media.soundStableId)},
                {QStringLiteral("file"), packMetadataDir.relativeFilePath(waveformDir.filePath(fileName))},
                {QStringLiteral("trimStartFrame"), static_cast<qint64>(range->trimStartFrame)},
                {QStringLiteral("trimEndFrame"), static_cast<qint64>(range->trimEndFrame)}});
        }
    }
    if (!mediaIds.empty()) {
        *error = QStringLiteral("Pack '%1' contains media without a waveform source").arg(pack.fileName);
        return false;
    }
    packMetadata->insert(QStringLiteral("waveforms"), waveforms);
    return true;
}

std::vector<std::uint8_t> encodeVariableRequirements(
    const std::vector<std::pair<harken::studio::types::ObjectId, std::uint64_t>>& rows,
    const std::unordered_map<harken::studio::types::ObjectId, std::uint64_t>& labelFingerprints)
{
    using namespace harken::studio::build;
    std::vector<PackVariableRequirement> wires;
    wires.reserve(rows.size());
    for (const auto& row : rows)
        wires.push_back({row.first, row.second, labelFingerprints.at(row.first)});
    struct Record {
        const PackVariableRequirement& wire;
        const auto& Wire() const noexcept { return wire; }
        bool Finish() const noexcept { return true; }
    };
    struct Visitor {
        const std::vector<PackVariableRequirement>& wires;
        PackVariableRequirementsHeader header;
        const auto& Value() const noexcept { return header; }
        const auto& Header() const noexcept { return header; }
        bool PrepareRows(std::uint64_t count) const noexcept { return count == wires.size(); }
        Record Rows(std::uint64_t index) const noexcept { return {wires[static_cast<std::size_t>(index)]}; }
        bool Finish() const noexcept { return true; }
    } visitor{wires, {static_cast<std::uint32_t>(rows.size())}};
    std::uint64_t bytes = 0;
    if (!MeasurePackVariableRequirementsPayload(visitor, bytes)) return {};
    std::vector<std::uint8_t> payload(static_cast<std::size_t>(bytes), 0);
    PackWireBuffer<std::uint8_t> buffer{payload.data(), payload.size()};
    if (!AppendPackVariableRequirementsPayload(buffer, visitor) || buffer.offset != bytes) return {};
    return payload;
}

bool claimIdentity(std::unordered_map<harken::studio::types::ObjectId, IdentityOwner>* owners,
                   harken::studio::types::ObjectId stableId, std::uint64_t packStableId,
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
                         .arg(harken::studio::objectIdString(stableId));
        return false;
    }
    (*owners)[stableId] = {{packStableId}, identityClass};
    return true;
}

bool assignOwnership(
    const CanonicalPackSetExportRequest& request,
    const std::vector<PackMeta>& packs, const QString& language,
    std::uint64_t initId,
    const harken::studio::build::ContainerWorldInstallViewV1& world,
    std::unordered_map<harken::studio::types::ObjectId, IdentityOwner>* owners, QString* error)
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
                                   harken::studio::build::kInitCatalogIdentityClassAction, error))
                    return false;
            for (const ActionClosureMember& member : variant.closure.objects) {
                if (!isPlayableType(member.type) || harken::studio::types::IsNull(member.stableId)
                    || member.type == ProjectItem::Type::Layer)
                    continue;
                if (!claimIdentity(owners, member.stableId, pack.packStableId,
                                   harken::studio::build::kInitCatalogIdentityClassPlayable, error))
                    return false;
            }
        }
    }

    for (std::uint32_t i = 0; i < world.variableCount; ++i) {
        if (!claimIdentity(owners, world.variables[i].variableId, initId,
                           harken::studio::build::kInitCatalogIdentityClassVariable, error))
            return false;
    }
    for (std::uint32_t i = 0; i < world.playablePropertyCount; ++i) {
        const auto& property = world.playableProperties[i];
        if (property.contentPropertyKind
                != harken::studio::build::playableproperties::ContentPropertyKindV1::Layer)
            continue;
        if (!claimIdentity(owners, property.stableId, initId,
                           harken::studio::build::kInitCatalogIdentityClassPlayable, error))
            return false;
    }
    return true;
}

bool ownedBy(const std::unordered_map<harken::studio::types::ObjectId, IdentityOwner>& owners,
             harken::studio::types::ObjectId stableId, std::uint64_t packStableId)
{
    const auto it = owners.find(stableId);
    return it != owners.end() && it->second.packStableIds.count(packStableId) != 0;
}

bool hostOwnedByPack(const ProjectItem& host, const PackMeta& pack,
                     const std::unordered_map<harken::studio::types::ObjectId, IdentityOwner>& owners)
{
    if (pack.actionStableIds.empty())
        return false;
    const harken::studio::types::ObjectId id = itemStableId(host);
    return ownedBy(owners, id, pack.packStableId);
}

bool prepareOwnedPayload(harken::studio::build::PlayablePropertyPayloadViewV1* payload,
                         std::vector<harken::studio::build::SendV1>* ownedSends,
                         QString* error)
{
    if (payload == nullptr)
        return true;
    constexpr auto kOutputSlot =
        static_cast<std::size_t>(harken::studio::build::playableproperties::PlayablePropertyIdV1::OutputBusRef)
        - 1;
    const auto output = payload->values[kOutputSlot].objectId;
    if (payload->sendCount == 0 || payload->sends == nullptr)
        return true;
    if (ownedSends == nullptr) {
        if (error)
            *error = QStringLiteral("Send payload is missing owned storage");
        return false;
    }
    ownedSends->assign(payload->sends, payload->sends + payload->sendCount);
    for (auto& send : *ownedSends) {
        if (send.targetBusId == output) {
            if (error) {
                *error = QStringLiteral(
                    "Send target equals Output (%1); export refuses send==Output")
                    .arg(harken::studio::objectIdString(output));
            }
            return false;
        }
    }
    payload->sends = ownedSends->data();
    return true;
}

const harken::studio::build::PlayablePropertyProgramViewV1* findSlicedPlayable(
    const std::vector<harken::studio::build::PlayablePropertyProgramViewV1>& playables,
    harken::studio::types::ObjectId stableId)
{
    for (const auto& playable : playables) {
        if (playable.stableId == stableId)
            return &playable;
    }
    return nullptr;
}

void rebindPlayableRef(harken::studio::build::PlayableRefV1* ref,
                       const std::vector<harken::studio::build::PlayablePropertyProgramViewV1>& playables)
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

const harken::studio::build::TrackScheduleProgramViewV1* findSlicedTrack(
    const std::vector<harken::studio::build::TrackScheduleProgramViewV1>& tracks,
    harken::studio::types::ObjectId stableId)
{
    for (const auto& track : tracks) {
        if (track.rootDefinition.stableId == stableId)
            return &track;
    }
    return nullptr;
}

const harken::studio::build::SegmentScheduleProgramViewV1* findSlicedSegment(
    const std::vector<harken::studio::build::SegmentScheduleProgramViewV1>& segments,
    harken::studio::types::ObjectId stableId)
{
    for (const auto& segment : segments) {
        if (segment.rootDefinition.stableId == stableId)
            return &segment;
    }
    return nullptr;
}

void remapActionObjectRef(
    harken::studio::build::ActionObjectRefV1* ref,
    const std::vector<harken::studio::build::PlayablePropertyProgramViewV1>& playables)
{
    if (ref == nullptr || ref->stableId == harken::studio::types::ObjectId{})
        return;
    if (const auto* playable = findSlicedPlayable(playables, ref->stableId))
        ref->expectedProgramSemanticFingerprint = playable->programSemanticFingerprint;
}

bool preparePlayableAfterOwnedCopy(
    harken::studio::build::PlayablePropertyProgramViewV1* playable,
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

struct SlicedWorldIndex
{
    std::unordered_map<harken::studio::types::ObjectId,
        const harken::studio::build::PlayablePropertyProgramViewV1*> playables;
    std::unordered_map<harken::studio::types::ObjectId,
        const harken::studio::build::TrackScheduleProgramViewV1*> tracks;
    std::unordered_map<harken::studio::types::ObjectId,
        const harken::studio::build::SegmentScheduleProgramViewV1*> segments;
    std::unordered_map<harken::studio::types::ObjectId,
        const harken::studio::build::ActionProgramViewV1*> actions;

    SlicedWorldIndex(const std::vector<PackMeta>& packs, const QString& language)
    {
        for (const auto& pack : packs) {
            if (!pack.language.isEmpty() && pack.language != language)
                continue;
            for (const auto& playable : pack.slicedPlayables)
                playables.emplace(playable.stableId, &playable);
            for (const auto& track : pack.slicedTracks)
                tracks.emplace(track.rootDefinition.stableId, &track);
            for (const auto& segment : pack.slicedSegments)
                segments.emplace(segment.rootDefinition.stableId, &segment);
            for (const auto& action : pack.slicedActions)
                actions.emplace(action.actionStableId, &action);
        }
    }
};

const harken::studio::build::PlayablePropertyProgramViewV1* findLiveSlicedPlayable(
    const SlicedWorldIndex& index, harken::studio::types::ObjectId stableId)
{
    const auto found = index.playables.find(stableId);
    return found == index.playables.end() ? nullptr : found->second;
}

const harken::studio::build::TrackScheduleProgramViewV1* findLiveSlicedTrack(
    const SlicedWorldIndex& index, harken::studio::types::ObjectId stableId)
{
    const auto found = index.tracks.find(stableId);
    return found == index.tracks.end() ? nullptr : found->second;
}

const harken::studio::build::SegmentScheduleProgramViewV1* findLiveSlicedSegment(
    const SlicedWorldIndex& index, harken::studio::types::ObjectId stableId)
{
    const auto found = index.segments.find(stableId);
    return found == index.segments.end() ? nullptr : found->second;
}

const harken::studio::build::ActionProgramViewV1* findLiveSlicedAction(
    const SlicedWorldIndex& index, harken::studio::types::ObjectId stableId)
{
    const auto found = index.actions.find(stableId);
    return found == index.actions.end() ? nullptr : found->second;
}

void rebindPlayableRefLive(
    harken::studio::build::PlayableRefV1* ref,
    const SlicedWorldIndex& index)
{
    if (ref == nullptr)
        return;
    if (const auto* playable = findLiveSlicedPlayable(index, ref->stableId)) {
        ref->expectedProgramSemanticFingerprint = playable->programSemanticFingerprint;
        ref->contentRevision = playable->contentRevision;
        ref->routeStableId = playable->routeStableId;
        ref->compatibilityFingerprint = playable->compatibilityFingerprint;
    }
}

void remapActionObjectRefLive(
    harken::studio::build::ActionObjectRefV1* ref,
    const SlicedWorldIndex& index)
{
    if (ref == nullptr || ref->stableId == harken::studio::types::ObjectId{})
        return;
    if (const auto* playable = findLiveSlicedPlayable(index, ref->stableId)) {
        ref->expectedProgramSemanticFingerprint = playable->programSemanticFingerprint;
        return;
    }
    if (const auto* action = findLiveSlicedAction(index, ref->stableId))
        ref->expectedProgramSemanticFingerprint = action->programSemanticFingerprint;
}

void rebindPlayableCrossPackRefs(
    harken::studio::build::PlayablePropertyProgramViewV1* playable,
    const SlicedWorldIndex& index)
{
    if (playable == nullptr)
        return;
    if (playable->organizationLayerAncestorCount != 0
        && playable->organizationLayerAncestors != nullptr) {
        auto* ancestors = const_cast<harken::studio::build::LayerAncestorRefV1*>(
            playable->organizationLayerAncestors);
        for (std::uint32_t i = 0; i < playable->organizationLayerAncestorCount; ++i) {
            if (const auto* layer = findLiveSlicedPlayable(
                    index, ancestors[i].layerStableId))
                ancestors[i].layerProgramSemanticFingerprint =
                    layer->programSemanticFingerprint;
        }
    }
    if (playable->structuralAncestorCount != 0
        && playable->structuralAncestors != nullptr) {
        auto* ancestors = const_cast<harken::studio::build::StructuralAncestorRefV1*>(
            playable->structuralAncestors);
        for (std::uint32_t i = 0; i < playable->structuralAncestorCount; ++i) {
            const auto* current = findLiveSlicedPlayable(index, ancestors[i].stableId);
            if (current == nullptr)
                continue;
            ancestors[i].programSemanticFingerprint = current->programSemanticFingerprint;
            ancestors[i].payload =
                harken::studio::build::MakeStructuralAncestorPayloadV1(current->payload);
        }
    }
    playable->programSemanticFingerprint =
        harken::studio::build::ComputePlayablePropertyFingerprintV1(*playable);
}

void rebindLayerAncestorFingerprints(
    std::vector<harken::studio::build::PlayablePropertyProgramViewV1>* playables)
{
    if (playables == nullptr)
        return;
    for (auto& playable : *playables) {
        if (playable.organizationLayerAncestorCount != 0
            && playable.organizationLayerAncestors != nullptr) {
            auto* ancestors = const_cast<harken::studio::build::LayerAncestorRefV1*>(
                playable.organizationLayerAncestors);
            for (std::uint32_t i = 0; i < playable.organizationLayerAncestorCount; ++i) {
                if (const auto* layer = findSlicedPlayable(*playables, ancestors[i].layerStableId))
                    ancestors[i].layerProgramSemanticFingerprint =
                        layer->programSemanticFingerprint;
            }
        }
        playable.programSemanticFingerprint =
            harken::studio::build::ComputePlayablePropertyFingerprintV1(playable);
    }
}

bool remapScheduleReferenceHintLive(
    harken::studio::build::ChildTimingHintV2* hint,
    const SlicedWorldIndex& index,
    QString* error)
{
    if (hint == nullptr
        || hint->kind != harken::studio::build::ChildTimingHintKindV2::ScheduleReference)
        return true;
    auto& key = hint->payload.schedule;
    if (key.kind == harken::studio::build::PlayableKindV1::Track) {
        const auto* track = findLiveSlicedTrack(index, key.stableId);
        if (track == nullptr) {
            if (error)
                *error = QStringLiteral(
                    "Schedule reference track %1 is missing from the sliced canonical set")
                    .arg(harken::studio::objectIdString(key.stableId));
            return false;
        }
        key.scheduleSemanticFingerprint = track->scheduleSemanticFingerprint;
        return true;
    }
    if (key.kind == harken::studio::build::PlayableKindV1::Segment) {
        const auto* segment = findLiveSlicedSegment(index, key.stableId);
        if (segment == nullptr) {
            if (error)
                *error = QStringLiteral(
                    "Schedule reference segment %1 is missing from the sliced canonical set")
                    .arg(harken::studio::objectIdString(key.stableId));
            return false;
        }
        key.scheduleSemanticFingerprint = segment->scheduleSemanticFingerprint;
        return true;
    }
    if (error)
        *error = QStringLiteral(
            "Schedule reference %1 has unsupported kind %2")
            .arg(harken::studio::objectIdString(key.stableId))
            .arg(static_cast<std::uint32_t>(key.kind));
    return false;
}

bool sliceWorld(
    const harken::studio::build::ContainerWorldInstallViewV1& world, std::uint64_t packStableId,
    const std::unordered_map<harken::studio::types::ObjectId, IdentityOwner>& owners,
    std::vector<harken::studio::build::VariableDefinitionV1>* variables,
    std::vector<harken::studio::build::PlayablePropertyProgramViewV1>* playables,
    std::vector<harken::studio::build::PlayableOccurrencePropertyViewV1>* occurrences,
    std::vector<harken::studio::build::TrackScheduleProgramViewV1>* tracks,
    std::vector<harken::studio::build::SegmentScheduleProgramViewV1>* segments,
    std::vector<harken::studio::build::ContainerProgramViewV3>* containers,
    std::vector<harken::studio::build::TransitionProgramViewV1>* transitions,
    std::vector<harken::studio::build::ActionProgramViewV1>* actions,
    PackMeta* pack,
    QString* error)
{
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
            harken::studio::build::ComputePlayablePropertyFingerprintV1(playable);
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
            harken::studio::build::ComputePlayableOccurrenceFingerprintV1(occurrence);
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
            harken::studio::build::ComputeTrackScheduleFingerprintV1(track);
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
            harken::studio::build::ComputeSegmentScheduleFingerprintV1(segment);
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
        harken::studio::build::ContainerProgramViewV3 container = world.containers[i];
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
            [&](const harken::studio::build::TransitionProgramViewV1& candidate) {
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
                harken::studio::build::ComputeActionProgramFingerprintV1(action);
        }
        actions->push_back(action);
    }

    return true;
}

void setPartitionSignals(harken::studio::build::ContainerWorldInstallViewV1& slice,
                         std::span<const harken::studio::build::SignalRelationV1> relations,
                         std::span<const harken::studio::build::SignalReceiverV1> receivers)
{
    slice.signalRelations = relations.empty() ? nullptr : relations.data();
    slice.signalRelationCount = static_cast<std::uint32_t>(relations.size());
    slice.signalReceivers = receivers.empty() ? nullptr : receivers.data();
    slice.signalReceiverCount = static_cast<std::uint32_t>(receivers.size());
    slice.sourceBodyVintage = relations.empty() && receivers.empty()
        ? harken::studio::build::kCurrentWorldShippingBodyVintage
        : harken::studio::build::kCurrentWorldBodyVintageV20;
    slice.identity.capabilityMask = harken::studio::build::detail::ComputeCurrentWorldLocalCapabilityMaskV1(slice);
    slice.identity.runtimeSemanticFingerprint = harken::studio::build::ComputeCurrentWorldBodyFingerprint(slice);
}

bool encodePartition(std::uint64_t packStableId,
                     const harken::studio::build::ContainerWorldInstallViewV1& slice,
                     std::uint32_t ownedDefinitions,
                     std::vector<std::uint8_t>* payload, QString* error,
                     harken::studio::build::CanonicalBusTriple busTriple,
                     std::uint64_t partitionRevision = 1)
{
    std::vector<std::uint8_t> body;
    if (!harken::studio::build::EncodeCurrentWorldPartitionBody(slice, body)) {
        if (error)
            *error = QStringLiteral("Failed to encode definition partition");
        return false;
    }
    harken::studio::build::PackCanonicalPartitionHeader header;
    header.headerBytes = harken::studio::build::kPackBytes<HkPackCanonicalPartitionHeader>;
    header.bodyVintage = slice.sourceBodyVintage;
    header.bodyBytes = body.size();
    header.bodyChecksum = harken::studio::build::Fnv1a64(body.data(), body.size());
    header.packStableId = packStableId;
    header.partitionRevision = partitionRevision;
    header.ownedDefinitionCount = ownedDefinitions;
    harken::studio::build::ApplyCanonicalBusTriple(&header, busTriple);
    payload->assign(harken::studio::build::kPackBytes<HkPackCanonicalPartitionHeader> + body.size(), 0);
    harken::studio::build::SerializePackRow<HkPackCanonicalPartitionHeader>(header, payload->data());
    header.headerChecksum = harken::studio::build::Fnv1a64(payload->data(), offsetof(HkPackCanonicalPartitionHeader, headerChecksum));
    harken::studio::build::SerializePackRow<HkPackCanonicalPartitionHeader>(header, payload->data());
    if (!body.empty()) {
        std::memcpy(payload->data() + harken::studio::build::kPackBytes<HkPackCanonicalPartitionHeader>,
                    body.data(), body.size());
    }
    return true;
}

std::uint64_t lineageFingerprint(
    const std::unordered_map<harken::studio::types::ObjectId, IdentityOwner>& owners,
    std::uint64_t packStableId)
{
    std::vector<harken::studio::types::ObjectId> ids;
    for (const auto& [stableId, owner] : owners) {
        if (owner.packStableIds.count(packStableId) != 0)
            ids.push_back(stableId);
    }
    std::sort(ids.begin(), ids.end());
    std::vector<std::uint8_t> bytes;
    bytes.reserve(ids.size() * 16);
    for (const auto id : ids) {
        const auto encoded = harken::studio::types::ObjectIdBytes(id);
        bytes.insert(bytes.end(), encoded.begin(), encoded.end());
    }
    return harken::studio::build::Fnv1a64(bytes.data(), bytes.size());
}

std::uint32_t ownedDefinitionCount(
    const std::unordered_map<harken::studio::types::ObjectId, IdentityOwner>& owners,
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

bool decodePreviousPack(const PackMeta& pack, harken::studio::build::CurrentWorldBodyImage* body)
{
    const auto found = std::find_if(pack.previousEntries.begin(), pack.previousEntries.end(), [](const auto& entry) {
        return entry.kind == HK_PACK_ENTRY_KIND_CANONICAL_DEFINITION_PARTITION;
    });
    if (found == pack.previousEntries.end()) return false;
    harken::studio::build::PackCanonicalPartitionHeader header;
    return harken::studio::build::ReadPackCanonicalPartitionHeader(
            found->payload.data(), found->payload.size(), &header)
        && body->Decode(found->payload.data() + header.headerBytes,
            static_cast<std::size_t>(header.bodyBytes), header.bodyVintage);
}

struct PublicationAuthoredOrders
{
    QString language;
    std::map<harken::studio::types::ObjectId, std::uint32_t> properties;
    std::map<harken::studio::types::ObjectId, std::uint32_t> containers;
};

std::vector<PublicationAuthoredOrders> readPublicationAuthoredOrders(
    const QMap<QString, PackMeta>& publication,
    std::set<std::uint32_t>* reservedProperties,
    std::set<std::uint32_t>* reservedContainers)
{
    std::vector<PublicationAuthoredOrders> result;
    for (const auto& pack : publication) {
        harken::studio::build::CurrentWorldBodyImage body;
        if (!decodePreviousPack(pack, &body)) continue;
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
                                    const std::set<harken::studio::types::ObjectId>& neutralDefinitions,
                                    std::map<harken::studio::types::ObjectId, std::uint32_t>* properties,
                                    std::map<harken::studio::types::ObjectId, std::uint32_t>* containers,
                                    QString* error)
{
    std::map<std::uint32_t, harken::studio::types::ObjectId> propertyOwners, containerOwners;
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

bool mergeCapacity(harken::studio::build::WorldCapacitySummaryV1* target,
                   harken::studio::build::WorldCapacitySummaryV1 source)
{
    CapacityFields destinationFields, sourceFields;
    harken::studio::build::VisitWorldCapacitySummaryV1(*target, destinationFields);
    harken::studio::build::VisitWorldCapacitySummaryV1(source, sourceFields);
    bool changed = false;
    for (std::size_t i = 0; i < destinationFields.values.size(); ++i) {
        auto& value = *destinationFields.values[i];
        const auto required = *sourceFields.values[i];
        changed = changed || value < required;
        value = std::max(value, required);
    }
    return changed;
}

std::set<harken::studio::types::ObjectId> effectPresetReferences(
    const harken::studio::build::ContainerWorldInstallViewV1& world)
{
    std::set<harken::studio::types::ObjectId> references;
    const auto collect = [&](const auto& payload) {
        for (std::uint32_t i = 0; i < payload.effectNodeCount; ++i)
            if (!harken::studio::types::IsNull(payload.effectNodes[i].presetId))
                references.insert(payload.effectNodes[i].presetId);
    };
    for (std::uint32_t i = 0; i < world.playablePropertyCount; ++i) {
        const auto& property = world.playableProperties[i];
        collect(property.payload);
        for (std::uint32_t j = 0; j < property.structuralAncestorCount; ++j)
            collect(property.structuralAncestors[j].payload);
    }
    for (std::uint32_t i = 0; i < world.playableOccurrenceCount; ++i)
        collect(world.playableOccurrences[i].payload);
    return references;
}

bool harmonizePublicationCapacity(std::vector<PackMeta>* packs,
                                  PackBuildState* buildState,
                                  const std::map<harken::studio::types::ObjectId, std::vector<std::uint8_t>>& authoredPresets,
                                   std::map<harken::studio::types::ObjectId, harken::studio::build::SignalReceiverV1> receivers,
                                  QString* error)
{
    std::vector<std::unique_ptr<harken::studio::build::CurrentWorldBodyImage>> bodies;
    std::vector<harken::studio::build::PackCanonicalPartitionHeader> headers;
    harken::studio::build::WorldCapacitySummaryV1 common;
    QStringList languages{QString()};
    std::map<harken::studio::types::ObjectId, std::vector<std::uint8_t>> presets;
    std::set<harken::studio::types::ObjectId> receiverTargets;
    for (const auto& pack : *packs) {
        if (pack.packKind != HK_PACK_KIND_INIT) continue;
        const auto& entries = pack.retained ? pack.previousEntries : pack.resourceEntries;
        for (const auto& entry : entries)
            if (entry.kind == HK_PACK_ENTRY_KIND_EFFECT_PRESET)
                presets.emplace(entry.id, entry.payload);
    }
    for (auto& pack : *packs) {
        auto body = std::make_unique<harken::studio::build::CurrentWorldBodyImage>();
        if (pack.retained) {
            if (!decodePreviousPack(pack, body.get())) {
                *error = QStringLiteral("Cannot read retained Pack '%1'").arg(pack.fileName);
                return false;
            }
            pack.previousValid = true;
            if (buildState)
                pack.resourceFingerprint = buildState->resourceFingerprint(pack.fileName);
            for (const auto& entry : pack.previousEntries) {
                if (entry.kind == HK_PACK_ENTRY_KIND_CANONICAL_DEFINITION_PARTITION)
                    pack.partitionPayload = entry.payload;
                else if (entry.kind == HK_PACK_ENTRY_KIND_VARIABLE_POLICY_OVERLAY)
                    pack.variablePolicyOverlayPayload = entry.payload;
                else if (entry.kind == HK_PACK_ENTRY_KIND_VARIABLE_REQUIREMENTS)
                    pack.variableRequirementsPayload = entry.payload;
                else if (entry.kind == HK_PACK_ENTRY_KIND_PACK_REQUIREMENT_BLOCK) {
                    harken::studio::build::PackRequirementBlock requirement;
                    if (!harken::studio::build::ReadPackRequirementBlock(
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
        harken::studio::build::PackCanonicalPartitionHeader header;
        if (!harken::studio::build::ReadPackCanonicalPartitionHeader(
                pack.partitionPayload.data(), pack.partitionPayload.size(), &header)
            || header.headerBytes > pack.partitionPayload.size()
            || header.bodyBytes != pack.partitionPayload.size() - header.headerBytes
            || (!pack.retained && !body->Decode(
                pack.partitionPayload.data() + header.headerBytes,
                static_cast<std::size_t>(header.bodyBytes), header.bodyVintage))) {
            *error = QStringLiteral("Cannot decode Pack capacity for '%1'").arg(pack.fileName);
            return false;
        }
        for (const auto& presetId : effectPresetReferences(*body->ProductionView())) {
            const auto preset = presets.find(presetId);
            if (preset == presets.end()) {
                *error = QStringLiteral("Pack '%1' references missing Effect preset %2; re-export Init with its Actions")
                    .arg(pack.fileName, objectIdString(presetId));
                return false;
            }
            if (pack.retained) continue;
            const auto authored = authoredPresets.find(presetId);
            if (authored == authoredPresets.end() || authored->second != preset->second) {
                *error = QStringLiteral("Pack '%1' requires an updated Effect preset %2; re-export Init with its Actions")
                    .arg(pack.fileName, objectIdString(presetId));
                return false;
            }
        }
        const auto& view = *body->ProductionView();
        if (view.signalRelationCount && pack.packKind != HK_PACK_KIND_INIT
            && !harken::studio::build::CanonicalBusTripleEqual(
                harken::studio::build::PartitionBusTriple(header),
                harken::studio::build::PartitionBusTriple(headers.front()))) {
            *error = QStringLiteral("Ducking in Pack '%1' requires an updated Bus snapshot. Re-export Init with its Actions.")
                .arg(pack.fileName);
            return false;
        }
        for (std::uint32_t i = 0; i < view.signalRelationCount; ++i)
            receiverTargets.insert(view.signalRelations[i].target.stableId);
        for (std::uint32_t i = 0; i < view.signalReceiverCount; ++i)
            receivers.try_emplace(view.signalReceivers[i].target.stableId, view.signalReceivers[i]);
        mergeCapacity(&common, view.capacity);
        if (pack.previousValid) mergeCapacity(&common, pack.previousCapacity);
        if (!languages.contains(pack.language)) languages.append(pack.language);
        headers.push_back(header);
        bodies.push_back(std::move(body));
    }
    for (const auto& language : languages) {
        std::vector<harken::studio::build::VariableDefinitionV1> variables;
        std::vector<harken::studio::build::PlayablePropertyProgramViewV1> properties;
        std::vector<harken::studio::build::PlayableOccurrencePropertyViewV1> occurrences;
        std::vector<harken::studio::build::ContainerProgramViewV3> containers;
        std::vector<harken::studio::build::ActionProgramViewV1> actions;
        std::vector<harken::studio::build::TrackScheduleProgramViewV1> tracks;
        std::vector<harken::studio::build::SegmentScheduleProgramViewV1> segments;
        std::set<harken::studio::types::ObjectId> variableIds, propertyIds, containerIds, actionIds;
        std::set<harken::studio::types::ObjectId> trackIds, segmentIds;
        std::set<std::tuple<std::uint32_t, harken::studio::types::ObjectId, std::uint32_t>> occurrenceIds;
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
        if (variables.size() > harken::studio::build::kHardVariableDefinitionCapacity
            || properties.size() > harken::studio::build::kHardPlayablePropertyProgramCapacity
            || occurrences.size() > harken::studio::build::kHardPlayableOccurrencePropertyCapacity
            || containers.size() > harken::studio::build::kHardContainerProgramCapacity
            || actions.size() > harken::studio::build::kHardActionDefinitionCapacity
            || tracks.size() > harken::studio::build::kHardTrackScheduleProgramCapacity
            || segments.size() > harken::studio::build::kHardSegmentScheduleProgramCapacity) {
            *error = QStringLiteral("Published Pack set exceeds World representation limits");
            return false;
        }
        harken::studio::build::ContainerWorldInstallViewV1 combined;
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
        if (!harken::studio::build::GrowAuthoredWorldCapacityV1(combined, &common)) {
            *error = QStringLiteral("Published Pack set exceeds World content capacity");
            return false;
        }
    }
    std::vector<harken::studio::build::SignalReceiverV1> initReceivers;
    for (const auto& target : receiverTargets) {
        const auto receiver = receivers.find(target);
        if (receiver == receivers.end()) {
            *error = QStringLiteral("Missing Ducking receiver for Bus %1").arg(objectIdString(target));
            return false;
        }
        initReceivers.push_back(receiver->second);
    }
    for (std::size_t i = 0; i < packs->size(); ++i) {
        auto& pack = (*packs)[i];
        auto world = *bodies[i]->ProductionView();
        const auto previousFingerprint = world.identity.runtimeSemanticFingerprint;
        const bool capacityChanged = mergeCapacity(&world.capacity, common);
        if (pack.packKind == HK_PACK_KIND_INIT) {
            setPartitionSignals(world, {world.signalRelations, world.signalRelationCount}, initReceivers);
        } else if (capacityChanged) {
            world.identity.runtimeSemanticFingerprint = harken::studio::build::ComputeCurrentWorldBodyFingerprint(world);
        }
        if (!capacityChanged && world.identity.runtimeSemanticFingerprint == previousFingerprint) continue;
        if (!encodePartition(pack.packStableId, world, pack.ownedDefinitions,
                &pack.partitionPayload, error,
                harken::studio::build::PartitionBusTriple(headers[i]),
                headers[i].partitionRevision))
            return false;
        pack.semanticFingerprint = harken::studio::build::Fnv1a64(
            pack.partitionPayload.data() + harken::studio::build::kPackBytes<HkPackCanonicalPartitionHeader>,
            pack.partitionPayload.size() - harken::studio::build::kPackBytes<HkPackCanonicalPartitionHeader>);
        pack.metadataUpdated = pack.retained;
    }
    return true;
}

bool readExistingPack(const QDir& output, const QString& path,
                      const QString& projectId, const QString& platformId,
                      PackMeta* pack, QString* error)
{
    QByteArray bytes;
    harken::studio::build::PackCanonicalImage image;
    harken::studio::build::CurrentWorldBodyImage body;
    if (!safePublicationPath(output, path)
        || !readAll(output.filePath(path), &bytes, error)) return false;
    if (harken::studio::build::ParsePackCanonicalImage(
            reinterpret_cast<const std::uint8_t*>(bytes.constData()), bytes.size(), &image)
                != harken::studio::build::kPackOk
        || !body.Decode(image.partitionBody.data(), image.partitionBody.size(),
            image.partitionHeader.bodyVintage)) {
        *error = QStringLiteral("Cannot read existing Pack '%1'").arg(path);
        return false;
    }
    if (body.ProductionView()->identity.projectId != runtimeObjectId(QUuid(projectId))) {
        *error = QStringLiteral("Pack '%1' belongs to another project").arg(path);
        return false;
    }
    const QFileInfo file(path);
    const QString packMetadataPath = QDir::cleanPath(QDir(file.path()).filePath(file.completeBaseName() + QStringLiteral(".json")));
    QByteArray packMetadataBytes;
    if (!readAll(output.filePath(packMetadataPath), &packMetadataBytes, error)) return false;
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(packMetadataBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        *error = QStringLiteral("Cannot read existing Pack metadata '%1'").arg(packMetadataPath);
        return false;
    }
    if (!collectEntries(bytes, output.filePath(path), &pack->previousEntries, error, false, false)) return false;
    pack->fileName = path;
    pack->formatVersion = HK_PACK_FORMAT_VERSION;
    pack->fileSize = bytes.size();
    pack->packStableId = image.packStableId;
    pack->packKind = image.packKind;
    pack->contentRevision = image.requirement.packContentRevision;
    pack->language = file.path() == QStringLiteral(".") ? QString() : file.path();
    pack->platform = platformId;
    pack->packMetadata = document.object();
    pack->name = image.packKind == HK_PACK_KIND_INIT
        ? QStringLiteral("Init") : pack->packMetadata.value(QStringLiteral("name")).toString();
    pack->unitKey = image.packKind == HK_PACK_KIND_INIT
        ? QStringLiteral("Init") : QStringLiteral("a:") + pack->packMetadata.value(QStringLiteral("guid")).toString();
    if (image.packKind != HK_PACK_KIND_INIT)
        pack->actionStableIds.push_back(runtimeObjectId(QUuid(pack->packMetadata.value(QStringLiteral("guid")).toString())));
    QJsonArray dependencies;
    for (const auto& edge : image.dependencyEdges) {
        pack->dependencies.push_back(edge.prerequisitePackStableId);
        dependencies.append(QString::number(edge.prerequisitePackStableId));
    }
    const auto& world = *body.ProductionView();
    for (std::uint32_t i = 0; i < world.playablePropertyCount; ++i) {
        const auto& property = world.playableProperties[i];
        if (property.contentPropertyKind == harken::studio::build::playableproperties::ContentPropertyKindV1::Sound)
            pack->soundContentRevisions.emplace(property.stableId, property.contentRevision);
    }
    pack->previousCapacity = body.ProductionView()->capacity;
    pack->previousValid = true;
    pack->previousRow = QJsonObject{
        {QStringLiteral("path"), path}, {QStringLiteral("name"), pack->name},
        {QStringLiteral("packStableId"), QString::number(pack->packStableId)},
        {QStringLiteral("packKind"), static_cast<int>(pack->packKind)},
        {QStringLiteral("contentRevision"), static_cast<qint64>(pack->contentRevision)},
        {QStringLiteral("dependencies"), dependencies},
        {QStringLiteral("language"), pack->language}, {QStringLiteral("packMetadata"), pack->packMetadata}};
    return true;
}

bool inspectPackPublication(const QString& directory, const QString& projectId,
                            const QString& platformId, QJsonArray* packs,
                            QStringList* managedFiles, QString* error, bool requireComplete,
                            const QStringList& expectedPackPaths, bool allowPlatformReplacement,
                            const std::function<bool()>& cancelCheck);

}

bool ReadPackPublication(const QString& directory, const QString& projectId,
                         const QString& platformId, QJsonArray* packs,
                         QStringList* managedFiles, QString* error, bool requireComplete,
                         const QStringList& expectedPackPaths, bool allowPlatformReplacement,
                         const std::function<bool()>& cancelCheck)
{
    return inspectPackPublication(directory, projectId, platformId, packs, managedFiles,
        error, requireComplete, expectedPackPaths, allowPlatformReplacement, cancelCheck);
}

namespace {

bool inspectPackPublication(const QString& directory, const QString& projectId,
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
    if (!scanOutputFiles(dir, &files, &binaries, &detail, cancelCheck)) return reject(detail);
    for (const auto& path : expectedPackPaths)
        if (!safePublicationPath(dir, path))
            return reject(QStringLiteral("Unsafe requested Pack path '%1'").arg(path));
    if (!expectedPackPaths.isEmpty())
        std::stable_sort(binaries.begin(), binaries.end(), [&](const auto& left, const auto& right) {
            return expectedPackPaths.contains(left) && !expectedPackPaths.contains(right);
        });
    harken::studio::types::ObjectId expectedProject = runtimeObjectId(QUuid(projectId));
    std::uint64_t initId = 0;
    bool selectedInit = false;
    QSet<QString> selectedPaths;
    QSet<qulonglong> ids;
    std::unordered_map<std::uint64_t, std::uint32_t> kinds;
    std::vector<harken::studio::build::PackDependencyEdge> dependencyEdges;
    std::set<harken::studio::types::ObjectId> presetIds, presetReferences;
    QSet<QString> names;
    QSet<QString> actionIds;
    QSet<QString> localizedNames;
    QSet<QString> localizedActionIds;
    for (const auto& path : binaries) {
        if (exportCanceled(cancelCheck, error)) return false;
        const auto info = QFileInfo(path);
        const bool selected = expectedPackPaths.isEmpty() || expectedPackPaths.contains(path);
        const QString packMetadataPath = QDir::cleanPath(QDir(info.path()).filePath(info.completeBaseName() + QStringLiteral(".json")));
        const bool hasPackMetadata = files.contains(packMetadataPath);
        QByteArray bytes;
        harken::studio::build::PackCanonicalImage image;
        harken::studio::build::CurrentWorldBodyImage body;
        const bool parsed = readAll(dir.filePath(path), &bytes, &detail)
            && harken::studio::build::ParsePackCanonicalImage(reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                bytes.size(), &image) == harken::studio::build::kPackOk;
        const bool decoded = parsed
            && body.Decode(image.partitionBody.data(), image.partitionBody.size(),
                image.partitionHeader.bodyVintage);
        if (!decoded) {
            if (requireComplete && selected && hasPackMetadata)
                return reject(QStringLiteral("Invalid published Pack '%1'").arg(path));
            continue;
        }
        const auto& world = *body.ProductionView();
        const auto pairProject = world.identity.projectId;
        if (!harken::studio::types::IsNull(pairProject) && !harken::studio::types::IsNull(expectedProject)
            && pairProject != expectedProject)
            return reject(QStringLiteral("Pack publication contains another project's content: %1").arg(path));
        if (selected && !platformId.isEmpty() && (requireComplete || !allowPlatformReplacement)) {
            const auto expectedInit = fnvName(platformId + QStringLiteral("/Init"));
            if (image.packKind == HK_PACK_KIND_INIT && image.packStableId != expectedInit)
                return reject(QStringLiteral("Pack publication contains another platform's content: %1").arg(path));
            for (const auto& edge : image.dependencyEdges)
                if (edge.edgeKind == HK_PACK_DEPENDENCY_EDGE_KIND_INIT
                    && edge.prerequisitePackStableId != expectedInit
                    && harken::studio::build::ValidateCanonicalInitDependency(
                        image.dependencyEdges, edge.prerequisitePackStableId) == harken::studio::build::kPackOk)
                    return reject(QStringLiteral("Pack publication contains another platform's content: %1").arg(path));
        }
        if (!hasPackMetadata) continue;
        const auto hash = harken::studio::build::Fnv1a64(bytes.constData(), bytes.size());
        QJsonObject row;
        QString pairError;
        const auto inspectPair = [&] {
            const auto rejectPair = [&](const QString& message) {
                pairError = message;
                return false;
            };
            QJsonObject packMetadata;
            QByteArray packMetadataBytes;
            if (!readAll(dir.filePath(packMetadataPath), &packMetadataBytes, &detail)) return rejectPair(detail);
            QJsonParseError parseError;
            const auto document = QJsonDocument::fromJson(packMetadataBytes, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject())
                return rejectPair(QStringLiteral("Invalid Pack metadata '%1'").arg(packMetadataPath));
            packMetadata = document.object();
            const auto packMetadataErrors = ProjectSchemaValidator::validatePackMetadata(
                HARKEN_PACK_METADATA_VERSION, packMetadata, packMetadataPath);
            if (!packMetadataErrors.isEmpty()) return rejectPair(packMetadataErrors.join(QLatin1Char('\n')));
            if (harken::studio::types::IsNull(pairProject))
                return rejectPair(QStringLiteral("Pack publication contains an invalid project identity: %1").arg(path));
            if (requireComplete && selected && ids.contains(image.packStableId))
                return rejectPair(QStringLiteral("Pack publication repeats a Pack identity: %1").arg(path));
            QString name;
            if (image.packKind == HK_PACK_KIND_INIT) {
                if ((selected && initId != 0) || path != QStringLiteral("Init.bytes")
                    || (!platformId.isEmpty() && image.packStableId != fnvName(platformId + QStringLiteral("/Init")))
                    || packMetadata.size() != 2 + packMetadata.contains(QStringLiteral("waveforms"))
                        + packMetadata.contains(QStringLiteral("metadata"))
                    || !packMetadata.value(QStringLiteral("variables")).isArray())
                    return rejectPair(QStringLiteral("Invalid Init publication in '%1'").arg(directory));
                name = QStringLiteral("Init");
                const auto variables = packMetadata.value(QStringLiteral("variables")).toArray();
                if (variables.size() != world.variableCount)
                    return rejectPair(QStringLiteral("Init Pack metadata does not match its Variable definitions"));
                QSet<QString> variableNames;
                std::set<harken::studio::types::ObjectId> variableIds;
                for (const auto& value : variables) {
                    const auto variable = value.toObject();
                    const QString variableName = variable.value(QStringLiteral("name")).toString();
                    const QString guid = variable.value(QStringLiteral("guid")).toString();
                    const auto id = runtimeObjectId(QUuid(guid));
                    if (variable.size() != 3 || !NameValidator::validateItemName(variableName, ProjectItem::Type::Variable)
                        || harken::studio::types::IsNull(id) || objectIdString(id) != guid
                        || variableNames.contains(variableName) || !variableIds.insert(id).second
                        || !variable.value(QStringLiteral("labels")).isArray()
                        || std::none_of(world.variables, world.variables + world.variableCount,
                            [&](const auto& definition) { return definition.variableId == id; }))
                        return rejectPair(QStringLiteral("Invalid Variable in Init Pack metadata"));
                    variableNames.insert(variableName);
                    const auto overlay = std::find_if(image.variablePolicyOverlays.begin(), image.variablePolicyOverlays.end(),
                        [&](const auto& row) { return row.variableId == id; });
                    const auto labels = variable.value(QStringLiteral("labels")).toArray();
                    if (overlay == image.variablePolicyOverlays.end())
                        return rejectPair(QStringLiteral("Init Pack metadata does not match its Label definitions"));
                    QSet<QString> labelNames;
                    std::set<harken::studio::types::ObjectId> labelIds;
                    for (const auto& labelValue : labels) {
                        const auto label = labelValue.toObject();
                        const QString labelName = label.value(QStringLiteral("name")).toString();
                        const QString labelGuid = label.value(QStringLiteral("guid")).toString();
                        const auto labelId = runtimeObjectId(QUuid(labelGuid));
                        if (label.size() != 2 || labelName.isEmpty() || harken::studio::types::IsNull(labelId)
                            || objectIdString(labelId) != labelGuid || labelNames.contains(labelName)
                            || !labelIds.insert(labelId).second
                            || std::none_of(overlay->labels.begin(), overlay->labels.end(),
                                [&](const auto& row) { return row.labelId == labelId; }))
                            return rejectPair(QStringLiteral("Invalid Label in Init Pack metadata"));
                        labelNames.insert(labelName);
                    }
                }
            } else {
                name = packMetadata.value(QStringLiteral("name")).toString();
                const QString guid = packMetadata.value(QStringLiteral("guid")).toString();
                const auto id = runtimeObjectId(QUuid(guid));
                const QString scope = info.path() == QStringLiteral(".") ? QString() : info.path();
                if (image.packKind != HK_PACK_KIND_CANONICAL_CONTENT
                    || packMetadata.size() != 3 + packMetadata.contains(QStringLiteral("waveforms"))
                        + packMetadata.contains(QStringLiteral("metadata"))
                    || !NameValidator::validateItemName(name, ProjectItem::Type::Action)
                    || harken::studio::types::IsNull(id) || objectIdString(id) != guid || world.actionCount == 0
                    || std::none_of(world.actions, world.actions + world.actionCount,
                        [&](const auto& action) { return action.actionStableId == id; })
                    || (!platformId.isEmpty() && image.packStableId != fnvName(PackBuildPlanning::relativePackPath(
                        platformId, scope, PackBuildPlanning::stableArtifactKey(id))))
                    || (requireComplete && selected && (names.contains(scope + QLatin1Char('/') + name)
                    || actionIds.contains(scope + QLatin1Char('/') + guid)
                    || (!scope.isEmpty() && (names.contains(QLatin1Char('/') + name)
                        || actionIds.contains(QLatin1Char('/') + guid)))
                    || (scope.isEmpty() && (localizedNames.contains(name) || localizedActionIds.contains(guid))))))
                    return rejectPair(QStringLiteral("Invalid Action Pack metadata '%1'").arg(packMetadataPath));
            }
            if (packMetadata.contains(QStringLiteral("metadata"))) {
                const auto metadata = packMetadata.value(QStringLiteral("metadata")).toObject();
                const auto language = info.path() == QStringLiteral(".") ? QString() : info.path();
                if (metadata.value(QStringLiteral("packStableId")).toString() != QString::number(image.packStableId)
                    || metadata.value(QStringLiteral("language")).toString() != language)
                    return rejectPair(QStringLiteral("Pack metadata identity does not match '%1'").arg(packMetadataPath));
                auto expectedObjects = packMetadataObjectIds(image, world);
                for (const auto& value : metadata.value(QStringLiteral("objects")).toArray()) {
                    const auto object = value.toObject();
                    const auto id = runtimeObjectId(QUuid(object.value(QStringLiteral("guid")).toString()));
                    if (expectedObjects.erase(id) != 1
                        || (!image.hasBusDefinitionSnapshot
                            && object.value(QStringLiteral("type")).toString() == QStringLiteral("Bus")))
                        return rejectPair(QStringLiteral("Invalid metadata object in '%1'").arg(packMetadataPath));
                }
                if (!expectedObjects.empty())
                    return rejectPair(QStringLiteral("Pack metadata does not cover the dependencies in '%1'").arg(packMetadataPath));
            }
            std::set<harken::studio::types::ObjectId> waveformSounds;
            for (const auto& value : packMetadata.value(QStringLiteral("waveforms")).toArray()) {
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
                    return rejectPair(QStringLiteral("Invalid waveform index in '%1'").arg(packMetadataPath));
            }
            if (packMetadata.contains(QStringLiteral("waveforms")) && waveformSounds.size() != image.media.size())
                return rejectPair(QStringLiteral("Waveform index does not cover the Pack media in '%1'").arg(packMetadataPath));
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
                {QStringLiteral("platformId"), platformId}, {QStringLiteral("packMetadata"), packMetadata}};
            return true;
        };
        if (!inspectPair()) {
            if (requireComplete && selected) return reject(pairError);
            continue;
        }
        if (harken::studio::types::IsNull(expectedProject) && selected) expectedProject = pairProject;
        if (selected && image.packKind == HK_PACK_KIND_INIT) {
            initId = image.packStableId;
            selectedInit = true;
        } else if (selected) {
            const QString name = row.value(QStringLiteral("name")).toString();
            const QString guid = row.value(QStringLiteral("packMetadata")).toObject().value(QStringLiteral("guid")).toString();
            const QString scope = row.value(QStringLiteral("language")).toString();
            names.insert(scope + QLatin1Char('/') + name);
            actionIds.insert(scope + QLatin1Char('/') + guid);
            if (!scope.isEmpty()) {
                localizedNames.insert(name);
                localizedActionIds.insert(guid);
            }
        }
        managedFiles->append(path);
        managedFiles->append(packMetadataPath);
        if (!selected) continue;
        selectedPaths.insert(path);
        ids.insert(image.packStableId);
        kinds.emplace(image.packStableId, image.packKind);
        dependencyEdges.insert(dependencyEdges.end(), image.dependencyEdges.begin(), image.dependencyEdges.end());
        for (const auto& preset : image.effectPresets)
            presetIds.insert(preset.header.presetId);
        const auto references = effectPresetReferences(world);
        presetReferences.insert(references.begin(), references.end());
        packs->append(row);
    }
    if (requireComplete) {
        for (const auto& presetId : presetReferences)
            if (presetIds.count(presetId) == 0)
                return reject(QStringLiteral("Pack publication references missing Effect preset %1")
                    .arg(objectIdString(presetId)));
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
            if (!document.isObject() || !ProjectSchemaValidator::validatePackMetadata(
                    HARKEN_PACK_METADATA_VERSION, document.object(), relative).isEmpty()) continue;
            if (!document.object().contains(QStringLiteral("guid"))
                && !document.object().contains(QStringLiteral("variables"))) continue;
            return reject(QStringLiteral("Pack metadata has no valid matching binary: %1").arg(relative));
        }
        for (const auto& edge : dependencyEdges) {
            const auto prerequisite = kinds.find(edge.prerequisitePackStableId);
            if (prerequisite == kinds.end()
                || (edge.edgeKind == HK_PACK_DEPENDENCY_EDGE_KIND_INIT)
                    != (prerequisite->second == HK_PACK_KIND_INIT))
                return reject(QStringLiteral("Pack publication contains an incompatible dependency"));
        }
    }
    if (initId != 0) {
        for (const auto& path : binaries) {
            if (exportCanceled(cancelCheck, error)) return false;
            if (managedFiles->contains(path)) continue;
            const QFileInfo info(path);
            const auto packMetadataPath = QDir::cleanPath(QDir(info.path()).filePath(info.completeBaseName() + QStringLiteral(".json")));
            if (files.contains(packMetadataPath)) continue;
            QByteArray bytes;
            harken::studio::build::PackCanonicalImage image;
            harken::studio::build::CurrentWorldBodyImage body;
            if (!readAll(dir.filePath(path), &bytes, &detail)
                || harken::studio::build::ParsePackCanonicalImage(reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                    bytes.size(), &image) != harken::studio::build::kPackOk
                || !body.Decode(image.partitionBody.data(), image.partitionBody.size(),
                    image.partitionHeader.bodyVintage)
                || body.ProductionView()->identity.projectId != expectedProject) continue;
            if ((image.packKind == HK_PACK_KIND_CANONICAL_CONTENT
                    && harken::studio::build::ValidateCanonicalInitDependency(image.dependencyEdges, initId)
                        == harken::studio::build::kPackOk)
                || (image.packKind == HK_PACK_KIND_INIT && image.packStableId == initId))
                managedFiles->append(path);
        }
    }
    managedFiles->removeDuplicates();
    return !exportCanceled(cancelCheck, error);
}

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
    const auto sourceIndex = request.options.buildContext
        ? request.options.buildContext->capabilities->sourceIndex()
        : bridge::canonicalRuntimeProjectSourceIndex(project);
    const auto actionFor = [&](harken::studio::types::ObjectId id) -> const ProjectItem* {
        const auto found = sourceIndex->byQtGuid.find(QUuid(objectIdString(id)));
        return found != sourceIndex->byQtGuid.end()
            && found->second->type == ProjectItem::Type::Action
            && found->second->comp<VAction>() ? found->second : nullptr;
    };
    BindingRuntimeLowering lowering;
    std::map<harken::studio::types::ObjectId, RuntimeCapabilityEntry> bindingCapabilities;
    for (const auto* bus : sourceIndex->buses) {
        const auto capability = RuntimeCapabilityMatrix::lowerBindings(
            *bus, project, RuntimeCapabilityOperation::PackBuild, &lowering);
        if (capability.state != RuntimeCapabilityState::Ready) {
            result.error = capability.diagnostic;
            return result;
        }
        bindingCapabilities.emplace(bus->runtimeId(), capability);
    }
    std::vector<bridge::ProjectInputDiagnostic> busDiagnostics;
    const auto busProjection = [&] {
        if (!request.options.buildContext)
            return bridge::compileStudioBusProjectionForPlatform(
                project, QUuid::fromString(request.options.platform), &busDiagnostics);
        const auto context = request.options.buildContext->capabilities->canonicalContext(
            QUuid(request.options.platform));
        busDiagnostics = context->diagnostics;
        if (!context->ok()) return harken::studio::bus_compiler::PublishedBusProjectionV1{};
        return harken::studio::bus_compiler::BuildPublishedBusProjectionV1(
            *context->busResult,
            harken::studio::bus_compiler::BusPublicationCapabilityVintageV1::
                TopologyVolumeSendAndEffectsNeutralOtherProperties);
    }();
    if (!busDiagnostics.empty() || busProjection.fingerprint == 0) {
        result.error = busDiagnostics.empty()
            ? QStringLiteral("Bus projection is unavailable for target platform %1")
                  .arg(request.options.platform)
            : QStringLiteral("Bus projection on %1: %2 (%3)")
                  .arg(request.options.platform, busDiagnostics.front().message,
                       busDiagnostics.front().field);
        return result;
    }
    const auto busSnapshotPayload = harken::studio::build::VolumeBusSnapshotPayload(busProjection);
    harken::studio::build::PackBusDefinitionSnapshotHeader busSnapshotHeader;
    if (busSnapshotPayload.empty() ||
        !harken::studio::build::ReadPackBusDefinitionSnapshotHeader(
            busSnapshotPayload.data(), busSnapshotPayload.size(), &busSnapshotHeader)) {
        result.error = QStringLiteral("Bus snapshot cannot represent target platform %1")
                           .arg(request.options.platform);
        return result;
    }
    const auto busTriple = harken::studio::build::SnapshotBusTriple(busSnapshotHeader);
    QDir liveDir(request.outputDir);
    if (!liveDir.exists())
        liveDir.mkpath(".");
    auto outputLock = request.options.platformLocks.value(request.options.platform);
    if (!outputLock) {
        outputLock = std::make_shared<QLockFile>(liveDir.filePath(QStringLiteral(".harken-pack-platform.lock")));
        outputLock->setStaleLockTime(0);
        if (!outputLock->tryLock()) {
            result.error = QStringLiteral("Platform output is being built by another request: %1").arg(request.outputDir);
            return result;
        }
    }
    std::vector<PackMeta> packs;
    auto recordUnit = [&](const PackMeta& pack, const ProjectPackExporter::ExportResult& unit) {
        if (pack.retained && unit.success) return;
        result.warnings.append(unit.warnings);
        if (!outRecords) return;
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
            const ProjectItem* action = actionFor(pack.actionStableIds.front());
            if (action) record.objectID = action->objectID;
        }
        outRecords->push_back(std::move(record));
    };
    const auto reportProgress = [&](const PackMeta& pack, int percent, PackGenerationStage stage) {
        if (!request.options.progressCallback) return;
        request.options.progressCallback({request.options.platform, pack.language,
            pack.unitKey, percent, stage});
    };
    const auto observeStage = [&](PackBuildObservationStage stage) {
        if (request.options.observationCallback)
            request.options.observationCallback({request.options.platform, {}, {}, stage});
    };
    const auto observeUnit = [&](const PackMeta& pack, PackBuildObservationStage stage) {
        if (request.options.observationCallback && !pack.retained)
            request.options.observationCallback({request.options.platform, pack.language,
                pack.unitKey, stage, pack.name});
    };

    QMap<QString, PackMeta> previousPacks;
    QStringList errors;
    QSet<QString> failedPaths;
    const auto recordFailure = [&](PackMeta& pack, const QString& error) {
        if (pack.failed) return;
        pack.failed = true;
        failedPaths.insert(pack.fileName);
        ProjectPackExporter::ExportResult unit;
        unit.error = error;
        recordUnit(pack, unit);
        reportProgress(pack, 100, PackGenerationStage::Failed);
        errors.append(QStringLiteral("%1: %2").arg(pack.name, error));
    };
    const auto fail = [&](const QString& error) {
        result.success = false;
        result.error = error;
        result.canceled = request.options.cancelCheck && request.options.cancelCheck();
        for (auto& pack : packs) {
            if (pack.finished || pack.failed) continue;
            ProjectPackExporter::ExportResult unit;
            unit.error = error;
            recordUnit(pack, unit);
            reportProgress(pack, 100, result.canceled ? PackGenerationStage::Canceled : PackGenerationStage::Failed);
            pack.failed = true;
        }
        return result;
    };

    ProjectPackExporter::ExportOptions buildOptions = request.options;
    buildOptions.deferBindings = true;
    const std::uint64_t initId = fnvName(request.options.platform + QStringLiteral("/Init"));

    {
        PackMeta init;
        init.name = QStringLiteral("Init");
        init.unitKey = QStringLiteral("Init");
        init.fileName = QStringLiteral("Init") + bridge::packArtifactExtension();
        init.platform = request.options.platform;
        init.packStableId = initId;
        init.packKind = HK_PACK_KIND_INIT;
        packs.push_back(std::move(init));
    }
    for (const PackBuildUnit& planned : request.plan->units) {
        PackMeta content;
        content.name = planned.name;
        content.unitKey = planned.grouped ? QStringLiteral("g:") + planned.artifactKey
            : QStringLiteral("a:") + objectIdString(planned.actionStableIds.front());
        content.fileName = PackBuildPlanning::packFileName(
            planned.variant.language, planned.name, bridge::packArtifactExtension());
        content.language = planned.variant.language;
        content.platform = planned.variant.platform;
        content.grouped = planned.grouped;
        for (harken::studio::types::ObjectId actionId : planned.actionStableIds) {
            if (request.includedActionIds.count(actionId) == 0
                || request.failedActionVariants.count({actionId, planned.variant.language}) != 0)
                continue;
            const ProjectItem* action = actionFor(actionId);
            if (action && action->comp<VAction>() && !action->comp<VAction>()->steps.empty())
                content.actionStableIds.push_back(actionId);
        }
        if (content.actionStableIds.empty())
            continue;
        content.packStableId = fnvName(PackBuildPlanning::relativePackPath(
            planned.variant.platform, planned.variant.language, planned.artifactKey));
        content.packKind = HK_PACK_KIND_CANONICAL_CONTENT;
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
                    recordFailure(content, QStringLiteral("Pack '%1' on %2/%3: %4")
                        .arg(content.name, content.platform, content.language, diagnostic.message));
            }
        }
    }

    QStringList previousPaths;
    if (request.options.updateSelectedOnly) {
        QStringList files;
        QString scanError;
        if (!scanOutputFiles(liveDir, &files, nullptr, &scanError, request.options.cancelCheck)) return fail(scanError);
        for (const auto& path : files)
            if (path.endsWith(bridge::packArtifactExtension())) previousPaths.append(path);
    } else if (!request.options.forceReexport) {
        for (const auto& pack : packs)
            if (request.options.packBuildState
                && (request.options.packBuildState->isCurrent(pack.fileName)
                    || !request.options.packBuildState->resourceFingerprint(pack.fileName).isEmpty()))
                previousPaths.append(pack.fileName);
    }
    for (const auto& path : previousPaths) {
        if (!QFileInfo::exists(liveDir.filePath(path))) continue;
        PackMeta previous;
        QString error;
        if (!readExistingPack(liveDir, path, project.projectId, request.options.platform, &previous, &error)) {
            const auto current = std::find_if(packs.begin(), packs.end(), [&](const auto& pack) { return pack.fileName == path; });
            if (current != packs.end()) recordFailure(*current, error);
            else {
                previous.name = QFileInfo(path).completeBaseName();
                previous.fileName = path;
                previous.unitKey = path;
                recordFailure(previous, error);
            }
            continue;
        }
        previousPacks.insert(path, std::move(previous));
    }
    std::set<harken::studio::types::ObjectId> previousInitLayerIds;
    for (auto& pack : packs) {
        const auto previous = previousPacks.constFind(pack.fileName);
        if (previous == previousPacks.cend() || previous->packStableId != pack.packStableId
            || previous->packKind != pack.packKind) continue;
        pack.previousRow = previous->previousRow;
        pack.previousEntries = previous->previousEntries;
        pack.previousCapacity = previous->previousCapacity;
        pack.previousValid = true;
        if (request.options.updateSelectedOnly && pack.packKind == HK_PACK_KIND_INIT) {
            harken::studio::build::CurrentWorldBodyImage body;
            if (decodePreviousPack(pack, &body)) {
                const auto& world = *body.ProductionView();
                for (std::uint32_t i = 0; i < world.playablePropertyCount; ++i)
                    if (world.playableProperties[i].contentPropertyKind
                        == harken::studio::build::playableproperties::ContentPropertyKindV1::Layer)
                        previousInitLayerIds.insert(world.playableProperties[i].stableId);
            }
        }
        if (!request.options.forceReexport && request.options.packBuildState) {
            pack.previousResourceFingerprint = request.options.packBuildState->resourceFingerprint(pack.fileName);
            pack.current = request.options.packBuildState->isCurrent(pack.fileName);
            for (const auto& variant : request.plan->variants) {
                if (std::find(pack.actionStableIds.begin(), pack.actionStableIds.end(), variant.actionStableId)
                    == pack.actionStableIds.end() || variant.variant.language != pack.language) continue;
                for (const auto& media : variant.closure.controlMedia)
                    if (!previous->soundContentRevisions.contains(media.soundStableId)) pack.current = false;
            }
            if (pack.current) pack.soundContentRevisions = previous->soundContentRevisions;
        }
    }

    for (std::size_t i = 0; i < packs.size(); ++i) {
        auto& pack = packs[i];
        for (std::size_t j = 0; j < i; ++j) {
            auto& previous = packs[j];
            if (pack.packStableId != previous.packStableId
                && pack.fileName.compare(previous.fileName, Qt::CaseInsensitive) != 0) continue;
            const auto error = QStringLiteral("Pack output identity or file name collides at '%1'").arg(pack.fileName);
            recordFailure(previous, error);
            recordFailure(pack, error);
        }
        const QFileInfo file(pack.fileName);
        const auto packMetadataPath = QDir::cleanPath(QDir(file.path()).filePath(file.completeBaseName() + QStringLiteral(".json")));
        for (const auto& path : {pack.fileName, packMetadataPath})
            if (!safePublicationPath(liveDir, path) || QFileInfo(liveDir.filePath(path)).isDir())
                recordFailure(pack, QStringLiteral("Invalid Pack output path: %1").arg(path));
    }

    auto prepareUnit = [&](PackMeta& meta) -> ProjectPackExporter::ExportResult {
        const QString path = liveDir.filePath(meta.fileName);
        QDir().mkpath(QFileInfo(path).absolutePath());
        ProjectPackExporter::ExportResult unit;
        auto resourceOptions = buildOptions;
        resourceOptions.forceReexport = request.options.forceReexport && !meta.retained;
        resourceOptions.resourceReuse.reset();
        if (request.options.packBuildState)
            resourceOptions.resourceReuse = ProjectPackExporterInternal::PackResourceReuse{
                meta.previousValid && !meta.grouped
                    ? liveDir.filePath(meta.previousRow.value(QStringLiteral("path")).toString()) : QString(),
                meta.previousResourceFingerprint};
        const auto appendResources = [&](const ProjectPackExporter::ExportResult& resource,
                                         std::vector<bridge::PackBuildSession::CanonicalPackEntry> entries) {
            if (exportCanceled(request.options.cancelCheck, &unit.error)) {
                unit.canceled = true;
                return false;
            }
            if (resource.reusedResources) {
                for (const auto& entry : meta.previousEntries)
                    if (entry.kind == HK_PACK_ENTRY_KIND_MEDIA
                        || entry.kind == HK_PACK_ENTRY_KIND_BUS
                        || entry.kind == HK_PACK_ENTRY_KIND_BUS_SEND
                        || entry.kind == HK_PACK_ENTRY_KIND_ATTENUATION
                        || entry.kind == HK_PACK_ENTRY_KIND_EFFECT_PRESET
                        || entry.kind == HK_PACK_ENTRY_KIND_BUS_EFFECT
                        || entry.kind == HK_PACK_ENTRY_KIND_EFFECT_DYNAMIC_ROUTE)
                        meta.resourceEntries.push_back(entry);
                return true;
            }
            for (auto& entry : entries)
                if (!appendEntry(std::move(entry), path, &meta.resourceEntries, &unit.error)) return false;
            return true;
        };
        if (meta.current) {
            const auto media = request.options.packBuildState->mediaResults(meta.fileName);
            if (!media) {
                unit.error = QStringLiteral("Cannot restore recorded media for Pack '%1'").arg(meta.fileName);
                return unit;
            }
            unit.success = true;
            unit.reusedResources = true;
            unit.media = *media;
            unit.outputPath = liveDir.filePath(meta.fileName);
            if (!appendResources(unit, {})) unit.success = false;
            meta.resourceFingerprint = meta.previousResourceFingerprint;
            for (const auto& entry : meta.resourceEntries)
                if (entry.kind == HK_PACK_ENTRY_KIND_MEDIA) ++meta.ownedResources;
            unit.mediaCount = static_cast<int>(meta.ownedResources);
            unit.actionCount = static_cast<int>(meta.actionStableIds.size());
            return unit;
        }
        if (meta.packKind == HK_PACK_KIND_INIT) {
            std::vector<bridge::PackBuildSession::CanonicalPackEntry> entries;
            unit = exporter.ExportInitPack(project, path, resourceOptions, entries);
            if (!unit.success) return unit;
            meta.resourceFingerprint = unit.resourceFingerprint;
            if (!appendResources(unit, std::move(entries))) {
                unit.success = false;
                return unit;
            }
        } else {
            ProjectPackExporter::ExportOptions contentOpts = resourceOptions;
            contentOpts.language = meta.language;
            std::set<std::tuple<QString, QString, QString>> mediaIdentities;
            const auto appendMedia = [&](const QJsonArray& media) {
                for (const auto& value : media) {
                    const auto row = value.toObject();
                    if (mediaIdentities.emplace(
                            row.value(QStringLiteral("soundId")).toString(),
                            row.value(QStringLiteral("sourceEntryId")).toString(),
                            row.value(QStringLiteral("language")).toString()).second)
                        unit.media.append(row);
                }
            };
            for (harken::studio::types::ObjectId actionId : meta.actionStableIds) {
                const ProjectItem* action = actionFor(actionId);
                if (!action) {
                    unit.error = QStringLiteral("Missing Action %1 in Pack '%2'")
                                     .arg(harken::studio::objectIdString(actionId), meta.name);
                    return unit;
                }
                std::vector<bridge::PackBuildSession::CanonicalPackEntry> entries;
                auto member = contentOpts.buildContext
                    ? exporter.exportSingleActionPack(
                        project, *action, path, contentOpts, entries)
                    : exporter.ExportActionPack(
                        project, action->name, path, contentOpts, entries);
                appendMedia(member.media);
                if (!member.success) {
                    member.media = std::move(unit.media);
                    return member;
                }
                for (const auto& [soundId, revision] : member.soundContentRevisions) {
                    const auto [found, inserted] = meta.soundContentRevisions.emplace(soundId, revision);
                    if (!inserted && found->second != revision) {
                        unit.error = QStringLiteral("Conflicting control media revision for Sound %1")
                            .arg(harken::studio::objectIdString(soundId));
                        return unit;
                    }
                }
                unit.warnings += member.warnings;
                unit.objectCount += member.objectCount;
                unit.actionCount += member.actionCount;
                unit.mediaCacheHits += member.mediaCacheHits;
                unit.mediaConversions += member.mediaConversions;
                if (!appendResources(member, std::move(entries))) return unit;
                meta.resourceFingerprint = member.resourceFingerprint;
            }
            unit.success = true;
        }
        for (const auto& entry : meta.resourceEntries) {
            if (exportCanceled(request.options.cancelCheck, &unit.error)) {
                unit.success = false;
                unit.canceled = true;
                return unit;
            }
            if (entry.kind == HK_PACK_ENTRY_KIND_CONTAINER) {
                unit.success = false;
                unit.error = QStringLiteral(
                    "Pack '%1' emitted pack entry kind 9; CurrentWorld is the "
                    "playback program and v2 Container entries are refused")
                                 .arg(meta.fileName);
                return unit;
            }
            if (entry.kind == HK_PACK_ENTRY_KIND_VOICE_RECIPE) {
                unit.success = false;
                unit.error = QStringLiteral(
                    "Pack '%1' emitted pack entry kind 11; CurrentWorld is the "
                    "playback program and v2 VoiceRecipe entries are refused")
                                 .arg(meta.fileName);
                return unit;
            }
            if (entry.kind != HK_PACK_ENTRY_KIND_MEDIA)
                continue;
            unit.success = false;
            unit.error = QStringLiteral("Pack '%1' contains invalid media %2")
                .arg(meta.fileName).arg(harken::studio::objectIdString(entry.id));
            if (entry.payload.size() != sizeof(HkPackMedia)) return unit;
            const auto media = harken::studio::build::ReadPackMedia(entry.payload.data());
            std::uint64_t revision = 0;
            if (media.sampleFormat == HK_PACK_SAMPLE_FORMAT_VORBIS_RESIDENT) {
                if (!entry.encodedBytes || entry.mediaBytes() != media.blobBytes) return unit;
                revision = media.blobChecksum ? media.blobChecksum : 1;
            } else if (meta.soundContentRevisions.count(entry.id) != 0) {
                ++meta.ownedResources;
                unit.success = true;
                unit.error.clear();
                continue;
            } else {
                const auto pcmBytes = entry.mediaBytes();
                if (media.sampleFormat != HK_PACK_SAMPLE_FORMAT_F32_PLANAR
                    || media.blobBytes != pcmBytes || pcmBytes % sizeof(float) != 0
                    || media.channelCount == 0 || media.frameCount == 0)
                    return unit;
                const auto sampleCount = pcmBytes / sizeof(float);
                if (sampleCount % media.channelCount != 0
                    || sampleCount / media.channelCount != media.frameCount)
                    return unit;
                std::vector<const float*> channels(media.channelCount);
                for (std::uint16_t channel = 0; channel < media.channelCount; ++channel)
                    channels[channel] = entry.media->data() + channel * media.frameCount;
                const bool loopable =
                    (media.flags & HK_PACK_MEDIA_FLAG_LOOPABLE) != 0;
                if (!harken::studio::build::ComputeMediaPcmContentRevisionV1(
                        media.sampleRate, media.channelCount, media.frameCount,
                        channels.data(), 1, &revision,
                        loopable ? media.loopStartFrame : 0,
                        loopable ? media.loopEndFrame : 0))
                    return unit;
            }
            const auto [found, inserted] = meta.soundContentRevisions.emplace(media.soundId, revision);
            if (!inserted && found->second != revision) {
                unit.error = QStringLiteral("Control and packed media revisions disagree for Sound %1")
                    .arg(harken::studio::objectIdString(media.soundId));
                return unit;
            }
            ++meta.ownedResources;
            unit.success = true;
            unit.error.clear();
        }
        unit.mediaCount = static_cast<int>(meta.ownedResources);
        return unit;
    };
    observeStage(PackBuildObservationStage::PreparingMedia);
    {
        const auto releaseMedia = qScopeGuard([&] {
            buildOptions.mediaPreparation = nullptr;
            if (request.options.snapshot && request.options.snapshot->derivativeCache)
                request.options.snapshot->derivativeCache->clear();
            if (request.options.snapshot && request.options.snapshot->mediaCache)
                request.options.snapshot->mediaCache->clear();
        });
        std::unique_ptr<PackMediaPreparation> mediaPreparation;
        if (request.options.snapshot && request.options.buildContext) {
            mediaPreparation = std::make_unique<PackMediaPreparation>(request.options.snapshot->media,
                request.options.snapshot->derivativeCache, QUuid(request.options.platform),
                request.options.packBuildState
                    ? QDir(PathUtils::getDotCacheDir(QFileInfo(project.projectPath).absolutePath()))
                        .filePath(QStringLiteral("pack/media")) : QString());
            const auto& sourceIndex = *request.options.buildContext->capabilities->sourceIndex();
            for (const PackMeta& pack : packs) {
                if (pack.current || pack.failed) continue;
                for (const auto actionId : pack.actionStableIds) {
                    const auto planned = std::find_if(request.plan->variants.begin(), request.plan->variants.end(),
                        [&](const auto& variant) {
                            return variant.actionStableId == actionId
                                && variant.variant.platform == request.options.platform
                                && (pack.language.isEmpty() || variant.variant.language.isEmpty()
                                    || variant.variant.language == pack.language);
                        });
                    if (planned == request.plan->variants.end()) continue;
                    std::set<harken::studio::types::ObjectId> sounds;
                    for (const auto* inputs : {&planned->closure.media, &planned->closure.controlMedia}) {
                        for (const auto& media : *inputs) {
                            if (!sounds.insert(media.soundStableId).second) continue;
                            const auto owner = sourceIndex.byQtGuid.find(QUuid(objectIdString(media.soundStableId)));
                            const auto* sound = owner == sourceIndex.byQtGuid.end()
                                ? nullptr : owner->second->comp<VSound>();
                            const auto* source = sound
                                ? SoundSourceSemantics::findEntry(*sound, media.sourceEntryId) : nullptr;
                            if (source)
                                mediaPreparation->add(media.soundStableId, *source, sound->scheduleOffsetSec);
                        }
                    }
                }
            }
            mediaPreparation->start();
            buildOptions.mediaPreparation = mediaPreparation.get();
        }
        for (PackMeta& pack : packs) {
            if (pack.failed) continue;
            reportProgress(pack, 5, PackGenerationStage::Preparing);
            if (exportCanceled(request.options.cancelCheck, &result.error)) return fail(result.error);
            observeUnit(pack, PackBuildObservationStage::Preparing);
            QElapsedTimer timer;
            timer.start();
            pack.mediaResult = prepareUnit(pack);
            pack.elapsedNanoseconds += timer.nsecsElapsed();
            result.mediaCacheHits += pack.mediaResult.mediaCacheHits;
            result.mediaConversions += pack.mediaResult.mediaConversions;
            for (const auto& value : pack.mediaResult.media) {
                auto media = value.toObject();
                media.insert(QStringLiteral("pack"), pack.fileName);
                result.media.append(media);
            }
            if (!pack.mediaResult.success) {
                recordFailure(pack, pack.mediaResult.error);
                continue;
            }
            observeUnit(pack, PackBuildObservationStage::Prepared);
        }
    }

    if (packs.front().failed) return fail(errors.join(QLatin1Char('\n')));
    packs.erase(std::remove_if(packs.begin(), packs.end(), [](const auto& pack) { return pack.failed; }), packs.end());
    for (const auto& pack : packs)
        reportProgress(pack, 40, PackGenerationStage::Compiling);
    if (exportCanceled(request.options.cancelCheck, &result.error)) return fail(result.error);
    observeStage(PackBuildObservationStage::CompilingProject);
    QString compileError;
    std::set<harken::studio::types::ObjectId> requiredDefinitions;
    for (const auto& variant : request.plan->variants) {
        if (std::none_of(packs.begin(), packs.end(), [&](const auto& pack) {
                return std::find(pack.actionStableIds.begin(), pack.actionStableIds.end(), variant.actionStableId)
                    != pack.actionStableIds.end();
            })) continue;
        for (const auto* members : {&variant.closure.objects, &variant.closure.controlObjects})
            for (const auto& member : *members) requiredDefinitions.insert(member.stableId);
    }
    std::set<harken::studio::types::ObjectId> sharedLayerIds;
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
    const QString preferredLanguage = buildOptions.language.trimmed().isEmpty()
        ? project.defaultLanguage : buildOptions.language.trimmed();
    const QString selectedLanguage = languages.contains(preferredLanguage)
        ? preferredLanguage : languages.front();
    std::map<harken::studio::types::ObjectId, std::uint32_t> newPropertyOrders, newContainerOrders;
    std::set<std::uint32_t> reservedPropertyOrders, reservedContainerOrders;
    std::vector<PublicationAuthoredOrders> publicationOrders;
    std::set<harken::studio::types::ObjectId> neutralDefinitionIds = sharedLayerIds;
    std::map<harken::studio::types::ObjectId, harken::studio::build::SignalReceiverV1> receivers;
    if (request.options.updateSelectedOnly) {
        for (const auto& variant : request.plan->variants) {
            if (!variant.variant.language.isEmpty()) continue;
            for (const auto* members : {&variant.closure.objects, &variant.closure.controlObjects})
                for (const auto& member : *members) neutralDefinitionIds.insert(member.stableId);
        }
        publicationOrders = readPublicationAuthoredOrders(previousPacks,
            &reservedPropertyOrders, &reservedContainerOrders);
    }
    for (const QString& language : languages) {
        if (exportCanceled(request.options.cancelCheck, &result.error)) return fail(result.error);
        std::set<harken::studio::types::ObjectId> packedActionIds;
        std::set<harken::studio::types::ObjectId> packedDefinitionIds;
        const auto collectPackedInputs = [&]() {
            packedActionIds.clear();
            packedDefinitionIds = sharedLayerIds;
            for (const auto& variant : request.plan->variants) {
                if ((!variant.variant.language.isEmpty() && variant.variant.language != language)
                    || std::none_of(packs.begin(), packs.end(), [&](const auto& pack) {
                        return pack.language == variant.variant.language
                            && std::find(pack.actionStableIds.begin(), pack.actionStableIds.end(),
                                         variant.actionStableId) != pack.actionStableIds.end();
                    }))
                    continue;
                for (const auto& member : variant.closure.actions) packedActionIds.insert(member.stableId);
                for (const auto& member : variant.closure.objects) packedDefinitionIds.insert(member.stableId);
                for (const auto& member : variant.closure.controlObjects) packedDefinitionIds.insert(member.stableId);
            }
        };
        collectPackedInputs();
        CurrentWorldBuildOptions buildOptions;
        const auto fieldGroups = request.options.buildContext
            ? request.options.buildContext->capabilities->fieldGroups(QUuid(request.options.platform))
            : nullptr;
        if (request.options.buildContext) {
            buildOptions.sourceIndex = request.options.buildContext->capabilities->sourceIndex();
            buildOptions.actionLookup = &request.options.buildContext->closures->targetLookup();
            buildOptions.canonicalContext = request.options.buildContext->capabilities->canonicalContext(
                QUuid(request.options.platform));
        }
        if (!buildOptions.sourceIndex)
            buildOptions.sourceIndex = bridge::canonicalRuntimeProjectSourceIndex(project);
        std::vector<CurrentWorldBuildDiagnostic> loweringDiagnostics;
        buildOptions.loweringDiagnostics = &loweringDiagnostics;
        buildOptions.fieldGroups = fieldGroups.get();
        buildOptions.includedActionIds = &packedActionIds;
        buildOptions.includedDefinitionIds = &packedDefinitionIds;
        buildOptions.validatePackResources = true;
        buildOptions.bindingLowering = &lowering;
        buildOptions.bindingCapabilities = &bindingCapabilities;
        buildOptions.language = language;
        auto worldRequest = buildCurrentWorldSyncRequestForContext(
            project, QUuid::fromString(request.options.platform),
            &compileError, nullptr, &buildOptions, 0, QString());
        if (!worldRequest)
            return fail(compileError);
        for (const auto& diagnostic : loweringDiagnostics) {
            bool affected = false;
            for (auto& pack : packs) {
                if (diagnostic.kind != CurrentWorldBuildDiagnosticKind::Definition
                    && !pack.language.isEmpty() && pack.language != language)
                    continue;
                const bool usesDefinition = std::any_of(request.plan->variants.begin(),
                    request.plan->variants.end(), [&](const auto& variant) {
                        if (variant.variant.language != pack.language
                            || std::find(pack.actionStableIds.begin(), pack.actionStableIds.end(),
                                         variant.actionStableId) == pack.actionStableIds.end())
                            return false;
                        for (const auto* members : {&variant.closure.actions, &variant.closure.objects,
                                                    &variant.closure.controlObjects}) {
                            for (const auto& member : *members) {
                                if (member.stableId == diagnostic.stableId)
                                    return true;
                                if (diagnostic.kind != CurrentWorldBuildDiagnosticKind::Definition)
                                    continue;
                                const auto item = buildOptions.sourceIndex->byObjectId.find(member.objectId);
                                for (const ProjectItem* ancestor = item == buildOptions.sourceIndex->byObjectId.end()
                                         ? nullptr : item->second->parent;
                                     ancestor; ancestor = ancestor->parent)
                                    if (ancestor->runtimeId() == diagnostic.stableId)
                                        return true;
                            }
                        }
                        return false;
                    });
                if (!usesDefinition)
                    continue;
                affected = true;
                recordFailure(pack, diagnostic.error);
            }
            if (!affected)
                return fail(diagnostic.error);
        }
        if (!loweringDiagnostics.empty()) {
            std::erase_if(packs, [](const auto& pack) { return pack.failed; });
            requiredDefinitions.clear();
            sharedLayerIds.clear();
            for (const auto& variant : request.plan->variants) {
                if (std::none_of(packs.begin(), packs.end(), [&](const auto& pack) {
                        return pack.language == variant.variant.language
                            && std::find(pack.actionStableIds.begin(), pack.actionStableIds.end(),
                                         variant.actionStableId) != pack.actionStableIds.end();
                    }))
                    continue;
                for (const auto* members : {&variant.closure.objects, &variant.closure.controlObjects})
                    for (const auto& member : *members) requiredDefinitions.insert(member.stableId);
            }
            for (const auto* binding : project.allFileBindings()) collectLayers(binding->rootItem.get());
            collectPackedInputs();
            retainCurrentWorldSyncRequestInputs(*worldRequest, *buildOptions.sourceIndex,
                                                packedActionIds, packedDefinitionIds);
        }
        if (request.options.updateSelectedOnly) {
            std::map<harken::studio::types::ObjectId, std::uint32_t> containerOrders;
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
                                    .arg(harken::studio::objectIdString(soundId)));
            }
        }
        for (const auto& source : worldRequest->soundSources) {
            if (worldRequest->soundContentRevisions.count(source.stableId) == 0)
                return fail(QStringLiteral("Missing packed media for Sound %1 on %2/%3")
                                .arg(harken::studio::objectIdString(source.stableId)).arg(request.options.platform, language));
        }
        auto compiled = build::compileCurrentWorldRequest(*worldRequest);
        if (exportCanceled(request.options.cancelCheck, &result.error)) return fail(result.error);
        if (!compiled.ok() || !compiled.world) {
            return fail(QStringLiteral(
                            "Current World project compilation failed (%1, stable id %2, language '%3')")
                            .arg(static_cast<std::uint32_t>(compiled.status))
                            .arg(harken::studio::objectIdString(compiled.relatedStableId)).arg(language));
        }
        const harken::studio::build::ContainerWorldInstallViewV1& world = compiled.world->View();
        std::vector<harken::studio::build::SignalRelationV1> busRelations;
        std::unordered_map<harken::studio::types::ObjectId, std::vector<harken::studio::build::SignalRelationV1>> soundRelations;
        for (std::uint32_t i = 0; i < world.signalRelationCount; ++i) {
            const auto& relation = world.signalRelations[i];
            if (relation.source.kind == harken::studio::build::SignalEndpointKindV1::Bus)
                busRelations.push_back(relation);
            else
                soundRelations[relation.source.stableId].push_back(relation);
        }
        for (std::uint32_t i = 0; i < world.signalReceiverCount; ++i)
            receivers.try_emplace(world.signalReceivers[i].target.stableId, world.signalReceivers[i]);
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

        std::unordered_map<harken::studio::types::ObjectId, IdentityOwner> owners;
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
                    harken::studio::build::kInitCatalogIdentityClassPlayable, &compileError))
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
        const SlicedWorldIndex slicedIndex(packs, language);
        const auto rebindOwnedPlayables = [&](bool initOnly, bool* changed) {
            for (PackMeta& pack : packs) {
                if (!packNeedsEncode(pack))
                    continue;
                if (initOnly != (pack.packKind == HK_PACK_KIND_INIT))
                    continue;
                for (auto& playable : pack.slicedPlayables) {
                    const auto before = playable.programSemanticFingerprint;
                    rebindPlayableCrossPackRefs(&playable, slicedIndex);
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
                        slicedIndex, occurrence.parentStableId))
                    occurrence.parentProgramSemanticFingerprint = parent->programSemanticFingerprint;
                rebindPlayableRefLive(&occurrence.childPlayable, slicedIndex);
                occurrence.occurrenceSemanticFingerprint =
                    harken::studio::build::ComputePlayableOccurrenceFingerprintV1(occurrence);
            }
            for (auto& track : pack.slicedTracks) {
                rebindPlayableRefLive(&track.rootDefinition, slicedIndex);
                if (track.placements != nullptr) {
                    auto* placements = const_cast<harken::studio::build::ScheduleClipPlacementV1*>(
                        track.placements);
                    for (std::uint32_t i = 0; i < track.placementCount; ++i)
                        rebindPlayableRefLive(&placements[i].content,
                                             slicedIndex);
                }
                track.scheduleSemanticFingerprint =
                    harken::studio::build::ComputeTrackScheduleFingerprintV1(track);
            }
        }
        for (PackMeta& pack : packs) {
            if (!packNeedsEncode(pack))
                continue;
            for (auto& segment : pack.slicedSegments) {
                rebindPlayableRefLive(&segment.rootDefinition, slicedIndex);
                if (segment.trackRefs != nullptr) {
                    auto* trackRefs = const_cast<harken::studio::build::ScheduleTrackRefV1*>(
                        segment.trackRefs);
                    for (std::uint32_t i = 0; i < segment.trackRefCount; ++i) {
                        rebindPlayableRefLive(&trackRefs[i].trackDefinition,
                                             slicedIndex);
                        const auto* track = findLiveSlicedTrack(
                            slicedIndex, trackRefs[i].trackDefinition.stableId);
                        if (track == nullptr)
                            return fail(QStringLiteral(
                                "Segment %1 references missing sliced Track %2")
                                .arg(harken::studio::objectIdString(segment.rootDefinition.stableId),
                                     harken::studio::objectIdString(trackRefs[i].trackDefinition.stableId)));
                        trackRefs[i].trackScheduleSemanticFingerprint =
                            track->scheduleSemanticFingerprint;
                    }
                }
                segment.scheduleSemanticFingerprint =
                    harken::studio::build::ComputeSegmentScheduleFingerprintV1(segment);
            }
            for (auto& container : pack.slicedContainers) {
                if (container.children != nullptr) {
                    auto* children = const_cast<harken::studio::build::ContainerChildProgramViewV2*>(
                        container.children);
                    for (std::uint32_t i = 0; i < container.childCount; ++i) {
                        rebindPlayableRefLive(&children[i].playable,
                                             slicedIndex);
                        if (!remapScheduleReferenceHintLive(&children[i].timingHint,
                                                            slicedIndex, &compileError))
                            return fail(compileError);
                    }
                }
                const auto* definition = findLiveSlicedPlayable(
                    slicedIndex, container.containerStableId);
                if (definition == nullptr)
                    return fail(QStringLiteral(
                        "Container %1 is missing from the sliced canonical set")
                        .arg(harken::studio::objectIdString(container.containerStableId)));
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
                        auto* steps = const_cast<harken::studio::build::ActionStepProgramViewV1*>(
                            action.steps);
                        for (std::uint32_t i = 0; i < action.stepCount; ++i) {
                            remapActionObjectRefLive(&steps[i].target, slicedIndex);
                            remapActionObjectRefLive(&steps[i].quantizeReference, slicedIndex);
                        }
                    }
                    const auto before = action.programSemanticFingerprint;
                    action.programSemanticFingerprint =
                        harken::studio::build::ComputeActionProgramFingerprintV1(action);
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
            harken::studio::build::ContainerWorldInstallViewV1 slice = world;

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
            std::vector<harken::studio::build::SignalRelationV1> relations;
            if (pack.packStableId == initId) {
                relations = busRelations;
            } else if (!soundRelations.empty()) {
                for (const auto& playable : pack.slicedPlayables) {
                    const auto source = soundRelations.find(playable.stableId);
                    if (source == soundRelations.end()) continue;
                    for (auto relation : source->second) {
                        relation.source.expectedPropertyFingerprint = playable.programSemanticFingerprint;
                        relations.push_back(relation);
                    }
                }
                std::sort(relations.begin(), relations.end(),
                          [](const auto& a, const auto& b) { return a.stableId < b.stableId; });
            }
            setPartitionSignals(slice, relations, {});
            if (!encodePartition(pack.packStableId, slice, pack.ownedDefinitions,
                                 &pack.partitionPayload, &compileError, busTriple))
                return fail(compileError);
            pack.semanticFingerprint = harken::studio::build::Fnv1a64(
                pack.partitionPayload.data() + harken::studio::build::kPackBytes<HkPackCanonicalPartitionHeader>,
                pack.partitionPayload.size()
                    - harken::studio::build::kPackBytes<HkPackCanonicalPartitionHeader>);
            pack.lineageFingerprint = lineageFingerprint(owners, pack.packStableId);
            if (pack.packKind == HK_PACK_KIND_INIT &&
                !fillInitPackMetadata(world, project, &pack.packMetadata, &compileError))
                return fail(compileError);
            if (pack.packKind == HK_PACK_KIND_INIT
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
                std::set<harken::studio::types::ObjectId> referenced;
                for (std::uint32_t i = 0; i < world.variableCount; ++i) {
                    const harken::studio::types::ObjectId variableId = world.variables[i].variableId;
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
                std::vector<std::pair<harken::studio::types::ObjectId, std::uint64_t>> rows;
                for (harken::studio::types::ObjectId variableId : referenced) {
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

    std::map<harken::studio::types::ObjectId, std::vector<std::uint8_t>> authoredPresets;
    for (const auto& entry : packs.front().resourceEntries)
        if (entry.kind == HK_PACK_ENTRY_KIND_EFFECT_PRESET)
            authoredPresets.emplace(entry.id, entry.payload);
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
        for (auto it = previousPacks.begin(); it != previousPacks.end(); ++it) {
            if (failedPaths.contains(it->fileName)
                || std::any_of(it->actionStableIds.begin(), it->actionStableIds.end(),
                    [&](auto id) { return request.failedActionVariants.count({id, it->language}) != 0; })) continue;
            if (it->packStableId != initId && std::any_of(packs.begin(), packs.end(), [&](const auto& pack) {
                    return pack.packStableId == it->packStableId
                        || pack.fileName.compare(it->fileName, Qt::CaseInsensitive) == 0;
                })) continue;
            auto retained = std::move(it.value());
            retained.retained = true;
            if (retained.packStableId == initId) packs.front() = std::move(retained);
            else packs.push_back(std::move(retained));
        }
    }
    if (!harmonizePublicationCapacity(&packs, request.options.packBuildState.get(), authoredPresets,
                                      std::move(receivers), &compileError))
        return fail(compileError);
    std::set<harken::studio::types::ObjectId> bindingIds;
    std::erase_if(lowering.programs, [&](const auto& program) {
        return !bindingIds.insert(program.bindingId).second;
    });
    std::set<std::pair<harken::studio::types::ObjectId, harken::studio::types::ObjectId>> bindingAttachments;
    std::erase_if(lowering.attachments, [&](const auto& attachment) {
        return !bindingAttachments.emplace(attachment.bindingId, attachment.objectId).second;
    });
    for (PackMeta& pack : packs) {
        if (pack.retained && !pack.metadataUpdated) continue;
        std::vector<bridge::PackBuildSession::CanonicalPackEntry> retainedGlobals;
        auto& globals = pack.retained ? retainedGlobals : pack.resourceEntries;
        if (exportCanceled(request.options.cancelCheck, &result.error)) return fail(result.error);
        if (pack.retained) {
            globals = pack.previousEntries;
            std::erase_if(globals, [](const auto& entry) {
                return entry.kind == HK_PACK_ENTRY_KIND_CANONICAL_DEFINITION_PARTITION
                    || entry.kind == HK_PACK_ENTRY_KIND_PACK_REQUIREMENT_BLOCK
                    || entry.kind == HK_PACK_ENTRY_KIND_PACK_DEPENDENCY_EDGE
                    || entry.kind == HK_PACK_ENTRY_KIND_VARIABLE_POLICY_OVERLAY
                    || entry.kind == HK_PACK_ENTRY_KIND_VARIABLE_REQUIREMENTS
                    || entry.kind == HK_PACK_ENTRY_KIND_BUS_DEFINITION_SNAPSHOT;
            });
        } else if (!appendCanonicalBindingEntries(lowering, pack, &globals, &compileError)) {
            return fail(compileError);
        }
        const auto samePayload = [&](std::uint32_t kind, const auto& payload) {
            const auto previous = std::find_if(pack.previousEntries.begin(), pack.previousEntries.end(),
                [&](const auto& entry) { return entry.kind == kind; });
            return previous == pack.previousEntries.end() ? payload.empty() : previous->payload == payload;
        };
        if (pack.current
            && samePayload(HK_PACK_ENTRY_KIND_CANONICAL_DEFINITION_PARTITION, pack.partitionPayload)
            && samePayload(HK_PACK_ENTRY_KIND_VARIABLE_POLICY_OVERLAY, pack.variablePolicyOverlayPayload)
            && samePayload(HK_PACK_ENTRY_KIND_VARIABLE_REQUIREMENTS, pack.variableRequirementsPayload)
            && std::all_of(globals.begin(), globals.end(), [&](const auto& entry) {
                return std::any_of(pack.previousEntries.begin(), pack.previousEntries.end(), [&](const auto& previous) {
                    return entry.kind == previous.kind && entry.vintage == previous.vintage
                        && entry.id == previous.id && entry.relationOrdinal == previous.relationOrdinal
                        && entry.payload == previous.payload && sameMedia(entry, previous);
                });
            })) {
            pack.contentRevision = pack.previousRow.value(QStringLiteral("contentRevision")).toVariant().toULongLong();
            continue;
        }
        auto contentRevision = harken::studio::build::Fnv1a64(
            pack.partitionPayload.data(), pack.partitionPayload.size());
        for (const auto& entry : globals) {
            std::array<std::uint8_t, 32> header{};
            harken::studio::build::StoreLE32(header.data(), entry.kind);
            harken::studio::build::StoreLE32(header.data() + 4, entry.vintage);
            harken::studio::build::StoreObjectId(header.data() + 8, entry.id);
            harken::studio::build::StoreLE64(header.data() + 24, entry.relationOrdinal);
            contentRevision = harken::studio::build::Fnv1a64(
                header.data(), header.size(), contentRevision);
            contentRevision = harken::studio::build::Fnv1a64(
                entry.payload.data(), entry.payload.size(), contentRevision);
            contentRevision = harken::studio::build::Fnv1a64(
                entry.mediaData(), entry.mediaBytes(), contentRevision);
        }
        for (const auto& row : {std::make_pair(HK_PACK_ENTRY_KIND_VARIABLE_POLICY_OVERLAY,
                                              &pack.variablePolicyOverlayPayload),
                                std::make_pair(HK_PACK_ENTRY_KIND_VARIABLE_REQUIREMENTS,
                                              &pack.variableRequirementsPayload)}) {
            if (row.second->empty()) continue;
            std::array<std::uint8_t, 16> header{};
            harken::studio::build::StoreLE32(header.data(), row.first);
            harken::studio::build::StoreLE32(header.data() + 4,
                                       harken::studio::build::PackEntryVintage(row.first));
            harken::studio::build::StoreLE64(header.data() + 8, 0);
            contentRevision = harken::studio::build::Fnv1a64(
                header.data(), header.size(), contentRevision);
            contentRevision = harken::studio::build::Fnv1a64(
                row.second->data(), row.second->size(), contentRevision);
        }
        pack.contentRevision = contentRevision & INT64_MAX;
        if (pack.contentRevision == 0) pack.contentRevision = 1;
    }
    for (auto& pack : packs) {
        pack.requiredInitRevision = packs.front().contentRevision;
        if (pack.retained) {
            for (const auto& entry : pack.previousEntries) {
                if (entry.kind != HK_PACK_ENTRY_KIND_PACK_DEPENDENCY_EDGE) continue;
                harken::studio::build::PackDependencyEdge edge;
                if (!harken::studio::build::ReadPackDependencyEdge(
                        entry.payload.data(), entry.payload.size(), &edge))
                    return fail(QStringLiteral("Invalid retained Pack dependency"));
                if (edge.prerequisitePackStableId == initId
                    && edge.expectedPrerequisiteContentRevision != pack.requiredInitRevision)
                    pack.metadataUpdated = true;
            }
        }
    }
    QSet<QString> currentPaths;
    for (const auto& pack : packs) {
        if (currentPaths.contains(pack.fileName.toCaseFolded()))
            return fail(QStringLiteral("The pack path '%1' conflicts with another published pack").arg(pack.fileName));
        currentPaths.insert(pack.fileName.toCaseFolded());
    }

    auto writeUnit = [&](PackMeta& meta) -> ProjectPackExporter::ExportResult {
        const QString path = liveDir.filePath(meta.fileName);
        auto unit = std::move(meta.mediaResult);
        const auto reuse = [&] {
            unit.success = true;
            meta.skipped = true;
            unit.outputPath = path;
            unit.fileSize = QFileInfo(path).size();
            return unit;
        };
        if (meta.retained && !meta.metadataUpdated) return reuse();
        if (meta.retained) {
            auto entries = meta.previousEntries;
            for (auto& entry : entries) {
                if (entry.kind == HK_PACK_ENTRY_KIND_CANONICAL_DEFINITION_PARTITION) {
                    entry.payload = meta.partitionPayload;
                } else if (entry.kind == HK_PACK_ENTRY_KIND_PACK_REQUIREMENT_BLOCK) {
                    harken::studio::build::PackRequirementBlock requirement;
                    if (!harken::studio::build::ReadPackRequirementBlock(
                            entry.payload.data(), entry.payload.size(), &requirement)) {
                        unit.error = QStringLiteral("Invalid retained Pack requirement");
                        return unit;
                    }
                    requirement.semanticFingerprint = meta.semanticFingerprint;
                    requirement.packContentRevision = meta.contentRevision;
                    harken::studio::build::SerializePackRow<HkPackRequirementBlock>(requirement, entry.payload.data());
                    harken::studio::build::StampRequirementCoverage(&entry.payload);
                } else if (entry.kind == HK_PACK_ENTRY_KIND_PACK_DEPENDENCY_EDGE) {
                    harken::studio::build::PackDependencyEdge edge;
                    if (!harken::studio::build::ReadPackDependencyEdge(
                            entry.payload.data(), entry.payload.size(), &edge)) {
                        unit.error = QStringLiteral("Invalid retained Pack dependency");
                        return unit;
                    }
                    if (edge.prerequisitePackStableId == initId) {
                        edge.expectedPrerequisiteContentRevision = meta.requiredInitRevision;
                        harken::studio::build::SerializePackRow<HkPackDependencyEdge>(edge, entry.payload.data());
                    }
                }
            }
            unit.success = bridge::PackBuildSession::writeCanonicalPackFile(
                path, meta.packKind == HK_PACK_KIND_INIT
                    ? bridge::PackBuildSession::CanonicalPackKind::Init
                    : bridge::PackBuildSession::CanonicalPackKind::CanonicalContent,
                entries, &unit.error);
            unit.outputPath = path;
            unit.fileSize = QFileInfo(path).size();
            return unit;
        }
        auto& entries = meta.resourceEntries;
        bridge::PackBuildSession::CanonicalPackEntry partition;
        partition.kind = HK_PACK_ENTRY_KIND_CANONICAL_DEFINITION_PARTITION;
        partition.vintage = HK_PACK_CANONICAL_DEFINITION_PARTITION_VINTAGE;
        partition.relationOrdinal = 0;
        partition.payload = meta.partitionPayload;
        entries.push_back(std::move(partition));
        if (meta.packKind == HK_PACK_KIND_INIT
            && !meta.variablePolicyOverlayPayload.empty()) {
            bridge::PackBuildSession::CanonicalPackEntry overlay;
            overlay.kind = HK_PACK_ENTRY_KIND_VARIABLE_POLICY_OVERLAY;
            overlay.vintage = HK_PACK_VARIABLE_POLICY_OVERLAY_VINTAGE;
            overlay.id = {};
            overlay.payload = meta.variablePolicyOverlayPayload;
            entries.push_back(std::move(overlay));
        }
        if (meta.packKind == HK_PACK_KIND_CANONICAL_CONTENT
            && !meta.variableRequirementsPayload.empty()) {
            bridge::PackBuildSession::CanonicalPackEntry requirements;
            requirements.kind = HK_PACK_ENTRY_KIND_VARIABLE_REQUIREMENTS;
            requirements.vintage = HK_PACK_VARIABLE_REQUIREMENTS_VINTAGE;
            requirements.id = {};
            requirements.payload = meta.variableRequirementsPayload;
            entries.push_back(std::move(requirements));
        }

        std::uint32_t edgeOrdinal = 0;
        for (std::uint64_t dependency : meta.dependencies) {
            bridge::PackBuildSession::CanonicalPackEntry edge;
            edge.kind = HK_PACK_ENTRY_KIND_PACK_DEPENDENCY_EDGE;
            edge.vintage = HK_PACK_PACK_DEPENDENCY_EDGE_VINTAGE;
            edge.relationOrdinal = edgeOrdinal++;
            edge.payload = dependencyPayload(
                dependency,
                dependency == initId ? HK_PACK_DEPENDENCY_EDGE_KIND_INIT
                    : HK_PACK_DEPENDENCY_EDGE_KIND_CONTENT,
                dependency == initId ? meta.requiredInitRevision : 0,
                busTriple);
            entries.push_back(std::move(edge));
        }
        bridge::PackBuildSession::CanonicalPackEntry requirement;
        requirement.kind = HK_PACK_ENTRY_KIND_PACK_REQUIREMENT_BLOCK;
        requirement.vintage = HK_PACK_PACK_REQUIREMENT_BLOCK_VINTAGE;
        requirement.relationOrdinal = 0;
        requirement.payload = requirementPayload(
            meta.packStableId, meta.ownedResources, meta.ownedDefinitions, 1,
            meta.semanticFingerprint, meta.lineageFingerprint, meta.contentRevision,
            busTriple);
        entries.push_back(std::move(requirement));

        if (meta.packKind == HK_PACK_KIND_INIT) {
            bridge::PackBuildSession::CanonicalPackEntry snapshot;
            snapshot.kind = HK_PACK_ENTRY_KIND_BUS_DEFINITION_SNAPSHOT;
            snapshot.vintage = HK_PACK_BUS_DEFINITION_SNAPSHOT_VINTAGE;
            snapshot.id = {};
            snapshot.relationOrdinal = 1;
            snapshot.payload = busSnapshotPayload;
            entries.push_back(std::move(snapshot));
        }

        const auto sameEntry = [](const auto& first, const auto& second) {
            return first.kind == second.kind && first.vintage == second.vintage
                && first.id == second.id && first.relationOrdinal == second.relationOrdinal
                && first.payload == second.payload && sameMedia(first, second);
        };
        if ((meta.current || (meta.previousValid && !request.options.forceReexport
                && meta.packKind == HK_PACK_KIND_INIT))
            && entries.size() == meta.previousEntries.size()
            && std::all_of(entries.begin(), entries.end(), [&](const auto& entry) {
                return std::any_of(meta.previousEntries.begin(), meta.previousEntries.end(),
                    [&](const auto& previous) { return sameEntry(entry, previous); });
            })) return reuse();
        if (!bridge::PackBuildSession::writeCanonicalPackFile(
                path,
                meta.packKind == HK_PACK_KIND_INIT
                    ? bridge::PackBuildSession::CanonicalPackKind::Init
                    : bridge::PackBuildSession::CanonicalPackKind::CanonicalContent,
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

    std::unordered_map<harken::studio::types::ObjectId, const ProjectItem*> metadataItems;
    if (project.buildExportPackMetadata) {
        std::unordered_set<const ProjectItem*> visited;
        const auto indexItem = [&](auto&& self, const ProjectItem* item) -> void {
            if (!item || !visited.insert(item).second) return;
            metadataItems.emplace(item->runtimeId(), item);
            for (const auto& child : item->children) self(self, child.get());
        };
        indexItem(indexItem, project.root.get());
        for (const auto* binding : project.allFileBindings())
            if (binding) indexItem(indexItem, binding->rootItem.get());
    }
    QString waveformOutputRoot;
    QString waveformOutputError;
    std::unique_ptr<QLockFile> waveformLock;
    QHash<uint32_t, QString> exportedWaveformSources;
    if (project.buildExportWaveforms) {
        waveformOutputRoot = request.options.waveformOutputDir.isEmpty()
            ? PathUtils::waveformBuildOutputRoot(project.projectPath, project.buildWaveformOutputDir)
            : request.options.waveformOutputDir;
        if (!QDir::isAbsolutePath(waveformOutputRoot)
            || PathUtils::isProtectedPackOutputPath(waveformOutputRoot, project.projectPath)) {
            waveformOutputError = PathUtils::packBuildOutputLocationError();
        } else if (!QDir().mkpath(waveformOutputRoot)) {
            waveformOutputError = QStringLiteral("Cannot create waveform output directory '%1'").arg(waveformOutputRoot);
        } else {
            waveformLock = std::make_unique<QLockFile>(QDir(waveformOutputRoot).filePath(QString::fromLatin1(kWaveformLock)));
            waveformLock->setStaleLockTime(0);
            if (!waveformLock->tryLock())
                waveformOutputError = QStringLiteral("Waveform output is in use: %1").arg(waveformOutputRoot);
        }
    }
    observeStage(PackBuildObservationStage::WritingPacks);
    QSet<qulonglong> completed;
    for (PackMeta& pack : packs) {
        if (exportCanceled(request.options.cancelCheck, &result.error)) return fail(result.error);
        const auto retainedMedia = pack.retained && request.options.packBuildState
            ? request.options.packBuildState->mediaResults(pack.fileName) : std::nullopt;
        ProjectPackExporter::ExportResult unit;
        const auto failedDependency = std::find_if(pack.dependencies.begin(), pack.dependencies.end(),
            [&](auto dependency) { return !completed.contains(dependency); });
        if (failedDependency != pack.dependencies.end()) {
            unit.error = QStringLiteral("Pack '%1' cannot be built because dependency %2 failed")
                .arg(pack.name).arg(*failedDependency);
        } else {
            reportProgress(pack, 60, PackGenerationStage::Writing);
            observeUnit(pack, PackBuildObservationStage::Writing);
            QElapsedTimer timer;
            timer.start();
            unit = writeUnit(pack);
            pack.elapsedNanoseconds += timer.nsecsElapsed();
        }
        if (unit.success && !pack.retained) {
            if (pack.packKind == HK_PACK_KIND_CANONICAL_CONTENT) {
                const auto* action = pack.actionStableIds.size() == 1 ? actionFor(pack.actionStableIds.front()) : nullptr;
                if (!action) {
                    unit.success = false;
                    unit.error = QStringLiteral("Action publication identity is unavailable for '%1'").arg(pack.name);
                } else {
                    pack.packMetadata = QJsonObject{{QStringLiteral("packMetadataVersion"), HARKEN_PACK_METADATA_VERSION},
                        {QStringLiteral("name"), action->name}, {QStringLiteral("guid"), objectIdString(action->runtimeId())}};
                }
            }
            if (unit.success && project.buildExportWaveforms) {
                QString error = waveformOutputError;
                if (!error.isEmpty() || !fillWaveformPackMetadata(request, pack, waveformOutputRoot,
                        &exportedWaveformSources, &pack.packMetadata, &error)) {
                    unit.success = false;
                    unit.error = QStringLiteral("Cannot export waveforms for Pack '%1': %2").arg(pack.name, error);
                }
            }
            if (unit.success && project.buildExportPackMetadata) {
                harken::studio::build::CurrentWorldBodyImage body;
                harken::studio::build::PackCanonicalPartitionHeader header;
                unit.success = harken::studio::build::ReadPackCanonicalPartitionHeader(
                        pack.partitionPayload.data(), pack.partitionPayload.size(), &header)
                    && body.Decode(pack.partitionPayload.data() + header.headerBytes,
                        static_cast<std::size_t>(header.bodyBytes), header.bodyVintage);
                if (!unit.success) {
                    unit.error = QStringLiteral("Cannot read Pack metadata for '%1'").arg(pack.name);
                } else {
                    auto ids = worldMetadataObjectIds(*body.ProductionView());
                    for (const auto& entry : pack.resourceEntries) {
                        if (entry.kind == HK_PACK_ENTRY_KIND_MEDIA
                            || entry.kind == HK_PACK_ENTRY_KIND_ATTENUATION) ids.insert(entry.id);
                        if (entry.kind == HK_PACK_ENTRY_KIND_BINDING_DEFINITION) {
                            harken::studio::types::ObjectId id;
                            harken::studio::build::BindingDefinition binding;
                            if (!harken::studio::build::ReadPackBindingDefinition(
                                    entry.payload.data(), entry.payload.size(), &id, &binding)) {
                                unit.success = false;
                                unit.error = QStringLiteral("Cannot read binding metadata for '%1'").arg(pack.name);
                                break;
                            }
                            ids.insert(binding.variableId);
                        }
                    }
                    if (!pack.variableRequirementsPayload.empty()) {
                        const auto* data = pack.variableRequirementsPayload.data();
                        harken::studio::build::PackVariableRequirementsHeader header;
                        if (!harken::studio::build::ParsePackRow<HkPackVariableRequirementsHeader>(data,
                                harken::studio::build::kPackBytes<HkPackVariableRequirementsHeader>, &header)) {
                            unit.success = false;
                            unit.error = QStringLiteral("Cannot read variable requirement metadata for '%1'").arg(pack.name);
                            break;
                        }
                        for (std::uint32_t i = 0; i < header.rowCount; ++i) {
                            harken::studio::build::PackVariableRequirement row;
                            if (!harken::studio::build::ParsePackRow<HkPackVariableRequirement>(data +
                                    harken::studio::build::kPackBytes<HkPackVariableRequirementsHeader> + i * harken::studio::build::kPackBytes<HkPackVariableRequirement>,
                                    harken::studio::build::kPackBytes<HkPackVariableRequirement>, &row)) {
                                unit.success = false;
                                unit.error = QStringLiteral("Cannot read variable requirement metadata for '%1'").arg(pack.name);
                                break;
                            }
                            ids.insert(row.stableId);
                        }
                    }
                    if (pack.packKind == HK_PACK_KIND_INIT)
                        for (const auto& bus : busProjection.buses) ids.insert(bus.busId);
                    ids.erase(harken::studio::types::ObjectId{});
                    ids.erase(harken::studio::types::kMasterBusObjectId);
                    if (unit.success)
                        unit.success = fillPackMetadata(project, pack, ids, metadataItems, &pack.packMetadata, &unit.error);
                }
            }
            if (unit.success) {
                const QFileInfo binary(pack.fileName);
                const auto relative = QDir::cleanPath(QDir(binary.path()).filePath(binary.completeBaseName() + QStringLiteral(".json")));
                const auto path = liveDir.filePath(relative);
                const auto bytes = QJsonDocument(pack.packMetadata).toJson(QJsonDocument::Indented);
                QFile current(path);
                const bool unchanged = current.open(QIODevice::ReadOnly | QIODevice::Text) && current.readAll() == bytes;
                current.close();
                if (!unchanged) unit.success = writeJson(path, pack.packMetadata, &unit.error);
            }
        }
        if (unit.success) {
            const QFileInfo binary(pack.fileName);
            const QString packMetadataPath = QDir::cleanPath(
                QDir(binary.path()).filePath(binary.completeBaseName() + QStringLiteral(".json")));
            unit.success = preserveOutputFileSpelling(liveDir.filePath(pack.fileName), &unit.error)
                && preserveOutputFileSpelling(liveDir.filePath(packMetadataPath), &unit.error);
        }
        recordUnit(pack, unit);
        if (!unit.success) {
            pack.failed = true;
            errors.append(QStringLiteral("%1: %2").arg(pack.name, unit.error));
            reportProgress(pack, 100, PackGenerationStage::Failed);
            continue;
        }
        pack.finished = true;
        completed.insert(pack.packStableId);
        pack.fileSize = unit.fileSize;
        pack.formatVersion = HK_PACK_FORMAT_VERSION;
        if (!pack.retained || pack.packKind == HK_PACK_KIND_INIT) {
            result.fileSize += unit.fileSize;
            result.mediaCount += unit.mediaCount;
            result.objectCount += unit.objectCount;
            result.actionCount += unit.actionCount;
            const auto path = liveDir.filePath(pack.fileName);
            result.artifacts.append({pack.unitKey, request.options.platform, pack.language,
                pack.name, path, unit.fileSize, QFileInfo(path).lastModified(), pack.formatVersion});
        }
        if (!pack.retained) {
            if (pack.skipped) ++result.skippedPackCount;
            else ++result.builtPackCount;
        }
        QJsonObject output{{QStringLiteral("key"), pack.unitKey},
                           {QStringLiteral("path"), pack.fileName}};
        if (!pack.retained)
            output.insert(QStringLiteral("media"), unit.media);
        else if (retainedMedia)
            output.insert(QStringLiteral("media"), *retainedMedia);
        if (pack.packMetadata.contains(QStringLiteral("waveforms")))
            output.insert(QStringLiteral("packMetadata"), QJsonObject{
                {QStringLiteral("waveforms"), pack.packMetadata.value(QStringLiteral("waveforms"))}});
        result.outputs.append(output);
        if (request.options.packBuildState && !pack.resourceFingerprint.isEmpty())
            request.options.packBuildState->setResourceFingerprint(pack.fileName, pack.resourceFingerprint);
        pack.previousEntries.clear();
        pack.resourceEntries.clear();
        reportProgress(pack, 100, pack.skipped ? PackGenerationStage::Skipped : PackGenerationStage::Complete);
    }
    result.success = errors.isEmpty();
    result.error = errors.join(QLatin1Char('\n'));
    result.outputPath = request.outputDir;
    return result;
}

ProjectPackExporter::ExportResult ProjectPackExporter::FinalizePackExport(
    const QString& baseDir, const ProjectData& project,
    const QStringList& selectedPlatformIds, bool selfCleaning,
    const QMap<QString, QJsonArray>& outputs,
    const std::function<bool()>& cancelCheck)
{
    ExportResult result;
    if (exportCanceled(cancelCheck, &result.error)) {
        result.canceled = true;
        return result;
    }
    QStringList errors;
    const QDir root(baseDir);
    if (PathUtils::isProtectedPackOutputPath(baseDir, project.projectPath)) {
        result.error = PathUtils::packBuildOutputLocationError();
        return result;
    }
    QStringList keep;
    if (selfCleaning) {
        for (const auto& id : selectedPlatformIds) {
            const auto* platform = ProjectPlatformSemantics::find(project.platforms, QUuid(id));
            if (!platform) {
                result.error = QStringLiteral("Unknown project platform ID: %1").arg(id);
                return result;
            }
            const QDir directory(platformOutputDir(baseDir, *platform));
            for (const auto& value : outputs.value(id)) {
                const auto row = value.toObject();
                const QString path = row.value(QStringLiteral("path")).toString();
                const QFileInfo file(path);
                keep.append(root.relativeFilePath(directory.filePath(path)));
                keep.append(root.relativeFilePath(directory.filePath(QDir::cleanPath(
                    QDir(file.path()).filePath(file.completeBaseName() + QStringLiteral(".json"))))));
                keep.append(packMetadataWaveformFiles(row.value(QStringLiteral("packMetadata")).toObject(),
                    directory.filePath(path), root));
            }
        }
    }
    const QString metadataName = QStringLiteral("Project.json");
    if (project.buildExportProjectData) {
        QJsonArray platforms;
        for (const auto& platform : project.platforms)
            platforms.append(QJsonObject{{QStringLiteral("guid"), ProjectPlatformSemantics::idString(platform.id)},
                {QStringLiteral("name"), platform.name},
                {QStringLiteral("basePlatform"), ProjectPlatformSemantics::basePlatformName(platform.basePlatform)}});
        const QJsonObject metadata{
            {QStringLiteral("packMetadataVersion"), HARKEN_PACK_METADATA_VERSION},
            {QStringLiteral("projectGuid"), QUuid(project.projectId).toString(QUuid::WithoutBraces)},
            {QStringLiteral("projectName"), project.name},
            {QStringLiteral("studioVersion"), QString::fromUtf8(Harken::BuildInfo::Version.data(),
                static_cast<qsizetype>(Harken::BuildInfo::Version.size()))},
            {QStringLiteral("studioSchemaVersion"), ProjectSchema::kProjectSchemaVersion},
            {QStringLiteral("platforms"), platforms},
            {QStringLiteral("languages"), QJsonArray::fromStringList(ProjectLanguageSemantics::buildLanguages(project))}};
        QFile current(root.filePath(metadataName));
        const bool unchanged = current.open(QIODevice::ReadOnly | QIODevice::Text)
            && current.readAll() == QJsonDocument(metadata).toJson(QJsonDocument::Indented);
        current.close();
        if (!unchanged && !writeJson(root.filePath(metadataName), metadata, &result.error)) return result;
        keep.append(metadataName);
    }
    if (selfCleaning) {
        QStringList files;
        QStringList directories;
        if (!scanOutputFiles(root, &files, nullptr, &result.error, cancelCheck, &directories)) {
            result.canceled = cancelCheck && cancelCheck();
            return result;
        }
        QStringList preserved = keep;
        std::vector<std::unique_ptr<QLockFile>> waveformLocks;
        QStringList warnings;
        const bool waveformOutputsPreserved = preserveBusyWaveformOutputs(
            root, files, &keep, &preserved, &waveformLocks, &warnings, &result.error, cancelCheck);
        result.addGlobalWarnings(warnings);
        if (!waveformOutputsPreserved) return result;
        for (const auto& relative : files) {
            if (exportCanceled(cancelCheck, &result.error)) {
                result.canceled = true;
                return result;
            }
            if (containsPublicationFile(root, keep, relative)) continue;
            const auto path = root.filePath(relative);
            if (!QFile::remove(path)) {
                errors.append(QStringLiteral("Cannot remove obsolete output '%1'").arg(path));
                continue;
            }
            if (relative.endsWith(bridge::packArtifactExtension())) ++result.removedPackCount;
        }
        std::sort(directories.begin(), directories.end(), [](const auto& a, const auto& b) {
            const auto aDepth = a.count(QLatin1Char('/'));
            const auto bDepth = b.count(QLatin1Char('/'));
            return aDepth != bDepth ? aDepth > bDepth : a < b;
        });
        for (const auto& relative : directories) {
            if (exportCanceled(cancelCheck, &result.error)) {
                result.canceled = true;
                return result;
            }
            const auto path = root.filePath(relative);
            if (!QDir(path).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System))
                continue;
            if (!root.rmdir(relative))
                errors.append(QStringLiteral("Cannot remove empty output directory '%1'").arg(path));
        }
    } else {
        for (const auto& name : {metadataName, QStringLiteral("ProjectData.json")}) {
            if (keep.contains(name) || !QFileInfo::exists(root.filePath(name))) continue;
            if (!QFile::remove(root.filePath(name)))
                errors.append(QStringLiteral("Cannot remove obsolete output '%1'").arg(root.filePath(name)));
        }
    }
    result.outputPath = baseDir;
    result.success = errors.isEmpty();
    result.error = errors.join(QLatin1Char('\n'));
    return result;
}

}
