#include "RotaryKnobPresenter.h"
#include "DecimalSpinBox.h"

#include "ParameterBinding.h"
#include "RotaryKnobRenderer.h"
#include "Core/FontManager.h"
#include "Core/QtControlSkin.h"
#include "Core/ReentrancyGuard.h"
#include "Core/ThemeManager.h"

#include <QDial>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QLabel>
#include <QPainter>
#include <QVBoxLayout>

#include <algorithm>

namespace VNX {

namespace {

constexpr int kDialRangeMaximum = 1000;

}

class RotaryKnobControl final : public QDial
{
public:
    explicit RotaryKnobControl(ThemeManager* themeManager,
                               QWidget* parent = nullptr)
        : QDial(parent)
        , themeManager_(themeManager)
    {
        setNotchesVisible(false);
        setWrapping(false);
        const int extent = themeManager_->metrics().parameter.knobExtent;
        setMinimumSize(extent, extent);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        setFocusPolicy(Qt::StrongFocus);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        RotaryKnobRenderer::render(
            painter, themeManager_->snapshot()->ui,
            RotaryKnobRenderFrame{
                QRectF(rect()).adjusted(2.0, 2.0, -2.0, -2.0),
                value() / static_cast<double>(std::max(1, maximum())),
                isEnabled(), underMouse(), isSliderDown()});
    }

    void enterEvent(QEnterEvent* event) override
    {
        QDial::enterEvent(event);
        update();
    }

    void leaveEvent(QEvent* event) override
    {
        QDial::leaveEvent(event);
        update();
    }

private:
    ThemeManager* themeManager_;
};

RotaryKnobPresenter::RotaryKnobPresenter(ParameterBinding* binding,
                                         ThemeManager* themeManager,
                                         QWidget* parent)
    : ParameterPresenter(parent)
    , binding_(binding)
    , themeManager_(themeManager)
    , label_(new QLabel(this))
    , knob_(new RotaryKnobControl(themeManager_, this))
    , spinBox_(new DecimalSpinBox(this))
{
    const auto& metrics = themeManager_->metrics();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(metrics.spacingXSmall, metrics.spacingXSmall,
                               metrics.spacingXSmall, metrics.spacingXSmall);
    layout->setSpacing(metrics.spacingXSmall);
    layout->setAlignment(Qt::AlignHCenter);

    label_->setAlignment(Qt::AlignCenter);
    label_->setBuddy(knob_);
    layout->addWidget(label_);
    layout->addWidget(knob_, 0, Qt::AlignHCenter);

    spinBox_->setMinimumWidth(metrics.parameter.valueMinimumWidth);
    spinBox_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    spinBox_->setAlignment(Qt::AlignCenter);
    spinBox_->setKeyboardTracking(false);
    layout->addWidget(spinBox_, 0, Qt::AlignHCenter);

    connect(knob_, &QDial::valueChanged, this, [this](int position) {
        if (updating_)
            return;
        const double value = binding_->valueFromPosition(
            position / static_cast<double>(knob_->maximum()));
        if (binding_->isEditing())
            binding_->previewEdit(value);
        else
            binding_->commitValue(value);
    });
    connect(knob_, &QDial::sliderPressed, binding_, &ParameterBinding::beginEdit);
    connect(knob_, &QDial::sliderReleased, binding_, &ParameterBinding::commitEdit);
    connect(spinBox_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &RotaryKnobPresenter::onSpinBoxChanged);
    connect(binding_, &ParameterBinding::valueUpdated, this,
            &RotaryKnobPresenter::syncValue);
    connect(binding_, &ParameterBinding::metadataChanged, this,
            &RotaryKnobPresenter::syncMetadata);

    syncMetadata();
    syncValue(binding_->value());
    applyTheme();
}

QWidget* RotaryKnobPresenter::primaryEditor() const
{
    return knob_;
}

bool RotaryKnobPresenter::isGestureActive() const
{
    return knob_->isSliderDown();
}

void RotaryKnobPresenter::setCompactValueColumn(int width)
{
    label_->hide();
    spinBox_->setMinimumWidth(0);
    spinBox_->setFixedWidth(width);
    spinBox_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    spinBox_->setFont(FontManager::instance().getFont(FontRole::Mono));
    QtControlSkin::setSemanticProperty(spinBox_, "fieldRole",
                                      QStringLiteral("inspectorValue"));
}

void RotaryKnobPresenter::syncMetadata()
{
    const QString name = binding_->name();
    ReentrancyGuard guard(updating_);
    label_->setText(name);
    label_->setVisible(!name.isEmpty());
    spinBox_->setDecimals(binding_->decimals());
    spinBox_->setRange(binding_->toDisplayValue(binding_->minimum()),
                      binding_->toDisplayValue(binding_->maximum()));
    spinBox_->setValue(binding_->toDisplayValue(binding_->value()));
    const int steps = binding_->positionSteps(kDialRangeMaximum);
    knob_->setRange(0, steps);
    knob_->setPageStep(std::max(1, steps / 100));
    knob_->setValue(binding_->positionOf(binding_->value(), steps));
    setAccessibleName(name);
    label_->setAccessibleName(name);
    knob_->setAccessibleName(name);
    spinBox_->setAccessibleName(name);
}

void RotaryKnobPresenter::syncValue(double value)
{
    if (updating_)
        return;
    ReentrancyGuard guard(updating_);
    spinBox_->setValue(binding_->toDisplayValue(value));
    knob_->setValue(binding_->positionOf(value, knob_->maximum()));
}

void RotaryKnobPresenter::onSpinBoxChanged(double value)
{
    if (!updating_)
        binding_->commitValue(binding_->fromDisplayValue(value));
}

void RotaryKnobPresenter::applyTheme()
{
    QtControlSkin::setSemanticProperty(label_, "textRole",
                                      QStringLiteral("formLabel"));
    QtControlSkin::setSemanticProperty(spinBox_, "fieldRole",
                                      QStringLiteral("compact"));
    knob_->update();
}

}
