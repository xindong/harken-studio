#include "TableGroupHeader.h"

#include "Core/FontManager.h"
#include "Core/ThemeAssetCatalog.h"
#include "Core/ThemeManager.h"

#include <QApplication>
#include <QCheckBox>
#include <QPainter>
#include <QStyle>
#include <QToolButton>

#include <algorithm>

namespace VNX {

namespace {

class GroupCheckBox : public QCheckBox
{
public:
    using QCheckBox::QCheckBox;

protected:
    void nextCheckState() override
    {
        setCheckState(checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
    }
};

QFont captionFont(ThemeManager* themeManager)
{
    auto font = FontManager::instance().getFont(themeManager->snapshot(), FontRole::Body);
    font.setBold(true);
    return font;
}

}

TableGroupHeader::TableGroupHeader(const QString& text, QWidget* parent, ThemeManager* themeManager,
                                   const TableGroupOptions& options)
    : QLabel(text, parent), themeManager_(themeManager), options_(options)
{
    setProperty("tableGroup", true);
    setTextFormat(Qt::PlainText);
    setAlignment(Qt::AlignCenter);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    if (options_.checkable) {
        checkBox_ = new GroupCheckBox(this);
        checkBox_->setAccessibleName(tr("Select All in %1").arg(text));
        connect(checkBox_, &QCheckBox::clicked, this, [this] {
            emit checkStateRequested(checkBox_->isChecked() ? Qt::Checked : Qt::Unchecked);
        });
    }
    if (options_.collapsible) {
        chevron_ = new QToolButton(this);
        chevron_->setProperty("tableGroupChevron", true);
        chevron_->setAccessibleName(tr("Expand or Collapse %1").arg(text));
        connect(chevron_, &QToolButton::clicked, this, [this] { setExpanded(!expanded_); });
    }
    connect(themeManager_, &ThemeManager::themeChanged, this, &TableGroupHeader::applyTheme);
    connect(themeManager_, &ThemeManager::appearanceChanged, this, [this] {
        if (chevron_) chevron_->setIcon(chevronIcon(expanded_, themeManager_));
        update();
    });
    connect(&FontManager::instance(), &FontManager::fontChanged, this, &TableGroupHeader::applyTheme);
    applyTheme();
}

void TableGroupHeader::setCheckState(Qt::CheckState state)
{
    if (!checkBox_) return;
    checkBox_->setCheckState(state);
}

Qt::CheckState TableGroupHeader::checkState() const
{
    return checkBox_ ? checkBox_->checkState() : Qt::Unchecked;
}

void TableGroupHeader::setCheckEnabled(bool enabled)
{
    if (checkBox_) checkBox_->setEnabled(enabled);
}

void TableGroupHeader::setExpanded(bool expanded)
{
    if (expanded_ == expanded) return;
    expanded_ = expanded;
    if (chevron_) chevron_->setIcon(chevronIcon(expanded_, themeManager_));
    emit expandedChanged(expanded_);
}

int TableGroupHeader::preferredHeight(ThemeManager* themeManager)
{
    const auto& metrics = themeManager->metrics().table;
    return std::max(metrics.headerHeight,
        QFontMetrics(captionFont(themeManager)).height() + metrics.cellPaddingY * 2 + 1);
}

QRect TableGroupHeader::checkBoxRect(const QRect& rect, ThemeManager* themeManager, const QWidget* widget)
{
    auto* style = widget ? widget->style() : QApplication::style();
    const int width = style->pixelMetric(QStyle::PM_IndicatorWidth, nullptr, widget);
    const int height = style->pixelMetric(QStyle::PM_IndicatorHeight, nullptr, widget);
    return QRect(rect.left() + themeManager->metrics().table.cellPaddingX,
                 rect.top() + (rect.height() - height) / 2, width, height);
}

QRect TableGroupHeader::chevronRect(const QRect& rect, ThemeManager* themeManager)
{
    const int extent = themeManager->metrics().navigationChevronExtent;
    return QRect(rect.right() + 1 - themeManager->metrics().table.cellPaddingX - extent,
                 rect.top() + (rect.height() - extent) / 2, extent, extent);
}

QIcon TableGroupHeader::chevronIcon(bool expanded, ThemeManager* themeManager)
{
    return themeManager->resolveIcon(
        QString::fromLatin1(expanded ? ThemeAssetIds::NavigationChevronDown
                                    : ThemeAssetIds::NavigationChevronRight),
        ThemeAssetVariant{QSize(), 1.0, 0, themeManager->colors().textMuted}).icon;
}

void TableGroupHeader::paint(QPainter& painter, const QRect& rect, const QString& text,
                             ThemeManager* themeManager, const TableGroupOptions& options,
                             const QWidget* widget)
{
    const auto& colors = themeManager->colors();
    const auto& metrics = themeManager->metrics().table;
    const auto font = captionFont(themeManager);
    painter.save();
    painter.fillRect(rect, colors.headerBackground);
    painter.setPen(colors.borderPrimary);
    painter.drawLine(rect.bottomLeft(), rect.bottomRight());
    painter.setPen(colors.textTitle);
    painter.setFont(font);
    const int controlWidth = std::max(options.checkable ? checkBoxRect(rect, themeManager, widget).width() : 0,
        options.collapsible ? themeManager->metrics().navigationChevronExtent : 0);
    const int inset = metrics.cellPaddingX + (controlWidth ? controlWidth + metrics.cellPaddingX : 0);
    const auto content = rect.adjusted(inset, metrics.cellPaddingY, -inset, -metrics.cellPaddingY);
    painter.drawText(content, Qt::AlignCenter,
        QFontMetrics(font).elidedText(text, Qt::ElideRight, std::max(0, content.width())));
    painter.restore();
}

void TableGroupHeader::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    paint(painter, rect(), text(), themeManager_, options_, checkBox_);
}

void TableGroupHeader::resizeEvent(QResizeEvent* event)
{
    QLabel::resizeEvent(event);
    layoutControls();
}

void TableGroupHeader::layoutControls()
{
    if (checkBox_) checkBox_->setGeometry(checkBoxRect(rect(), themeManager_, checkBox_));
    if (chevron_) chevron_->setGeometry(chevronRect(rect(), themeManager_));
}

void TableGroupHeader::applyTheme()
{
    setFont(captionFont(themeManager_));
    setFixedHeight(preferredHeight(themeManager_));
    if (chevron_) {
        const int extent = themeManager_->metrics().navigationChevronExtent;
        chevron_->setIconSize(QSize(extent, extent));
        chevron_->setIcon(chevronIcon(expanded_, themeManager_));
    }
    layoutControls();
    update();
}

}
