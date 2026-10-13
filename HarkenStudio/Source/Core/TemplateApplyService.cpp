#include "FieldGroupEditing.h"
#include "FieldGroupDerivedView.h"
#include "FieldGroupProjectAdapter.h"

#include "TemplateApplyService.h"
#include "AuthoringPolicy.h"
#include "AttenuationStore.h"
#include "BusPlacementRules.h"
#include "ItemTypeCatalog.h"
#include "ProjectSerializer.h"
#include "SoundSourceSemantics.h"

#include "Components/VAction.h"
#include "Components/VAttenuation.h"
#include "Components/VBus.h"
#include "Components/VMixNode.h"
#include "Components/VSelection.h"
#include "Components/VSound.h"
#include "Components/VSpatial.h"
#include "Components/VTrack.h"
#include "Components/VPiece.h"
#include "Components/VVariable.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QUuid>

#include <algorithm>
#include <functional>
#include <memory>
#include <utility>


namespace VNX {

namespace {

QString seedObjectPath(const ProjectItem* parent, const QString& name)
{
    return parent ? parent->objectPath + QStringLiteral("\\") + name
                  : QStringLiteral("\\") + name;
}

struct TemplateIdentityMap
{
    QHash<uint32_t, uint32_t> objects;
    QHash<harkenstudio::ObjectId, harkenstudio::ObjectId> stableIds;
    QHash<QUuid, QUuid> guids;
};

QString importedObjectPath(const ProjectItem* parent, const QString& name)
{
    return parent ? parent->objectPath + QStringLiteral("\\") + name
                  : QStringLiteral("\\") + name;
}

void remintTree(ProjectData& project, ProjectItem* item, ProjectItem* parent,
                TemplateIdentityMap& identities)
{
    if (!item) return;
    const uint32_t oldObjectId = item->objectID;
    const harkenstudio::ObjectId oldStableId = item->runtimeId();
    const QUuid oldGuid = item->guid;
    item->objectID = project.generateObjectID();
    item->guid = QUuid::createUuid();
    item->name = item->name.normalized(QString::NormalizationForm_C);
    item->parent = parent;
    item->parentRef = ParentRef{};
    if (parent) {
        item->parentRef.kind = ParentRef::Kind::Item;
        item->parentRef.guid = parent->guid;
    }
    item->objectPath = importedObjectPath(parent, item->name);
    if (oldObjectId != 0) identities.objects.insert(oldObjectId, item->objectID);
    if (!harkenstudio::IsNull(oldStableId)) identities.stableIds.insert(oldStableId, item->runtimeId());
    if (!oldGuid.isNull()) identities.guids.insert(oldGuid, item->guid);
    for (auto& child : item->children)
        remintTree(project, child.get(), item, identities);
}

void remapAttenuationReference(PlayablePropertyOccurrenceV1& properties,
                              const TemplateIdentityMap& identities)
{
    constexpr auto propertyId =
        harkenplayableproperties::PlayablePropertyIdV1::SpatialAttenuationRef;
    if (!properties.presence.Has(propertyId))
        return;
    const auto* value = properties.value(propertyId);
    const auto* reference = value ? std::get_if<harkenstudio::ObjectId>(value) : nullptr;
    if (reference)
        properties.setValue(propertyId, identities.stableIds.value(*reference, *reference));
}

void remapAttenuationReferences(PlatformOverrideLedgerV1& ledger,
                               const TemplateIdentityMap& identities)
{
    for (auto& row : ledger.rows)
        remapAttenuationReference(row.properties, identities);
}

void remapTree(ProjectItem* item, const TemplateIdentityMap& identities)
{
    if (!item) return;
    if (auto* action = item->comp<VAction>()) {
        for (auto& step : action->steps) {
            step.targetObjectID = identities.objects.value(
                step.targetObjectID, step.targetObjectID);
            step.quantize.referenceObjectID = identities.objects.value(
                step.quantize.referenceObjectID, step.quantize.referenceObjectID);
        }
    }
    if (auto* mix = item->comp<VMixNode>()) {
        if (item->type == ProjectItem::Type::Bus) {
            for (auto& send : mix->sends)
                send.targetBusID = identities.objects.value(send.targetBusID, send.targetBusID);
        }
        std::vector<ParameterBindingDef> bindings = mix->bindings.entries();
        for (auto& binding : bindings) {
            binding.variable = identities.guids.value(binding.variable, binding.variable);
        }
        mix->bindings.replace(std::move(bindings));
    }
    const auto remapGuid = [&](harkenprojectschema::AuthoringGuid& guid) {
        const auto original = fromFieldGroupGuid(guid);
        guid = toFieldGroupGuid(identities.guids.value(original, original));
    };
    const auto remapHistory = [&](auto& group, const auto& remap) {
        if (group.shared.localHistory) remap(*group.shared.localHistory);
        for (auto& [platform, state] : group.platforms)
            if (state.localHistory) remap(*state.localHistory);
    };
    remapHistory(item->fieldGroups.mainOutput, [&](MainOutputFieldGroup& value) {
        remapGuid(value.mainOutputBusRef);
    });
    remapHistory(item->fieldGroups.send, [&](SendFieldGroup& value) {
        for (auto& row : value.sends) remapGuid(row.targetBusID);
    });
    remapHistory(item->fieldGroups.spatialRating, [&](SpatialRatingFieldGroup& value) {
        remapGuid(value.spatialAttenuationRef);
    });
    remapAttenuationReferences(item->platformOverrides, identities);
    if (auto* selection = item->comp<VSelection>()) {
        QHash<QUuid, QUuid> rowGuids;
        for (auto* table : selection->selections.tables()) {
            for (auto& axis : table->matchAxes) axis = identities.guids.value(axis, axis);
            for (auto& row : table->mixingRows) {
                if (!rowGuids.contains(row.guid)) rowGuids.insert(row.guid, QUuid::createUuid());
                row.guid = rowGuids.value(row.guid);
                row.variable = identities.guids.value(row.variable, row.variable);
            }
            for (auto& entry : table->entries) {
                entry.childGuid = identities.guids.value(entry.childGuid, entry.childGuid);
                for (auto& curve : entry.curves) curve.rowGuid = rowGuids.value(curve.rowGuid);
            }
        }
        for (auto& rule : selection->transitions.rules)
            for (auto* reference : {&rule.source, &rule.target, &rule.bridge})
                if (*reference)
                    *reference = identities.guids.value(**reference, **reference);
    }
    if (auto* sound = item->comp<VSound>()) SoundSourceSemantics::remintEntries(*sound);
    if (auto* timeline = item->comp<VTrack>())
        for (auto& clip : timeline->clips.items) {
            clip.clipGuid = QUuid::createUuid();
            clip.targetGuid = identities.guids.value(clip.targetGuid, clip.targetGuid);
            remapAttenuationReference(clip.playableProperties, identities);
            remapAttenuationReferences(clip.platformOverrides, identities);
        }
    if (auto* timeline = item->comp<VPiece>())
        for (auto& marker : timeline->markers.items) {
            marker.markerGuid = QUuid::createUuid();
        }
    for (auto& child : item->children) remapTree(child.get(), identities);
}

void remintSubentityStableIds(ProjectData& project, ProjectItem* item)
{
    if (!item) return;
    if (auto* mix = item->comp<VMixNode>()) {
        std::vector<ParameterBindingDef> bindings = mix->bindings.entries();
        for (auto& binding : bindings)
            if (binding.typed) binding.guid = QUuid::createUuid();
        mix->bindings.replace(std::move(bindings));
        std::vector<EffectEntry> effects = mix->effects.entries();
        for (auto& effect : effects)
            if (effect.typed) effect.guid = QUuid::createUuid();
        mix->effects.replace(std::move(effects));
    }
    if (auto* timeline = item->comp<VTrack>())
        for (auto& clip : timeline->clips.items)
            clip.clipGuid = QUuid::createUuid();
    if (auto* timeline = item->comp<VPiece>())
        for (auto& marker : timeline->markers.items)
            marker.markerGuid = QUuid::createUuid();
    for (auto& child : item->children)
        remintSubentityStableIds(project, child.get());
}

}

bool TemplateApplyService::seedBusBranch(ProjectData& project,
                                         ProjectItem* parentBus,
                                         const QJsonObject& nodeJson,
                                         ProjectSerializer& serializer,
                                         QString* error)
{
    if (!parentBus) {
        if (error) *error = QStringLiteral("seedBusBranch called with null parent");
        return false;
    }
    const QString name = nodeJson.value("name").toString();
    if (name.isEmpty()) {
        if (error) *error = QStringLiteral("bus child missing 'name'");
        return false;
    }
    QString reason;
    if (!validateBusHierarchyPlacement(
            parentBus, project.busses[0].rootItem.get(), &reason)) {
        if (error) *error = reason;
        return false;
    }

    auto childItem = std::make_shared<ProjectItem>(name, ProjectItem::Type::Bus);
    QJsonObject authored = nodeJson;
    authored.remove(QStringLiteral("children"));
    if (authored.contains(QStringLiteral("type"))) {
        if (!serializer.projectItemFromJson(authored, *childItem)
            || childItem->type != ProjectItem::Type::Bus) {
            if (error) *error = QStringLiteral("invalid authored bus: %1").arg(name);
            return false;
        }
    } else {
        childItem = std::make_shared<ProjectItem>(name, ProjectItem::Type::Bus);
        childItem->attachDefaultComponents();
    }
    childItem->name = name;
    childItem->parent     = parentBus;
    childItem->objectPath = seedObjectPath(parentBus, name);

    parentBus->children.push_back(std::move(childItem));
    ProjectItem* childRaw = parentBus->children.back().get();

    for (const auto& c : nodeJson.value("children").toArray()) {
        if (!c.isObject()) continue;
        if (!seedBusBranch(project, childRaw, c.toObject(), serializer, error))
            return false;
    }
    return true;
}

namespace {

QVector<CurvePointDef> parseAttenuationCurvePoints(const QJsonArray& pointsJson)
{
    QVector<CurvePointDef> out;
    out.reserve(pointsJson.size());
    for (const auto& v : pointsJson) {
        CurvePointDef p;
        if (v.isArray()) {
            const auto arr = v.toArray();
            if (arr.size() < 2) continue;
            p.x = static_cast<float>(arr.at(0).toDouble());
            p.value    = static_cast<float>(arr.at(1).toDouble());
        } else if (v.isObject()) {
            const auto obj = v.toObject();
            p.x = static_cast<float>(obj.value("distance").toDouble(
                obj.value("x").toDouble(0.0)));
            p.value    = static_cast<float>(obj.value("value").toDouble(
                obj.value("y").toDouble(0.0)));
        } else {
            continue;
        }
        out.push_back(p);
    }
    std::sort(out.begin(), out.end(),
              [](const CurvePointDef& a, const CurvePointDef& b) {
                  return a.x < b.x;
              });
    return out;
}

void seedAttenuationCurveFromJson(VAttenuation& attenuation,
                                  const QJsonObject& jsonObj,
                                  const QString& slotName)
{
    if (jsonObj.isEmpty()) return;
    AttenuationDimension type = AttenuationDimension::Volume;
    if (!attenuationDimensionFromName(slotName, &type))
        return;
    auto candidate = attenuation.snapshot();
    auto& dimension = candidate.ensureDimension(type);
    dimension.curve = parseAttenuationCurvePoints(jsonObj.value("points").toArray());
    dimension.handles.clear();
    attenuationNormalizeCurve(type, dimension.curve, dimension.handles);
    attenuation.dimensions = std::move(candidate.dimensions);
}

}

bool TemplateApplyService::seedTemplateInto(
    ProjectData& project,
    const QJsonObject& templateData,
    QString* error)
{
    ValidationResult validation;
    const auto parsed = ProjectTemplateLoader::parse(templateData, &validation);
    if (!parsed) {
        if (error) *error = validation.toUserText();
        return false;
    }
    return seedTemplateInto(project, *parsed, error);
}

bool TemplateApplyService::seedTemplateInto(
    ProjectData& project,
    const ProjectTemplateData& templateData,
    QString* error)
{
    auto setErr = [error](const QString& msg) -> bool {
        if (error) *error = msg;
        return false;
    };

    const auto validation = ProjectTemplateLoader::validate(QJsonDocument(templateData.toJson()));
    if (!validation.valid)
        return setErr(validation.toUserText());

    ProjectSerializer serializer;
    QJsonArray referenceRoots;
    const QJsonObject busRoot = templateData.toJson().value(
        QStringLiteral("bus")).toObject();
    if (busRoot.contains(QStringLiteral("type")))
        referenceRoots.append(busRoot);
    for (const QJsonValue& value : templateData.content)
        referenceRoots.append(value);
    for (const QJsonValue& value : templateData.variables)
        referenceRoots.append(value);
    for (const QJsonValue& value : templateData.actions)
        referenceRoots.append(value);
    for (const TemplateAttenuationDef& attenuation : templateData.attenuations)
        if (!attenuation.authoring.isEmpty())
            referenceRoots.append(attenuation.authoring);
    if (!serializer.prepareStandaloneReferenceRead(referenceRoots))
        return setErr(serializer.lastError());

    struct PendingBinding {
        std::shared_ptr<ProjectItem> item;
        std::vector<ProjectFileBinding>* bucket = nullptr;
        QString folder;
    };
    std::vector<PendingBinding> pending;
    auto parseItems = [&](const QJsonArray& source, ProjectItem::Type expected,
                          std::vector<ProjectFileBinding>* bucket,
                          const QString& folder) -> bool {
        for (const auto& value : source) {
            if (!value.isObject()) return setErr(QStringLiteral("template item must be an object"));
            auto item = std::make_shared<ProjectItem>();
            if (!serializer.projectItemFromJson(value.toObject(), *item))
                return setErr(serializer.lastError().isEmpty()
                    ? QStringLiteral("template item data is invalid")
                    : serializer.lastError());
            if (item->type != expected)
                return setErr(QStringLiteral("template item type mismatch"));
            pending.push_back({std::move(item), bucket, folder});
        }
        return true;
    };
    for (const auto& value : templateData.content) {
        if (!value.isObject()) return setErr(QStringLiteral("template content must be an object"));
        auto item = std::make_shared<ProjectItem>();
        if (!serializer.projectItemFromJson(value.toObject(), *item))
            return setErr(serializer.lastError().isEmpty()
                ? QStringLiteral("template content data is invalid")
                : serializer.lastError());
        if (ItemTypeCatalog::bucketOf(item->type) != ItemTypeCatalog::Bucket::Layers)
            return setErr(QStringLiteral("template content root has invalid type"));
        pending.push_back({std::move(item), &project.layers,
                           QString::fromLatin1(ItemTypeCatalog::kLayersFolderName)});
    }
    if (!parseItems(templateData.variables, ProjectItem::Type::Variable,
                    &project.variables,
                    QString::fromLatin1(ItemTypeCatalog::kVariablesFolderName))
        || !parseItems(templateData.actions, ProjectItem::Type::Action,
                       &project.actions,
                       QString::fromLatin1(ItemTypeCatalog::kActionsFolderName)))
        return false;

    for (const auto& a : templateData.attenuations) {
        const QString name = a.name;
        if (name.isEmpty()) continue;
        if (!a.authoring.isEmpty()) {
            auto item = std::make_shared<ProjectItem>();
            if (!serializer.projectItemFromJson(a.authoring, *item))
                return setErr(serializer.lastError().isEmpty()
                    ? QStringLiteral("template attenuation data is invalid")
                    : serializer.lastError());
            if (item->type != ProjectItem::Type::Attenuation)
                return setErr(QStringLiteral("template attenuation type mismatch"));
            item->name = name;
            auto* attenuation = item->comp<VAttenuation>();
            if (!attenuation)
                return setErr(QStringLiteral("template attenuation component missing"));
            attenuation->maxDistance = static_cast<float>(a.maxDistance);
            QJsonArray points;
            for (const QPointF& point : a.volumePoints)
                points.append(QJsonArray{point.x(), point.y()});
            seedAttenuationCurveFromJson(
                *attenuation,
                QJsonObject{{QStringLiteral("points"), points}},
                QStringLiteral("volume"));
            pending.push_back({std::move(item), &project.attenuationItems,
                               QString::fromLatin1(ItemTypeCatalog::kAttenuationsFolderName)});
            continue;
        }
        auto item = std::make_shared<ProjectItem>(name, ProjectItem::Type::Attenuation);
        item->attachDefaultComponents();
        auto* attenuation = item->comp<VAttenuation>();
        if (!attenuation)
            return setErr(QStringLiteral("failed to create attenuation component"));
        attenuation->maxDistance = static_cast<float>(a.maxDistance);
        QJsonArray points;
        for (const QPointF& point : a.volumePoints)
            points.append(QJsonArray{point.x(), point.y()});
        seedAttenuationCurveFromJson(*attenuation,
                                     QJsonObject{{QStringLiteral("points"), points}},
                                     QStringLiteral("volume"));
        pending.push_back({std::move(item), &project.attenuationItems,
                           QString::fromLatin1(ItemTypeCatalog::kAttenuationsFolderName)});
    }

    QHash<QUuid, ProjectItem*> authoredByGuid;
    QHash<uint32_t, const ProjectItem*> authoredByObject;
    const auto collect = [&](auto&& self, ProjectItem* item) -> void {
        if (!item) return;
        authoredByGuid.insert(item->guid, item);
        authoredByObject.insert(item->objectID, item);
        for (const auto& child : item->children) self(self, child.get());
    };
    for (const auto& value : pending) collect(collect, value.item.get());
    for (const auto* item : authoredByGuid) {
        const auto* variable = item->comp<VVariable>();
        if (!variable) continue;
        QString labelError;
        if (variable->rangeMin < 0 || !VariableSemantics::validateLabels(variable->labels, &labelError))
            return setErr(QStringLiteral("Variable '%1': %2").arg(item->name,
                labelError.isEmpty() ? QStringLiteral("range minimum must be nonnegative") : labelError));
    }
    for (auto* item : authoredByGuid) {
        auto* selection = item->comp<VSelection>();
        if (!selection) continue;
        for (auto* table : selection->selections.tables()) {
            for (size_t axis = 0; axis < table->matchAxes.size(); ++axis) {
                auto* variableItem = authoredByGuid.value(table->matchAxes[axis], nullptr);
                auto* variable = variableItem ? variableItem->comp<VVariable>() : nullptr;
                if (!variable) return setErr(QStringLiteral("Template Grid axis has no Variable"));
                for (auto& entry : table->entries) {
                    for (auto& condition : entry.conditions) {
                        auto& cell = condition.cells[axis];
                        if (!cell) continue;
                        if (!cell->labelId.isNull()) {
                            if (!VariableSemantics::labelById(variable->labels, cell->labelId))
                                return setErr(QStringLiteral("Template Grid Label identity is missing"));
                            continue;
                        }
                        const int index = cell->legacyValue;
                        if (index < 0 || index > VariableSemantics::kStepExactIntBound)
                            return setErr(QStringLiteral("Template Grid Label index is invalid"));
                        const auto* label = VariableSemantics::labelAtIndex(variable->labels, index);
                        if (!label) {
                            variable->labels.push_back({index, {}});
                            label = &variable->labels.back();
                        }
                        cell = DiscreteGridCell(label->id);
                    }
                }
            }
        }
    }
    for (const auto& value : pending) {
        const auto authoring = AuthoringPolicy::validateNewSubtree(
            *value.item, nullptr,
            [&](const QUuid& guid) { return authoredByGuid.value(guid, nullptr); },
            [&](uint32_t objectID) { return authoredByObject.value(objectID, nullptr); });
        if (!authoring) return setErr(authoring.message);
    }

    if (project.busses.empty() || !project.busses[0].rootItem)
        return setErr(QStringLiteral(
            "Master Bus not seeded — call ensureDefaultMasterBus first"));

    ProjectItem* master = project.busses[0].rootItem.get();

    const auto rootJson = templateData.toJson().value("bus").toObject();

    const size_t seededBefore = master->children.size();
    const auto oldObjectId = project.nextObjectID;
    const auto layerCount = project.layers.size();
    const auto actionCount = project.actions.size();
    const auto variableCount = project.variables.size();
    const auto attenuationCount = project.attenuationItems.size();
    const auto rollbackGroups = [&] {
        master->children.resize(seededBefore);
        project.layers.resize(layerCount);
        project.actions.resize(actionCount);
        project.variables.resize(variableCount);
        project.attenuationItems.resize(attenuationCount);
        project.nextObjectID = oldObjectId;
    };
    for (const auto& c : rootJson.value("children").toArray()) {
        if (!c.isObject()) continue;
        if (!seedBusBranch(project, master, c.toObject(), serializer, error)) {
            rollbackGroups();
            return false;
        }
    }

    TemplateIdentityMap identities;
    const QJsonObject masterAuthoring = templateData.bus.authoring;
    const QUuid sourceMasterGuid = QUuid::fromString(
        masterAuthoring.value(QStringLiteral("guid")).toString());
    const uint32_t sourceMasterObjectId =
        serializer.transientObjectIdForGuid(sourceMasterGuid);
    const harkenstudio::ObjectId sourceMasterStableId =
        serializer.transientRuntimeStableIdForGuid(sourceMasterGuid);
    if (sourceMasterObjectId != 0)
        identities.objects.insert(sourceMasterObjectId, master->objectID);
    if (!harkenstudio::IsNull(sourceMasterStableId))
        identities.stableIds.insert(sourceMasterStableId, master->runtimeId());
    if (!sourceMasterGuid.isNull())
        identities.guids.insert(sourceMasterGuid, master->guid);
    for (size_t index = seededBefore; index < master->children.size(); ++index)
        remintTree(project, master->children[index].get(), master, identities);
    for (auto& value : pending)
        remintTree(project, value.item.get(), nullptr, identities);
    for (size_t index = seededBefore; index < master->children.size(); ++index) {
        remapTree(master->children[index].get(), identities);
        remintSubentityStableIds(project, master->children[index].get());
    }
    for (auto& value : pending) {
        remapTree(value.item.get(), identities);
        remintSubentityStableIds(project, value.item.get());
        ProjectFileBinding binding;
        binding.parentFolder = value.folder;
        binding.rootItem = std::move(value.item);
        value.bucket->push_back(std::move(binding));
    }

    const auto groupValidation = validateFieldGroupProject(project);
    if (!groupValidation.ok()) {
        rollbackGroups();
        return setErr(groupValidation.diagnostics.front().message);
    }
    if (!refreshFieldGroupDerivedViews(project)) {
        rollbackGroups();
        return setErr(QStringLiteral("Cannot derive field group references"));
    }
    for (size_t i = actionCount; i < project.actions.size(); ++i) {
        auto& item = *project.actions[i].rootItem;
        item.nameBindingRevision = 1;
        ActionPublishedNameHistoryRow row;
        row.name = item.name;
        row.objectGuid = item.guid;
        row.event = QStringLiteral("created");
        project.actionPublishedNameHistory.push_back(std::move(row));
    }
    return true;
}

}
