#include "CanonicalRuntimeProjectAdapter.h"
#include "CanonicalRuntimeProjectAdapterPrivate.h"
#include "CompileTargetProfileCatalog.h"

#include "AssetPipeline/AssetRegistry.h"
#include "BindingRuntimeLowering.h"
#include "Components/VAudio.h"
#include "Components/VMixNode.h"
#include "Components/VBus.h"
#include "Components/VPiece.h"
#include "Components/VSpatial.h"
#include "Components/VSelection.h"
#include "Components/VTrack.h"
#include "LanguageContextService.h"
#include "PlayablePropertyAuthoringV1.h"
#include "PlaybackReferenceValidation.h"
#include "PlatformContextService.h"
#include "ProjectTypes.h"
#include "SoundSourceEditing.h"

#ifdef slots
#undef slots
#endif

#include <harken_project_schema/BuildPieceIR.h>
#include <harken_project_schema/BuildPieceProgram.h>
#include <harken_project_schema/FrontEnd.h>
#include <harken_project_schema/ProjectInput.h>
#include <harken_project_schema/Serialize.h>

#include <QHash>
#include <QByteArray>
#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace HarkenEditor::Bridge {

class CompiledProjectSnapshot::Impl
{
public:
    harkenprojectschema::FrontEndResult frontEnd;
    harkenprojectschema::ResourceTable resources;
    std::optional<harkenprojectschema::PieceIR> ir;
    std::optional<harkenprojectschema::PieceProgram> program;
};

CompiledProjectSnapshot::CompiledProjectSnapshot()
    : impl_(std::make_unique<Impl>())
{
}

CompiledProjectSnapshot::~CompiledProjectSnapshot() = default;
CompiledProjectSnapshot::CompiledProjectSnapshot(CompiledProjectSnapshot&&) noexcept = default;
CompiledProjectSnapshot& CompiledProjectSnapshot::operator=(CompiledProjectSnapshot&&) noexcept = default;

bool CompiledProjectSnapshot::valid() const
{
    return impl_ && impl_->frontEnd.ok() && impl_->frontEnd.project
        && impl_->frontEnd.facts && impl_->frontEnd.resolution
        && impl_->frontEnd.sends && impl_->frontEnd.effectChains;
}

const void* compiledProjectSnapshotImpl(const CompiledProjectSnapshot& snapshot)
{
    return snapshot.impl_.get();
}

const harkenprojectschema::PieceProgram* compiledPieceProgram(
    const CompiledProjectSnapshot& snapshot)
{
    const auto* impl = static_cast<const CompiledProjectSnapshot::Impl*>(
        compiledProjectSnapshotImpl(snapshot));
    return impl && impl->program ? &*impl->program : nullptr;
}

std::vector<harkenstudio::ObjectId> compiledPieceIds(const CompiledProjectSnapshot& snapshot)
{
    std::vector<harkenstudio::ObjectId> result;
    const auto* program = compiledPieceProgram(snapshot);
    if (!program) return result;
    result.reserve(program->ir().pieces().size());
    for (const auto& piece : program->ir().pieces())
        result.push_back(piece.pieceId);
    return result;
}

namespace {

using namespace harkenprojectschema;
using namespace VNX;

AuthoringGuid toAuthoringGuid(const QUuid& value)
{
    const QByteArray bytes = value.toRfc4122();
    if (bytes.size() != 16) return {};
    AuthoringGuid result{};
    for (int i = 0; i < 8; ++i)
        result.high = (result.high << 8) | static_cast<uint8_t>(bytes[i]);
    for (int i = 8; i < 16; ++i)
        result.low = (result.low << 8) | static_cast<uint8_t>(bytes[i]);
    return result;
}

bool isContentType(ProjectItem::Type type)
{
    return type == ProjectItem::Type::Layer || type == ProjectItem::Type::Sound
        || type == ProjectItem::Type::Container || type == ProjectItem::Type::Track
        || type == ProjectItem::Type::Piece;
}

ContentKind contentKind(ProjectItem::Type type)
{
    switch (type) {
    case ProjectItem::Type::Layer: return ContentKind::Layer;
    case ProjectItem::Type::Sound: return ContentKind::Sound;
    case ProjectItem::Type::Container: return ContentKind::Container;
    case ProjectItem::Type::Track: return ContentKind::Track;
    case ProjectItem::Type::Piece: return ContentKind::Piece;
    default: return ContentKind::Sound;
    }
}

void appendDiagnostic(ProjectInputCompileResult& result,
                      ProjectInputDiagnosticCode code, const QString& message,
                      const ProjectItem* item, const QUuid& entity,
                      const QString& field)
{
    result.diagnostics.push_back({code, message, item ? item->guid : QUuid{}, entity, field});
}

void appendDiagnostic(std::vector<ProjectInputDiagnostic>& diagnostics,
                      ProjectInputDiagnosticCode code, const QString& message,
                      const ProjectItem* item, const QUuid& entity,
                      const QString& field)
{
    diagnostics.push_back(
        {code, message, item ? item->guid : QUuid{}, entity, field});
}

std::shared_ptr<CanonicalRuntimeProjectSourceIndexV1> buildSourceIndex(
    const ProjectData& project)
{
    auto index = std::make_shared<CanonicalRuntimeProjectSourceIndexV1>();
    const auto canonicalBindingPath = [&](const ProjectItem* item) {
        QString path = QDir::fromNativeSeparators(project.objectFilePath(item));
        if (path.isEmpty()) {
            path = QStringLiteral("Project/%1.vp").arg(
                item->guid.toString(QUuid::WithoutBraces));
        }
        if (QDir::isAbsolutePath(path) && !project.projectPath.isEmpty()) {
            const QFileInfo projectInfo(project.projectPath);
            const QString projectDirectory = projectInfo.isDir()
                ? projectInfo.absoluteFilePath() : projectInfo.absolutePath();
            path = QDir(projectDirectory).relativeFilePath(path);
        }
        return QDir::cleanPath(path);
    };
    const auto assignDocumentPath = [&](const ProjectItem* item) {
        std::vector<const ProjectItem*> pending{item};
        std::set<const ProjectItem*> visited;
        while (!pending.empty()) {
            const auto* current = pending.back();
            pending.pop_back();
            if (!current || !visited.insert(current).second) continue;
            index->documentPaths[current] = canonicalBindingPath(current).toStdString();
            for (const auto& child : current->children)
                pending.push_back(child.get());
        }
    };
    for (const auto* binding : project.allFileBindings()) {
        if (!binding || !binding->rootItem) continue;
        assignDocumentPath(binding->rootItem.get());
    }
    std::set<harkenstudio::ObjectId> stableIds;
    std::set<ProjectItem*> visited;
    std::map<ProjectItem*, ProjectItem*> owners;
    std::vector<ProjectItem*> items;
    std::vector<ProjectItem*> pending;
    if (project.root) pending.push_back(project.root.get());
    for (auto* binding : project.allFileBindings())
        if (binding && binding->rootItem)
            pending.push_back(binding->rootItem.get());
    std::reverse(pending.begin(), pending.end());
    const auto structureError = [&](ProjectItem* item, ProjectItem* related,
                                     const QString& message) {
        index->valid = false;
        appendDiagnostic(index->structureDiagnostics,
                         ProjectInputDiagnosticCode::FrontEndRejected,
                         message, item, related ? related->guid : QUuid{},
                         QStringLiteral("parent"));
    };
    while (!pending.empty()) {
        ProjectItem* item = pending.back();
        pending.pop_back();
        if (!item || !visited.insert(item).second) continue;
        items.push_back(item);
        if (!item->guid.isNull()) {
            if (!index->byQtGuid.emplace(item->guid, item).second)
                index->duplicateGuids.insert(item->guid);
            if (!index->byGuid.emplace(toAuthoringGuid(item->guid), item).second)
                index->valid = false;
        }
        if (item->objectID != 0
            && !index->byObjectId.emplace(item->objectID, item).second)
            index->valid = false;
        if (!harkenstudio::IsNull(item->runtimeId()) && !stableIds.insert(item->runtimeId()).second)
            index->duplicateStableIds.insert(item->runtimeId());
        if (isContentType(item->type))
            index->definitions.push_back(item);
        if (item->type == ProjectItem::Type::Action)
            index->actions.push_back(item);
        if (item->type == ProjectItem::Type::Bus)
            index->buses.push_back(item);
        for (const auto& child : item->children) {
            if (!child) {
                structureError(item, nullptr,
                               QStringLiteral("Project ownership contains a missing child"));
                continue;
            }
            if (!owners.emplace(child.get(), item).second)
                structureError(child.get(), item,
                               QStringLiteral("Project item has duplicate ownership edges"));
        }
        for (auto child = item->children.rbegin(); child != item->children.rend(); ++child)
            if (*child) pending.push_back(child->get());
    }
    std::set<ProjectItem*> checked;
    for (ProjectItem* item : items) {
        const auto owner = owners.find(item);
        if (item->parent && (owner == owners.end() || owner->second != item->parent))
            structureError(item, nullptr,
                           QStringLiteral("Project parent is not its ownership parent"));
        std::set<ProjectItem*> path;
        ProjectItem* current = item;
        while (current && !checked.contains(current)) {
            if (!path.insert(current).second) {
                structureError(current, nullptr,
                               QStringLiteral("Project ownership contains a cycle"));
                break;
            }
            const auto parent = owners.find(current);
            current = parent == owners.end() ? nullptr : parent->second;
        }
        checked.insert(path.begin(), path.end());
    }
    if (!index->structureDiagnostics.empty()) return index;
    for (ProjectItem* item : index->definitions) {
        const auto owner = owners.find(item);
        ProjectItem* parent = owner == owners.end() ? nullptr : owner->second;
        while (parent && !isContentType(parent->type)) {
            if (parent->type != ProjectItem::Type::None) {
                structureError(item, parent,
                               QStringLiteral("Content ownership passes through a non-content object"));
                parent = nullptr;
                break;
            }
            const auto next = owners.find(parent);
            parent = next == owners.end() ? nullptr : next->second;
        }
        index->parentDefinition[item] = parent;
    }
    if (!index->structureDiagnostics.empty()) return index;
    for (ProjectItem* parent : index->definitions) {
        std::vector<ProjectItem*> descendants;
        const auto appendChildren = [&](ProjectItem* item) {
            for (auto child = item->children.rbegin(); child != item->children.rend(); ++child)
                descendants.push_back(child->get());
        };
        appendChildren(parent);
        while (!descendants.empty()) {
            ProjectItem* child = descendants.back();
            descendants.pop_back();
            if (isContentType(child->type))
                index->children[parent].push_back(child);
            else
                appendChildren(child);
        }
    }
    std::sort(index->definitions.begin(), index->definitions.end(),
              [](const auto* a, const auto* b) {
        if (a->runtimeId() != b->runtimeId())
            return a->runtimeId() < b->runtimeId();
        return a->guid.toString() < b->guid.toString();
    });
    std::sort(index->buses.begin(), index->buses.end(),
              [](const auto* a, const auto* b) {
        return a->objectID < b->objectID;
    });
    return index;
}

harkenprojectschema::RuntimeIdentityKindV1 identityKindForItem(
    ProjectItem::Type type)
{
    using Kind = harkenprojectschema::RuntimeIdentityKindV1;
    switch (type) {
    case ProjectItem::Type::Sound: return Kind::Sound;
    case ProjectItem::Type::Container: return Kind::Container;
    case ProjectItem::Type::Track: return Kind::Track;
    case ProjectItem::Type::Piece: return Kind::Piece;
    case ProjectItem::Type::Layer: return Kind::Layer;
    case ProjectItem::Type::Bus: return Kind::Bus;
    case ProjectItem::Type::Action: return Kind::Action;
    case ProjectItem::Type::Variable: return Kind::Variable;
    case ProjectItem::Type::Attenuation: return Kind::Attenuation;
    default: return Kind::Invalid;
    }
}

harkenprojectschema::RuntimeIdentityIndexBuildOutcomeV1
buildRuntimeIdentityIndex(const ProjectData& project,
                          const std::vector<const ProjectItem*>& buses,
                          const std::optional<
                              harkenprojectschema::RuntimeIdentityClaimV1>&
                              transientPreviewPiece = std::nullopt)
{
    using namespace harkenprojectschema;
    std::vector<RuntimeIdentityIndexBuildRowV1> claims;
    std::set<const ProjectItem*> visited;
    std::function<void(const ProjectItem*)> visit = [&](const ProjectItem* item) {
        if (!item || !visited.insert(item).second) return;
        const auto kind = identityKindForItem(item->type);
        if (kind != RuntimeIdentityKindV1::Invalid) {
            const harkenstudio::ObjectId id = item->runtimeId();
            const auto guid = toAuthoringGuid(item->guid);
            claims.push_back({item->objectID,
                              {id, kind, guid, {guid, kind, 0, id}}});
        }
        if (const auto* mix = item->comp<VMixNode>()) {
            const auto effectKind = item->type == ProjectItem::Type::Bus
                ? RuntimeIdentityKindV1::BusEffect
                : RuntimeIdentityKindV1::PlayableEffect;
            for (const auto& effect : mix->effects.entries()) {
                const auto owner = toAuthoringGuid(item->guid);
                claims.push_back({0,
                                  {effect.runtimeId(), effectKind, toAuthoringGuid(effect.guid),
                                   {owner, effectKind, 0,
                                    effect.runtimeId()}}});
            }
            for (const auto& binding : mix->bindings.entries()) {
                const auto owner = toAuthoringGuid(item->guid);
                claims.push_back({0,
                                  {binding.runtimeId(),
                                   RuntimeIdentityKindV1::Binding, toAuthoringGuid(binding.guid),
                                   {owner, RuntimeIdentityKindV1::Binding, 0,
                                    binding.runtimeId()}}});
            }
        }
        if (const auto* timeline = item->comp<VTrack>()) {
            for (const auto& clip : timeline->clips.items) {
                const auto owner = toAuthoringGuid(item->guid);
                claims.push_back({0,
                                  {clip.runtimeId(),
                                   RuntimeIdentityKindV1::TimelineClip, toAuthoringGuid(clip.clipGuid),
                                   {owner, RuntimeIdentityKindV1::TimelineClip, 0,
                                    clip.runtimeId()}}});
            }
        }
        if (const auto* timeline = item->comp<VPiece>()) {
            for (const auto& marker : timeline->markers.items) {
                const auto owner = toAuthoringGuid(item->guid);
                claims.push_back({0,
                                  {marker.runtimeId(),
                                   RuntimeIdentityKindV1::TimelineMarker, toAuthoringGuid(marker.markerGuid),
                                   {owner, RuntimeIdentityKindV1::TimelineMarker, 0,
                                    marker.runtimeId()}}});
            }
        }
        for (const auto& child : item->children) visit(child.get());
    };
    if (project.root) visit(project.root.get());
    for (const auto* binding : project.allFileBindings())
        if (binding && binding->rootItem) visit(binding->rootItem.get());
    // Bus roots may be kept in a bucket that is not reachable from the general
    // project root. Visit them explicitly so every Runtime-facing claim shares
    // the same project-wide collision domain.
    for (const auto* bus : buses) visit(bus);
    if (transientPreviewPiece.has_value()) {
        claims.push_back({UINT64_MAX, *transientPreviewPiece});
    }
    return BuildRuntimeIdentityIndexV1(claims);
}

std::shared_ptr<const harkenprojectschema::RuntimeIdentityIndexV1>
buildCanonicalRuntimeIdentityIndex(ProjectData& project)
{
    std::vector<const ProjectItem*> buses;
    std::set<const ProjectItem*> visited;
    const auto visit = [&](const ProjectItem* item, const auto& self) -> void {
        if (!item || !visited.insert(item).second)
            return;
        if (item->type == ProjectItem::Type::Bus) {
            buses.push_back(item);
        }
        for (const auto& child : item->children)
            self(child.get(), self);
    };
    if (project.root)
        visit(project.root.get(), visit);
    for (const auto* binding : project.allFileBindings())
        if (binding && binding->rootItem)
            visit(binding->rootItem.get(), visit);

    const auto outcome = buildRuntimeIdentityIndex(project, buses);
    return outcome.ok() ? outcome.index : nullptr;
}

std::optional<harkenprojectschema::AuthoringGuid> transientPreviewPieceGuid(
    const CanonicalRuntimeProjectSourceIndexV1& index,
    const harkenprojectschema::AuthoringGuid& trackGuid)
{
    constexpr std::uint64_t kHighSalt = UINT64_C(0x5a4f9c31d27be680);
    constexpr std::uint64_t kLowSalt = UINT64_C(0xa5b064ce2d84197f);
    const std::size_t attempts = index.byGuid.size() + 1;
    for (std::size_t salt = 0; salt < attempts; ++salt) {
        const harkenprojectschema::AuthoringGuid candidate{
            trackGuid.high ^ kHighSalt,
            trackGuid.low ^ (kLowSalt + static_cast<std::uint64_t>(salt))};
        if (!harkenprojectschema::IsNull(candidate)
            && index.byGuid.find(candidate) == index.byGuid.end()) {
            return candidate;
        }
    }
    return std::nullopt;
}

const harkenprojectschema::RuntimeIdentityClaimV1* findBusClaimForObjectHandle(
    const harkenprojectschema::RuntimeIdentityIndexV1& identity,
    uint32_t objectId)
{
    const auto* claim = identity.findBySourceHandle(
        harkenprojectschema::RuntimeIdentityKindV1::Bus, objectId);
    return claim && claim->kind ==
            harkenprojectschema::RuntimeIdentityKindV1::Bus
        ? claim : nullptr;
}

harkenstudio::ObjectId resolveBusObjectReference(
    const harkenprojectschema::RuntimeIdentityIndexV1& identity,
    uint32_t objectId)
{
    const auto* claim = findBusClaimForObjectHandle(identity, objectId);
    return claim ? claim->stableId : harkenstudio::ObjectId{};
}

std::shared_ptr<const harkenbuscompiler::CompiledBusResultV1>
compileBusResult(
    const ProjectData& project,
    const CanonicalRuntimeProjectSourceIndexV1& index,
    const std::shared_ptr<const harkenprojectschema::RuntimeIdentityIndexV1>&
        identityIndex,
    const std::shared_ptr<const harkenprojectschema::CompileTargetProfileV1>&
        compileTargetProfile,
    std::uint64_t projectGeneration,
    std::vector<ProjectInputDiagnostic>& diagnostics,
    PlatformScope scope)
{
    if (project.busses.size() != 1 || !project.busses.front().rootItem
        || project.busses.front().rootItem->type != ProjectItem::Type::Bus) {
        appendDiagnostic(diagnostics,
                         ProjectInputDiagnosticCode::UnsupportedField,
                         QStringLiteral("Project must contain exactly one structural Bus root"),
                         nullptr, {}, QStringLiteral("bus"));
        return {};
    }
    const ProjectItem* master = project.busses.front().rootItem.get();
    if (index.buses.empty()) return {};
    if (master->runtimeId() == harkenstudio::kMasterBusObjectId) {
        appendDiagnostic(diagnostics,
                         ProjectInputDiagnosticCode::UnsupportedField,
                         QStringLiteral("Project Master Bus must have its own UUID"),
                         master, {}, QStringLiteral("runtimeStableId"));
        return {};
    }
    if (!identityIndex) {
        appendDiagnostic(diagnostics,
                         ProjectInputDiagnosticCode::DuplicateIdentity,
                         QStringLiteral("Bus identity index rejected a duplicate claim"),
                         master, {}, QStringLiteral("runtimeStableId"));
        return {};
    }
    harkenbuscompiler::BusCompilerInputV1 input;
    input.projectGeneration = projectGeneration;
    input.identityIndex = identityIndex;
    input.compileTargetProfile = compileTargetProfile;
    if (!input.compileTargetProfile) {
        appendDiagnostic(diagnostics,
                         ProjectInputDiagnosticCode::UnsupportedField,
                         QStringLiteral("The project compile target profile is invalid"),
                         master, {}, QStringLiteral("sampleRate/channelConfig"));
        return {};
    }

    const auto unsupported = validateBusPublicationSupport(
        project, scope ? *scope : QUuid{});
    diagnostics.insert(diagnostics.end(), unsupported.begin(), unsupported.end());
    bool invalidBusProperty = !unsupported.empty();

    std::set<const ProjectItem*> addedBuses;
    std::function<void(const ProjectItem*, harkenstudio::ObjectId)> add =
        [&](const ProjectItem* item, harkenstudio::ObjectId parentId) {
        if (!item || item->type != ProjectItem::Type::Bus
            || !addedBuses.insert(item).second)
            return;
        const harkenstudio::ObjectId id = item->runtimeId();
        const auto* mix = item->comp<VMixNode>();
        harkenbuscompiler::BusAuthoringV1 bus;
        bus.busId = id;
        bus.parentId = parentId;
        const auto locator = PlayablePropertyAuthoringV1::definitionLocator(*item);
        const auto readBusDb = [&](harkenplayableproperties::PlayablePropertyIdV1 property,
                                   const QString& field, float* out) {
            const auto state = locator ? PlayablePropertyAuthoringV1::readProperty(
                const_cast<ProjectItem&>(*item), *locator,
                static_cast<std::uint8_t>(property), scope) : std::nullopt;
            if (!state || !state->value.isDouble() ||
                !std::isfinite(state->value.toDouble())) {
                appendDiagnostic(diagnostics, ProjectInputDiagnosticCode::UnsupportedField,
                    QStringLiteral("Bus property is invalid for target platform"),
                    item, {}, field);
                invalidBusProperty = true;
                return;
            }
            *out = static_cast<float>(state->value.toDouble());
        };
        using Property = harkenplayableproperties::PlayablePropertyIdV1;
        readBusDb(Property::Volume, QStringLiteral("volume"), &bus.faderDb);
        readBusDb(Property::MakeUpGain, QStringLiteral("makeUpGain"),
                  &bus.makeUpGainDb);
        readBusDb(Property::OutputBusVolume, QStringLiteral("outputBusVolume"),
                  &bus.outputBusVolumeDb);
        const auto sourcePath = index.documentPaths.find(item);
        const std::string documentPath = sourcePath != index.documentPaths.end()
            ? sourcePath->second
            : QStringLiteral("Project/%1.vp")
                  .arg(item->guid.toString(QUuid::WithoutBraces))
                  .toStdString();
        bus.source = {toAuthoringGuid(item->guid), documentPath, "/volume"};
        const uint32_t effectFirst = static_cast<uint32_t>(input.effects.size());
        if (mix) {
            for (uint32_t slot = 0; slot < mix->effects.entries().size(); ++slot) {
                const auto& effect = mix->effects.entries()[slot];
                auto effectSource = bus.source;
                effectSource.fieldPath = "/effects/" + std::to_string(slot);
                const uint32_t parameterFirst =
                    static_cast<uint32_t>(input.effectParameters.size());
                for (uint32_t i = 0; i < effect.parameters.size(); ++i) {
                    auto parameterSource = effectSource;
                    parameterSource.fieldPath += "/parameters/"
                        + std::to_string(i);
                    input.effectParameters.push_back({effect.runtimeId(), i,
                                                      effect.parameters[i],
                                                      parameterSource});
                }
                input.effects.push_back({id, effect.runtimeId(), slot,
                                         static_cast<uint32_t>(effect.type), 2,
                                         effect.bypass, parameterFirst,
                                         static_cast<uint32_t>(effect.parameters.size()),
                                         effectSource});
            }
            const auto sendState = locator ? PlayablePropertyAuthoringV1::readProperty(
                const_cast<ProjectItem&>(*item), *locator,
                static_cast<std::uint8_t>(Property::Sends), scope) : std::nullopt;
            if (!sendState || !sendState->value.isArray()) {
                appendDiagnostic(diagnostics, ProjectInputDiagnosticCode::UnsupportedField,
                    QStringLiteral("Bus sends are invalid for target platform"),
                    item, {}, QStringLiteral("sends"));
                invalidBusProperty = true;
            } else for (qsizetype sendIndex = 0;
                 sendIndex < sendState->value.toArray().size(); ++sendIndex) {
                auto sendSource = bus.source;
                sendSource.fieldPath = "/sends/" + std::to_string(sendIndex);
                const QJsonValue rowValue = sendState->value.toArray().at(sendIndex);
                const QJsonObject row = rowValue.toObject();
                const auto targetValue = row.value(QStringLiteral("targetBusID"));
                const auto levelValue = row.value(QStringLiteral("sendLevel"));
                const double targetNumber = targetValue.toDouble(-1.0);
                const double levelNumber = levelValue.toDouble(
                    std::numeric_limits<double>::quiet_NaN());
                const float levelDb = static_cast<float>(levelNumber);
                if (!rowValue.isObject() || !targetValue.isDouble() ||
                    targetNumber < 0.0 || targetNumber > UINT32_MAX ||
                    std::trunc(targetNumber) != targetNumber ||
                    !levelValue.isDouble() || !std::isfinite(levelNumber) ||
                    !std::isfinite(levelDb) || levelDb < -96.0f || levelDb > 24.0f ||
                    !std::isfinite(VMixNode::dbToLinear(levelDb))) {
                    appendDiagnostic(diagnostics, ProjectInputDiagnosticCode::UnsupportedField,
                        QStringLiteral("Bus send row %1 is invalid for target platform")
                            .arg(sendIndex), item, {}, QStringLiteral("sends"));
                    invalidBusProperty = true;
                    continue;
                }
                const auto targetHandle = static_cast<std::uint32_t>(targetNumber);
                const harkenstudio::ObjectId target = resolveBusObjectReference(
                    *identityIndex, targetHandle == kMasterBusID
                        ? master->objectID : targetHandle);
                if (harkenstudio::IsNull(target)) {
                    appendDiagnostic(diagnostics, ProjectInputDiagnosticCode::MissingReference,
                        QStringLiteral("Bus send row %1 has no Bus target")
                            .arg(sendIndex), item, {}, QStringLiteral("sends"));
                    invalidBusProperty = true;
                    continue;
                }
                input.sends.push_back(
                    {id, target, levelDb == 0.0f ? 0.0f : levelDb, sendSource});
            }
        }
        bus.effectFirst = effectFirst;
        bus.effectCount = static_cast<uint32_t>(input.effects.size()) - effectFirst;
        input.buses.push_back(bus);
        for (const auto& child : item->children)
            if (child && child->type == ProjectItem::Type::Bus)
                add(child.get(), id);
    };
    add(master, harkenstudio::kMasterBusObjectId);
    if (addedBuses.size() != index.buses.size()) {
        appendDiagnostic(diagnostics,
                         ProjectInputDiagnosticCode::UnsupportedField,
                         QStringLiteral("Bus graph contains an additional or unreachable Bus"),
                         master, {}, QStringLiteral("bus"));
        return {};
    }
    if (invalidBusProperty) return {};
    const auto outcome = harkenbuscompiler::CompileBusProjectV1(input);
    for (const auto& diagnostic : outcome.diagnostics) {
        const auto code = diagnostic.code == harkenbuscompiler::BusDiagnosticCodeV1::MissingParent
            || diagnostic.code == harkenbuscompiler::BusDiagnosticCodeV1::MissingSendTarget
            ? ProjectInputDiagnosticCode::MissingReference
            : ProjectInputDiagnosticCode::UnsupportedField;
        appendDiagnostic(diagnostics, code,
                         QStringLiteral("Bus compiler rejected authored routing (%1)")
                             .arg(static_cast<uint32_t>(diagnostic.code)),
                         nullptr, {}, QStringLiteral("bus"));
    }
    return outcome.result;
}


ProjectInputDiagnosticCode diagnosticCode(DiagnosticCode code)
{
    switch (code) {
    case DiagnosticCode::DuplicateGuid:
    case DiagnosticCode::DuplicateStableId: return ProjectInputDiagnosticCode::DuplicateIdentity;
    case DiagnosticCode::DanglingContentRef:
    case DiagnosticCode::DanglingOwnership: return ProjectInputDiagnosticCode::MissingReference;
    case DiagnosticCode::IllegalContentRefType:
    case DiagnosticCode::IllegalOwnership: return ProjectInputDiagnosticCode::InvalidReferenceType;
    default: return ProjectInputDiagnosticCode::FrontEndRejected;
    }
}

}

std::vector<ProjectInputDiagnostic> validateBusPublicationSupport(
    const VNX::ProjectData& project, QUuid targetPlatform)
{
    std::vector<ProjectInputDiagnostic> diagnostics;
    const PlatformScope scope = targetPlatform.isNull()
        ? std::nullopt : PlatformScope{targetPlatform};
    const auto inspect = [&](const ProjectItem* item, const auto& self) -> void {
        if (!item) return;
        if (item->type == ProjectItem::Type::Bus) {
            const auto reject = [&](const QString& field, const QString& reason) {
                appendDiagnostic(diagnostics, ProjectInputDiagnosticCode::UnsupportedField,
                    QStringLiteral("Bus '%1' (%2): %3 %4 cannot be published")
                        .arg(item->name,
                             item->guid.toString(QUuid::WithoutBraces),
                             field, reason),
                    item, {}, field);
            };
            if (const auto* bus = item->comp<VBus>()) {
                if (!std::isfinite(bus->pan) || bus->pan != 0.0f)
                    reject(QStringLiteral("pan"), QStringLiteral("is non-neutral or invalid"));
                if (bus->listenerRelativeRouting)
                    reject(QStringLiteral("listenerRelativeRouting"), QStringLiteral("is enabled"));
            }
            const auto locator = PlayablePropertyAuthoringV1::definitionLocator(*item);
            const auto pitch = locator ? PlayablePropertyAuthoringV1::readProperty(
                const_cast<ProjectItem&>(*item), *locator,
                static_cast<std::uint8_t>(
                    harkenplayableproperties::PlayablePropertyIdV1::Pitch), scope)
                : std::nullopt;
            if (!pitch || !pitch->value.isDouble()
                || !std::isfinite(pitch->value.toDouble()))
                reject(QStringLiteral("pitch"), QStringLiteral("is invalid for the target platform"));
            else if (pitch->value.toDouble() != 0.0)
                reject(QStringLiteral("pitch"), QStringLiteral("is non-neutral"));
        }
        for (const auto& child : item->children)
            self(child.get(), self);
    };
    for (const auto& binding : project.busses)
        inspect(binding.rootItem.get(), inspect);
    return diagnostics;
}

std::shared_ptr<const CanonicalRuntimeProjectSourceIndexV1>
canonicalRuntimeProjectSourceIndex(const VNX::ProjectData& project)
{
    return buildSourceIndex(project);
}

namespace {

bool variableControlHasPlayableGrid(const ProjectItem& item, PlatformScope scope)
{
    const auto* selection = item.comp<VSelection>();
    if (selection == nullptr)
        return false;
    if (selection->mode == SelectionMode::Match)
        return !selection->matchGrid(scope).axes.empty() && !selection->activeRows(scope).empty();
    if (selection->mode == SelectionMode::Blend)
        return !selection->activeRows(scope).empty();
    return true;
}

}

std::unordered_set<ProjectItem*> canonicalRuntimeDefinitions(
    const CanonicalRuntimeProjectSourceIndexV1& index, PlatformScope scope)
{
    std::unordered_set<ProjectItem*> included;
    for (ProjectItem* item : index.definitions) {
        if (item->type != ProjectItem::Type::Container)
            included.insert(item);
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (ProjectItem* item : index.definitions) {
            if (item->type != ProjectItem::Type::Container
                || included.find(item) != included.end()
                || !variableControlHasPlayableGrid(*item, scope))
                continue;
            bool hasRuntimeChild = false;
            for (const auto& occurrence :
                 item->comp<VSelection>()->activeRows(scope)) {
                const auto child = index.byGuid.find(
                    toAuthoringGuid(occurrence.childGuid));
                if (child != index.byGuid.end()
                    && included.find(child->second) != included.end()) {
                    hasRuntimeChild = true;
                    break;
                }
            }
            if (hasRuntimeChild)
                changed = included.insert(item).second || changed;
        }
    }
    return included;
}


float resolvedStudioSpatializationMix(
    ProjectItem& item, const CanonicalRuntimeProjectSourceIndexV1& index)
{
    const auto source = PlayablePropertyAuthoringV1::resolveSpatializationMix(
        item, [&index](const ProjectItem& child) -> ProjectItem* {
            const auto parent = index.parentDefinition.find(const_cast<ProjectItem*>(&child));
            return parent == index.parentDefinition.end() ? nullptr : parent->second;
        });
    return source ? static_cast<float>(source->state.value.toDouble()) : 1.0f;
}

bool copyStudioDefinitionPayload(
    ProjectItem& item, PlatformScope scope,
    const CanonicalRuntimeProjectSourceIndexV1& index,
    const FieldGroupEffectiveSnapshot& groups,
    const RuntimeIdentityIndexV1& identity, InputNode* output, QString* error)
{
    using Property = harkenplayableproperties::PlayablePropertyIdV1;
    const auto fail = [&](const QString& message) {
        if (error) *error = message;
        return false;
    };
    const auto found = groups.definitions.find(toFieldGroupGuid(item.guid));
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(item);
    if (!output || !groups.ok() || found == groups.definitions.end() || !locator
        || (item.playablePropertyPresence.bits & kFieldGroupPropertyMask) != 0)
        return fail(QStringLiteral("Definition field group snapshot is invalid"));
    for (const auto& platform : item.platformOverrides.rows)
        if ((platform.properties.presence.bits & kFieldGroupPropertyMask) != 0)
            return fail(QStringLiteral("Definition platform contains legacy field group properties"));
    InputNode node = *output;
    for (const auto& field : PlayablePropertyAuthoringV1::fields()) {
        const auto id = static_cast<uint8_t>(field.id);
        const auto property = static_cast<Property>(field.id);
        if (PlayablePropertyAuthoringV1::isComponentStored(id) || FieldGroupForProperty(property)
            || !PlayablePropertyAuthoringV1::isSlotApplicable(item.type, id))
            continue;
        const auto state = PlayablePropertyAuthoringV1::readProperty(item, *locator, id, scope);
        if (!state) return fail(QStringLiteral("Definition property is invalid"));
        if (!state->present) continue;
        node.presence.Set(property);
        switch (property) {
        case Property::Volume: node.volume = static_cast<float>(state->value.toDouble()); break;
        case Property::Pitch: node.pitch = static_cast<float>(state->value.toDouble()); break;
        case Property::MakeUpGain: node.makeUpGain = static_cast<float>(state->value.toDouble()); break;
        case Property::LoopCount: node.loopCount = static_cast<uint32_t>(state->value.toInteger()); break;
        case Property::StreamingMode: node.streamingMode = static_cast<uint32_t>(state->value.toInteger()); break;
        default: return fail(QStringLiteral("Definition property is unsupported"));
        }
    }
    const auto& effective = found->second;
    const auto* bus = identity.find(effective.mainOutput.mainOutputBusRef);
    if (!bus || bus->kind != RuntimeIdentityKindV1::Bus)
        return fail(QStringLiteral("Main Output Bus identity is missing"));
    node.presence.bits |= kFieldGroupPropertyMask;
    node.presence.Set(Property::SpatializationMix);
    node.mainOutputBusRef = bus->stableId;
    node.outputBusVolume = effective.mainOutput.outputBusVolume;
    node.sends.clear();
    for (const auto& send : effective.send.sends) {
        const auto* target = identity.find(send.targetBusID);
        if (!target || target->kind != RuntimeIdentityKindV1::Bus)
            return fail(QStringLiteral("Send Bus identity is missing"));
        node.sends.push_back({target->stableId, send.sendLevel});
    }
    const auto& spatial = effective.spatialRating;
    node.spatial.panning = spatial.spatialPanning;
    node.spatial.is3DEnabled = spatial.spatialIs3DEnabled;
    node.spatial.attenuationDistanceScaling = spatial.spatialAttenuationDistanceScaling;
    node.spatial.spatializationMix = resolvedStudioSpatializationMix(item, index);
    node.spatial.attenuationRef = {};
    if (!IsNull(spatial.spatialAttenuationRef)) {
        const auto* target = identity.find(spatial.spatialAttenuationRef);
        if (!target || target->kind != RuntimeIdentityKindV1::Attenuation)
            return fail(QStringLiteral("Attenuation identity is missing"));
        node.spatial.attenuationRef = target->stableId;
    }
    if (const auto* audio = item.comp<VAudio>()) {
        if (!node.presence.Has(Property::LoopCount)) node.loopCount = audio->loopCount;
        if (!node.presence.Has(Property::StreamingMode))
            node.streamingMode = static_cast<uint32_t>(audio->streamingMode);
    }
    node.presence.Set(Property::LoopCount);
    node.presence.Set(Property::StreamingMode);
    node.hasSignalGraph = true;
    node.signalGraph = {};
    node.presence.Set(Property::SignalGraph);
    if (const auto* mix = item.comp<VMixNode>()) {
        for (const ParameterBindingDef& binding : mix->bindings.entries()) {
            if (binding.typed && BindingContractSemantics::engineTargetId(binding.address)
                && harkenstudio::IsNull(binding.runtimeId()))
                return fail(QStringLiteral("Typed binding is missing a stable identity"));
        }
        uint32_t previous = 0;
        for (const EffectEntry& effect : mix->effects.entries()) {
            if (!effect.typed || harkenstudio::IsNull(effect.runtimeId()))
                return fail(QStringLiteral("Effect chain contains an untyped entry"));
            const auto id = static_cast<uint32_t>(node.signalGraph.nodes.size() + 1);
            EffectNode graphNode;
            graphNode.nodeId = id;
            graphNode.effectType = static_cast<uint32_t>(effect.type);
            graphNode.effectId = effect.runtimeId();
            for (size_t i = 0; i < effect.parameters.size(); ++i)
                graphNode.params.push_back({static_cast<uint32_t>(i),
                    std::bit_cast<uint32_t>(effect.parameters[i])});
            node.signalGraph.nodes.push_back(std::move(graphNode));
            if (previous != 0) node.signalGraph.edges.push_back({previous, id});
            previous = id;
        }
    }
    *output = std::move(node);
    return true;
}

bool copyStudioOccurrencePayload(
    ProjectItem& owner, const PlayablePropertyLocatorV1& locator,
    PlatformScope scope, PlayablePropertyPayloadV1* output, QString* error)
{
    const auto fail = [&] {
        if (error) *error = QStringLiteral("Occurrence property payload is invalid");
        return false;
    };
    const PlayablePropertyOccurrenceV1* occurrence = nullptr;
    const PlatformOverrideLedgerV1* platforms = nullptr;
    if (locator.kind == PlayablePropertyTargetKindV1::ContainerChild) {
        const auto resolved = PlayablePropertyAuthoringV1::containerChildLocator(
            owner, locator.stableOccurrenceOrdinal, scope);
        if (!output || !resolved || *resolved != locator) return fail();
        *output = PlayablePropertyPayloadV1{};
        return true;
    } else if (locator.kind == PlayablePropertyTargetKindV1::TrackClip) {
        if (const auto* timeline = owner.comp<VTrack>()) {
            for (const auto& clip : timeline->clips.items) {
                if (clip.clipGuid != locator.occurrenceGuid) continue;
                occurrence = &clip.playableProperties;
                platforms = &clip.platformOverrides;
                break;
            }
        }
    }
    if (!output || !occurrence || !platforms
        || (occurrence->presence.bits & kFieldGroupPropertyMask) != 0)
        return fail();
    for (const auto& platform : platforms->rows)
        if ((platform.properties.presence.bits & kFieldGroupPropertyMask) != 0)
            return fail();
    PlayablePropertyPayloadV1 payload;
    using Property = harkenplayableproperties::PlayablePropertyIdV1;
    for (const auto& field : PlayablePropertyAuthoringV1::fields()) {
        const auto property = static_cast<Property>(field.id);
        if (FieldGroupForProperty(property)) continue;
        const auto state = PlayablePropertyAuthoringV1::readProperty(
            owner, locator, static_cast<uint8_t>(field.id), scope);
        if (!state) return fail();
        if (!state->present) continue;
        payload.presence.Set(property);
        switch (property) {
        case Property::Volume: payload.volume = static_cast<float>(state->value.toDouble()); break;
        case Property::Pitch: payload.pitch = static_cast<float>(state->value.toDouble()); break;
        case Property::MakeUpGain: payload.makeUpGain = static_cast<float>(state->value.toDouble()); break;
        case Property::LoopCount: payload.loopCount = static_cast<uint32_t>(state->value.toInteger()); break;
        case Property::StreamingMode: payload.streamingMode = static_cast<uint32_t>(state->value.toInteger()); break;
        default: return fail();
        }
    }
    *output = std::move(payload);
    return true;
}

CanonicalRuntimeProjectContextV1 canonicalRuntimeProjectContext(
    VNX::ProjectData& project,
    uint64_t projectGeneration,
    std::optional<harkenstudio::ObjectId> previewTrackStableId)
{
    return canonicalRuntimeProjectContext(
        project, projectGeneration, previewTrackStableId,
        currentWorldPlatformScope(PlatformContextService::instance().context()));
}

CanonicalRuntimeProjectContextV1 canonicalRuntimeProjectContext(
    VNX::ProjectData& project,
    uint64_t projectGeneration,
    std::optional<harkenstudio::ObjectId> previewTrackStableId,
    PlatformScope busPlatformScope)
{
    CanonicalRuntimeProjectContextV1 context;
    auto sourceIndex = canonicalRuntimeProjectSourceIndex(project);
    context.sourceIndex = sourceIndex;
    context.diagnostics = sourceIndex->structureDiagnostics;
    for (const QUuid& guid : sourceIndex->duplicateGuids) {
        appendDiagnostic(
            context.diagnostics,
            ProjectInputDiagnosticCode::DuplicateIdentity,
            QStringLiteral("Duplicate ProjectItem GUID"),
            sourceIndex->byQtGuid.at(guid), guid, QStringLiteral("guid"));
    }
    for (harkenstudio::ObjectId id : sourceIndex->duplicateStableIds) {
        appendDiagnostic(
            context.diagnostics,
            ProjectInputDiagnosticCode::DuplicateIdentity,
            QStringLiteral("Duplicate runtimeStableId %1").arg(VNX::objectIdString(id)), nullptr,
            {}, QStringLiteral("runtimeStableId"));
    }
    if (!sourceIndex->valid || !context.diagnostics.empty())
        return context;

    std::optional<harkenprojectschema::RuntimeIdentityClaimV1>
        transientPreviewPiece;
    if (previewTrackStableId.has_value()) {
        const auto track = std::find_if(
            sourceIndex->definitions.begin(), sourceIndex->definitions.end(),
            [&](const ProjectItem* item) {
                return item && item->type == ProjectItem::Type::Track
                    && item->runtimeId() == *previewTrackStableId;
            });
        if (track == sourceIndex->definitions.end()) {
            appendDiagnostic(
                context.diagnostics,
                ProjectInputDiagnosticCode::MissingReference,
                QStringLiteral("Preview Track is missing"), nullptr, {},
                QStringLiteral("previewTrack"));
            return context;
        }
        const auto wrapperGuid = transientPreviewPieceGuid(
            *sourceIndex, toAuthoringGuid((*track)->guid));
        if (!wrapperGuid.has_value()) {
            appendDiagnostic(
                context.diagnostics,
                ProjectInputDiagnosticCode::DuplicateIdentity,
                QStringLiteral("Track Preview wrapper GUID space is exhausted"),
                *track, {}, QStringLiteral("previewTrack"));
            return context;
        }
        context.previewTrack = *track;
        context.previewRootGuid = *wrapperGuid;
        context.previewRootStableId = *wrapperGuid;
        transientPreviewPiece = harkenprojectschema::RuntimeIdentityClaimV1{
            context.previewRootStableId,
            harkenprojectschema::RuntimeIdentityKindV1::Piece,
            context.previewRootGuid,
            {context.previewRootGuid,
             harkenprojectschema::RuntimeIdentityKindV1::Piece, 0,
             context.previewRootStableId}};
    }

    context.compileTargetProfile = CompileTargetProfileCatalog::resolve(project);
    if (!context.compileTargetProfile) {
        appendDiagnostic(
            context.diagnostics, ProjectInputDiagnosticCode::UnsupportedField,
            QStringLiteral("The project compile target profile is invalid"),
            nullptr, {}, QStringLiteral("sampleRate/channelConfig"));
        return context;
    }
    QString referenceError;
    PlaybackReferenceError referenceCode;
    if (!validatePlaybackReferences(project, &referenceError, &referenceCode)) {
        appendDiagnostic(context.diagnostics,
                         referenceCode == PlaybackReferenceError::InvalidTargetType
                             ? ProjectInputDiagnosticCode::InvalidReferenceType
                             : ProjectInputDiagnosticCode::MissingReference,
                         referenceError, nullptr, {}, QStringLiteral("playbackReferences"));
        return context;
    }

    std::vector<const ProjectItem*> busPointers(
        sourceIndex->buses.begin(), sourceIndex->buses.end());
    const auto identityOutcome = buildRuntimeIdentityIndex(
        project, busPointers, transientPreviewPiece);
    context.identityIndex = identityOutcome.ok()
        ? identityOutcome.index : nullptr;
    if (!context.identityIndex) {
        appendDiagnostic(
            context.diagnostics,
            ProjectInputDiagnosticCode::DuplicateIdentity,
            QStringLiteral("Project runtime identity index is ambiguous"),
            nullptr, {}, QStringLiteral("runtimeStableId"));
        return context;
    }
    context.busResult = compileBusResult(
        project, *sourceIndex, context.identityIndex,
        context.compileTargetProfile, projectGeneration,
        context.diagnostics, busPlatformScope);
    if (!context.busResult && context.diagnostics.empty()) {
        appendDiagnostic(
            context.diagnostics, ProjectInputDiagnosticCode::MissingIdentity,
            QStringLiteral("Project has no canonical Master Bus"), nullptr, {},
            QStringLiteral("bus"));
    }
    return context;
}

ProjectInputCompileResult compileStudioProjectInput(
    const ProjectData& project, std::optional<harkenstudio::ObjectId> previewTrackStableId,
    uint64_t projectGeneration)
{
    ProjectInputCompileResult result;
    auto context = canonicalRuntimeProjectContext(
        const_cast<ProjectData&>(project), projectGeneration,
        previewTrackStableId);
    result.diagnostics = context.diagnostics;
    result.identityIndex = context.identityIndex;
    result.compileTargetProfile = context.compileTargetProfile;
    result.busResult = context.busResult;
    if (context.sourceIndex && context.identityIndex) {
        for (const ProjectItem* bus : context.sourceIndex->buses) {
            if (!bus) continue;
            const auto* claim = context.identityIndex->findBySourceHandle(
                harkenprojectschema::RuntimeIdentityKindV1::Bus,
                bus->objectID);
            if (claim)
                result.legacyPackBusDisplayRows.push_back(
                    {claim->stableId, bus->name});
        }
        std::sort(result.legacyPackBusDisplayRows.begin(),
                  result.legacyPackBusDisplayRows.end(),
                  [](const LegacyPackBusDisplayRowV1& left,
                     const LegacyPackBusDisplayRowV1& right) {
                      return left.busId < right.busId;
                  });
    }
    if (!context.ok()) return result;
    const auto& index = *context.sourceIndex;
    const auto scope = currentWorldPlatformScope(PlatformContextService::instance().context());
    const auto groups = buildFieldGroupEffectiveSnapshot(project, toFieldGroupScope(scope));
    if (!groups.ok()) {
        result.diagnostics = groups.diagnostics;
        return result;
    }

    ProjectInput input;
    input.projectId = runtimeObjectId(QUuid(project.projectId));
    input.identityIndex = result.identityIndex;
    input.compileTargetProfile = result.compileTargetProfile;
    std::set<QUuid> owned;
    for (const ProjectItem* item : index.definitions)
        for (const auto& child : item->children)
            if (child && isContentType(child->type)) owned.insert(child->guid);

    for (ProjectItem* item : index.definitions) {
        if (item->guid.isNull() || harkenstudio::IsNull(item->runtimeId())) {
            appendDiagnostic(result, ProjectInputDiagnosticCode::MissingIdentity,
                             QStringLiteral("Content object has no stable identity"), item,
                             {}, QStringLiteral("identity"));
            continue;
        }
        InputNode node;
        node.kind = contentKind(item->type);
        node.guid = toAuthoringGuid(item->guid);
        node.editorOrder = item->objectID;
        QString payloadError;
        if (!copyStudioDefinitionPayload(*item, scope, index, groups, *result.identityIndex,
                                         &node, &payloadError)) {
            appendDiagnostic(result, ProjectInputDiagnosticCode::FrontEndRejected,
                             payloadError, item, {}, QStringLiteral("properties"));
            continue;
        }
        for (const auto& child : item->children)
            if (child && isContentType(child->type))
                node.ownedChildren.push_back(toAuthoringGuid(child->guid));
        if (item->type == ProjectItem::Type::Container) {
            for (const auto& row : item->comp<VSelection>()->activeRows(scope)) {
                const auto locator = PlayablePropertyAuthoringV1::containerChildLocator(
                    *item, row.stableOccurrenceOrdinal, scope);
                const auto child = index.byQtGuid.find(row.childGuid);
                if (!locator || child == index.byQtGuid.end()) {
                    appendDiagnostic(result, ProjectInputDiagnosticCode::MissingReference,
                        QStringLiteral("Container occurrence target is missing"), item,
                        row.childGuid, QStringLiteral("selections"));
                    continue;
                }
                InputContainerChildOccurrence occurrence;
                occurrence.childGuid = toAuthoringGuid(row.childGuid);
                occurrence.stableOccurrenceOrdinal = row.stableOccurrenceOrdinal;
                if (!copyStudioOccurrencePayload(*item, *locator, scope, &occurrence,
                                                  &payloadError)) {
                    appendDiagnostic(result, ProjectInputDiagnosticCode::FrontEndRejected,
                                     payloadError, item, row.childGuid,
                                     QStringLiteral("selections"));
                    continue;
                }
                node.containerChildOccurrences.push_back(std::move(occurrence));
            }
        }

        if (item->type == ProjectItem::Type::Track) {
            const auto* timeline = item->comp<VTrack>();
            if (!timeline) {
                appendDiagnostic(result, ProjectInputDiagnosticCode::UnsupportedField,
                                 QStringLiteral("Track timeline is missing"), item, {},
                                 QStringLiteral("clips"));
                continue;
            }
            for (size_t i = 0; i < timeline->clips.items.size(); ++i) {
                const VNX::Clip& source = timeline->clips.items[i];
                const auto target = index.byQtGuid.find(source.targetGuid);
                if (target == index.byQtGuid.end()) {
                    appendDiagnostic(result, ProjectInputDiagnosticCode::MissingReference,
                                     QStringLiteral("Clip target is missing"), item,
                                     source.clipGuid, QStringLiteral("clips.target"));
                    continue;
                }
                if (!IsPlayableRefKind(contentKind(target->second->type))) {
                    appendDiagnostic(result, ProjectInputDiagnosticCode::InvalidReferenceType,
                                     QStringLiteral("ProjectSchema does not accept this Clip target type"),
                                     item, source.clipGuid, QStringLiteral("clips.target"));
                    continue;
                }
                InputClip clip;
                clip.guid = toAuthoringGuid(source.clipGuid);
                if (harkenstudio::IsNull(source.runtimeId())) {
                    appendDiagnostic(result, ProjectInputDiagnosticCode::MissingIdentity,
                                     QStringLiteral("Clip has no runtime stable identity"),
                                     item, source.clipGuid, QStringLiteral("clips.runtimeId()"));
                    continue;
                }
                clip.targetGuid = toAuthoringGuid(source.targetGuid);
                clip.time = source.start;
                clip.duration = source.duration;
                clip.semanticOrderKey = source.stableOccurrenceOrdinal;
                clip.stableOccurrenceOrdinal = source.stableOccurrenceOrdinal;
                const auto locator = PlayablePropertyAuthoringV1::trackClipLocator(
                    *item, source.clipGuid);
                if (!locator || !copyStudioOccurrencePayload(*item, *locator, scope,
                                                              &clip, &payloadError)) {
                    appendDiagnostic(result, ProjectInputDiagnosticCode::FrontEndRejected,
                                     payloadError, item, source.clipGuid,
                                     QStringLiteral("clips.properties"));
                    continue;
                }
                node.clips.push_back(std::move(clip));
            }
        } else if (item->type == ProjectItem::Type::Piece) {
            const auto* timeline = item->comp<VPiece>();
            if (!timeline) {
                appendDiagnostic(result, ProjectInputDiagnosticCode::UnsupportedField,
                                 QStringLiteral("Piece timeline is missing"), item, {},
                                 QStringLiteral("pieceTime"));
                continue;
            }
            InputPieceTimeSpec time;
            time.kind = timeline->pieceTime.domain == TimelineDomain::Musical
                ? PieceTimeKind::Musical : PieceTimeKind::Absolute;
            for (const auto& source : timeline->pieceTime.tempoMap)
                time.tempoMap.push_back({source.tick, source.microsecondsPerQuarter});
            for (const auto& source : timeline->pieceTime.meterMap)
                time.meterMap.push_back({source.tick,
                    static_cast<uint16_t>(source.numerator),
                    static_cast<uint16_t>(source.denominator)});
            node.timeSpec = std::move(time);
            node.startPoint = InputStartPoint{};
            uint32_t nextMarkerId = 1;
            for (const VNX::Marker& source : timeline->markers.items) {
                if (source.kind == VNX::MarkerKind::Entry) {
                    if (source.position != 0) {
                        appendDiagnostic(result, ProjectInputDiagnosticCode::UnsupportedField,
                                         QStringLiteral("ProjectSchema entry start point cannot carry an authored position"),
                                         item, source.markerGuid, QStringLiteral("markers.entry.position"));
                    }
                    continue;
                }
                if (harkenstudio::IsNull(source.runtimeId())) {
                    appendDiagnostic(result, ProjectInputDiagnosticCode::MissingIdentity,
                                     QStringLiteral("Marker has no runtime stable identity"),
                                     item, source.markerGuid, QStringLiteral("markers.runtimeId()"));
                    continue;
                }
                const uint32_t id = nextMarkerId++;
                InputMarker marker;
                marker.guid = toAuthoringGuid(source.markerGuid);
                marker.markerId = id;
                marker.kind = source.kind == VNX::MarkerKind::Exit ? harkenprojectschema::MarkerKind::Exit
                                                        : harkenprojectschema::MarkerKind::Custom;
                marker.position = source.position;
                node.markers.push_back(marker);
            }
        }
        input.nodes.push_back(std::move(node));
        if (owned.count(item->guid) == 0)
            input.roots.push_back(toAuthoringGuid(item->guid));
    }
    if (previewTrackStableId) {
        const ProjectItem* track = context.previewTrack;
        if (!track || harkenstudio::IsNull(context.previewRootStableId)
            || harkenprojectschema::IsNull(context.previewRootGuid))
            return result;
        InputNode wrapper;
        wrapper.kind = ContentKind::Piece;
        wrapper.guid = context.previewRootGuid;
        wrapper.presence.bits = kFieldGroupPropertyMask;
        wrapper.mainOutputBusRef = project.busses.front().rootItem->runtimeId();
        wrapper.hasSignalGraph = true;
        const AuthoringGuid trackGuid = toAuthoringGuid(track->guid);
        for (InputNode& node : input.nodes) {
            if (node.kind != ContentKind::Piece) continue;
            node.ownedChildren.erase(
                std::remove_if(
                    node.ownedChildren.begin(), node.ownedChildren.end(),
                    [&](const InputOwnershipEdge& edge) {
                        return edge.childGuid == trackGuid;
                    }),
                node.ownedChildren.end());
        }
        input.roots.erase(
            std::remove(input.roots.begin(), input.roots.end(), trackGuid),
            input.roots.end());
        wrapper.ownedChildren.push_back(trackGuid);
        wrapper.timeSpec = InputPieceTimeSpec{};
        wrapper.startPoint = InputStartPoint{};
        input.nodes.push_back(std::move(wrapper));
        input.roots.push_back(context.previewRootGuid);
        result.previewRootStableId = context.previewRootStableId;
    }
    if (!result.diagnostics.empty()) return result;

    auto snapshot = std::make_shared<CompiledProjectSnapshot>();
    snapshot->impl_->frontEnd = CompileFrontEnd(input);
    const FrontEndResult& compiled = snapshot->impl_->frontEnd;
    if (!compiled.ok()) {
        for (const Diagnostic& diagnostic : compiled.diagnostics)
            result.diagnostics.push_back({diagnosticCode(diagnostic.code),
                QString::fromLatin1(DiagnosticCodeName(diagnostic.code)), {}, {},
                QStringLiteral("frontEnd")});
        return result;
    }

    std::map<harkenstudio::ObjectId, const ProjectItem*> contentByStableId;
    for (const ProjectItem* item : index.definitions)
        contentByStableId.emplace(item->runtimeId(), item);
    for (const auto& object : compiled.project->objects) {
        if (object.kind != ContentKind::Sound) continue;
        const auto owner = contentByStableId.find(object.chassis().id);
        const ProjectItem* soundItem = owner == contentByStableId.end()
            ? nullptr : owner->second;
        const uint32_t sourceId = soundItem
            ? SoundSourceEditing::activeSourceId(
                  *soundItem,
                  scope.value_or(ProjectPlatformId{}),
                  SoundSourceEditing::previewLanguage(
                      *soundItem,
                      LanguageContextService::instance().language()))
            : 0;
        const AssetInfo* asset = sourceId != 0
            ? AssetRegistry::instance().getAsset(sourceId) : nullptr;
        if (!soundItem || !asset || asset->absolutePath.isEmpty()
            || asset->metadata.sampleRate == 0 || asset->metadata.channels == 0
            || asset->metadata.totalSamples == 0)
            continue;
        ResourceInput resource;
        resource.facts.valid = true;
        resource.facts.sampleRate = asset->metadata.sampleRate;
        resource.facts.channelCount = asset->metadata.channels;
        resource.facts.frameCount = asset->metadata.totalSamples;
        resource.seekable = true;
        snapshot->impl_->resources.emplace(object.chassis().id, resource);
        result.resources.push_back({object.chassis().id, sourceId, asset->absolutePath,
                                    asset->metadata.sampleRate, asset->metadata.channels,
                                    asset->metadata.totalSamples, true});
    }
    if (!result.diagnostics.empty()) return result;

    snapshot->impl_->ir = BuildPieceIR(
        *compiled.project, *compiled.facts, snapshot->impl_->resources,
        *compiled.resolution, *compiled.sends, *compiled.effectChains);
    if (!snapshot->impl_->ir) {
        result.diagnostics.push_back({ProjectInputDiagnosticCode::IrRejected,
            QStringLiteral("ProjectSchema rejected the resource-backed Piece IR"),
            {}, {}, QStringLiteral("pieceIR")});
        return result;
    }
    snapshot->impl_->program = BuildPieceProgram(*snapshot->impl_->ir);
    if (!snapshot->impl_->program) {
        result.diagnostics.push_back({ProjectInputDiagnosticCode::ProgramRejected,
            QStringLiteral("ProjectSchema rejected the Piece program"),
            {}, {}, QStringLiteral("pieceProgram")});
        return result;
    }

    const std::string rendered = RenderProject(*compiled.project);
    result.canonicalProject = QByteArray(rendered.data(), static_cast<qsizetype>(rendered.size()));
    result.snapshot = std::move(snapshot);
    return result;
}

harkenbuscompiler::PublishedBusProjectionV1 compileStudioBusProjectionForPlatform(
    const VNX::ProjectData& project, QUuid targetPlatform,
    std::vector<ProjectInputDiagnostic>* diagnostics)
{
    auto context = canonicalRuntimeProjectContext(
        const_cast<VNX::ProjectData&>(project), 0, std::nullopt, targetPlatform);
    if (diagnostics) *diagnostics = context.diagnostics;
    if (targetPlatform.isNull() || !context.ok()) return {};
    return harkenbuscompiler::BuildPublishedBusProjectionV1(
        *context.busResult,
        harkenbuscompiler::BusPublicationCapabilityVintageV1::
            TopologyVolumeAndSendNeutralOtherProperties);
}

const harkenbuscompiler::CompiledBusResultV1* compiledBusResult(
    const ProjectInputCompileResult& result)
{
    return result.busResult.get();
}

std::shared_ptr<const harkenprojectschema::RuntimeIdentityIndexV1>
canonicalRuntimeIdentityIndexForProject(VNX::ProjectData& project)
{
    return buildCanonicalRuntimeIdentityIndex(project);
}

}

VNX::CanonicalRuntimeProjectInputV1
VNX::CanonicalRuntimeProjectAdapter::compile(
    const VNX::ProjectData& project,
    std::optional<harkenstudio::ObjectId> previewTrackStableId,
    uint64_t projectGeneration)
{
    return HarkenEditor::Bridge::compileStudioProjectInput(
        project, previewTrackStableId, projectGeneration);
}

std::shared_ptr<const harkenprojectschema::RuntimeIdentityIndexV1>
VNX::canonicalRuntimeIdentityIndex(VNX::ProjectData& project)
{
    return HarkenEditor::Bridge::canonicalRuntimeIdentityIndexForProject(project);
}
