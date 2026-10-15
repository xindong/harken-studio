#include "PreviewPanel.h"
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

PreviewPanel::PreviewPanel(QWidget* parent)
    : QWidget(parent)
{
    setupUI();

    auto& manager = VNX::ProjectManager::instance();
    connect(&manager, &VNX::ProjectManager::projectClosed, this,
            [this] {
        setItem(nullptr);
        setPinned(false);
    });
    connect(&manager, &VNX::ProjectManager::itemCreated,
            this, &PreviewPanel::scheduleVariableRefresh);
    connect(&manager, &VNX::ProjectManager::itemDeleted,
            this, &PreviewPanel::scheduleVariableRefresh);
    connect(&manager, &VNX::ProjectManager::itemMoved,
            this, &PreviewPanel::scheduleVariableRefresh);
    connect(&manager, &VNX::ProjectManager::itemRenamed,
            this, &PreviewPanel::scheduleVariableRefresh);
    connect(&manager, &VNX::ProjectManager::itemPropertyChanged,
            this, &PreviewPanel::scheduleVariableRefresh);
    connect(&VNX::PlatformContextService::instance(),
            &VNX::PlatformContextService::contextChanged,
            this, &PreviewPanel::scheduleVariableRefresh);
}

void PreviewPanel::setItem(VNX::ProjectItem* item)
{
    selectedItemId_ = item ? item->guid : QUuid();
    if (pinned_) return;
    boundItemId_ = selectedItemId_;
    refreshVariables();
}

VNX::ProjectItem* PreviewPanel::boundItem() const
{
    return VNX::ProjectManager::instance().findItemByGuid(boundItemId_);
}

void PreviewPanel::setPinned(bool pinned)
{
    if (pinned_ == pinned) return;
    pinned_ = pinned;
    if (!pinned_) {
        boundItemId_ = selectedItemId_;
        refreshVariables();
    }
    emit pinToggled(pinned_);
}

QVariantMap PreviewPanel::snapshotState() const
{
    QVariantMap state;
    if (pinned_ && boundItem())
        state[QStringLiteral("pinnedObjectGuid")] = boundItemId_.toString(QUuid::WithoutBraces);
    return state;
}

void PreviewPanel::restoreState(const QVariantMap& state)
{
    const auto* item = VNX::ProjectManager::instance().findItemByGuid(
        QUuid(state.value(QStringLiteral("pinnedObjectGuid")).toString()));
    boundItemId_ = item ? item->guid : selectedItemId_;
    const bool pinned = item != nullptr;
    if (pinned_ != pinned) {
        pinned_ = pinned;
        emit pinToggled(pinned_);
    }
    refreshVariables();
}

QSize PreviewPanel::sizeHint() const
{
    return QSize(std::max(880, minimumSizeHint().width()), 140);
}

QSize PreviewPanel::minimumSizeHint() const
{
    const int bodyWidth = std::max(variableTable_->rowsWidget()->minimumWidth(),
                                   bindingTable_->rowsWidget()->minimumWidth());
    const int scrollWidth = variableTable_->scrollArea()->verticalScrollBar()->sizeHint().width();
    const int tableWidth = bodyWidth + scrollWidth + variableTable_->frameWidth() * 2;
    const int tableHeight = std::max(variableTable_->minimumSizeHint().height(),
                                     bindingTable_->minimumSizeHint().height());
    return QSize(tableWidth * 2 + 32, tableHeight + 16);
}

void PreviewPanel::scheduleVariableRefresh()
{
    if (variableRefreshPending_) return;
    variableRefreshPending_ = true;
    QTimer::singleShot(0, this, [this] {
        variableRefreshPending_ = false;
        refreshVariables();
    });
}

void PreviewPanel::refreshVariables()
{
    auto& manager = VNX::ProjectManager::instance();
    const auto* item = boundItem();
    if (!item && !boundItemId_.isNull()) {
        boundItemId_ = QUuid();
        if (pinned_) {
            setPinned(false);
            return;
        }
    }
    const auto dependencies = VNX::VariableConsumerSemantics::dependenciesForAudition(
        manager.currentProject(), item,
        VNX::toFieldGroupGuid(VNX::PlatformContextService::instance().context().primary));
    variableTable_->setVariables(dependencies.variables);
    bindingTable_->setConsumers(dependencies.consumers);
    updateGeometry();
    emit contextChanged();
}

void PreviewPanel::setupUI()
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setObjectName(QStringLiteral("PreviewPanel"));

    auto* root = new QGridLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setHorizontalSpacing(16);
    root->setColumnStretch(0, 1);
    root->setColumnStretch(1, 1);

    variableTable_ = new VNX::RehearsalTable(this);
    bindingTable_ = new VNX::VariableBindingTable(this);
    root->addWidget(variableTable_, 0, 0);
    root->addWidget(bindingTable_, 0, 1);
}
