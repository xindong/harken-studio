
#pragma once

#include "Core/Components/VAudio.h"
#include "Core/Components/VMixNode.h"
#include "Core/Components/VSpatial.h"
#include "Core/PlayablePropertyAuthoringV1.h"
#include "Core/FieldGroups.h"
#include "SendsFieldSchema.h"
#include "ProjectItemInspectorRows.h"
#include "Shared/SchemaFieldPick.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace VNX {

inline PropertySchema contentObjectInspectorRows(ProjectItem::Type type)
{
    PropertySchema out;
    out.reserve(std::size(VNX::PlayablePropertyAuthoringV1::fields()));
    for (const auto& field : VNX::PlayablePropertyAuthoringV1::fields()) {
        if (!PlayablePropertyAuthoringV1::isInspectorVisible(field.id))
            continue;
        const auto group = FieldGroupForProperty(
            static_cast<harkenplayableproperties::PlayablePropertyIdV1>(field.id));
        if (group ? !PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(type)
                  : !PlayablePropertyAuthoringV1::isSlotApplicable(type, field.id))
            continue;
        const PropertySchema* schema = nullptr;
        const QLatin1String component(field.component);
        if (component == QLatin1String("audio")) schema = &VAudio::schema();
        else if (component == QLatin1String("mixnode")) schema = &VMixNode::schema();
        else if (component == QLatin1String("spatial")) schema = &VSpatial::schema();
        if (!schema) continue;
        const auto* storage = pickSchemaField(
            *schema, QLatin1String(field.storageField));
        if (!storage) continue;
        PropertyDescriptor descriptor = field.wire
                == harkenplayableproperties::PlayablePropertyWireV1::RowSpan
            ? sendsInspectorDescriptor() : *storage;
        descriptor.field = QLatin1String(field.fieldName);
        descriptor.jsonKey = QLatin1String(field.jsonKey);
        descriptor.flags.sourceIndicator = false;
        auto position = out.end();
        if (field.referenceRole
            == harkenplayableproperties::PlayablePropertyReferenceRoleV1::MainOutputBus)
            descriptor.refFilter = static_cast<int>(ProjectItem::Type::Bus);
        else if (field.referenceRole
                 == harkenplayableproperties::PlayablePropertyReferenceRoleV1::Attenuation) {
            descriptor.refFilter = static_cast<int>(ProjectItem::Type::Attenuation);
            position = std::find_if(out.begin(), out.end(), [](const PropertyDescriptor& d) {
                return d.field == QLatin1String("spatialAttenuationDistanceScaling");
            });
        }
        out.insert(position, std::move(descriptor));
    }
    out.push_back(notesInspectorDescriptor());
    return out;
}

}
