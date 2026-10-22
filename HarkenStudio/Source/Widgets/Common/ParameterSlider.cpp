#include "ParameterSlider.h"

#include "ParameterBinding.h"
#include "ParameterPresenter.h"

#include <QDoubleSpinBox>
#include <QComboBox>
#include <QLineEdit>
#include <QSlider>
#include <QSignalBlocker>
#include <QStyle>

namespace VNX {

ParameterSlider::ParameterSlider(const QString& label, QWidget* parent,
                                 ThemeManager* themeManager)
    : ParameterControlHost(label, ParameterPresenterContext::InspectorFloat,
                           parent, themeManager)
{
    setObjectName(QStringLiteral("ParameterSlider"));

    connect(binding(), &ParameterBinding::valueCommitted,
            this, [this](double value) {
        const bool wasMixed = mixedValue_;
        onMixedInteractionStarted();
        if (wasMixed)
            clearMixedDisplay();
        emit valueChanged(value);
    });
    connect(binding(), &ParameterBinding::editBegan,
            this, [this](double startValue) {
        onMixedInteractionStarted();
        emit interactiveEditBegin(startValue);
    });
    connect(binding(), &ParameterBinding::editPreviewed,
            this, &ParameterSlider::interactivePreview);
    connect(binding(), &ParameterBinding::editEnded,
            this, &ParameterSlider::interactiveEditEnd);
    presenterInstalled();
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
    if (mixedValue_)
        setMixedValue(false);
    binding()->setValue(value);
}

void ParameterSlider::simulateValueChange(double value)
{
    binding()->commitValue(value);
}

void ParameterSlider::setMixedValue(bool mixed)
{
    binding()->setMixedValue(mixed);
    mixedTextEdited_ = false;
    if (mixedValue_ == mixed) {
        if (mixedValue_)
            applyMixedDisplay();
        return;
    }
    mixedValue_ = mixed;
    if (mixedValue_)
        applyMixedDisplay();
    else
        clearMixedDisplay();
}

void ParameterSlider::cancelPendingInput()
{
    mixedTextEdited_ = false;
    binding()->cancelEdit();
    if (auto* current = presenter()) {
        if (auto* editor = current->valueEditor()) {
            QSignalBlocker blocker(editor);
            editor->setValue(binding()->toDisplayValue(binding()->value()));
        }
    }
    if (mixedValue_)
        applyMixedDisplay();
}

void ParameterSlider::applyMixedDisplay()
{
    ParameterPresenter* current = presenter();
    if (!current)
        return;
    if (auto* editor = current->valueEditor()) {
        QSignalBlocker blocker(editor);
        editor->setProperty("mixedAcrossPlatforms", true);
        editor->style()->unpolish(editor);
        editor->style()->polish(editor);
        editor->clear();
        if (auto* line = editor->findChild<QLineEdit*>()) {
            line->setPlaceholderText(tr("Mixed"));
            line->clear();
        }
    }
    if (auto* editor = current->choiceEditor()) {
        QSignalBlocker blocker(editor);
        editor->setPlaceholderText(tr("Mixed"));
        editor->setCurrentIndex(-1);
    }
    if (auto* slider = current->slider()) {
        QSignalBlocker blocker(slider);
        const int mid = (slider->minimum() + slider->maximum()) / 2;
        slider->setSliderPosition(mid);
        slider->setProperty("mixedAcrossPlatforms", true);
        slider->style()->unpolish(slider);
        slider->style()->polish(slider);
    }
}

void ParameterSlider::clearMixedDisplay()
{
    ParameterPresenter* current = presenter();
    if (!current)
        return;
    if (auto* editor = current->valueEditor()) {
        QSignalBlocker blocker(editor);
        editor->setValue(binding()->toDisplayValue(binding()->value()));
        if (auto* line = editor->findChild<QLineEdit*>())
            line->setPlaceholderText(QString());
        editor->setProperty("mixedAcrossPlatforms", false);
        editor->style()->unpolish(editor);
        editor->style()->polish(editor);
    }
    if (auto* slider = current->slider()) {
        QSignalBlocker blocker(slider);
        slider->setValue(binding()->positionOf(binding()->value(), slider->maximum()));
        slider->setProperty("mixedAcrossPlatforms", false);
        slider->style()->unpolish(slider);
        slider->style()->polish(slider);
    }
    if (auto* editor = current->choiceEditor()) {
        QSignalBlocker blocker(editor);
        editor->setPlaceholderText(QString());
        editor->setCurrentIndex(editor->findData(binding()->value()));
    }
}

void ParameterSlider::presenterInstalled()
{
    mixedTextEdited_ = false;
    if (auto* current = presenter()) {
        if (auto* editor = current->valueEditor()) {
            if (auto* line = editor->findChild<QLineEdit*>())
                connect(line, &QLineEdit::textEdited, this, [this] {
                    if (mixedValue_) mixedTextEdited_ = true;
                });
            connect(editor, &QDoubleSpinBox::editingFinished, this, [this, editor] {
                if (!isEnabled() || !mixedValue_ || !mixedTextEdited_) return;
                mixedTextEdited_ = false;
                binding()->commitValue(binding()->fromDisplayValue(editor->value()));
            });
        }
    }
    if (mixedValue_)
        applyMixedDisplay();
}

void ParameterSlider::onMixedInteractionStarted()
{
    binding()->setMixedValue(false);
    mixedTextEdited_ = false;
    if (!mixedValue_)
        return;
    mixedValue_ = false;
    ParameterPresenter* current = presenter();
    if (!current)
        return;
    if (auto* editor = current->valueEditor()) {
        if (auto* line = editor->findChild<QLineEdit*>())
            line->setPlaceholderText(QString());
        editor->setProperty("mixedAcrossPlatforms", false);
        editor->style()->unpolish(editor);
        editor->style()->polish(editor);
    }
    if (auto* slider = current->slider()) {
        slider->setProperty("mixedAcrossPlatforms", false);
        slider->style()->unpolish(slider);
        slider->style()->polish(slider);
    }
    if (auto* editor = current->choiceEditor())
        editor->setPlaceholderText(QString());
}

}
