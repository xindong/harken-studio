#include "VerticalFaderPresenter.h"
#include "DecimalSpinBox.h"

#include "DirectJumpSlider.h"
#include "ParameterBinding.h"
#include "Core/FontManager.h"
#include "Core/QtControlSkin.h"
#include "Core/ReentrancyGuard.h"
#include "Core/ThemeManager.h"

#include <QDoubleSpinBox>
#include <QLabel>
#include <QSlider>
#include <QVBoxLayout>

#include <algorithm>

namespace VNX {

namespace {

constexpr int kSliderRangeMaximum = 1000;

}

VerticalFaderPresenter::VerticalFaderPresenter(ParameterBinding* binding,
                                               ThemeManager* themeManager,
                                               QWidget* parent)
    : ParameterPresenter(parent)
    , binding_(binding)
    , themeManager_(themeManager)
    , label_(new QLabel(this))
    , slider_(new DirectJumpSlider(Qt::Vertical, this))
    , spinBox_(new DecimalSpinBox(this))
{
    const auto& metrics = themeManager_->metrics();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(metrics.spacingXSmall, metrics.spacingXSmall,
                               metrics.spacingXSmall, metrics.spacingXSmall);
    layout->setSpacing(metrics.spacingSmall);
    layout->setAlignment(Qt::AlignHCenter);

    label_->setAlignment(Qt::AlignCenter);
    label_->setBuddy(spinBox_);
    layout->addWidget(label_);

    slider_->setMinimumHeight(metrics.parameter.faderMinimumHeight);
    slider_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    layout->addWidget(slider_, 1, Qt::AlignHCenter);

    spinBox_->setMinimumWidth(metrics.parameter.valueMinimumWidth);
    spinBox_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    spinBox_->setAlignment(Qt::AlignCenter);
    spinBox_->setKeyboardTracking(false);
    layout->addWidget(spinBox_, 0, Qt::AlignHCenter);

    connect(slider_, &QSlider::valueChanged, this,
            &VerticalFaderPresenter::onSliderChanged);
    connect(slider_, &QSlider::sliderPressed, binding_,
            &ParameterBinding::beginEdit);
    connect(slider_, &QSlider::sliderReleased, binding_,
            &ParameterBinding::commitEdit);
    connect(spinBox_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &VerticalFaderPresenter::onSpinBoxChanged);
    connect(binding_, &ParameterBinding::valueUpdated, this,
            &VerticalFaderPresenter::syncValue);
    connect(binding_, &ParameterBinding::metadataChanged, this,
            &VerticalFaderPresenter::syncMetadata);

    syncMetadata();
    syncValue(binding_->value());
    applyTheme();
}

QWidget* VerticalFaderPresenter::primaryEditor() const
{
    return slider_;
}

bool VerticalFaderPresenter::isGestureActive() const
{
    return slider_->isSliderDown();
}

void VerticalFaderPresenter::setCompactValueColumn(int width)
{
    label_->hide();
    spinBox_->setMinimumWidth(0);
    spinBox_->setFixedWidth(width);
    spinBox_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    spinBox_->setFont(FontManager::instance().getFont(FontRole::Mono));
    QtControlSkin::setSemanticProperty(spinBox_, "fieldRole",
                                      QStringLiteral("inspectorValue"));
}

void VerticalFaderPresenter::syncMetadata()
{
    const QString name = binding_->name();
    ReentrancyGuard guard(updating_);
    label_->setText(name);
    label_->setVisible(!name.isEmpty());
    spinBox_->setDecimals(binding_->decimals());
    spinBox_->setRange(binding_->toDisplayValue(binding_->minimum()),
                      binding_->toDisplayValue(binding_->maximum()));
    spinBox_->setValue(binding_->toDisplayValue(binding_->value()));
    const int steps = binding_->positionSteps(kSliderRangeMaximum);
    slider_->setRange(0, steps);
    slider_->setPageStep(std::max(1, steps / 100));
    slider_->setValue(binding_->positionOf(binding_->value(), steps));
    setAccessibleName(name);
    label_->setAccessibleName(name);
    slider_->setAccessibleName(name);
    spinBox_->setAccessibleName(name);
}

void VerticalFaderPresenter::syncValue(double value)
{
    if (updating_)
        return;
    ReentrancyGuard guard(updating_);
    spinBox_->setValue(binding_->toDisplayValue(value));
    slider_->setValue(binding_->positionOf(value, slider_->maximum()));
}

void VerticalFaderPresenter::onSliderChanged(int position)
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

void VerticalFaderPresenter::onSpinBoxChanged(double value)
{
    if (!updating_)
        binding_->commitValue(binding_->fromDisplayValue(value));
}

void VerticalFaderPresenter::applyTheme()
{
    QtControlSkin::setSemanticProperty(label_, "textRole",
                                      QStringLiteral("formLabel"));
    QtControlSkin::setSemanticProperty(slider_, "sliderRole",
                                      QStringLiteral("parameter"));
    QtControlSkin::setSemanticProperty(spinBox_, "fieldRole",
                                      QStringLiteral("compact"));
}

}
