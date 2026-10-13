#pragma once

#include "ParameterPresenter.h"

class QDoubleSpinBox;
class QComboBox;
class QLabel;
class QSlider;

namespace VNX {

class ParameterBinding;
class ThemeManager;
class DirectJumpSlider;
class ParameterValueEditor;

class LinearSliderPresenter : public ParameterPresenter
{
    Q_OBJECT

public:
    explicit LinearSliderPresenter(ParameterBinding* binding,
                                   bool showValue,
                                   ThemeManager* themeManager,
                                   QWidget* parent = nullptr);

    ParameterPresenterId presenterId() const override;
    QWidget* primaryEditor() const override;
    QSlider* slider() const override;
    QDoubleSpinBox* valueEditor() const override;
    QComboBox* choiceEditor() const override;
    bool isGestureActive() const override;

    QDoubleSpinBox* spinBox() const;

    void setCompactValueColumn(int width) override;
    void setValueColumnStretch(int primaryStretch, int valueStretch) override;
    void applyTheme();

protected:
    bool event(QEvent* event) override;

private:
    void syncMetadata();
    void syncValue(double value);
    void onSliderChanged(int position);

    ParameterBinding* binding_;
    QLabel* label_;
    DirectJumpSlider* slider_;
    ParameterValueEditor* valueControl_;
    bool showValue_ = true;
    bool updating_ = false;
    bool compactValueColumn_ = false;
};

}
