#include "FileViewerPanel.h"

#include "FileViewerWidget.h"

#include <QChildEvent>
#include <QFocusEvent>
#include <QVBoxLayout>

namespace harken::studio {

QString FileViewerPanel::typeId()
{
    return QStringLiteral("FileViewer");
}

QString FileViewerPanel::dockObjectPrefix()
{
    return QStringLiteral("FileViewerDock");
}

FileViewerPanel* FileViewerPanel::panelContaining(QWidget* widget)
{
    for (QWidget* current = widget; current; current = current->parentWidget()) {
        if (auto* panel = qobject_cast<FileViewerPanel*>(current))
            return panel;
    }
    return nullptr;
}

FileViewerPanel::FileViewerPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("FileViewerPanel"));
    setFocusPolicy(Qt::StrongFocus);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    viewer_ = new FileViewerWidget(this);
    layout->addWidget(viewer_);
    installFocusFilterRecursively(this);
}

void FileViewerPanel::installFocusFilterRecursively(QObject* root)
{
    if (!root)
        return;
    root->installEventFilter(this);
    for (QObject* child : root->children())
        installFocusFilterRecursively(child);
}

uint32_t FileViewerPanel::sourceId() const
{
    return viewer_->sourceId();
}

bool FileViewerPanel::isPreviewPlayheadActive() const
{
    return viewer_->isPreviewPlayheadActive();
}

QString FileViewerPanel::previewObjectName() const
{
    return viewer_->previewObjectName();
}

void FileViewerPanel::setPinned(bool pinned)
{
    if (pinned_ == pinned)
        return;
    pinned_ = pinned;
    emit pinToggled(pinned_);
}

void FileViewerPanel::bindSource(uint32_t sourceId)
{
    viewer_->bindSource(sourceId);
}

void FileViewerPanel::setAuditionManager(harken::studio::AuditionManager* audition)
{
    viewer_->setAuditionManager(audition);
}

bool FileViewerPanel::handlePlayStopShortcut()
{
    return viewer_->handlePlayStopShortcut();
}

void FileViewerPanel::clearEditor()
{
    viewer_->clearSource();
}

QVariantMap FileViewerPanel::snapshotState() const
{
    QVariantMap state;
    if (sourceId() != 0)
        state[QStringLiteral("sourceId")] = sourceId();
    return state;
}

void FileViewerPanel::restoreState(const QVariantMap& state)
{
    bindSource(state.value(QStringLiteral("sourceId")).toUInt());
}

bool FileViewerPanel::eventFilter(QObject* watched, QEvent* event)
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

void FileViewerPanel::focusInEvent(QFocusEvent* event)
{
    emit panelFocused(panelId_);
    QWidget::focusInEvent(event);
}

}
