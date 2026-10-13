#pragma once

#include "Core/PlacementContext.h"
#include "Core/ProjectTypes.h"
#include "Core/Timeline/TimelineTypes.h"
#include "Core/TransitionSemantics.h"

#include <QUuid>
#include <QVariantMap>
#include <QWidget>

#include <optional>
#include <utility>
#include <vector>

class QComboBox;
class QKeyEvent;
class QLabel;
class QResizeEvent;
class QScrollArea;
class QShowEvent;
class QStackedWidget;
class QToolButton;

namespace VNX {

struct ProjectItem;
class CollapsibleSection;
class TableWidget;
class ObjectRefWidget;
class TransitionFadeButton;

enum class TransitionFadeSlot : uint8_t
{
    SourceFadeOut = 0,
    BridgeFadeIn,
    BridgeFadeOut,
    TargetFadeIn
};

class TransitionEditorWidget final : public QWidget
{
    Q_OBJECT

public:
    explicit TransitionEditorWidget(QWidget* parent = nullptr);

    static bool acceptsItem(const ProjectItem* item);
    static bool acceptsItemType(ProjectItem::Type type);

    bool isPinned() const { return pinned_; }
    void setPinned(bool pinned);

    QUuid currentItemGuid() const { return boundGuid_; }
    ProjectItem* boundItem() const { return boundItem_; }
    QString stateText() const;
    TransitionMountState mountState() const { return mountState_; }
    const PlacementContext* placementContext() const;
    TransitionSemantics::TransitionWatchView watchView() const { return watchView_; }

    int ruleCount() const;
    bool ruleIsDefault(int displayIndex) const;
    int ruleSpecificity(int displayIndex) const;
    std::optional<QUuid> ruleSource(int displayIndex) const;
    std::optional<QUuid> ruleTarget(int displayIndex) const;
    TransitionExitTiming ruleExitTiming(int displayIndex) const;
    std::optional<QUuid> ruleBridge(int displayIndex) const;
    TransitionEntryMode ruleEntryMode(int displayIndex) const;
    TransitionFadeDef ruleFade(int displayIndex, TransitionFadeSlot slot) const;
    QString ruleSummaryText(int displayIndex) const;

    int selectedRuleIndex() const;
    bool selectRuleDisplayIndex(int displayIndex);

    int candidateCount() const;
    QUuid candidateGuid(int index) const;
    bool isCombinationOccupied(const std::optional<QUuid>& source,
                               const std::optional<QUuid>& target) const;

    bool addRule(const std::optional<QUuid>& source, const std::optional<QUuid>& target);
    bool removeRule(int displayIndex);
    bool removeSelectedRule();

    bool setRuleEndpoints(int displayIndex, const std::optional<QUuid>& source,
                          const std::optional<QUuid>& target);
    bool setRuleExitTiming(int displayIndex, TransitionExitTiming timing);
    bool setRuleBridge(int displayIndex, const std::optional<QUuid>& bridge);
    bool setRuleEntryMode(int displayIndex, TransitionEntryMode mode);
    bool setRuleFade(int displayIndex, TransitionFadeSlot slot, const TransitionFadeDef& fade);

    QString samePositionReadoutText(int displayIndex) const;
    QString handoverReadoutText() const;
    bool spliceReadoutVisible() const;

    QString lastInteractionError() const { return lastInteractionError_; }

    QVariantMap snapshotState() const;

public slots:
    void bindItem(ProjectItem* item);
    void bindGuid(const QUuid& guid);
    void setPlacementContext(const std::optional<PlacementContext>& placement);
    void clearEditor();
    void restoreState(const QVariantMap& state);

signals:
    void pinToggled(bool pinned);
    void watchViewChanged(const TransitionSemantics::TransitionWatchView& view);

protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    using Endpoints = std::pair<std::optional<QUuid>, std::optional<QUuid>>;

    bool addUnusedRule();

    void rebuild();
    void rebuildRuleList();
    void rebuildDetail();
    void applyHostMargins();
    void refreshChrome();
    void refreshStatusBar();
    void refreshHeadTooltips();
    void applyDefaultCollapse(bool force);
    void refreshRehearsalWinner();
    void refreshWatchView();
    QString watchReadoutText() const;
    QString watchReadoutTooltip() const;
    void refreshSpliceReadout();
    void showState(const QString& text);
    void rejectInteraction(const QString& error);
    bool commitTable(TransitionTableDef candidate, const QString& undoText);
    bool applyRuleUpdate(int displayIndex, TransitionRuleDef rule, const QString& undoText);
    void openFadePopup(TransitionFadeSlot slot, QWidget* anchor);
    void selectRule(int displayIndex);
    void refreshFooterGating();
    QLabel* makeEndpointCell(QWidget* parent, const std::optional<QUuid>& guid) const;
    QString endpointName(const std::optional<QUuid>& guid) const;
    QString exitTimingText(TransitionExitTiming timing) const;
    QString entryModeText(TransitionEntryMode mode) const;
    QString mountStateExplanation() const;
    QString mountBadgeTooltip() const;
    QString mountNoteText() const;
    QString modeText() const;
    TimelineItemResolver itemResolver() const;
    const TransitionRuleDef* ruleAt(int displayIndex) const;
    const TransitionTableDef* table() const;
    std::vector<ProjectItem*> candidateChildren() const;
    std::optional<Endpoints> firstUnusedCombination() const;
    bool tableReferences(const QUuid& guid) const;
    bool isQueueMode() const;
    bool isNarrow() const;

    bool pinned_ = false;
    bool collapseDirty_ = false;
    bool applyingCollapse_ = false;
    bool bridgeFormRevealed_ = false;
    int selectedDisplayIndex_ = -1;
    QUuid boundGuid_;
    ProjectItem* boundItem_ = nullptr;
    TransitionMountState mountState_ = TransitionMountState::None;
    std::optional<Endpoints> selectedEndpoints_;
    std::optional<PlacementContext> placementContext_;
    TransitionSemantics::TransitionWatchView watchView_;
    QString lastInteractionError_;

    QLabel* mountBadge_ = nullptr;
    QLabel* modeLabel_ = nullptr;
    QLabel* statusSeparator_ = nullptr;
    QLabel* ruleCountLabel_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QLabel* stateLabel_ = nullptr;
    QScrollArea* scroll_ = nullptr;
    QWidget* contentPage_ = nullptr;

    CollapsibleSection* rulesSection_ = nullptr;
    QLabel* mountNoteLabel_ = nullptr;
    QWidget* listRows_ = nullptr;
    TableWidget* listTable_ = nullptr;
    QWidget* listHead_ = nullptr;
    QLabel* headNum_ = nullptr;
    QLabel* headSource_ = nullptr;
    QLabel* headTarget_ = nullptr;
    QLabel* headExit_ = nullptr;
    QLabel* headEntry_ = nullptr;
    QWidget* headFlags_ = nullptr;
    QToolButton* addButton_ = nullptr;
    QToolButton* removeButton_ = nullptr;
    std::vector<QWidget*> rowWidgets_;

    CollapsibleSection* sourceExitSection_ = nullptr;
    ObjectRefWidget* sourceRef_ = nullptr;
    QLabel* sourceAnyLabel_ = nullptr;
    QComboBox* exitTimingCombo_ = nullptr;
    TransitionFadeButton* sourceFadeButton_ = nullptr;
    QLabel* exitNoteLabel_ = nullptr;

    CollapsibleSection* bridgeSection_ = nullptr;
    QWidget* bridgeSlotAdd_ = nullptr;
    QToolButton* bridgeInsertButton_ = nullptr;
    QWidget* bridgeForm_ = nullptr;
    ObjectRefWidget* bridgeRef_ = nullptr;
    TransitionFadeButton* bridgeFadeInButton_ = nullptr;
    TransitionFadeButton* bridgeFadeOutButton_ = nullptr;

    CollapsibleSection* targetEntrySection_ = nullptr;
    ObjectRefWidget* targetRef_ = nullptr;
    QLabel* targetAnyLabel_ = nullptr;
    QComboBox* entryModeCombo_ = nullptr;
    TransitionFadeButton* targetFadeInButton_ = nullptr;
    QWidget* positionRow_ = nullptr;
    QLabel* positionValue_ = nullptr;

    CollapsibleSection* resolutionSection_ = nullptr;
    QLabel* watchValue_ = nullptr;
    QLabel* handoverValue_ = nullptr;
    QWidget* spliceRow_ = nullptr;
    QLabel* spliceValue_ = nullptr;
};

}
