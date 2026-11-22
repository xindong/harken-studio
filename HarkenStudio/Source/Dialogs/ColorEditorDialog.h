#pragma once

#include "Core/BaseDialog.h"

#include <QColor>
#include <array>

class QLineEdit;
class QSpinBox;
class QTimer;

namespace harken::studio {

class ColorPicker;
class ParameterBinding;

class ColorEditorDialog : public BaseDialog
{
    Q_OBJECT

public:
    explicit ColorEditorDialog(const QColor& color, QWidget* parent = nullptr);
    static QString hexText(const QColor& color);
    QColor currentColor() const { return color_; }
    void setCurrentColor(const QColor& color);

signals:
    void currentColorChanged(const QColor& color);

protected:
    bool event(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void publishColor(const QColor& color);
    void publishHsv();
    void pickScreenColor();
    bool updateScreenColor();
    void finishScreenPick(bool accept);

    QColor color_;
    qreal hue_ = 0.0;
    qreal saturation_ = 0.0;
    qreal value_ = 0.0;
    ColorPicker* picker_ = nullptr;
    std::array<QSpinBox*, 6> channels_{};
    ParameterBinding* alpha_ = nullptr;
    QLineEdit* hexEdit_ = nullptr;
    QTimer* screenPickTimer_ = nullptr;
    QColor screenPickOriginal_;
    std::array<qreal, 3> screenPickHsv_{};
    bool screenPicking_ = false;
};

}
