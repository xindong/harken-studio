#include "FieldGroupProjectAdapter.h"
#include "AttenuationStore.h"
#include "ActionManager.h"

#include "Components/VSpatial.h"
#include "Components/VAttenuation.h"
#include "Components/VTrack.h"
#include "PlayablePropertyAuthoringV1.h"
#include "ProjectManager.h"
#include "NameValidator.h"
#include "RuntimeBridge/ProjectSyncBridge.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>

namespace VNX {

namespace {

QString trimmedName(const QString& name)
{
    return name.trimmed();
}

ProjectItem* findItemByObjectID(ProjectData* project, uint32_t objectID)
{
    if (!project || objectID == 0)
        return nullptr;

    for (auto* binding : project->allFileBindings())
    {
        if (!binding || !binding->rootItem)
            continue;

        std::vector<ProjectItem*> stack;
        stack.push_back(binding->rootItem.get());
        while (!stack.empty())
        {
            ProjectItem* item = stack.back();
            stack.pop_back();
            if (item && item->objectID == objectID)
                return item;
            if (!item)
                continue;
            for (auto& child : item->children)
            {
                if (child)
                    stack.push_back(child.get());
            }
        }
    }
    return nullptr;
}

VAttenuation* attenuationComponent(ProjectItem* item)
{
    if (!item || item->type != ProjectItem::Type::Attenuation)
        return nullptr;
    return item->comp<VAttenuation>();
}

ProjectFileBinding* findBindingByGuid(ProjectData& project, const QUuid& guid)
{
    if (guid.isNull())
        return nullptr;

    for (auto& binding : project.attenuationItems)
    {
        if (binding.rootItem && binding.rootItem->guid == guid)
            return &binding;
    }
    return nullptr;
}

ProjectFileBinding* findBindingByName(ProjectData& project, const QString& name)
{
    const auto* item = project.findItemBySelector(name, ProjectItem::Type::Attenuation);
    return item ? findBindingByGuid(project, item->guid) : nullptr;
}

ProjectItem* createItem(ProjectData& project, const AttenuationSnapshot& def)
{
    const QString name = def.name;

    auto item = std::make_shared<ProjectItem>(name, ProjectItem::Type::Attenuation);
    item->attachDefaultComponents();
    item->guid = def.guid;
    item->objectID = project.generateObjectID();
    item->objectPath = name;
    item->parent = nullptr;
    if (!item->comp<VAttenuation>()->fromJson(def.toJson()))
        return nullptr;
    item->markDirty();

    ProjectFileBinding binding;
    binding.parentFolder = QStringLiteral("Attenuations");
    project.setObjectFileLocation(item.get(), nullptr, {});
    binding.rootItem = std::move(item);
    project.attenuationItems.push_back(std::move(binding));
    return project.attenuationItems.back().rootItem.get();
}

}

QString AttenuationStore::guidString(const QUuid& guid)
{
    return guid.isNull() ? QString() : guid.toString(QUuid::WithoutBraces);
}

void AttenuationStore::ensureGuid(AttenuationSnapshot& def)
{
    if (def.guid.isNull())
        def.guid = QUuid::createUuid();
}

VAttenuation* AttenuationStore::find(ProjectData* project, const QString& name)
{
    if (!project || trimmedName(name).isEmpty())
        return nullptr;

    if (auto* binding = findBindingByName(*project, name))
    {
        if (auto* comp = attenuationComponent(binding->rootItem.get()))
            return comp;
    }
    return nullptr;
}

const VAttenuation* AttenuationStore::find(const ProjectData* project, const QString& name)
{
    return find(const_cast<ProjectData*>(project), name);
}

VAttenuation* AttenuationStore::findByGuid(ProjectData* project, const QUuid& guid)
{
    if (!project || guid.isNull())
        return nullptr;

    if (auto* binding = findBindingByGuid(*project, guid))
    {
        if (auto* comp = attenuationComponent(binding->rootItem.get()))
            return comp;
    }
    return nullptr;
}

const VAttenuation* AttenuationStore::findByGuid(const ProjectData* project, const QUuid& guid)
{
    return findByGuid(const_cast<ProjectData*>(project), guid);
}

ProjectItem* AttenuationStore::findItemByStableId(ProjectData* project, harkenstudio::ObjectId stableId)
{
    if (!project || harkenstudio::IsNull(stableId))
        return nullptr;
    for (auto& binding : project->attenuationItems)
    {
        auto* item = binding.rootItem.get();
        if (item && item->type == ProjectItem::Type::Attenuation
            && item->runtimeId() == stableId)
            return item;
    }
    return nullptr;
}

const ProjectItem* AttenuationStore::findItemByStableId(const ProjectData* project, harkenstudio::ObjectId stableId)
{
    return findItemByStableId(const_cast<ProjectData*>(project), stableId);
}

VAttenuation* AttenuationStore::findByStableId(ProjectData* project, harkenstudio::ObjectId stableId)
{
    auto* comp = attenuationComponent(findItemByStableId(project, stableId));
    return comp;
}

const VAttenuation* AttenuationStore::findByStableId(const ProjectData* project, harkenstudio::ObjectId stableId)
{
    return findByStableId(const_cast<ProjectData*>(project), stableId);
}

VAttenuation* AttenuationStore::find(ProjectData* project, const QUuid& guid, const QString& name)
{
    if (!guid.isNull()) return findByGuid(project, guid);
    return find(project, name);
}

const VAttenuation* AttenuationStore::find(const ProjectData* project, const QUuid& guid, const QString& name)
{
    return find(const_cast<ProjectData*>(project), guid, name);
}

bool AttenuationStore::exists(const ProjectData* project, const QString& name)
{
    if (!project) return false;
    return std::any_of(project->attenuationItems.begin(), project->attenuationItems.end(),
        [&](const auto& binding) {
            return binding.rootItem && binding.rootItem->name.compare(name.normalized(QString::NormalizationForm_C), Qt::CaseInsensitive) == 0;
        });
}

std::vector<const VAttenuation*> AttenuationStore::all(const ProjectData& project)
{
    std::vector<const VAttenuation*> out;
    out.reserve(project.attenuationItems.size());
    for (const auto& binding : project.attenuationItems)
    {
        const ProjectItem* item = binding.rootItem.get();
        if (!item || item->type != ProjectItem::Type::Attenuation)
            continue;
        if (const auto* comp = item->comp<VAttenuation>())
            out.push_back(comp);
    }
    return out;
}

harkenstudio::ObjectId AttenuationStore::runtimeId(const ProjectData& project, const VAttenuation& attenuation)
{
    for (const auto& binding : project.attenuationItems)
    {
        const auto* item = binding.rootItem.get();
        if (item && item->type == ProjectItem::Type::Attenuation
            && item == attenuation.hostItem())
            return item->runtimeId();
    }
    return {};
}

bool AttenuationStore::syncToRuntime(const ProjectData& project, const VAttenuation& def)
{
    return syncToRuntime(project, def.snapshot());
}

bool AttenuationStore::syncToRuntime(const ProjectData& project, const AttenuationSnapshot& def)
{
    const auto* authored = findByGuid(&project, def.guid);
    const harkenstudio::ObjectId id = authored ? runtimeId(project, *authored) : harkenstudio::ObjectId{};
    if (harkenstudio::IsNull(id))
        return false;

    HarkenEditor::Bridge::PackAttenuation att;
    att.id = id;
    att.maxDistance = def.maxDistance;
    for (const auto& dimension : def.dimensions)
    {
        if (dimension.curve.isEmpty() || dimension.type != AttenuationDimension::Volume)
            continue;
        HarkenEditor::Bridge::PackAttenuation::Slot slot;
        slot.slot = static_cast<uint8_t>(dimension.type);
        slot.points.reserve(static_cast<size_t>(dimension.curve.size()));
        for (const auto& p : dimension.curve)
            slot.points.push_back({p.x, p.value});
        slot.handles.reserve(static_cast<size_t>(dimension.handles.size()));
        for (const auto& h : dimension.handles)
            slot.handles.push_back({h.h1x, h.h1y, h.h2x, h.h2y});
        att.curveSlots.push_back(std::move(slot));
    }
    return HarkenEditor::Bridge::syncAttenuation(att);
}

VAttenuation* AttenuationStore::create(ProjectData* project,
                                                         const AttenuationSnapshot& input,
                                                         ProjectManager* projectManager)
{
    if (!project || trimmedName(input.name).isEmpty())
        return nullptr;

    AttenuationSnapshot def = input;
    def.name = def.name.normalized(QString::NormalizationForm_C);
    if (!NameValidator::validateItemName(def.name, ProjectItem::Type::Attenuation)) return nullptr;
    ensureGuid(def);

    if (!findBindingByGuid(*project, def.guid)) {
        def.name = NameValidator::makeUniqueName(def.name, [&](const QString& candidate) {
            return std::none_of(project->attenuationItems.begin(), project->attenuationItems.end(),
                [&](const auto& binding) {
                    return binding.rootItem && !project->objectFolder(binding.rootItem.get())
                        && binding.rootItem->name.compare(candidate, Qt::CaseInsensitive) == 0;
                });
        });
    }
    return upsertJson(project, def.name, def.toJson(), projectManager);
}

VAttenuation* AttenuationStore::upsertJson(ProjectData* project, const QString& name,
                                         const QJsonObject& json,
                                         ProjectManager* projectManager)
{
    if (!project) return nullptr;
    AttenuationSnapshot def = AttenuationSnapshot::fromJson(json);
    def.name = name.normalized(QString::NormalizationForm_C);
    if (!NameValidator::validateItemName(def.name, ProjectItem::Type::Attenuation)) return nullptr;
    ensureGuid(def);

    if (auto* existing = findBindingByGuid(*project, def.guid))
    {
        ProjectItem* item = existing->rootItem.get();
        auto* comp = item->compOrCreate<VAttenuation>();
        if (!comp)
            return nullptr;
        if (!comp->fromJson(def.toJson()))
            return nullptr;
        if (item->name != def.name)
        {
            item->name = def.name;
            item->objectPath = def.name;
            item->markDirty();
        }
        item->markDirty();
        syncToRuntime(*project, *comp);
        return comp;
    }

    ProjectItem* item = createItem(*project, def);
    if (!item)
        return nullptr;
    if (projectManager)
        emit projectManager->itemCreated(item);

    auto* comp = attenuationComponent(item);
    if (!comp)
        return nullptr;
    syncToRuntime(*project, *comp);
    return comp;
}

bool AttenuationStore::eraseByName(ProjectData* project, const QString& name,
                                           ProjectManager* projectManager)
{
    return erase(project, {}, name, projectManager);
}

bool AttenuationStore::erase(ProjectData* project,
                                           const QUuid& guid,
                                           const QString& name,
                                           ProjectManager* projectManager)
{
    if (!project)
        return false;

    const auto* target = guid.isNull() ? findBindingByName(*project, name) : findBindingByGuid(*project, guid);
    if (!target) return false;
    const QUuid targetGuid = target->rootItem->guid;
    auto& items = project->attenuationItems;
    for (auto it = items.begin(); it != items.end(); ++it)
    {
        ProjectItem* item = it->rootItem.get();
        if (!item)
            continue;
        if (item->guid != targetGuid)
            continue;

        if (projectManager)
            emit projectManager->itemDeleted(item);
        items.erase(it);
        return true;
    }
    return false;
}

std::vector<AttenuationReference> AttenuationStore::references(
    ProjectData* project, const ProjectItem& attenuation, QString* error)
{
    std::vector<AttenuationReference> out;
    if (error) error->clear();
    const auto fail = [&](const QString& message) {
        if (error) *error = message;
    };
    if (!project) {
        fail(QStringLiteral("No project is available for Attenuation reference inspection."));
        return out;
    }
    const auto attenuationGuid = toFieldGroupGuid(attenuation.guid);
    if (harkenprojectschema::IsNull(attenuationGuid)) {
        fail(QStringLiteral("Attenuation reference inspection requires a persistent GUID."));
        return out;
    }
    const auto view = buildFieldGroupProjectView(*project);
    if (!view.ok()) {
        fail(view.diagnostics.front().message);
        return out;
    }
    const auto* master = project->busses.empty() ? nullptr : project->busses.front().rootItem.get();
    const FieldGroupResolver resolver(view.definitions,
        master ? toFieldGroupGuid(master->guid) : harkenprojectschema::AuthoringGuid{});
    if (!resolver.ok()) {
        const auto& diagnostic = resolver.diagnostics().front();
        fail(QStringLiteral("Cannot inspect Attenuation references: invalid field group state (%1, %2).")
                 .arg(static_cast<unsigned>(diagnostic.code))
                 .arg(QString::fromUtf8(diagnostic.field.data(),
                                       static_cast<qsizetype>(diagnostic.field.size()))));
        return out;
    }
    std::map<harkenprojectschema::AuthoringGuid, bool> roots;
    for (const auto& definition : view.definitions)
        roots.emplace(definition.guid, harkenprojectschema::IsNull(definition.parentGuid));
    const auto references = [attenuationGuid](
        const GroupState<SpatialRatingFieldGroup>& state, bool root) {
        return (state.mode == FieldGroupMode::Local
                || (root && state.mode == FieldGroupMode::Inherit))
            && state.localHistory
            && state.localHistory->spatialAttenuationRef == attenuationGuid;
    };
    std::set<ProjectItem*> visited;
    for (auto* binding : project->allFileBindings())
    {
        if (!binding || !binding->rootItem)
            continue;

        std::vector<ProjectItem*> stack;
        stack.push_back(binding->rootItem.get());
        while (!stack.empty())
        {
            ProjectItem* item = stack.back();
            stack.pop_back();
            if (!item || !visited.insert(item).second)
                continue;

            const auto root = roots.find(toFieldGroupGuid(item->guid));
            if (root != roots.end()) {
                bool referenced = references(item->fieldGroups.spatialRating.shared, root->second);
                for (const auto& platform : project->platforms)
                    referenced = referenced || references(ResolveFieldGroupLocal(
                        item->fieldGroups.spatialRating, toFieldGroupGuid(platform.id)), root->second);
                if (referenced) {
                    out.push_back(AttenuationReference{
                        item, binding->parentFolder, attenuation.name, attenuation.guid
                    });
                }
            }

            for (auto& child : item->children)
            {
                if (child)
                    stack.push_back(child.get());
            }
        }
    }
    return out;
}

QJsonArray AttenuationStore::referencesJson(
    ProjectData* project, const ProjectItem& attenuation, QString* error)
{
    QJsonArray refs;
    for (const auto& reference : references(project, attenuation, error))
    {
        const auto* item = reference.item;
        refs.append(QJsonObject{
            {"object_id", static_cast<qint64>(item->objectID)},
            {"name", item->name},
            {"object_path", item->objectPath},
            {"binding", reference.binding},
            {"attenuation", reference.attenuationName},
            {"attenuation_guid", guidString(reference.attenuationGuid)}
        });
    }
    return refs;
}

int AttenuationStore::referenceCount(
    ProjectData* project, const ProjectItem& attenuation, QString* error)
{
    QString queryError;
    const auto refs = references(project, attenuation, &queryError);
    if (error) *error = queryError;
    return queryError.isEmpty() ? static_cast<int>(refs.size()) : -1;
}

bool AttenuationStore::rename(ProjectData* project,
                                      const QString& oldName,
                                      const QString& newName,
                                      ProjectManager* projectManager)
{
    if (!project || trimmedName(newName).isEmpty())
        return false;

    ProjectFileBinding* binding = findBindingByName(*project, oldName);
    if (!binding)
        return false;
    ProjectItem* item = binding->rootItem.get();
    auto* comp = item->compOrCreate<VAttenuation>();
    if (!comp)
        return false;

    const QString displayName = newName.normalized(QString::NormalizationForm_C);
    if (!NameValidator::validateItemName(displayName, ProjectItem::Type::Attenuation)) return false;
    if (item->name != displayName)
    {
        const QString previous = item->name;
        item->name = displayName;
        item->objectPath = displayName;
        item->markDirty();
        if (projectManager)
            emit projectManager->itemRenamed(item, previous);
    }

    notifyReferencesChanged(project, item->guid, projectManager, {QStringLiteral("attenuationRef")});
    return true;
}

void AttenuationStore::notifyReferencesChanged(ProjectData* project,
                                                       const QUuid& guid,
                                                       ProjectManager* projectManager,
                                                       const QStringList& propertyNames,
                                                       const QObject* originator)
{
    if (!project)
        return;

    const auto* attenuation = findByGuid(project, guid);
    if (!attenuation) return;
    auto* item = attenuation->hostItem();
    item->markDirty();
    syncToRuntime(*project, *attenuation);
    if (projectManager)
        for (const auto& propertyName : propertyNames)
            emit projectManager->itemPropertyChanged(item, propertyName, originator);

    QString error;
    const auto refs = references(project, *item, &error);
    if (!error.isEmpty()) {
        if (projectManager) emit projectManager->errorOccurred(error);
        return;
    }
    for (const auto& ref : refs)
    {
        ProjectItem* item = ref.item;
        if (!item)
            continue;
        ProjectManager::resyncItemToRuntime(item);
        if (projectManager)
            for (const auto& propertyName : propertyNames)
                emit projectManager->itemPropertyChanged(item, propertyName);
    }
}

bool AttenuationStore::applyBinding(ProjectManager* projectManager,
                                            uint32_t objectID,
                                            const PlayablePropertyStateV1& state)
{
    auto* project = projectManager && projectManager->isProjectOpen()
        ? projectManager->currentProject()
        : ProjectManager::instance().currentProject();
    auto* item = findItemByObjectID(project, objectID);
    if (!item)
        return false;

    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(*item);
    constexpr auto propertyId = static_cast<uint8_t>(
        harkenplayableproperties::PlayablePropertyIdV1::SpatialAttenuationRef);
    if (!locator || !PlayablePropertyAuthoringV1::restoreProperty(
            *item, *locator, propertyId, state, std::nullopt))
        return false;
    item->markDirty();
    ProjectManager::resyncItemToRuntime(item);
    if (projectManager)
        emit projectManager->itemPropertyChanged(item, QStringLiteral("attenuationRef"));
    return true;
}

QJsonObject AttenuationStore::summaryJson(const VAttenuation& def)
{
    return QJsonObject{
        {"guid", guidString(def.hostItem()->guid)},
        {"name", def.hostItem()->name},
        {"max_distance", static_cast<double>(def.maxDistance)}
    };
}

ActionResult AttenuationStore::edit(ProjectManager* projectManager, VAttenuation& attenuation,
                                   const QJsonObject& fields, const QString& undoText,
                                   const QObject* originator)
{
    if (!projectManager || !attenuation.hostItem())
        return {false, {}, "INVALID_PROJECT", "Attenuation has no project owner"};
    const QJsonObject before = attenuation.toJson();
    VAttenuation candidate;
    if (!candidate.fromJson(before))
        return {false, {}, "INVALID_PROJECT", "Invalid stored Attenuation fields"};
    for (auto field = fields.begin(); field != fields.end(); ++field)
        if (!candidate.setField(field.key(), field.value().toVariant()))
            return {false, {}, "INVALID_PARAM", "Invalid Attenuation field: " + field.key()};
    const QJsonObject after = candidate.toJson();
    const QUuid guid = attenuation.hostItem()->guid;
    ActionResult result;
    result.success = true;
    if (before != after) {
        attenuation.fromJson(after);
        QStringList changedFields;
        for (auto field = fields.begin(); field != fields.end(); ++field)
            if (before.value(field.key()) != after.value(field.key()))
                changedFields.append(field.key());
        notifyReferencesChanged(projectManager->currentProject(), guid, projectManager,
                                changedFields, originator);
        const auto restore = [projectManager, guid, changedFields](const QJsonObject& value) {
            auto* project = projectManager->currentProject();
            auto* component = findByGuid(project, guid);
            if (!component || !component->fromJson(value)) return;
            notifyReferencesChanged(project, guid, projectManager, changedFields);
        };
        result.undoFn = [restore, before] { restore(before); };
        result.redoFn = [restore, after] { restore(after); };
        result.undoText = undoText;
    }
    result.data = QJsonObject{{"definition", attenuation.snapshot().toJson()}};
    return result;
}

}
