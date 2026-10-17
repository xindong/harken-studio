#include "TableGroupHeader.h"

#include "Core/FontManager.h"
#include "Core/ThemeManager.h"

#include <QPainter>

#include <algorithm>

namespace VNX {

namespace {

QFont captionFont(ThemeManager* themeManager)
{
    auto font = FontManager::instance().getFont(themeManager->snapshot(), FontRole::Body);
    font.setBold(true);
    return font;
}

}

TableGroupHeader::TableGroupHeader(const QString& text, QWidget* parent, ThemeManager* themeManager)
    : QLabel(text, parent), themeManager_(themeManager)
{
    setProperty("tableGroup", true);
    setTextFormat(Qt::PlainText);
    setAlignment(Qt::AlignCenter);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    connect(themeManager_, &ThemeManager::themeChanged, this, &TableGroupHeader::applyTheme);
    connect(&FontManager::instance(), &FontManager::fontChanged, this, &TableGroupHeader::applyTheme);
    applyTheme();
}

int TableGroupHeader::preferredHeight(ThemeManager* themeManager)
{
    const auto& metrics = themeManager->metrics().table;
    return std::max(metrics.headerHeight,
        QFontMetrics(captionFont(themeManager)).height() + metrics.cellPaddingY * 2 + 1);
}

void TableGroupHeader::paint(QPainter& painter, const QRect& rect, const QString& text,
                             ThemeManager* themeManager)
{
    const auto& colors = themeManager->colors();
    const auto& metrics = themeManager->metrics().table;
    const auto font = captionFont(themeManager);
    painter.save();
    painter.fillRect(rect, colors.headerBackground);
    painter.setPen(colors.borderPrimary);
    painter.drawLine(rect.bottomLeft(), rect.bottomRight());
    painter.setPen(colors.textMuted);
    painter.setFont(font);
    const auto content = rect.adjusted(metrics.cellPaddingX, metrics.cellPaddingY,
                                       -metrics.cellPaddingX, -metrics.cellPaddingY);
    painter.drawText(content, Qt::AlignCenter,
        QFontMetrics(font).elidedText(text, Qt::ElideRight, content.width()));
    painter.restore();
}

void TableGroupHeader::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    paint(painter, rect(), text(), themeManager_);
}

void TableGroupHeader::applyTheme()
{
    setFont(captionFont(themeManager_));
    setFixedHeight(preferredHeight(themeManager_));
    update();
}

}
