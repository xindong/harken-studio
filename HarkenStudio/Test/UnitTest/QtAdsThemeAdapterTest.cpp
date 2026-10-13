#include "Core/BuiltinTheme.h"
#include "Core/QtAdsThemeAdapter.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QRegularExpression>
#include <QTextStream>

#include <memory>
#include <utility>

namespace {

VNX::QtAdsThemeAdapter builtinAdapter()
{
    return VNX::QtAdsThemeAdapter(
        std::make_shared<const VNX::ThemeSnapshot>(VNX::createBuiltinDarkTheme()));
}

QString packagedApplicationStyleSheet()
{
    QFile file(QStringLiteral(":/Resources/styles/dark_theme.qss"));
    EXPECT_TRUE(file.open(QFile::ReadOnly | QFile::Text));
    QTextStream stream(&file);
    return stream.readAll();
}

} // namespace

TEST(QtAdsThemeAdapter, BuiltinRulesMatchThemeContract)
{
    const QString fragment = builtinAdapter().styleSheet();
    ASSERT_FALSE(fragment.isEmpty());
    EXPECT_FALSE(fragment.contains(QRegularExpression(QStringLiteral("@[A-Z_]+@"))));
}

TEST(QtAdsThemeAdapter, StyleSheetConsumesProvidedColorsAndMetrics)
{
    VNX::ThemeSnapshot theme = VNX::createBuiltinDarkTheme();
    theme.ui.colors.accent = QColor(QStringLiteral("#123456"));
    theme.thirdParty.qtAds.closeButtonHover = QColor(QStringLiteral("#246813"));
    theme.thirdParty.qtAds.closeButtonPressed = QColor(QStringLiteral("#654321"));
    theme.thirdParty.qtAds.metrics.titleBarMinimumHeight = 47;
    theme.thirdParty.qtAds.metrics.titleBarIconSize = 18;
    theme.ui.metrics.navigationChevronExtent = 24;

    const VNX::QtAdsThemeAdapter adapter(
        std::make_shared<const VNX::ThemeSnapshot>(theme));
    const QString sheet = adapter.styleSheet();

    for (const auto& color : {theme.ui.colors.accent, theme.thirdParty.qtAds.closeButtonHover,
                              theme.thirdParty.qtAds.closeButtonPressed})
        EXPECT_TRUE(sheet.contains(color.name(QColor::HexRgb), Qt::CaseInsensitive))
            << color.name().toStdString();
    for (const int metric : {theme.thirdParty.qtAds.metrics.titleBarMinimumHeight,
                             theme.thirdParty.qtAds.metrics.titleBarIconSize,
                             theme.ui.metrics.navigationChevronExtent})
        EXPECT_TRUE(sheet.contains(QString::number(metric) + QStringLiteral("px"))) << metric;
}

TEST(QtAdsThemeAdapter, CompositionConsumesOneMarkerAndInsertsOneFragment)
{
    const VNX::QtAdsThemeAdapter adapter = builtinAdapter();
    const QString marker = VNX::QtAdsThemeAdapter::styleSheetMarker();
    const QString packaged = packagedApplicationStyleSheet();
    ASSERT_FALSE(packaged.isNull());
    ASSERT_EQ(packaged.count(marker), 1);

    const QString fragment = adapter.styleSheet();
    const QString composed = adapter.composeApplicationStyleSheet(packaged);

    EXPECT_FALSE(composed.contains(marker));
    EXPECT_EQ(composed.count(fragment), 1);
    EXPECT_EQ(composed.count(QStringLiteral("ads--CDockContainerWidget {")), 1);
    EXPECT_EQ(composed.count(QStringLiteral("QSplitter#inspectorEmbeddedSplitter::handle {")), 1);
}
