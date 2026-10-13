#pragma once

#include "ParameterPresenter.h"

class QDoubleSpinBox;
class QLabel;
class QSlider;

namespace VNX {

class ParameterBinding;
class ThemeManager;

class VerticalFaderPresenter : public ParameterPresenter
{
    Q_OBJECT

public:
    explicit VerticalFaderPresenter(ParameterBinding* binding,
                                    ThemeManager* themeManager,
                                    QWidget* parent = nullptr);

    ParameterPresenterId presenterId() const override
    {
        return ParameterPresenterId::VerticalFader;
    }
    QWidget* primaryEditor() const override;
    QSlider* slider() const override { return slider_; }
    QDoubleSpinBox* valueEditor() const override { return spinBox_; }
    bool isGestureActive() const override;
    void setCompactValueColumn(int width) override;

private:
    void syncMetadata();
    void syncValue(double value);
    void onSliderChanged(int position);
    void onSpinBoxChanged(double value);
    void applyTheme();

    ParameterBinding* binding_;
    ThemeManager* themeManager_;
    QLabel* label_;
    QSlider* slider_;
    QDoubleSpinBox* spinBox_;
    bool updating_ = false;
};

}
