#pragma once

#include <QPointer>
#include <QWidget>

class QComboBox;
class QDoubleSpinBox;

namespace VNX {

class DiscreteValueModel;
class ParameterBinding;
class ThemeManager;

class ParameterValueEditor : public QWidget
{
    Q_OBJECT

public:
    explicit ParameterValueEditor(ParameterBinding* binding, ThemeManager* themeManager,
                                  QWidget* parent = nullptr);

    QDoubleSpinBox* spinBox() const { return spinBox_; }
    QComboBox* comboBox() const { return comboBox_; }
    QDoubleSpinBox* valueEditor() const;
    QComboBox* choiceEditor() const;
    QSize sizeHint() const override;
    void setCompact(bool compact);
    void cancelPendingInput();

protected:
    bool event(QEvent* event) override;

private:
    void syncMetadata();
    void syncValue(double value);
    void syncToolTips();

    QPointer<ParameterBinding> binding_;
    ThemeManager* themeManager_;
    QDoubleSpinBox* spinBox_;
    QComboBox* comboBox_;
    DiscreteValueModel* choiceModel_;
    bool updating_ = false;
    bool compact_ = false;
};

}
