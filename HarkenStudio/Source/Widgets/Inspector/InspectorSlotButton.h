#pragma once

#include "InspectorSlotKind.h"

#include <QToolButton>

namespace harken::studio {

class InspectorSlotButton : public QToolButton
{
    Q_OBJECT

public:
    explicit InspectorSlotButton(InspectorSlotKind kind, QWidget* parent = nullptr);

    bool isBound() const { return bound_; }
    void setBound(bool bound);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    InspectorSlotKind kind_;
    bool bound_ = false;
};

}
