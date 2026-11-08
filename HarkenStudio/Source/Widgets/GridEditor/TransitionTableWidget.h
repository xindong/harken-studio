#pragma once

#include "Core/PlacementContext.h"
#include "Core/TransitionSemantics.h"

#include <QWidget>

#include <functional>
#include <optional>
#include <utility>
#include <vector>

class QComboBox;
class QKeyEvent;
class QToolButton;

namespace VNX {

struct ProjectItem;
class TableWidget;

class TransitionTableWidget final : public QWidget
{
    Q_OBJECT

public:
    explicit TransitionTableWidget(QWidget* parent = nullptr);

    void setBoundItem(ProjectItem* item);
    void setPlacementContext(const std::optional<PlacementContext>& placement);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    using Endpoints = std::pair<std::optional<QUuid>, std::optional<QUuid>>;
    enum class FadeSlot { SourceOut, TargetIn };

    const TransitionTableDef* table() const;
    void rebuildRows();
    QWidget* buildRow(int displayIndex, const TransitionRuleDef& rule);
    QWidget* buildEndpointCell(QWidget* row, const TransitionRuleDef& rule, bool source);
    QWidget* buildExitCell(QWidget* row, const TransitionRuleDef& rule);
    QWidget* buildEntryCell(QWidget* row, const TransitionRuleDef& rule);
    QWidget* buildFadeCell(QWidget* row, const TransitionRuleDef& rule, FadeSlot slot);
    void selectRow(int displayIndex);
    void refreshFooter();
    void addRule();
    void removeSelected();
    bool updateRule(const Endpoints& endpoints, const std::function<void(TransitionRuleDef&)>& edit,
                    const QString& undoText);
    bool commit(const TransitionTableDef& candidate, const QString& undoText);
    void openFadePopup(const Endpoints& endpoints, FadeSlot slot, QWidget* anchor);

    ProjectItem* boundItem_ = nullptr;
    std::optional<PlacementContext> placement_;
    std::optional<Endpoints> selected_;
    std::vector<QWidget*> rows_;
    std::vector<Endpoints> rowEndpoints_;
    TableWidget* table_ = nullptr;
    QToolButton* addButton_ = nullptr;
    QToolButton* removeButton_ = nullptr;
    quint64 generation_ = 0;
    bool selfWriting_ = false;
};

}
