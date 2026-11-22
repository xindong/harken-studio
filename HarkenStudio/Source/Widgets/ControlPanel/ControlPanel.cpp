#include "ControlPanel.h"
#include "Core/ThemeLayout.h"
#include "RehearsalTable.h"
#include "VariableBindingTable.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/FieldGroupProjectAdapter.h"
#include "Core/VariableConsumerSemantics.h"

#include <QGridLayout>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>

#include <algorithm>

ControlPanel::ControlPanel(QWidget* parent)
    : QWidget(parent)
{
    setupUI();

    auto& manager = harken::studio::ProjectManager::instance();
    connect(&manager, &harken::studio::ProjectManager::projectClosed, this,
            [this] {
        setItem(nullptr);
        setPinned(false);
    });
    connect(&manager, &harken::studio::ProjectManager::itemCreated,
            this, &ControlPanel::scheduleVariableRefresh);
    connect(&manager, &harken::studio::ProjectManager::itemDeleted,
            this, &ControlPanel::scheduleVariableRefresh);
    connect(&manager, &harken::studio::ProjectManager::itemMoved,
            this, &ControlPanel::scheduleVariableRefresh);
    connect(&manager, &harken::studio::ProjectManager::itemRenamed,
            this, &ControlPanel::scheduleVariableRefresh);
    connect(&manager, &harken::studio::ProjectManager::itemPropertyChanged,
            this, &ControlPanel::scheduleVariableRefresh);
    connect(&harken::studio::PlatformContextService::instance(),
            &harken::studio::PlatformContextService::contextChanged,
            this, &ControlPanel::scheduleVariableRefresh);
}

void ControlPanel::setItem(harken::studio::ProjectItem* item)
{
    selectedItemId_ = item ? item->guid : QUuid();
    if (pinned_) return;
    boundItemId_ = selectedItemId_;
    refreshVariables();
}

harken::studio::ProjectItem* ControlPanel::boundItem() const
{
    return harken::studio::ProjectManager::instance().findItemByGuid(boundItemId_);
}

void ControlPanel::setPinned(bool pinned)
{
    if (pinned_ == pinned) return;
    pinned_ = pinned;
    if (!pinned_) {
        boundItemId_ = selectedItemId_;
        refreshVariables();
    }
    emit pinToggled(pinned_);
}

QVariantMap ControlPanel::snapshotState() const
{
    QVariantMap state;
    if (pinned_ && boundItem())
        state[QStringLiteral("pinnedObjectGuid")] = boundItemId_.toString(QUuid::WithoutBraces);
    return state;
}

void ControlPanel::restoreState(const QVariantMap& state)
{
    const auto* item = harken::studio::ProjectManager::instance().findItemByGuid(
        QUuid(state.value(QStringLiteral("pinnedObjectGuid")).toString()));
    boundItemId_ = item ? item->guid : selectedItemId_;
    const bool pinned = item != nullptr;
    if (pinned_ != pinned) {
        pinned_ = pinned;
        emit pinToggled(pinned_);
    }
    refreshVariables();
}

QSize ControlPanel::sizeHint() const
{
    const auto& metrics = harken::studio::ThemeManager::instance().metrics();
    return minimumSizeHint().expandedTo(QSize(metrics.dialog.wideWidth, metrics.panel.minimumHeight));
}

QSize ControlPanel::minimumSizeHint() const
{
    const int bodyWidth = std::max(variableTable_->rowsWidget()->minimumWidth(),
                                   bindingTable_->rowsWidget()->minimumWidth());
    const int scrollWidth = variableTable_->scrollArea()->verticalScrollBar()->sizeHint().width();
    const int tableWidth = bodyWidth + scrollWidth + variableTable_->frameWidth() * 2;
    const int tableHeight = std::max(variableTable_->minimumSizeHint().height(),
                                     bindingTable_->minimumSizeHint().height());
    const auto margins = layout()->contentsMargins();
    return QSize(tableWidth * 2 + margins.left() + margins.right()
                 + qobject_cast<QGridLayout*>(layout())->horizontalSpacing(),
                 tableHeight + margins.top() + margins.bottom());
}

void ControlPanel::scheduleVariableRefresh()
{
    if (variableRefreshPending_) return;
    variableRefreshPending_ = true;
    QTimer::singleShot(0, this, [this] {
        variableRefreshPending_ = false;
        refreshVariables();
    });
}

void ControlPanel::refreshVariables()
{
    auto& manager = harken::studio::ProjectManager::instance();
    const auto* item = boundItem();
    if (!item && !boundItemId_.isNull()) {
        boundItemId_ = QUuid();
        if (pinned_) {
            setPinned(false);
            return;
        }
    }
    const auto dependencies = harken::studio::VariableConsumerSemantics::dependenciesForAudition(
        manager.currentProject(), item,
        harken::studio::toFieldGroupGuid(harken::studio::PlatformContextService::instance().context().primary));
    variableTable_->setVariables(dependencies.variables);
    bindingTable_->setConsumers(dependencies.consumers);
    updateGeometry();
    emit contextChanged();
}

void ControlPanel::setupUI()
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setObjectName(QStringLiteral("ControlPanel"));
    setAttribute(Qt::WA_StyledBackground);

    auto* root = new QGridLayout(this);
    harken::studio::ThemeLayout::margins(root, harken::studio::ThemeLayout::Space::Panel);
    harken::studio::ThemeLayout::horizontalSpacing(root, harken::studio::ThemeLayout::Space::Section);
    root->setColumnStretch(0, 1);
    root->setColumnStretch(1, 1);

    variableTable_ = new harken::studio::RehearsalTable(this);
    bindingTable_ = new harken::studio::VariableBindingTable(this);
    root->addWidget(variableTable_, 0, 0);
    root->addWidget(bindingTable_, 0, 1);
}
