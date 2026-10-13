#pragma once

#include "TransitionEditorWidget.h"

#include <QUuid>
#include <QVariantMap>
#include <QWidget>

#include <optional>

class QEvent;
class QFocusEvent;
class QObject;

namespace VNX {

class TransitionEditorPanel final : public QWidget
{
    Q_OBJECT

public:
    explicit TransitionEditorPanel(QWidget* parent = nullptr);

    static bool acceptsItem(const ProjectItem* item);

    int panelId() const { return panelId_; }
    void setPanelId(int id) { panelId_ = id; }
    bool isPinned() const { return editor_->isPinned(); }
    void setPinned(bool pinned);

    QUuid currentItemGuid() const { return editor_->currentItemGuid(); }
    ProjectItem* boundItem() const { return editor_->boundItem(); }
    QString stateText() const { return editor_->stateText(); }
    TransitionMountState mountState() const { return editor_->mountState(); }
    const PlacementContext* placementContext() const { return editor_->placementContext(); }
    TransitionSemantics::TransitionWatchView watchView() const { return editor_->watchView(); }

    int ruleCount() const { return editor_->ruleCount(); }
    bool ruleIsDefault(int displayIndex) const { return editor_->ruleIsDefault(displayIndex); }
    int ruleSpecificity(int displayIndex) const { return editor_->ruleSpecificity(displayIndex); }
    std::optional<QUuid> ruleSource(int displayIndex) const { return editor_->ruleSource(displayIndex); }
    std::optional<QUuid> ruleTarget(int displayIndex) const { return editor_->ruleTarget(displayIndex); }
    TransitionExitTiming ruleExitTiming(int displayIndex) const { return editor_->ruleExitTiming(displayIndex); }
    std::optional<QUuid> ruleBridge(int displayIndex) const { return editor_->ruleBridge(displayIndex); }
    TransitionEntryMode ruleEntryMode(int displayIndex) const { return editor_->ruleEntryMode(displayIndex); }
    TransitionFadeDef ruleFade(int displayIndex, TransitionFadeSlot slot) const
    {
        return editor_->ruleFade(displayIndex, slot);
    }
    QString ruleSummaryText(int displayIndex) const { return editor_->ruleSummaryText(displayIndex); }

    int selectedRuleIndex() const { return editor_->selectedRuleIndex(); }
    bool selectRuleDisplayIndex(int displayIndex)
    {
        return editor_->selectRuleDisplayIndex(displayIndex);
    }

    int candidateCount() const { return editor_->candidateCount(); }
    QUuid candidateGuid(int index) const { return editor_->candidateGuid(index); }
    bool isCombinationOccupied(const std::optional<QUuid>& source,
                               const std::optional<QUuid>& target) const
    {
        return editor_->isCombinationOccupied(source, target);
    }

    bool addRule(const std::optional<QUuid>& source, const std::optional<QUuid>& target)
    {
        return editor_->addRule(source, target);
    }
    bool removeRule(int displayIndex) { return editor_->removeRule(displayIndex); }

    bool setRuleEndpoints(int displayIndex, const std::optional<QUuid>& source,
                          const std::optional<QUuid>& target)
    {
        return editor_->setRuleEndpoints(displayIndex, source, target);
    }
    bool setRuleExitTiming(int displayIndex, TransitionExitTiming timing)
    {
        return editor_->setRuleExitTiming(displayIndex, timing);
    }
    bool setRuleBridge(int displayIndex, const std::optional<QUuid>& bridge)
    {
        return editor_->setRuleBridge(displayIndex, bridge);
    }
    bool setRuleEntryMode(int displayIndex, TransitionEntryMode mode)
    {
        return editor_->setRuleEntryMode(displayIndex, mode);
    }
    bool setRuleFade(int displayIndex, TransitionFadeSlot slot, const TransitionFadeDef& fade)
    {
        return editor_->setRuleFade(displayIndex, slot, fade);
    }

    QString samePositionReadoutText(int displayIndex) const
    {
        return editor_->samePositionReadoutText(displayIndex);
    }
    QString handoverReadoutText() const { return editor_->handoverReadoutText(); }
    bool spliceReadoutVisible() const { return editor_->spliceReadoutVisible(); }

    QString lastInteractionError() const { return editor_->lastInteractionError(); }

    QVariantMap snapshotState() const { return editor_->snapshotState(); }

public slots:
    void bindItem(ProjectItem* item);
    void bindGuid(const QUuid& guid);
    void setPlacementContext(const std::optional<PlacementContext>& placement);
    void clearEditor();
    void restoreState(const QVariantMap& state);

signals:
    void panelFocused(int panelId);
    void pinToggled(bool pinned);
    void watchViewChanged(const TransitionSemantics::TransitionWatchView& view);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;

private:
    void installFocusFilterRecursively(QObject* root);

    int panelId_ = 0;
    TransitionEditorWidget* editor_ = nullptr;
};

}
