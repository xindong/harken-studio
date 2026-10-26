#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/Common/ParameterSlider.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QObject>

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

TEST(ParameterSlider, MixedValueDoesNotCommitSentinelOnDisplay)
{
    ensureApp();
    VNX::ParameterSlider slider(QStringLiteral("Volume"));
    slider.setRange(-96.0, 24.0, 1);
    slider.setValue(-6.0);
    int committed = 0;
    int previewed = 0;
    QObject::connect(slider.binding(), &VNX::ParameterBinding::valueCommitted,
                     [&committed](double) { ++committed; });
    QObject::connect(slider.binding(), &VNX::ParameterBinding::editPreviewed,
                     [&previewed](double) { ++previewed; });
    slider.setMixedValue(true);
    EXPECT_EQ(committed, 0);
    EXPECT_EQ(previewed, 0);
    EXPECT_DOUBLE_EQ(slider.binding()->value(), -6.0);
}

TEST(ParameterSlider, MixedDisplaySurvivesPresenterRebuildWithoutEmitting)
{
    ensureApp();
    VNX::ParameterSlider slider(QStringLiteral("Volume"));
    slider.setRange(-96.0, 24.0, 1);
    slider.setValue(-12.0);
    int valueChanges = 0;
    int committed = 0;
    QObject::connect(&slider, &VNX::ParameterSlider::valueChanged,
                     [&valueChanges](double) { ++valueChanges; });
    QObject::connect(slider.binding(), &VNX::ParameterBinding::valueCommitted,
                     [&committed](double) { ++committed; });
    slider.setMixedValue(true);
    EXPECT_TRUE(slider.mixedValue());
    slider.setPresenterOverride(VNX::ParameterPresenterId::VerticalFader);
    EXPECT_TRUE(slider.mixedValue());
    EXPECT_EQ(valueChanges, 0);
    EXPECT_EQ(committed, 0);
}

}
