
#pragma once

#include "InspectorCustomizationRegistry.h"
#include "InspectorSlotKind.h"

#include <QPointer>
#include <QSize>
#include <QWidget>

#include <array>
#include <vector>

class QHBoxLayout;

namespace VNX {

class InspectorSlotStrip : public QWidget, public IFieldDecorator
{
    Q_OBJECT

public:
    explicit InspectorSlotStrip(QWidget* parent = nullptr);

    void addSlot(InspectorSlotKind kind, QWidget* widget);
    int occupiedSlotCount() const;
    QWidget* slotWidget(InspectorSlotKind kind) const;

    InspectorSlotKind slotKind() const override { return InspectorSlotKind::Binding; }
    void bind(ProjectItem* item) override;
    QWidget* detailWidget() const override;
    std::vector<QWidget*> detailWidgets() const;
    QString hoverSummary() const override;
    bool isReadOnly() const override;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    struct Slot {
        InspectorSlotKind kind = InspectorSlotKind::Binding;
        QWidget* cell = nullptr;
        QPointer<QWidget> widget;
        IFieldDecorator* decorator = nullptr;
    };

    std::array<Slot, 2> slots_{{{InspectorSlotKind::Binding},
                               {InspectorSlotKind::Attenuation}}};
    QHBoxLayout* layout_ = nullptr;
};

}
