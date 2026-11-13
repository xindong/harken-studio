
#include "RehearsalSlider.h"

#include "ParameterBinding.h"
#include "ParameterPresenter.h"
#include "Core/Components/VVariable.h"
#include "Core/ThemeManager.h"
#include "Core/VariableSemantics.h"

#include <QSlider>

namespace VNX {

RehearsalSlider::RehearsalSlider(QWidget* parent)
    : ParameterSlider(QString(), parent)
{
    setObjectName(QStringLiteral("variableRehearsalSliderHost"));
    ThemeManager::instance().bindMetrics(this, [this](const ThemeMetrics& metrics) {
        setCompactValueColumn(metrics.parameter.compactValueMinimumWidth);
    });
    setPresenterOverride(ParameterPresenterId::SliderWithValue);
}

void RehearsalSlider::configure(const VVariable& variable)
{
    const bool step = variable.kind == VariableKind::Step;
    const auto form = VariableSemantics::editFormForKind(variable.kind);
    const float lo = variable.rangeMin;
    const float hi = variable.rangeMax;
    setRange(lo, hi > lo ? hi : lo, form.decimals);
    setDecimals(form.decimals);
    binding()->setQuantum(form.quantum);
    binding()->setDefaultValue(variable.defaultValue);
    binding()->setAnnotation([labels = variable.labels](double value) {
        return VariableSemantics::labelTextForValue(
            static_cast<float>(value), labels).value_or(QString());
    });
    binding()->setDiscreteSelection(step);

    setProperty("variableKind", step ? QStringLiteral("Step")
                                     : QStringLiteral("Smooth"));
    setProperty("labelAnchorCount", static_cast<int>(variable.labels.size()));
    QStringList anchors;
    for (const VariableLabel& label : variable.labels)
        anchors.push_back(tr("%1 · %2").arg(label.anchor).arg(label.name));
    setProperty("labelAnchors", anchors);
    setAccessibleDescription(anchors.join(QStringLiteral(", ")));

    if (QSlider* inner = presenter() ? presenter()->slider() : nullptr) {
        inner->setObjectName(QStringLiteral("variableRehearsalSlider"));
        inner->setProperty("variableKind",
                           step ? QStringLiteral("Step") : QStringLiteral("Smooth"));
        inner->setProperty("labelAnchorCount",
                           static_cast<int>(variable.labels.size()));
        inner->setProperty("labelAnchors", anchors);
        inner->setAccessibleDescription(anchors.join(QStringLiteral(", ")));
        if (step) {
            inner->setProperty("variableRehearsalSlider", QStringLiteral("Step"));
        }
        inner->update();
    }
}

float RehearsalSlider::rawValue() const
{
    return static_cast<float>(value());
}

void RehearsalSlider::setRawValue(float raw)
{
    setValue(raw);
}

}
