#include "TransitionEditorPanel.h"

#include <QChildEvent>
#include <QFocusEvent>
#include <QVBoxLayout>

namespace VNX {

TransitionEditorPanel::TransitionEditorPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("TransitionEditorPanel"));
    setFocusPolicy(Qt::StrongFocus);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    editor_ = new TransitionEditorWidget();
    root->addWidget(editor_, 1);

    connect(editor_, &TransitionEditorWidget::pinToggled,
            this, &TransitionEditorPanel::pinToggled);
    connect(editor_, &TransitionEditorWidget::watchViewChanged,
            this, &TransitionEditorPanel::watchViewChanged);

    installFocusFilterRecursively(this);
}

void TransitionEditorPanel::installFocusFilterRecursively(QObject* root)
{
    if (!root)
        return;
    root->installEventFilter(this);
    for (QObject* child : root->children())
        installFocusFilterRecursively(child);
}

bool TransitionEditorPanel::eventFilter(QObject* watched, QEvent* event)
{
    const QEvent::Type type = event->type();
    if (type == QEvent::FocusIn) {
        if (watched != this)
            emit panelFocused(panelId_);
    } else if (type == QEvent::ChildAdded) {
        if (auto* childEvent = static_cast<QChildEvent*>(event))
            installFocusFilterRecursively(childEvent->child());
    }
    return QWidget::eventFilter(watched, event);
}

bool TransitionEditorPanel::acceptsItem(const ProjectItem* item)
{
    return TransitionEditorWidget::acceptsItem(item);
}

void TransitionEditorPanel::setPinned(bool pinned)
{
    editor_->setPinned(pinned);
}

void TransitionEditorPanel::bindItem(ProjectItem* item)
{
    editor_->bindItem(item);
}

void TransitionEditorPanel::bindGuid(const QUuid& guid)
{
    editor_->bindGuid(guid);
}

void TransitionEditorPanel::setPlacementContext(const std::optional<PlacementContext>& placement)
{
    editor_->setPlacementContext(placement);
}

void TransitionEditorPanel::clearEditor()
{
    editor_->clearEditor();
}

void TransitionEditorPanel::restoreState(const QVariantMap& state)
{
    editor_->restoreState(state);
}

void TransitionEditorPanel::focusInEvent(QFocusEvent* event)
{
    emit panelFocused(panelId_);
    QWidget::focusInEvent(event);
}

}
