
#include "SoundInspectorSchema.h"
#include "InspectorLayout.h"
#include "ContentObjectSchemaRows.h"
#include "Core/Components/VSound.h"

#include <algorithm>

namespace VNX {

namespace {

#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#  pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif
PropertyDescriptor scheduleOffsetDescriptor()
{
    return {
        .field="scheduleOffsetSec", .ownerComponent=ComponentType::Sound,
        .type=PropType::Float, .label="Offset of Start", .section=PropertySection::Playback,
        .unit=" s", .min=0.0, .decimals=3,
        .getItem=[](const ProjectItem* item) -> QVariant {
            const auto* sound = item ? item->comp<VSound>() : nullptr;
            return sound ? QVariant(sound->scheduleOffsetSec) : QVariant();
        },
    };
}

}

const PropertySchema& soundInspectorSchema()
{
    static const PropertySchema s = [] {
        PropertySchema rows = contentObjectInspectorRows(ProjectItem::Type::Sound);
        auto notes = std::find_if(rows.begin(), rows.end(), [](const PropertyDescriptor& d) {
            return d.field == QLatin1String("notes");
        });
        rows.insert(notes == rows.end() ? rows.end() : notes, scheduleOffsetDescriptor());
        PropertyDescriptor language;
        language.field = QStringLiteral("distinguishLanguage");
        language.ownerComponent = ComponentType::Sound;
        language.type = PropType::Bool;
        language.label = QStringLiteral("Language");
        language.section = PropertySection::Source;
        language.flags.serialize = false;
        language.getItem = [](const ProjectItem* item) -> QVariant {
            const auto* sound = item ? item->comp<VSound>() : nullptr;
            return sound ? QVariant(sound->distinguishLanguage) : QVariant();
        };
        rows.push_back(std::move(language));

        PropertyDescriptor sources;
        sources.field = QStringLiteral("sources");
        sources.ownerComponent = ComponentType::Sound;
        sources.type = PropType::List;
        sources.label = QStringLiteral("Sources");
        sources.section = PropertySection::Source;
        sources.flags.serialize = false;
        sources.elementSchema = {
            {.field="sourceId", .type=PropType::ObjectRef, .label="Audio"},
            {.field="range", .type=PropType::Struct, .label="Wave",
             .elementSchema={
                 {.field="trimStartFrame", .type=PropType::Int, .label="Trim Start"},
                 {.field="trimEndFrame", .type=PropType::Int, .label="Trim End"},
                 {.field="loopRegion", .type=PropType::Struct, .label="Loop Region"},
             }},
            {.field="active", .type=PropType::Bool, .label="Active",
             .flags={.serialize=false, .settableByName=false, .state=true}},
        };
        sources.getItem = [](const ProjectItem* item) -> QVariant {
            const auto* sound = item ? item->comp<VSound>() : nullptr;
            return sound ? sound->toJson().value(QStringLiteral("sources")).toVariant() : QVariant();
        };
        rows.push_back(withInspectorRowOrder(std::move(sources)));
        return rows;
    }();
    return s;
}

#if defined(__clang__)
#  pragma clang diagnostic pop
#endif

}
