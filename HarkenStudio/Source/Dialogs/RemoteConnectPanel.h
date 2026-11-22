#pragma once

#include <QWidget>

class QLabel;
class QPushButton;
class QToolButton;

namespace harken::studio::bridge { class RemoteConnection; }

namespace harken::studio {

class ProportionalTreeWidget;

class RemoteConnectPanel final : public QWidget
{
    Q_OBJECT

public:
    explicit RemoteConnectPanel(harken::studio::bridge::RemoteConnection* connection,
                                 QWidget* parent = nullptr);
    QSize sizeHint() const override;

private:
    void refreshTargets();
    void refreshState();
    void selectTarget();
    void connectSelectedTarget();
    void openManualConnection();

    harken::studio::bridge::RemoteConnection* connection_ = nullptr;
    ProportionalTreeWidget* targets_ = nullptr;
    QToolButton* manual_ = nullptr;
    QPushButton* connect_ = nullptr;
    QPushButton* disconnect_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* identity_ = nullptr;
    QString selectedTargetKey_;
};

}
