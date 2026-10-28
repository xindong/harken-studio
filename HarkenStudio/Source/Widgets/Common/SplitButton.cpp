#include "SplitButton.h"

#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"

#include <QAction>
#include <QAbstractItemView>
#include <QComboBox>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPushButton>
#include <QStandardItemModel>
#include <QToolButton>

namespace VNX {

SplitButton::SplitButton(QWidget* parent)
    : QWidget(parent)
    , primaryButton_(new QPushButton(this))
    , dropdownButton_(new QToolButton(this))
    , divider_(new QFrame(this))
    , popupCombo_(new QComboBox(this))
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
    dropdownButton_->setProperty("flatIconButton", true);
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

    QtControlSkin::setPopupIconColumn(this, PopupIconColumn::Automatic);
    popupCombo_->setObjectName(QStringLiteral("splitButtonPopup"));
    popupCombo_->setProperty("popupHasCheckableItems", false);
    popupCombo_->setFocusProxy(dropdownButton_);
    popupCombo_->hide();
    popupCombo_->view()->installEventFilter(this);
    popupCombo_->view()->viewport()->installEventFilter(this);
    popupCombo_->view()->window()->installEventFilter(this);
    connect(popupCombo_, &QComboBox::activated, this, [this](int index) {
        auto* action = popupActions_.value(index);
        if (action && action->isEnabled() && action->isVisible()) action->trigger();
    });
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
    else if (event->type() == QEvent::Resize || event->type() == QEvent::Hide)
        popupCombo_->hidePopup();
    return handled;
}

bool SplitButton::eventFilter(QObject* watched, QEvent* event)
{
    auto* view = popupCombo_->view();
    auto* popup = view->window();
    if (watched == view || watched == view->viewport() || watched == popup) {
        if (event->type() == QEvent::Leave) {
            view->selectionModel()->clear();
        } else if (event->type() == QEvent::MouseMove) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            const QPoint position = view->viewport()->mapFromGlobal(mouse->globalPosition().toPoint());
            const auto index = view->indexAt(position);
            if (!view->viewport()->rect().contains(position) || !index.isValid()
                || !index.flags().testFlag(Qt::ItemIsEnabled)) {
                view->selectionModel()->clear();
                return true;
            }
        }
    }
    if (watched == popup && event->type() == QEvent::MouseButtonPress) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (!popup->rect().contains(mouse->position().toPoint())) {
            const bool overDropdown = dropdownButton_->rect().contains(
                dropdownButton_->mapFromGlobal(mouse->globalPosition().toPoint()));
            popup->setAttribute(Qt::WA_NoMouseReplay, overDropdown);
            popupCombo_->hidePopup();
            return true;
        }
    }
    if ((watched == primaryButton_ || watched == dropdownButton_)
        && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        const bool openPopup = key->key() == Qt::Key_Down
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
    } else if (watched == dropdownButton_
               && (event->type() == QEvent::Hide
                   || (event->type() == QEvent::EnabledChange && !dropdownButton_->isEnabled()))) {
        popupCombo_->hidePopup();
    }
    return QWidget::eventFilter(watched, event);
}

void SplitButton::actionEvent(QActionEvent* event)
{
    QWidget::actionEvent(event);
    auto* current = popupActions_.value(popupCombo_->view()->currentIndex().row());
    popupCombo_->clear();
    popupActions_.clear();
    auto* model = static_cast<QStandardItemModel*>(popupCombo_->model());
    bool hasCheckableItems = false;
    for (auto* action : actions()) {
        if (!action->isVisible() || action->isSeparator()) continue;
        popupCombo_->addItem(action->icon(), action->text());
        auto* item = model->item(popupActions_.size());
        item->setEnabled(action->isEnabled());
        item->setToolTip(action->toolTip());
        item->setTextAlignment(Qt::AlignCenter);
        if (action->isCheckable()) {
            item->setData(action->isChecked() ? Qt::Checked : Qt::Unchecked, Qt::CheckStateRole);
            hasCheckableItems = true;
        }
        popupActions_.append(action);
    }
    popupCombo_->setProperty("popupHasCheckableItems", hasCheckableItems);
    popupCombo_->setCurrentIndex(popupActions_.indexOf(current));
    if (!current) popupCombo_->view()->selectionModel()->clear();
    updateDropdownAvailability();
}

void SplitButton::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::EnabledChange && !isEnabled()) popupCombo_->hidePopup();
}

void SplitButton::updateDropdownAvailability()
{
    const bool available = !popupActions_.isEmpty();
    if (!available) {
        popupCombo_->hidePopup();
        if (dropdownButton_->hasFocus()) primaryButton_->setFocus(Qt::OtherFocusReason);
    }
    dropdownButton_->setEnabled(available);
    if (primaryButton_->isEnabled() || !available) setFocusProxy(primaryButton_);
    else setFocusProxy(dropdownButton_);
    updateGeometry();
}

void SplitButton::updateMetrics()
{
    primaryButton_->ensurePolished();
    const int height = qMax(QtControlSkin::current().fieldHeight(),
                            primaryButton_->sizeHint().height());
    setFixedHeight(height);
    dropdownButton_->setFixedWidth(height);
    divider_->setFixedHeight(primaryButton_->fontMetrics().height());
}

void SplitButton::showPopup()
{
    if (!dropdownButton_->isVisible() || !dropdownButton_->isEnabled()) return;
    if (popupCombo_->view()->isVisible()) {
        popupCombo_->hidePopup();
        return;
    }
    popupCombo_->ensurePolished();
    popupCombo_->setFixedSize(size());
    popupCombo_->view()->window()->setFixedWidth(width());
    popupCombo_->setCurrentIndex(-1);
    popupCombo_->showPopup();
    popupCombo_->view()->selectionModel()->clear();
}

}
