#pragma once

#include <QColor>
#include <QFont>
#include <QMargins>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <QStyle>

#include <array>
#include <memory>

class QWidget;

namespace VNX {

struct ControlVisual
{
    QColor background;
    QColor foreground;
    QColor alternateBackground;
    QColor selectionBackground;
    QColor selectionForeground;
    QColor gridline;
    QFont font;
    bool borderSpecified = false;
    QMargins borders;
    std::array<QColor, 4> borderColors;
    std::array<QSizeF, 4> radii{QSizeF(0, 0), QSizeF(0, 0), QSizeF(0, 0), QSizeF(0, 0)};
    QString image;
    Qt::Alignment textAlignment{};
    QMargins padding;
    QMargins margin;
    QSize size;
    QSize minimum;
    QSize maximum;
};

class WidgetAppearance
{
public:
    WidgetAppearance();
    ~WidgetAppearance();
    WidgetAppearance(const WidgetAppearance&) = delete;
    WidgetAppearance& operator=(const WidgetAppearance&) = delete;

    bool updateStyleSheet(QString styleSheet);
    const QString& updateWidgetStyle(QWidget* widget, QString styleSheet);
    void updateWidgetGeometry(QWidget* widget, const ControlVisual& visual);
    const QString& nativeStyleSheet() const;
    ControlVisual visual(const QWidget* widget, QStyle::State state,
                         QStringView part = {}, QStringList extraStates = {}) const;
    void invalidate(const QWidget* widget);
    bool synchronize(const QWidget* widget);
    bool takeDependencyChanges(const QWidget* widget);
    bool usesProperty(const QWidget* widget, const QByteArray& name) const;
    void clearWidgetCache();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
