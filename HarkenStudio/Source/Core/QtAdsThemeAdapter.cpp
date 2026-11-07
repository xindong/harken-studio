#include "QtAdsThemeAdapter.h"

#include "ThemeAssetCatalog.h"
#include "FontManager.h"
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
    const auto& metrics = snapshot_->ui.metrics;

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
    border: @BORDER_WIDTH@px solid @BORDER_PRIMARY@;
}

/* --- Title Bar ---------------------------------------------------------- */

ads--CDockAreaTitleBar {
    background-color: @BG_SECONDARY@;
    border-bottom: @BORDER_WIDTH@px solid @BORDER_PRIMARY@;
    min-height: @LARGE_HEIGHT@px;
    padding: 0;
}

ads--CDockAreaTitleBar QPushButton {
    border: none;
    padding: @SPACING_COMPACT@px;
}

ads--CTitleBarButton {
    padding: 0px 0px;
    qproperty-iconSize: @ICON_EXTENT@px @ICON_EXTENT@px;
}

#dockAreaCloseButton,
#startupLoadingCloseButton,
#dockPinButton {
    border: none;
    padding: @BORDER_WIDTH@px;
    max-width: @CLOSE_BUTTON_EXTENT@px;
    max-height: @CLOSE_BUTTON_EXTENT@px;
}

#dockPinButton {
    qproperty-iconSize: @ICON_EXTENT@px @ICON_EXTENT@px;
}

#tabsMenuButton {
    qproperty-iconSize: @ICON_EXTENT@px @ICON_EXTENT@px;
    min-width: @ICON_EXTENT@px;
    min-height: @ICON_EXTENT@px;
}

#tabsMenuButton::menu-indicator {
    image: none;
}

/* --- Dock Widget Tabs --------------------------------------------------- */

ads--CDockAreaTabBar,
#tabsContainerWidget {
    background-color: @BG_SECONDARY@;
    border: none;
    min-height: @LARGE_HEIGHT@px;
}

ads--CDockWidgetTab {
    background-color: @BG_PRIMARY@;
    border: none;
    border-top: @ACCENT_BORDER_WIDTH@px solid @ACTIVE_TAB_ACCENT@;
    margin-right: @SPACING_COMPACT@px;
    padding: @SPACING_COMPACT@px @SPACING_XSMALL@px @SPACING_XSMALL@px @SPACING_XSMALL@px;
    min-height: @CONTROL_HEIGHT@px;
    qproperty-iconSize: @SMALL_ICON_EXTENT@px @SMALL_ICON_EXTENT@px;
}

ads--CDockWidgetTab:hover {
    background-color: @BG_TERTIARY@;
}

ads--CDockWidgetTab[focused="true"] {
    background-color: @BG_SELECTED@;
    border-top: @ACCENT_BORDER_WIDTH@px solid @ACCENT@;
}

/* --- Tab Label (CElidingLabel) — explicit color per state --------------- */

ads--CDockWidgetTab QLabel {
    @TAB_FONT@
    color: @TEXT_PRIMARY@;
    background-color: transparent;
}

ads--CDockWidgetTab[focused="true"] QLabel {
    color: @ACCENT@;
}

/* --- Tab Close Button --------------------------------------------------- */

#tabCloseButton {
    margin-top: @SPACING_COMPACT@px;
    border: none;
    padding: 0px -@SPACING_COMPACT@px;
    qproperty-iconSize: @ICON_EXTENT@px @ICON_EXTENT@px;
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
    border: @ACCENT_BORDER_WIDTH@px solid @ACCENT@;
}

/* --- Splitter ----------------------------------------------------------- */

ads--CDockSplitter::handle {
    background-color: @CHROME_BACKGROUND@;
    height: @SPLITTER_EXTENT@px;
    width: @SPLITTER_EXTENT@px;
}

ads--CDockSplitter::handle:hover {
    background-color: @ACCENT@;
}

/* --- Auto-Hide Tabs ----------------------------------------------------- */

ads--CAutoHideTab {
    qproperty-iconSize: @SMALL_ICON_EXTENT@px @SMALL_ICON_EXTENT@px;
    background: none;
    border: none;
    padding-left: @SPACING_COMPACT@px;
    padding-right: 0px;
    text-align: center;
    min-height: @COMPACT_HEIGHT@px;
    color: @INACTIVE_TAB_TEXT@;
}

ads--CAutoHideTab:hover {
    color: @ACCENT@;
}

ads--CAutoHideSideBar {
    background-color: @CHROME_BACKGROUND@;
    border: none;
    qproperty-spacing: @SPACING_MEDIUM@;
}

#sideTabsContainerWidget {
    background: transparent;
}

ads--CAutoHideDockContainer {
    background-color: @BG_PRIMARY@;
}

ads--CAutoHideDockContainer ads--CDockAreaTitleBar {
    background-color: @BG_SECONDARY@;
    border-bottom: @ACCENT_BORDER_WIDTH@px solid @ACCENT@;
    padding: 0px;
}

#autoHideTitleLabel {
    padding-left: @SPACING_XSMALL@px;
    color: @ACCENT@;
}

/* --- Resize Handle ------------------------------------------------------ */

ads--CResizeHandle {
    background-color: @CHROME_BACKGROUND@;
}
)");

    result.replace(QStringLiteral("@CHROME_BACKGROUND@"), colorValue(style.chromeBackground));
    result.replace(QStringLiteral("@BG_PRIMARY@"), colorValue(colors.bgPrimary));
    result.replace(QStringLiteral("@BG_SECONDARY@"), colorValue(colors.bgSecondary));
    result.replace(QStringLiteral("@BORDER_PRIMARY@"), colorValue(colors.borderPrimary));
    result.replace(QStringLiteral("@BG_TERTIARY@"), colorValue(colors.bgTertiary));
    result.replace(QStringLiteral("@ACTIVE_TAB_ACCENT@"), rgba(colors.accent, style.activeTabAccentOpacity));
    result.replace(QStringLiteral("@BG_SELECTED@"), colorValue(colors.bgSelected));
    result.replace(QStringLiteral("@ACCENT@"), colorValue(colors.accent));
    result.replace(QStringLiteral("@INACTIVE_TAB_TEXT@"), colorValue(colors.textSecondary));
    result.replace(QStringLiteral("@TEXT_PRIMARY@"), colorValue(colors.textPrimary));
    result.replace(QStringLiteral("@CLOSE_BUTTON_HOVER@"), colorValue(colors.shellCloseHover));
    result.replace(QStringLiteral("@CLOSE_BUTTON_PRESSED@"), colorValue(colors.shellClosePressed));
    result.replace(QStringLiteral("@DROP_OVERLAY@"), rgba(colors.accent, style.dropOverlayOpacity));

    const auto number = [&result](const char* token, int value) {
        result.replace(QLatin1String(token), QString::number(value));
    };
    number("@BORDER_WIDTH@", metrics.borderWidth);
    number("@ACCENT_BORDER_WIDTH@", metrics.accentBorderWidth);
    number("@SPACING_COMPACT@", metrics.spacingCompact);
    number("@SPACING_XSMALL@", metrics.spacingXSmall);
    number("@SPACING_MEDIUM@", metrics.spacingMedium);
    number("@CONTROL_HEIGHT@", metrics.controlHeight);
    number("@COMPACT_HEIGHT@", metrics.control.compactHeight);
    number("@LARGE_HEIGHT@", metrics.control.largeHeight);
    number("@ICON_EXTENT@", metrics.iconExtent);
    number("@SMALL_ICON_EXTENT@", metrics.control.smallIconExtent);
    number("@CLOSE_BUTTON_EXTENT@", metrics.iconExtent + metrics.spacingCompact);
    number("@SPLITTER_EXTENT@", metrics.control.splitterExtent);
    result.replace(QStringLiteral("@TAB_FONT@"), FontManager::instance().styleSheet(snapshot_, FontRole::Body));
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
