#include "LinearSliderPresenter.h"

#include "DirectJumpSlider.h"
#include "ParameterBinding.h"
#include "ParameterValueEditor.h"
#include "TableWidget.h"
#include "Core/QtControlSkin.h"
#include "Core/ReentrancyGuard.h"
#include "Core/ThemeManager.h"

#include <QDoubleSpinBox>
#include <QComboBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>

#include <algorithm>

namespace VNX {

namespace {

// Resolution a continuous parameter keeps; a quantized one spans one position
// per step instead.
constexpr int kSliderRangeMaximum = 1000;

}

LinearSliderPresenter::LinearSliderPresenter(ParameterBinding* binding,
                                             bool showValue,
                                             ThemeManager* themeManager,
                                             QWidget* parent)
    : ParameterPresenter(parent)
    , binding_(binding)
    , label_(new QLabel(this))
    , slider_(new DirectJumpSlider(Qt::Horizontal, this))
    , valueControl_(showValue ? new ParameterValueEditor(binding, themeManager, this) : nullptr)
    , showValue_(showValue)
{
    auto& theme = *themeManager;
    const auto& metrics = theme.metrics();

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, metrics.spacingXSmall / 2, 0,
                               metrics.spacingXSmall / 2);
    layout->setSpacing(metrics.spacingSmall);
    for (const QWidget* scope = parent; scope; scope = scope->parentWidget()) {
        if (qobject_cast<const TableCell*>(scope)) {
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(metrics.table.cellPaddingX * 2);
            break;
        }
    }

    label_->setMinimumWidth(metrics.parameter.labelMinimumWidth
                            + metrics.spacingSmall);
    label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    layout->addWidget(label_);

    slider_->setSingleStep(1);
    for (const QWidget* scope = parent; scope; scope = scope->parentWidget()) {
        if (scope->property("sliderStyle").toString() == QStringLiteral("compact")) {
            slider_->setCursor(Qt::SizeHorCursor);
            break;
        }
    }
    layout->addWidget(slider_, 1);

    if (valueControl_)
    {
        valueControl_->setMinimumWidth(metrics.parameter.compactValueMinimumWidth);
        layout->addWidget(valueControl_);
    }
    else
    {
        label_->setBuddy(slider_);
        label_->hide();
    }

    connect(slider_, &QSlider::valueChanged, this,
            &LinearSliderPresenter::onSliderChanged);
    connect(slider_, &QSlider::sliderPressed, binding_,
            &ParameterBinding::beginEdit);
    connect(slider_, &QSlider::sliderReleased, binding_,
            &ParameterBinding::commitEdit);
    connect(binding_, &ParameterBinding::valueUpdated, this,
            &LinearSliderPresenter::syncValue);
    connect(binding_, &ParameterBinding::metadataChanged, this,
            &LinearSliderPresenter::syncMetadata);

    syncMetadata();
    syncValue(binding_->value());
    applyTheme();
}

ParameterPresenterId LinearSliderPresenter::presenterId() const
{
    return showValue_ ? ParameterPresenterId::SliderWithValue
                      : ParameterPresenterId::HorizontalSlider;
}

QWidget* LinearSliderPresenter::primaryEditor() const
{
    return slider_;
}

QSlider* LinearSliderPresenter::slider() const
{
    return slider_;
}

QDoubleSpinBox* LinearSliderPresenter::valueEditor() const
{
    return valueControl_ ? valueControl_->valueEditor() : nullptr;
}

QComboBox* LinearSliderPresenter::choiceEditor() const
{
    return valueControl_ ? valueControl_->choiceEditor() : nullptr;
}

QDoubleSpinBox* LinearSliderPresenter::spinBox() const
{
    return valueControl_ ? valueControl_->spinBox() : nullptr;
}

bool LinearSliderPresenter::isGestureActive() const
{
    return slider_->isSliderDown();
}

void LinearSliderPresenter::setCompactValueColumn(int width)
{
    compactValueColumn_ = true;
    label_->hide();
    if (!valueControl_)
        return;
    valueControl_->setCompact(true);
    valueControl_->setFixedWidth(width);
    applyTheme();
}

void LinearSliderPresenter::setValueColumnStretch(int primaryStretch, int valueStretch)
{
    if (!valueControl_)
        return;
    valueControl_->setMinimumWidth(0);
    valueControl_->setMaximumWidth(QWIDGETSIZE_MAX);
    valueControl_->setSizePolicy(QSizePolicy::Ignored, valueControl_->sizePolicy().verticalPolicy());
    slider_->setSizePolicy(QSizePolicy::Ignored, slider_->sizePolicy().verticalPolicy());
    auto* row = qobject_cast<QHBoxLayout*>(layout());
    row->setStretchFactor(slider_, primaryStretch);
    row->setStretchFactor(valueControl_, valueStretch);
}

void LinearSliderPresenter::applyTheme()
{
    QtControlSkin::setSemanticProperty(label_, "textRole",
                                      QStringLiteral("formLabel"));
    QtControlSkin::setSemanticProperty(slider_, "sliderRole",
                                      QStringLiteral("parameter"));
    if (valueControl_)
        valueControl_->setCompact(compactValueColumn_);
}

bool LinearSliderPresenter::event(QEvent* event)
{
    if (event->type() == QEvent::ToolTipChange && valueControl_)
        valueControl_->setToolTip(toolTip());
    return ParameterPresenter::event(event);
}

void LinearSliderPresenter::syncMetadata()
{
    const QString name = binding_->name();
    ReentrancyGuard guard(updating_);
    label_->setText(name);
    label_->setVisible(showValue_ && !compactValueColumn_ && !name.isEmpty());
    if (valueControl_)
        label_->setBuddy(binding_->discreteSelection()
            ? static_cast<QWidget*>(valueControl_->comboBox())
            : static_cast<QWidget*>(valueControl_->spinBox()));

    setAccessibleName(name);
    slider_->setAccessibleName(name);
    label_->setAccessibleName(name);
    const int steps = binding_->positionSteps(kSliderRangeMaximum);
    slider_->setRange(0, steps);
    slider_->setPageStep(std::max(1, steps / 100));
    slider_->setValue(binding_->positionOf(binding_->value(), steps));
}

void LinearSliderPresenter::syncValue(double value)
{
    if (updating_)
        return;
    ReentrancyGuard guard(updating_);
    slider_->setValue(binding_->positionOf(value, slider_->maximum()));
}

void LinearSliderPresenter::onSliderChanged(int position)
{
    if (updating_)
        return;
    const double value = binding_->valueFromPosition(
        position / static_cast<double>(slider_->maximum()));
    if (binding_->isEditing())
        binding_->previewEdit(value);
    else
        binding_->commitValue(value);
}

}
