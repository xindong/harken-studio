#pragma once

#include "ParameterPresenter.h"

class QDoubleSpinBox;
class QLabel;

namespace VNX {

class ParameterBinding;
class RotaryKnobControl;
class ThemeManager;

class RotaryKnobPresenter : public ParameterPresenter
{
    Q_OBJECT

public:
    explicit RotaryKnobPresenter(ParameterBinding* binding,
                                 ThemeManager* themeManager,
                                 QWidget* parent = nullptr);

    ParameterPresenterId presenterId() const override
    {
        return ParameterPresenterId::RotaryKnob;
    }
    QWidget* primaryEditor() const override;
    QDoubleSpinBox* valueEditor() const override { return spinBox_; }
    RotaryKnobControl* knob() const { return knob_; }
    bool isGestureActive() const override;
    void setCompactValueColumn(int width) override;

private:
    void syncMetadata();
    void syncValue(double value);
    void onSpinBoxChanged(double value);
    void applyTheme();

    ParameterBinding* binding_;
    ThemeManager* themeManager_;
    QLabel* label_;
    RotaryKnobControl* knob_;
    QDoubleSpinBox* spinBox_;
    bool updating_ = false;
};

}
