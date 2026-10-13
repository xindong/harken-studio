
#pragma once

#include "../Component.h"
#include "../PropertyDescriptor.h"
#include "../VariableSemantics.h"

#include <QString>
#include <QVariant>
#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace VNX {

// Authoring bound of a declared range: the field is a free numeric domain, so
// the editor only fences it against runaway input.
inline constexpr double kVariableRangeAuthoringBound = 1000000.0;

class VVariable : public ReflectedComponent<VVariable>
{
public:
    static constexpr ComponentType kType = ComponentType::Variable;
    ComponentType getType() const override { return kType; }

    static const PropertySchema& schema();

    VariableKind  kind = VariableKind::Smooth;
    float rangeMin = 0.0f;
    float rangeMax = 100.0f;
    float defaultValue = 0.0f;
    std::uint8_t writers = 1;
    QString unit;
    std::int32_t precision = 0;
    std::uint8_t displayScale = 0;
    QString description;
    std::vector<VariableLabel> labels;

    void toJsonExtra(QJsonObject& json) const override
    {
        if (!labels.empty())
            json[QStringLiteral("labels")] = QJsonArray::fromVariantList(getField(QStringLiteral("labels")).toList());
    }

    void fromJsonExtra(const QJsonObject& json) override
    {
        rangeMin = static_cast<float>(json.value(QStringLiteral("rangeMin")).toDouble(0.0));
        rangeMax = static_cast<float>(json.value(QStringLiteral("rangeMax")).toDouble(100.0));
        defaultValue = static_cast<float>(json.value(QStringLiteral("defaultValue")).toDouble(0.0));
        labels.clear();
        for (const auto& entry : json.value(QStringLiteral("labels")).toArray()) {
            const auto row = entry.toObject();
            VariableLabel label{row.value(QStringLiteral("anchor")).toInt(),
                                row.value(QStringLiteral("name")).toString()};
            if (row.contains(QStringLiteral("id")))
                label.id = QUuid(row.value(QStringLiteral("id")).toString());
            labels.push_back(std::move(label));
        }
        normalizeDeclaration();
    }

    void normalizeDeclaration()
    {
        const auto n = VariableSemantics::normalizeDeclarationForKind(
            kind, rangeMin, rangeMax, defaultValue);
        rangeMin = n.rangeMin;
        rangeMax = n.rangeMax;
        defaultValue = n.defaultValue;
        if (writers < 1 || writers > 3)
            writers = 1;
    }
};

#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#  pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif
inline const PropertySchema& VVariable::schema()
{
    static const PropertySchema s = withOwner<VVariable>({
        { .field="kind", .type=PropType::Enum, .label="Kind", .section=PropertySection::Identity,
          .def=1, .flags={ .segmented=true }, .options={{"Step",0},{"Smooth",1}},
          .get=[](const VNX::IComponent* c){
              return QVariant(static_cast<int>(static_cast<const VVariable*>(c)->kind));
          },
          .set=[](VNX::IComponent* c, const QVariant& v) -> bool {
              const int raw = v.toInt();
              if (raw < static_cast<int>(VariableKind::Step)
                  || raw > static_cast<int>(VariableKind::Smooth)) return false;
              auto* var = static_cast<VVariable*>(c);
              var->kind = static_cast<VariableKind>(raw);
              var->normalizeDeclaration();
              return true;
          } },
        { .field="writers", .type=PropType::Int, .label="Writers", .section=PropertySection::Identity,
          .min=1, .max=3, .def=1, .flags={ .advanced=true },
          .get=[](const VNX::IComponent* c){
              return QVariant(static_cast<int>(static_cast<const VVariable*>(c)->writers));
          },
          .set=[](VNX::IComponent* c, const QVariant& v) -> bool {
              const int raw = v.toInt();
              if (raw < 1 || raw > 3) return false;
              static_cast<VVariable*>(c)->writers = static_cast<std::uint8_t>(raw);
              return true;
          },
          .serializeWhen=[](const VNX::IComponent* c){
              return static_cast<const VVariable*>(c)->writers != 1;
          } },
        { .field="unit", .type=PropType::String, .label="Unit", .section=PropertySection::Identity,
          .flags={ .advanced=true },
          .get=VNX_PROP_GET(VVariable, unit),
          .set=[](VNX::IComponent* c, const QVariant& v) -> bool {
              static_cast<VVariable*>(c)->unit = v.toString();
              return true;
          },
          .serializeWhen=[](const VNX::IComponent* c){
              return !static_cast<const VVariable*>(c)->unit.isEmpty();
          } },
        { .field="precision", .type=PropType::Int, .label="Precision", .section=PropertySection::Identity,
          .flags={ .advanced=true },
          .get=VNX_PROP_GETI(VVariable, precision),
          .set=VNX_PROP_SET_I(VVariable, precision),
          .serializeWhen=[](const VNX::IComponent* c){
              return static_cast<const VVariable*>(c)->precision != 0;
          } },
        { .field="displayScale", .type=PropType::Int, .label="Display Scale", .section=PropertySection::Identity,
          .min=0, .max=255, .def=0, .flags={ .advanced=true },
          .get=[](const VNX::IComponent* c){
              return QVariant(static_cast<int>(static_cast<const VVariable*>(c)->displayScale));
          },
          .set=[](VNX::IComponent* c, const QVariant& v) -> bool {
              const int raw = v.toInt();
              if (raw < 0 || raw > 255) return false;
              static_cast<VVariable*>(c)->displayScale =
                  static_cast<std::uint8_t>(raw);
              return true;
          },
          .serializeWhen=[](const VNX::IComponent* c){
              return static_cast<const VVariable*>(c)->displayScale != 0;
          } },
        { .field="description", .type=PropType::String, .label="Description", .section=PropertySection::Identity,
          .flags={ .advanced=true },
          .get=VNX_PROP_GET(VVariable, description),
          .set=[](VNX::IComponent* c, const QVariant& v) -> bool {
              static_cast<VVariable*>(c)->description = v.toString();
              return true;
          },
          .serializeWhen=[](const VNX::IComponent* c){
              return !static_cast<const VVariable*>(c)->description.isEmpty();
          } },
        // Storage rows: the serialized v6 keys and the by-name setters every
        // non-inspector writer (IPC, adapters, templates) reaches for. The page
        // edits the pair through the "range" row below instead.
        { .field="rangeMin", .type=PropType::Float, .label="Range Min", .section=PropertySection::Identity,
          .decimals=2,
          .get=VNX_PROP_GET(VVariable, rangeMin),
          .set=[](VNX::IComponent* c, const QVariant& v) -> bool {
              auto* var = static_cast<VVariable*>(c);
              bool ok = false;
              const float value = v.toFloat(&ok);
              if (!ok || !std::isfinite(value) || value < 0.0f || value > var->rangeMax) return false;
              var->rangeMin = value;
              var->normalizeDeclaration();
              return true;
          } },
        { .field="rangeMax", .type=PropType::Float, .label="Range Max", .section=PropertySection::Identity,
          .def=100.0, .decimals=2,
          .get=VNX_PROP_GET(VVariable, rangeMax),
          .set=[](VNX::IComponent* c, const QVariant& v) -> bool {
              auto* var = static_cast<VVariable*>(c);
              bool ok = false;
              const float value = v.toFloat(&ok);
              if (!ok || !std::isfinite(value) || value < 0.0f || value < var->rangeMin) return false;
              var->rangeMax = value;
              var->normalizeDeclaration();
              return true;
          } },
        // The declared domain is one property: both bounds land in a single
        // write so no edit passes through an inverted or half-applied range.
        { .field="range", .type=PropType::Range, .label="Range", .section=PropertySection::Identity,
          .decimals=2, .flags={ .serialize=false },
          .get=[](const VNX::IComponent* c){
              const auto* var = static_cast<const VVariable*>(c);
              return QVariant(QVariantList{ var->rangeMin, var->rangeMax });
          },
          .set=[](VNX::IComponent* c, const QVariant& v) -> bool {
              const QVariantList bounds = v.toList();
              if (bounds.size() != 2) return false;
              bool aOk = false;
              bool bOk = false;
              const float a = bounds.at(0).toFloat(&aOk);
              const float b = bounds.at(1).toFloat(&bOk);
              if (!aOk || !bOk || !std::isfinite(a) || !std::isfinite(b)
                  || a < 0.0f || b < 0.0f) return false;
              auto* var = static_cast<VVariable*>(c);
              var->rangeMin = std::min(a, b);
              var->rangeMax = std::max(a, b);
              var->normalizeDeclaration();
              return true;
          },
          .domain=[](const VNX::IComponent* c) -> std::optional<FieldDomain> {
              const auto form = VariableSemantics::editFormForKind(
                  static_cast<const VVariable*>(c)->kind);
              return FieldDomain{ 0.0,
                                  kVariableRangeAuthoringBound,
                                  form.decimals, form.quantum, {} };
          } },
        { .field="defaultValue", .type=PropType::Float, .label="Default Value", .section=PropertySection::Identity,
          .decimals=2,
          .get=VNX_PROP_GET(VVariable, defaultValue),
          .set=[](VNX::IComponent* c, const QVariant& v) -> bool {
              auto* var = static_cast<VVariable*>(c);
              bool ok = false;
              const float value = v.toFloat(&ok);
              if (!ok || !std::isfinite(value)) return false;
              var->defaultValue = value;
              var->normalizeDeclaration();
              return true;
          },
          .domain=[](const VNX::IComponent* c) -> std::optional<FieldDomain> {
              const auto* var = static_cast<const VVariable*>(c);
              const auto form = VariableSemantics::editFormForKind(var->kind);
              return FieldDomain{
                  var->rangeMin, var->rangeMax, form.decimals, form.quantum,
                  [labels = var->labels](double value) {
                      return VariableSemantics::labelTextForValue(
                          static_cast<float>(value), labels).value_or(QString());
                  }, var->kind == VariableKind::Step };
          } },
        // The declared range is a window onto the integer domain, not a fence
        // around the labels: authoring happens inside the window, but an anchor
        // the window has moved off keeps its name and comes back when the range
        // widens again. A label belongs to the float domain, not to a range.
        { .field="labels", .type=PropType::List, .label="Labels", .section=PropertySection::Label,
          .elementSchema={
              { .field="anchor", .type=PropType::Int, .label="#", .def=0,
                .flags={ .state=true } },
              { .field="name",   .type=PropType::String, .label="Name" },
          },
          .get=[](const VNX::IComponent* c){
              QVariantList out;
              for (const VariableLabel& l : static_cast<const VVariable*>(c)->labels)
                  out.push_back(QVariantMap{
                      { QStringLiteral("anchor"), l.anchor },
                      { QStringLiteral("id"), l.id.toString(QUuid::WithoutBraces) },
                      { QStringLiteral("name"),   l.name } });
              return QVariant(out);
          },
          .set=[](VNX::IComponent* c, const QVariant& v) -> bool {
              auto* variable = static_cast<VVariable*>(c);
              std::vector<VariableLabel> labels;
              if (v.metaType().id() != QMetaType::QVariantList) return false;
              for (const QVariant& e : v.toList()) {
                  const QVariantMap m = e.toMap();
                  bool ok = false;
                  const double raw = m.value(QStringLiteral("anchor")).toDouble(&ok);
                  if (!ok || !std::isfinite(raw) || raw < 0.0
                      || raw > VariableSemantics::kStepExactIntBound || raw != std::floor(raw)) return false;
                  VariableLabel label{static_cast<int>(raw),
                      m.value(QStringLiteral("name")).toString()};
                  if (m.contains(QStringLiteral("id")))
                      label.id = QUuid(m.value(QStringLiteral("id")).toString());
                  else if (const auto* previous = VariableSemantics::labelAtIndex(variable->labels, label.anchor))
                      label.id = previous->id;
                  labels.push_back(std::move(label));
              }
              if (!VariableSemantics::validateLabels(labels)) return false;
              std::sort(labels.begin(), labels.end(),
                  [](const auto& a, const auto& b) { return a.anchor < b.anchor; });
              variable->labels = std::move(labels);
              return true;
          },
          .serializeWhen=[](const VNX::IComponent* c){
              return !static_cast<const VVariable*>(c)->labels.empty();
          },
          .domain=[](const VNX::IComponent* c) -> std::optional<FieldDomain> {
              const auto* var = static_cast<const VVariable*>(c);
              return FieldDomain{ var->rangeMin, var->rangeMax, 0, 1.0, {} };
          },
          .validateElement=[](const QVariantList& rows, int row, const QString& field) {
              if (field != QLatin1String("name")) return QString();
              QSet<QString> otherNames;
              for (int i = 0; i < rows.size(); ++i)
                  if (i != row)
                      otherNames.insert(rows[i].toMap().value(QStringLiteral("name")).toString());
              return VariableSemantics::labelNameError(
                  rows[row].toMap().value(field).toString(), otherNames);
          } },
    });
    return s;
}
#if defined(__clang__)
#  pragma clang diagnostic pop
#endif

}
