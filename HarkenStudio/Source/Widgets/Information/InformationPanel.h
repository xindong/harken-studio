
#pragma once

#include "Core/ContextInfo.h"

#include <QUuid>
#include <QWidget>

class QFrame;
class QLabel;

namespace VNX {

class InformationPanel : public QWidget
{
    Q_OBJECT

public:
    explicit InformationPanel(QWidget* parent = nullptr);

signals:
    void locateRequested(const QUuid& guid);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void setupUI();
    void applyFont(int size);
    void renderTopic(const ContextTopic& topic);

    QLabel* headerLabel_ = nullptr;
    QFrame* separator_ = nullptr;
    QLabel* bodyLabel_ = nullptr;
    QUuid currentGuid_;
};

}
