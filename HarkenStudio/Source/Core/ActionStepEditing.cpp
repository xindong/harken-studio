#include "ActionStepEditing.h"

#include <QJsonObject>
#include <QJsonValue>

#include <algorithm>
#include <cmath>

namespace harken::studio {
namespace ActionStepEditing {

namespace {

bool resolveSupportedType(const QString& text, VActionStep::Type& out)
{
    const std::optional<VActionStep::Type> type = VActionStep::typeFromString(text);
    if (!type)
        return false;
    out = *type;
    return true;
}

float clampToDomain(double value, float maxValue)
{
    if (!std::isfinite(value) || value <= 0.0)
        return 0.0f;
    if (value >= static_cast<double>(maxValue))
        return maxValue;
    return static_cast<float>(value);
}

float finiteValue(double value)
{
    return std::isfinite(value) ? static_cast<float>(value) : 0.0f;
}

uint32_t clampObjectID(const QJsonValue& value)
{
    const qint64 raw = value.toInteger(0);
    if (raw <= 0 || raw > static_cast<qint64>(UINT32_MAX))
        return 0;
    return static_cast<uint32_t>(raw);
}

QString resetScopeName(ResetScope scope)
{
    return scope == ResetScope::Global
        ? QStringLiteral("Global") : QStringLiteral("Emitter");
}

bool validObjectID(const QJsonValue& value)
{
    if (!value.isDouble()) return false;
    const double raw = value.toDouble();
    return std::isfinite(raw) && std::trunc(raw) == raw
        && raw >= 0.0 && raw <= static_cast<double>(UINT32_MAX);
}

bool validFiniteNumber(const QJsonValue& value)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    return std::isfinite(number)
        && number >= -static_cast<double>(std::numeric_limits<float>::max())
        && number <= static_cast<double>(std::numeric_limits<float>::max());
}

QString checkedStepError(qsizetype index, const QString& detail)
{
    return QStringLiteral("Action step %1 %2").arg(index).arg(detail);
}

bool validateCheckedStep(const QJsonValue& entry, qsizetype index, QString* error)
{
    if (!entry.isObject()) {
        if (error) *error = checkedStepError(index, QStringLiteral("must be an object"));
        return false;
    }
    const QJsonObject obj = entry.toObject();
    if (!obj.value(QStringLiteral("type")).isString()
        || !VActionStep::typeFromString(obj.value(QStringLiteral("type")).toString())) {
        if (error) *error = checkedStepError(index, QStringLiteral("has an unknown type"));
        return false;
    }
    const VActionStep::Type type = *VActionStep::typeFromString(
        obj.value(QStringLiteral("type")).toString());
    if (obj.contains(QStringLiteral("targetObjectID"))
        && !validObjectID(obj.value(QStringLiteral("targetObjectID")))) {
        if (error) *error = checkedStepError(index, QStringLiteral("has an invalid targetObjectID"));
        return false;
    }
    if (obj.contains(QStringLiteral("targetName"))
        && !obj.value(QStringLiteral("targetName")).isString()) {
        if (error) *error = checkedStepError(index, QStringLiteral("has an invalid targetName"));
        return false;
    }
    if (obj.contains(QStringLiteral("delay"))
        && !validFiniteNumber(obj.value(QStringLiteral("delay")))) {
        if (error) *error = checkedStepError(index, QStringLiteral("has an invalid delay"));
        return false;
    }
    if (type == VActionStep::Type::Stop
        && obj.contains(QStringLiteral("fadeTime"))
        && !validFiniteNumber(obj.value(QStringLiteral("fadeTime")))) {
        if (error) *error = checkedStepError(index, QStringLiteral("has an invalid fadeTime"));
        return false;
    }
    if (type == VActionStep::Type::SetVariable
        && obj.contains(QStringLiteral("value"))
        && !validFiniteNumber(obj.value(QStringLiteral("value")))) {
        if (error) *error = checkedStepError(index, QStringLiteral("has an invalid value"));
        return false;
    }
    if (type == VActionStep::Type::SetVariable
        && obj.contains(QStringLiteral("scope"))) {
        const QJsonValue scope = obj.value(QStringLiteral("scope"));
        if (!scope.isString()
            || (scope.toString() != QLatin1String("Emitter")
                && scope.toString() != QLatin1String("Global"))) {
            if (error) *error = checkedStepError(index, QStringLiteral("has an invalid scope"));
            return false;
        }
    }
    if (type == VActionStep::Type::Reset
        && obj.contains(QStringLiteral("resetScope"))) {
        const QJsonValue scope = obj.value(QStringLiteral("resetScope"));
        if (!scope.isString()
            || (scope.toString() != QLatin1String("Emitter")
                && scope.toString() != QLatin1String("Global"))) {
            if (error) *error = checkedStepError(index, QStringLiteral("has an invalid resetScope"));
            return false;
        }
    }
    return true;
}

}

std::vector<VActionStep> stepsFromJson(const QJsonArray& value)
{
    std::vector<VActionStep> steps;
    steps.reserve(static_cast<size_t>(value.size()));
    for (const QJsonValue& entry : value) {
        const QJsonObject obj = entry.toObject();

        VActionStep::Type type;
        if (!resolveSupportedType(obj.value(QStringLiteral("type")).toString(), type))
            continue;

        VActionStep step;
        step.type = type;
        step.targetObjectID = clampObjectID(obj.value(QStringLiteral("targetObjectID")));
        step.targetName = obj.value(QStringLiteral("targetName")).toString();
        step.delay = clampToDomain(obj.value(QStringLiteral("delay")).toDouble(0.0),
                                   kMaxDelayMs);

        if (type == VActionStep::Type::Stop)
            step.fadeTime = clampToDomain(
                obj.value(QStringLiteral("fadeTime")).toDouble(0.0), kMaxFadeTimeMs);

        if (type == VActionStep::Type::SetVariable) {
            step.value = finiteValue(obj.value(QStringLiteral("value")).toDouble(0.0));
            step.scope = obj.value(QStringLiteral("scope")).toString()
                    == QLatin1String("Global")
                ? VariableScope::Global : VariableScope::Emitter;
        }

        if (type == VActionStep::Type::Reset) {
            step.resetScope = obj.value(QStringLiteral("resetScope")).toString()
                    == QLatin1String("Global")
                ? ResetScope::Global : ResetScope::Emitter;
        }

        steps.push_back(step);
    }
    return steps;
}

std::optional<std::vector<VActionStep>> checkedStepsFromJson(
    const QJsonArray& value, QString* error)
{
    for (qsizetype i = 0; i < value.size(); ++i)
        if (!validateCheckedStep(value.at(i), i, error))
            return std::nullopt;
    std::vector<VActionStep> steps = stepsFromJson(value);
    if (steps.size() != static_cast<size_t>(value.size())) {
        if (error) *error = QStringLiteral("Action step ledger is invalid");
        return std::nullopt;
    }
    return steps;
}

QJsonArray stepsToJson(const std::vector<VActionStep>& steps)
{
    QJsonArray value;
    for (const VActionStep& step : steps) {
        const QString typeName = step.typeString();
        if (typeName.isEmpty())
            continue;

        QJsonObject obj;
        obj[QStringLiteral("type")] = typeName;
        obj[QStringLiteral("targetObjectID")] = static_cast<qint64>(step.targetObjectID);
        obj[QStringLiteral("targetName")] = step.targetName;
        obj[QStringLiteral("delay")] = clampToDomain(step.delay, kMaxDelayMs);

        if (step.type == VActionStep::Type::Stop)
            obj[QStringLiteral("fadeTime")] = clampToDomain(
                step.fadeTime, kMaxFadeTimeMs);

        if (step.type == VActionStep::Type::SetVariable) {
            obj[QStringLiteral("value")] = finiteValue(step.value);
            obj[QStringLiteral("scope")] = step.scope == VariableScope::Global
                ? QStringLiteral("Global") : QStringLiteral("Emitter");
        }

        if (step.type == VActionStep::Type::Reset)
            obj[QStringLiteral("resetScope")] = resetScopeName(step.resetScope);

        value.append(obj);
    }
    return value;
}

float normalizeSetVariableValue(float value, VariableKind kind,
                        float rangeMin, float rangeMax)
{
    if (!std::isfinite(rangeMin) || !std::isfinite(rangeMax))
        return 0.0f;
    if (!std::isfinite(value))
        value = 0.0f;
    if (rangeMin > rangeMax)
        std::swap(rangeMin, rangeMax);
    value = VariableSemantics::clampToRange(value, rangeMin, rangeMax);
    if (kind == VariableKind::Step)
        value = VariableSemantics::clampToRange(
            static_cast<float>(VariableSemantics::discreteRead(value)),
            rangeMin, rangeMax);
    return value;
}

}
}
