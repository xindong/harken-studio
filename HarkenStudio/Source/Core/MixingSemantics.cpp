#include "MixingSemantics.h"

#include "GridSemantics.h"
#include "ProjectTypes.h"

#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace VNX {

MixingChildDef MixingChildDef::unity(const QUuid& target, float minimum, float maximum)
{
    MixingChildDef child{target, {{minimum, 1.0f}, {maximum > minimum ? maximum : minimum + 1.0f, 1.0f}}, {}};
    child.normalize();
    return child;
}

void MixingChildDef::normalize()
{
    curveNormalize(0.0f, 1.0f, points, handles);
}

float MixingChildDef::gainAt(float value) const
{
    if (points.isEmpty() || value < points.front().x || value > points.back().x) return 0.0f;
    return curveBezierValueAt(points, handles, value);
}

bool MixingDef::hasChildren() const
{
    return std::any_of(rows.begin(), rows.end(),
        [](const auto& row) { return !row.children.empty(); });
}

int MixingDef::rowIndexForVariable(const QUuid& variable) const
{
    if (variable.isNull()) return -1;
    const auto found = std::find_if(rows.begin(), rows.end(),
        [&](const auto& row) { return row.variable == variable; });
    return found == rows.end() ? -1 : static_cast<int>(found - rows.begin());
}

int MixingDef::rowIndex(const QUuid& guid) const
{
    const auto found = std::find_if(rows.begin(), rows.end(),
        [&](const auto& row) { return row.guid == guid; });
    return found == rows.end() ? -1 : static_cast<int>(found - rows.begin());
}

QJsonArray MixingDef::toJson() const
{
    QJsonArray result;
    for (const auto& row : rows) {
        QJsonArray children;
        for (const auto& child : row.children) {
            QJsonArray points;
            QJsonArray handles;
            for (const auto& point : child.points)
                points.append(QJsonArray{point.x, point.value});
            for (const auto& handle : child.handles)
                handles.append(QJsonArray{handle.h1x, handle.h1y, handle.h2x, handle.h2y});
            children.append(QJsonObject{
                {QStringLiteral("target"), child.target.toString(QUuid::WithoutBraces)},
                {QStringLiteral("points"), points}, {QStringLiteral("handles"), handles}});
        }
        result.append(QJsonObject{
            {QStringLiteral("guid"), row.guid.toString(QUuid::WithoutBraces)},
            {QStringLiteral("variable"), row.variable.isNull() ? QJsonValue(QJsonValue::Null)
                : QJsonValue(row.variable.toString(QUuid::WithoutBraces))},
            {QStringLiteral("children"), children}});
    }
    return result;
}

std::optional<MixingDef> MixingDef::fromJson(const QJsonArray& json)
{
    MixingDef result;
    QSet<QUuid> variables;
    QSet<QUuid> guids;
    const auto tuple = [](const QJsonValue& value, int size) {
        if (!value.isArray() || value.toArray().size() != size) return false;
        for (const auto& number : value.toArray())
            if (!number.isDouble() || !std::isfinite(number.toDouble())
                || !std::isfinite(static_cast<float>(number.toDouble()))) return false;
        return true;
    };
    for (const auto& value : json) {
        if (!value.isObject()) return std::nullopt;
        const auto object = value.toObject();
        MixingRowDef row;
        const auto variable = object.value(QStringLiteral("variable"));
        row.variable = QUuid(variable.toString());
        const bool hasGuid = object.contains(QStringLiteral("guid"));
        row.guid = hasGuid ? QUuid(object.value(QStringLiteral("guid")).toString())
                            : MixingSemantics::legacyRowGuid(row.variable);
        if (object.size() != (hasGuid ? 3 : 2)
            || (!variable.isNull() && (!variable.isString() || row.variable.isNull()))
            || (!row.variable.isNull() && variables.contains(row.variable))
            || row.guid.isNull() || guids.contains(row.guid)
            || !object.value(QStringLiteral("children")).isArray()) return std::nullopt;
        variables.insert(row.variable);
        guids.insert(row.guid);
        QSet<QUuid> targets;
        for (const auto& member : object.value(QStringLiteral("children")).toArray()) {
            if (!member.isObject()) return std::nullopt;
            const auto source = member.toObject();
            MixingChildDef child;
            child.target = QUuid(source.value(QStringLiteral("target")).toString());
            if (source.size() != 3 || child.target.isNull() || targets.contains(child.target)
                || !source.value(QStringLiteral("points")).isArray()
                || !source.value(QStringLiteral("handles")).isArray()) return std::nullopt;
            targets.insert(child.target);
            const auto points = source.value(QStringLiteral("points")).toArray();
            const auto handles = source.value(QStringLiteral("handles")).toArray();
            const bool unconfigured = row.variable.isNull() && points.isEmpty() && handles.isEmpty();
            if (!unconfigured && (points.size() < 2 || points.size() > kCurveMaxPoints
                || handles.size() != points.size() - 1)) return std::nullopt;
            for (const auto& point : points) {
                if (!tuple(point, 2)) return std::nullopt;
                const auto p = point.toArray();
                child.points.append({static_cast<float>(p[0].toDouble()), static_cast<float>(p[1].toDouble())});
            }
            for (const auto& handle : handles) {
                if (!tuple(handle, 4)) return std::nullopt;
                const auto h = handle.toArray();
                child.handles.append({static_cast<float>(h[0].toDouble()), static_cast<float>(h[1].toDouble()),
                    static_cast<float>(h[2].toDouble()), static_cast<float>(h[3].toDouble())});
            }
            for (int i = 1; i < child.points.size(); ++i)
                if (!(child.points[i].x > child.points[i - 1].x)) return std::nullopt;
            if (!unconfigured) child.normalize();
            row.children.push_back(std::move(child));
        }
        result.rows.push_back(std::move(row));
    }
    return result;
}

namespace MixingSemantics {

QUuid legacyRowGuid(const QUuid& variable)
{
    static const QUuid nameSpace(QStringLiteral("ff37991a-e8d0-4d2b-99c3-3f9ad6a4c33a"));
    return QUuid::createUuidV5(nameSpace, variable.toRfc4122());
}

std::optional<QHash<QUuid, float>> evaluate(const MixingDef& mixing,
                                           const VariableValue& value)
{
    QHash<QUuid, float> result;
    for (const auto& row : mixing.rows) {
        if (row.variable.isNull()) {
            for (const auto& child : row.children)
                result[child.target] = result.value(child.target, 1.0f);
            continue;
        }
        const auto input = value(row.variable);
        if (!input || !std::isfinite(*input)) return std::nullopt;
        for (const auto& child : row.children)
            result[child.target] = result.value(child.target, 1.0f) * child.gainAt(*input);
    }
    return result;
}

void remapTargets(MixingDef& mixing, const QHash<QUuid, QUuid>& guidMap)
{
    for (auto& row : mixing.rows)
        for (auto& child : row.children)
            child.target = guidMap.value(child.target, child.target);
}

bool validateTargets(const ProjectItem& owner, const MixingDef& mixing, QString* error)
{
    for (const auto& row : mixing.rows)
        for (const auto& child : row.children)
            if (!GridSemantics::isGridTargetCandidate(owner, child.target)) {
                if (error) *error = QStringLiteral("Container '%1' Mixing target %2 is not a direct child")
                    .arg(owner.name, child.target.toString(QUuid::WithoutBraces));
                return false;
            }
    return true;
}

}
}
