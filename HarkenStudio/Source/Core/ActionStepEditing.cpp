#include "ActionStepEditing.h"

#include <QJsonObject>
#include <QJsonValue>

#include <algorithm>
#include <cmath>

namespace VNX {
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

QString timingName(QuantizeTiming timing)
{
    switch (timing) {
        case QuantizeTiming::Immediate:      return QStringLiteral("Immediate");
        case QuantizeTiming::NextBeat:       return QStringLiteral("NextBeat");
        case QuantizeTiming::NextBar:        return QStringLiteral("NextBar");
        case QuantizeTiming::NextMarker:        return QStringLiteral("NextMarker");
        case QuantizeTiming::NaturalEnd:     return QStringLiteral("NaturalEnd");
        case QuantizeTiming::CurrentLoopEnd: return QStringLiteral("CurrentLoopEnd");
        case QuantizeTiming::EntryMarker:       return QStringLiteral("EntryMarker");
        case QuantizeTiming::ExitMarker:        return QStringLiteral("ExitMarker");
    }
    return QStringLiteral("Immediate");
}

QuantizeTiming timingFromName(const QString& name)
{
    if (name == QLatin1String("NextBeat"))       return QuantizeTiming::NextBeat;
    if (name == QLatin1String("NextBar"))        return QuantizeTiming::NextBar;
    if (name == QLatin1String("NextMarker"))        return QuantizeTiming::NextMarker;
    if (name == QLatin1String("NaturalEnd"))     return QuantizeTiming::NaturalEnd;
    if (name == QLatin1String("CurrentLoopEnd")) return QuantizeTiming::CurrentLoopEnd;
    if (name == QLatin1String("EntryMarker"))       return QuantizeTiming::EntryMarker;
    if (name == QLatin1String("ExitMarker"))        return QuantizeTiming::ExitMarker;
    return QuantizeTiming::Immediate;
}

QString ifSilentName(QuantizeIfSilent behavior)
{
    return behavior == QuantizeIfSilent::Drop
        ? QStringLiteral("Drop") : QStringLiteral("Immediate");
}

QString resetScopeName(ResetScope scope)
{
    return scope == ResetScope::Global
        ? QStringLiteral("Global") : QStringLiteral("Emitter");
}

ActionQuantizeDef quantizeFromJson(const QJsonObject& obj)
{
    ActionQuantizeDef quantize;
    quantize.referenceObjectID = clampObjectID(
        obj.value(QStringLiteral("referenceObjectID")));
    quantize.referenceName = obj.value(QStringLiteral("referenceName")).toString();
    quantize.timing = timingFromName(obj.value(QStringLiteral("timing")).toString());
    quantize.ifSilent = obj.value(QStringLiteral("ifSilent")).toString()
            == QLatin1String("Drop")
        ? QuantizeIfSilent::Drop : QuantizeIfSilent::Immediate;
    return quantize;
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

bool validTimingName(const QString& name)
{
    return name == QLatin1String("Immediate")
        || name == QLatin1String("NextBeat")
        || name == QLatin1String("NextBar")
        || name == QLatin1String("NextMarker")
        || name == QLatin1String("NaturalEnd")
        || name == QLatin1String("CurrentLoopEnd")
        || name == QLatin1String("EntryMarker")
        || name == QLatin1String("ExitMarker");
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
    const bool supportsQuantize = type == VActionStep::Type::Play
        || type == VActionStep::Type::Stop
        || type == VActionStep::Type::PostAction;
    if (!supportsQuantize || !obj.contains(QStringLiteral("quantize")))
        return true;
    if (!obj.value(QStringLiteral("quantize")).isObject()) {
        if (error) *error = checkedStepError(index, QStringLiteral("has an invalid quantize block"));
        return false;
    }
    const QJsonObject quantize = obj.value(QStringLiteral("quantize")).toObject();
    for (auto field = quantize.begin(); field != quantize.end(); ++field) {
        if (field.key() != QLatin1String("referenceObjectID")
            && field.key() != QLatin1String("referenceName")
            && field.key() != QLatin1String("timing")
            && field.key() != QLatin1String("ifSilent")) {
            if (error) *error = checkedStepError(index,
                QStringLiteral("has an unknown quantize field '%1'").arg(field.key()));
            return false;
        }
    }
    if (quantize.contains(QStringLiteral("referenceObjectID"))
        && !validObjectID(quantize.value(QStringLiteral("referenceObjectID")))) {
        if (error) *error = checkedStepError(index, QStringLiteral("has an invalid quantize referenceObjectID"));
        return false;
    }
    if (quantize.contains(QStringLiteral("referenceName"))
        && !quantize.value(QStringLiteral("referenceName")).isString()) {
        if (error) *error = checkedStepError(index, QStringLiteral("has an invalid quantize referenceName"));
        return false;
    }
    if (quantize.contains(QStringLiteral("timing"))) {
        const QJsonValue timing = quantize.value(QStringLiteral("timing"));
        if (!timing.isString() || !validTimingName(timing.toString())) {
            if (error) *error = checkedStepError(index, QStringLiteral("has an invalid quantize timing"));
            return false;
        }
    }
    if (quantize.contains(QStringLiteral("ifSilent"))) {
        const QJsonValue behavior = quantize.value(QStringLiteral("ifSilent"));
        if (!behavior.isString()
            || (behavior.toString() != QLatin1String("Immediate")
                && behavior.toString() != QLatin1String("Drop"))) {
            if (error) *error = checkedStepError(index, QStringLiteral("has an invalid quantize ifSilent"));
            return false;
        }
    }
    return true;
}

bool isCanonicalNoOp(const ActionQuantizeDef& quantize)
{
    return quantize.referenceObjectID == 0
        && quantize.referenceName.isEmpty()
        && quantize.timing == QuantizeTiming::Immediate
        && quantize.ifSilent == QuantizeIfSilent::Immediate;
}

QJsonObject quantizeToJson(const ActionQuantizeDef& quantize)
{
    QJsonObject obj;
    obj[QStringLiteral("referenceObjectID")] =
        static_cast<qint64>(quantize.referenceObjectID);
    obj[QStringLiteral("referenceName")] = quantize.referenceName;
    obj[QStringLiteral("timing")] = timingName(quantize.timing);
    obj[QStringLiteral("ifSilent")] = ifSilentName(quantize.ifSilent);
    return obj;
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

        if (type == VActionStep::Type::Play
            || type == VActionStep::Type::Stop
            || type == VActionStep::Type::PostAction) {
            step.quantize = quantizeFromJson(
                obj.value(QStringLiteral("quantize")).toObject());
        }

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

        if ((step.type == VActionStep::Type::Play
             || step.type == VActionStep::Type::Stop
             || step.type == VActionStep::Type::PostAction)
            && !isCanonicalNoOp(step.quantize)) {
            obj[QStringLiteral("quantize")] = quantizeToJson(step.quantize);
        }

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
