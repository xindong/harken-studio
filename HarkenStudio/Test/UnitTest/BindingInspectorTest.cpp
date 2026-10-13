#include "UnitTest/StudioObjectIdTestSupport.h"
#include "BindingEditing.h"
#include "Components/VMixNode.h"
#include "Components/VTrack.h"
#include "Components/VVariable.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Inspector/BindingFieldDecorator.h"
#include "Widgets/Inspector/BusInspectorSchema.h"
#include "Widgets/Inspector/ContentObjectSchemaRows.h"
#include "Widgets/Inspector/InspectorCustomizationBootstrap.h"
#include "Widgets/Inspector/InspectorPanel.h"
#include "Widgets/ExternalEditor/ExternalEditorPanel.h"
#include "Widgets/Inspector/LayerInspectorSchema.h"
#include "Widgets/Inspector/PropertyEditorEngine.h"
#include "Widgets/Selector/ObjectRefWidget.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QLabel>
#include <QTemporaryDir>
#include <QToolButton>
#include <QVBoxLayout>

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

struct Page
{
    VNX::PropertyEditorEngine engine;
    std::unique_ptr<QWidget> widget;

    explicit Page(VNX::ProjectItem::Type type)
    {
        if (type == VNX::ProjectItem::Type::Bus)
            engine.setSchema(VNX::busInspectorSchema(), VNX::ComponentType::Bus);
        else if (type == VNX::ProjectItem::Type::Layer)
            engine.setSchema(VNX::layerInspectorSchema(), VNX::ComponentType::MixNode);
        else
            engine.setSchema(VNX::contentObjectInspectorRows(type), VNX::ComponentType::Audio);
        engine.setItemType(type);
        widget.reset(engine.buildPage());
    }
};

}

TEST(BindingInspector, MixingSixExposeVolumeAndPitchHangPoints)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    for (const VNX::ProjectItem::Type type : {
             VNX::ProjectItem::Type::Sound,
             VNX::ProjectItem::Type::Container,
             VNX::ProjectItem::Type::Track,
             VNX::ProjectItem::Type::Piece,
             VNX::ProjectItem::Type::Layer}) {
        VNX::ProjectItem item(QStringLiteral("Bound"), type);
        item.attachDefaultComponents();
        Page page(type);
        page.engine.bindItem(&item);

        auto* volume = page.widget->findChild<QToolButton*>(
            QStringLiteral("bindingPoint_volume"));
        auto* pitch = page.widget->findChild<QToolButton*>(
            QStringLiteral("bindingPoint_pitch"));
        ASSERT_NE(volume, nullptr) << static_cast<int>(type);
        ASSERT_NE(pitch, nullptr) << static_cast<int>(type);
        EXPECT_EQ(volume->property("bindingState").toString(), QStringLiteral("none"));
        EXPECT_EQ(pitch->property("bindingState").toString(), QStringLiteral("none"));
        EXPECT_TRUE(volume->text().isEmpty());
        EXPECT_TRUE(pitch->text().isEmpty());
        EXPECT_TRUE(volume->icon().isNull());
        EXPECT_TRUE(pitch->icon().isNull());
        for (const char* field : {"volume", "pitch"}) {
            auto* attenuation = page.widget->findChild<VNX::InspectorSlotButton*>(
                QStringLiteral("attenuationPoint_") + QLatin1String(field));
            ASSERT_NE(attenuation, nullptr);
            EXPECT_FALSE(attenuation->isEnabled());
            EXPECT_FALSE(attenuation->isBound());
        }
    }

    VNX::ProjectItem bus(QStringLiteral("Master"), VNX::ProjectItem::Type::Bus);
    bus.attachDefaultComponents();
    Page busPage(VNX::ProjectItem::Type::Bus);
    busPage.engine.bindItem(&bus);
    auto* busVolume = busPage.widget->findChild<QToolButton*>(
        QStringLiteral("bindingPoint_volume"));
    auto* busPitch = busPage.widget->findChild<QToolButton*>(
        QStringLiteral("bindingPoint_pitch"));
    ASSERT_NE(busVolume, nullptr);
    EXPECT_EQ(busPitch, nullptr);
    EXPECT_EQ(busVolume->property("bindingState").toString(), QStringLiteral("none"));
    EXPECT_TRUE(busVolume->text().isEmpty());
    EXPECT_TRUE(busVolume->icon().isNull());
    EXPECT_EQ(busPage.widget->findChild<QWidget*>(QStringLiteral("attenuationPoint_volume")), nullptr);
}

TEST(BindingInspector, HangPointReflectsOnlyOwnBindingsWithoutReadout)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    VNX::ProjectItem parent(QStringLiteral("Owner"), VNX::ProjectItem::Type::Layer);
    VNX::ProjectItem child(QStringLiteral("Child"), VNX::ProjectItem::Type::Sound);
    parent.attachDefaultComponents();
    child.attachDefaultComponents();
    child.parent = &parent;
    const QUuid variable = QUuid::createUuid();
    auto contributed = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(), variable);
    contributed.guid = VNX::authoringUuid(harkentest::StudioObjectId(1));
    ASSERT_TRUE(parent.comp<VNX::VMixNode>()->bindings.upsert(std::move(contributed)));

    Page page(VNX::ProjectItem::Type::Sound);
    page.engine.bindItem(&child);
    auto* point = page.widget->findChild<QToolButton*>(
        QStringLiteral("bindingPoint_volume"));
    auto* readout = page.widget->findChild<QLabel*>(
        QStringLiteral("bindingReadout_volume"));
    ASSERT_NE(point, nullptr);
    EXPECT_EQ(readout, nullptr);
    EXPECT_EQ(point->property("bindingState").toString(), QStringLiteral("none"));

    auto own = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(), QUuid::createUuid());
    own.guid = VNX::authoringUuid(harkentest::StudioObjectId(2));
    ASSERT_TRUE(child.comp<VNX::VMixNode>()->bindings.upsert(std::move(own)));
    page.engine.bindItem(&child);
    EXPECT_EQ(point->property("bindingState").toString(), QStringLiteral("own"));
    EXPECT_EQ(page.widget->findChild<QLabel*>(QStringLiteral("bindingReadout_volume")), nullptr);
}

TEST(BindingInspector, BoundPitchDoesNotAddReadoutToBaseField)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    VNX::ProjectItem item(QStringLiteral("Bound"), VNX::ProjectItem::Type::Sound);
    item.attachDefaultComponents();
    item.comp<VNX::VMixNode>()->pitch = 2.0f;
    auto binding = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::pitchAddress(), QUuid::createUuid());
    binding.guid = VNX::authoringUuid(harkentest::StudioObjectId(3));
    ASSERT_TRUE(item.comp<VNX::VMixNode>()->bindings.upsert(std::move(binding)));

    Page page(VNX::ProjectItem::Type::Sound);
    page.engine.bindItem(&item);
    auto* readout = page.widget->findChild<QLabel*>(
        QStringLiteral("bindingReadout_pitch"));
    EXPECT_EQ(readout, nullptr);
    auto* point = page.widget->findChild<QToolButton*>(QStringLiteral("bindingPoint_pitch"));
    ASSERT_NE(point, nullptr);
    EXPECT_FLOAT_EQ(item.comp<VNX::VMixNode>()->pitch, 2.0f);
}

TEST(BindingInspector, AddressSignalRefreshesOnlyMatchingHangPoint)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    VNX::ProjectItem item(QStringLiteral("Bound"), VNX::ProjectItem::Type::Sound);
    item.attachDefaultComponents();
    Page page(VNX::ProjectItem::Type::Sound);
    page.engine.bindItem(&item);

    auto* volume = page.widget->findChild<QToolButton*>(QStringLiteral("bindingPoint_volume"));
    auto* pitch = page.widget->findChild<QToolButton*>(QStringLiteral("bindingPoint_pitch"));
    ASSERT_NE(volume, nullptr);
    ASSERT_NE(pitch, nullptr);
    EXPECT_EQ(volume->property("bindingState").toString(), QStringLiteral("none"));
    EXPECT_EQ(pitch->property("bindingState").toString(), QStringLiteral("none"));

    auto own = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(), QUuid::createUuid());
    own.guid = VNX::authoringUuid(harkentest::StudioObjectId(3));
    ASSERT_TRUE(item.comp<VNX::VMixNode>()->bindings.upsert(std::move(own)));
    EXPECT_TRUE(page.engine.refreshField(
        VNX::BindingEditing::changeKey(VNX::BindingSemantics::volumeAddress())));
    EXPECT_EQ(volume->property("bindingState").toString(), QStringLiteral("own"));
    EXPECT_EQ(pitch->property("bindingState").toString(), QStringLiteral("none"));
}

class BindingInspectorCurveDomainTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& manager = VNX::ProjectManager::instance();
        if (manager.isProjectOpen())
            manager.closeProject();
        ASSERT_TRUE(tempDir_.isValid());
        VNX::ProjectConfig config;
        config.name = QStringLiteral("BindingInspectorCurveDomain");
        config.location = tempDir_.path();
        ASSERT_TRUE(manager.createProject(config));
        item_ = manager.createItem(nullptr, QStringLiteral("BoundSound"),
                                   VNX::ProjectItem::Type::Sound);
        variable_ = manager.createItem(nullptr, QStringLiteral("FixedValue"),
                                       VNX::ProjectItem::Type::Variable);
        ASSERT_NE(item_, nullptr);
        ASSERT_NE(variable_, nullptr);
        VNX::UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        VNX::UndoRedoManager::instance().clear();
        auto& manager = VNX::ProjectManager::instance();
        if (manager.isProjectOpen())
            manager.closeProject();
    }

    QTemporaryDir tempDir_;
    VNX::ProjectItem* item_ = nullptr;
    VNX::ProjectItem* variable_ = nullptr;
};

TEST_F(BindingInspectorCurveDomainTest, FixedVariableRangePreservesNormalizedCurveShape)
{
    auto* variable = variable_->compOrCreate<VNX::VVariable>();
    variable->rangeMin = 5.0f;
    variable->rangeMax = 5.0f;
    VNX::ParameterBindingDef binding = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(), variable_->guid);
    binding.outputMin = -10.0f;
    binding.outputMax = 0.0f;
    binding.curve = {{0.0f, -10.0f}, {0.25f, -5.0f}, {1.0f, 0.0f}};
    binding.handles = {{0.1f, -8.0f, 0.2f, -6.0f},
                       {0.4f, -4.0f, 0.8f, -1.0f}};
    binding.guid = VNX::authoringUuid(harkentest::StudioObjectId(11));
    ASSERT_TRUE(item_->compOrCreate<VNX::VMixNode>()->bindings.upsert(binding));

    const auto* stored = item_->comp<VNX::VMixNode>()->bindings.findFirst(
        VNX::BindingSemantics::volumeAddress());
    ASSERT_NE(stored, nullptr);
    ASSERT_EQ(stored->curve.size(), 3);
    EXPECT_FLOAT_EQ(stored->curve[1].x, 0.25f);
    ASSERT_EQ(stored->handles.size(), 2);
    EXPECT_NEAR(stored->handles[0].h1x, 0.1f, 0.000001f);
    EXPECT_NEAR(stored->handles[1].h2x, 0.8f, 0.000001f);
    EXPECT_NEAR(VNX::BindingSemantics::contribution(
                    *stored, VNX::VariableKind::Smooth, 5.0f, 5.0f, 5.0f),
                -10.0f, 0.0001f);
}

TEST_F(BindingInspectorCurveDomainTest, TimelineInspectionUsesDefinitionBindingSource)
{
    VNX::registerBuiltinInspectorCustomizations();
    auto& manager = VNX::ProjectManager::instance();
    VNX::ProjectItem* piece = manager.createItem(
        nullptr, QStringLiteral("ContextPiece"), VNX::ProjectItem::Type::Piece);
    VNX::ProjectItem* track = manager.createItem(
        piece, QStringLiteral("ContextTrack"), VNX::ProjectItem::Type::Track);
    ASSERT_NE(track, nullptr);
    VNX::Clip clip{QUuid::createUuid(), item_->guid, 0, 1};
    track->comp<VNX::VTrack>()->clips.items.push_back(clip);
    auto placement = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(), variable_->guid);
    placement.guid = VNX::authoringUuid(harkentest::StudioObjectId(12));
    ASSERT_TRUE(track->comp<VNX::VMixNode>()->bindings.upsert(std::move(placement)));

    VNX::InspectorPanel panel;
    panel.showItemInspector(
        item_, VNX::PlacementContext::fromOccurrence(
                   {track->guid, clip.clipGuid, item_->guid}));
    auto* point = panel.findChild<QToolButton*>(QStringLiteral("bindingPoint_volume"));
    auto* readout = panel.findChild<QLabel*>(QStringLiteral("bindingReadout_volume"));
    ASSERT_NE(point, nullptr);
    EXPECT_EQ(readout, nullptr);

    panel.showItemInspector(item_);
    EXPECT_EQ(panel.placementContext(), nullptr);
}

TEST_F(BindingInspectorCurveDomainTest, PlacementBindingChangesDoNotEnterDefinitionInspectorOrEditor)
{
    VNX::ExternalEditorPanel editor;
    VNX::registerBuiltinInspectorCustomizations();
    auto& manager = VNX::ProjectManager::instance();
    auto* piece = manager.createItem(nullptr, QStringLiteral("ContextPiece"), VNX::ProjectItem::Type::Piece);
    auto* track = manager.createItem(piece, QStringLiteral("ContextTrack"), VNX::ProjectItem::Type::Track);
    ASSERT_NE(track, nullptr);
    VNX::Clip clip{QUuid::createUuid(), item_->guid, 0, 1};
    track->comp<VNX::VTrack>()->clips.items.push_back(clip);
    VNX::InspectorPanel panel;
    panel.showItemInspector(item_, VNX::PlacementContext::fromOccurrence(
        {track->guid, clip.clipGuid, item_->guid}));
    auto* point = panel.findChild<QToolButton*>(QStringLiteral("bindingPoint_volume"));
    ASSERT_NE(point, nullptr);
    EXPECT_EQ(panel.findChild<QLabel*>(QStringLiteral("bindingReadout_volume")), nullptr);
    EXPECT_EQ(point->property("bindingState").toString(), QStringLiteral("none"));
    editor.bindSelection(VNX::ExternalEditorSelection::binding(
        item_->guid, VNX::BindingSemantics::volumeAddress()));
    auto* popup = editor.findChild<QWidget*>(QStringLiteral("bindingFieldEditor_volume"));
    ASSERT_NE(popup, nullptr);
    auto* table = popup->findChild<VNX::TableWidget*>();
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(table->rowsLayout()->count(), 0);
    EXPECT_EQ(table->rowsWidget()->minimumHeight(), 0);
    EXPECT_TRUE(popup->findChildren<VNX::ObjectRefWidget*>(QStringLiteral("bindingVariableRef")).isEmpty());

    ASSERT_TRUE(VNX::BindingEditing::upsert(track,
        VNX::BindingSemantics::defaultBinding(VNX::BindingSemantics::volumeAddress(), variable_->guid)));
    QApplication::processEvents();
    EXPECT_EQ(point->property("bindingState").toString(), QStringLiteral("none"));
    EXPECT_EQ(table->rowsLayout()->count(), 0);
    EXPECT_TRUE(popup->findChildren<VNX::ObjectRefWidget*>(QStringLiteral("bindingVariableRef")).isEmpty());

    ASSERT_TRUE(VNX::BindingEditing::removeAll(track, VNX::BindingSemantics::volumeAddress()));
    QApplication::processEvents();
    EXPECT_EQ(point->property("bindingState").toString(), QStringLiteral("none"));
    EXPECT_EQ(panel.findChild<QLabel*>(QStringLiteral("bindingReadout_volume")), nullptr);
    EXPECT_EQ(table->rowsLayout()->count(), 0);
    EXPECT_EQ(table->rowsWidget()->minimumHeight(), 0);
    EXPECT_TRUE(popup->findChildren<VNX::ObjectRefWidget*>(QStringLiteral("bindingVariableRef")).isEmpty());
    ASSERT_TRUE(VNX::BindingEditing::upsert(item_,
        VNX::BindingSemantics::defaultBinding(VNX::BindingSemantics::volumeAddress(), variable_->guid)));
    QApplication::processEvents();
    EXPECT_EQ(point->property("bindingState").toString(), QStringLiteral("own"));
    EXPECT_EQ(table->rowsLayout()->count(), 1);
    EXPECT_EQ(popup->findChildren<VNX::ObjectRefWidget*>(QStringLiteral("bindingVariableRef")).size(), 1);
}
