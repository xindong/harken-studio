#pragma once

#include "Core/BaseDialog.h"

class QLabel;
class QPushButton;
class QToolButton;

namespace HarkenEditor::Bridge { class RemoteConnection; }

namespace VNX {

class ProportionalTreeWidget;

class RemoteConnectDialog final : public BaseDialog
{
    Q_OBJECT

public:
    explicit RemoteConnectDialog(HarkenEditor::Bridge::RemoteConnection* connection,
                                 QWidget* parent = nullptr);

private:
    void refreshTargets();
    void refreshState();
    void selectTarget();
    void connectSelectedTarget();
    void openManualConnection();

    HarkenEditor::Bridge::RemoteConnection* connection_ = nullptr;
    ProportionalTreeWidget* targets_ = nullptr;
    QToolButton* manual_ = nullptr;
    QPushButton* connect_ = nullptr;
    QPushButton* disconnect_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* identity_ = nullptr;
    QString selectedTargetKey_;
};

}
