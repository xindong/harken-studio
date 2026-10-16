#pragma once

#include <QUuid>
#include <QWidget>

#include <cstdint>
#include <vector>

class QToolButton;

namespace VNX {

struct ProjectItem;
struct SelectionData;
struct SelectionEntry;
class TableWidget;

class SelectionListWidget : public QWidget
{
    Q_OBJECT

public:
    explicit SelectionListWidget(QWidget* parent = nullptr);

    void setBoundItem(ProjectItem* item);
    ProjectItem* boundItem() const { return boundItem_; }

signals:
    void drillRequested(const QUuid& target);
    void navigateToItem(uint32_t objectID);
    void locateItem(uint32_t objectID);
    void sourceEditorRequested(const QUuid& soundGuid, const QUuid& entryId);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void setWeightVisible(bool visible);
    QWidget* buildHeader();
    QWidget* buildFooter();
    QWidget* buildRow(int index, const SelectionEntry& occurrence, ProjectItem* child);
    void selectRow(int index);
    bool canSelectChild(uint32_t objectId) const;
    void showAddSelector();
    bool canAddChildren(const QList<uint32_t>& objectIds) const;
    bool addChildren(const QList<uint32_t>& objectIds);
    bool replaceOccurrence(uint32_t stableOccurrenceOrdinal, uint32_t objectId);
    void removeSelected();
    SelectionData occurrenceCandidate() const;
    bool commitOccurrences(SelectionData candidate);
    void moveRow(int from, int to);
    void commitWeight(uint32_t stableOccurrenceOrdinal, int value);
    void refreshWeightValues();
    bool hasRowForChild(const QUuid& childGuid) const;
    ProjectItem* occurrenceTarget(const QUuid& childGuid) const;
    void rebuildRows();

    ProjectItem* boundItem_ = nullptr;
    TableWidget* box_ = nullptr;
    QWidget* rows_ = nullptr;
    QToolButton* addButton_ = nullptr;
    QToolButton* removeButton_ = nullptr;
    std::vector<QWidget*> rowWidgets_;
    int selected_ = -1;
    bool weightVisible_ = true;
    bool refreshingWeights_ = false;
    bool selfWriting_ = false;
    quint64 generation_ = 0;
};

}
