#include "ParameterBinding.h"

#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace VNX {

ParameterBinding::ParameterBinding(QObject* parent)
    : QObject(parent)
{
}

double ParameterBinding::valueFromPosition(double position) const
{
    const double normalized = std::clamp(position, 0.0, 1.0);
    return minimum_ + normalized * (maximum_ - minimum_);
}

double ParameterBinding::positionFromValue(double value) const
{
    const double range = maximum_ - minimum_;
    if (range <= 0.0) return 0.0;
    return (normalizedValue(value) - minimum_) / range;
}

int ParameterBinding::positionSteps(int smoothSteps) const
{
    if (quantum_ <= 0.0 || smoothSteps <= 0) return smoothSteps;
    const double span = maximum_ - minimum_;
    if (span <= 0.0) return smoothSteps;
    const int steps = static_cast<int>(std::lround(span / quantum_));
    return steps > 0 && steps < smoothSteps ? steps : smoothSteps;
}

int ParameterBinding::positionOf(double value, int steps) const
{
    return static_cast<int>(std::lround(positionFromValue(value) * steps));
}

void ParameterBinding::setRange(double minimum, double maximum)
{
    if (!std::isfinite(minimum) || !std::isfinite(maximum)) return;
    if (maximum < minimum) std::swap(minimum, maximum);
    if (qFuzzyCompare(minimum_ + 1.0, minimum + 1.0)
        && qFuzzyCompare(maximum_ + 1.0, maximum + 1.0))
        return;

    interruptEdit();
    minimum_ = minimum;
    maximum_ = maximum;
    const double normalized = normalizedValue(value_);
    const bool valueChanged = !qFuzzyCompare(value_ + 1.0, normalized + 1.0);
    value_ = normalized;
    emit metadataChanged();
    if (valueChanged)
        emit valueUpdated(value_);
}

void ParameterBinding::setMinimum(double minimum)
{
    setRange(minimum, std::max(minimum, maximum_));
}

void ParameterBinding::setMaximum(double maximum)
{
    setRange(std::min(minimum_, maximum), maximum);
}

void ParameterBinding::setDefaultValue(double value)
{
    if (!std::isfinite(value) || qFuzzyCompare(defaultValue_ + 1.0, value + 1.0)) return;
    defaultValue_ = value;
    emit metadataChanged();
}

void ParameterBinding::setDecimals(int decimals)
{
    const int normalized = std::clamp(decimals, 0, 15);
    if (decimals_ == normalized) return;
    decimals_ = normalized;
    emit metadataChanged();
}

void ParameterBinding::setQuantum(double quantum)
{
    if (!std::isfinite(quantum) || quantum < 0.0) return;
    if (qFuzzyCompare(quantum_ + 1.0, quantum + 1.0)) return;
    quantum_ = quantum;
    const double normalized = normalizedValue(value_);
    const bool valueChanged = !qFuzzyCompare(value_ + 1.0, normalized + 1.0);
    value_ = normalized;
    emit metadataChanged();
    if (valueChanged)
        emit valueUpdated(value_);
}

void ParameterBinding::setDisplayScale(double scale)
{
    if (!std::isfinite(scale) || scale <= 0.0) return;
    if (qFuzzyCompare(displayScale_, scale)) return;
    displayScale_ = scale;
    emit metadataChanged();
}

void ParameterBinding::setDiscreteSelection(bool enabled)
{
    if (discreteSelection_ == enabled) return;
    discreteSelection_ = enabled;
    emit metadataChanged();
}

void ParameterBinding::setUnit(const QString& unit)
{
    if (unit_ == unit) return;
    unit_ = unit;
    emit metadataChanged();
}

void ParameterBinding::setName(const QString& name)
{
    if (name_ == name) return;
    name_ = name;
    emit metadataChanged();
}

void ParameterBinding::setAnnotation(std::function<QString(double)> annotation)
{
    annotation_ = std::move(annotation);
    emit metadataChanged();
}

QString ParameterBinding::annotationFor(double value) const
{
    return annotation_ ? annotation_(value) : QString();
}

void ParameterBinding::setValue(double value)
{
    interruptEdit();
    updateValue(value);
}

void ParameterBinding::commitValue(double value)
{
    if (editing_ || !std::isfinite(value)) return;
    const bool wasMixed = mixedValue_;
    mixedValue_ = false;
    if (updateValue(value) || wasMixed)
        emit valueCommitted(value_);
}

void ParameterBinding::beginEdit()
{
    if (editing_) return;
    editing_ = true;
    editStartValue_ = value_;
    emit editBegan(editStartValue_);
}

void ParameterBinding::previewEdit(double value)
{
    if (!editing_) return;
    if (updateValue(value))
        emit editPreviewed(value_);
}

void ParameterBinding::commitEdit()
{
    if (!editing_) return;
    editing_ = false;
    mixedValue_ = false;
    emit editEnded(editStartValue_, value_, true);
}

void ParameterBinding::cancelEdit()
{
    if (!editing_) return;
    const double endValue = value_;
    editing_ = false;
    emit editEnded(editStartValue_, endValue, false);
    updateValue(editStartValue_);
}

double ParameterBinding::normalizedValue(double value) const
{
    if (!std::isfinite(value)) return value_;
    if (quantum_ > 0.0)
        value = std::floor(value / quantum_ + 0.5) * quantum_;
    return std::clamp(value, minimum_, maximum_);
}

bool ParameterBinding::updateValue(double value)
{
    const double normalized = normalizedValue(value);
    if (qFuzzyCompare(value_ + 1.0, normalized + 1.0)) return false;
    value_ = normalized;
    emit valueUpdated(value_);
    return true;
}

void ParameterBinding::interruptEdit()
{
    if (!editing_) return;
    const double endValue = value_;
    editing_ = false;
    emit editEnded(editStartValue_, endValue, false);
}

}
