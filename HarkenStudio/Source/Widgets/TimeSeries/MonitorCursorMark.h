#pragma once

#include "MonitorCursorModel.h"

#include <QColor>
#include <QString>
#include <QWidget>

namespace VNX {

class MonitorCursorMark : public QWidget
{
    Q_OBJECT

public:
    explicit MonitorCursorMark(QWidget* parent = nullptr);

    void updateData(const MonitorCursorSnapshot& snapshot);
    const MonitorCursorSnapshot& snapshot() const { return snapshot_; }

    QString statusText() const;
    QString uncertaintyLabel() const;
    QColor uncertaintyColor() const;
    MonitorCursorUncertaintyKind displayedUncertainty() const;
    bool showsLine() const;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    MonitorCursorSnapshot snapshot_{};
};

} // namespace VNX
