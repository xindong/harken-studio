#include "QtAdsThemeAdapter.h"

#include "ThemeAssetCatalog.h"
#include "ThemeAssetResolver.h"

#include <DockManager.h>
#include <IconProvider.h>
#include <QtGlobal>

#include <utility>

namespace VNX {

namespace {

QString colorValue(const QColor& color)
{
    return color.name(QColor::HexArgb).toUpper();
}

QString rgba(const QColor& color, qreal opacity)
{
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(QString::number(color.alphaF() * opacity, 'g', 6));
}

}

QtAdsThemeAdapter::QtAdsThemeAdapter(ThemeSnapshotPtr snapshot)
    : snapshot_(std::move(snapshot))
{
}

QString QtAdsThemeAdapter::styleSheetMarker()
{
    return QStringLiteral("/* @harken-qtads-theme@ */");
}

QString QtAdsThemeAdapter::styleSheet() const
{
    const auto& colors = snapshot_->ui.colors;
    const auto& style = snapshot_->thirdParty.qtAds;
    const auto& metrics = style.metrics;

    QString result = QStringLiteral(R"(
/* ==========================================================================
   Qt Advanced Docking System (QtADS) — Dark Theme
   Single styling authority: this QSS replaces CDockManager's built-in
   focus_highlighting.css.
   ========================================================================== */

/* --- Container & Dock Widget -------------------------------------------- */

ads--CDockContainerWidget {
    background-color: @CHROME_BACKGROUND@;
}

ads--CDockWidget {
    background-color: @BG_PRIMARY@;
    border: none;
}

QScrollArea#dockWidgetScrollArea {
    padding: 0px;
    border: none;
}

/* --- Dock Area ---------------------------------------------------------- */

ads--CDockAreaWidget {
    background-color: @BG_PRIMARY@;
    border: @BORDER_WIDTH@px solid @VIEW_BORDER@;
}

/* --- Title Bar ---------------------------------------------------------- */

ads--CDockAreaTitleBar {
    background-color: @BG_SECONDARY@;
    border-bottom: @BORDER_WIDTH@px solid @BORDER_PRIMARY@;
    min-height: @TITLE_BAR_MIN_HEIGHT@px;
    padding: 0;
}

ads--CDockAreaTitleBar QPushButton {
    border: none;
    padding: @TITLE_BUTTON_PADDING@px;
}

ads--CTitleBarButton {
    padding: 0px 0px;
    qproperty-iconSize: @TITLE_ICON_SIZE@px @TITLE_ICON_SIZE@px;
}

#dockAreaCloseButton,
#startupLoadingCloseButton,
#dockPinButton {
    border: none;
    padding: @CLOSE_BUTTON_PADDING@px;
    max-width: @CLOSE_BUTTON_EXTENT@px;
    max-height: @CLOSE_BUTTON_EXTENT@px;
}

#dockPinButton {
    qproperty-iconSize: 20px 20px;
}

#tabsMenuButton {
    qproperty-iconSize: @CHEVRON_EXTENT@px @CHEVRON_EXTENT@px;
    min-width: @CHEVRON_EXTENT@px;
    min-height: @CHEVRON_EXTENT@px;
}

#tabsMenuButton::menu-indicator {
    image: none;
}

/* --- Dock Widget Tabs --------------------------------------------------- */

ads--CDockAreaTabBar,
#tabsContainerWidget {
    background-color: @BG_SECONDARY@;
    border: none;
    min-height: @TAB_BAR_MIN_HEIGHT@px;
}

ads--CDockWidgetTab {
    background-color: @BG_PRIMARY@;
    border: none;
    border-top: @TAB_ACCENT_BORDER_WIDTH@px solid @ACTIVE_TAB_ACCENT@;
    margin-right: @TAB_GAP_WIDTH@px;
    padding: @TAB_PADDING_TOP@px @TAB_PADDING_HORIZONTAL@px @TAB_PADDING_BOTTOM@px @TAB_PADDING_HORIZONTAL@px;
    min-height: @TAB_MIN_HEIGHT@px;
    font-size: @TAB_FONT_SIZE@px;
    qproperty-iconSize: @TAB_ICON_SIZE@px @TAB_ICON_SIZE@px;
}

ads--CDockWidgetTab:hover {
    background-color: @BG_TERTIARY@;
}

ads--CDockWidgetTab[focused="true"] {
    background-color: @BG_SELECTED@;
    border-top: @TAB_ACCENT_BORDER_WIDTH@px solid @ACCENT@;
}

/* --- Tab Label (CElidingLabel) — explicit color per state --------------- */

ads--CDockWidgetTab QLabel {
    color: @TEXT_PRIMARY@;
    background-color: transparent;
}

ads--CDockWidgetTab[focused="true"] QLabel {
    color: @ACCENT@;
}

/* --- Tab Close Button --------------------------------------------------- */

#tabCloseButton {
    margin-top: @TAB_CLOSE_MARGIN_TOP@px;
    border: none;
    padding: 0px @TAB_CLOSE_PADDING_RIGHT@px;
    qproperty-iconSize: @TITLE_ICON_SIZE@px @TITLE_ICON_SIZE@px;
}

#dockAreaCloseButton:hover:enabled,
#startupLoadingCloseButton:hover:enabled,
#tabCloseButton:hover:enabled {
    background-color: @CLOSE_BUTTON_HOVER@;
}

#dockAreaCloseButton:pressed:enabled,
#startupLoadingCloseButton:pressed:enabled,
#tabCloseButton:pressed:enabled {
    background-color: @CLOSE_BUTTON_PRESSED@;
}

/* --- Floating Container ------------------------------------------------- */

ads--CFloatingDockContainer {
    background-color: @CHROME_BACKGROUND@;
    border: @BORDER_WIDTH@px solid @BORDER_PRIMARY@;
}

/* --- Drop Overlay ------------------------------------------------------- */

ads--CDockOverlay {
    background-color: @DROP_OVERLAY@;
    border: @OVERLAY_BORDER_WIDTH@px solid @ACCENT@;
}

/* --- Splitter ----------------------------------------------------------- */

ads--CDockSplitter::handle {
    background-color: @CHROME_BACKGROUND@;
    height: @SPLITTER_HANDLE_EXTENT@px;
    width: @SPLITTER_HANDLE_EXTENT@px;
}

ads--CDockSplitter::handle:hover {
    background-color: @ACCENT@;
}

/* --- Auto-Hide Tabs ----------------------------------------------------- */

ads--CAutoHideTab {
    qproperty-iconSize: @AUTO_HIDE_TAB_ICON_SIZE@px @AUTO_HIDE_TAB_ICON_SIZE@px;
    background: none;
    border: none;
    padding-left: @AUTO_HIDE_TAB_PADDING_LEFT@px;
    padding-right: @AUTO_HIDE_TAB_PADDING_RIGHT@px;
    text-align: center;
    min-height: @AUTO_HIDE_TAB_MIN_HEIGHT@px;
    color: @INACTIVE_TAB_TEXT@;
}

ads--CAutoHideTab:hover {
    color: @ACCENT@;
}

ads--CAutoHideSideBar {
    background-color: @CHROME_BACKGROUND@;
    border: none;
    qproperty-spacing: @AUTO_HIDE_SIDE_BAR_SPACING@;
}

#sideTabsContainerWidget {
    background: transparent;
}

ads--CAutoHideDockContainer {
    background-color: @BG_PRIMARY@;
}

ads--CAutoHideDockContainer ads--CDockAreaTitleBar {
    background-color: @BG_SECONDARY@;
    border-bottom: @AUTO_HIDE_TITLE_BORDER_WIDTH@px solid @ACCENT@;
    padding: 0px;
}

#autoHideTitleLabel {
    padding-left: @AUTO_HIDE_TITLE_PADDING_LEFT@px;
    color: @ACCENT@;
}

/* --- Resize Handle ------------------------------------------------------ */

ads--CResizeHandle {
    background-color: @CHROME_BACKGROUND@;
}
)");

    result.replace(QStringLiteral("@CHROME_BACKGROUND@"), colorValue(style.chromeBackground));
    result.replace(QStringLiteral("@BG_PRIMARY@"), colorValue(colors.bgPrimary));
    result.replace(QStringLiteral("@VIEW_BORDER@"), colorValue(colors.viewBorder));
    result.replace(QStringLiteral("@BG_SECONDARY@"), colorValue(colors.bgSecondary));
    result.replace(QStringLiteral("@BORDER_PRIMARY@"), colorValue(colors.borderPrimary));
    result.replace(QStringLiteral("@BG_TERTIARY@"), colorValue(colors.bgTertiary));
    result.replace(QStringLiteral("@ACTIVE_TAB_ACCENT@"), rgba(colors.accent, style.activeTabAccentOpacity));
    result.replace(QStringLiteral("@BG_SELECTED@"), colorValue(colors.bgSelected));
    result.replace(QStringLiteral("@ACCENT@"), colorValue(colors.accent));
    result.replace(QStringLiteral("@INACTIVE_TAB_TEXT@"), colorValue(style.inactiveTabText));
    result.replace(QStringLiteral("@TEXT_PRIMARY@"), colorValue(colors.textPrimary));
    result.replace(QStringLiteral("@CLOSE_BUTTON_HOVER@"), colorValue(style.closeButtonHover));
    result.replace(QStringLiteral("@CLOSE_BUTTON_PRESSED@"), colorValue(style.closeButtonPressed));
    result.replace(QStringLiteral("@DROP_OVERLAY@"), rgba(colors.accent, style.dropOverlayOpacity));

    result.replace(QStringLiteral("@BORDER_WIDTH@"), QString::number(metrics.borderWidth));
    result.replace(QStringLiteral("@TITLE_BAR_MIN_HEIGHT@"), QString::number(metrics.titleBarMinimumHeight));
    result.replace(QStringLiteral("@TITLE_BUTTON_PADDING@"), QString::number(metrics.titleBarButtonPadding));
    result.replace(QStringLiteral("@TITLE_ICON_SIZE@"), QString::number(metrics.titleBarIconSize));
    result.replace(QStringLiteral("@CHEVRON_EXTENT@"),
                   QString::number(snapshot_->ui.metrics.navigationChevronExtent));
    result.replace(QStringLiteral("@CLOSE_BUTTON_PADDING@"), QString::number(metrics.closeButtonPadding));
    result.replace(QStringLiteral("@CLOSE_BUTTON_EXTENT@"), QString::number(metrics.closeButtonExtent));
    result.replace(QStringLiteral("@TAB_BAR_MIN_HEIGHT@"), QString::number(metrics.tabBarMinimumHeight));
    result.replace(QStringLiteral("@TAB_ACCENT_BORDER_WIDTH@"), QString::number(metrics.tabAccentBorderWidth));
    result.replace(QStringLiteral("@TAB_GAP_WIDTH@"), QString::number(metrics.tabGapWidth));
    result.replace(QStringLiteral("@TAB_PADDING_TOP@"), QString::number(metrics.tabPaddingTop));
    result.replace(QStringLiteral("@TAB_PADDING_HORIZONTAL@"), QString::number(metrics.tabPaddingHorizontal));
    result.replace(QStringLiteral("@TAB_PADDING_BOTTOM@"), QString::number(metrics.tabPaddingBottom));
    result.replace(QStringLiteral("@TAB_MIN_HEIGHT@"), QString::number(metrics.tabMinimumHeight));
    result.replace(QStringLiteral("@TAB_FONT_SIZE@"), QString::number(metrics.tabFontSize));
    result.replace(QStringLiteral("@TAB_ICON_SIZE@"), QString::number(metrics.tabIconSize));
    result.replace(QStringLiteral("@TAB_CLOSE_MARGIN_TOP@"), QString::number(metrics.tabCloseMarginTop));
    result.replace(QStringLiteral("@TAB_CLOSE_PADDING_RIGHT@"), QString::number(metrics.tabClosePaddingRight));
    result.replace(QStringLiteral("@OVERLAY_BORDER_WIDTH@"), QString::number(metrics.overlayBorderWidth));
    result.replace(QStringLiteral("@SPLITTER_HANDLE_EXTENT@"), QString::number(metrics.splitterHandleExtent));
    result.replace(QStringLiteral("@AUTO_HIDE_TAB_ICON_SIZE@"), QString::number(metrics.autoHideTabIconSize));
    result.replace(QStringLiteral("@AUTO_HIDE_TAB_PADDING_LEFT@"), QString::number(metrics.autoHideTabPaddingLeft));
    result.replace(QStringLiteral("@AUTO_HIDE_TAB_PADDING_RIGHT@"), QString::number(metrics.autoHideTabPaddingRight));
    result.replace(QStringLiteral("@AUTO_HIDE_TAB_MIN_HEIGHT@"), QString::number(metrics.autoHideTabMinimumHeight));
    result.replace(QStringLiteral("@AUTO_HIDE_SIDE_BAR_SPACING@"), QString::number(metrics.autoHideSideBarSpacing));
    result.replace(QStringLiteral("@AUTO_HIDE_TITLE_BORDER_WIDTH@"), QString::number(metrics.autoHideTitleBorderWidth));
    result.replace(QStringLiteral("@AUTO_HIDE_TITLE_PADDING_LEFT@"), QString::number(metrics.autoHideTitlePaddingLeft));
    return result;
}

QString QtAdsThemeAdapter::composeApplicationStyleSheet(QString applicationStyleSheet) const
{
    const QString marker = styleSheetMarker();
    Q_ASSERT_X(applicationStyleSheet.count(marker) == 1,
               "QtAdsThemeAdapter::composeApplicationStyleSheet",
               "Application stylesheet must contain exactly one QtADS theme marker");
    applicationStyleSheet.replace(marker, styleSheet());
    return applicationStyleSheet;
}

void QtAdsThemeAdapter::applyIcons(ThemeAssetResolver& resolver,
                                   const ThemeSnapshotPtr& snapshot)
{
    auto& iconProvider = ads::CDockManager::iconProvider();
    iconProvider.registerCustomIcon(
        ads::DockAreaCloseIcon,
        resolver.resolveIcon(
            snapshot, QString::fromLatin1(ThemeAssetIds::DockClose)).icon);
    iconProvider.registerCustomIcon(
        ads::TabCloseIcon,
        resolver.resolveIcon(
            snapshot, QString::fromLatin1(ThemeAssetIds::DockClose)).icon);
    iconProvider.registerCustomIcon(
        ads::DockAreaMenuIcon,
        resolver.resolveIcon(
            snapshot, QString::fromLatin1(ThemeAssetIds::NavigationChevronDown)).icon);
}

ads::CDockManager* QtAdsThemeAdapter::createDockManager(QWidget* parent) const
{
    auto* dockManager = new ads::CDockManager(parent);
    dockManager->setStyleSheet(QStringLiteral(""));
    return dockManager;
}

}
