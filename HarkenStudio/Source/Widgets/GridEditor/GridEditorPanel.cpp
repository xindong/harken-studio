#include "GridEditorPanel.h"

#include <QChildEvent>
#include <QFocusEvent>
#include <QVBoxLayout>

namespace VNX {

GridEditorPanel::GridEditorPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("GridEditorPanel"));
    setFocusPolicy(Qt::StrongFocus);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    editor_ = new GridEditorWidget();
    root->addWidget(editor_, 1);

    connect(editor_, &GridEditorWidget::pinToggled,
            this, &GridEditorPanel::pinToggled);
    connect(editor_, &GridEditorWidget::auditionTargetRequested,
            this, &GridEditorPanel::auditionTargetRequested);
    connect(editor_, &GridEditorWidget::navigateToItem,
            this, &GridEditorPanel::navigateToItem);
    connect(editor_, &GridEditorWidget::locateItem,
            this, &GridEditorPanel::locateItem);
    connect(editor_, &GridEditorWidget::sourceEditorRequested,
            this, &GridEditorPanel::sourceEditorRequested);

    installFocusFilterRecursively(this);
}

void GridEditorPanel::installFocusFilterRecursively(QObject* root)
{
    if (!root)
        return;
    root->installEventFilter(this);
    for (QObject* child : root->children())
        installFocusFilterRecursively(child);
}

bool GridEditorPanel::acceptsItem(const ProjectItem* item)
{
    return GridEditorWidget::acceptsItem(item);
}

bool GridEditorPanel::eventFilter(QObject* watched, QEvent* event)
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

void GridEditorPanel::setPinned(bool pinned)
{
    editor_->setPinned(pinned);
}

void GridEditorPanel::bindItem(ProjectItem* item)
{
    editor_->bindItem(item);
}

void GridEditorPanel::clearEditor()
{
    editor_->clearEditor();
}

void GridEditorPanel::focusInEvent(QFocusEvent* event)
{
    emit panelFocused(panelId_);
    QWidget::focusInEvent(event);
}

}
