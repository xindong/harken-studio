#include "Core/Components/VSelection.h"
#include "FieldGroupEditing.h"

#include "CanonicalRuntimeProjectAdapterPrivate.h"
#include "Components/VTrack.h"
#include "PlayablePropertyAuthoringV1.h"
#include "ProjectTypes.h"
#include "FieldGroupProjectAdapter.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <type_traits>
#include <utility>

namespace VNX {
namespace {

using Guid = harkenprojectschema::AuthoringGuid;
using Property = harkenplayableproperties::PlayablePropertyIdV1;
using EditCode = FieldGroupEditDiagnosticCode;
using Diagnostics = std::vector<FieldGroupEditDiagnostic>;
using SourceIndex = HarkenEditor::Bridge::CanonicalRuntimeProjectSourceIndexV1;

struct EditingContext {
    std::shared_ptr<const SourceIndex> source;
    std::map<const ProjectItem*, Guid> identities;
    FieldGroupProjectView view;
    Guid masterBus;
    std::vector<Guid> platforms;
    Diagnostics diagnostics;
};

void addDiagnostic(Diagnostics& diagnostics, EditCode code, const QString& message,
                   const QUuid& definition = {}, std::optional<FieldGroupId> group = {},
                   FieldGroupScope scope = {}, const QString& field = {},
                   std::optional<std::size_t> row = {}, Guid related = {})
{
    diagnostics.push_back({code, message, definition, group, scope, field, row, related, {}});
}

QString fieldName(Property property)
{
    for (const auto& entry : harkenplayableproperties::kPlayablePropertyRegistryV1)
        if (entry.id == property) return QString::fromLatin1(entry.name);
    return {};
}

QString groupMessage(FieldGroupDiagnosticCode code)
{
    switch (code) {
    case FieldGroupDiagnosticCode::MissingReference:
        return QStringLiteral("Referenced object does not exist");
    case FieldGroupDiagnosticCode::WrongReferenceKind:
        return QStringLiteral("Referenced object has an incompatible type");
    case FieldGroupDiagnosticCode::DuplicateSendTarget:
        return QStringLiteral("Send targets must be unique");
    case FieldGroupDiagnosticCode::MissingLocalHistory:
        return QStringLiteral("A local or root field group requires a complete current value");
    case FieldGroupDiagnosticCode::EmptyRequiredReference:
        return QStringLiteral("A Bus reference is required");
    case FieldGroupDiagnosticCode::InvalidValue:
        return QStringLiteral("Field group value is outside its valid range");
    case FieldGroupDiagnosticCode::SharedUseShared:
        return QStringLiteral("Shared settings cannot use UseShared mode");
    case FieldGroupDiagnosticCode::InvalidPlatform:
        return QStringLiteral("Field group platform is invalid");
    case FieldGroupDiagnosticCode::InvalidMode:
        return QStringLiteral("Field group mode is invalid");
    default:
        return QStringLiteral("Field group state or ownership is invalid");
    }
}

void appendGroupDiagnostics(Diagnostics& diagnostics, const EditingContext& context,
                            const std::vector<FieldGroupDiagnostic>& errors,
                            std::optional<Guid> definition = {},
                            FieldGroupScope scope = {})
{
    for (const auto& error : errors) {
        const auto identity = definition.value_or(error.definition);
        const auto item = context.source->byGuid.find(identity);
        const bool referenceError = error.code == FieldGroupDiagnosticCode::MissingReference ||
                                    error.code == FieldGroupDiagnosticCode::WrongReferenceKind ||
                                    error.code == FieldGroupDiagnosticCode::DuplicateSendTarget ||
                                    error.code == FieldGroupDiagnosticCode::InvalidReferenceCatalog;
        diagnostics.push_back({
            referenceError ? EditCode::InvalidReference : EditCode::InvalidGroups,
            groupMessage(error.code),
            item == context.source->byGuid.end() ? QUuid{} : item->second->guid,
            error.group, definition ? scope : error.scope,
            QString::fromUtf8(error.field.data(), static_cast<qsizetype>(error.field.size())),
            error.row, error.relatedGuid, error.code});
    }
}

void validateLegacyGroupState(const ProjectItem& item, Diagnostics& diagnostics)
{
    const auto validatePresence = [&](std::uint64_t bits, FieldGroupScope scope,
                                      const QString& field, std::optional<std::size_t> row = {}) {
        if ((bits & kFieldGroupPropertyMask) != 0)
            addDiagnostic(diagnostics, EditCode::InvalidGroups,
                QStringLiteral("Field group fields cannot use legacy property slots"),
                item.guid, {}, scope, field, row);
    };
    const auto validatePlatforms = [&](const PlatformOverrideLedgerV1& ledger,
                                       const QString& field, std::optional<std::size_t> row = {}) {
        for (const auto& platform : ledger.rows)
            validatePresence(platform.properties.presence.bits, toFieldGroupGuid(platform.platformId), field, row);
    };
    if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item.type)) {
        validatePresence(item.playablePropertyPresence.bits, {},
                         QStringLiteral("playablePropertyPresence"));
        validatePlatforms(item.platformOverrides, QStringLiteral("platformOverrides"));
    } else if (item.fieldGroups != DefinitionFieldGroups{}) {
        addDiagnostic(diagnostics, EditCode::UnsupportedDefinition,
            QStringLiteral("This object type cannot author field groups"),
            item.guid, {}, {}, QStringLiteral("fieldGroups"));
    }
    if (const auto* track = item.comp<VTrack>()) {
        for (std::size_t row = 0; row < track->clips.items.size(); ++row) {
            const auto& clip = track->clips.items[row];
            validatePresence(clip.playableProperties.presence.bits, {},
                             QStringLiteral("clips.playableProperties"), row);
            validatePlatforms(clip.platformOverrides,
                              QStringLiteral("clips.platformOverrides"), row);
        }
    }
}

EditingContext buildContext(ProjectData& project,
                            std::span<const FieldGroupDefinitionSnapshot> replacements = {},
                            std::span<const FieldGroupParentSnapshot> parents = {})
{
    EditingContext context;
    for (const auto& platform : project.platforms)
        context.platforms.push_back(toFieldGroupGuid(platform.id));
    context.source = HarkenEditor::Bridge::canonicalRuntimeProjectSourceIndex(project);
    if (!context.source->valid || !context.source->duplicateGuids.empty()) {
        for (const auto& diagnostic : context.source->structureDiagnostics)
            addDiagnostic(context.diagnostics, EditCode::InvalidProject, diagnostic.message,
                          diagnostic.itemGuid, {}, {}, diagnostic.field);
        if (context.diagnostics.empty())
            addDiagnostic(context.diagnostics, EditCode::InvalidProject,
                          QStringLiteral("Project ownership or identity is ambiguous"));
        return context;
    }
    for (const auto& [identity, item] : context.source->byGuid) {
        context.identities.emplace(item, identity);
        validateLegacyGroupState(*item, context.diagnostics);
    }
    std::map<QUuid, const DefinitionFieldGroups*> authored;
    for (const auto& replacement : replacements) {
        const auto item = context.source->byQtGuid.find(replacement.definition);
        if (item == context.source->byQtGuid.end()) {
            addDiagnostic(context.diagnostics, EditCode::UnknownDefinition,
                          QStringLiteral("Field group definition does not exist"),
                          replacement.definition);
        } else if (!PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item->second->type)) {
            addDiagnostic(context.diagnostics, EditCode::UnsupportedDefinition,
                          QStringLiteral("This object type cannot author field groups"),
                          replacement.definition);
        } else if (!authored.emplace(replacement.definition, &replacement.groups).second) {
            addDiagnostic(context.diagnostics, EditCode::InvalidOperation,
                          QStringLiteral("A definition cannot be replaced more than once"),
                          replacement.definition);
        }
    }
    if (!context.diagnostics.empty()) return context;
    context.view = buildFieldGroupProjectView(project, [&](const ProjectItem& item) {
        const auto replacement = authored.find(item.guid);
        return replacement == authored.end() ? &item.fieldGroups : replacement->second;
    });
    for (const auto& diagnostic : context.view.diagnostics)
        addDiagnostic(context.diagnostics, EditCode::InvalidProject, diagnostic.message,
                      diagnostic.itemGuid, {}, {}, diagnostic.field);
    std::set<QUuid> moved;
    for (const auto& replacement : parents) {
        const auto found = std::find_if(context.view.definitions.begin(), context.view.definitions.end(),
            [&](const auto& definition) { return definition.guid == toFieldGroupGuid(replacement.definition); });
        if (found == context.view.definitions.end() || !moved.insert(replacement.definition).second) {
            addDiagnostic(context.diagnostics, EditCode::InvalidOperation,
                QStringLiteral("A parent replacement requires a distinct existing definition"),
                replacement.definition);
        } else {
            found->parentGuid = toFieldGroupGuid(replacement.parent);
        }
    }
    const auto* master = project.busses.empty() ? nullptr : project.busses.front().rootItem.get();
    if (!master || master->type != ProjectItem::Type::Bus || master->guid.isNull() ||
        !context.identities.contains(master)) {
        addDiagnostic(context.diagnostics, EditCode::InvalidMasterBus,
                      QStringLiteral("Project Master Bus must have a valid persistent GUID"));
    } else {
        context.masterBus = context.identities.at(master);
    }
    return context;
}

ProjectItem* findDefinition(const EditingContext& context, const QUuid& guid,
                            Diagnostics& diagnostics)
{
    const auto found = context.source->byQtGuid.find(guid);
    if (found == context.source->byQtGuid.end()) {
        addDiagnostic(diagnostics, EditCode::UnknownDefinition,
                      QStringLiteral("Field group definition does not exist"), guid);
        return nullptr;
    }
    if (!PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(found->second->type)) {
        addDiagnostic(diagnostics, EditCode::UnsupportedDefinition,
                      QStringLiteral("This object type cannot author field groups"), guid);
        return nullptr;
    }
    return found->second;
}

Diagnostics validateView(const EditingContext& context,
                         std::span<const FieldGroupDefinitionView> definitions)
{
    Diagnostics diagnostics;
    const FieldGroupResolver resolver(definitions, context.masterBus);
    if (!resolver.ok()) {
        appendGroupDiagnostics(diagnostics, context, resolver.diagnostics());
        return diagnostics;
    }
    std::vector<FieldGroupScope> scopes{std::nullopt};
    for (const auto platform : context.platforms)
        scopes.push_back(platform);
    for (const auto& definition : definitions) {
        for (const auto scope : scopes) {
            const auto output = resolver.Resolve(definition.guid, scope, FieldGroupId::MainOutput);
            const auto send = resolver.Resolve(definition.guid, scope, FieldGroupId::Send);
            const auto spatial = resolver.Resolve(definition.guid, scope, FieldGroupId::SpatialRating);
            for (const auto* result : {&output, &send, &spatial}) {
                appendGroupDiagnostics(diagnostics, context, result->diagnostics);
                if (result->value)
                    appendGroupDiagnostics(diagnostics, context,
                        ValidateActiveFieldGroup(result->value->group,
                            result->value->payload, context.view.referenceTargets),
                        definition.guid, scope);
            }
            if (!output.ok() || !send.ok()) continue;
            const auto outputBus = std::get<MainOutputFieldGroup>(output.value->payload).mainOutputBusRef;
            const auto& sends = std::get<SendFieldGroup>(send.value->payload).sends;
            for (std::size_t row = 0; row < sends.size(); ++row) {
                if (sends[row].targetBusID == outputBus)
                    addDiagnostic(diagnostics, EditCode::MainOutputSendConflict,
                        QStringLiteral("A Send target cannot also be the effective Main Output"),
                        fromFieldGroupGuid(definition.guid),
                        FieldGroupId::Send, scope, QStringLiteral("targetBusID"), row, outputBus);
            }
        }
    }
    return diagnostics;
}

std::vector<FieldGroupScope> targetScopes(const ProjectData& project,
                                          const FieldGroupEditRequest& request,
                                          Diagnostics& diagnostics)
{
    std::vector<FieldGroupScope> scopes;
    bool valid = request.scopes.shared ? request.scopes.platforms.empty()
                                       : !request.scopes.platforms.empty();
    std::set<Guid> seen;
    for (const auto platform : request.scopes.platforms) {
        if (!ProjectPlatformSemantics::find(project.platforms, fromFieldGroupGuid(platform))
            || !seen.insert(platform).second)
            valid = false;
        scopes.push_back(platform);
    }
    if (!valid) {
        addDiagnostic(diagnostics, EditCode::InvalidScopes,
                      QStringLiteral("Select Shared settings or a nonempty list of distinct target platforms"),
                      request.definition, request.group, {}, QStringLiteral("scopes"));
        return {};
    }
    if (request.scopes.shared) scopes.push_back({});
    return scopes;
}

template <typename Payload>
const GroupState<Payload>* storedState(const AuthoredGroup<Payload>& group,
                                      FieldGroupScope scope)
{
    if (!scope) return &group.shared;
    const auto state = group.platforms.find(*scope);
    return state == group.platforms.end() ? nullptr : &state->second;
}

template <typename Payload>
GroupState<Payload>& writableState(AuthoredGroup<Payload>& group, FieldGroupScope scope)
{
    if (!scope) return group.shared;
    return group.platforms.try_emplace(*scope,
        GroupState<Payload>{FieldGroupMode::UseShared, {}}).first->second;
}

bool setField(MainOutputFieldGroup& payload, const SetFieldGroupField& edit, Guid master)
{
    if (edit.property == Property::MainOutputBusRef) {
        const auto* value = std::get_if<Guid>(&edit.value);
        if (!value) return false;
        payload.mainOutputBusRef = harkenprojectschema::IsNull(*value) ? master : *value;
        return true;
    }
    if (edit.property == Property::OutputBusVolume) {
        const auto* value = std::get_if<float>(&edit.value);
        if (!value) return false;
        payload.outputBusVolume = *value;
        return true;
    }
    return false;
}

bool setField(SpatialRatingFieldGroup& payload, const SetFieldGroupField& edit, Guid)
{
    if (edit.property == Property::SpatialIs3DEnabled) {
        const auto* value = std::get_if<bool>(&edit.value);
        if (!value) return false;
        payload.spatialIs3DEnabled = *value;
        return true;
    }
    if (edit.property == Property::SpatialAttenuationRef) {
        const auto* value = std::get_if<Guid>(&edit.value);
        if (!value) return false;
        payload.spatialAttenuationRef = *value;
        return true;
    }
    const auto* value = std::get_if<float>(&edit.value);
    if (!value) return false;
    switch (edit.property) {
    case Property::SpatialPanning: payload.spatialPanning = *value; return true;
    case Property::SpatialAttenuationDistanceScaling:
        payload.spatialAttenuationDistanceScaling = *value;
        return true;
    default: return false;
    }
}

bool setField(SendFieldGroup&, const SetFieldGroupField&, Guid)
{
    return false;
}

template <typename Payload>
void normalizeGroup(AuthoredGroup<Payload>& group)
{
    for (auto it = group.platforms.begin(); it != group.platforms.end();) {
        auto& state = it->second;
        if (state.mode == group.shared.mode) state.mode = FieldGroupMode::UseShared;
        if (state.localHistory && group.shared.localHistory)
            state.localFields &= FieldGroupDifferenceMask(*state.localHistory, *group.shared.localHistory);
        if (state.localFields == 0) {
            state.localHistory.reset();
            state.localFields = FieldGroupPropertyMask(Payload::groupId);
        } else if (state.localHistory) {
            Payload canonical;
            CopyFieldGroupFields(canonical, *state.localHistory, state.localFields);
            state.localHistory = std::move(canonical);
        }
        if (state.mode == FieldGroupMode::UseShared && !state.localHistory)
            it = group.platforms.erase(it);
        else
            ++it;
    }
}

template <typename Payload>
void editGroup(const EditingContext& context, const FieldGroupResolver& resolver,
               Guid definition, bool canInherit, const FieldGroupEditRequest& request,
               const std::vector<FieldGroupScope>& scopes,
               const AuthoredGroup<Payload>& before, AuthoredGroup<Payload>& after,
               Diagnostics& diagnostics)
{
    const auto* field = std::get_if<SetFieldGroupField>(&request.operation);
    const auto* clear = std::get_if<ClearFieldGroupFieldOverride>(&request.operation);
    const bool sendsEdit = std::holds_alternative<ReplaceFieldGroupSends>(request.operation);
    if (field || sendsEdit || clear) {
        auto inspected = scopes;
        if (request.scopes.shared) {
            inspected.clear();
            for (const auto platform : context.platforms) inspected.push_back(platform);
        }
        for (const auto scope : inspected) {
            const auto current = resolver.Resolve(definition, scope, request.group);
            appendGroupDiagnostics(diagnostics, context, current.diagnostics);
            if (current.ok() && canInherit && current.value->effectiveInherit)
                addDiagnostic(diagnostics, EditCode::ReadOnly,
                              QStringLiteral("Inherited field group fields cannot be edited"),
                              request.definition, request.group, scope, QStringLiteral("inherit"));
        }
    }
    if (!diagnostics.empty()) return;
    for (const auto scope : scopes) {
        auto& next = writableState(after, scope);
        const auto initializeLocal = [&]() {
            const auto local = ResolveFieldGroupLocal(after, scope);
            if (local.localHistory && local.localFields == FieldGroupPropertyMask(request.group))
                return true;
            const auto current = resolver.Resolve(definition, scope, request.group);
            appendGroupDiagnostics(diagnostics, context, current.diagnostics);
            if (!current.ok()) return false;
            auto payload = std::get<Payload>(current.value->payload);
            if (next.localHistory)
                CopyFieldGroupFields(payload, *next.localHistory, next.localFields);
            next.localHistory = std::move(payload);
            next.localFields = FieldGroupPropertyMask(request.group);
            return true;
        };
        if (const auto* mode = std::get_if<SetFieldGroupMode>(&request.operation)) {
            if (!scope && mode->mode == FieldGroupMode::UseShared) {
                addDiagnostic(diagnostics, EditCode::InvalidOperation,
                              QStringLiteral("Shared inheritance must be enabled or disabled"),
                              request.definition, request.group, scope, QStringLiteral("inherit"));
                continue;
            }
            if (mode->mode == FieldGroupMode::Local && !initializeLocal()) continue;
            next.mode = mode->mode;
            if (!scope)
                for (auto& [platform, state] : after.platforms) state.mode = FieldGroupMode::UseShared;
        } else if (std::holds_alternative<RebuildLocalFieldGroup>(request.operation)) {
            const auto current = resolver.Resolve(definition, scope, request.group);
            appendGroupDiagnostics(diagnostics, context, current.diagnostics);
            if (!current.ok()) continue;
            next.localHistory = std::get<Payload>(current.value->payload);
            next.localFields = FieldGroupPropertyMask(request.group);
            next.mode = FieldGroupMode::Local;
            if (!scope) after.platforms.clear();
        } else if (clear) {
            if (next.localHistory)
                next.localFields &= ~(FieldGroupFieldMask(clear->property));
        } else {
            if (!scope && !initializeLocal()) continue;
            if (!next.localHistory) {
                next.localHistory.emplace();
                next.localFields = 0;
            }
            const auto property = field ? field->property : Property::Sends;
            const auto bit = FieldGroupFieldMask(property);
            if (field) {
                if (!setField(*next.localHistory, *field, context.masterBus)) {
                    addDiagnostic(diagnostics, EditCode::InvalidOperation,
                                  QStringLiteral("Field group field value has an incompatible type"),
                                  request.definition, request.group, scope, fieldName(field->property));
                    continue;
                }
            } else if constexpr (std::is_same_v<Payload, SendFieldGroup>) {
                *next.localHistory = std::get<ReplaceFieldGroupSends>(request.operation).value;
            }
            next.localFields |= bit;
            if (!scope)
                for (auto& [platform, state] : after.platforms)
                    if (state.localHistory) state.localFields &= ~bit;
        }
    }
    normalizeGroup(after);
}


bool checkExpected(const DefinitionFieldGroups& current,
                   const std::optional<DefinitionFieldGroups>& expected,
                   const QUuid& definition, Diagnostics& diagnostics)
{
    if (!expected || current == *expected) return true;
    addDiagnostic(diagnostics, EditCode::StaleState,
                  QStringLiteral("Field group state changed after this edit was prepared"),
                  definition, {}, {}, QStringLiteral("fieldGroups"));
    return false;
}

void finishCandidate(const EditingContext& context, Guid definition,
                     DefinitionFieldGroups candidate, FieldGroupEditResult& result)
{
    auto definitions = context.view.definitions;
    for (auto& view : definitions)
        if (view.guid == definition) view.groups = &candidate;
    result.diagnostics = validateView(context, definitions);
    if (!result.diagnostics.empty()) return;
    result.changed = candidate != *result.before;
    result.after = std::move(candidate);
}

}

FieldGroupEditResult prepareFieldGroupEdit(
    ProjectData& project, const FieldGroupEditRequest& request)
{
    FieldGroupEditResult result;
    const auto scopes = targetScopes(project, request, result.diagnostics);
    if (const auto* clear = std::get_if<ClearFieldGroupFieldOverride>(&request.operation)) {
        if (request.scopes.shared || FieldGroupForProperty(clear->property) != request.group)
            addDiagnostic(result.diagnostics, EditCode::InvalidOperation,
                          QStringLiteral("Select platforms to reset a field in this field group"),
                          request.definition, request.group, {}, fieldName(clear->property));
    }
    if (FieldGroupName(request.group).empty())
        addDiagnostic(result.diagnostics, EditCode::InvalidOperation,
                      QStringLiteral("Unknown field group"), request.definition, request.group);
    if (const auto* field = std::get_if<SetFieldGroupField>(&request.operation)) {
        if (FieldGroupForProperty(field->property) != request.group ||
            field->property == Property::Sends)
            addDiagnostic(result.diagnostics, EditCode::InvalidOperation,
                          QStringLiteral("Field does not belong to this editable field group"),
                          request.definition, request.group, {}, fieldName(field->property));
    }
    if (std::holds_alternative<ReplaceFieldGroupSends>(request.operation) &&
        request.group != FieldGroupId::Send)
        addDiagnostic(result.diagnostics, EditCode::InvalidOperation,
                      QStringLiteral("A Send replacement requires the Send group"),
                      request.definition, request.group, {}, QStringLiteral("sends"));
    if (!result.diagnostics.empty()) return result;
    const auto context = buildContext(project);
    result.diagnostics = context.diagnostics;
    if (!result.diagnostics.empty()) return result;
    const auto* item = findDefinition(context, request.definition, result.diagnostics);
    if (!item) return result;
    result.before = item->fieldGroups;
    if (!checkExpected(*result.before, request.expectedBefore,
                       request.definition, result.diagnostics)) return result;
    const FieldGroupResolver resolver(context.view.definitions, context.masterBus);
    if (!resolver.ok()) {
        appendGroupDiagnostics(result.diagnostics, context, resolver.diagnostics());
        return result;
    }
    const auto definition = context.identities.at(item);
    const bool canInherit = std::any_of(context.view.definitions.begin(),
        context.view.definitions.end(), [&](const FieldGroupDefinitionView& view) {
            return view.guid == definition && !harkenprojectschema::IsNull(view.parentGuid);
        });
    const auto* mode = std::get_if<SetFieldGroupMode>(&request.operation);
    if (!canInherit && ((mode && mode->mode != FieldGroupMode::UseShared)
        || std::holds_alternative<RebuildLocalFieldGroup>(request.operation))) {
        addDiagnostic(result.diagnostics, EditCode::ReadOnly,
                      QStringLiteral("Inheritance is unavailable without a parent"),
                      request.definition, request.group, {}, QStringLiteral("mode"));
        return result;
    }
    auto candidate = *result.before;
    switch (request.group) {
    case FieldGroupId::MainOutput:
        editGroup(context, resolver, definition, canInherit, request, scopes,
                  result.before->mainOutput, candidate.mainOutput, result.diagnostics);
        break;
    case FieldGroupId::Send:
        editGroup(context, resolver, definition, canInherit, request, scopes,
                  result.before->send, candidate.send, result.diagnostics);
        break;
    case FieldGroupId::SpatialRating:
        editGroup(context, resolver, definition, canInherit, request, scopes,
                  result.before->spatialRating, candidate.spatialRating, result.diagnostics);
        break;
    }
    if (result.diagnostics.empty())
        finishCandidate(context, definition, std::move(candidate), result);
    return result;
}

FieldGroupEditResult prepareFieldGroupRestore(
    ProjectData& project, const QUuid& definition,
    const DefinitionFieldGroups& replacement,
    const std::optional<DefinitionFieldGroups>& expectedBefore)
{
    FieldGroupEditResult result;
    const auto context = buildContext(project);
    result.diagnostics = context.diagnostics;
    if (!result.diagnostics.empty()) return result;
    const auto* item = findDefinition(context, definition, result.diagnostics);
    if (!item) return result;
    result.before = item->fieldGroups;
    if (!checkExpected(*result.before, expectedBefore, definition, result.diagnostics))
        return result;
    finishCandidate(context, context.identities.at(item), replacement, result);
    return result;
}

FieldGroupProjectValidation validateFieldGroupProject(
    ProjectData& project, std::span<const FieldGroupDefinitionSnapshot> replacements,
    std::span<const FieldGroupParentSnapshot> parents,
    std::span<const FieldGroupCopySnapshot> copies)
{
    const auto context = buildContext(project, replacements, parents);
    if (!context.diagnostics.empty()) return {context.diagnostics};
    if (copies.empty()) return {validateView(context, context.view.definitions)};
    auto definitions = context.view.definitions;
    Diagnostics diagnostics;
    for (const auto& copy : copies) {
        const auto collect = [&](auto&& self, const ProjectItem& item, Guid parent) -> void {
            validateLegacyGroupState(item, diagnostics);
            auto definition = fieldGroupDefinitionView(item);
            definition.guid = toFieldGroupGuid(QUuid::createUuid());
            definition.parentGuid = parent;
            if (&item == copy.root && copy.rootGroups)
                definition.groups = copy.rootGroups;
            const auto validatePlatforms = [&](const auto& group) {
                for (const auto& [platform, state] : group.platforms)
                    if (std::find(context.platforms.begin(), context.platforms.end(), platform)
                        == context.platforms.end())
                        addDiagnostic(diagnostics, EditCode::InvalidScopes,
                            QStringLiteral("Copied field group platform does not belong to the project"),
                            item.guid);
            };
            validatePlatforms(definition.groups->mainOutput);
            validatePlatforms(definition.groups->send);
            validatePlatforms(definition.groups->spatialRating);
            definitions.push_back(definition);
            for (const auto& child : item.children)
                self(self, *child, definition.guid);
        };
        collect(collect, *copy.root, toFieldGroupGuid(copy.parent));
    }
    if (!diagnostics.empty()) return {diagnostics};
    return {validateView(context, definitions)};
}

}
