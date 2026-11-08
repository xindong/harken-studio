#include "PaneSection.h"

#include "Core/ThemeAssetCatalog.h"
#include "Core/ThemeManager.h"

#include <QHBoxLayout>
#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QToolButton>
#include <QVBoxLayout>

namespace VNX {

namespace {

class PaneSectionToggle final : public QToolButton
{
public:
    using QToolButton::QToolButton;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QStylePainter painter(this);
        QStyleOptionToolButton option;
        initStyleOption(&option);
        option.icon = QIcon();
        option.text.clear();
        painter.drawComplexControl(QStyle::CC_ToolButton, option);

        const int extent = iconSize().width();
        const int padding = ThemeManager::instance().metrics().spacingXSmall;
        icon().paint(&painter, QRect(padding, (height() - extent) / 2, extent, extent),
                     Qt::AlignCenter, isEnabled() ? QIcon::Normal : QIcon::Disabled,
                     isChecked() ? QIcon::On : QIcon::Off);
        const int textInset = extent + 2 * padding;
        const QRect textRect = rect().adjusted(textInset, 0, -textInset, 0);
        painter.drawItemText(textRect, Qt::AlignCenter | Qt::TextSingleLine,
                             option.palette, isEnabled(),
                             fontMetrics().elidedText(text(), Qt::ElideRight,
                                                      qMax(0, textRect.width())),
                             QPalette::ButtonText);
    }
};

}

PaneSection::PaneSection(const QString& title, const QString& settingsId, QWidget* parent)
    : QWidget(parent)
    , settingsId_(settingsId)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    header_ = new QWidget(this);
    header_->setProperty("paneRole", "sectionHeader");
    header_->setAttribute(Qt::WA_StyledBackground);
    header_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(header_, &QWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        emit headerContextMenuRequested(header_->mapToGlobal(position));
    });
    auto* headerLayout = new QHBoxLayout(header_);
    headerLayout->setSpacing(0);
    toggle_ = new PaneSectionToggle(header_);
    toggle_->setProperty("paneRole", "sectionToggle");
    toggle_->setObjectName(QStringLiteral("paneSectionToggle"));
    toggle_->setText(title);
    toggle_->setAccessibleName(title);
    toggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toggle_->setCheckable(true);
    toggle_->setCursor(Qt::PointingHandCursor);
    toggle_->setChecked(true);
    toggle_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    headerLayout->addWidget(toggle_, 1);
    layout->addWidget(header_);
    content_ = new QWidget(this);
    content_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    layout->addWidget(content_, 1);
    connect(toggle_, &QToolButton::toggled, this, [this](bool expanded) {
        content_->setVisible(expanded);
        updateHeader();
        emit expandedChanged(expanded);
    });
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, [this]() { updateHeader(); });
    connect(&ThemeManager::instance(), &ThemeManager::appearanceChanged, this, [this]() {
        toggle_->setIcon(ThemeManager::instance().resolveIcon(QString::fromLatin1(isExpanded()
            ? ThemeAssetIds::NavigationChevronDown : ThemeAssetIds::NavigationChevronRight)).icon);
    });
    updateHeader();
}

bool PaneSection::isExpanded() const
{
    return toggle_->isChecked();
}

void PaneSection::setExpanded(bool expanded)
{
    toggle_->setChecked(expanded);
}

void PaneSection::updateHeader()
{
    auto& theme = ThemeManager::instance();
    const auto& metrics = theme.metrics();
    const auto& layout = metrics.layout;
    const int height = qMax(layout.sectionHeaderHeight,
                            toggle_->fontMetrics().height() + layout.rowInset * 2 + metrics.borderWidth);
    header_->setFixedHeight(height);
    header_->layout()->setContentsMargins(0, 0, 0, metrics.borderWidth);
    const int extent = metrics.iconExtent;
    toggle_->setIconSize(QSize(extent, extent));
    toggle_->setIcon(theme.resolveIcon(QString::fromLatin1(isExpanded()
        ? ThemeAssetIds::NavigationChevronDown : ThemeAssetIds::NavigationChevronRight)).icon);
    setMinimumHeight(isExpanded() ? height + metrics.control.largeHeight : height);
    setMaximumHeight(isExpanded() ? QWIDGETSIZE_MAX : height);
    setSizePolicy(QSizePolicy::Expanding, isExpanded() ? QSizePolicy::Expanding : QSizePolicy::Fixed);
    updateGeometry();
}

}
