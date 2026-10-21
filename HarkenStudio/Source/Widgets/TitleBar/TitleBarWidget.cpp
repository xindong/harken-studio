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
#include <QMetaObject>
#include <QPixmap>
#include <QResizeEvent>
#include <QScreen>
#include <QShowEvent>
#include <QSizePolicy>
#include <QStyle>
#include <QStyleOptionMenuItem>
#include <QWindow>

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

TitleBarWidget::TitleBarWidget(QWidget* parent) : QWidget(parent) {
    setObjectName("TitleBarWidget");

    layout_ = new QHBoxLayout(this);
    layout_->setContentsMargins(8, 0, 0, 0);
    layout_->setSpacing(6);

    logoLabel_ = new QLabel(this);
    logoLabel_->setObjectName("titleBarLogo");
    logoLabel_->setAccessibleName(QStringLiteral("Harken Studio"));
    const QSize logoSize(150, 12);
    logoLabel_->setFixedSize(logoSize);
    logoLabel_->setAlignment(Qt::AlignCenter);
    const auto logo = ThemeManager::instance().resolveImage(
        QString::fromLatin1(ThemeAssetIds::AppLogo));
    if (logo.resource) {
        QPixmap pixmap = QPixmap::fromImage(logo.resource->image).scaled(
            logoSize * devicePixelRatioF(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        pixmap.setDevicePixelRatio(devicePixelRatioF());
        logoLabel_->setPixmap(pixmap);
    }

    layout_->addWidget(logoLabel_);
    layout_->addStretch();

    auto makeWinBtn = [this](const QString& name, TitleBarButtonKind kind, const QString& tip) {
        auto* btn = new TitleBarButton(kind, this);
        btn->setObjectName(name);
        btn->setToolTip(tip);
        return btn;
    };

    btnMin_   = makeWinBtn("titleBarMin", TitleBarButtonKind::Minimize, tr("Minimize"));
    btnMax_   = makeWinBtn("titleBarMax", TitleBarButtonKind::Maximize, tr("Maximize"));
    btnClose_ = makeWinBtn("titleBarClose", TitleBarButtonKind::Close, tr("Close"));

    layout_->addWidget(btnMin_);
    layout_->addWidget(btnMax_);
    layout_->addWidget(btnClose_);

    versionLabel_ = new QLabel(this);
    versionLabel_->setObjectName("titleBarVersion");
    versionLabel_->setTextFormat(Qt::RichText);
    versionLabel_->setAlignment(Qt::AlignCenter);
    versionLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
    setProjectTitle({}, false);

    trackWindow(parentWidget());
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
    layout_->insertWidget(1, menuBar_, 0, Qt::AlignVCenter);
    menuBar_->setVisible(true);
    menuBar_->installEventFilter(this);
    syncChromeMetrics();
}

void TitleBarWidget::insertControlWidget(QWidget* widget) {
    if (!widget) return;
    widget->setParent(this);
    layout_->insertWidget(layout_->indexOf(btnMin_), widget, 0, Qt::AlignVCenter);
}

void TitleBarWidget::updateMaxButton(bool maximized) {
    btnMax_->setButtonKind(maximized ? TitleBarButtonKind::Restore : TitleBarButtonKind::Maximize);
    btnMax_->setToolTip(maximized ? tr("Restore Down") : tr("Maximize"));
}

void TitleBarWidget::setProjectTitle(const QString& name, bool modified) {
    projectName_ = name;
    projectModified_ = modified;
    layoutVersionLabel();
}

bool TitleBarWidget::event(QEvent* event) {
    const bool handled = QWidget::event(event);
    if (event->type() == QEvent::LayoutRequest)
        layoutVersionLabel();
    return handled;
}

bool TitleBarWidget::eventFilter(QObject* watched, QEvent* event) {
    if (watched == trackedWindow_.data() &&
        (event->type() == QEvent::WindowStateChange || event->type() == QEvent::Show)) {
        syncMaxButton();
    }
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
    return QWidget::eventFilter(watched, event);
}

void TitleBarWidget::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange ||
        event->type() == QEvent::ApplicationFontChange ||
        event->type() == QEvent::StyleChange) {
        requestChromeMetricsSync();
    }
}

void TitleBarWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    trackWindow(window());
    syncMaxButton();
    syncChromeMetrics();
}

void TitleBarWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
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

    QString version = QStringLiteral("Harken Studio %1")
                          .arg(QCoreApplication::applicationVersion());
    if (!projectName_.isEmpty())
        version.prepend(QStringLiteral(" - "));
    const QString suffix = projectModified_ && !projectName_.isEmpty()
                               ? QStringLiteral(" *") : QString();
    const QFontMetrics versionMetrics(versionLabel_->font());
    QFont projectFont = versionLabel_->font();
    projectFont.setItalic(projectModified_);
    const QFontMetrics projectMetrics(projectFont);
    const int versionWidth = versionMetrics.horizontalAdvance(version);
    const int titleWidth = qMin(qMax(0, available.width()),
        versionWidth + projectMetrics.horizontalAdvance(projectName_ + suffix) + 2);
    const int textWidth = qMax(0, titleWidth - 2);
    QString title;
    if (projectName_.isEmpty()) {
        title = versionMetrics.elidedText(version, Qt::ElideRight, textWidth).toHtmlEscaped();
    } else {
        const int suffixWidth = projectMetrics.horizontalAdvance(suffix);
        const int minimumProjectWidth = qMin(projectMetrics.horizontalAdvance(projectName_ + suffix),
            projectMetrics.horizontalAdvance(QString(QChar(0x2026)) + suffix));
        if (textWidth < versionWidth + minimumProjectWidth)
            version.clear();
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

void TitleBarWidget::trackWindow(QWidget* window) {
    QWidget* topLevel = window ? window->window() : nullptr;
    if (!topLevel)
        return;
    if (trackedWindow_ != topLevel) {
        if (trackedWindow_)
            trackedWindow_->removeEventFilter(this);
        trackedWindow_ = topLevel;
        trackedWindow_->installEventFilter(this);
    }
    if (QWindow* handle = trackedWindow_->windowHandle()) {
        connect(handle, &QWindow::screenChanged, this, &TitleBarWidget::trackScreen,
                Qt::UniqueConnection);
        trackScreen(handle->screen());
    }
    syncMaxButton();
}

void TitleBarWidget::trackScreen(QScreen* screen) {
    if (trackedScreen_ == screen)
        return;
    if (trackedScreen_)
        trackedScreen_->disconnect(this);
    trackedScreen_ = screen;
    if (screen) {
        connect(screen, &QScreen::logicalDotsPerInchChanged, this,
                &TitleBarWidget::syncChromeMetrics, Qt::UniqueConnection);
    }
    syncChromeMetrics();
}

void TitleBarWidget::syncMaxButton() {
    if (trackedWindow_)
        updateMaxButton(trackedWindow_->isMaximized());
}

void TitleBarWidget::requestChromeMetricsSync()
{
    if (metricsSyncQueued_)
        return;
    metricsSyncQueued_ = true;
    QMetaObject::invokeMethod(this, &TitleBarWidget::syncChromeMetrics,
                              Qt::QueuedConnection);
}

void TitleBarWidget::syncChromeMetrics() {
    metricsSyncQueued_ = false;
    const int textHeight = menuBar_ ? menuBar_->fontMetrics().height()
                                    : fontMetrics().height();
    int itemHeight = textHeight;
    int contentWidth = 0;
    if (menuBar_) {
        const int targetHeight = qMax(36, textHeight);
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
    const int barHeight = qMax(36, itemHeight);
    setFixedHeight(barHeight);
    if (menuBar_) {
        menuBar_->setMinimumWidth(contentWidth);
        menuBar_->setFixedHeight(barHeight);
    }
    if (btnMin_)
        btnMin_->setFixedSize(46, barHeight);
    if (btnMax_)
        btnMax_->setFixedSize(46, barHeight);
    if (btnClose_)
        btnClose_->setFixedSize(46, barHeight);
    layoutVersionLabel();
}

}
