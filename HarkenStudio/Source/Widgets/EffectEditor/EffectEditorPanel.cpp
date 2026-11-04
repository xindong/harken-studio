#include "EffectEditorPanel.h"

#include "Core/Components/VMixNode.h"
#include "Core/ProjectManager.h"

#include <QChildEvent>
#include <QFocusEvent>

#include <algorithm>

namespace VNX {

EffectEditorPanel::EffectEditorPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("EffectEditorPanel"));
    setFocusPolicy(Qt::StrongFocus);

    auto& project = ProjectManager::instance();
    connect(&project, &ProjectManager::projectClosed, this, &EffectEditorPanel::clearEditor);
    connect(&project, &ProjectManager::itemDeleted, this, [this] {
        if (!ownerGuid_.isNull() && !boundItem())
            clearEditor();
    });
    connect(&project, &ProjectManager::itemRenamed, this, [this](ProjectItem* item) {
        if (item && item->guid == ownerGuid_)
            refreshEffect();
    });
    connect(&project, &ProjectManager::itemPropertyChanged, this,
            [this](ProjectItem* item, const QString& property, const QObject*) {
        if (item && item->guid == ownerGuid_ && property == QLatin1String("effects"))
            QMetaObject::invokeMethod(this, &EffectEditorPanel::refreshEffect, Qt::QueuedConnection);
    });

    refreshEffect();
    installFocusFilterRecursively(this);
}

ProjectItem* EffectEditorPanel::boundItem() const
{
    return ProjectManager::instance().findItemByGuid(ownerGuid_);
}

void EffectEditorPanel::setPinned(bool pinned)
{
    if (pinned_ == pinned)
        return;
    pinned_ = pinned;
    emit pinToggled(pinned_);
}

void EffectEditorPanel::bindEffect(const QUuid& ownerGuid, const QUuid& effectGuid)
{
    ownerGuid_ = ownerGuid;
    effectGuid_ = effectGuid;
    refreshEffect();
}

void EffectEditorPanel::clearEditor()
{
    ownerGuid_ = QUuid{};
    effectGuid_ = QUuid{};
    refreshEffect();
}

void EffectEditorPanel::refreshEffect()
{
    QString title = tr("Effect Editor");
    const auto* item = boundItem();
    const auto* mix = item ? item->comp<VMixNode>() : nullptr;
    if (mix) {
        const auto& effects = mix->effects.entries();
        const auto effect = std::find_if(effects.begin(), effects.end(), [this](const EffectEntry& entry) {
            return entry.guid == effectGuid_;
        });
        if (effect != effects.end() && effect->typed)
            title += QStringLiteral(" - %1/%2").arg(item->name, EffectChainSemantics::displayName(effect->type));
    }
    setWindowTitle(title);
}

QVariantMap EffectEditorPanel::snapshotState() const
{
    return {
        {QStringLiteral("ownerGuid"), ownerGuid_.toString(QUuid::WithoutBraces)},
        {QStringLiteral("effectGuid"), effectGuid_.toString(QUuid::WithoutBraces)}
    };
}

void EffectEditorPanel::restoreState(const QVariantMap& state)
{
    bindEffect(state.value(QStringLiteral("ownerGuid")).toUuid(),
               state.value(QStringLiteral("effectGuid")).toUuid());
}

void EffectEditorPanel::installFocusFilterRecursively(QObject* root)
{
    root->installEventFilter(this);
    for (QObject* child : root->children())
        installFocusFilterRecursively(child);
}

bool EffectEditorPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::FocusIn && watched != this)
        emit panelFocused(panelId_);
    else if (event->type() == QEvent::ChildAdded)
        installFocusFilterRecursively(static_cast<QChildEvent*>(event)->child());
    return QWidget::eventFilter(watched, event);
}

void EffectEditorPanel::focusInEvent(QFocusEvent* event)
{
    emit panelFocused(panelId_);
    QWidget::focusInEvent(event);
}

}
