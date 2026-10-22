#include "SplitButton.h"

#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"

#include <QAction>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPushButton>
#include <QScreen>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace VNX {

SplitButton::SplitButton(QWidget* parent)
    : QWidget(parent)
    , primaryButton_(new QPushButton(this))
    , dropdownButton_(new QToolButton(this))
    , divider_(new QFrame(this))
    , popup_(new QWidget(this, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint))
{
    setAttribute(Qt::WA_StyledBackground);
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    setFocusProxy(primaryButton_);

    primaryButton_->setProperty("splitButtonPart", QStringLiteral("primary"));
    primaryButton_->setAutoDefault(false);
    primaryButton_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Expanding);
    primaryButton_->setCursor(Qt::PointingHandCursor);
    primaryButton_->installEventFilter(this);

    dropdownButton_->setProperty("splitButtonPart", QStringLiteral("dropdown"));
    dropdownButton_->setArrowType(Qt::DownArrow);
    dropdownButton_->setFocusPolicy(Qt::StrongFocus);
    dropdownButton_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    dropdownButton_->setCursor(Qt::PointingHandCursor);
    dropdownButton_->setAccessibleName(tr("More actions"));
    dropdownButton_->setToolTip(tr("More actions"));
    dropdownButton_->installEventFilter(this);

    divider_->setProperty("splitButtonPart", QStringLiteral("divider"));
    divider_->setFixedWidth(1);
    divider_->setAttribute(Qt::WA_TransparentForMouseEvents);

    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    row->addWidget(primaryButton_, 1);
    row->addWidget(divider_, 0, Qt::AlignVCenter);
    row->addWidget(dropdownButton_);

    popup_->setObjectName(QStringLiteral("splitButtonPopup"));
    auto* popupLayout = new QVBoxLayout(popup_);
    popupLayout->setContentsMargins(0, 0, 0, 0);
    popupLayout->setSpacing(2);
    popupLayout->setSizeConstraint(QLayout::SetNoConstraint);
    popup_->installEventFilter(this);
    connect(dropdownButton_, &QToolButton::clicked, this, &SplitButton::showPopup);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &SplitButton::updateMetrics);
    updateDropdownAvailability();
    updateMetrics();
}

bool SplitButton::event(QEvent* event)
{
    const bool handled = QWidget::event(event);
    if (event->type() == QEvent::LayoutRequest) updateMetrics();
    else if (event->type() == QEvent::Resize && popup_->isVisible()) positionPopup();
    else if (event->type() == QEvent::Hide) popup_->hide();
    return handled;
}

bool SplitButton::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == popup_ && event->type() == QEvent::MouseButtonPress) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (!popup_->rect().contains(mouse->position().toPoint())) {
            const bool overDropdown = dropdownButton_->rect().contains(
                dropdownButton_->mapFromGlobal(mouse->globalPosition().toPoint()));
            popup_->setAttribute(Qt::WA_NoMouseReplay, overDropdown);
            popup_->hide();
            return true;
        }
    }
    if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        const bool inPopup = watched == popup_ || watched->parent() == popup_;
        if (inPopup && key->key() == Qt::Key_Escape) {
            popup_->hide();
            return true;
        }
        const bool openPopup = !inPopup && key->key() == Qt::Key_Down
            && (key->modifiers() == Qt::NoModifier || key->modifiers() == Qt::AltModifier);
        const bool activate = (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter
            || (watched == dropdownButton_ && key->key() == Qt::Key_Space))
            && key->modifiers() == Qt::NoModifier;
        if (openPopup && dropdownButton_->isVisible() && dropdownButton_->isEnabled()) {
            if (!key->isAutoRepeat()) showPopup();
            return true;
        }
        if (activate) {
            if (!key->isAutoRepeat()) {
                if (auto* button = qobject_cast<QPushButton*>(watched)) button->click();
                else if (watched == dropdownButton_) showPopup();
            }
            return true;
        }
    } else if (watched == dropdownButton_ && event->type() == QEvent::KeyRelease
               && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Space) {
        return true;
    } else if (watched == primaryButton_) {
        if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
            updateMetrics();
        else if (event->type() == QEvent::EnabledChange)
            updateDropdownAvailability();
    }
    return QWidget::eventFilter(watched, event);
}

void SplitButton::actionEvent(QActionEvent* event)
{
    QWidget::actionEvent(event);
    auto* action = event->action();
    if (event->type() == QEvent::ActionRemoved) {
        delete actionButtons_.take(action);
    } else {
        auto* button = actionButtons_.value(action);
        if (!button) {
            button = new QPushButton(popup_);
            button->setProperty("splitButtonPart", QStringLiteral("action"));
            button->setAutoDefault(false);
            button->setCursor(Qt::PointingHandCursor);
            button->installEventFilter(this);
            static_cast<QVBoxLayout*>(popup_->layout())->insertWidget(
                actions().indexOf(action), button);
            actionButtons_.insert(action, button);
            connect(button, &QPushButton::clicked, this, [this, action] {
                popup_->hide();
                action->trigger();
            });
        }
        button->setText(action->text());
        button->setIcon(action->icon());
        button->setToolTip(action->toolTip());
        button->setEnabled(action->isEnabled());
        button->setVisible(action->isVisible() && !action->isSeparator());
        button->setFixedHeight(height());
    }
    updateDropdownAvailability();
}

void SplitButton::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::EnabledChange && !isEnabled()) popup_->hide();
}

void SplitButton::updateDropdownAvailability()
{
    const auto alternatives = actions();
    const bool available = std::any_of(alternatives.cbegin(), alternatives.cend(),
        [](const QAction* action) { return action->isVisible() && !action->isSeparator(); });
    if (!available) {
        popup_->hide();
        if (dropdownButton_->hasFocus()) primaryButton_->setFocus(Qt::OtherFocusReason);
    }
    dropdownButton_->setEnabled(available);
    if (primaryButton_->isEnabled() || !available) setFocusProxy(primaryButton_);
    else setFocusProxy(dropdownButton_);
    updateGeometry();
    if (popup_->isVisible()) positionPopup();
}

void SplitButton::updateMetrics()
{
    primaryButton_->ensurePolished();
    const int height = qMax(QtControlSkin::current().fieldHeight(),
                            primaryButton_->sizeHint().height());
    setFixedHeight(height);
    dropdownButton_->setFixedWidth(height);
    divider_->setFixedHeight(primaryButton_->fontMetrics().height());
    for (auto* button : actionButtons_) button->setFixedHeight(height);
    if (popup_->isVisible()) positionPopup();
}

void SplitButton::showPopup()
{
    if (!dropdownButton_->isVisible() || !dropdownButton_->isEnabled()) return;
    if (popup_->isVisible()) {
        popup_->hide();
        return;
    }
    popup_->ensurePolished();
    positionPopup();
    popup_->show();
    for (auto* action : actions()) {
        auto* button = actionButtons_.value(action);
        if (button->isVisible() && button->isEnabled()) {
            button->setFocus(Qt::PopupFocusReason);
            break;
        }
    }
}

void SplitButton::positionPopup()
{
    popup_->setFixedSize(width(), popup_->sizeHint().height());
    const QRect available = screen()->availableGeometry();
    const QPoint origin = mapToGlobal(QPoint());
    QPoint position(origin.x(), origin.y() + height() + 2);
    if (position.y() + popup_->height() > available.bottom() + 1)
        position.setY(origin.y() - popup_->height() - 2);
    position.setX(qBound(available.left(), position.x(),
                        qMax(available.left(), available.right() - popup_->width() + 1)));
    popup_->move(position);
}

}
