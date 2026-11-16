#include "TitleBarWidget.h"
#include "Core/ThemeAssetCatalog.h"
#include "Core/ThemeManager.h"
#include <QAction>
#include <QCoreApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPixmap>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QStyle>
#include <QStyleOptionMenuItem>

namespace VNX {

namespace {

QSize menuBarItemSize(const QMenuBar* menuBar, QAction* action)
{
    if (!menuBar || !action)
        return {};

    QStyleOptionMenuItem option;
    option.initFrom(menuBar);
    option.menuRect = menuBar->rect();
    option.state = QStyle::State_None;
    option.checkType = QStyleOptionMenuItem::NotCheckable;
    option.menuHasCheckableItems = false;
    option.menuItemType = action->menu() ? QStyleOptionMenuItem::SubMenu
                                         : QStyleOptionMenuItem::Normal;
    option.text = action->text();
    option.font = menuBar->font();
    return menuBar->style()->sizeFromContents(
        QStyle::CT_MenuBarItem, &option,
        option.fontMetrics.size(Qt::TextShowMnemonic, option.text), menuBar);
}

QSize menuBarContentSize(const QMenuBar* menuBar)
{
    if (!menuBar)
        return {};

    const auto actions = menuBar->actions();
    int itemHeight = menuBar->fontMetrics().height();
    int width = 0;
    for (QAction* action : actions) {
        const QSize size = menuBarItemSize(menuBar, action);
        itemHeight = qMax(itemHeight, size.height());
        width += size.width();
    }
    if (actions.isEmpty() || width <= 0) {
        QAction probe(QStringLiteral("File"));
        const QSize size = menuBarItemSize(menuBar, &probe);
        itemHeight = qMax(itemHeight, size.height());
        if (width <= 0)
            width = size.width();
    }

    const QStyle* style = menuBar->style();
    width += 2 * style->pixelMetric(QStyle::PM_MenuBarHMargin, nullptr, menuBar);
    width += 2 * style->pixelMetric(QStyle::PM_MenuBarPanelWidth, nullptr, menuBar);
    if (actions.size() > 1) {
        width += style->pixelMetric(QStyle::PM_MenuBarItemSpacing, nullptr, menuBar)
                 * (actions.size() - 1);
    }
    return {width, itemHeight};
}

}

TitleBarWidget::TitleBarWidget(QWidget* parent)
    : TitleBarBase(Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint, parent) {
    menuLayout_ = new QHBoxLayout;
    menuLayout_->setContentsMargins(0, 0, 0, 0);
    menuLayout_->setSpacing(0);

    logoLabel_ = new QLabel(this);
    logoLabel_->setObjectName("titleBarLogo");
    logoLabel_->setAccessibleName(QStringLiteral("Harken Studio"));
    logoLabel_->setAlignment(Qt::AlignCenter);

    menuLayout_->addWidget(logoLabel_);
    layout_->insertLayout(0, menuLayout_);

    versionLabel_ = new QLabel(this);
    versionLabel_->setObjectName("titleBarVersion");
    versionLabel_->setTextFormat(Qt::RichText);
    versionLabel_->setAlignment(Qt::AlignCenter);
    versionLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
    setProjectTitle({}, false);

    syncChromeMetrics();
}

void TitleBarWidget::embedMenuBar(QMenuBar* menuBar) {
    if (!menuBar || menuBar_) return;
    menuBar_ = menuBar;
    menuBarBaseStyleSheet_ = menuBar_->styleSheet();
    appliedMenuBarStyleSheet_ = menuBarBaseStyleSheet_;
    menuBar_->setParent(this);
    menuBar_->setObjectName("titleBarMenuBar");
    menuBar_->setNativeMenuBar(false);
    menuBar_->setDefaultUp(false);
    menuBar_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    menuLayout_->addWidget(menuBar_, 0, Qt::AlignVCenter);
    menuBar_->setVisible(true);
    menuBar_->installEventFilter(this);
    syncChromeMetrics();
}

void TitleBarWidget::insertControlWidget(QWidget* widget) {
    if (!widget) return;
    widget->setParent(this);
    layout_->insertWidget(layout_->indexOf(minButton()), widget, 0, Qt::AlignVCenter);
}

void TitleBarWidget::setProjectTitle(const QString& name, bool modified) {
    projectName_ = name;
    projectModified_ = modified;
    layoutVersionLabel();
}

bool TitleBarWidget::event(QEvent* event) {
    const bool handled = TitleBarBase::event(event);
    if (event->type() == QEvent::LayoutRequest)
        layoutVersionLabel();
    return handled;
}

bool TitleBarWidget::eventFilter(QObject* watched, QEvent* event) {
    if (watched == menuBar_ &&
        (event->type() == QEvent::ActionAdded ||
         event->type() == QEvent::ActionRemoved ||
         event->type() == QEvent::ActionChanged ||
         event->type() == QEvent::LayoutRequest ||
         event->type() == QEvent::LanguageChange ||
         event->type() == QEvent::FontChange ||
         event->type() == QEvent::ApplicationFontChange ||
         event->type() == QEvent::StyleChange)) {
        requestChromeMetricsSync();
    }
    return TitleBarBase::eventFilter(watched, event);
}

void TitleBarWidget::resizeEvent(QResizeEvent* event) {
    TitleBarBase::resizeEvent(event);
    layoutVersionLabel();
}

void TitleBarWidget::layoutVersionLabel() {
    if (!versionLabel_ || !layout_)
        return;
    layout_->activate();
    QRect available;
    for (int i = 0; i < layout_->count(); ++i) {
        if (QSpacerItem* spacer = layout_->itemAt(i)->spacerItem()) {
            available = spacer->geometry();
            break;
        }
    }

    QString version = QCoreApplication::applicationVersion();
    if (!projectName_.isEmpty())
        version.prepend(QStringLiteral(" - "));
    const QString suffix = projectModified_ && !projectName_.isEmpty()
                               ? QStringLiteral(" *") : QString();
    const QFontMetrics versionMetrics(versionLabel_->font());
    QFont projectFont = versionLabel_->font();
    projectFont.setItalic(projectModified_);
    const QFontMetrics projectMetrics(projectFont);
    const int fullProjectWidth = projectMetrics.horizontalAdvance(projectName_ + suffix);
    if (!projectName_.isEmpty()
        && available.width() < fullProjectWidth + versionMetrics.horizontalAdvance(version) + 2)
        version.clear();
    const int titleWidth = qMin(qMax(0, available.width()),
        versionMetrics.horizontalAdvance(version) + fullProjectWidth + 2);
    const int textWidth = qMax(0, titleWidth - 2);
    QString title;
    if (projectName_.isEmpty()) {
        title = versionMetrics.elidedText(version, Qt::ElideRight, textWidth).toHtmlEscaped();
    } else {
        const int suffixWidth = projectMetrics.horizontalAdvance(suffix);
        const int minimumProjectWidth = qMin(fullProjectWidth,
            projectMetrics.horizontalAdvance(QString(QChar(0x2026)) + suffix));
        const int projectWidth = textWidth - versionMetrics.horizontalAdvance(version);
        QString projectTitle = projectWidth < minimumProjectWidth
            ? projectMetrics.elidedText(projectName_ + suffix, Qt::ElideRight, projectWidth)
            : projectMetrics.elidedText(projectName_, Qt::ElideRight, projectWidth - suffixWidth) + suffix;
        projectTitle = projectTitle.toHtmlEscaped();
        if (projectModified_)
            projectTitle = QStringLiteral("<i>%1</i>").arg(projectTitle);
        title = projectTitle + version.toHtmlEscaped();
    }
    versionLabel_->setText(title);
    versionLabel_->setGeometry(qBound(available.left(), (width() - titleWidth) / 2,
                                     available.x() + available.width() - titleWidth),
                              0, titleWidth, height());
    versionLabel_->setVisible(titleWidth > 0);
}

int TitleBarWidget::syncContentMetrics() {
    const int textHeight = menuBar_ ? menuBar_->fontMetrics().height()
                                    : fontMetrics().height();
    int itemHeight = textHeight;
    int contentWidth = 0;
    if (menuBar_) {
        const int targetHeight = qMax(ThemeManager::instance().metrics().chrome.titleBarHeight, textHeight);
        const int slack = targetHeight - textHeight;
        const QString itemPadding = QStringLiteral(
            "#titleBarMenuBar::item { padding-top: %1px; padding-bottom: %2px; }")
            .arg(slack / 2)
            .arg(slack - slack / 2);
        const QString currentStyleSheet = menuBar_->styleSheet();
        if (currentStyleSheet != appliedMenuBarStyleSheet_)
            menuBarBaseStyleSheet_ = currentStyleSheet;
        QString styleSheet = menuBarBaseStyleSheet_;
        if (!styleSheet.isEmpty() && !styleSheet.endsWith('\n'))
            styleSheet.append('\n');
        styleSheet.append(itemPadding);
        appliedMenuBarStyleSheet_ = styleSheet;
        if (currentStyleSheet != styleSheet)
            menuBar_->setStyleSheet(styleSheet);
        menuBar_->ensurePolished();
        const QSize content = menuBarContentSize(menuBar_);
        itemHeight = qMax(textHeight, content.height());
        contentWidth = content.width();
    }
    if (menuBar_)
        menuBar_->setMinimumWidth(contentWidth);
    return itemHeight;
}

void TitleBarWidget::updateContentGeometry() {
    const int barHeight = height();
    const int logoHeight = qRound(12 * ThemeManager::instance().snapshot()->uiScale);
    const auto logo = ThemeManager::instance().resolveImage(
        QString::fromLatin1(ThemeAssetIds::AppLogo));
    if (logo.resource) {
        QPixmap pixmap = QPixmap::fromImage(logo.resource->image).scaledToHeight(
            qRound(logoHeight * devicePixelRatioF()), Qt::SmoothTransformation);
        pixmap.setDevicePixelRatio(devicePixelRatioF());
        logoLabel_->setFixedSize(qRound(pixmap.deviceIndependentSize().width()) + barHeight - logoHeight,
                                barHeight);
        logoLabel_->setPixmap(pixmap);
    }
    if (menuBar_) {
        menuBar_->setFixedHeight(barHeight);
    }
    layoutVersionLabel();
}

}
