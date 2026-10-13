#pragma once

#include "NodeContentInfo.h"

#include <QMenu>

#include <array>

class QCheckBox;
class QLabel;
class QScrollArea;

namespace VNX {

class BlueprintVisibilityMenu : public QMenu
{
    Q_OBJECT

public:
    explicit BlueprintVisibilityMenu(QWidget* parent = nullptr);

    void setFields(NodeContentFields fields);

signals:
    void fieldsToggled(NodeContentFields fields);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    QScrollArea* scroll_ = nullptr;
    QWidget* contents_ = nullptr;
    std::array<QCheckBox*, 7> checks_{};
    std::array<QLabel*, 7> labels_{};

    void relayout_();
};

}
