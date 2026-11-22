#pragma once

#include <QWidget>
#include <QUuid>
#include <QVariantMap>
#include <cstdint>

namespace harken::studio {
class RehearsalTable;
class VariableBindingTable;
struct ProjectItem;
}

class ControlPanel : public QWidget
{
    Q_OBJECT
public:
    explicit ControlPanel(QWidget* parent = nullptr);

    void setItem(harken::studio::ProjectItem* item);
    harken::studio::ProjectItem* boundItem() const;
    bool isPinned() const { return pinned_; }
    void setPinned(bool pinned);
    QVariantMap snapshotState() const;
    void restoreState(const QVariantMap& state);
    harken::studio::RehearsalTable* variableTable() const { return variableTable_; }
    harken::studio::VariableBindingTable* bindingTable() const { return bindingTable_; }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void pinToggled(bool pinned);
    void contextChanged();

private:
    void setupUI();
    void scheduleVariableRefresh();
    void refreshVariables();

    harken::studio::RehearsalTable* variableTable_ = nullptr;
    harken::studio::VariableBindingTable* bindingTable_ = nullptr;
    QUuid selectedItemId_;
    QUuid boundItemId_;
    bool pinned_ = false;
    bool variableRefreshPending_ = false;
};
