#include "ActionManager.h"
#include "Components/VAttenuation.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "SelectionRouter.h"
#include "Widgets/ExternalEditor/ExternalEditorPanel.h"
#include "Widgets/CurveEditor/CurveEditorWidget.h"
#include "Widgets/Inspector/InspectorCustomizationBootstrap.h"
#include "Widgets/Inspector/InspectorPanel.h"
#include "Widgets/Inspector/Providers/DimensionsSectionProvider.h"

#include <gtest/gtest.h>

#include <QComboBox>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QStackedWidget>
#include <QTemporaryDir>

namespace {

class CurveEditorWidgetTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        VNX::registerBuiltinInspectorCustomizations();
        auto& projectManager = VNX::ProjectManager::instance();
        if (projectManager.isProjectOpen())
            projectManager.closeProject();

        ASSERT_TRUE(tempDir_.isValid());
        VNX::ProjectConfig config;
        config.name = QStringLiteral("CurveEditorWidgetProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(projectManager.createProject(config));

        attenuation_ = projectManager.createItem(
            nullptr, QStringLiteral("CurveEditorAttenuation"),
            VNX::ProjectItem::Type::Attenuation);
        ASSERT_NE(attenuation_, nullptr);
        auto* component = attenuation_->comp<VNX::VAttenuation>();
        ASSERT_NE(component, nullptr);
        component->dimensions.emplaceBack(VNX::AttenuationDimensionDef{VNX::AttenuationDimension::Volume});
        component->dimensions.emplaceBack(VNX::AttenuationDimensionDef{VNX::AttenuationDimension::Pitch});
    }

    void TearDown() override
    {
        auto& projectManager = VNX::ProjectManager::instance();
        if (projectManager.isProjectOpen())
            projectManager.closeProject();
    }

    QTemporaryDir tempDir_;
    VNX::ProjectItem* attenuation_ = nullptr;
};

TEST_F(CurveEditorWidgetTest, StandaloneEditorHasNoDimensionSelector)
{
    const auto* component = attenuation_->comp<VNX::VAttenuation>();
    ASSERT_NE(component, nullptr);

    VNX::ExternalEditorPanel standalone;
    standalone.bindSelection(VNX::CurveSelection::attenuation(
        component->hostItem()->guid,
        component->hostItem()->name,
        VNX::AttenuationDimension::Volume));
    const auto editors = standalone.findChildren<VNX::CurveEditorWidget*>();
    ASSERT_EQ(editors.size(), 1);
    auto* editor = editors.front();
    ASSERT_NE(editor, nullptr);
    EXPECT_TRUE(editor->findChildren<QComboBox*>().isEmpty());
    EXPECT_EQ(editor->dimension(), VNX::AttenuationDimension::Volume);
}

TEST_F(CurveEditorWidgetTest, BindingAnAttenuationLeavesTheEditorBlankUntilADimensionIsSelected)
{
    VNX::ExternalEditorPanel panel;
    EXPECT_FALSE(panel.acceptsSelection(attenuation_));
    panel.bindItem(attenuation_);
    EXPECT_TRUE(panel.acceptsSelection(attenuation_));
    EXPECT_FALSE(panel.acceptsSelection(nullptr));
    EXPECT_EQ(panel.boundItem(), attenuation_);
    EXPECT_TRUE(panel.selection().curve.attenuationGuid.isNull());
    EXPECT_TRUE(panel.selection().curve.attenuationName.isEmpty());

    auto* stack = panel.findChild<QStackedWidget*>();
    ASSERT_NE(stack, nullptr);
    EXPECT_EQ(stack->currentIndex(), 0);
    for (auto* label : stack->currentWidget()->findChildren<QLabel*>())
        EXPECT_TRUE(label->text().isEmpty());

    const auto* component = attenuation_->comp<VNX::VAttenuation>();
    ASSERT_NE(component, nullptr);
    panel.bindSelection(VNX::CurveSelection::attenuation(
        component->hostItem()->guid, component->hostItem()->name,
        VNX::AttenuationDimension::Volume));
    EXPECT_EQ(panel.boundItem(), attenuation_);
    EXPECT_EQ(panel.selection().curve.attenuationGuid, component->hostItem()->guid);
    EXPECT_EQ(panel.selection().curve.dimension, VNX::AttenuationDimension::Volume);

    panel.bindItem(attenuation_);
    EXPECT_EQ(panel.selection().curve.attenuationGuid, component->hostItem()->guid);
}

TEST_F(CurveEditorWidgetTest, EditorConsumesCurveSelectionWithoutOwningASelector)
{
    VNX::CurveEditorWidget editor;
    const auto* component = attenuation_->comp<VNX::VAttenuation>();
    ASSERT_NE(component, nullptr);
    editor.bindSelection(VNX::CurveSelection::attenuation(
        component->hostItem()->guid, component->hostItem()->name,
        VNX::AttenuationDimension::Pitch));

    EXPECT_TRUE(editor.findChildren<QComboBox*>().isEmpty());
    EXPECT_EQ(editor.dimension(), VNX::AttenuationDimension::Pitch);
}

TEST_F(CurveEditorWidgetTest, EmptyDimensionThumbnailsMatchDefaultEditorCurves)
{
    auto* component = attenuation_->comp<VNX::VAttenuation>();
    ASSERT_NE(component, nullptr);
    component->maxDistance = 250.0f;
    const QJsonObject before = component->toJson();

    VNX::DimensionsSectionProvider provider;
    std::unique_ptr<QWidget> emptySection(provider.build(nullptr));
    provider.bind(emptySection.get(), attenuation_);
    EXPECT_EQ(component->toJson(), before);
    const auto emptyThumbnails = emptySection->findChildren<QWidget*>(
        QStringLiteral("curveThumbnail"));
    ASSERT_EQ(emptyThumbnails.size(), 2);

    VNX::CurveEditorWidget editor;
    for (const auto type : {VNX::AttenuationDimension::Volume,
                            VNX::AttenuationDimension::Pitch}) {
        editor.bindSelection(VNX::CurveSelection::attenuation(
            component->hostItem()->guid, component->hostItem()->name, type));
        auto dimension = std::find_if(component->dimensions.begin(), component->dimensions.end(),
            [type](const auto& entry) { return entry.type == type; });
        ASSERT_NE(dimension, component->dimensions.end());
        EXPECT_TRUE(dimension->curve.isEmpty());
        dimension->curve = editor.canvas()->curve();
        dimension->handles = editor.canvas()->curveHandles();
    }
    std::unique_ptr<QWidget> explicitSection(provider.build(nullptr));
    provider.bind(explicitSection.get(), attenuation_);
    const auto explicitThumbnails = explicitSection->findChildren<QWidget*>(
        QStringLiteral("curveThumbnail"));
    ASSERT_EQ(explicitThumbnails.size(), 2);

    const auto render = [](QWidget* thumbnail) {
        thumbnail->resize(180, 24);
        QImage image(thumbnail->size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        thumbnail->render(&image);
        return image;
    };
    for (int i = 0; i < emptyThumbnails.size(); ++i)
        EXPECT_EQ(render(emptyThumbnails[i]), render(explicitThumbnails[i]));
}

TEST_F(CurveEditorWidgetTest, CurveSelectionIsRoutedOnceAndConsumedWithoutRepublishing)
{
    VNX::InspectorPanel inspector;
    inspector.showItemInspector(attenuation_);
    VNX::SelectionRouter router(nullptr);
    int requestedCount = 0;
    int routedCount = 0;
    VNX::CurveSelection last;
    QObject::connect(&inspector, &VNX::InspectorPanel::curveSelectionRequested,
                     [&router, &requestedCount](const VNX::CurveSelection& selection) {
        ++requestedCount;
        router.selectCurve(selection);
    });
    QObject::connect(
        &router, &VNX::SelectionRouter::curveSelectionChanged,
        [&last, &routedCount](const VNX::CurveSelection& selection) {
            ++routedCount;
            last = selection;
        });

    inspector.selectCurveDimension(VNX::AttenuationDimension::Pitch);

    const auto* component = attenuation_->comp<VNX::VAttenuation>();
    ASSERT_NE(component, nullptr);
    EXPECT_TRUE(last.belongsTo(*component->hostItem()));
    EXPECT_EQ(last.dimension, VNX::AttenuationDimension::Pitch);
    EXPECT_EQ(requestedCount, 1);
    EXPECT_EQ(routedCount, 1);
}

TEST_F(CurveEditorWidgetTest, SelectingForeignObjectClearsUnpinnedCurveEditor)
{
    const auto* component = attenuation_->comp<VNX::VAttenuation>();
    ASSERT_NE(component, nullptr);

    auto& projectManager = VNX::ProjectManager::instance();
    VNX::ProjectItem* other = projectManager.createItem(
        nullptr, QStringLiteral("CurveEditorOtherSound"),
        VNX::ProjectItem::Type::Sound);
    ASSERT_NE(other, nullptr);

    VNX::SelectionRouter router(nullptr);
    VNX::CurveSelection last;
    int emptyCount = 0;
    QObject::connect(
        &router, &VNX::SelectionRouter::curveSelectionChanged,
        [&last, &emptyCount](const VNX::CurveSelection& selection) {
            last = selection;
            if (!selection.hasAttenuation())
                ++emptyCount;
        });

    VNX::ExternalEditorPanel unpinned;
    VNX::ExternalEditorPanel pinned;
    pinned.setPinned(true);
    QObject::connect(
        &router, &VNX::SelectionRouter::curveSelectionChanged,
        [&unpinned, &pinned](const VNX::CurveSelection& selection) {
            if (!selection.hasAttenuation()) {
                if (!unpinned.isPinned())
                    unpinned.clearEditor();
                if (!pinned.isPinned())
                    pinned.clearEditor();
                return;
            }
            if (!unpinned.isPinned())
                unpinned.bindSelection(selection);
            if (!pinned.isPinned())
                pinned.bindSelection(selection);
        });

    const VNX::CurveSelection volume = VNX::CurveSelection::attenuation(
        component->hostItem()->guid, component->hostItem()->name,
        VNX::AttenuationDimension::Volume);
    unpinned.bindSelection(volume);
    pinned.bindSelection(volume);
    router.selectCurve(volume);
    EXPECT_TRUE(last.hasAttenuation());
    EXPECT_EQ(unpinned.selection().curve.attenuationGuid, component->hostItem()->guid);
    EXPECT_EQ(pinned.selection().curve.attenuationGuid, component->hostItem()->guid);

    router.routeSelection(other, false);
    EXPECT_FALSE(last.hasAttenuation());
    EXPECT_EQ(emptyCount, 1);
    EXPECT_TRUE(unpinned.selection().curve.attenuationGuid.isNull());
    EXPECT_TRUE(unpinned.selection().curve.attenuationName.isEmpty());
    EXPECT_EQ(pinned.selection().curve.attenuationGuid, component->hostItem()->guid);
}

TEST_F(CurveEditorWidgetTest, SelectingOwningAttenuationKeepsCurveSelection)
{
    const auto* component = attenuation_->comp<VNX::VAttenuation>();
    ASSERT_NE(component, nullptr);

    VNX::SelectionRouter router(nullptr);
    VNX::CurveSelection last;
    int publishCount = 0;
    QObject::connect(
        &router, &VNX::SelectionRouter::curveSelectionChanged,
        [&last, &publishCount](const VNX::CurveSelection& selection) {
            last = selection;
            ++publishCount;
        });

    router.selectCurve(VNX::CurveSelection::attenuation(
        component->hostItem()->guid, component->hostItem()->name,
        VNX::AttenuationDimension::Pitch));
    router.routeSelection(attenuation_, false);

    EXPECT_TRUE(last.hasAttenuation());
    EXPECT_TRUE(last.belongsTo(*component->hostItem()));
    EXPECT_EQ(last.dimension, VNX::AttenuationDimension::Pitch);
    EXPECT_EQ(publishCount, 2);
}

TEST(CurveEditorOpenAction, OmittedDimensionDefaultsToVolumeAndRejectsInvalid)
{
    auto& mgr = VNX::ActionManager::instance();
    mgr.clearAllForTesting();
    VNX::registerSystemActions(mgr, nullptr, nullptr, nullptr);

    const auto omitted = mgr.execute(
        QStringLiteral("open_curve_editor"),
        QJsonObject{{QStringLiteral("name"), QStringLiteral("AnyAttenuation")}});
    EXPECT_FALSE(omitted.success);
    EXPECT_EQ(omitted.errorCode, QStringLiteral("MAIN_WINDOW_NOT_AVAILABLE"));

    const auto empty = mgr.execute(
        QStringLiteral("open_curve_editor"),
        QJsonObject{{QStringLiteral("name"), QStringLiteral("AnyAttenuation")},
                    {QStringLiteral("dimension"), QString()}});
    EXPECT_FALSE(empty.success);
    EXPECT_EQ(empty.errorCode, QStringLiteral("MAIN_WINDOW_NOT_AVAILABLE"));

    const auto pitch = mgr.execute(
        QStringLiteral("open_curve_editor"),
        QJsonObject{{QStringLiteral("name"), QStringLiteral("AnyAttenuation")},
                    {QStringLiteral("dimension"), QStringLiteral("pitch")}});
    EXPECT_FALSE(pitch.success);
    EXPECT_EQ(pitch.errorCode, QStringLiteral("MAIN_WINDOW_NOT_AVAILABLE"));

    const auto invalid = mgr.execute(
        QStringLiteral("open_curve_editor"),
        QJsonObject{{QStringLiteral("name"), QStringLiteral("AnyAttenuation")},
                    {QStringLiteral("dimension"), QStringLiteral("nope")}});
    EXPECT_FALSE(invalid.success);
    EXPECT_EQ(invalid.errorCode, QStringLiteral("DIMENSION_REQUIRED"));

    const auto numeric = mgr.execute(
        QStringLiteral("open_curve_editor"),
        QJsonObject{{QStringLiteral("name"), QStringLiteral("AnyAttenuation")},
                    {QStringLiteral("dimension"), 0}});
    EXPECT_FALSE(numeric.success);
    EXPECT_EQ(numeric.errorCode, QStringLiteral("DIMENSION_REQUIRED"));

    const auto boolean = mgr.execute(
        QStringLiteral("open_curve_editor"),
        QJsonObject{{QStringLiteral("name"), QStringLiteral("AnyAttenuation")},
                    {QStringLiteral("dimension"), true}});
    EXPECT_FALSE(boolean.success);
    EXPECT_EQ(boolean.errorCode, QStringLiteral("DIMENSION_REQUIRED"));

    const auto array = mgr.execute(
        QStringLiteral("open_curve_editor"),
        QJsonObject{{QStringLiteral("name"), QStringLiteral("AnyAttenuation")},
                    {QStringLiteral("dimension"), QJsonArray{QStringLiteral("volume")}}});
    EXPECT_FALSE(array.success);
    EXPECT_EQ(array.errorCode, QStringLiteral("DIMENSION_REQUIRED"));

    mgr.clearAllForTesting();
}

}
