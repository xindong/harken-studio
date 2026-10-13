#include "VAttenuation.h"
#include "../CurveSemantics.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace VNX {

namespace {

bool parseDimensions(const QJsonValue& value, QVector<AttenuationDimensionDef>& out)
{
    if (!value.isArray()) return false;
    for (const auto& entry : value.toArray()) {
        if (!entry.isObject()) return false;
        const auto json = entry.toObject();
        if (!json.value("points").isArray()) return false;
        for (const auto& point : json.value("points").toArray()) {
            if (!point.isObject()) return false;
            for (const auto* key : {"distance", "value"}) {
                const auto number = point.toObject().value(QLatin1String(key));
                if (!number.isDouble() || !std::isfinite(number.toDouble())
                    || std::abs(number.toDouble()) > std::numeric_limits<float>::max())
                    return false;
            }
            if (point.toObject().value("distance").toDouble() < 0.0) return false;
        }
        if (json.contains("handles")) {
            if (!json.value("handles").isArray()) return false;
            for (const auto& handle : json.value("handles").toArray()) {
                if (!handle.isArray() || handle.toArray().size() != 4) return false;
                for (const auto& number : handle.toArray())
                    if (!number.isDouble() || !std::isfinite(number.toDouble())
                        || std::abs(number.toDouble()) > std::numeric_limits<float>::max())
                        return false;
            }
        }
        AttenuationDimensionDef dimension;
        if (!AttenuationDimensionDef::fromJson(json, &dimension)
            || std::any_of(out.begin(), out.end(), [&](const auto& existing) {
                return existing.type == dimension.type;
            }))
            return false;
        out.push_back(std::move(dimension));
    }
    return true;
}

}

const AttenuationDimensionDef* VAttenuation::dimension(AttenuationDimension type) const
{
    for (const auto& entry : dimensions)
        if (entry.type == type) return &entry;
    return nullptr;
}

AttenuationSnapshot VAttenuation::snapshot() const
{
    AttenuationSnapshot result;
    if (const auto* item = hostItem()) {
        result.guid = item->guid;
        result.name = item->name;
    }
    result.maxDistance = maxDistance;
    result.dimensions = dimensions;
    return result;
}

bool VAttenuation::setDimensions(const QJsonArray& value)
{
    QVector<AttenuationDimensionDef> parsed;
    if (!parseDimensions(value, parsed)) return false;
    for (const auto& entry : parsed) {
        const auto* previous = dimension(entry.type);
        const auto previousCount = previous ? previous->curve.size() : 0;
        if (entry.curve.size() > kCurveMaxPoints && entry.curve.size() > previousCount)
            return false;
    }
    dimensions = std::move(parsed);
    return true;
}

QJsonObject VAttenuation::toJson() const
{
    QJsonArray rows;
    for (const auto& entry : dimensions) rows.append(entry.toJson());
    return {{"maxDistance", maxDistance}, {"dimensions", rows}};
}

bool VAttenuation::fromJson(const QJsonObject& json)
{
    const auto distance = json.value("maxDistance");
    if (!distance.isDouble() || !std::isfinite(distance.toDouble())
        || distance.toDouble() < 0.0
        || distance.toDouble() > std::numeric_limits<float>::max())
        return false;
    QVector<AttenuationDimensionDef> parsed;
    if (!parseDimensions(json.value("dimensions"), parsed)) return false;
    maxDistance = static_cast<float>(distance.toDouble());
    dimensions = std::move(parsed);
    return true;
}

#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#  pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif
const PropertySchema& VAttenuation::schema()
{
    static const PropertySchema s = withOwner<VAttenuation>({
        {.field="maxDistance", .type=PropType::Float, .label="Max Distance",
         .section=PropertySection::Dimension, .min=0.0, .def=100.0,
         .get=VNX_PROP_GET(VAttenuation, maxDistance),
         .set=[](IComponent* component, const QVariant& value) {
             const auto number = QJsonValue::fromVariant(value);
             if (!number.isDouble() || !std::isfinite(number.toDouble())
                 || number.toDouble() < 0.0
                 || number.toDouble() > std::numeric_limits<float>::max())
                 return false;
             static_cast<VAttenuation*>(component)->maxDistance = static_cast<float>(number.toDouble());
             return true;
         }},
        {.field="dimensions", .type=PropType::List, .label="Dimensions",
         .section=PropertySection::Dimension,
         .elementSchema={
             {.field="type", .type=PropType::Enum, .label="Property"},
             {.field="points", .type=PropType::List, .label="Curve"},
         },
         .get=[](const IComponent* component) -> QVariant {
             QJsonArray rows;
             for (const auto& entry : static_cast<const VAttenuation*>(component)->dimensions)
                 rows.append(entry.toJson());
             return rows.toVariantList();
         },
         .set=[](IComponent* component, const QVariant& value) {
             const auto json = QJsonValue::fromVariant(value);
             return json.isArray()
                 && static_cast<VAttenuation*>(component)->setDimensions(json.toArray());
         }},
    });
    return s;
}
#if defined(__clang__)
#  pragma clang diagnostic pop
#endif

}
