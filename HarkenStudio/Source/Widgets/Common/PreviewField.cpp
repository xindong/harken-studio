#include "PreviewField.h"

#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"

#include <QMouseEvent>
#include <QPainter>

namespace VNX {

PreviewField::PreviewField(QWidget* parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    QtControlSkin::setSemanticProperty(this, "fieldRole", QStringLiteral("preview"));
    connect(&ThemeManager::instance(), &ThemeManager::appearanceChanged,
        this, [this] { update(); });
}

void PreviewField::setDimmed(bool dimmed)
{
    QtControlSkin::setSemanticProperty(this, "dimmed", dimmed);
    update();
}

void PreviewField::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    QtControlSkin::paintFieldFrame(painter, this);
    paintPreview(painter, QRectF(rect()).adjusted(2, 2, -2, -2));
}

void PreviewField::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) emit clicked();
    else QWidget::mousePressEvent(event);
}

}
