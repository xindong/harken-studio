#include "EffectEditorPanel.h"

#include "Core/Components/VEffect.h"
#include "Core/Components/VMixNode.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Widgets/Inspector/FieldGroupInspectorView.h"

#include <QChildEvent>
#include <QFocusEvent>

#include <algorithm>

namespace VNX {
namespace {

std::optional<EffectEntry> findRow(const ProjectItem* item, const QUuid& rowGuid)
{
    const auto* mix = item ? item->comp<VMixNode>() : nullptr;
    if (!mix || rowGuid.isNull()) return {};
    const auto& context = PlatformContextService::instance().context();
    auto chain = mix->effectsForPlatform(context.all ? std::nullopt : currentWorldPlatformScope(context));
    if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item->type)) {
        const auto* project = ProjectManager::instance().currentProject();
        if (!project) return {};
        const auto view = buildFieldGroupInspectorView(*project, item->guid,
            FieldGroupId::Effect, PlatformContextService::instance().context());
        const auto effective = view.effectChain();
        if (!effective) return {};
        chain = *effective;
    }
    const auto& entries = chain.entries();
    const auto row = std::find_if(entries.begin(), entries.end(), [&rowGuid](const EffectEntry& entry) {
        return entry.guid == rowGuid;
    });
    return row != entries.end() ? std::optional(*row) : std::nullopt;
}

}

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
        else
            refreshEffect();
    });
    connect(&project, &ProjectManager::itemCreated, this, [this] { refreshEffect(); });
    connect(&project, &ProjectManager::itemRenamed, this, [this] { refreshEffect(); });
    connect(&project, &ProjectManager::itemPropertyChanged, this,
            [this](ProjectItem* item, const QString& property, const QObject*) {
        if (item && (property == QLatin1String("fieldGroups")
            || (item->guid == ownerGuid_ && property == QLatin1String("effects"))
            || item->type == ProjectItem::Type::Effect))
            QMetaObject::invokeMethod(this, &EffectEditorPanel::refreshEffect, Qt::QueuedConnection);
    });
    connect(&project, &ProjectManager::itemMoved, this, &EffectEditorPanel::refreshEffect);
    connect(&PlatformContextService::instance(), &PlatformContextService::contextChanged,
            this, &EffectEditorPanel::refreshEffect);

    refreshEffect();
    installFocusFilterRecursively(this);
}

ProjectItem* EffectEditorPanel::boundItem() const
{
    return ProjectManager::instance().findItemByGuid(ownerGuid_);
}

bool EffectEditorPanel::hasEffect() const
{
    return canOpen(boundItem(), rowGuid_);
}

bool EffectEditorPanel::canOpen(const ProjectItem* item, const QUuid& rowGuid)
{
    if (rowGuid.isNull())
        return item && item->type == ProjectItem::Type::Effect && item->comp<VEffect>();
    const auto row = findRow(item, rowGuid);
    const auto* project = ProjectManager::instance().currentProject();
    return row && row->typed && project && EffectChainSemantics::resolve(*row, *project);
}

void EffectEditorPanel::setPinned(bool pinned)
{
    if (pinned_ == pinned) return;
    pinned_ = pinned;
    emit pinToggled(pinned_);
}

void EffectEditorPanel::bindEffect(const QUuid& ownerGuid, const QUuid& rowGuid)
{
    ownerGuid_ = ownerGuid;
    rowGuid_ = rowGuid;
    refreshEffect();
}

void EffectEditorPanel::clearEditor()
{
    ownerGuid_ = QUuid{};
    rowGuid_ = QUuid{};
    refreshEffect();
}

void EffectEditorPanel::refreshEffect()
{
    QString title = tr("Effect Editor");
    const auto* item = boundItem();
    if (item && rowGuid_.isNull()) {
        if (const auto* effect = item->comp<VEffect>())
            title += QStringLiteral(" - %1/%2").arg(
                item->name, EffectChainSemantics::displayName(effect->definition.type));
    } else if (const auto row = findRow(item, rowGuid_); row && row->typed) {
        const auto* project = ProjectManager::instance().currentProject();
        if (const auto* effect = project ? EffectChainSemantics::resolve(*row, *project) : nullptr) {
            const auto* preset = ProjectManager::instance().findItemByGuid(row->effectGuid);
            title += QStringLiteral(" - %1/%2").arg(item->name,
                preset ? preset->name : EffectChainSemantics::displayName(effect->type));
        }
    }
    setWindowTitle(title);
}

QVariantMap EffectEditorPanel::snapshotState() const
{
    return {
        {QStringLiteral("ownerGuid"), ownerGuid_.toString(QUuid::WithoutBraces)},
        {QStringLiteral("rowGuid"), rowGuid_.toString(QUuid::WithoutBraces)}
    };
}

void EffectEditorPanel::restoreState(const QVariantMap& state)
{
    bindEffect(state.value(QStringLiteral("ownerGuid")).toUuid(),
               state.value(QStringLiteral("rowGuid")).toUuid());
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
