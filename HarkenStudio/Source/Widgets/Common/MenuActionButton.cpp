#include "MenuActionButton.h"

#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"

#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QStyleOptionButton>
#include <QStylePainter>
#include <QToolButton>

namespace VNX {
namespace {

class MenuActionPrimaryButton final : public QPushButton
{
public:
    using QPushButton::QPushButton;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QStylePainter painter(this);
        QStyleOptionButton option;
        initStyleOption(&option);
        const QRect textRect = style()->subElementRect(QStyle::SE_PushButtonContents, &option, this);
        const int iconWidth = option.icon.isNull() ? 0 : option.iconSize.width() + 4;
        option.text = fontMetrics().elidedText(option.text, Qt::ElideRight,
                                               qMax(0, textRect.width() - iconWidth));
        painter.drawControl(QStyle::CE_PushButton, option);
    }
};

}

MenuActionButton::MenuActionButton(QWidget* parent)
    : QWidget(parent)
    , primaryButton_(new MenuActionPrimaryButton(this))
    , dropdownButton_(new QToolButton(this))
    , divider_(new QFrame(this))
    , menu_(new QMenu(this))
{
    setAttribute(Qt::WA_StyledBackground);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setFocusProxy(primaryButton_);

    primaryButton_->setProperty("menuActionButtonPart", QStringLiteral("primary"));
    primaryButton_->setAutoDefault(false);
    primaryButton_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    primaryButton_->setCursor(Qt::PointingHandCursor);
    primaryButton_->installEventFilter(this);

    dropdownButton_->setProperty("menuActionButtonPart", QStringLiteral("dropdown"));
    dropdownButton_->setArrowType(Qt::DownArrow);
    dropdownButton_->setFocusPolicy(Qt::StrongFocus);
    dropdownButton_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    dropdownButton_->setCursor(Qt::PointingHandCursor);
    dropdownButton_->setAccessibleName(tr("More actions"));
    dropdownButton_->setToolTip(tr("More actions"));
    dropdownButton_->installEventFilter(this);

    divider_->setProperty("menuActionButtonPart", QStringLiteral("divider"));
    ThemeManager::instance().bindMetrics(divider_, [this](const ThemeMetrics& metrics) {
        divider_->setFixedWidth(metrics.borderWidth);
    });
    divider_->setAttribute(Qt::WA_TransparentForMouseEvents);

    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    row->addWidget(primaryButton_, 1);
    row->addWidget(divider_, 0, Qt::AlignVCenter);
    row->addWidget(dropdownButton_);

    menu_->installEventFilter(this);
    connect(dropdownButton_, &QToolButton::clicked, this, &MenuActionButton::showMenu);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &MenuActionButton::updateMetrics);
    updateMetrics();
}

bool MenuActionButton::event(QEvent* event)
{
    const bool handled = QWidget::event(event);
    if (event->type() == QEvent::LayoutRequest) updateMetrics();
    else if (event->type() == QEvent::Resize || event->type() == QEvent::Hide
             || (event->type() == QEvent::EnabledChange && !isEnabled()))
        menu_->close();
    return handled;
}

bool MenuActionButton::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == menu_ && event->type() == QEvent::MouseButtonPress) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (!menu_->rect().contains(mouse->position().toPoint()))
            menu_->setAttribute(Qt::WA_NoMouseReplay, dropdownButton_->rect().contains(
                dropdownButton_->mapFromGlobal(mouse->globalPosition().toPoint())));
    }
    if ((watched == primaryButton_ || watched == dropdownButton_)
        && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        const bool openMenu = key->key() == Qt::Key_Down
            && (key->modifiers() == Qt::NoModifier || key->modifiers() == Qt::AltModifier);
        const bool activate = (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter
            || (watched == dropdownButton_ && key->key() == Qt::Key_Space))
            && key->modifiers() == Qt::NoModifier;
        if (openMenu || activate) {
            if (!key->isAutoRepeat()) {
                if (openMenu || watched == dropdownButton_) showMenu();
                else primaryButton_->click();
            }
            return true;
        }
    } else if (watched == dropdownButton_ && event->type() == QEvent::KeyRelease
               && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Space) {
        return true;
    } else if (watched == primaryButton_
               && (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)) {
        updateMetrics();
    } else if (watched == dropdownButton_
               && (event->type() == QEvent::Hide
                   || (event->type() == QEvent::EnabledChange && !dropdownButton_->isEnabled()))) {
        menu_->close();
    }
    return QWidget::eventFilter(watched, event);
}

void MenuActionButton::updateMetrics()
{
    primaryButton_->ensurePolished();
    const int height = qMax(QtControlSkin::current().fieldHeight(), primaryButton_->sizeHint().height());
    setFixedHeight(height);
    dropdownButton_->setFixedWidth(height);
    divider_->setFixedHeight(primaryButton_->fontMetrics().height());
}

void MenuActionButton::showMenu()
{
    if (!dropdownButton_->isVisible() || !dropdownButton_->isEnabled() || menu_->isEmpty()) return;
    if (menu_->isVisible()) {
        menu_->close();
        return;
    }
    menu_->popup(dropdownButton_->mapToGlobal(QPoint(0, dropdownButton_->height())));
}

}
