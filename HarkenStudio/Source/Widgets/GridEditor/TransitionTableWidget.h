#pragma once

#include "Core/ExternalEditorSelection.h"
#include "Core/PlacementContext.h"
#include "Core/TransitionFieldGating.h"
#include "Core/TransitionSemantics.h"
#include "Widgets/TransitionFade/TransitionRuleEditing.h"

#include <QWidget>

#include <functional>
#include <optional>
#include <vector>

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

signals:
    void externalEditorRequested(const ExternalEditorSelection& selection);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    using Endpoints = TransitionEndpoints;

    const TransitionTableDef* table() const;
    void rebuildRows();
    QWidget* buildRow(int displayIndex, const TransitionRuleDef& rule);
    QWidget* buildEndpointCell(QWidget* row, const TransitionRuleDef& rule, bool source);
    QWidget* buildExitCell(QWidget* row, const TransitionRuleDef& rule, const TransitionFieldGating& gating);
    QWidget* buildEntryCell(QWidget* row, const TransitionRuleDef& rule, const TransitionFieldGating& gating);
    QWidget* buildDetails(const TransitionRuleDef& rule, const TransitionFieldGating& gating);
    void selectRow(int displayIndex);
    void refreshFooter();
    void addRule();
    void removeSelected();
    bool updateRule(const Endpoints& endpoints, const std::function<void(TransitionRuleDef&)>& edit,
                    const QString& undoText);
    bool commit(const TransitionTableDef& candidate, const QString& undoText);
    void setEndpoint(const Endpoints& endpoints, bool source, const std::optional<QUuid>& guid);

    ProjectItem* boundItem_ = nullptr;
    std::optional<PlacementContext> placement_;
    std::optional<Endpoints> selected_;
    std::vector<Endpoints> expanded_;
    std::vector<QWidget*> rows_;
    std::vector<QWidget*> details_;
    std::vector<Endpoints> rowEndpoints_;
    TableWidget* table_ = nullptr;
    QToolButton* addButton_ = nullptr;
    QToolButton* removeButton_ = nullptr;
    quint64 generation_ = 0;
    bool selfWriting_ = false;
};

}
