#pragma once

#include "Monitor/MonitorSessionCompareUi.h"

#include <QString>
#include <QWidget>

class QLabel;

namespace VNX {

class MonitorSessionComparePanel : public QWidget
{
    Q_OBJECT

public:
    explicit MonitorSessionComparePanel(QWidget* parent = nullptr);

    void applyState(const Monitor::MonitorCompareUiState& state);
    QString resultText() const;
    QString pickerText() const;
    QString exportText() const;

private:
    QLabel* resultLabel_ = nullptr;
    QLabel* pickerLabel_ = nullptr;
    QLabel* exportLabel_ = nullptr;
};

} // namespace VNX
