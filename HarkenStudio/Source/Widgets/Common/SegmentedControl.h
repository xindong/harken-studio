#pragma once

#include <QVariant>
#include <QWidget>
#include <QList>
#include <QPair>

class QButtonGroup;
class QToolButton;

namespace VNX {

class SegmentedControl final : public QWidget
{
    Q_OBJECT

public:
    explicit SegmentedControl(QWidget* parent = nullptr);

    void setOptions(const QList<QPair<QString, QVariant>>& options, bool exclusive = true);
    void setAdaptiveSizing(bool enabled);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    QToolButton* button(int index) const;
    QButtonGroup* buttonGroup() const { return group_; }

private:
    bool event(QEvent* event) override;
    void changeEvent(QEvent* event) override;
    void scheduleLayoutUpdate();
    QButtonGroup* group_ = nullptr;
    bool adaptiveSizing_ = false;
    bool layoutUpdatePending_ = false;
};

}
