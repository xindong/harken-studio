#pragma once

#include "CurveSemantics.h"

#include <QHash>
#include <QJsonArray>
#include <QUuid>

#include <functional>
#include <optional>
#include <vector>

namespace VNX {

struct ProjectItem;

struct MixingChildDef
{
    QUuid target;
    QVector<CurvePointDef> points;
    QVector<CurveHandlePairDef> handles;

    static MixingChildDef unity(const QUuid& target, float minimum, float maximum);
    void normalize();
    float gainAt(float value) const;
};

struct MixingRowDef
{
    QUuid variable;
    std::vector<MixingChildDef> children;
    QUuid guid = QUuid::createUuid();
};

struct MixingDef
{
    std::vector<MixingRowDef> rows;

    bool empty() const { return rows.empty(); }
    bool hasChildren() const;
    int rowIndexForVariable(const QUuid& variable) const;
    int rowIndex(const QUuid& guid) const;
    QJsonArray toJson() const;
    static std::optional<MixingDef> fromJson(const QJsonArray& json);
};

namespace MixingSemantics {

using VariableValue = std::function<std::optional<float>(const QUuid&)>;
QUuid legacyRowGuid(const QUuid& variable);
std::optional<QHash<QUuid, float>> evaluate(const MixingDef& mixing,
                                           const VariableValue& value);
void remapTargets(MixingDef& mixing, const QHash<QUuid, QUuid>& guidMap);
bool validateTargets(const ProjectItem& owner, const MixingDef& mixing,
                     QString* error = nullptr);

}
}
