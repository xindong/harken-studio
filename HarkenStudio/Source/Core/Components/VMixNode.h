
#pragma once

#include "../Component.h"
#include "../BindingSemantics.h"
#include "../EffectChainSemantics.h"
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

    VMixNode() = default;
    ~VMixNode() override = default;

    VMixNode(VMixNode&&) noexcept = default;
    VMixNode& operator=(VMixNode&&) noexcept = default;

    VMixNode(const VMixNode&) = default;
    VMixNode& operator=(const VMixNode&) = default;

    float volume = 0.0f;
    float pitch  = 0.0f;

    uint32_t outputBusId     = 0;
    float    outputBusVolume = 0.0f;
    harken::studio::types::ObjectId outputBusRef{};

    std::vector<Send> sends;

    BindingLedger bindings;
    EffectChainLedger effects;
    std::map<ProjectPlatformId, EffectChainLedger> effectPlatforms;

    const EffectChainLedger& effectsForPlatform(PlatformScope platform) const
    {
        const auto found = platform ? effectPlatforms.find(*platform) : effectPlatforms.end();
        return found == effectPlatforms.end() ? effects : found->second;
    }

    void toJsonExtra(QJsonObject& j) const override
    {
        if (!sends.empty()) j[QStringLiteral("sends")] = QJsonArray::fromVariantList(getField(QStringLiteral("sends")).toList());
        if (!bindings.isEmpty()) j[QStringLiteral("bindings")] = bindings.toJson();
        if (!effects.isEmpty())  j[QStringLiteral("effects")]  = effects.toJson();
        if (!effectPlatforms.empty()) {
            QJsonObject platforms;
            for (const auto& [platform, chain] : effectPlatforms)
                platforms.insert(ProjectPlatformSemantics::idString(platform), chain.toJson());
            j.insert(QStringLiteral("effectsPlatforms"), platforms);
        }
    }

    bool fromJson(const QJsonObject& j) override
    {
        if (!ReflectedComponent<VMixNode>::fromJson(j)) return false;
        return !j.contains(QStringLiteral("sends"))
            || (j.value(QStringLiteral("sends")).isArray()
                && setField(QStringLiteral("sends"), j.value(QStringLiteral("sends")).toVariant()));
    }

    void fromJsonExtra(const QJsonObject& j) override
    {
        bindings = BindingLedger::fromJson(j[QStringLiteral("bindings")].toArray());
        effects  = EffectChainLedger::fromJson(j[QStringLiteral("effects")].toArray());
        effectPlatforms.clear();
        const auto platforms = j.value(QStringLiteral("effectsPlatforms")).toObject();
        for (auto it = platforms.begin(); it != platforms.end(); ++it)
            effectPlatforms.emplace(QUuid(it.key()), EffectChainLedger::fromJson(it.value().toArray()));
    }
};

#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#  pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif
inline const PropertySchema& VMixNode::schema()
{
    static const PropertySchema s = withOwner<VMixNode>({
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
              { .field="type", .type=PropType::String, .label="Type", .def="Send" },
              { .field="duckTarget", .type=PropType::Bool, .label="Target Ducking", .def=false,
                .options={{"Off", false, "Sidechain to..."}, {"On", true, "Sidechain to... (Ducking Enabled)"}},
                .toolTip="Use this sidechain to drive built-in ducking on the target bus." },
          },
          .get=[](const harken::studio::IComponent* c){
              QVariantList out;
              for (const Send& s : static_cast<const VMixNode*>(c)->sends) {
                  auto row = s.toJson();
                  row[QStringLiteral("sendLevel")] = linearToDb(s.sendLevel);
                  out.push_back(row.toVariantMap());
              }
              return QVariant(out);
          },
          .set=[](harken::studio::IComponent* c, const QVariant& v) -> bool {
              auto* m = static_cast<VMixNode*>(c);
              if (m->hostItem() && PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(m->hostItem()->type)) return false;
              std::vector<Send> sends;
              for (const QVariant& e : v.toList()) {
                  auto send = Send::fromJson(QJsonObject::fromVariantMap(e.toMap()));
                  if (!send) return false;
                  send->sendLevel = dbToLinear(send->sendLevel);
                  sends.push_back(*send);
              }
              m->sends = std::move(sends);
              return true;
          },
          .serializeWhen=[](const harken::studio::IComponent* c){ return !static_cast<const VMixNode*>(c)->sends.empty(); } },
    });
    return s;
}
#if defined(__clang__)
#  pragma clang diagnostic pop
#endif

}
