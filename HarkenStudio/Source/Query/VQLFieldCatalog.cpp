#include "VQLFieldCatalog.h"

#include "../Core/Component.h"
#include "../Core/ItemTypeCatalog.h"
#include "../Core/ProjectTypes.h"
#include "../Core/LanguageContextService.h"
#include "../Core/PlatformContextService.h"
#include "../Core/PropertyDescriptor.h"
#include "../Core/SoundSourceEditing.h"
#include "../Core/Components/VAction.h"
#include "../Core/Components/VAttenuation.h"
#include "../Core/Components/VAudio.h"
#include "../Core/Components/VBus.h"
#include "../Core/Components/VMixNode.h"
#include "../Core/Components/VSelection.h"
#include "../Core/Components/VSound.h"
#include "../Core/Components/VSpatial.h"
#include "../Core/Components/VVariable.h"

#include <array>

namespace VNX {
namespace VQLFieldCatalog {
namespace {

using Reader = VQLValue (*)(const ProjectItem&, const ProjectData*);

struct FieldDescriptor
{
    const char* name;
    const char* outputName;
    Reader read;
};

VQLValue name(const ProjectItem& item, const ProjectData*) { return item.name; }
VQLValue type(const ProjectItem& item, const ProjectData*) { return ItemTypeCatalog::canonicalName(item.type); }
VQLValue objectId(const ProjectItem& item, const ProjectData*) { return static_cast<double>(item.objectID); }
VQLValue sourceId(const ProjectItem& item, const ProjectData*)
{
    return static_cast<double>(SoundSourceEditing::activeSourceId(
        item, PlatformContextService::instance().context().primary,
        SoundSourceEditing::previewLanguage(
            item, LanguageContextService::instance().language())));
}
VQLValue sourceCount(const ProjectItem& item, const ProjectData*)
{
    const auto* sound = item.comp<VSound>();
    if (!sound) return 0.0;
    if (!sound->distinguishLanguage)
        return static_cast<double>(sound->sources.plain.entries.size());
    size_t count = 0;
    for (const auto& set : sound->sources.localized) count += set.entries.size();
    return static_cast<double>(count);
}
VQLValue activeSourceLanguage(const ProjectItem& item, const ProjectData*)
{
    const SoundSourceDef* source = SoundSourceEditing::activeSource(
        item, PlatformContextService::instance().context().primary,
        SoundSourceEditing::previewLanguage(
            item, LanguageContextService::instance().language()));
    return source ? SoundSourceEditing::previewLanguage(
        item, LanguageContextService::instance().language()) : QString{};
}
VQLValue loopCount(const ProjectItem& item, const ProjectData*)
{
    const auto* audio = item.comp<VAudio>();
    return static_cast<double>(audio ? audio->loopCount : 0);
}
VQLValue scheduleOffsetSec(const ProjectItem& item, const ProjectData*)
{
    const auto* sound = item.comp<VSound>();
    return sound ? sound->scheduleOffsetSec : 0.0;
}
VQLValue childCount(const ProjectItem& item, const ProjectData*)
{
    return static_cast<double>(item.children.size());
}
VQLValue path(const ProjectItem& item, const ProjectData* projectData)
{
    if (item.type == ProjectItem::Type::Sound) return item.sourcePath;
    return projectData && !item.parent ? projectData->itemFolderPath(&item) : QString{};
}
VQLValue isContentObject(const ProjectItem& item, const ProjectData*)
{
    return ItemTypeCatalog::isContentObject(item.type);
}
VQLValue hasMixingCapability(const ProjectItem& item, const ProjectData*)
{
    return ItemTypeCatalog::hasMixingCapability(item.type);
}
VQLValue hasSpatialCapability(const ProjectItem& item, const ProjectData*)
{
    return ItemTypeCatalog::hasSpatialCapability(item.type);
}
VQLValue isTriggerable(const ProjectItem& item, const ProjectData*)
{
    return ItemTypeCatalog::isTriggerable(item.type);
}

constexpr std::array<FieldDescriptor, 14> kFields = {{
    {"name", "name", name},
    {"type", "type", type},
    {"objectid", "objectID", objectId},
    {"sourceid", "sourceID", sourceId},
    {"sourcecount", "sourceCount", sourceCount},
    {"activesourcelanguage", "activeSourceLanguage", activeSourceLanguage},
    {"loopcount", "loopCount", loopCount},
    {"scheduleoffsetsec", "scheduleOffsetSec", scheduleOffsetSec},
    {"childcount", "childCount", childCount},
    {"path", "path", path},
    {"iscontentobject", "isContentObject", isContentObject},
    {"hasmixingcapability", "hasMixingCapability", hasMixingCapability},
    {"hasspatialcapability", "hasSpatialCapability", hasSpatialCapability},
    {"istriggerable", "isTriggerable", isTriggerable},
}};

const FieldDescriptor* builtinField(const QString& fieldName)
{
    for (const FieldDescriptor& field : kFields) {
        if (fieldName.compare(QLatin1String(field.name), Qt::CaseInsensitive) == 0)
            return &field;
    }
    return nullptr;
}

const PropertyDescriptor* findInSchema(const PropertySchema& schema,
                                       const QString& fieldName)
{
    for (const PropertyDescriptor& descriptor : schema) {
        if (descriptor.field == fieldName)
            return &descriptor;
        for (const QString& alias : descriptor.aliases) {
            if (alias == fieldName)
                return &descriptor;
        }
    }
    return nullptr;
}

const PropertyDescriptor* componentField(const QString& fieldName)
{
    const std::array<const PropertySchema*, 8> schemas = {{
        &VMixNode::schema(), &VAudio::schema(), &VSelection::schema(),
        &VBus::schema(), &VAction::schema(), &VVariable::schema(),
        &VAttenuation::schema(), &VSpatial::schema(),
    }};
    for (const PropertySchema* schema : schemas) {
        if (const PropertyDescriptor* descriptor = findInSchema(*schema, fieldName))
            return descriptor;
    }
    return nullptr;
}

VQLValue variantToValue(const QVariant& value)
{
    if (value.isNull()) return {};
    switch (value.typeId()) {
    case QMetaType::Bool:    return value.toBool();
    case QMetaType::Int:     return static_cast<double>(value.toInt());
    case QMetaType::Float:   return static_cast<double>(value.toFloat());
    case QMetaType::Double:  return value.toDouble();
    case QMetaType::QString: return value.toString();
    default:
        if (value.canConvert<double>()) return value.toDouble();
        if (value.canConvert<QString>()) return value.toString();
        return {};
    }
}

}

bool isKnownField(const QString& fieldName)
{
    return builtinField(fieldName) || componentField(fieldName);
}

bool project(const ProjectItem* item, const QString& fieldName,
             QString* outputName, VQLValue* value, const ProjectData* projectData)
{
    if (!item || !value) return false;

    if (const FieldDescriptor* field = builtinField(fieldName)) {
        if (outputName) *outputName = QLatin1String(field->outputName);
        *value = field->read(*item, projectData);
        return true;
    }

    const PropertyDescriptor* descriptor = componentField(fieldName);
    if (!descriptor) return false;

    QVariant componentValue;
    item->components.forEach([&](const IComponent& component) {
        if (componentValue.isNull())
            componentValue = component.getField(fieldName);
    });
    if (outputName) *outputName = fieldName;
    *value = variantToValue(componentValue);
    return true;
}

}
}
