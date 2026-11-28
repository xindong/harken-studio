#include "ParameterSlider.h"

#include "ParameterBinding.h"
#include "ParameterPresenter.h"

#include <QDoubleSpinBox>
#include <QSignalBlocker>

namespace harken::studio {

ParameterSlider::ParameterSlider(const QString& label, QWidget* parent,
                                 ThemeManager* themeManager)
    : ParameterControlHost(label, ParameterPresenterContext::InspectorFloat,
                           parent, themeManager)
{
    setObjectName(QStringLiteral("ParameterSlider"));

    connect(binding(), &ParameterBinding::valueCommitted,
            this, &ParameterSlider::valueChanged);
    connect(binding(), &ParameterBinding::editBegan,
            this, &ParameterSlider::interactiveEditBegin);
    connect(binding(), &ParameterBinding::editPreviewed,
            this, &ParameterSlider::interactivePreview);
    connect(binding(), &ParameterBinding::editEnded,
            this, &ParameterSlider::interactiveEditEnd);
}

ParameterSlider::~ParameterSlider() = default;

double ParameterSlider::value() const
{
    return binding()->value();
}

double ParameterSlider::minimum() const
{
    return binding()->minimum();
}

double ParameterSlider::maximum() const
{
    return binding()->maximum();
}

QString ParameterSlider::suffix() const
{
    return binding()->unit();
}

void ParameterSlider::setRange(double minimum, double maximum, int decimals)
{
    binding()->setRange(minimum, maximum);
    binding()->setDecimals(decimals);
}

void ParameterSlider::setMinimum(double minimum)
{
    binding()->setMinimum(minimum);
}

void ParameterSlider::setMaximum(double maximum)
{
    binding()->setMaximum(maximum);
}

void ParameterSlider::setSuffix(const QString& suffix)
{
    binding()->setUnit(suffix);
}

void ParameterSlider::setDecimals(int decimals)
{
    binding()->setDecimals(decimals);
}

void ParameterSlider::setValue(double value)
{
    binding()->setValue(value);
}

void ParameterSlider::simulateValueChange(double value)
{
    binding()->commitValue(value);
}

void ParameterSlider::cancelPendingInput()
{
    binding()->cancelEdit();
    if (auto* current = presenter()) {
        if (auto* editor = current->valueEditor()) {
            QSignalBlocker blocker(editor);
            editor->setValue(binding()->toDisplayValue(binding()->value()));
        }
    }
}

}
