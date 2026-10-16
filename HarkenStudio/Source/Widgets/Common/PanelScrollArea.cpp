#include "PanelScrollArea.h"

#include "Core/QtControlSkin.h"

#include <QScrollBar>

namespace VNX {

PanelScrollArea::PanelScrollArea(QWidget* parent)
    : QScrollArea(parent)
{
    setWidgetResizable(true);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    QtControlSkin::setSemanticProperty(this, "scrollRole", QStringLiteral("panel"));
    auto* bar = verticalScrollBar();
    QtControlSkin::setSemanticProperty(bar, "scrollRole", QStringLiteral("panel"));
    connect(bar, &QScrollBar::rangeChanged, bar, [bar](int minimum, int maximum) {
        bar->setEnabled(minimum < maximum);
    });
    bar->setEnabled(bar->minimum() < bar->maximum());
}

}
