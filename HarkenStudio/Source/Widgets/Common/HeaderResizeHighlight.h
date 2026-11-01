#pragma once

#include <QWidget>

class QHeaderView;

namespace VNX {

class ThemeManager;

class HeaderResizeHighlight final : public QWidget
{
public:
    HeaderResizeHighlight(QHeaderView* header, ThemeManager* theme);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    bool canResize(int column) const;
    int columnAt(int x) const;
    void refresh();

    QHeaderView* header_;
    ThemeManager* theme_;
    QPoint position_;
    int resizingColumn_ = -1;
    bool inside_ = false;
    bool pressed_ = false;
};

}
