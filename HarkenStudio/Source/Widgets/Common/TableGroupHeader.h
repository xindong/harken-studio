#pragma once

#include <QLabel>

class QCheckBox;
class QToolButton;

namespace VNX {

class ThemeManager;

struct TableGroupOptions
{
    bool checkable = false;
    bool collapsible = false;
};

class TableGroupHeader : public QLabel
{
    Q_OBJECT

public:
    TableGroupHeader(const QString& text, QWidget* parent, ThemeManager* themeManager,
                     const TableGroupOptions& options = {});
    void setCheckState(Qt::CheckState state);
    Qt::CheckState checkState() const;
    void setCheckEnabled(bool enabled);
    void setExpanded(bool expanded);
    bool isExpanded() const { return expanded_; }
    static int preferredHeight(ThemeManager* themeManager);
    static QRect checkBoxRect(const QRect& rect, ThemeManager* themeManager, const QWidget* widget);
    static QRect chevronRect(const QRect& rect, ThemeManager* themeManager);
    static QIcon chevronIcon(bool expanded, ThemeManager* themeManager);
    static void paint(QPainter& painter, const QRect& rect, const QString& text,
                      ThemeManager* themeManager, const TableGroupOptions& options = {},
                      const QWidget* widget = nullptr);

signals:
    void checkStateRequested(Qt::CheckState state);
    void expandedChanged(bool expanded);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void applyTheme();
    void layoutControls();

    ThemeManager* themeManager_;
    TableGroupOptions options_;
    QCheckBox* checkBox_ = nullptr;
    QToolButton* chevron_ = nullptr;
    bool expanded_ = true;
};

}
