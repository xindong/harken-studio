#pragma once

#include <QPointer>
#include <QTableView>

namespace VNX {

class ThemeManager;

class ResizableTableView : public QTableView
{
    Q_OBJECT

public:
    explicit ResizableTableView(QWidget* parent = nullptr, ThemeManager* theme = nullptr);
    void setMinimumRowHeight(int height);

signals:
    void rowHeightAdjusted(int row, int height);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void updateGeometries() override;
    void scrollContentsBy(int dx, int dy) override;

private:
    void scheduleHandles();
    void refreshHandles();
    void finishResize(bool cancel);

    ThemeManager* theme_;
    QList<QPointer<QWidget>> handles_;
    QPointer<QWidget> activeHandle_;
    QPoint dragOrigin_;
    QList<int> initialWidths_;
    int initialHeight_ = 0;
    int minimumRowHeight_ = 0;
    bool handlesPending_ = false;
};

}
