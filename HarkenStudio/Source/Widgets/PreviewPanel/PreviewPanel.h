#pragma once

#include <QWidget>
#include <QUuid>
#include <QVariantMap>
#include <cstdint>

namespace VNX {
class RehearsalTable;
class VariableBindingTable;
struct ProjectItem;
}

class PreviewPanel : public QWidget
{
    Q_OBJECT
public:
    explicit PreviewPanel(QWidget* parent = nullptr);

    void setItem(VNX::ProjectItem* item);
    VNX::ProjectItem* boundItem() const;
    bool isPinned() const { return pinned_; }
    void setPinned(bool pinned);
    QVariantMap snapshotState() const;
    void restoreState(const QVariantMap& state);
    VNX::RehearsalTable* variableTable() const { return variableTable_; }
    VNX::VariableBindingTable* bindingTable() const { return bindingTable_; }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void pinToggled(bool pinned);
    void contextChanged();

private:
    void setupUI();
    void scheduleVariableRefresh();
    void refreshVariables();

    VNX::RehearsalTable* variableTable_ = nullptr;
    VNX::VariableBindingTable* bindingTable_ = nullptr;
    QUuid selectedItemId_;
    QUuid boundItemId_;
    bool pinned_ = false;
    bool variableRefreshPending_ = false;
};
