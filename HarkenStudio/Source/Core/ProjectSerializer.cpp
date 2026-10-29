
#include "ProjectSerializer.h"
#include "Components/VSpatial.h"
#include "AssetPipeline/PathUtils.h"
#include "AssetPipeline/AssetRegistry.h"
#include "PlayablePropertyAuthoringV1.h"
#include "PlaybackReferenceValidation.h"
#include "ActionStepUtils.h"
#include "ProjectPackExporter.h"
#include "ProjectPreflight/ProjectPreflight.h"
#include "ItemTypeCatalog.h"
#include "MigrationManager.h"
#include "ProjectDocument.h"
#include "ProjectContract.h"
#include "ProjectLocator.h"
#include "FieldGroupJson.h"
#include "FieldGroupEditing.h"
#include "ProjectTransaction.h"
#include "ProjectUpgradeManager.h"
#include "ProjectUpgradeMessages.h"
#include "NameValidator.h"
#include "RuntimeU64Json.h"
#include "Component.h"
#include "ComponentCatalog.h"
#include "ContainerBehaviorSemantics.h"
#include "ProjectLanguageSemantics.h"
#include "Components/VAudio.h"
#include "Components/VMixNode.h"
#include "Components/VAction.h"
#include "Components/VBus.h"
#include "Components/VSelection.h"
#include "Components/VSound.h"
#include "Components/VAttenuation.h"
#include "Widgets/ProjectExplorer/ProjectExplorer.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSaveFile>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QDebug>
#include <QThread>
#include <algorithm>
#include <functional>
#include <limits>

namespace VNX {

namespace {

QString componentTypeName(ComponentType type)
{
    return ComponentCatalog::componentName(type);
}

std::optional<ProjectItem::Type> itemTypeFromJson(const QJsonObject& json)
{
    const QString raw = json.value(QStringLiteral("type")).toString();
    const auto type = ItemTypeCatalog::parseTypeName(
        raw, Qt::CaseSensitive, false);
    if (!type) {
        qWarning() << "[ProjectSerializer] Unknown item type value" << raw
                   << "for item" << json["name"].toString() << "-- rejecting.";
        return std::nullopt;
    }
    return *type;
}

constexpr char kNameBindingRevision[] = "nameBindingRevision";
constexpr char kActionPublishedNameHistory[] = "actionPublishedNameHistory";

void writeActionNameBindingRevision(QJsonObject& json, const ProjectItem& item)
{
    if (item.type != ProjectItem::Type::Action) return;
    json[QLatin1String(kNameBindingRevision)] =
        static_cast<qint64>(item.nameBindingRevision);
}

void readActionNameBindingRevision(const QJsonObject& json, ProjectItem& item)
{
    if (item.type != ProjectItem::Type::Action) return;
    item.nameBindingRevision = 1;
    if (!json.contains(QLatin1String(kNameBindingRevision))) return;
    const qint64 value =
        json.value(QLatin1String(kNameBindingRevision)).toInteger(0);
    if (value >= 1)
        item.nameBindingRevision = static_cast<uint64_t>(value);
}

bool isActionPublishedNameEvent(const QString& event)
{
    return event == QStringLiteral("created")
        || event == QStringLiteral("deleted")
        || event == QStringLiteral("renamed_from")
        || event == QStringLiteral("reuse_approved")
        || event == QStringLiteral("reuse_undone");
}

bool eventRequiresPriorName(const QString& event)
{
    return event == QStringLiteral("renamed_from")
        || event == QStringLiteral("reuse_undone");
}

QJsonObject actionPublishedNameHistoryRowToJson(
    const ActionPublishedNameHistoryRow& row)
{
    QJsonObject obj;
    obj[QStringLiteral("name")] = row.name;
    obj[QStringLiteral("objectGuid")] =
        row.objectGuid.toString(QUuid::WithoutBraces);
    obj[QLatin1String(kNameBindingRevision)] =
        static_cast<qint64>(row.nameBindingRevision);
    obj[QStringLiteral("event")] = row.event;
    if (!row.priorName.isEmpty())
        obj[QStringLiteral("priorName")] = row.priorName;
    return obj;
}

bool actionPublishedNameHistoryRowFromJson(const QJsonObject& obj,
                                           ActionPublishedNameHistoryRow* row)
{
    if (!row) return false;
    ActionPublishedNameHistoryRow parsed;
    parsed.name = obj.value(QStringLiteral("name")).toString();
    if (parsed.name.isEmpty() || !obj.value(QStringLiteral("objectGuid")).isString())
        return false;
    parsed.objectGuid =
        QUuid::fromString(obj.value(QStringLiteral("objectGuid")).toString());
    if (parsed.objectGuid.isNull()) return false;
    const qint64 revision =
        obj.value(QLatin1String(kNameBindingRevision)).toInteger(0);
    if (revision < 1) return false;
    parsed.nameBindingRevision = static_cast<uint64_t>(revision);
    parsed.event = obj.value(QStringLiteral("event")).toString();
    if (!isActionPublishedNameEvent(parsed.event)) return false;
    parsed.priorName = obj.value(QStringLiteral("priorName")).toString();
    if (eventRequiresPriorName(parsed.event)) {
        if (parsed.priorName.isEmpty()) return false;
    } else if (obj.contains(QStringLiteral("priorName"))) {
        return false;
    }
    *row = std::move(parsed);
    return true;
}

struct ComponentFields {
    QStringList definition;
    QStringList fieldGroup;
};

const QHash<QString, ComponentFields>& componentAuthorityFields()
{
    static const QHash<QString, ComponentFields> fieldsByComponent = [] {
        QHash<QString, ComponentFields> result;
        for (const auto& field : PlayablePropertyAuthoringV1::fields()) {
            if (PlayablePropertyAuthoringV1::isComponentStored(field.id)) continue;
            auto& fields = result[QLatin1String(field.component)].definition;
            fields.append(QLatin1String(field.storageField));
            fields.append(QLatin1String(field.jsonKey));
        }
        for (const auto& field : harkenplayableproperties::studio::kJsonFieldsV1) {
            if (!FieldGroupForProperty(
                    static_cast<harkenplayableproperties::PlayablePropertyIdV1>(field.id)))
                continue;
            auto& fields = result[QLatin1String(field.component)].fieldGroup;
            fields.append(QLatin1String(field.storageField));
            fields.append(QLatin1String(field.jsonKey));
            fields.append(QLatin1String(field.fieldName));
        }
        for (const auto& entry : {std::pair{"mixnode", "outputBusId"},
                                  std::pair{"mixnode", "outputBusRef"},
                                  std::pair{"audio", "outputBusOverride"},
                                  std::pair{"audio", "overrideBits"},
                                  std::pair{"spatial", "attenuation"},
                                  std::pair{"spatial", "attenuationGuid"}})
            result[QLatin1String(entry.first)].fieldGroup.append(QLatin1String(entry.second));
        for (auto& fields : result) {
            fields.definition.removeDuplicates();
            fields.fieldGroup.removeDuplicates();
        }
        return result;
    }();
    return fieldsByComponent;
}

const QHash<QString, QSet<QString>>& excludedComponentFields(ProjectItem::Type type)
{
    static const QHash<QString, QSet<QString>> empty;
    if (!PlayablePropertyAuthoringV1::isDefinitionType(type)) return empty;
    static const auto definitionFields = [] {
        const auto& fieldsByComponent = componentAuthorityFields();
        QHash<QString, QSet<QString>> result;
        for (auto fields = fieldsByComponent.cbegin(); fields != fieldsByComponent.cend(); ++fields)
            if (!fields->definition.isEmpty())
                result.insert(fields.key(), QSet<QString>(fields->definition.cbegin(), fields->definition.cend()));
        return result;
    }();
    static const auto groupedFields = [] {
        const auto& fieldsByComponent = componentAuthorityFields();
        auto result = definitionFields;
        for (auto fields = fieldsByComponent.cbegin(); fields != fieldsByComponent.cend(); ++fields)
            if (!fields->fieldGroup.isEmpty())
                result[fields.key()].unite(QSet<QString>(fields->fieldGroup.cbegin(), fields->fieldGroup.cend()));
        return result;
    }();
    return PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(type)
        ? groupedFields : definitionFields;
}

void stripPlayableDefinitionComponentAuthority(ProjectItem::Type type,
                                                QJsonObject& components)
{
    if (!PlayablePropertyAuthoringV1::isDefinitionType(type)) return;
    const auto& fieldsByComponent = componentAuthorityFields();
    const bool grouped = PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(type);
    for (auto fields = fieldsByComponent.cbegin(); fields != fieldsByComponent.cend(); ++fields) {
        const bool stripGrouped = grouped && !fields->fieldGroup.isEmpty();
        if (fields->definition.isEmpty() && !stripGrouped) continue;
        auto value = components.find(fields.key());
        if (value == components.end() || (!value->isObject() && !stripGrouped)) continue;
        auto component = value->toObject();
        for (const auto& field : fields->definition) component.remove(field);
        if (stripGrouped)
            for (const auto& field : fields->fieldGroup) component.remove(field);
        if (component.isEmpty()) components.erase(value);
        else value.value() = component;
    }
}


bool readFieldGroups(const QJsonObject& json, ProjectItem& item, QString& error)
{
    if (!PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item.type)) {
        item.fieldGroups = {};
        return true;
    }
    std::string detail;
    if (ReadFieldGroupsJson(QJsonDocument(json.value(QStringLiteral("fieldGroups")).toObject())
            .toJson(QJsonDocument::Compact).toStdString(), item.fieldGroups, detail))
        return true;
    error = QString::fromStdString(detail);
    return false;
}

bool hasPlayableDefinitionComponentAuthority(ProjectItem::Type type,
                                             const QJsonObject& components)
{
    if (!PlayablePropertyAuthoringV1::isDefinitionType(type)) return false;
    for (const auto& field : VNX::PlayablePropertyAuthoringV1::fields()) {
        if (PlayablePropertyAuthoringV1::isComponentStored(field.id)) continue;
        const QJsonObject component = components.value(
            QLatin1String(field.component)).toObject();
        if (component.contains(QLatin1String(field.storageField))
            || component.contains(QLatin1String(field.jsonKey)))
            return true;
    }
    return false;
}

}

bool ProjectSerializer::writeFieldGroups(const ProjectItem& item, QJsonObject& json) const
{
    if (!PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item.type)) {
        if (item.fieldGroups == DefinitionFieldGroups{}) return true;
        lastError_ = QStringLiteral("This object type cannot author fieldGroups");
        return false;
    }
    if (cachedFieldGroups_ && cachedFieldGroups_->input == item.fieldGroups) {
        json[QStringLiteral("fieldGroups")] = cachedFieldGroups_->json;
        return true;
    }
    std::string text;
    std::string detail;
    if (!WriteFieldGroupsJson(item.fieldGroups, text, detail)) {
        lastError_ = QString::fromStdString(detail);
        return false;
    }
    auto object = QJsonDocument::fromJson(QByteArray::fromStdString(text)).object();
    cachedFieldGroups_ = CachedFieldGroups{item.fieldGroups, object};
    json[QStringLiteral("fieldGroups")] = std::move(object);
    return true;
}

ProjectSerializer::ProjectSerializer(QObject* parent)
    : QObject(parent)
{
}

ProjectSerializer::~ProjectSerializer() = default;

bool ProjectSerializer::registerWriteItem(const ProjectItem& item,
                                          const QUuid& documentId)
{
    if (!NameValidator::validateItemName(item.name, item.type)) {
        lastError_ = QStringLiteral("Project contains an invalid object name: %1").arg(item.name);
        return false;
    }
    if (!registerSdkName(item.name, item.type, item.guid)) return false;
    if (item.guid.isNull() || item.objectID == 0) {
        lastError_ = QStringLiteral("Project contains an object without a valid identity");
        return false;
    }
    if (referencesByGuid_.contains(item.guid)) {
        lastError_ = QStringLiteral("Project contains a duplicate object GUID: %1")
                         .arg(item.guid.toString(QUuid::WithoutBraces));
        return false;
    }
    if (referencesByObjectId_.contains(item.objectID)) {
        lastError_ = QStringLiteral("Project contains a duplicate temporary object handle: %1")
                         .arg(item.objectID);
        return false;
    }
    const ReferenceTarget target{item.objectID, item.type,
                                 item.guid, documentId, item.name};
    referencesByGuid_.insert(item.guid, target);
    referencesByObjectId_.insert(item.objectID, target);
    if (!harkenstudio::IsNull(item.runtimeId())) {
        if (referencesByRuntimeStableId_.contains(item.runtimeId())) {
            lastError_ = QStringLiteral("Project contains a duplicate runtime identity: %1")
                             .arg(objectIdString(item.runtimeId()));
            return false;
        }
        referencesByRuntimeStableId_.insert(item.runtimeId(), target);
    }
    return true;
}

bool ProjectSerializer::prepareReferenceWrite(const ProjectData& project)
{
    lastError_.clear();
    strictReferenceRead_ = false;
    referencesByObjectId_.clear();
    referencesByRuntimeStableId_.clear();
    referencesByGuid_.clear();
    sdkNames_.clear();
    parentOverrides_.clear();
    nextTransientObjectId_ = 1;

    std::function<bool(const ProjectItem*, const QUuid&)> registerTree =
        [&](const ProjectItem* item, const QUuid& documentId) {
            if (!item) return true;
            if (const PhysicalFolder* folder = project.objectFolder(item)) {
                if (!ItemTypeCatalog::isFolderAddressable(item->type)
                    || project.folderBucket(folder) != ItemTypeCatalog::folderNameOf(ItemTypeCatalog::bucketOf(item->type))) {
                    lastError_ = QStringLiteral("Project object '%1' is stored in a folder that is not registered in its bucket")
                                     .arg(item->name);
                    return false;
                }
            }
            if (!registerWriteItem(*item, documentId)) return false;
            for (const auto& child : item->children) {
                const QUuid childDocument = child ? child->guid : documentId;
                if (!registerTree(child.get(), childDocument)) return false;
            }
            return true;
        };

    for (const ProjectFileBinding* binding : project.allFileBindings()) {
        if (!binding || !binding->rootItem) continue;
        if (!registerTree(binding->rootItem.get(), binding->rootItem->guid))
            return false;
        ProjectItem& root = *binding->rootItem;
        ParentRef parent = root.parentRef;
        if (root.type == ProjectItem::Type::Bus) {
            parent.kind = root.parent ? ParentRef::Kind::Bus : ParentRef::Kind::Root;
            parent.guid = root.parent ? root.parent->guid : QUuid{};
        } else if (ItemTypeCatalog::isFolderAddressable(root.type)) {
            parent = ParentRef{};
        }
        if (parent.hasGuid()) {
            const auto target = referencesByGuid_.constFind(parent.guid);
            if (target == referencesByGuid_.cend()) {
                lastError_ = QStringLiteral("Project object '%1' points to a missing parent")
                                 .arg(root.name);
                return false;
            }
            parent.documentId = target->documentId;
        }
        parentOverrides_.insert(root.guid, parent);
    }
    return true;
}

bool ProjectSerializer::registerSdkName(const QString& name, ProjectItem::Type type,
                                         const QUuid& guid) const
{
    if (type != ProjectItem::Type::Action && type != ProjectItem::Type::Variable) return true;
    if (!NameValidator::validateItemName(name, type)) {
        lastError_ = QStringLiteral("Project contains an invalid SDK object name: %1").arg(name);
        return false;
    }
    const QString key = ItemTypeCatalog::canonicalName(type) + QChar('\0') + name;
    const auto existing = sdkNames_.constFind(key);
    if (existing != sdkNames_.cend() && existing.value() != guid) {
        lastError_ = QStringLiteral("Project contains a duplicate %1 name: %2")
                         .arg(ItemTypeCatalog::canonicalName(type), name);
        return false;
    }
    sdkNames_.insert(key, guid);
    return true;
}

bool ProjectSerializer::registerReadGuid(const QUuid& guid,
                                         ProjectItem::Type type,
                                         const QUuid& documentId,
                                         const QString& name) const
{
    if (!registerSdkName(name, type, guid)) return false;
    if (guid.isNull()) {
        lastError_ = QStringLiteral("Project contains an object without a valid GUID");
        return false;
    }
    if (referencesByGuid_.contains(guid)) {
        lastError_ = QStringLiteral("Project contains a duplicate object GUID: %1")
                         .arg(guid.toString(QUuid::WithoutBraces));
        return false;
    }
    if (nextTransientObjectId_ == 0
        || nextTransientObjectId_ == std::numeric_limits<uint32_t>::max()) {
        lastError_ = QStringLiteral("Project contains too many objects to open safely");
        return false;
    }
    const uint32_t objectId = nextTransientObjectId_++;
    const ReferenceTarget target{objectId, type, guid,
                                 documentId, name};
    referencesByGuid_.insert(guid, target);
    referencesByObjectId_.insert(objectId, target);
    referencesByRuntimeStableId_.insert(runtimeObjectId(guid), target);
    return true;
}

void ProjectSerializer::resetReferenceReadState()
{
    strictReferenceRead_ = true;
    referencesByObjectId_.clear();
    referencesByRuntimeStableId_.clear();
    referencesByGuid_.clear();
    sdkNames_.clear();
    parentOverrides_.clear();
    nextTransientObjectId_ = 1;
}

bool ProjectSerializer::registerReferenceReadTree(
    const QJsonObject& node, const QUuid& documentId,
    const QString& sourceLabel, bool documentPerObject)
{
    const auto type = itemTypeFromJson(node);
    if (!type) {
        lastError_ = QStringLiteral("Project data contains an unknown object type in %1")
                         .arg(sourceLabel);
        return false;
    }
    if (!registerReadGuid(
            QUuid::fromString(node.value(QStringLiteral("guid")).toString()),
            *type, documentId, node.value(QStringLiteral("name")).toString()))
        return false;
    for (const QJsonValue& child : node.value(QStringLiteral("children")).toArray()) {
        if (!child.isObject())
            return false;
        const QJsonObject childObject = child.toObject();
        QUuid childDocumentId = documentId;
        if (documentPerObject) {
            const auto childType = itemTypeFromJson(childObject);
            if (!childType)
                return false;
            childDocumentId = QUuid::fromString(
                childObject.value(QStringLiteral("guid")).toString());
        }
        if (!registerReferenceReadTree(
                childObject, childDocumentId, sourceLabel,
                documentPerObject))
            return false;
    }
    return true;
}

bool ProjectSerializer::prepareReferenceRead(const QString& projectFilePath)
{
    resetReferenceReadState();

    QStringList paths{QFileInfo(projectFilePath).absoluteFilePath()};
    const QDir projectDirectory(QFileInfo(projectFilePath).absolutePath());
    QDirIterator iterator(projectDirectory.absolutePath(),
                          {QStringLiteral("*.vp")}, QDir::Files,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        const QString relative = QDir::fromNativeSeparators(projectDirectory.relativeFilePath(path));
        if (relative.startsWith(QStringLiteral(".cache/folder-operations/"))) continue;
        paths.append(path);
    }
    std::sort(paths.begin() + 1, paths.end(),
              [](const QString& left, const QString& right) {
                  return left.compare(right, Qt::CaseInsensitive) < 0;
              });

    for (const QString& path : paths) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            lastError_ = QStringLiteral("Cannot read project data file: %1").arg(path);
            return false;
        }
        QJsonParseError parseError;
        const QJsonDocument json = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !json.isObject()) {
            lastError_ = QStringLiteral("Project data file contains invalid JSON: %1")
                             .arg(path);
            return false;
        }
        const QJsonObject envelope = json.object()
                                         .value(QStringLiteral("harkenDocument"))
                                         .toObject();
        const QString documentType = envelope.value(QStringLiteral("type")).toString();
        const QUuid documentId = QUuid::fromString(
            envelope.value(QStringLiteral("id")).toString());
        const QJsonObject content = json.object().value(QStringLiteral("content")).toObject();
        if (documentType == QLatin1String("project.root")) continue;

        if (!registerReferenceReadTree(content, documentId, path)) return false;
    }
    return true;
}

bool ProjectSerializer::prepareStandaloneReferenceRead(
    const QJsonArray& itemRoots)
{
    for (const QJsonValue& value : itemRoots) {
        if (!value.isObject()) {
            lastError_ = QStringLiteral("Standalone project data contains a non-object item");
            return false;
        }
        if (!ProjectDocument::validateFieldGroupAuthoring(value.toObject(), &lastError_))
            return false;
    }
    resetReferenceReadState();
    for (const QJsonValue& value : itemRoots) {
        const QJsonObject root = value.toObject();
        const QUuid documentId = QUuid::fromString(
            root.value(QStringLiteral("guid")).toString());
        if (!registerReferenceReadTree(
                root, documentId, QStringLiteral("standalone project data"),
                true))
            return false;
    }
    return true;
}

uint32_t ProjectSerializer::transientObjectIdForGuid(const QUuid& guid) const
{
    const auto target = referencesByGuid_.constFind(guid);
    return target == referencesByGuid_.cend() ? 0 : target->objectId;
}

harkenstudio::ObjectId ProjectSerializer::transientRuntimeStableIdForGuid(
    const QUuid& guid) const
{
    const auto target = referencesByGuid_.constFind(guid);
    return target == referencesByGuid_.cend() ? harkenstudio::ObjectId{} : target->runtimeId();
}

uint32_t ProjectSerializer::transientObjectId(const QUuid& guid) const
{
    const auto target = referencesByGuid_.constFind(guid);
    if (target != referencesByGuid_.cend()) return target->objectId;
    if (strictReferenceRead_) {
        lastError_ = QStringLiteral("Project references an object that is not registered: %1")
                         .arg(guid.toString(QUuid::WithoutBraces));
        return 0;
    }
    if (!registerReadGuid(guid, ProjectItem::Type::None, guid, QString()))
        return 0;
    return referencesByGuid_.value(guid).objectId;
}

QJsonObject ProjectSerializer::persistentReference(
    uint32_t objectId, std::optional<ProjectItem::Type> expectedType) const
{
    if (objectId == 0) return {};
    const auto target = referencesByObjectId_.constFind(objectId);
    if (target == referencesByObjectId_.cend()) {
        lastError_ = QStringLiteral("Project contains a reference to a missing object");
        return {};
    }
    if (expectedType && target->type != *expectedType) {
        lastError_ = QStringLiteral("Project contains a reference to the wrong object type");
        return {};
    }
    QJsonObject reference{{QStringLiteral("id"),
                           target->guid.toString(QUuid::WithoutBraces)}};
    if (!target->documentId.isNull())
        reference[QStringLiteral("documentId")] =
            target->documentId.toString(QUuid::WithoutBraces);
    return reference;
}

QJsonObject ProjectSerializer::persistentRuntimeReference(
    harkenstudio::ObjectId runtimeStableId, ProjectItem::Type expectedType) const
{
    if (harkenstudio::IsNull(runtimeStableId)) return {};
    const auto target = referencesByRuntimeStableId_.constFind(runtimeStableId);
    if (target == referencesByRuntimeStableId_.cend()) {
        lastError_ = QStringLiteral("Project contains a reference to a missing object");
        return {};
    }
    if (target->type != expectedType) {
        lastError_ = QStringLiteral("Project contains a reference to the wrong object type");
        return {};
    }
    QJsonObject reference{{QStringLiteral("id"),
                           target->guid.toString(QUuid::WithoutBraces)}};
    if (!target->documentId.isNull())
        reference[QStringLiteral("documentId")] =
            target->documentId.toString(QUuid::WithoutBraces);
    return reference;
}

bool ProjectSerializer::transientReference(
    const QJsonValue& value, std::optional<ProjectItem::Type> expectedType,
    uint32_t* objectId, QString* targetName) const
{
    if (!objectId || !value.isObject()) return false;
    const QJsonObject reference = value.toObject();
    const QUuid guid = QUuid::fromString(reference.value(QStringLiteral("id")).toString());
    const auto target = referencesByGuid_.constFind(guid);
    if (guid.isNull() || target == referencesByGuid_.cend()) {
        lastError_ = QStringLiteral("Project contains a reference to a missing object");
        return false;
    }
    if (expectedType && target->type != *expectedType) {
        lastError_ = QStringLiteral("Project contains a reference to the wrong object type");
        return false;
    }
    if (reference.contains(QStringLiteral("documentId"))) {
        const QUuid documentId = QUuid::fromString(
            reference.value(QStringLiteral("documentId")).toString());
        if (documentId.isNull() || documentId != target->documentId) {
            lastError_ = QStringLiteral("Project reference points to the wrong document");
            return false;
        }
    }
    *objectId = target->objectId;
    if (targetName) *targetName = target->name;
    return true;
}

bool ProjectSerializer::transientRuntimeReference(
    const QJsonValue& value, ProjectItem::Type expectedType,
    harkenstudio::ObjectId* runtimeStableId) const
{
    if (!runtimeStableId) return false;
    if (value.isNull()) {
        *runtimeStableId = {};
        return true;
    }
    uint32_t objectId = 0;
    if (!transientReference(value, expectedType, &objectId)) return false;
    const auto target = referencesByObjectId_.constFind(objectId);
    if (target == referencesByObjectId_.cend() || harkenstudio::IsNull(target->runtimeId())) {
        lastError_ = QStringLiteral("Project reference target has no runtime identity");
        return false;
    }
    *runtimeStableId = target->runtimeId();
    return true;
}

QJsonObject ProjectSerializer::persistentParentReference(const ProjectItem& item) const
{
    ParentRef ref = parentOverrides_.value(item.guid, item.parentRef);
    if ((item.type == ProjectItem::Type::Bus || ref.kind == ParentRef::Kind::Item)
        && item.parent) {
        ref.kind = item.type == ProjectItem::Type::Bus
            ? ParentRef::Kind::Bus : ParentRef::Kind::Item;
        ref.guid = item.parent->guid;
    }
    if (ref.hasGuid()) {
        const auto target = referencesByGuid_.constFind(ref.guid);
        if (target == referencesByGuid_.cend()) {
            lastError_ = QStringLiteral("Project object '%1' points to a missing parent")
                             .arg(item.name);
            return {};
        }
        ref.documentId = target->documentId;
    }
    return ref.toJson();
}

bool ProjectSerializer::loadParentReference(const QJsonObject& json,
                                            ParentRef* ref) const
{
    if (!ref) return false;
    const auto parsed = ParentRef::fromJson(json);
    if (!parsed) return false;
    if (parsed->hasGuid()) {
        uint32_t ignored = 0;
        if (!transientReference(json, std::nullopt, &ignored)) return false;
    }
    *ref = *parsed;
    return true;
}

bool ProjectSerializer::encodeComponentReferences(QJsonObject* components) const
{
    if (!components) return false;
    const QString actionKey = componentTypeName(ComponentType::Action);
    if (components->value(actionKey).isObject()) {
        QJsonObject action = components->value(actionKey).toObject();
        if (action.contains(QStringLiteral("steps"))) {
            if (!action.value(QStringLiteral("steps")).isArray()) return false;
            QJsonArray steps = action.value(QStringLiteral("steps")).toArray();
            if (!encodeActionReferences(&steps)) return false;
            action[QStringLiteral("steps")] = steps;
        }
        (*components)[actionKey] = action;
    }

    const QString mixKey = componentTypeName(ComponentType::MixNode);
    if (components->value(mixKey).isObject()) {
        QJsonObject mix = components->value(mixKey).toObject();
        if (mix.contains(QStringLiteral("outputBusId"))) {
            const uint32_t id = static_cast<uint32_t>(
                mix.value(QStringLiteral("outputBusId")).toInteger(0));
            const QJsonObject reference = persistentReference(id, ProjectItem::Type::Bus);
            if (id != 0 && reference.isEmpty()) return false;
            mix.remove(QStringLiteral("outputBusId"));
            if (!reference.isEmpty())
                mix[QStringLiteral("outputBusRef")] = reference;
        }
        if (mix.contains(QStringLiteral("sends"))) {
            if (!mix.value(QStringLiteral("sends")).isArray()) return false;
            QJsonArray sends;
            for (const QJsonValue& value : mix.value(QStringLiteral("sends")).toArray()) {
                if (!value.isObject()) return false;
                QJsonObject send = value.toObject();
                const uint32_t id = static_cast<uint32_t>(
                    send.value(QStringLiteral("targetBusID")).toInteger(0));
                const QJsonObject reference = persistentReference(
                    id, ProjectItem::Type::Bus);
                if (id == 0 || reference.isEmpty()) return false;
                send.remove(QStringLiteral("targetBusID"));
                send[QStringLiteral("targetBusRef")] = reference;
                sends.append(send);
            }
            mix[QStringLiteral("sends")] = sends;
        }
        (*components)[mixKey] = mix;
    }

    const QString timelineKey = componentTypeName(ComponentType::Track);
    if (components->value(timelineKey).isObject()) {
        QJsonObject timeline = components->value(timelineKey).toObject();
        if (timeline.contains(QStringLiteral("clips"))) {
            if (!timeline.value(QStringLiteral("clips")).isArray()) return false;
            QJsonArray clips;
            for (const QJsonValue& value : timeline.value(QStringLiteral("clips")).toArray()) {
                if (!value.isObject()) return false;
                QJsonObject clip = value.toObject();
                QJsonObject properties = clip.value(
                    QLatin1String(harkenplayableproperties::studio::kEnvelopeJsonKeyV1))
                                             .toObject();
                if (!encodePlayableReferences(&properties)) return false;
                clip[QLatin1String(
                    harkenplayableproperties::studio::kEnvelopeJsonKeyV1)] = properties;
                clips.append(clip);
            }
            timeline[QStringLiteral("clips")] = clips;
        }
        (*components)[timelineKey] = timeline;
    }
    return true;
}

bool ProjectSerializer::decodeComponentReferences(QJsonObject* components) const
{
    if (!components) return false;
    const QString actionKey = componentTypeName(ComponentType::Action);
    if (components->value(actionKey).isObject()) {
        QJsonObject action = components->value(actionKey).toObject();
        if (action.contains(QStringLiteral("steps"))) {
            if (!action.value(QStringLiteral("steps")).isArray()) return false;
            QJsonArray steps = action.value(QStringLiteral("steps")).toArray();
            if (!decodeActionReferences(&steps)) return false;
            action[QStringLiteral("steps")] = steps;
        }
        (*components)[actionKey] = action;
    }

    const QString mixKey = componentTypeName(ComponentType::MixNode);
    if (components->value(mixKey).isObject()) {
        QJsonObject mix = components->value(mixKey).toObject();
        if (mix.contains(QStringLiteral("outputBusRef"))) {
            uint32_t id = 0;
            if (!transientReference(mix.value(QStringLiteral("outputBusRef")),
                                    ProjectItem::Type::Bus, &id))
                return false;
            mix.remove(QStringLiteral("outputBusRef"));
            mix[QStringLiteral("outputBusId")] = static_cast<qint64>(id);
        }
        if (mix.contains(QStringLiteral("sends"))) {
            if (!mix.value(QStringLiteral("sends")).isArray()) return false;
            QJsonArray sends;
            for (const QJsonValue& value : mix.value(QStringLiteral("sends")).toArray()) {
                if (!value.isObject()) return false;
                QJsonObject send = value.toObject();
                uint32_t id = 0;
                if (!transientReference(send.value(QStringLiteral("targetBusRef")),
                                        ProjectItem::Type::Bus, &id))
                    return false;
                send.remove(QStringLiteral("targetBusRef"));
                send[QStringLiteral("targetBusID")] = static_cast<qint64>(id);
                sends.append(send);
            }
            mix[QStringLiteral("sends")] = sends;
        }
        (*components)[mixKey] = mix;
    }

    const QString timelineKey = componentTypeName(ComponentType::Track);
    if (components->value(timelineKey).isObject()) {
        QJsonObject timeline = components->value(timelineKey).toObject();
        if (timeline.contains(QStringLiteral("clips"))) {
            if (!timeline.value(QStringLiteral("clips")).isArray()) return false;
            QJsonArray clips;
            for (const QJsonValue& value : timeline.value(QStringLiteral("clips")).toArray()) {
                if (!value.isObject()) return false;
                QJsonObject clip = value.toObject();
                const QLatin1String key(
                    harkenplayableproperties::studio::kEnvelopeJsonKeyV1);
                if (!clip.value(key).isObject()) return false;
                QJsonObject properties = clip.value(key).toObject();
                if (!decodePlayableReferences(&properties)) return false;
                clip[key] = properties;
                clips.append(clip);
            }
            timeline[QStringLiteral("clips")] = clips;
        }
        (*components)[timelineKey] = timeline;
    }
    return true;
}

bool ProjectSerializer::removeRetiredPlayableProperties(QJsonObject* properties) const
{
    if (!properties) return false;
    const QString key = QStringLiteral("spatial.attenuationCurveType");
    const QString graphKey = QStringLiteral("signalGraph");
    const QString presenceKey = QStringLiteral("presence");
    const QString presence = properties->value(presenceKey).toString();
    bool validPresence = false;
    const uint64_t bits = presence.toULongLong(&validPresence, 16);
    constexpr uint64_t curveBit = UINT64_C(0x1000);
    constexpr uint64_t graphBit = UINT64_C(0x8000);
    const bool curve = properties->contains(key) || (bits & curveBit) != 0;
    const bool graph = properties->contains(graphKey) || (bits & graphBit) != 0;
    if (!curve && !graph) return true;
    const uint64_t remainingBits = bits & ~(curveBit | graphBit);
    if (!validPresence
        || PlayablePropertyAuthoringV1::encodePresence(bits) != presence
        || !PlayablePropertyAuthoringV1::decodePresence(
            PlayablePropertyAuthoringV1::encodePresence(remainingBits))) {
        lastError_ = curve
            ? QStringLiteral("Retired spatial.attenuationCurveType has an invalid value or presence")
            : QStringLiteral("Retired signalGraph has an invalid presence");
        return false;
    }
    if (curve) {
        const QJsonValue value = properties->value(key);
        if ((bits & curveBit) == 0 || !value.isDouble()
            || value.toInt(-1) < 0 || value.toInt(-1) > 2
            || value.toDouble() != value.toInt(-1)) {
            lastError_ = QStringLiteral("Retired spatial.attenuationCurveType has an invalid value or presence");
            return false;
        }
    }
    properties->remove(key);
    properties->remove(graphKey);
    (*properties)[presenceKey] = PlayablePropertyAuthoringV1::encodePresence(remainingBits);
    return true;
}

bool ProjectSerializer::rewritePlayableReferenceFields(QJsonObject* properties,
                                                       bool encode) const
{
    if (!properties) return false;
    using Role = harkenplayableproperties::PlayablePropertyReferenceRoleV1;
    for (const auto& field : VNX::PlayablePropertyAuthoringV1::fields()) {
        const QLatin1String key(field.jsonKey);
        if (!properties->contains(key)) continue;
        if (field.referenceRole == Role::MainOutputBus
            || field.referenceRole == Role::Attenuation) {
            const ProjectItem::Type expectedType =
                field.referenceRole == Role::MainOutputBus
                    ? ProjectItem::Type::Bus
                    : ProjectItem::Type::Attenuation;
            if (encode) {
                const QJsonValue value = properties->value(key);
                if (!value.isString()) return false;
                harkenstudio::ObjectId stableId{};
                if (!parseObjectIdJson(value, &stableId)) return false;
                if (harkenstudio::IsNull(stableId)) {
                    (*properties)[key] = QJsonValue::Null;
                    continue;
                }
                const QJsonObject reference = persistentRuntimeReference(
                    stableId, expectedType);
                if (reference.isEmpty()) return false;
                (*properties)[key] = reference;
            } else {
                harkenstudio::ObjectId stableId = {};
                if (!transientRuntimeReference(
                        properties->value(key), expectedType, &stableId))
                    return false;
                (*properties)[key] = objectIdString(stableId);
            }
            continue;
        }
        if (field.id != 8) continue;
        const QJsonValue value = properties->value(key);
        if (!value.isArray()) return false;
        QJsonArray sends;
        for (const QJsonValue& rowValue : value.toArray()) {
            if (!rowValue.isObject()) return false;
            QJsonObject row = rowValue.toObject();
            if (encode) {
                const uint32_t targetId = static_cast<uint32_t>(
                    row.value(QStringLiteral("targetBusID")).toInteger(0));
                const QJsonObject reference = persistentReference(
                    targetId, ProjectItem::Type::Bus);
                if (targetId == 0 || reference.isEmpty()) return false;
                row.remove(QStringLiteral("targetBusID"));
                row[QStringLiteral("targetBusRef")] = reference;
            } else {
                uint32_t targetId = 0;
                if (!transientReference(row.value(QStringLiteral("targetBusRef")),
                                        ProjectItem::Type::Bus, &targetId))
                    return false;
                row.remove(QStringLiteral("targetBusRef"));
                row[QStringLiteral("targetBusID")] = static_cast<qint64>(targetId);
            }
            sends.append(row);
        }
        (*properties)[key] = sends;
    }
    return true;
}

bool ProjectSerializer::encodePlayableReferences(QJsonObject* properties,
                                                  bool allowFieldGroupFields) const
{
    if (!properties || properties->isEmpty()) {
        lastError_ = QStringLiteral("Playable property authoring is invalid");
        return false;
    }
    if (!allowFieldGroupFields && validatedOccurrenceProperties_ != *properties) {
        std::string detail;
        if (!ValidateOccurrenceFieldGroupExclusionJson(
                QJsonDocument(*properties).toJson(QJsonDocument::Compact).toStdString(), detail)) {
            lastError_ = QString::fromStdString(detail);
            return false;
        }
        validatedOccurrenceProperties_ = *properties;
    }
    if (!rewritePlayableReferenceFields(properties, true)) return false;
    const QLatin1String overlaysKey(
        PlayablePropertyAuthoringV1::kPlatformOverridesJsonKey);
    if (!properties->contains(overlaysKey)) return true;
    const QJsonValue overlaysValue = properties->value(overlaysKey);
    if (!overlaysValue.isArray()) return false;
    QJsonArray rewritten;
    for (const QJsonValue& rowValue : overlaysValue.toArray()) {
        if (!rowValue.isObject()) return false;
        QJsonObject row = rowValue.toObject();
        if (!rewritePlayableReferenceFields(&row, true)) return false;
        rewritten.append(row);
    }
    (*properties)[overlaysKey] = rewritten;
    return true;
}

bool ProjectSerializer::decodePlayableReferences(QJsonObject* properties,
                                                  bool allowFieldGroupFields) const
{
    if (!properties) return false;
    if (!allowFieldGroupFields) {
        std::string detail;
        if (!ValidateOccurrenceFieldGroupExclusionJson(
                QJsonDocument(*properties).toJson(QJsonDocument::Compact).toStdString(), detail)) {
            lastError_ = QString::fromStdString(detail);
            return false;
        }
    }
    if (!removeRetiredPlayableProperties(properties)) return false;
    if (!rewritePlayableReferenceFields(properties, false)) return false;
    const QLatin1String overlaysKey(
        PlayablePropertyAuthoringV1::kPlatformOverridesJsonKey);
    if (!properties->contains(overlaysKey)) return true;
    const QJsonValue overlaysValue = properties->value(overlaysKey);
    if (!overlaysValue.isArray()) return false;
    QJsonArray rewritten;
    for (const QJsonValue& rowValue : overlaysValue.toArray()) {
        if (!rowValue.isObject()) return false;
        QJsonObject row = rowValue.toObject();
        if (!removeRetiredPlayableProperties(&row)) return false;
        if (!rewritePlayableReferenceFields(&row, false)) return false;
        rewritten.append(row);
    }
    (*properties)[overlaysKey] = rewritten;
    return true;
}

bool ProjectSerializer::encodeActionReferences(QJsonArray* actions) const
{
    if (!actions) return false;
    QJsonArray encoded;
    for (const QJsonValue& value : *actions) {
        QJsonObject action = value.toObject();
        const auto type = VActionStep::typeFromString(
            action.value(QStringLiteral("type")).toString());
        if (!type) return false;
        const uint32_t targetId = static_cast<uint32_t>(
            action.value(QStringLiteral("targetObjectID")).toInteger(0));
        action.remove(QStringLiteral("targetObjectID"));
        action.remove(QStringLiteral("targetName"));
        if (targetId != 0) {
            const auto target = referencesByObjectId_.constFind(targetId);
            if (target == referencesByObjectId_.cend()
                || !EditorUtils::canActionStepReferenceObject(
                    *type, target->type, targetId, 0)) {
                lastError_ = QStringLiteral("Action contains an invalid target reference");
                return false;
            }
            const QJsonObject reference = persistentReference(targetId);
            if (reference.isEmpty()) return false;
            action[QStringLiteral("targetRef")] = reference;
        }
        if (action.value(QStringLiteral("quantize")).isObject()) {
            QJsonObject quantize = action.value(QStringLiteral("quantize")).toObject();
            const uint32_t referenceId = static_cast<uint32_t>(
                quantize.value(QStringLiteral("referenceObjectID")).toInteger(0));
            quantize.remove(QStringLiteral("referenceObjectID"));
            quantize.remove(QStringLiteral("referenceName"));
            if (referenceId != 0) {
                const auto target = referencesByObjectId_.constFind(referenceId);
                const bool validType = target != referencesByObjectId_.cend()
                    && (target->type == ProjectItem::Type::Sound
                        || target->type == ProjectItem::Type::Container
                        || target->type == ProjectItem::Type::Track
                        || target->type == ProjectItem::Type::Piece);
                if (!validType) {
                    lastError_ = QStringLiteral("Action contains an invalid timing reference");
                    return false;
                }
                const QJsonObject reference = persistentReference(referenceId);
                if (reference.isEmpty()) return false;
                quantize[QStringLiteral("referenceRef")] = reference;
            }
            action[QStringLiteral("quantize")] = quantize;
        }
        encoded.append(action);
    }
    *actions = encoded;
    return true;
}

bool ProjectSerializer::decodeActionReferences(QJsonArray* actions) const
{
    if (!actions) return false;
    QJsonArray decoded;
    for (const QJsonValue& value : *actions) {
        QJsonObject action = value.toObject();
        const auto type = VActionStep::typeFromString(
            action.value(QStringLiteral("type")).toString());
        if (!type) return false;
        uint32_t targetId = 0;
        QString targetName;
        if (action.contains(QStringLiteral("targetRef"))) {
            if (!transientReference(action.value(QStringLiteral("targetRef")),
                                    std::nullopt, &targetId, &targetName))
                return false;
            const auto target = referencesByObjectId_.constFind(targetId);
            if (target == referencesByObjectId_.cend()
                || !EditorUtils::canActionStepReferenceObject(
                    *type, target->type, targetId, 0)) {
                lastError_ = QStringLiteral("Action contains a target of the wrong type");
                return false;
            }
        }
        action.remove(QStringLiteral("targetRef"));
        action[QStringLiteral("targetObjectID")] = static_cast<qint64>(targetId);
        action[QStringLiteral("targetName")] = targetName;
        if (action.value(QStringLiteral("quantize")).isObject()) {
            QJsonObject quantize = action.value(QStringLiteral("quantize")).toObject();
            uint32_t referenceId = 0;
            QString referenceName;
            if (quantize.contains(QStringLiteral("referenceRef"))) {
                if (!transientReference(quantize.value(QStringLiteral("referenceRef")),
                                        std::nullopt, &referenceId, &referenceName))
                    return false;
                const auto target = referencesByObjectId_.constFind(referenceId);
                const bool validType = target != referencesByObjectId_.cend()
                    && (target->type == ProjectItem::Type::Sound
                        || target->type == ProjectItem::Type::Container
                        || target->type == ProjectItem::Type::Track
                        || target->type == ProjectItem::Type::Piece);
                if (!validType) {
                    lastError_ = QStringLiteral("Action contains a timing target of the wrong type");
                    return false;
                }
            }
            quantize.remove(QStringLiteral("referenceRef"));
            quantize[QStringLiteral("referenceObjectID")] =
                static_cast<qint64>(referenceId);
            quantize[QStringLiteral("referenceName")] = referenceName;
            action[QStringLiteral("quantize")] = quantize;
        }
        decoded.append(action);
    }
    *actions = decoded;
    return true;
}

bool ProjectSerializer::saveProject(const QString& filePath, ProjectData& project)
{
    if (!prepareProjectSave(filePath, project)) return false;
    return commitProjectSave(&project.documentHeader);
}

bool ProjectSerializer::saveProject(const QString& filePath, const ProjectData& project)
{
    ProjectData copy = project;
    return saveProject(filePath, copy);
}

bool ProjectSerializer::prepareProjectSave(const QString& filePath,
                                            ProjectData& project,
                                            const QString& stagingFilePath)
{
    if (!PathUtils::isValidPackBuildOutputDir(project.packBuildOutputDir, filePath)) {
        lastError_ = tr("Pack output must use a valid directory path. ")
            + PathUtils::packBuildOutputLocationError();
        emit errorOccurred(lastError_);
        return false;
    }
    if (!PathUtils::isValidWaveformBuildOutputDir(project.buildWaveformOutputDir, filePath)) {
        lastError_ = tr("Waveform output must use a valid directory outside the project root and object directories.");
        emit errorOccurred(lastError_);
        return false;
    }
    if (!validatePlaybackReferences(project, &lastError_)) {
        emit errorOccurred(lastError_);
        return false;
    }
    const auto groupValidation = validateFieldGroupProject(project);
    if (!groupValidation.ok()) {
        lastError_ = groupValidation.diagnostics.front().message;
        emit errorOccurred(lastError_);
        return false;
    }
    if (project.projectId.isEmpty())
        project.projectId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (QUuid::fromString(project.projectId).isNull()) {
        lastError_ = QStringLiteral("Project identity is invalid");
        emit errorOccurred(lastError_);
        return false;
    }
    if (!ProjectPlatformSemantics::validateProject(project, &lastError_)) {
        emit errorOccurred(lastError_);
        return false;
    }
    if (!prepareReferenceWrite(project)) {
        emit errorOccurred(lastError_);
        return false;
    }
    emit progressChanged(0, tr("Preparing project data..."));

    QJsonObject root;
    
    QJsonObject metadata;
    metadata["name"] = project.name;
    metadata["version"] = project.version;
    metadata["author"] = project.author;
    metadata["description"] = project.description;
    metadata["createdDate"] = project.createdDate.isEmpty() 
        ? QDateTime::currentDateTime().toString(Qt::ISODate) 
        : project.createdDate;
    metadata["modifiedDate"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    metadata["toolVersion"] = "HarkenStudio 1.0.0";
    {
        metadata["platforms"] = ProjectPlatformSemantics::platformsToJson(project.platforms);
        metadata["sampleRate"]        = project.sampleRate;
        metadata["channelConfig"]     = static_cast<int>(project.channelConfig);
        metadata["enableLFE"]         = project.enableLFE;
        metadata["projectTemplateId"] = project.projectTemplateId;
    }
    root["metadata"] = metadata;

    if (!project.extensions.isEmpty())
        root[QStringLiteral("extensions")] = project.extensions;

    emit progressChanged(20, tr("Saving project structure..."));

    QJsonObject paths;
    paths["audioFolder"] = project.audioFolder;
    paths["packFolder"] = project.packFolder;
    root["paths"] = paths;

    QJsonObject sourceAssets;
    for (auto it = project.sourceAssets.cbegin(); it != project.sourceAssets.cend(); ++it)
        sourceAssets[QString::number(it.key())] = it.value();
    QMap<uint32_t, QString> validatedSources;
    if (!AssetRegistry::sourceAssetsFromJson(sourceAssets, validatedSources)) {
        lastError_ = QStringLiteral("Invalid source asset references");
        emit errorOccurred(lastError_);
        return false;
    }
    root[QStringLiteral("sourceAssets")] = sourceAssets;


    emit progressChanged(40, tr("Saving project metadata..."));



    const QStringList projectLanguages =
        ProjectLanguageSemantics::dropSfxNames(project.projectLanguages);
    if (!projectLanguages.isEmpty()) {
        QJsonArray langArray;
        for (const auto& lang : projectLanguages)
            langArray.append(lang);
        root["projectLanguages"] = langArray;
    }
    if (!project.defaultLanguage.isEmpty())
        root["defaultLanguage"] = project.defaultLanguage;

    if (!project.deploymentProfiles.empty()) {
        QJsonArray profiles;
        for (const auto& profile : project.deploymentProfiles) {
            QJsonObject row;
            row["profileId"] = profile.profileId;
            row["name"] = profile.name;
            row["engineKind"] = profile.engineKind;
            row["destination"] = profile.destination;
            QJsonArray platforms;
            for (const QString& platform : profile.platformIds)
                platforms.append(platform);
            row["platformIds"] = platforms;
            QJsonArray languages;
            for (const QString& language : profile.languages)
                languages.append(language);
            row["languages"] = languages;
            row["deployPacks"] = profile.deployPacks;
            row["deployManifest"] = profile.deployManifest;
            row["deployCurrentWorld"] = profile.deployCurrentWorld;
            row["autoDeployAfterBuild"] = profile.autoDeployAfterBuild;
            profiles.append(row);
        }
        root["deploymentProfiles"] = profiles;
    }

    if (!project.packBuildOutputDir.isEmpty()) {
        root["packBuildOutputDir"] = PathUtils::normalizePackBuildOutputDir(project.packBuildOutputDir, filePath);
    }
    if (project.buildExportProjectData)
        root["buildExportProjectData"] = true;
    if (project.buildExportPackMetadata)
        root["buildExportPackMetadata"] = true;
    if (project.buildExportWaveforms)
        root["buildExportWaveforms"] = true;
    if (!project.buildWaveformOutputDir.isEmpty())
        root["buildWaveformOutputDir"] = PathUtils::normalizePackBuildOutputDir(project.buildWaveformOutputDir, filePath);

    if (!project.actionPublishedNameHistory.empty()) {
        QJsonArray history;
        for (const auto& row : project.actionPublishedNameHistory)
            history.append(actionPublishedNameHistoryRowToJson(row));
        root[QLatin1String(kActionPublishedNameHistory)] = history;
    }

    emit progressChanged(60, tr("Writing file..."));

    cancelProjectSave();
    const QString outputFilePath = stagingFilePath.isEmpty() ? filePath : stagingFilePath;
    pendingProjectSave_ = std::make_unique<QSaveFile>(outputFilePath);
    if (!pendingProjectSave_->open(QIODevice::WriteOnly | QIODevice::Text))
    {
        lastError_ = tr("Cannot open file for writing: %1")
                         .arg(pendingProjectSave_->errorString());
        emit errorOccurred(lastError_);
        pendingProjectSave_.reset();
        return false;
    }

    const QJsonObject document = ProjectDocument::create(
        QStringLiteral("project.root"), project.projectId, project.projectId, root,
        project.documentHeader);
    QJsonDocument doc(document);
    const QByteArray bytes = doc.toJson(QJsonDocument::Indented);
    if (pendingProjectSave_->write(bytes) != bytes.size()) {
        lastError_ = tr("Cannot write project file: %1")
                         .arg(pendingProjectSave_->errorString());
        emit errorOccurred(lastError_);
        cancelProjectSave();
        return false;
    }

    pendingSaveFilePath_ = outputFilePath;
    pendingSaveBytes_ = bytes;
    pendingSaveHeader_ = document.value(QStringLiteral("harkenDocument")).toObject();
    return true;
}

bool ProjectSerializer::commitProjectSave(QJsonObject* savedHeader)
{
    if (!pendingProjectSave_) return false;
    // QSaveFile::commit() removes its temp file on a failed rename, so a
    // failed attempt can't be retried on the same object. On Windows the
    // rename can be transiently denied (e.g. AV scanning the temp file), so
    // rebuild the pending file from the already-serialized bytes and retry.
    constexpr int kMaxAttempts = 3;
    constexpr int kRetryDelayMs = 50;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        std::unique_ptr<QSaveFile> file = std::move(pendingProjectSave_);
        if (file->commit()) {
            if (savedHeader) *savedHeader = pendingSaveHeader_;
            pendingSaveHeader_ = {};
            pendingSaveFilePath_.clear();
            pendingSaveBytes_.clear();
            emit progressChanged(100, tr("Project saved successfully"));
            return true;
        }
        lastError_ = tr("Cannot commit project file: %1").arg(file->errorString());
        if (attempt == kMaxAttempts) break;

        QThread::msleep(kRetryDelayMs);
        pendingProjectSave_ = std::make_unique<QSaveFile>(pendingSaveFilePath_);
        if (!pendingProjectSave_->open(QIODevice::WriteOnly | QIODevice::Text)
            || pendingProjectSave_->write(pendingSaveBytes_) != pendingSaveBytes_.size())
        {
            lastError_ = tr("Cannot write project file: %1")
                             .arg(pendingProjectSave_->errorString());
            pendingProjectSave_.reset();
            break;
        }
    }
    emit errorOccurred(lastError_);
    pendingSaveFilePath_.clear();
    pendingSaveBytes_.clear();
    pendingSaveHeader_ = {};
    return false;
}

void ProjectSerializer::cancelProjectSave()
{
    if (!pendingProjectSave_) return;
    pendingProjectSave_->cancelWriting();
    pendingProjectSave_.reset();
    pendingSaveFilePath_.clear();
    pendingSaveBytes_.clear();
    pendingSaveHeader_ = {};
}

bool ProjectSerializer::loadProject(const QString& filePath, ProjectData& destinationProject,
                                    bool allowUpgrade,
                                    ProjectUpgradeProgressCallback upgradeProgress,
                                    ProjectUpgradeApprovalCallback upgradeApproval,
                                    ProjectContractReadContext* context)
{
    ProjectContractReadContext localContext;
    if (!context) context = &localContext;
    ProjectData project = destinationProject;
    warnings_.clear();
    lastError_.clear();
    upgradeResult_ = {};
    emit progressChanged(0, tr("Reading project file..."));

    const auto location = ProjectLocator::locate(filePath);
    if (!location.ok()) {
        lastError_ = projectIssueCode(location.issue);
        emit errorOccurred(ProjectUpgradeMessages::userMessage(lastError_, true));
        return false;
    }
    const auto modelProbe = ProjectContractRegistry::instance().probe(location.projectFilePath, context);
    if (!modelProbe.ok) {
        lastError_ = modelProbe.blockingErrors.isEmpty()
            ? modelProbe.detail : modelProbe.blockingErrors.first();
        emit errorOccurred(ProjectUpgradeMessages::userMessage(lastError_, true));
        return false;
    }

    const bool boundUpgradeOpen = static_cast<bool>(upgradeApproval);
    if (!boundUpgradeOpen) {
        QString recoveryError;
        const auto recoveryProgress = [&upgradeProgress](
            int completedFiles, int totalFiles,
            qint64 completedBytes, qint64 totalBytes) {
            if (upgradeProgress) {
                upgradeProgress({ProjectUpgradePhase::RollingBack,
                                 completedFiles, totalFiles,
                                 completedBytes, totalBytes, {}});
            }
        };
        if (!ProjectTransaction::recover(filePath, &recoveryError,
                                         recoveryProgress)) {
            lastError_ = recoveryError;
            emit errorOccurred(ProjectUpgradeMessages::userMessage(lastError_, true));
            return false;
        }
    }

    ProjectUpgradeManager upgradeManager;
    ProjectUpgradeAnalysis analysis = boundUpgradeOpen
        ? upgradeManager.analyzeForProjectOpen(filePath, upgradeProgress, context)
        : upgradeManager.analyze(filePath, upgradeProgress, context);
    if (!analysis.ok) {
        lastError_ = analysis.blockingErrors.isEmpty()
                         ? QStringLiteral("Project governance analysis failed")
                         : analysis.blockingErrors.first();
        if (analysis.issue == ProjectIssue::ProjectUpgradeFilesNotWritable)
            lastError_ = projectIssueCode(analysis.issue) + QLatin1Char(':') + analysis.detail;
        emit errorOccurred(ProjectUpgradeMessages::userMessage(lastError_, true));
        return false;
    }
    const QString resolvedProjectFilePath = analysis.projectFilePath;
    if (analysis.upgradeRequired) {
        if (!allowUpgrade) {
            const PreflightReport preflight = ProjectPreflight::run(
                resolvedProjectFilePath, analysis.targetSchema, context);
            if (!preflight.blockingErrors.isEmpty()) {
                lastError_ = preflight.blockingErrors.first();
                emit errorOccurred(ProjectUpgradeMessages::userMessage(lastError_, true));
                return false;
            }
            if (analysis.sourceSchema < analysis.targetSchema) {
                lastError_ = ProjectPreflight::schemaMismatchError(
                    analysis.sourceSchema, analysis.targetSchema);
            } else {
                lastError_ = QStringLiteral(
                    "Project upgrade requires explicit user approval before opening");
            }
            emit errorOccurred(ProjectUpgradeMessages::userMessage(lastError_, true));
            return false;
        }
        if (!upgradeApproval || !upgradeApproval(analysis)) {
            lastError_ = projectIssueCode(
                ProjectIssue::ProjectChangedDuringUpgrade);
            emit errorOccurred(ProjectUpgradeMessages::userMessage(lastError_, true));
            return false;
        }
        const ProjectUpgradeResult upgrade = upgradeManager.execute(
            analysis,
            [](const QString& stagedProjectFile, QString* error) {
                ProjectSerializer stagedSerializer;
                ProjectData stagedProject;
                if (stagedSerializer.loadProject(stagedProjectFile, stagedProject, false))
                    return true;
                if (error)
                    *error = stagedSerializer.lastError();
                return false;
            },
            upgradeProgress);
        if (!upgrade.ok) {
            lastError_ = upgrade.error;
            emit errorOccurred(ProjectUpgradeMessages::userMessage(lastError_, true));
            return false;
        }
        upgradeResult_ = upgrade;
        warnings_.append(upgrade.warnings);
    }

    const PreflightReport preflight = ProjectPreflight::run(resolvedProjectFilePath, 0, context);
    if (!preflight.blockingErrors.isEmpty()) {
        lastError_ = preflight.blockingErrors.first();
        emit errorOccurred(ProjectUpgradeMessages::userMessage(lastError_, true));
        return false;
    }
    if (!prepareReferenceRead(resolvedProjectFilePath)) {
        emit errorOccurred(ProjectUpgradeMessages::userMessage(lastError_, true));
        return false;
    }

    QFile file(resolvedProjectFilePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        lastError_ = tr("Cannot open file for reading: %1").arg(file.errorString());
        emit errorOccurred(ProjectUpgradeMessages::userMessage(lastError_, true));
        return false;
    }

    QByteArray data = file.readAll();
    file.close();

    emit progressChanged(20, tr("Parsing JSON..."));

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
    {
        lastError_ = tr("JSON parse error: %1").arg(parseError.errorString());
        emit errorOccurred(ProjectUpgradeMessages::userMessage(lastError_, true));
        return false;
    }

    ProjectDocumentData projectDocument;
    QString documentError;
    if (!ProjectDocument::parse(doc.object(), QStringLiteral("project.root"),
                                &projectDocument, &documentError)) {
        lastError_ = documentError;
        emit errorOccurred(ProjectUpgradeMessages::userMessage(lastError_, true));
        return false;
    }
    QJsonObject root = projectDocument.content;

    project.projectId = projectDocument.id;
    project.documentHeader = projectDocument.header;
    project.extensions = root.value(QStringLiteral("extensions")).toObject();
    if (!AssetRegistry::sourceAssetsFromJson(root.value(QStringLiteral("sourceAssets")),
                                            project.sourceAssets)) {
        lastError_ = QStringLiteral("Invalid source asset references");
        emit errorOccurred(lastError_);
        return false;
    }
    if (project.projectId.isEmpty()) {
        lastError_ = QStringLiteral("Project identity is missing");
        emit errorOccurred(ProjectUpgradeMessages::userMessage(lastError_, true));
        return false;
    }

    emit progressChanged(40, tr("Loading metadata..."));

    QJsonObject metadata = root["metadata"].toObject();
    project.name = metadata["name"].toString();
    project.version = metadata["version"].toString("1.0");
    project.author = metadata["author"].toString();
    project.description = metadata["description"].toString();
    project.createdDate = metadata["createdDate"].toString();
    project.modifiedDate = metadata["modifiedDate"].toString();

    if (!ProjectPlatformSemantics::platformsFromJson(metadata["platforms"], &project.platforms, &lastError_))
        return false;
    if (metadata.contains("sampleRate")) {
        project.sampleRate = metadata["sampleRate"].toInt(project.sampleRate);
    }
    if (metadata.contains("channelConfig")) {
        project.channelConfig = static_cast<AudioChannelConfig>(
            metadata["channelConfig"].toInt(static_cast<int>(project.channelConfig)));
    }
    if (metadata.contains("enableLFE")) {
        project.enableLFE = metadata["enableLFE"].toBool(project.enableLFE);
    }
    if (metadata.contains("projectTemplateId")) {
        project.projectTemplateId = metadata["projectTemplateId"].toString();
    }

    emit progressChanged(60, tr("Loading project structure..."));

    QJsonObject paths = root["paths"].toObject();
    project.audioFolder = paths["audioFolder"].toString(
        QString::fromLatin1(ItemTypeCatalog::kLayersFolderName));
    project.packFolder = paths["packFolder"].toString();
    project.projectPath = resolvedProjectFilePath;


    emit progressChanged(70, tr("Loading project metadata..."));


    QStringList loadedLanguages;
    for (const auto& v : root["projectLanguages"].toArray())
        loadedLanguages.append(v.toString());
    project.projectLanguages = ProjectLanguageSemantics::dropSfxNames(loadedLanguages);
    project.defaultLanguage = root["defaultLanguage"].toString();

    project.packBuildOutputDir = PathUtils::normalizePackBuildOutputDir(
        root["packBuildOutputDir"].toString(), project.projectPath);
    if (!PathUtils::isValidPackBuildOutputDir(project.packBuildOutputDir, project.projectPath, true)) {
        lastError_ = tr("Pack output must use a valid directory path distinct from the project root.");
        return false;
    }
    if (!PathUtils::isValidPackBuildOutputDir(project.packBuildOutputDir, project.projectPath))
        warnings_.append(PathUtils::packBuildOutputLocationError());

    project.buildExportProjectData = root["buildExportProjectData"].toBool(false);
    project.buildExportPackMetadata = root["buildExportPackMetadata"].toBool(false);
    project.buildExportWaveforms = root["buildExportWaveforms"].toBool(false);
    project.buildWaveformOutputDir = PathUtils::normalizePackBuildOutputDir(
        root["buildWaveformOutputDir"].toString(), project.projectPath);
    const QString waveformOutputRoot = PathUtils::waveformBuildOutputRoot(
        project.projectPath, project.buildWaveformOutputDir);
    if (!PathUtils::isValidPackBuildOutputDir(waveformOutputRoot, project.projectPath, true)
        || PathUtils::isProjectObjectDirectory(waveformOutputRoot, project.projectPath)) {
        lastError_ = tr("Waveform output must use a valid directory outside the project root and object directories.");
        return false;
    }
    if (!PathUtils::isValidWaveformBuildOutputDir(project.buildWaveformOutputDir, project.projectPath))
        warnings_.append(tr("The stored waveform output directory is no longer permitted. "
            "Correct it in Project Settings > Publish before saving. ")
            + PathUtils::packBuildOutputLocationError());

    project.deploymentProfiles.clear();
    if (root.contains("deploymentProfiles")) {
        for (const QJsonValue& value : root["deploymentProfiles"].toArray()) {
            const QJsonObject row = value.toObject();
            ProjectData::DeploymentProfile profile;
            profile.profileId = row["profileId"].toString();
            profile.name = row["name"].toString();
            profile.engineKind = row["engineKind"].toString();
            profile.destination = row["destination"].toString();
            for (const QJsonValue& platform : row["platformIds"].toArray())
                profile.platformIds.append(platform.toString());
            for (const QJsonValue& language : row["languages"].toArray())
                profile.languages.append(language.toString());
            profile.deployPacks = row["deployPacks"].toBool(true);
            profile.deployManifest = row["deployManifest"].toBool(true);
            profile.deployCurrentWorld = row["deployCurrentWorld"].toBool(false);
            profile.autoDeployAfterBuild = row["autoDeployAfterBuild"].toBool(false);
            if (!profile.profileId.isEmpty() && !profile.engineKind.isEmpty())
                project.deploymentProfiles.push_back(std::move(profile));
        }
    }
    project.actionPublishedNameHistory.clear();
    if (root.contains(QLatin1String(kActionPublishedNameHistory))) {
        for (const auto& value : root[QLatin1String(kActionPublishedNameHistory)].toArray()) {
            if (!value.isObject()) continue;
            ActionPublishedNameHistoryRow row;
            if (actionPublishedNameHistoryRowFromJson(value.toObject(), &row))
                project.actionPublishedNameHistory.push_back(std::move(row));
        }
    }

    project.nextObjectID = nextTransientObjectId_;

    destinationProject = std::move(project);
    emit progressChanged(100, tr("Project loaded successfully"));
    return true;
}


QJsonObject ProjectSerializer::projectItemToJson(const ProjectItem& item, bool includeChildren) const
{
    QJsonObject json;
    if (!writeFieldGroups(item, json)) return {};
    if (!item.extensions.isEmpty())
        json[QStringLiteral("extensions")] = item.extensions;
    json["name"] = item.name;
    json["type"] = ItemTypeCatalog::canonicalName(item.type);
    json["expanded"] = item.expanded;

    writeActionNameBindingRevision(json, item);

    json["guid"] = item.guid.toString(QUuid::WithoutBraces);
    json["objectPath"] = item.objectPath;

    json["parentRef"] = persistentParentReference(item);
    if (json.value(QStringLiteral("parentRef")).toObject().isEmpty()) return {};

    if (!item.notes.isEmpty()) {
        json["notes"] = item.notes;
    }

    if (item.color != 0) {
        json["color"] = static_cast<qint64>(item.color);
    }

    if (!PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item.type))
        json["outputBusOverride"] = item.outputBusOverride;

    if (item.canvasPos.x() != 0.0 || item.canvasPos.y() != 0.0)
    {
        QJsonObject pos;
        pos["x"] = item.canvasPos.x();
        pos["y"] = item.canvasPos.y();
        json["canvasPos"] = pos;
    }

    QJsonObject compsObj;
    const auto& excluded = excludedComponentFields(item.type);
    item.components.forEach([&](const IComponent& c) {
        const auto name = componentTypeName(c.getType());
        const auto fields = excluded.constFind(name);
        QJsonObject cj = fields == excluded.cend() ? c.toJson() : c.toJsonExcluding(*fields);
        if (!cj.isEmpty()) compsObj[name] = std::move(cj);
    });
    if (!encodeComponentReferences(&compsObj)) return {};
    if (!compsObj.isEmpty())
        json["components"] = compsObj;

    if (PlayablePropertyAuthoringV1::isDefinitionType(item.type)) {
        QJsonObject properties = PlayablePropertyAuthoringV1::definitionToJson(item);
        if (!encodePlayableReferences(&properties, item.type == ProjectItem::Type::Bus)) return {};
        json[QLatin1String(harkenplayableproperties::studio::kEnvelopeJsonKeyV1)] =
            properties;
    }

    QJsonArray children;
    if (includeChildren)
        for (const auto& child : item.children)
        {
            const auto childJson = projectItemToJson(*child);
            if (childJson.isEmpty()) return {};
            children.append(childJson);
        }
    json["children"] = children;

    return json;
}

bool ProjectSerializer::projectItemFromJson(const QJsonObject& json, ProjectItem& item) const
{
    if (!ProjectDocument::validateFieldGroupAuthoring(json, &lastError_)) return false;
    item.extensions = json.value(QStringLiteral("extensions")).toObject();
    item.name = json["name"].toString();
    const auto type = itemTypeFromJson(json);
    if (!type) return false;
    item.type = *type;
    if (!NameValidator::validateItemName(item.name, item.type)) {
        lastError_ = QStringLiteral("Invalid object name: %1").arg(item.name);
        return false;
    }
    if (!readFieldGroups(json, item, lastError_)) return false;
    item.sourcePath.clear();
    item.expanded = json["expanded"].toBool(false);

    readActionNameBindingRevision(json, item);

    if (!json.value(QStringLiteral("guid")).isString()) return false;
    item.guid = QUuid::fromString(json["guid"].toString());
    if (item.guid.isNull()) return false;
    if (!referencesByGuid_.contains(item.guid)
        && !registerReadGuid(item.guid, item.type, item.guid, item.name))
        return false;
    const auto registered = referencesByGuid_.constFind(item.guid);
    if (registered == referencesByGuid_.cend() || registered->type != item.type)
        return false;
    item.objectID = registered->objectId;
    item.objectPath = json["objectPath"].toString();

    if (json.contains("parentRef")) {
        if (!loadParentReference(json["parentRef"].toObject(), &item.parentRef)) {
            qWarning() << "[ProjectSerializer] Invalid parentRef for item"
                       << item.name << "-- rejecting.";
            return false;
        }
    } else return false;

    item.notes = json.value("notes").toString();
    item.color = static_cast<uint32_t>(json.value("color").toInteger(0));

    item.outputBusOverride = json["outputBusOverride"].toBool();

    if (json.contains("canvasPos"))
    {
        const QJsonObject pos = json["canvasPos"].toObject();
        item.canvasPos = QPointF(pos["x"].toDouble(0.0), pos["y"].toDouble(0.0));
    }

    const QLatin1String propertiesKey(
        harkenplayableproperties::studio::kEnvelopeJsonKeyV1);
    QJsonObject properties = json.value(propertiesKey).toObject();
    QJsonObject compsObj = json.value(QStringLiteral("components")).toObject();
    if (PlayablePropertyAuthoringV1::isDefinitionType(item.type)
        && !json.value(propertiesKey).isObject())
        return false;

    item.attachDefaultComponents();
    if (auto* spatial = item.comp<VSpatial>()) {
        spatial->spatializationMix = 1.0f;
        spatial->spatializationMixPresent = false;
    }
    if (json.contains("components")) {
        if (!json.value("components").isObject()) return false;
        if (!decodeComponentReferences(&compsObj)) return false;
        if (hasPlayableDefinitionComponentAuthority(item.type, compsObj))
            return false;
        bool componentsValid = true;
        item.components.forEach([&](IComponent& c) {
            QString key = componentTypeName(c.getType());
            if (compsObj.contains(key)) {
                if (!compsObj.value(key).isObject()
                    || !c.fromJson(compsObj[key].toObject()))
                    componentsValid = false;
            }
        });
        if (!componentsValid) return false;
    }
    if (PlayablePropertyAuthoringV1::isDefinitionType(item.type)) {
        if (!decodePlayableReferences(&properties, item.type == ProjectItem::Type::Bus)
            || !PlayablePropertyAuthoringV1::definitionFromJson(
                properties, item))
            return false;
    }
    if (item.type == ProjectItem::Type::Bus)
        if (auto* mix = item.comp<VMixNode>())
            mix->outputBusId = 0;

    QJsonArray children = json["children"].toArray();
    for (const QJsonValue& val : children)
    {
        ProjectItem childItem("", ProjectItem::Type::None);
        if (!projectItemFromJson(val.toObject(), childItem)) return false;
        auto childPtr = std::make_shared<ProjectItem>(std::move(childItem));
        childPtr->components.forEach([&childPtr](IComponent& c) { c.setHostItem(childPtr.get()); });
        childPtr->parent = &item;
        item.children.push_back(childPtr);

        std::function<void(ProjectItem*)> fixChildParents = [&fixChildParents](ProjectItem* parent) {
            for (auto& grandChild : parent->children) {
                grandChild->parent = parent;
                fixChildParents(grandChild.get());
            }
        };
        fixChildParents(childPtr.get());
    }

    if (json.contains(QStringLiteral("childOccurrences"))) return false;

    return true;
}


QJsonObject ProjectSerializer::audioItemToJson(const ProjectItem& layer) const
{
    const bool isBucketRoot =
        ItemTypeCatalog::bucketOf(layer.type) == ItemTypeCatalog::Bucket::Layers;
    if (!isBucketRoot)
    {
        qWarning() << "[AudioPack] audioItemToJson called on non-Layers-bucket type:"
                   << layer.name << "type=" << static_cast<int>(layer.type);
        return {};
    }
    return audioBranchToJson(layer);
}

bool ProjectSerializer::audioItemFromJson(const QJsonObject& json, ProjectItem& layer) const
{
    return audioBranchFromJson(json, layer);
}

bool ProjectSerializer::audioPackItemFromJson(
    const QJsonObject& json,
    ProjectItem& layer) const
{
    return audioBranchFromJson(json, layer);
}

QJsonObject ProjectSerializer::audioBranchToJson(const ProjectItem& node) const
{
    auto json = projectItemToJson(node, false);
    if (json.isEmpty()) return {};
    json.remove(QLatin1String(kNameBindingRevision));

    QJsonArray children;
    for (const auto& child : node.children)
    {
        if (!child) continue;
        if (ItemTypeCatalog::bucketOf(child->type) == ItemTypeCatalog::Bucket::Layers)
            continue;
        if (!ItemTypeCatalog::canBeChildOf(child->type, node.type))
        {
            qWarning() << "[AudioPack] Child" << child->name
                       << "type=" << static_cast<int>(child->type)
                       << "is not allowed under" << node.name
                       << "type=" << static_cast<int>(node.type);
            return {};
        }
        const QJsonObject childJson = audioBranchToJson(*child);
        if (childJson.isEmpty()) return {};
        children.append(childJson);
    }
    json["children"] = children;

    return json;
}

bool ProjectSerializer::audioBranchFromJson(
    const QJsonObject& json,
    ProjectItem& node) const
{
    if (!ProjectDocument::validateFieldGroupAuthoring(json, &lastError_)) return false;
    node.extensions = json.value(QStringLiteral("extensions")).toObject();
    node.name     = json["name"].toString();
    const auto type = itemTypeFromJson(json);
    if (!type) return false;
    node.type     = *type;
    if (!NameValidator::validateItemName(node.name, node.type)) {
        lastError_ = QStringLiteral("Invalid object name: %1").arg(node.name);
        return false;
    }
    if (!readFieldGroups(json, node, lastError_)) return false;
    node.sourcePath.clear();
    node.expanded = json["expanded"].toBool(false);

    if (!json.value(QStringLiteral("guid")).isString())
        return false;
    node.guid = QUuid::fromString(json["guid"].toString());
    if (node.guid.isNull())
        return false;
    if (!referencesByGuid_.contains(node.guid)
        && !registerReadGuid(node.guid, node.type, node.guid, node.name))
        return false;
    const auto registered = referencesByGuid_.constFind(node.guid);
    if (registered == referencesByGuid_.cend() || registered->type != node.type)
        return false;
    node.objectID = registered->objectId;
    node.objectPath = json["objectPath"].toString();
    if (json.contains("parentRef")) {
        if (!loadParentReference(json["parentRef"].toObject(), &node.parentRef)) {
            qWarning() << "[AudioPack] Invalid parentRef for item"
                       << node.name << "-- rejecting.";
            return false;
        }
    } else return false;
    node.notes = json.value("notes").toString();
    node.color = static_cast<uint32_t>(json.value("color").toInteger(0));

    node.outputBusOverride = json["outputBusOverride"].toBool();

    if (json.contains("canvasPos"))
    {
        const QJsonObject pos = json["canvasPos"].toObject();
        node.canvasPos = QPointF(pos["x"].toDouble(0.0), pos["y"].toDouble(0.0));
    }

    const QLatin1String propertiesKey(
        harkenplayableproperties::studio::kEnvelopeJsonKeyV1);
    QJsonObject properties = json.value(propertiesKey).toObject();
    QJsonObject compsObj = json.value(QStringLiteral("components")).toObject();
    if (PlayablePropertyAuthoringV1::isDefinitionType(node.type)
        && !json.value(propertiesKey).isObject())
        return false;

    node.attachDefaultComponents();
    if (auto* spatial = node.comp<VSpatial>()) {
        spatial->spatializationMix = 1.0f;
        spatial->spatializationMixPresent = false;
    }
    if (json.contains("components"))
    {
        if (!json.value("components").isObject()) return false;
        if (!decodeComponentReferences(&compsObj)) return false;
        if (hasPlayableDefinitionComponentAuthority(node.type, compsObj))
            return false;
        bool componentsValid = true;
        node.components.forEach([&](IComponent& c) {
            QString key = componentTypeName(c.getType());
            if (compsObj.contains(key)) {
                if (!compsObj.value(key).isObject()
                    || !c.fromJson(compsObj[key].toObject()))
                    componentsValid = false;
            }
        });
        if (!componentsValid) return false;
    }
    if (PlayablePropertyAuthoringV1::isDefinitionType(node.type)) {
        if (!decodePlayableReferences(&properties, node.type == ProjectItem::Type::Bus)
            || !PlayablePropertyAuthoringV1::definitionFromJson(
                properties, node))
            return false;
    }
    if (node.type == ProjectItem::Type::Bus)
        if (auto* mix = node.comp<VMixNode>())
            mix->outputBusId = 0;

    const QJsonArray children = json["children"].toArray();
    for (const QJsonValue& val : children)
    {
        ProjectItem childItem("", ProjectItem::Type::None);
        if (!audioBranchFromJson(val.toObject(), childItem)) return false;
        if (childItem.type == ProjectItem::Type::Layer)
        {
            qWarning() << "[AudioPack] Nested Layer entry inside"
                       << node.name << "subtree (boundary violation)";
            return false;
        }
        if (!ItemTypeCatalog::canBeChildOf(childItem.type, node.type))
        {
            qWarning() << "[AudioPack] Child" << childItem.name
                       << "type=" << static_cast<int>(childItem.type)
                       << "is not allowed under" << node.name
                       << "type=" << static_cast<int>(node.type);
            return false;
        }
        auto childPtr = std::make_shared<ProjectItem>(std::move(childItem));
        childPtr->components.forEach([&childPtr](IComponent& c) { c.setHostItem(childPtr.get()); });
        childPtr->parent = &node;
        node.children.push_back(childPtr);

        std::function<void(ProjectItem*)> fixChildParents = [&fixChildParents](ProjectItem* parent) {
            for (auto& grandChild : parent->children) {
                grandChild->parent = parent;
                fixChildParents(grandChild.get());
            }
        };
        fixChildParents(childPtr.get());
    }

    if (json.contains(QStringLiteral("childOccurrences"))) return false;

    return true;
}


QJsonObject ProjectSerializer::actionPackToJson(const ProjectItem& event) const
{
    if (event.type != ProjectItem::Type::Action)
    {
        qWarning() << "[ActionPack] actionPackToJson called on non-Action:" << event.name
                   << "type=" << static_cast<int>(event.type);
        return {};
    }

    QJsonObject obj;
    if (!writeFieldGroups(event, obj)) return {};
    if (!event.extensions.isEmpty())
        obj[QStringLiteral("extensions")] = event.extensions;
    obj[QStringLiteral("guid")]     = event.guid.toString(QUuid::WithoutBraces);
    obj[QStringLiteral("type")]     = QStringLiteral("Action");
    obj[QStringLiteral("name")]     = event.name;
    writeActionNameBindingRevision(obj, event);
    if (!event.objectPath.isEmpty())
        obj[QStringLiteral("objectPath")] = event.objectPath;
    obj[QStringLiteral("parentRef")] = persistentParentReference(event);
    if (obj.value(QStringLiteral("parentRef")).toObject().isEmpty()) return {};

    const auto* comp = event.comp<VAction>();
    if (!comp) {
        lastError_ = QStringLiteral("Action has no action data");
        return {};
    }
    QJsonArray actions = comp->toJson().value(QStringLiteral("steps")).toArray();
    if (!encodeActionReferences(&actions)) return {};
    obj[QStringLiteral("actions")] = actions;

    return obj;
}

bool ProjectSerializer::actionPackFromJson(const QJsonObject& json, ProjectItem& event) const
{
    if (!ProjectDocument::validateFieldGroupAuthoring(json, &lastError_)) return false;
    event.extensions = json.value(QStringLiteral("extensions")).toObject();
    if (json.value(QStringLiteral("type")).toString() != QStringLiteral("Action"))
    {
        qWarning() << "[ActionPack] Skipping non-Action payload (type="
                   << json.value(QStringLiteral("type")).toString() << ")";
        return false;
    }

    const QString name = json.value(QStringLiteral("name")).toString();
    NameValidator::Result vr;
    if (!NameValidator::validateItemName(name, ProjectItem::Type::Action, &vr))
    {
        qWarning() << "[ActionPack] Invalid action name in payload:" << name
                   << "(" << NameValidator::resultMessage(vr) << ")";
        return false;
    }

    event.name = name;
    event.type = ProjectItem::Type::Action;
    if (!json.value(QStringLiteral("guid")).isString())
        return false;
    event.guid = QUuid::fromString(json.value(QStringLiteral("guid")).toString());
    if (event.guid.isNull())
        return false;
    readActionNameBindingRevision(json, event);
    if (!referencesByGuid_.contains(event.guid)
        && !registerReadGuid(event.guid, event.type, event.guid, event.name))
        return false;
    const auto registered = referencesByGuid_.constFind(event.guid);
    if (registered == referencesByGuid_.cend()
        || registered->type != ProjectItem::Type::Action)
        return false;
    event.objectID = registered->objectId;
    event.objectPath = json.value(QStringLiteral("objectPath")).toString();

    if (json.contains(QStringLiteral("parentRef")))
    {
        if (!loadParentReference(json.value(QStringLiteral("parentRef")).toObject(),
                                 &event.parentRef))
        {
            qWarning() << "[ActionPack] Invalid parentRef for" << event.name
                       << "-- rejecting.";
            return false;
        }
    }
    else
        return false;

    event.attachDefaultComponents();
    if (auto* comp = event.comp<VAction>())
    {
        QJsonObject compJson;
        if (json.contains(QStringLiteral("actions"))) {
            QJsonArray actions = json.value(QStringLiteral("actions")).toArray();
            if (!decodeActionReferences(&actions)) return false;
            compJson[QStringLiteral("steps")] = actions;
        }

        comp->fromJson(compJson);
    }

    return true;
}


namespace {

void writeS3RootIdentity(QJsonObject& obj, const ProjectItem& item)
{
    if (!item.extensions.isEmpty())
        obj[QStringLiteral("extensions")] = item.extensions;
    obj[QStringLiteral("guid")]       = item.guid.toString(QUuid::WithoutBraces);
    obj[QStringLiteral("type")]       = ItemTypeCatalog::canonicalName(item.type);
    obj[QStringLiteral("name")]       = item.name;
    obj[QStringLiteral("expanded")]   = item.expanded;
    obj[QStringLiteral("objectPath")] = item.objectPath;
    if (!item.notes.isEmpty())
        obj[QStringLiteral("notes")] = item.notes;
    if (item.color != 0)
        obj[QStringLiteral("color")] = static_cast<qint64>(item.color);
}

bool readS3RootIdentity(const QJsonObject& json, ProjectItem& item, const char* tag)
{
    item.extensions = json.value(QStringLiteral("extensions")).toObject();
    const QString name = json.value(QStringLiteral("name")).toString();
    NameValidator::Result vr;
    if (!NameValidator::validateItemName(name, item.type, &vr))
    {
        qWarning() << "[S3Pack]" << tag << "Invalid name in payload:" << name
                   << "(" << NameValidator::resultMessage(vr) << ")";
        return false;
    }

    item.name     = name;
    item.expanded = json.value(QStringLiteral("expanded")).toBool(false);
    if (!json.value(QStringLiteral("guid")).isString())
        return false;
    item.guid = QUuid::fromString(json.value(QStringLiteral("guid")).toString());
    if (item.guid.isNull())
        return false;
    item.objectPath = json.value(QStringLiteral("objectPath")).toString();
    item.notes = json.value(QStringLiteral("notes")).toString();
    item.color = static_cast<uint32_t>(json.value(QStringLiteral("color")).toInteger(0));
    return true;
}

QJsonObject componentsToJsonDict(const ProjectItem& item)
{
    QJsonObject compsObj;
    item.components.forEach([&](const IComponent& c) {
        QJsonObject cj = c.toJson();
        if (!cj.isEmpty()) compsObj[componentTypeName(c.getType())] = cj;
    });
    return compsObj;
}

void readComponentsFromJsonDict(const QJsonObject& compsObj, ProjectItem& item)
{
    item.components.forEach([&](IComponent& c) {
        const QString key = componentTypeName(c.getType());
        if (compsObj.contains(key))
            c.fromJson(compsObj.value(key).toObject());
    });
}

}

QJsonObject ProjectSerializer::attenuationPackToJson(const ProjectItem& item) const
{
    if (item.type != ProjectItem::Type::Attenuation)
    {
        qWarning() << "[AttenuationPack] called on non-Attenuation:"
                   << item.name << "type=" << static_cast<int>(item.type);
        return {};
    }

    QJsonObject obj;
    if (!writeFieldGroups(item, obj)) return {};
    writeS3RootIdentity(obj, item);
    obj[QStringLiteral("parentRef")] = persistentParentReference(item);
    if (obj.value(QStringLiteral("parentRef")).toObject().isEmpty()) return {};

    QJsonObject compsObj = componentsToJsonDict(item);
    if (!encodeComponentReferences(&compsObj)) return {};
    if (!compsObj.isEmpty())
        obj[QStringLiteral("components")] = compsObj;

    return obj;
}

bool ProjectSerializer::attenuationPackFromJson(const QJsonObject& json, ProjectItem& item) const
{
    if (!ProjectDocument::validateFieldGroupAuthoring(json, &lastError_)) return false;
    const QString storedType = json.value(QStringLiteral("type")).toString();
    if (storedType != QStringLiteral("Attenuation"))
    {
        qWarning() << "[AttenuationPack] Skipping wrong-typed payload (type="
                   << storedType << ")";
        return false;
    }
    item.type = ProjectItem::Type::Attenuation;
    if (!readS3RootIdentity(json, item, "Attenuation")) return false;
    if (item.guid.isNull()) return false;
    if (!referencesByGuid_.contains(item.guid)
        && !registerReadGuid(item.guid, item.type, item.guid, item.name))
        return false;
    const auto registered = referencesByGuid_.constFind(item.guid);
    if (registered == referencesByGuid_.cend() || registered->type != item.type)
        return false;
    item.objectID = registered->objectId;
    if (!json.value(QStringLiteral("parentRef")).isObject()
        || !loadParentReference(json.value(QStringLiteral("parentRef")).toObject(),
                                &item.parentRef))
        return false;

    item.attachDefaultComponents();
    if (json.contains(QStringLiteral("components")))
    {
        QJsonObject components = json.value(QStringLiteral("components")).toObject();
        if (!decodeComponentReferences(&components)) return false;
        const auto key = componentTypeName(ComponentType::Attenuation);
        if (components.contains(key)
            && !item.comp<VAttenuation>()->fromJson(components.value(key).toObject())) {
            lastError_ = QStringLiteral("Invalid Attenuation fields");
            return false;
        }
    }

    item.children.clear();
    return true;
}

QJsonObject ProjectSerializer::variablePackToJson(const ProjectItem& item) const
{
    if (item.type != ProjectItem::Type::Variable)
    {
        qWarning() << "[VariablePack] called on non-Variable:"
                   << item.name << "type=" << static_cast<int>(item.type);
        return {};
    }

    QJsonObject obj;
    if (!writeFieldGroups(item, obj)) return {};
    writeS3RootIdentity(obj, item);
    obj[QStringLiteral("parentRef")] = persistentParentReference(item);
    if (obj.value(QStringLiteral("parentRef")).toObject().isEmpty()) return {};

    QJsonObject compsObj = componentsToJsonDict(item);
    if (!encodeComponentReferences(&compsObj)) return {};
    if (!compsObj.isEmpty())
        obj[QStringLiteral("components")] = compsObj;

    return obj;
}

bool ProjectSerializer::variablePackFromJson(const QJsonObject& json, ProjectItem& item) const
{
    if (!ProjectDocument::validateFieldGroupAuthoring(json, &lastError_)) return false;
    const QString storedType = json.value(QStringLiteral("type")).toString();
    if (storedType != QStringLiteral("Variable"))
    {
        qWarning() << "[VariablePack] Skipping wrong-typed payload (type="
                   << storedType << ")";
        return false;
    }
    item.type = ProjectItem::Type::Variable;
    if (!readS3RootIdentity(json, item, "Variable")) return false;
    if (item.guid.isNull()) return false;
    if (!referencesByGuid_.contains(item.guid)
        && !registerReadGuid(item.guid, item.type, item.guid, item.name))
        return false;
    const auto registered = referencesByGuid_.constFind(item.guid);
    if (registered == referencesByGuid_.cend() || registered->type != item.type)
        return false;
    item.objectID = registered->objectId;
    if (!json.value(QStringLiteral("parentRef")).isObject()
        || !loadParentReference(json.value(QStringLiteral("parentRef")).toObject(),
                                &item.parentRef))
        return false;

    item.attachDefaultComponents();
    if (json.contains(QStringLiteral("components")))
    {
        QJsonObject components = json.value(QStringLiteral("components")).toObject();
        if (!decodeComponentReferences(&components)) return false;
        auto variable = components.value(QStringLiteral("variable")).toObject();
        auto labels = variable.value(QStringLiteral("labels")).toArray();
        for (int i = 0; i < labels.size(); ++i) {
            auto label = labels[i].toObject();
            if (!label.contains(QStringLiteral("id"))) {
                label.insert(QStringLiteral("id"), QUuid::createUuidV5(item.guid,
                    QByteArray("variable-label-") + QByteArray::number(i)).toString(QUuid::WithoutBraces));
                labels[i] = label;
            }
        }
        if (!labels.isEmpty()) {
            variable.insert(QStringLiteral("labels"), labels);
            components.insert(QStringLiteral("variable"), variable);
        }
        readComponentsFromJsonDict(components, item);
    }

    item.children.clear();
    return true;
}


QJsonObject ProjectSerializer::busPackToJson(
    const ProjectItem& busItem) const
{
    const auto* bc = busItem.comp<VNX::VBus>();
    if (!bc)
    {
        qWarning() << "[BusPack] Bus ProjectItem missing VBus:"
                   << busItem.name;
        return {};
    }

    QJsonObject root;
    if (!writeFieldGroups(busItem, root)) return {};
    if (!busItem.extensions.isEmpty())
        root[QStringLiteral("extensions")] = busItem.extensions;
    root[QStringLiteral("type")]       = QStringLiteral("Bus");
    root[QStringLiteral("guid")]       = busItem.guid.toString(QUuid::WithoutBraces);
    root[QStringLiteral("name")]       = busItem.name;

    root[QStringLiteral("parentRef")] = persistentParentReference(busItem);
    if (root.value(QStringLiteral("parentRef")).toObject().isEmpty()) return {};

    root[QStringLiteral("componentData")] = bc->toJson();

    QJsonObject compsObj = componentsToJsonDict(busItem);
    if (!encodeComponentReferences(&compsObj)) return {};
    stripPlayableDefinitionComponentAuthority(busItem.type, compsObj);
    if (!compsObj.isEmpty())
        root[QStringLiteral("components")] = compsObj;
    if (PlayablePropertyAuthoringV1::isDefinitionType(busItem.type)) {
        QJsonObject properties =
            PlayablePropertyAuthoringV1::definitionToJson(busItem);
        if (!encodePlayableReferences(&properties, true)) return {};
        root[QLatin1String(harkenplayableproperties::studio::kEnvelopeJsonKeyV1)] =
            properties;
    }

    return root;
}

ProjectItem* ProjectSerializer::busPackFromJson(
    const QJsonObject& json,
    ProjectItem& bucketParent,
    std::unordered_map<uint32_t, uint32_t>& outParentEdges) const
{
    if (!ProjectDocument::validateFieldGroupAuthoring(json, &lastError_)) return nullptr;
    if (json.value(QStringLiteral("type")).toString() != QStringLiteral("Bus"))
    {
        qWarning() << "[BusPack] Skipping non-Bus payload (type="
                   << json.value(QStringLiteral("type")).toString() << ")";
        return nullptr;
    }

    if (!json.contains(QStringLiteral("componentData"))
        || !json.value(QStringLiteral("componentData")).isObject())
    {
        qWarning() << "[BusPack] Missing componentData in payload.";
        return nullptr;
    }

    auto item = std::make_shared<ProjectItem>(
        QString(), ProjectItem::Type::Bus);
    item->extensions = json.value(QStringLiteral("extensions")).toObject();
    item->attachDefaultComponents();

    VNX::VBus* bc = item->compOrCreate<VNX::VBus>();
    if (bc)
        bc->fromJson(json.value(QStringLiteral("componentData")).toObject());

    if (!json.value(QStringLiteral("guid")).isString())
        return nullptr;
    item->guid = QUuid::fromString(json.value(QStringLiteral("guid")).toString());
    if (item->guid.isNull())
        return nullptr;
    item->name = json.value(QStringLiteral("name")).toString(item->name);
    const auto registered = referencesByGuid_.constFind(item->guid);
    if (registered == referencesByGuid_.cend()
        || registered->type != ProjectItem::Type::Bus)
        return nullptr;
    const uint32_t objId = registered->objectId;
    item->objectID = objId;

    const QJsonObject parentRef = json.value(QStringLiteral("parentRef")).toObject();
    ParentRef ref;
    if (!loadParentReference(parentRef, &ref))
    {
        qWarning() << "[BusPack] Unknown parent_ref.kind '"
                   << parentRef.value(QStringLiteral("kind")).toString()
                   << "' for Bus" << item->name << "-- rejecting.";
        return nullptr;
    }
    item->parentRef = ref;
    if (ref.kind == ParentRef::Kind::Bus)
    {
        const uint32_t parentId = transientObjectId(ref.guid);
        if (parentId == 0 || parentId == objId) return nullptr;
        outParentEdges[objId] = parentId;
    }
    else if (!ref.isRoot())
    {
        qWarning() << "[BusPack] Unexpected parent_ref.kind for Bus" << item->name
                   << "-- rejecting.";
        return nullptr;
    }
    if (json.contains(QStringLiteral("components"))) {
        if (!json.value(QStringLiteral("components")).isObject())
            return nullptr;
        QJsonObject compsObj = json.value(QStringLiteral("components")).toObject();
        if (!decodeComponentReferences(&compsObj)) return nullptr;
        if (hasPlayableDefinitionComponentAuthority(item->type, compsObj)) {
            qWarning() << "[BusPack] Playable fields remain in components for Bus"
                       << item->name << "-- rejecting.";
            return nullptr;
        }
        readComponentsFromJsonDict(compsObj, *item);
    }
    if (PlayablePropertyAuthoringV1::isDefinitionType(item->type)) {
        const QLatin1String key(
            harkenplayableproperties::studio::kEnvelopeJsonKeyV1);
        QJsonObject properties = json.value(key).toObject();
        if (!json.value(key).isObject()
            || !decodePlayableReferences(&properties, true)
            || !PlayablePropertyAuthoringV1::definitionFromJson(
                properties, *item))
            return nullptr;
    }
    if (auto* mix = item->comp<VMixNode>())
        mix->outputBusId = 0;

    item->parent = &bucketParent;
    bucketParent.children.push_back(item);

    return item.get();
}

}
