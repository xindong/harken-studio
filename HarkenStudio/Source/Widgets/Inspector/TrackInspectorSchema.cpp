
#include "TrackInspectorSchema.h"
#include "ContentObjectSchemaRows.h"
#include "Core/Components/VTrack.h"

namespace VNX {

#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#  pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif

const PropertySchema& trackInspectorSchema()
{
    static const PropertySchema s = [] {
        auto rows = contentObjectInspectorRows(ProjectItem::Type::Track);
        PropertyDescriptor clips;
        clips.field = QStringLiteral("clips");
        clips.ownerComponent = ComponentType::Track;
        clips.type = PropType::List;
        clips.label = QStringLiteral("Clips");
        clips.section = PropertySection::Clip;
        clips.flags.serialize = false;
        clips.elementSchema = {
            {.field="order", .type=PropType::Int, .label="#",
             .flags={.serialize=false, .settableByName=false, .state=true}},
            {.field="targetGuid", .type=PropType::ObjectRef, .label="Child"},
        };
        clips.getItem = [](const ProjectItem* item) -> QVariant {
            const auto* track = item ? item->comp<VTrack>() : nullptr;
            return track ? track->clips.toJson().toVariantList() : QVariant();
        };
        rows.push_back(std::move(clips));
        return rows;
    }();
    return s;
}

#if defined(__clang__)
#  pragma clang diagnostic pop
#endif

}
