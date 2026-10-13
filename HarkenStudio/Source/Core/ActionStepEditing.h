
#pragma once

#include "ProjectTypes.h"
#include "VariableSemantics.h"

#include <QJsonArray>
#include <QString>

#include <optional>
#include <vector>

namespace VNX {

namespace ActionStepEditing {

constexpr float kMaxDelayMs = 60000.0f;
constexpr float kMaxFadeTimeMs = 60000.0f;

std::vector<VActionStep> stepsFromJson(const QJsonArray& value);

std::optional<std::vector<VActionStep>> checkedStepsFromJson(
    const QJsonArray& value, QString* error = nullptr);

QJsonArray stepsToJson(const std::vector<VActionStep>& steps);

float normalizeSetVariableValue(float value, VariableKind kind,
                        float rangeMin, float rangeMax);

}

}
