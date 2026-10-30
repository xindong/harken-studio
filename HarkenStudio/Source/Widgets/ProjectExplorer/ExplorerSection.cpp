#include "ExplorerSection.h"

#include "Core/ThemeAssetCatalog.h"
#include "Core/ThemeManager.h"

#include <QHBoxLayout>
#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QToolButton>
#include <QVBoxLayout>

namespace VNX {

namespace {

class ExplorerSectionToggle final : public QToolButton
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
        const int padding = 4;
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

ExplorerSection::ExplorerSection(const QString& title, QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    header_ = new QWidget(this);
    header_->setProperty("explorerRole", "sectionHeader");
    header_->setAttribute(Qt::WA_StyledBackground);
    header_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(header_, &QWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        emit headerContextMenuRequested(header_->mapToGlobal(position));
    });
    auto* headerLayout = new QHBoxLayout(header_);
    headerLayout->setContentsMargins(0, 1, 0, 1);
    headerLayout->setSpacing(0);
    toggle_ = new ExplorerSectionToggle(header_);
    toggle_->setProperty("explorerRole", "sectionToggle");
    toggle_->setObjectName(QStringLiteral("explorerSectionToggle"));
    toggle_->setText(title);
    toggle_->setAccessibleName(title);
    toggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toggle_->setCheckable(true);
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

bool ExplorerSection::isExpanded() const
{
    return toggle_->isChecked();
}

void ExplorerSection::setExpanded(bool expanded)
{
    toggle_->setChecked(expanded);
}

void ExplorerSection::updateHeader()
{
    auto& theme = ThemeManager::instance();
    const int height = qMax(theme.metrics().sectionHeaderHeight,
                           toggle_->fontMetrics().height() + 8);
    header_->setFixedHeight(height);
    const int extent = theme.metrics().navigationChevronExtent;
    toggle_->setIconSize(QSize(extent, extent));
    toggle_->setIcon(theme.resolveIcon(QString::fromLatin1(isExpanded()
        ? ThemeAssetIds::NavigationChevronDown : ThemeAssetIds::NavigationChevronRight)).icon);
    setMinimumHeight(isExpanded() ? height + 32 : height);
    setMaximumHeight(isExpanded() ? QWIDGETSIZE_MAX : height);
    setSizePolicy(QSizePolicy::Expanding, isExpanded() ? QSizePolicy::Expanding : QSizePolicy::Fixed);
    updateGeometry();
}

}
