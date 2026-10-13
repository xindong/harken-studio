#include "ContextInfoService.h"

#include "ProjectManager.h"

#include <QApplication>
#include <QEvent>
#include <QMouseEvent>
#include <QSinglePointEvent>
#include <QWidget>

namespace VNX {

namespace {

constexpr int kHoverDebounceMs = 150;

bool subtreeContainsGuid(const ProjectItem* item, const QUuid& guid)
{
    if (!item)
        return false;
    if (item->guid == guid)
        return true;
    for (const auto& child : item->children)
        if (subtreeContainsGuid(child.get(), guid))
            return true;
    return false;
}

}

ContextInfoService& ContextInfoService::instance()
{
    static ContextInfoService instance;
    return instance;
}

ContextInfoService::ContextInfoService(QObject* parent)
    : QObject(parent)
{
    debounce_.setSingleShot(true);
    debounce_.setInterval(kHoverDebounceMs);
    connect(&debounce_, &QTimer::timeout, this, [this]() { commitPending(); });

    ProjectManager& projectManager = ProjectManager::instance();
    connect(&projectManager, &ProjectManager::projectClosed,
            this, &ContextInfoService::clearTopic);
    connect(&projectManager, &ProjectManager::itemDeleted,
            this, [this](ProjectItem* item)
            {
                const bool currentDeleted = !current_.objectGuid.isNull()
                    && subtreeContainsGuid(item, current_.objectGuid);
                const bool pendingDeleted = !pending_.objectGuid.isNull()
                    && subtreeContainsGuid(item, pending_.objectGuid);
                if (currentDeleted || pendingDeleted)
                    clearTopic();
            });
}

ContextInfoService::~ContextInfoService()
{
    if (filterInstalled_ && qApp)
        qApp->removeEventFilter(this);
}

void ContextInfoService::setActive(bool active)
{
    if (active_ == active)
        return;

    active_ = active;

    if (!active_)
    {
        debounce_.stop();
        pending_ = ContextTopic();
    }

    updateEventFilter();
}

void ContextInfoService::setLocked(bool locked)
{
    if (locked_ == locked)
        return;

    locked_ = locked;
    if (locked_)
    {
        debounce_.stop();
        pending_ = current_;
    }

    updateEventFilter();
    emit lockedChanged(locked_);
}

void ContextInfoService::updateEventFilter()
{
    const bool shouldInstall = active_ && !locked_;
    if (filterInstalled_ == shouldInstall)
        return;

    if (shouldInstall)
        qApp->installEventFilter(this);
    else
        qApp->removeEventFilter(this);

    filterInstalled_ = shouldInstall;
}

void ContextInfoService::setFreezeRoot(QWidget* widget)
{
    freezeRoot_ = widget;
}

void ContextInfoService::clearTopic()
{
    debounce_.stop();
    pending_ = ContextTopic();

    if (current_ == ContextTopic())
        return;

    current_ = ContextTopic();

    emit topicChanged(current_);
}

bool ContextInfoService::eventFilter(QObject* watched, QEvent* event)
{
    const QEvent::Type type = event->type();
    auto* widget = qobject_cast<QWidget*>(watched);
    if (widget && active_ && !locked_)
    {
        if ((type == QEvent::Enter || type == QEvent::HoverMove) && !suspended(widget))
            considerPointer(widget,
                            static_cast<QSinglePointEvent*>(event)->globalPosition().toPoint(),
                            type == QEvent::Enter, false);
        else if (type == QEvent::MouseButtonPress)
            considerPointer(widget,
                            static_cast<QMouseEvent*>(event)->globalPosition().toPoint(),
                            false, true);
        else if (type == QEvent::FocusIn)
            presentWidget(widget);
    }

    return QObject::eventFilter(watched, event);
}

void ContextInfoService::considerPointer(QWidget* receiver, const QPoint& globalPos,
                                         bool allowHoverOptIn, bool immediate)
{
    // Qt reports one pointer position to every widget up the chain that asks for
    // hover, so the receiver says nothing about what the pointer is over. Descend
    // to the widget it actually addresses, and every one of those reports agrees.
    QWidget* under = receiver;
    if (QWidget* deepest = receiver->childAt(receiver->mapFromGlobal(globalPos)))
        under = deepest;

    for (QWidget* w = under; w; w = w->parentWidget())
    {
        if (freezeRoot_ && w == freezeRoot_.data())
        {
            debounce_.stop();
            pending_ = current_;
            return;
        }

        if (auto* source = dynamic_cast<IContextInfoSource*>(w))
        {
            if (allowHoverOptIn)
                under->setAttribute(Qt::WA_Hover, true);

            const ContextTopic topic = source->contextTopicAt(w->mapFromGlobal(globalPos));
            if (!topic.isNone())
            {
                if (immediate) present(topic); else scheduleTopic(topic, under);
                return;
            }
        }

        if (auto tagged = ContextInfo::tagOf(w))
        {
            if (immediate) present(*tagged); else scheduleTopic(*tagged, under);
            return;
        }
    }
}

bool ContextInfoService::suspended(QWidget* receiver) const
{
    if (!receiver || !active_ || locked_)
        return true;

    if (QApplication::activePopupWidget())
        return true;

    if (auto* modal = QApplication::activeModalWidget())
        if (!ContextInfo::tagOf(modal)
            || (receiver != modal && !modal->isAncestorOf(receiver)))
            return true;

    return QApplication::mouseButtons() != Qt::NoButton;
}

void ContextInfoService::present(const ContextTopic& topic)
{
    if (!active_ || locked_ || topic.isNone())
        return;
    debounce_.stop();
    pending_ = topic;
    if (current_ == topic)
        return;
    current_ = topic;
    emit topicChanged(current_);
}

void ContextInfoService::presentWidget(QWidget* widget)
{
    for (QWidget* current = widget; current; current = current->parentWidget())
    {
        if (freezeRoot_ && current == freezeRoot_.data())
            return;
        if (auto tagged = ContextInfo::tagOf(current))
        {
            present(*tagged);
            return;
        }
    }
}

void ContextInfoService::scheduleTopic(const ContextTopic& topic, QWidget* receiver)
{
    pendingReceiver_ = receiver;
    if (topic == current_)
    {
        debounce_.stop();
        pending_ = current_;
        return;
    }

    if (topic == pending_ && debounce_.isActive())
        return;

    pending_ = topic;
    debounce_.start();
}

void ContextInfoService::commitPending()
{
    if (suspended(pendingReceiver_) || pending_ == current_ || pending_.isNone())
        return;

    current_ = pending_;

    emit topicChanged(current_);
}

}
