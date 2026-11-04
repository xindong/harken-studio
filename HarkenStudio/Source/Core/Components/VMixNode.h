
#pragma once

#include "../Component.h"
#include "../BindingSemantics.h"
#include "../EffectChainSemantics.h"
#include "../PropertyDescriptor.h"
#include "../ProjectTypes.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <cmath>
#include <cstdint>
#include <vector>

namespace VNX {

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
    float makeUpGain = 0.0f;

    uint32_t outputBusId     = 0;
    float    outputBusVolume = 0.0f;
    harkenstudio::ObjectId mainOutputBusRef{};

    std::vector<Send> sends;

    BindingLedger bindings;
    EffectChainLedger effects;

    void toJsonExtra(QJsonObject& j) const override
    {
        if (!sends.empty()) j[QStringLiteral("sends")] = QJsonArray::fromVariantList(getField(QStringLiteral("sends")).toList());
        if (!bindings.isEmpty()) j[QStringLiteral("bindings")] = bindings.toJson();
        if (!effects.isEmpty())  j[QStringLiteral("effects")]  = effects.toJson();
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
          .unit=" dB", .min=-96.0, .max=24.0, .decimals=1, .flags={.sourceIndicator=true},
          .get=VNX_PROP_GET(VMixNode, volume), .set=VNX_PROP_SET_F(VMixNode, volume) },
        { .field="pitch", .type=PropType::Float, .label="Pitch", .section=PropertySection::Playback,
          .unit=" cent", .min=-4800.0, .max=4800.0, .decimals=0, .flags={.sourceIndicator=true},
          .get=VNX_PROP_GET(VMixNode, pitch), .set=VNX_PROP_SET_F(VMixNode, pitch) },
        { .field="makeUpGain", .type=PropType::Float, .label="Make-Up Gain", .section=PropertySection::Playback,
          .unit=" dB", .min=-24.0, .max=24.0, .decimals=1, .flags={.sourceIndicator=true}, .aliases={"makeupgain"},
          .get=VNX_PROP_GET(VMixNode, makeUpGain), .set=VNX_PROP_SET_F(VMixNode, makeUpGain) },
        { .field="outputBusId", .type=PropType::ObjectRef, .label="Main Output", .section=PropertySection::MainOutput,
          .aliases={"outputbusid"}, .refFilter=static_cast<int>(ProjectItem::Type::Bus),
          .get=[](const VNX::IComponent* c){
              const auto* m = static_cast<const VMixNode*>(c);
              const ProjectItem* h = m->hostItem();
              if (h && h->type == ProjectItem::Type::Bus)
              {
                  const ProjectItem* p = h->parent;
                  return QVariant(static_cast<qint64>(p ? p->objectID : 0));
              }
              return QVariant(static_cast<qint64>(m->outputBusId));
          },
          .set=[](VNX::IComponent* c, const QVariant& v) -> bool {
              auto* m = static_cast<VMixNode*>(c);
              const ProjectItem* h = m->hostItem();
              if (h && (h->type == ProjectItem::Type::Bus
                  || PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(h->type))) return false;
              m->outputBusId = v.toUInt();
              return true;
          },
          .serializeWhen=[](const VNX::IComponent* c){ return static_cast<const VMixNode*>(c)->outputBusId != 0; } },
        { .field="mainOutputBusRef", .type=PropType::ObjectRef, .label="Output Bus", .section=PropertySection::MainOutput,
          .flags={.serialize=false},
          .get=[](const VNX::IComponent* c) { return QVariant::fromValue(
              static_cast<const VMixNode*>(c)->mainOutputBusRef); },
          .set=[](IComponent* c, const QVariant& v) {
              auto* m = static_cast<VMixNode*>(c);
              if (m->hostItem() && PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(m->hostItem()->type)) return false;
              if (!v.canConvert<harkenstudio::ObjectId>()) return false;
              m->mainOutputBusRef = v.value<harkenstudio::ObjectId>();
              return true;
          } },
        { .field="outputBusVolume", .type=PropType::Float, .label="Output Volume", .section=PropertySection::MainOutput,
          .unit=" dB", .min=-96.0, .max=24.0, .decimals=1, .aliases={"outputbusvolume"},
          .get=VNX_PROP_GET(VMixNode, outputBusVolume), .set=[](IComponent* c, const QVariant& v) {
              auto* m = static_cast<VMixNode*>(c);
              if (m->hostItem() && PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(m->hostItem()->type)) return false;
              m->outputBusVolume = v.toFloat();
              return true;
          },
          .serializeWhen=[](const VNX::IComponent* c){ return static_cast<const VMixNode*>(c)->outputBusVolume != 0.0f; } },
        { .field="sends", .type=PropType::List, .label="Sends", .section=PropertySection::Send,
          .elementSchema={
              { .field="targetBusID", .type=PropType::ObjectRef, .label="Target", .def=0u,
                .refFilter=static_cast<int>(ProjectItem::Type::Bus) },
              { .field="sendLevel", .type=PropType::Float, .label="Volume", .unit=" dB",
                .min=-96.0, .max=24.0, .def=0.0, .decimals=1,
                .toolTip="Gain applied to the sent signal. In Ducking mode, this controls the detector input level, not the target bus attenuation." },
              { .field="mode", .type=PropType::Enum, .label="Mode", .def=0,
                .options={{"Mix", 0}, {"Ducking", 1}, {"Sidechain", 2}},
                .toolTip="Mix adds audio to the target bus. Ducking drives built-in ducking on the target bus. Sidechain supplies an auxiliary signal to sidechain receivers." },
          },
          .get=[](const VNX::IComponent* c){
              QVariantList out;
              for (const Send& s : static_cast<const VMixNode*>(c)->sends) {
                  QVariantMap row{
                      { QStringLiteral("mode"), static_cast<int>(s.mode) },
                      { QStringLiteral("targetBusID"), s.targetBusID },
                      { QStringLiteral("sendLevel"),   linearToDb(s.sendLevel) } };
                  if (!s.modeVolumesDb.empty())
                      row.insert(QStringLiteral("modeVolumes"), sendModeVolumesJson(s.modeVolumesDb).toVariantMap());
                  out.push_back(row);
              }
              return QVariant(out);
          },
          .set=[](VNX::IComponent* c, const QVariant& v) -> bool {
              auto* m = static_cast<VMixNode*>(c);
              if (m->hostItem() && PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(m->hostItem()->type)) return false;
              std::vector<Send> sends;
              for (const QVariant& e : v.toList()) {
                  const QVariantMap row = e.toMap();
                  const auto modeValue = QJsonValue::fromVariant(row.value(QStringLiteral("mode")));
                  const int mode = modeValue.toInt(-1);
                  if (row.contains(QStringLiteral("mode")) && (!modeValue.isDouble()
                      || modeValue.toDouble() != mode || mode < 0 || mode > 2)) return false;
                  Send send{row.value(QStringLiteral("targetBusID")).toUInt(),
                            dbToLinear(row.value(QStringLiteral("sendLevel")).toFloat()),
                            static_cast<SendMode>(row.value(QStringLiteral("mode")).toInt())};
                  if (!readSendModeVolumes(QJsonObject::fromVariantMap(row).value(QStringLiteral("modeVolumes")),
                                           send.mode, send.modeVolumesDb)) return false;
                  sends.push_back(std::move(send));
              }
              m->sends = std::move(sends);
              return true;
          },
          .serializeWhen=[](const VNX::IComponent* c){ return !static_cast<const VMixNode*>(c)->sends.empty(); } },
    });
    return s;
}
#if defined(__clang__)
#  pragma clang diagnostic pop
#endif

}
