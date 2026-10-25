#include "MenuComboBox.h"

#include <QKeyEvent>
#include <QMenu>
#include <QStylePainter>
#include <QWheelEvent>

namespace VNX {

MenuComboBox::MenuComboBox(QWidget* parent)
    : QComboBox(parent)
    , menu_(new QMenu(this))
{
    setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    addItem(QString());
    connect(menu_, &QMenu::aboutToHide, this, [this] {
        QComboBox::hidePopup();
        update();
    });
}

void MenuComboBox::setDisplayText(const QString& text)
{
    setItemText(0, text);
    setAccessibleDescription(text);
}

QSize MenuComboBox::minimumSizeHint() const
{
    QStyleOptionComboBox option;
    initStyleOption(&option);
    return style()->sizeFromContents(QStyle::CT_ComboBox, &option,
        QSize(0, fontMetrics().height()), this);
}

void MenuComboBox::showPopup()
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

void MenuComboBox::hidePopup()
{
    menu_->hide();
    QComboBox::hidePopup();
}

void MenuComboBox::paintEvent(QPaintEvent*)
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

void MenuComboBox::keyPressEvent(QKeyEvent* event)
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

void MenuComboBox::wheelEvent(QWheelEvent* event)
{
    event->ignore();
}

}
