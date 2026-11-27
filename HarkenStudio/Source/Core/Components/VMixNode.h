
#pragma once

#include "../Component.h"
#include "../BindingSemantics.h"
#include "../EffectChainSemantics.h"
#include "../FilterSemantics.h"
#include "../PropertyDescriptor.h"
#include "../ProjectTypes.h"
#include <harken_pack/PackBus.h>
#include <harken_pack/PackProperties.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

namespace harken::studio {

class VMixNode : public ReflectedComponent<VMixNode>
{
public:
    static constexpr ComponentType kType = ComponentType::MixNode;
    ComponentType getType() const override { return kType; }

    static const PropertySchema& schema();

    static float linearToDb(float linear)
    {
        return linear <= 0.0f ? -96.0f : 20.0f * std::log10(linear);
    }

    static float dbToLinear(float db)
    {
        return std::pow(10.0f, db / 20.0f);
    }

    static QVariant routesToVariant(const std::vector<BusRoute>& routes, bool sidechain)
    {
        QVariantList out;
        for (const auto& route : routes) out.push_back(route.toJson(sidechain).toVariantMap());
        return out;
    }

    static bool routesFromVariant(const QVariant& value, bool sidechain, std::vector<BusRoute>& routes)
    {
        std::vector<BusRoute> parsed;
        QSet<QUuid> ids;
        for (const QVariant& entry : value.toList()) {
            const auto route = BusRoute::fromJson(QJsonObject::fromVariantMap(entry.toMap()), sidechain);
            if (!route || ids.contains(route->id)) return false;
            ids.insert(route->id);
            parsed.push_back(*route);
        }
        routes = std::move(parsed);
        return true;
    }

    VMixNode() = default;
    ~VMixNode() override = default;

    VMixNode(VMixNode&&) noexcept = default;
    VMixNode& operator=(VMixNode&&) noexcept = default;

    VMixNode(const VMixNode&) = default;
    VMixNode& operator=(const VMixNode&) = default;

    float volume = 0.0f;
    float pitch  = 0.0f;
    FilterState filter;

    uint32_t outputBusId     = 0;
    float    outputBusVolume = 0.0f;
    harken::studio::types::ObjectId outputBusRef{};

    std::vector<BusRoute> sends;
    std::vector<BusRoute> sidechains;

    std::vector<Send> effectiveSends(PlatformScope scope) const
    {
        std::vector<Send> result;
        for (const auto& row : sends) {
            const auto route = row.forPlatform(scope);
            result.push_back({route.targetBusID, dbToLinear(route.sendLevelDb), SendType::Send, false});
        }
        for (const auto& row : sidechains) {
            const auto route = row.forPlatform(scope);
            result.push_back({route.targetBusID, dbToLinear(route.sendLevelDb), SendType::Sidechain, route.duckTarget});
        }
        return result;
    }

    BindingLedger bindings;
    EffectChainLedger effects;

    EffectChainLedger effectsForPlatform(PlatformScope platform) const
    {
        return platform ? effects.forPlatform(*platform) : effects;
    }

    void toJsonExtra(QJsonObject& j) const override
    {
        if (!sends.empty()) j[QStringLiteral("sends")] = QJsonArray::fromVariantList(getField(QStringLiteral("sends")).toList());
        if (!sidechains.empty()) j[QStringLiteral("sidechains")] = QJsonArray::fromVariantList(getField(QStringLiteral("sidechains")).toList());
        if (!bindings.isEmpty()) j[QStringLiteral("bindings")] = bindings.toJson();
        if (!effects.isEmpty())  j[QStringLiteral("effects")]  = effects.toJson();
    }

    bool fromJson(const QJsonObject& j) override
    {
        const auto value = j.value(QStringLiteral("filter"));
        if (!value.isUndefined()
            && (!value.isObject() || !FilterSemantics::fromJson(value.toObject()))) return false;
        if (!ReflectedComponent<VMixNode>::fromJson(j)) return false;
        for (const auto* key : {"sends", "sidechains"}) {
            const auto routes = j.value(QLatin1String(key));
            if (!routes.isUndefined()
                && (!routes.isArray() || !setField(QLatin1String(key), routes.toVariant()))) return false;
        }
        return true;
    }

    void fromJsonExtra(const QJsonObject& j) override
    {
        bindings = BindingLedger::fromJson(j[QStringLiteral("bindings")].toArray());
        effects  = EffectChainLedger::fromJson(j[QStringLiteral("effects")].toArray());
    }
};

#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#  pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif
inline const PropertySchema& VMixNode::schema()
{
    static const PropertySchema s = [] {
        auto schema = withOwner<VMixNode>({
        { .field="volume", .type=PropType::Float, .label="Volume", .section=PropertySection::Playback,
          .unit=" dB", .min=HK_PACK_PROPERTY_VOLUME_MIN, .max=HK_PACK_PROPERTY_VOLUME_MAX, .decimals=1,
          .flags={.sourceIndicator=true},
          .get=VNX_PROP_GET(VMixNode, volume), .set=[](IComponent* c, const QVariant& v) {
              auto* m = static_cast<VMixNode*>(c);
              const bool bus = m->hostItem() && m->hostItem()->type == ProjectItem::Type::Bus;
              m->volume = std::clamp(v.toFloat(), bus ? HK_PACK_BUS_VOLUME_MIN : HK_PACK_PROPERTY_VOLUME_MIN,
                                     bus ? HK_PACK_BUS_VOLUME_MAX : HK_PACK_PROPERTY_VOLUME_MAX);
              return true;
          } },
        { .field="pitch", .type=PropType::Float, .label="Pitch", .section=PropertySection::Playback,
          .unit=" cent", .min=HK_PACK_PROPERTY_PITCH_MIN, .max=HK_PACK_PROPERTY_PITCH_MAX, .decimals=0,
          .flags={.sourceIndicator=true},
          .get=VNX_PROP_GET(VMixNode, pitch), .set=VNX_PROP_SET_F(VMixNode, pitch) },
        { .field="filter", .type=PropType::Struct, .label="Filter", .section=PropertySection::Playback,
          .def=FilterSemantics::toJson(FilterState{}).toVariantMap(),
          .get=[](const IComponent* component) {
              return FilterSemantics::toJson(static_cast<const VMixNode*>(component)->filter).toVariantMap();
          },
          .set=[](IComponent* component, const QVariant& value) {
              const auto json = QJsonValue::fromVariant(value);
              if (!json.isObject()) return false;
              const auto parsed = FilterSemantics::fromJson(json.toObject());
              if (!parsed) return false;
              static_cast<VMixNode*>(component)->filter = *parsed;
              return true;
          } },
        { .field="outputBusId", .type=PropType::ObjectRef, .label="Target", .section=PropertySection::Output,
          .aliases={"outputbusid"}, .refFilter=static_cast<int>(ProjectItem::Type::Bus),
          .get=[](const harken::studio::IComponent* c){
              const auto* m = static_cast<const VMixNode*>(c);
              const ProjectItem* h = m->hostItem();
              if (h && h->type == ProjectItem::Type::Bus)
              {
                  const ProjectItem* p = h->parent;
                  return QVariant(static_cast<qint64>(p ? p->objectID : 0));
              }
              return QVariant(static_cast<qint64>(m->outputBusId));
          },
          .set=[](harken::studio::IComponent* c, const QVariant& v) -> bool {
              auto* m = static_cast<VMixNode*>(c);
              const ProjectItem* h = m->hostItem();
              if (h && (h->type == ProjectItem::Type::Bus
                  || PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(h->type))) return false;
              m->outputBusId = v.toUInt();
              return true;
          },
          .serializeWhen=[](const harken::studio::IComponent* c){ return static_cast<const VMixNode*>(c)->outputBusId != 0; } },
        { .field="outputBusRef", .type=PropType::ObjectRef, .label="Target", .section=PropertySection::Output,
          .flags={.serialize=false},
          .get=[](const harken::studio::IComponent* c) { return QVariant::fromValue(
              static_cast<const VMixNode*>(c)->outputBusRef); },
          .set=[](IComponent* c, const QVariant& v) {
              auto* m = static_cast<VMixNode*>(c);
              if (m->hostItem() && PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(m->hostItem()->type)) return false;
              if (!v.canConvert<harken::studio::types::ObjectId>()) return false;
              m->outputBusRef = v.value<harken::studio::types::ObjectId>();
              return true;
          } },
        { .field="outputBusVolume", .type=PropType::Float, .label="Volume", .section=PropertySection::Output,
          .unit=" dB", .min=HK_PACK_PROPERTY_OUTPUT_BUS_VOLUME_MIN, .max=HK_PACK_PROPERTY_OUTPUT_BUS_VOLUME_MAX,
          .decimals=1, .aliases={"outputbusvolume"},
          .get=VNX_PROP_GET(VMixNode, outputBusVolume), .set=[](IComponent* c, const QVariant& v) {
              auto* m = static_cast<VMixNode*>(c);
              if (m->hostItem() && PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(m->hostItem()->type)) return false;
              m->outputBusVolume = v.toFloat();
              return true;
          },
          .serializeWhen=[](const harken::studio::IComponent* c){ return static_cast<const VMixNode*>(c)->outputBusVolume != 0.0f; } },
        { .field="sends", .type=PropType::List, .label="Sends", .section=PropertySection::Send,
          .elementSchema={
              { .field="targetBusID", .type=PropType::ObjectRef, .label="Target", .def=0u,
                .refFilter=static_cast<int>(ProjectItem::Type::Bus) },
              { .field="sendLevel", .type=PropType::Float, .label="Volume", .unit=" dB",
                .min=HK_PACK_PROPERTY_SEND_LEVEL_MIN, .max=HK_PACK_PROPERTY_SEND_LEVEL_MAX, .def=0.0, .decimals=1,
                .toolTip="Gain applied to the sent signal or sidechain detector input." },
          },
          .get=[](const harken::studio::IComponent* c){
              return routesToVariant(static_cast<const VMixNode*>(c)->sends, false);
          },
          .set=[](harken::studio::IComponent* c, const QVariant& v) -> bool {
              auto* m = static_cast<VMixNode*>(c);
              if (m->hostItem() && PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(m->hostItem()->type)) return false;
              return routesFromVariant(v, false, m->sends);
          },
          .serializeWhen=[](const harken::studio::IComponent* c){ return !static_cast<const VMixNode*>(c)->sends.empty(); } },
        { .field="sidechains", .type=PropType::List, .label="Sidechains", .section=PropertySection::Sidechain,
          .elementSchema={
              { .field="targetBusID", .type=PropType::ObjectRef, .label="Target", .def=0u,
                .refFilter=static_cast<int>(ProjectItem::Type::Bus) },
              { .field="sendLevel", .type=PropType::Float, .label="Volume", .unit=" dB",
                .min=HK_PACK_PROPERTY_SEND_LEVEL_MIN, .max=HK_PACK_PROPERTY_SEND_LEVEL_MAX, .def=0.0, .decimals=1,
                .toolTip="Gain applied to the sidechain detector input." },
              { .field="duckTarget", .type=PropType::Bool, .label="Target Ducking", .def=false,
                .options={{"Off", false, "Sidechain to..."}, {"On", true, "Sidechain to... (Ducking Enabled)"}},
                .toolTip="Use this sidechain to drive built-in ducking on the target bus." },
          },
          .get=[](const harken::studio::IComponent* c){
              return routesToVariant(static_cast<const VMixNode*>(c)->sidechains, true);
          },
          .set=[](harken::studio::IComponent* c, const QVariant& v) -> bool {
              auto* m = static_cast<VMixNode*>(c);
              if (m->hostItem() && PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(m->hostItem()->type)) return false;
              return routesFromVariant(v, true, m->sidechains);
          },
          .serializeWhen=[](const harken::studio::IComponent* c){ return !static_cast<const VMixNode*>(c)->sidechains.empty(); } },
        });
        constexpr std::array<PropertySection, 4> sections{
            PropertySection::FilterLowPass, PropertySection::FilterHighPass,
            PropertySection::FilterLowShelf, PropertySection::FilterHighShelf};
        for (const auto& field : FilterSemantics::fields()) {
            PropertyDescriptor descriptor;
            descriptor.field = QLatin1String(field.field);
            descriptor.ownerComponent = ComponentType::MixNode;
            descriptor.type = PropType::Float;
            descriptor.section = sections[static_cast<std::size_t>(field.kind)];
            descriptor.label = field.parameter == FilterParameter::Frequency ? QStringLiteral("Frequency")
                : field.parameter == FilterParameter::Q ? QStringLiteral("Q Value") : QStringLiteral("Gain");
            descriptor.unit = field.parameter == FilterParameter::Frequency ? QStringLiteral(" Hz")
                : field.parameter == FilterParameter::Gain ? QStringLiteral(" dB") : QString{};
            descriptor.min = field.minimum;
            descriptor.max = field.maximum;
            descriptor.def = field.defaultValue;
            descriptor.decimals = field.parameter == FilterParameter::Frequency ? 0
                : field.parameter == FilterParameter::Q ? 3 : 1;
            descriptor.flags.serialize = false;
            descriptor.get = [field](const IComponent* component) {
                return FilterSemantics::value(static_cast<const VMixNode*>(component)->filter, field);
            };
            descriptor.set = [field](IComponent* component, const QVariant& value) {
                const auto number = QJsonValue::fromVariant(value);
                return number.isDouble() && FilterSemantics::setValue(
                    static_cast<VMixNode*>(component)->filter, field, number.toDouble());
            };
            schema.push_back(std::move(descriptor));
        }
        return schema;
    }();
    return s;
}
#if defined(__clang__)
#  pragma clang diagnostic pop
#endif

}
