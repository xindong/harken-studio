
#include "RehearsalSlider.h"

#include "ParameterBinding.h"
#include "ParameterPresenter.h"
#include "Core/Components/VVariable.h"
#include "Core/ThemeManager.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QToolTip>

#include <cmath>

namespace VNX {

RehearsalSlider::RehearsalSlider(QWidget* parent)
    : ParameterSlider(QString(), parent)
{
    setObjectName(QStringLiteral("variableRehearsalSliderHost"));
    setCompactValueColumn(80);
    setPresenterOverride(ParameterPresenterId::SliderWithValue);
}

void RehearsalSlider::attachInnerSlider()
{
    QSlider* inner = presenter() ? presenter()->slider() : nullptr;
    if (!inner) return;
    inner->setObjectName(QStringLiteral("variableRehearsalSlider"));
    if (!inner->property("rehearsalFilterInstalled").toBool()) {
        inner->installEventFilter(this);
        inner->setProperty("rehearsalFilterInstalled", true);
    }
    inner->setMouseTracking(true);
}

void RehearsalSlider::configure(const VVariable& variable)
{
    const bool step = variable.kind == VariableKind::Step;
    const auto form = VariableSemantics::editFormForKind(variable.kind);
    lo_ = variable.rangeMin;
    hi_ = variable.rangeMax;
    labels_ = QVector<VariableLabel>(variable.labels.cbegin(), variable.labels.cend());
    setRange(lo_, hi_ > lo_ ? hi_ : lo_, form.decimals);
    setDecimals(form.decimals);
    binding()->setQuantum(form.quantum);
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

    attachInnerSlider();
    if (QSlider* inner = presenter() ? presenter()->slider() : nullptr) {
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

int RehearsalSlider::anchorPosition(const QSlider* slider, float anchor) const
{
    if (!slider || anchor < lo_ || anchor > hi_ || hi_ <= lo_) return -1;
    QStyleOptionSlider option;
    option.initFrom(slider);
    option.orientation = slider->orientation();
    option.minimum = slider->minimum();
    option.maximum = slider->maximum();
    option.sliderPosition = slider->sliderPosition();
    option.sliderValue = slider->value();
    option.singleStep = slider->singleStep();
    option.pageStep = slider->pageStep();
    option.upsideDown = slider->invertedAppearance();
    const QRect groove = slider->style()->subControlRect(
        QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, slider);
    const QRect handle = slider->style()->subControlRect(
        QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, slider);
    const int available = groove.width() - handle.width();
    if (available <= 0) return -1;
    const float t = (anchor - lo_) / (hi_ - lo_);
    return groove.x() + handle.width() / 2 + static_cast<int>(std::lround(t * available));
}

void RehearsalSlider::paintAnchors(QSlider* slider, QPaintEvent*)
{
    if (!slider || labels_.isEmpty() || hi_ <= lo_) return;
    QPainter painter(slider);
    painter.setPen(ThemeManager::instance().colors().accent);
    for (const VariableLabel& label : labels_) {
        const int x = anchorPosition(slider, static_cast<float>(label.anchor));
        if (x >= 0)
            painter.drawLine(x, slider->height() - 5, x, slider->height() - 1);
    }
}

void RehearsalSlider::updateAnchorTooltip(QSlider* slider, QMouseEvent* event)
{
    if (!slider || !event) return;
    const VariableLabel* nearest = nullptr;
    int distance = 7;
    for (const VariableLabel& label : labels_) {
        if (label.name.isEmpty()) continue;
        const int x = anchorPosition(slider, static_cast<float>(label.anchor));
        if (x < 0) continue;
        const int delta = std::abs(event->position().toPoint().x() - x);
        if (delta < distance) {
            distance = delta;
            nearest = &label;
        }
    }
    if (nearest) {
        QToolTip::showText(event->globalPosition().toPoint(),
                           tr("%1 (%2)").arg(nearest->name).arg(nearest->anchor),
                           slider);
    } else {
        QToolTip::hideText();
    }
}

bool RehearsalSlider::eventFilter(QObject* watched, QEvent* event)
{
    auto* slider = qobject_cast<QSlider*>(watched);
    if (slider && presenter() && slider == presenter()->slider()) {
        if (event->type() == QEvent::Paint) {
            const bool filtered = ParameterSlider::eventFilter(watched, event);
            paintAnchors(slider, static_cast<QPaintEvent*>(event));
            return filtered;
        }
        if (event->type() == QEvent::MouseMove)
            updateAnchorTooltip(slider, static_cast<QMouseEvent*>(event));
        else if (event->type() == QEvent::Leave)
            QToolTip::hideText();
    }
    return ParameterSlider::eventFilter(watched, event);
}

}
