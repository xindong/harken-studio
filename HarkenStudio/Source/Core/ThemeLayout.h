#pragma once

#include "ThemeManager.h"

#include <QGridLayout>
#include <QBoxLayout>
#include <QLayout>

namespace VNX::ThemeLayout {

enum class Space { None, Tight, Control, Section, Panel };

inline int spacing(const ThemeMetrics& metrics, Space space)
{
    switch (space) {
    case Space::None: return 0;
    case Space::Tight: return metrics.layout.tightGap;
    case Space::Control: return metrics.layout.controlGap;
    case Space::Section: return metrics.layout.sectionGap;
    case Space::Panel: return metrics.layout.panelPadding;
    }
    Q_UNREACHABLE();
}

template<typename Layout>
inline void margins(Layout* layout, Space left, Space top, Space right, Space bottom,
                    ThemeManager& theme = ThemeManager::instance())
{
    theme.bindMetrics(layout, [=](const ThemeMetrics& metrics) {
        layout->setContentsMargins(spacing(metrics, left), spacing(metrics, top),
                                   spacing(metrics, right), spacing(metrics, bottom));
    });
}

template<typename Layout>
inline void margins(Layout* layout, Space space,
                    ThemeManager& theme = ThemeManager::instance())
{
    margins(layout, space, space, space, space, theme);
}

template<typename Layout>
inline void spacing(Layout* layout, Space space,
                    ThemeManager& theme = ThemeManager::instance())
{
    theme.bindMetrics(layout, [=](const ThemeMetrics& metrics) {
        layout->setSpacing(spacing(metrics, space));
    });
}

template<typename Layout>
inline void horizontalSpacing(Layout* layout, Space space,
                              ThemeManager& theme = ThemeManager::instance())
{
    theme.bindMetrics(layout, [=](const ThemeMetrics& metrics) {
        layout->setHorizontalSpacing(spacing(metrics, space));
    });
}

template<typename Layout>
inline void verticalSpacing(Layout* layout, Space space,
                            ThemeManager& theme = ThemeManager::instance())
{
    theme.bindMetrics(layout, [=](const ThemeMetrics& metrics) {
        layout->setVerticalSpacing(spacing(metrics, space));
    });
}

class SpacingItem final : public QSpacerItem
{
public:
    SpacingItem(QBoxLayout* layout, Space space, ThemeManager& theme)
        : QSpacerItem(0, 0)
    {
        connection_ = theme.bindMetrics(layout, [this, layout, space](const ThemeMetrics& metrics) {
            const auto direction = layout->direction();
            const bool horizontal = direction == QBoxLayout::LeftToRight || direction == QBoxLayout::RightToLeft;
            const int extent = spacing(metrics, space);
            changeSize(horizontal ? extent : 0, horizontal ? 0 : extent,
                       QSizePolicy::Fixed, QSizePolicy::Fixed);
            layout->invalidate();
        });
    }

    ~SpacingItem() override { QObject::disconnect(connection_); }

private:
    QMetaObject::Connection connection_;
};

inline void addSpacing(QBoxLayout* layout, Space space,
                       ThemeManager& theme = ThemeManager::instance())
{
    layout->addSpacerItem(new SpacingItem(layout, space, theme));
}

}
