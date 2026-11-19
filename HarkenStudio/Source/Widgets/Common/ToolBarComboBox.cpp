#include "ToolBarComboBox.h"

#include "Core/QtControlSkin.h"

#include <QKeyEvent>
#include <QMenu>
#include <QStylePainter>
#include <QWheelEvent>

namespace VNX {

ToolBarComboBox::ToolBarComboBox(QWidget* parent)
    : QComboBox(parent)
{
    QtControlSkin::setSemanticProperty(this, "fieldRole", QStringLiteral("toolBar"));
    setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    setMinimumContentsLength(12);
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
}

ToolBarMenuComboBox::ToolBarMenuComboBox(QWidget* parent)
    : ToolBarComboBox(parent)
    , menu_(new QMenu(this))
{
    addItem(QString());
    connect(menu_, &QMenu::aboutToHide, this, [this] {
        QComboBox::hidePopup();
        update();
    });
}

void ToolBarMenuComboBox::setDisplayText(const QString& text)
{
    setItemText(0, text);
    setAccessibleDescription(text);
}

QSize ToolBarMenuComboBox::minimumSizeHint() const
{
    QStyleOptionComboBox option;
    initStyleOption(&option);
    return style()->sizeFromContents(QStyle::CT_ComboBox, &option,
        QSize(0, fontMetrics().height()), this);
}

void ToolBarMenuComboBox::showPopup()
{
    menu_->setMinimumWidth(width());
    menu_->popup(mapToGlobal(QPoint(0, height())));
    update();
    for (auto* action : menu_->actions()) {
        if (action->isChecked()) {
            menu_->setActiveAction(action);
            break;
        }
    }
}

void ToolBarMenuComboBox::hidePopup()
{
    menu_->hide();
    QComboBox::hidePopup();
}

void ToolBarMenuComboBox::paintEvent(QPaintEvent*)
{
    QStylePainter painter(this);
    QStyleOptionComboBox option;
    initStyleOption(&option);
    if (menu_->isVisible())
        option.state |= QStyle::State_On;
    const QRect textRect = style()->subControlRect(QStyle::CC_ComboBox, &option,
                                                  QStyle::SC_ComboBoxEditField, this);
    option.currentText = fontMetrics().elidedText(option.currentText, Qt::ElideRight, textRect.width());
    painter.drawComplexControl(QStyle::CC_ComboBox, option);
    painter.drawControl(QStyle::CE_ComboBoxLabel, option);
}

void ToolBarMenuComboBox::keyPressEvent(QKeyEvent* event)
{
    switch (event->key()) {
    case Qt::Key_Space:
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_F4:
        if (!event->isAutoRepeat()) showPopup();
        event->accept();
        return;
    default:
        QComboBox::keyPressEvent(event);
    }
}

void ToolBarMenuComboBox::wheelEvent(QWheelEvent* event)
{
    event->ignore();
}

}
