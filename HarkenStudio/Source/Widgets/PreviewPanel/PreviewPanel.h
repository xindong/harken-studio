#pragma once

#include <QWidget>
#include <QUuid>
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
    VNX::RehearsalTable* variableTable() const { return variableTable_; }
    VNX::VariableBindingTable* bindingTable() const { return bindingTable_; }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

private:
    void setupUI();
    void scheduleVariableRefresh();
    void refreshVariables();

    VNX::RehearsalTable* variableTable_ = nullptr;
    VNX::VariableBindingTable* bindingTable_ = nullptr;
    QUuid selectedItemId_;
    bool variableRefreshPending_ = false;
};
