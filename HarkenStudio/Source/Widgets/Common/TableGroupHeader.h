#pragma once

#include <QLabel>

namespace VNX {

class ThemeManager;

class TableGroupHeader : public QLabel
{
public:
    TableGroupHeader(const QString& text, QWidget* parent, ThemeManager* themeManager);
    static int preferredHeight(ThemeManager* themeManager);
    static void paint(QPainter& painter, const QRect& rect, const QString& text,
                      ThemeManager* themeManager);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void applyTheme();

    ThemeManager* themeManager_;
};

}
