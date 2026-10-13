#pragma once

#include "Core/ParameterPresenterRegistry.h"

#include <QWidget>

class QDoubleSpinBox;
class QComboBox;
class QSlider;

namespace VNX {

class ParameterPresenter : public QWidget
{
    Q_OBJECT

public:
    using QWidget::QWidget;

    virtual ParameterPresenterId presenterId() const = 0;
    virtual QWidget* primaryEditor() const = 0;
    virtual QSlider* slider() const { return nullptr; }
    virtual QDoubleSpinBox* valueEditor() const { return nullptr; }
    virtual QComboBox* choiceEditor() const { return nullptr; }
    virtual bool isGestureActive() const { return false; }
    QDoubleSpinBox* spinBox() const { return valueEditor(); }
    virtual void setCompactValueColumn(int width) { Q_UNUSED(width); }
    virtual void setValueColumnStretch(int, int) {}
};

}
