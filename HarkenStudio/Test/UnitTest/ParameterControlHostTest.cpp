#include "Core/ParameterPresenterRegistry.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/ThemeManager.h"
#include "Widgets/Common/ComponentRecipeHost.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/Common/ParameterControlHost.h"
#include "Widgets/Common/ParameterPresenter.h"
#include "Widgets/Common/ParameterValueEditor.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDoubleSpinBox>
#include <QDir>
#include <QFile>
#include <QLayout>
#include <QPointer>
#include <QSlider>
#include <QTemporaryDir>

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

TEST(ParameterControlHost, ColorPreviewPreservesActiveEditingAndComponentStructure)
{
    ensureApp();
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const auto package = QDir(root.path()).filePath(QStringLiteral("structural"));
    ASSERT_TRUE(QDir().mkpath(package));
    const QList<QPair<QString, QByteArray>> files{
        {QStringLiteral("manifest.json"), R"json({
            "schemaVersion": 6, "id": "theme.structural", "displayName": "Structural",
            "version": "1.0.0", "baseThemeId": "harken.builtin.dark",
            "compatibility": {"harkenVersion": ">=0.1.0 <1.0.0",
                "capabilities": ["curve-v1", "meter-v1", "waveform-v1"]},
            "entries": {"tokens": "tokens.json", "assetMap": "assets.json",
                "visuals": {"curve": "visuals.json", "meter": "visuals.json", "waveform": "visuals.json"},
                "presenters": "presenters.json", "recipes": "recipes.json"}
        })json"},
        {QStringLiteral("tokens.json"), R"json({"colors":{}})json"},
        {QStringLiteral("visuals.json"), R"json({"colors":{}})json"},
        {QStringLiteral("assets.json"), R"json({"assets":{}})json"},
        {QStringLiteral("presenters.json"), R"json({"contexts":{"inspector.float":"rotary-knob"}})json"},
        {QStringLiteral("recipes.json"), R"json({"recipes":[{
            "host":"track.header", "variant":"track.expanded", "slots":[{"id":"identity"}]
        }]})json"}
    };
    for (const auto& [name, bytes] : files) {
        QFile file(QDir(package).filePath(name));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write(bytes), bytes.size());
    }

    VNX::ThemeManager manager(root.path(), std::make_unique<VNX::MemorySettingsStorage>());
    VNX::ParameterControlHost parameter(QStringLiteral("Level"),
        VNX::ParameterPresenterContext::InspectorFloat, nullptr, &manager);
    VNX::ComponentRecipeHost recipe(VNX::ComponentRecipeHostId::TrackHeader, nullptr, &manager);
    auto* identity = new QWidget;
    ASSERT_TRUE(recipe.registerSlot(VNX::ComponentRecipeSlotId::Identity, identity));
    QPointer<VNX::ParameterPresenter> presenter(parameter.presenter());
    QPointer<QLayout> layout(recipe.layout());
    ASSERT_NE(presenter, nullptr);
    ASSERT_NE(layout, nullptr);
    auto* slider = presenter->slider();
    ASSERT_NE(slider, nullptr);
    auto* binding = parameter.binding();
    binding->setValue(0.25);
    int ended = 0;
    QObject::connect(binding, &VNX::ParameterBinding::editEnded,
                     [&ended] { ++ended; });
    slider->setSliderDown(true);
    binding->previewEdit(0.75);
    ASSERT_TRUE(binding->isEditing());

    auto appearance = manager.appearance();
    const QColor accent(QStringLiteral("#e45a9d"));
    appearance.colors[appearance.paletteId].insert(QStringLiteral("accent.primary"), accent);
    ASSERT_TRUE(manager.previewAppearance(appearance).ok());
    QApplication::processEvents();
    ASSERT_FALSE(presenter.isNull());
    ASSERT_FALSE(layout.isNull());
    EXPECT_EQ(manager.colors().accent, accent);
    EXPECT_EQ(parameter.presenter(), presenter.data());
    EXPECT_EQ(recipe.layout(), layout.data());
    EXPECT_EQ(recipe.slotWidget(VNX::ComponentRecipeSlotId::Identity), identity);
    EXPECT_TRUE(binding->isEditing());
    EXPECT_TRUE(slider->isSliderDown());
    EXPECT_DOUBLE_EQ(binding->value(), 0.75);
    EXPECT_EQ(ended, 0);

    manager.cancelAppearancePreview();
    QApplication::processEvents();
    ASSERT_FALSE(presenter.isNull());
    ASSERT_FALSE(layout.isNull());
    EXPECT_EQ(parameter.presenter(), presenter.data());
    EXPECT_EQ(recipe.layout(), layout.data());
    EXPECT_TRUE(binding->isEditing());
    EXPECT_EQ(ended, 0);
    slider->setSliderDown(false);
    EXPECT_EQ(ended, 1);
    EXPECT_DOUBLE_EQ(binding->value(), 0.75);

    ASSERT_TRUE(manager.selectTheme(QStringLiteral("theme.structural")).ok());
    EXPECT_EQ(parameter.presenterId(), VNX::ParameterPresenterId::RotaryKnob);
    EXPECT_EQ(recipe.appliedVariant(), VNX::ComponentRecipeVariant::TrackExpanded);
    EXPECT_EQ(recipe.orderedSlotNames(), QStringList{QStringLiteral("identity")});
    EXPECT_EQ(recipe.slotWidget(VNX::ComponentRecipeSlotId::Identity), identity);
    EXPECT_TRUE(presenter.isNull());
    EXPECT_TRUE(layout.isNull());
    EXPECT_DOUBLE_EQ(binding->value(), 0.75);
}
