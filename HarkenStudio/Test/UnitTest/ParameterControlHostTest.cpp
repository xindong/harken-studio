#include "Core/ParameterPresenterRegistry.h"
#include "Core/ThemeManager.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/Common/ParameterControlHost.h"
#include "Widgets/Common/ParameterValueEditor.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDoubleSpinBox>

#include <memory>

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

}

TEST(ParameterControlHost, IndependentValueEditorSurvivesBindingDestruction)
{
    ensureApp();
    auto host = std::make_unique<VNX::ParameterControlHost>(
        QStringLiteral("Mode"), VNX::ParameterPresenterContext::InspectorFloat,
        VNX::ParameterPresenterId::HorizontalSlider);
    auto* binding = host->binding();
    binding->setRange(2, 5);
    binding->setDecimals(2);
    VNX::ParameterValueEditor editor(binding, &VNX::ThemeManager::instance());
    int commits = 0;
    QObject::connect(binding, &VNX::ParameterBinding::valueCommitted,
                     [&commits](double) { ++commits; });
    auto* spin = editor.valueEditor();
    ASSERT_NE(spin, nullptr);
    spin->setValue(3.25);
    EXPECT_DOUBLE_EQ(binding->value(), 3.25);
    ASSERT_GT(commits, 0);
    const int before = commits;
    host.reset();
    editor.cancelPendingInput();
    spin->setValue(4.5);
    EXPECT_EQ(commits, before);
}
