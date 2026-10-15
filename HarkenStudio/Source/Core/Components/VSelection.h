
#pragma once
#include "../MixingSemantics.h"

#include "../Component.h"
#include "../GridSemantics.h"
#include "../PropertyDescriptor.h"
#include "../SelectionData.h"
#include "../TransitionSemantics.h"

#include <QHash>
#include <QJsonObject>
#include <QVariant>

#include <cstdint>

namespace VNX {

enum class SelectionMode : uint8_t
{
    Random = 0,
    Sequence = 2,
    Match = 4,
    Blend = 5
};

enum class SelectionPlayType : uint8_t { Single = 0, Queue = 1 };
enum class RandomDiscipline : uint8_t { Draw = 0, Shuffle = 1 };
enum class SelectionMemoryScope : uint8_t { PerEmitter = 0, Global = 1 };

struct SelectionBehaviorDef
{
    RandomDiscipline discipline = RandomDiscipline::Draw;
    int avoidRepeat = 0;
    SelectionMemoryScope memoryScope = SelectionMemoryScope::PerEmitter;
    bool resumeFromLast = false;

    bool operator==(const SelectionBehaviorDef&) const = default;
};

class VSelection : public ReflectedComponent<VSelection>
{
public:
    static constexpr ComponentType kType = ComponentType::Selection;
    ComponentType getType() const override { return kType; }

    static const PropertySchema& schema();

    SelectionMode mode = SelectionMode::Random;
    SelectionPlayType playType = SelectionPlayType::Single;
    VariableResponse matchResponse = VariableResponse::AtStart;
    VariableResponse blendResponse = VariableResponse::Live;
    SelectionData selections;
    SelectionBehaviorDef behavior;
    bool spliceByMarker = false;

    TransitionTableDef transitions = canonicalTransitionTable();

    VariableResponse variableResponse() const
    {
        return mode == SelectionMode::Blend ? blendResponse : matchResponse;
    }

    DiscreteGridDef matchGrid(PlatformScope scope = std::nullopt) const
    {
        return selections.table(scope).matchGrid();
    }

    MixingDef mixing(PlatformScope scope = std::nullopt) const
    {
        return selections.table(scope).mixing();
    }

    std::vector<SelectionEntry> activeRows(PlatformScope scope = std::nullopt) const;

    bool isQueue() const
    {
        return playType == SelectionPlayType::Queue
            && (mode == SelectionMode::Random || mode == SelectionMode::Sequence);
    }

    bool fromJson(const QJsonObject& j) override;

    void toJsonExtra(QJsonObject& j) const override;

    void fromJsonExtra(const QJsonObject& j) override;
};

#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#  pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif
inline const PropertySchema& VSelection::schema()
{
    static const PropertySchema s = withOwner<VSelection>({
        { .field="mode", .type=PropType::Enum, .label="Mode", .section=PropertySection::Selection,
          .options={{"Random",0},{"Sequence",2},{"Match",4},{"Blend",5}},
          .get=[](const VNX::IComponent* c){
              return QVariant(static_cast<int>(static_cast<const VSelection*>(c)->mode));
          },
          .set=[](VNX::IComponent* c, const QVariant& v) -> bool {
              bool ok = false;
              const int raw = v.toInt(&ok);
              if (!ok || !QJsonValue::fromVariant(v).isDouble() || v.toDouble() != raw
                  || (raw != static_cast<int>(SelectionMode::Random)
                      && raw != static_cast<int>(SelectionMode::Sequence)
                      && raw != static_cast<int>(SelectionMode::Match)
                      && raw != static_cast<int>(SelectionMode::Blend))) return false;
              auto* selection = static_cast<VSelection*>(c);
              selection->mode = static_cast<SelectionMode>(raw);
              return true;
          } },
        { .field="playType", .type=PropType::Enum, .label="Play Type", .section=PropertySection::Selection,
          .options={{"Single",0},{"Queue",1}},
          .get=[](const VNX::IComponent* c) {
              return QVariant(static_cast<int>(static_cast<const VSelection*>(c)->playType));
          },
          .set=[](VNX::IComponent* c, const QVariant& v) -> bool {
              bool ok = false;
              const int raw = v.toInt(&ok);
              auto* selection = static_cast<VSelection*>(c);
              if (!ok || !QJsonValue::fromVariant(v).isDouble() || v.toDouble() != raw
                  || raw < 0 || raw > 1) return false;
              selection->playType = static_cast<SelectionPlayType>(raw);
              return true;
          } },
        { .field="variableResponse", .type=PropType::Enum, .label="Variable Response", .section=PropertySection::Selection,
          .flags={.serialize=false},
          .options={{"Fixed",0},{"Live",1}},
          .get=[](const VNX::IComponent* c) {
              return QVariant(static_cast<int>(static_cast<const VSelection*>(c)->variableResponse()));
          },
          .set=[](VNX::IComponent* c, const QVariant& v) -> bool {
              bool ok = false;
              const int raw = v.toInt(&ok);
              if (!ok || !QJsonValue::fromVariant(v).isDouble() || v.toDouble() != raw
                  || raw < 0 || raw > 1) return false;
              auto* selection = static_cast<VSelection*>(c);
              if (selection->mode == SelectionMode::Blend)
                  selection->blendResponse = static_cast<VariableResponse>(raw);
              else
                  selection->matchResponse = static_cast<VariableResponse>(raw);
              return true;
          } },
        { .field="selections", .type=PropType::List, .label="Selections", .section=PropertySection::Selection,
          .elementSchema={
              {.field="order", .type=PropType::Int, .label="#",
               .flags={.serialize=false, .settableByName=false, .state=true}},
              {.field="childGuid", .type=PropType::ObjectRef, .label="Child"},
              {.field="weight", .type=PropType::Int, .label="Weight"}},
          .get=[](const VNX::IComponent* c) {
              return static_cast<const VSelection*>(c)->selections.toJson().toVariantMap();
          },
          .set=[](VNX::IComponent* c, const QVariant& value) {
              auto parsed = SelectionData::fromJson(QJsonObject::fromVariantMap(value.toMap()));
              if (!parsed) return false;
              static_cast<VSelection*>(c)->selections = std::move(*parsed);
              return true;
          } },
    });
    return s;
}
#if defined(__clang__)
#  pragma clang diagnostic pop
#endif

}
