#include "SourceEditorPanel.h"

#include "SourceEditorWidget.h"

#include <QChildEvent>
#include <QFocusEvent>
#include <QVBoxLayout>

namespace VNX {

QString SourceEditorPanel::typeId()
{
    return QStringLiteral("SourceEditor");
}

QString SourceEditorPanel::dockObjectPrefix()
{
    return QStringLiteral("SourceEditorDock");
}

SourceEditorPanel* SourceEditorPanel::panelContaining(QWidget* widget)
{
    for (QWidget* current = widget; current; current = current->parentWidget()) {
        if (auto* panel = qobject_cast<SourceEditorPanel*>(current))
            return panel;
    }
    return nullptr;
}

SourceEditorPanel::SourceEditorPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("SourceEditorPanel"));
    setFocusPolicy(Qt::StrongFocus);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    editor_ = new SourceEditorWidget(this);
    layout->addWidget(editor_);
    installFocusFilterRecursively(this);
}

void SourceEditorPanel::installFocusFilterRecursively(QObject* root)
{
    if (!root)
        return;
    root->installEventFilter(this);
    for (QObject* child : root->children())
        installFocusFilterRecursively(child);
}

uint32_t SourceEditorPanel::sourceId() const
{
    return editor_->sourceId();
}

bool SourceEditorPanel::isPreviewPlayheadActive() const
{
    return editor_->isPreviewPlayheadActive();
}

QString SourceEditorPanel::previewObjectName() const
{
    return editor_->previewObjectName();
}

QUuid SourceEditorPanel::soundGuid() const
{
    return editor_->soundGuid();
}

QUuid SourceEditorPanel::entryId() const
{
    return editor_->entryId();
}

void SourceEditorPanel::setPinned(bool pinned)
{
    if (pinned_ == pinned)
        return;
    pinned_ = pinned;
    emit pinToggled(pinned_);
}

void SourceEditorPanel::bindSource(uint32_t sourceId, const QUuid& soundGuid, const QUuid& entryId)
{
    editor_->bindSource(sourceId, soundGuid, entryId);
}

void SourceEditorPanel::setAuditionManager(HarkenEditor::AuditionManager* audition)
{
    editor_->setAuditionManager(audition);
}

bool SourceEditorPanel::handlePlayStopShortcut()
{
    return editor_->handlePlayStopShortcut();
}

void SourceEditorPanel::clearEditor()
{
    editor_->clearSource();
}

QVariantMap SourceEditorPanel::snapshotState() const
{
    QVariantMap state;
    if (sourceId() != 0)
        state[QStringLiteral("sourceId")] = sourceId();
    if (!soundGuid().isNull())
        state[QStringLiteral("soundGuid")] = soundGuid().toString(QUuid::WithoutBraces);
    if (!entryId().isNull())
        state[QStringLiteral("entryId")] = entryId().toString(QUuid::WithoutBraces);
    return state;
}

void SourceEditorPanel::restoreState(const QVariantMap& state)
{
    bindSource(state.value(QStringLiteral("sourceId")).toUInt(),
               QUuid::fromString(state.value(QStringLiteral("soundGuid")).toString()),
               QUuid::fromString(state.value(QStringLiteral("entryId")).toString()));
}

bool SourceEditorPanel::eventFilter(QObject* watched, QEvent* event)
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

void SourceEditorPanel::focusInEvent(QFocusEvent* event)
{
    emit panelFocused(panelId_);
    QWidget::focusInEvent(event);
}

}
