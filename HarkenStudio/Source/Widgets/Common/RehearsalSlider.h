
#pragma once

#include "Core/VariableSemantics.h"

#include "ParameterSlider.h"

#include <QEvent>
#include <QVector>

class QMouseEvent;
class QPaintEvent;
class QSlider;

namespace VNX {

class VVariable;

class RehearsalSlider : public ParameterSlider
{
    Q_OBJECT

public:
    explicit RehearsalSlider(QWidget* parent = nullptr);

    void configure(const VVariable& variable);
    float rawValue() const;
    void setRawValue(float value);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void attachInnerSlider();
    int anchorPosition(const QSlider* slider, float anchor) const;
    void paintAnchors(QSlider* slider, QPaintEvent* event);
    void updateAnchorTooltip(QSlider* slider, QMouseEvent* event);

    QVector<VariableLabel> labels_;
    float lo_ = 0.0f;
    float hi_ = 0.0f;
};

}
