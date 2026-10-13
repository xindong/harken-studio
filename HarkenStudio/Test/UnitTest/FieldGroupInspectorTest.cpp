#include "Support/ProjectPlatformTestUtils.h"
#include "Support/FieldGroupTestUtils.h"
#include "ActionManager.h"
#include "AttenuationStore.h"
#include "ItemTypeCatalog.h"
#include "Components/VTrack.h"
#include "PlatformContextService.h"
#include "ProjectManager.h"
#include "FieldGroupProjectAdapter.h"
#include "UndoCommands.h"
#include "Widgets/Common/CollapsibleSection.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/Common/ParameterSlider.h"
#include "Widgets/Common/ParameterPresenter.h"
#include "Widgets/Inspector/ContentObjectSchemaRows.h"
#include "Widgets/Inspector/AttenuationFieldDecorator.h"
#include "Widgets/ExternalEditor/ExternalEditorPanel.h"
#include "Widgets/CurveEditor/CurveEditorWidget.h"
#include "Widgets/Inspector/BindingFieldDecorator.h"
#include "Widgets/Inspector/InspectorCustomizationBootstrap.h"
#include "Widgets/Inspector/InspectorPanel.h"
#include "Widgets/Inspector/PropertyEditorEngine.h"
#include "Widgets/Selector/ObjectRefWidget.h"
#include "Widgets/Selector/SelectorWindow.h"

#include <gtest/gtest.h>
#include <QApplication>
#include <QCheckBox>
#include <QContextMenuEvent>
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QLineEdit>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidget>

namespace VNX {
namespace {

class FieldGroupInspectorTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        if (!qApp) {
            static int argc = 1;
            static char name[] = "harken_editor_tests";
            static char* argv[] = {name, nullptr};
            static QApplication app(argc, argv);
        }
        auto& pm = ProjectManager::instance();
        if (pm.isProjectOpen()) pm.closeProject();
        ProjectConfig config;
        config.name = QStringLiteral("InspectorGroups");
        config.location = directory.path();
        config.platforms = HarkenTest::platforms({QStringLiteral("Windows"), QStringLiteral("Android")});
        ASSERT_TRUE(pm.createProject(config));
        parent = pm.createItem(nullptr, QStringLiteral("OwnerLayer"), ProjectItem::Type::Layer);
        sound = pm.createItem(parent, QStringLiteral("DefinitionSound"), ProjectItem::Type::Sound);
        other = pm.createItem(parent, QStringLiteral("OtherSound"), ProjectItem::Type::Sound);
        master = pm.getMasterBusItem();
        busA = pm.createItem(master, QStringLiteral("BusA"), ProjectItem::Type::Bus);
        busB = pm.createItem(master, QStringLiteral("BusB"), ProjectItem::Type::Bus);
        ASSERT_NE(sound, nullptr);
        ASSERT_NE(busA, nullptr);
        ASSERT_NE(busB, nullptr);
        auto& actions = ActionManager::instance();
        actions.clearAllForTesting();
        registerProjectActions(actions, &pm, nullptr, {});
        registerBuiltinInspectorCustomizations();
        UndoRedoManager::instance().clear();
        PlatformContextService::instance().setAll(true);
        panel = std::make_unique<InspectorPanel>();
        panel->showItemInspector(sound);
    }

    void TearDown() override
    {
        SelectorWindow::dismiss();
        panel.reset();
        UndoRedoManager::instance().clear();
        ActionManager::instance().clearAllForTesting();
        ProjectManager::instance().closeProject();
    }

    QJsonObject args(ProjectItem* item, const QString& group, const QString& platform = {})
    {
        QJsonObject value{{"definition_guid", item->guid.toString(QUuid::WithoutBraces)}, {"group", group}};
        if (platform.isEmpty()) value.insert("scope", "Shared");
        else value.insert("platform_ids", QJsonArray{HarkenTest::platformIdString(platform)});
        return value;
    }

    void mode(ProjectItem* item, const QString& group, const QString& modeName,
              const QString& platform = {})
    {
        auto value = args(item, group, platform);
        value.insert("mode", modeName);
        const auto result = ActionManager::instance().execute("set_field_group_mode", value);
        ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    }

    void field(ProjectItem* item, const QString& group, const QString& key,
               const QJsonValue& value, const QString& platform = {})
    {
        auto command = args(item, group, platform);
        command.insert("field", key);
        command.insert("value", value);
        const auto result = ActionManager::instance().execute("set_field_group_field", command);
        ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    }

    template<class T> T* widget(const QString& name)
    {
        auto* pages = panel->findChild<QStackedWidget*>();
        EXPECT_NE(pages, nullptr);
        auto* found = pages ? pages->currentWidget()->findChild<T*>(name) : nullptr;
        EXPECT_NE(found, nullptr) << name.toStdString();
        return found;
    }

    QCheckBox* inherit(const QString& group) { return widget<QCheckBox>("inherit_" + group); }
    ParameterSlider* slider(const QString& fieldName) { return widget<ParameterSlider>("prop_" + fieldName); }
    ObjectRefWidget* ref(const QString& fieldName) { return widget<ObjectRefWidget>("prop_" + fieldName); }

    void openMenu(const QString& group)
    {
        auto* section = inherit(group)->parentWidget()->parentWidget();
        QContextMenuEvent event(QContextMenuEvent::Mouse, QPoint(4, 4),
            section->mapToGlobal(QPoint(4, 4)));
        QApplication::sendEvent(section, &event);
    }

    void menu(const QString& group, const QString& actionName)
    {
        openMenu(group);
        auto* popup = panel->findChild<QMenu*>("fieldGroupMenu_" + group);
        ASSERT_NE(popup, nullptr);
        auto* action = popup->findChild<QAction*>(actionName);
        ASSERT_NE(action, nullptr);
        action->trigger();
        popup->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    void selectPlatforms()
    {
        PlatformContextService::instance().setSelected(
            {HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Windows, true);
    }

    QTemporaryDir directory;
    ProjectItem* parent = nullptr;
    ProjectItem* sound = nullptr;
    ProjectItem* other = nullptr;
    ProjectItem* master = nullptr;
    ProjectItem* busA = nullptr;
    ProjectItem* busB = nullptr;
    std::unique_ptr<InspectorPanel> panel;
};

TEST_F(FieldGroupInspectorTest, InheritedGroupsLockFieldsUntilLocalEditingIsEnabled)
{
    for (const auto type : {ProjectItem::Type::Sound, ProjectItem::Type::Container,
                           ProjectItem::Type::Layer, ProjectItem::Type::Track, ProjectItem::Type::Piece}) {
        auto* item = ProjectManager::instance().createItem(parent,
            QStringLiteral("Definition%1").arg(static_cast<int>(type)), type);
        ASSERT_NE(item, nullptr);
        InspectorPanel inspector;
        PropertyEditorEngine definitionEngine;
        std::unique_ptr<QWidget> definitionPage;
        QWidget* page = &inspector;
        if (ItemTypeCatalog::isUserFacingType(type)) {
            inspector.showItemInspector(item);
        } else {
            definitionEngine.setItemType(type);
            definitionEngine.setSchema(contentObjectInspectorRows(type), ComponentType::Audio);
            definitionPage.reset(definitionEngine.buildPage());
            definitionEngine.bindItem(item);
            page = definitionPage.get();
        }
        QStringList sections;
        for (auto* section : page->findChildren<CollapsibleSection*>()) sections.append(section->title());
        for (const auto* group : {"mainOutput", "send", "spatialRating"}) {
            auto* check = page->findChild<QCheckBox*>(QStringLiteral("inherit_") + group);
            ASSERT_NE(check, nullptr) << static_cast<int>(type) << "/" << group
                << " context=" << inspector.contextType().toStdString()
                << " sections=" << sections.join(QLatin1Char(',')).toStdString();
            EXPECT_EQ(check->checkState(), Qt::Checked);
            EXPECT_TRUE(check->isEnabled());
        }
        EXPECT_FALSE(page->findChild<ParameterSlider*>("prop_spatialPanning")->isEnabled());
        EXPECT_TRUE(page->findChild<ObjectRefWidget*>("prop_mainOutputBusRef")->isReadOnly());
        page->findChild<QCheckBox*>("inherit_spatialRating")->click();
        auto* panning = page->findChild<ParameterSlider*>("prop_spatialPanning");
        ASSERT_NE(panning, nullptr);
        EXPECT_TRUE(panning->isEnabled());
        panning->simulateValueChange(0.25);
        ASSERT_TRUE(item->fieldGroups.spatialRating.shared.localHistory);
        EXPECT_FLOAT_EQ(item->fieldGroups.spatialRating.shared.localHistory->spatialPanning, 0.25f);
    }
}

TEST_F(FieldGroupInspectorTest, CumulativeOutputGainDisplaysWithoutClampingOrChangingTheProject)
{
    for (const auto gain : {48.0, -160.0, 0.0}) {
        field(parent, "mainOutput", "outputBusVolume", gain);
        const auto before = sound->fieldGroups;
        panel->showItemInspector(sound);
        auto* control = slider("outputBusVolume");
        ASSERT_NE(control, nullptr);
        EXPECT_DOUBLE_EQ(control->value(), gain);
        EXPECT_LE(control->minimum(), gain);
        EXPECT_GE(control->maximum(), gain);
        EXPECT_EQ(sound->fieldGroups, before);
    }
}

TEST_F(FieldGroupInspectorTest, MovingFieldsBetweenSectionsPreservesTheirInheritanceRange)
{
    auto schema = contentObjectInspectorRows(ProjectItem::Type::Sound);
    for (auto& field : schema)
        if (field.field == QLatin1String("mainOutputBusRef"))
            field.section = PropertySection::Rating;
    const auto before = sound->fieldGroups;
    PropertyEditorEngine engine;
    engine.setItemType(ProjectItem::Type::Sound);
    engine.setSchema(schema, ComponentType::Audio);
    std::unique_ptr<QWidget> page(engine.buildPage());
    engine.bindItem(sound);
    auto controls = page->findChildren<QCheckBox*>(QStringLiteral("inherit_mainOutput"));
    ASSERT_EQ(controls.size(), 2);
    EXPECT_EQ(sound->fieldGroups, before);
    controls.front()->click();
    EXPECT_EQ(sound->fieldGroups.mainOutput.shared.mode, FieldGroupMode::Local);
    EXPECT_EQ(sound->fieldGroups.send, before.send);
    EXPECT_EQ(sound->fieldGroups.spatialRating, before.spatialRating);
    for (auto* control : controls) EXPECT_FALSE(control->isChecked());
    EXPECT_FALSE(page->findChild<ObjectRefWidget*>("prop_mainOutputBusRef")->isReadOnly());
    EXPECT_TRUE(page->findChild<ParameterSlider*>("prop_outputBusVolume")->isEnabled());
    controls.back()->click();
    EXPECT_EQ(sound->fieldGroups.mainOutput.shared.mode, FieldGroupMode::Inherit);
    for (auto* control : controls) EXPECT_TRUE(control->isChecked());
}

TEST_F(FieldGroupInspectorTest, RootGroupsPreserveCheckedInheritAndAllowFieldEdits)
{
    panel->showItemInspector(parent);
    for (const auto* group : {"mainOutput", "send", "spatialRating"}) {
        EXPECT_TRUE(inherit(group)->isChecked());
        EXPECT_FALSE(inherit(group)->isEnabled());
    }
    EXPECT_TRUE(slider("spatialPanning")->isEnabled());
    EXPECT_FALSE(ref("mainOutputBusRef")->isReadOnly());
    slider("spatialPanning")->simulateValueChange(0.25);
    ASSERT_TRUE(parent->fieldGroups.spatialRating.shared.localHistory);
    EXPECT_FLOAT_EQ(parent->fieldGroups.spatialRating.shared.localHistory->spatialPanning, 0.25f);
    EXPECT_EQ(parent->fieldGroups.spatialRating.shared.mode, FieldGroupMode::Inherit);
    EXPECT_TRUE(inherit("spatialRating")->isChecked());
    openMenu("spatialRating");
    auto* popup = panel->findChild<QMenu*>("fieldGroupMenu_spatialRating");
    ASSERT_NE(popup, nullptr);
    auto* rebuild = popup->findChild<QAction*>("rebuildLocal");
    ASSERT_NE(rebuild, nullptr);
    EXPECT_FALSE(rebuild->isEnabled());
    popup->close();
}

TEST_F(FieldGroupInspectorTest, MovingInheritedDefinitionToRootPreservesItsStateAndCurrentValues)
{
    field(parent, "spatialRating", "spatial.panning", 0.7);
    field(parent, "mainOutput", "mainOutputBusRef", busA->guid.toString(QUuid::WithoutBraces));
    EXPECT_TRUE(inherit("spatialRating")->isChecked());
    EXPECT_FALSE(slider("spatialPanning")->isEnabled());
    EXPECT_NEAR(slider("spatialPanning")->value(), 0.7, 0.001);

    ASSERT_TRUE(ProjectManager::instance().moveItemToBucketRoot(sound, QStringLiteral("Layers")));
    for (const auto* group : {"mainOutput", "send", "spatialRating"}) {
        EXPECT_TRUE(inherit(group)->isChecked());
        EXPECT_FALSE(inherit(group)->isEnabled());
    }
    EXPECT_TRUE(slider("spatialPanning")->isEnabled());
    EXPECT_FALSE(ref("mainOutputBusRef")->isReadOnly());
    EXPECT_NEAR(slider("spatialPanning")->value(), 0.7, 0.001);
    EXPECT_EQ(ref("mainOutputBusRef")->currentObjectID(), busA->objectID);
    field(parent, "spatialRating", "spatial.panning", 0.9);
    EXPECT_NEAR(slider("spatialPanning")->value(), 0.7, 0.001);
    slider("spatialPanning")->simulateValueChange(-0.2);
    EXPECT_EQ(sound->fieldGroups.spatialRating.shared.mode, FieldGroupMode::Inherit);
    EXPECT_FLOAT_EQ(sound->fieldGroups.spatialRating.shared.localHistory->spatialPanning, -0.2f);
    EXPECT_TRUE(inherit("spatialRating")->isChecked());
}

TEST_F(FieldGroupInspectorTest, MovingLocalDefinitionToRootPreservesUncheckedInherit)
{
    inherit("spatialRating")->click();
    slider("spatialPanning")->simulateValueChange(0.4);
    ASSERT_TRUE(ProjectManager::instance().moveItemToBucketRoot(sound, QStringLiteral("Layers")));
    EXPECT_FALSE(inherit("spatialRating")->isChecked());
    EXPECT_FALSE(inherit("spatialRating")->isEnabled());
    EXPECT_TRUE(slider("spatialPanning")->isEnabled());
    EXPECT_NEAR(slider("spatialPanning")->value(), 0.4, 0.001);
}

TEST_F(FieldGroupInspectorTest, PlatformSelectionKeepsGroupedAndCustomSlotsAligned)
{
    auto* container = ProjectManager::instance().createItem(
        parent, QStringLiteral("Container"), ProjectItem::Type::Container);
    ASSERT_NE(container, nullptr);
    panel->showItemInspector(container);
    auto* volume = widget<QWidget>(QStringLiteral("inspectorFieldGutter_volume"));
    ASSERT_NE(volume, nullptr);
    for (const auto* name : {"inspectorFieldGutter_outputBusVolume",
                             "inspectorFieldGutter_sends",
                             "inspectorFieldGutter_spatialPanning",
                             "selectionFormGutter_mode"}) {
        auto* gutter = widget<QWidget>(QLatin1String(name));
        ASSERT_NE(gutter, nullptr) << name;
        EXPECT_EQ(gutter->width(), volume->width()) << name;
    }
    auto* spatial = widget<QWidget>(QStringLiteral("inspectorFieldGutter_spatialPanning"));
    ASSERT_NE(spatial, nullptr);
    EXPECT_EQ(spatial->findChild<QWidget*>(QStringLiteral("platform_spatialPanning")), nullptr);
}

TEST_F(FieldGroupInspectorTest, AttenuationBarsFollowInheritedReferenceAndSavedPitchCurve)
{
    auto& manager = ProjectManager::instance();
    auto& actions = ActionManager::instance();
    registerSpatialActions(actions, nullptr, &manager);
    AttenuationSnapshot definition;
    definition.guid = QUuid::createUuid();
    definition.name = QStringLiteral("SlotAttenuation");
    auto& volumeCurve = definition.ensureDimension(AttenuationDimension::Volume);
    volumeCurve.curve = attenuationDefaultCurve(AttenuationDimension::Volume, definition.maxDistance);
    attenuationNormalizeCurve(volumeCurve.type, volumeCurve.curve, volumeCurve.handles);
    ASSERT_NE(AttenuationStore::create(manager.currentProject(), definition, &manager), nullptr);
    auto* volume = widget<AttenuationFieldDecorator>("attenuationPoint_volume");
    auto* pitch = widget<AttenuationFieldDecorator>("attenuationPoint_pitch");
    ASSERT_NE(volume, nullptr);
    ASSERT_NE(pitch, nullptr);
    EXPECT_FALSE(volume->isBound());
    EXPECT_FALSE(pitch->isBound());

    field(parent, "spatialRating", "spatial.attenuationRef",
          definition.guid.toString(QUuid::WithoutBraces));
    EXPECT_TRUE(volume->isBound());
    EXPECT_FALSE(pitch->isBound());
    EXPECT_TRUE(volume->isEnabled());
    EXPECT_TRUE(pitch->isEnabled());
    auto* variable = widget<BindingFieldDecorator>("bindingPoint_volume");
    ASSERT_NE(variable, nullptr);
    EXPECT_TRUE(variable->isEnabled());
    EXPECT_TRUE(volume->toolTip().contains(definition.name));
    const auto result = actions.execute("add_attenuation_dimension",
        { {"attenuation_guid", definition.guid.toString(QUuid::WithoutBraces)},
          {"dimension", "pitch"} });
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    EXPECT_TRUE(pitch->isBound());
    EXPECT_TRUE(pitch->isEnabled());
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_FALSE(pitch->isBound());
    EXPECT_TRUE(volume->isBound());
    UndoRedoManager::instance().undoStack()->redo();
    EXPECT_TRUE(pitch->isBound());
    ASSERT_TRUE(AttenuationStore::rename(manager.currentProject(), definition.name,
                                       QStringLiteral("RenamedAttenuation"), &manager));
    field(parent, "spatialRating", "spatial.attenuationRef", QUuid().toString(QUuid::WithoutBraces));
    EXPECT_FALSE(volume->isBound());
    EXPECT_FALSE(pitch->isBound());
}

TEST_F(FieldGroupInspectorTest, AttenuationBarsFollowSelectedPlatformsAndUndo)
{
    auto& manager = ProjectManager::instance();
    AttenuationSnapshot definition;
    definition.guid = QUuid::createUuid();
    definition.name = QStringLiteral("PlatformAttenuation");
    for (auto dimension : {AttenuationDimension::Volume, AttenuationDimension::Pitch}) {
        auto& curve = definition.ensureDimension(dimension);
        curve.curve = attenuationDefaultCurve(dimension, definition.maxDistance);
        attenuationNormalizeCurve(dimension, curve.curve, curve.handles);
    }
    ASSERT_NE(AttenuationStore::create(manager.currentProject(), definition, &manager), nullptr);
    mode(sound, "spatialRating", "Local");
    field(sound, "spatialRating", "spatial.attenuationRef",
          definition.guid.toString(QUuid::WithoutBraces), "Windows");
    auto* volume = widget<AttenuationFieldDecorator>("attenuationPoint_volume");
    auto* pitch = widget<AttenuationFieldDecorator>("attenuationPoint_pitch");
    ASSERT_NE(volume, nullptr);
    ASSERT_NE(pitch, nullptr);
    EXPECT_TRUE(volume->isBound());
    EXPECT_TRUE(pitch->isBound());
    auto& platforms = PlatformContextService::instance();
    platforms.setSelected({HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Android, false);
    EXPECT_FALSE(volume->isBound());
    EXPECT_FALSE(pitch->isBound());
    platforms.setSelected({HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
    EXPECT_TRUE(volume->isBound());
    EXPECT_TRUE(pitch->isBound());
    platforms.setAll(true);
    field(sound, "spatialRating", "spatial.attenuationRef", QUuid().toString(QUuid::WithoutBraces));
    EXPECT_FALSE(volume->isBound());
    EXPECT_FALSE(pitch->isBound());
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_TRUE(volume->isBound());
    EXPECT_TRUE(pitch->isBound());
    EXPECT_FALSE(volume->isEnabled());
    EXPECT_FALSE(pitch->isEnabled());
}

TEST_F(FieldGroupInspectorTest, AttenuationSlotAddsOnlyItsDimensionThenOpensExternalEditor)
{
    auto& manager = ProjectManager::instance();
    registerSpatialActions(ActionManager::instance(), nullptr, &manager);
    AttenuationSnapshot definition;
    definition.guid = QUuid::createUuid();
    definition.name = QStringLiteral("SlotTarget");
    ASSERT_NE(AttenuationStore::create(manager.currentProject(), definition, &manager), nullptr);
    auto* pitch = widget<AttenuationFieldDecorator>("attenuationPoint_pitch");
    ASSERT_NE(pitch, nullptr);
    EXPECT_FALSE(pitch->isEnabled());
    field(parent, "spatialRating", "spatial.attenuationRef", definition.guid.toString(QUuid::WithoutBraces));
    EXPECT_TRUE(pitch->isEnabled());
    EXPECT_FALSE(pitch->isBound());
    std::vector<ExternalEditorSelection> requests;
    QObject::connect(panel.get(), &InspectorPanel::externalEditorRequested,
        [&requests](const ExternalEditorSelection& selection, bool) { requests.push_back(selection); });
    pitch->click();
    auto* menu = pitch->findChild<QMenu*>(QStringLiteral("attenuationSlotAddMenu"));
    ASSERT_NE(menu, nullptr);
    ASSERT_EQ(menu->actions().size(), 1);
    EXPECT_TRUE(requests.empty());
    menu->actions().front()->trigger();
    menu->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    const auto* after = AttenuationStore::findByGuid(manager.currentProject(), definition.guid);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->dimension(AttenuationDimension::Volume), nullptr);
    ASSERT_NE(after->dimension(AttenuationDimension::Pitch), nullptr);
    EXPECT_TRUE(pitch->isBound());
    EXPECT_TRUE(requests.empty());
    pitch->click();
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests.front().ownerGuid, sound->guid);
    EXPECT_EQ(requests.front().curve.attenuationGuid, definition.guid);
    EXPECT_EQ(requests.front().curve.dimension, AttenuationDimension::Pitch);
    EXPECT_EQ(pitch->findChild<QMenu*>(), nullptr);
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_FALSE(pitch->isBound());
    EXPECT_TRUE(pitch->isEnabled());
    UndoRedoManager::instance().undoStack()->redo();
    EXPECT_TRUE(pitch->isBound());
}

TEST_F(FieldGroupInspectorTest, UnpinnedAttenuationEditorFollowsReferenceAndPinnedEditorRetainsTarget)
{
    auto& manager = ProjectManager::instance();
    AttenuationSnapshot first;
    first.guid = QUuid::createUuid();
    first.name = QStringLiteral("FirstTarget");
    first.ensureDimension(AttenuationDimension::Volume);
    AttenuationSnapshot second = first;
    second.guid = QUuid::createUuid();
    second.name = QStringLiteral("SecondTarget");
    ASSERT_NE(AttenuationStore::create(manager.currentProject(), first, &manager), nullptr);
    ASSERT_NE(AttenuationStore::create(manager.currentProject(), second, &manager), nullptr);
    field(parent, "spatialRating", "spatial.attenuationRef", first.guid.toString(QUuid::WithoutBraces));
    ExternalEditorPanel follow;
    ExternalEditorPanel pinned;
    const auto selection = ExternalEditorSelection::fromCurve(CurveSelection::attenuation(
        first.guid, first.name, AttenuationDimension::Volume), sound->guid);
    follow.bindSelection(selection);
    pinned.bindSelection(selection);
    pinned.setPinned(true);
    EXPECT_TRUE(follow.acceptsSelection(sound));
    EXPECT_FALSE(follow.acceptsSelection(other));
    field(parent, "spatialRating", "spatial.attenuationRef", second.guid.toString(QUuid::WithoutBraces));
    QApplication::processEvents();
    EXPECT_EQ(follow.selection().curve.attenuationGuid, second.guid);
    EXPECT_EQ(pinned.selection().curve.attenuationGuid, first.guid);
    EXPECT_EQ(follow.boundItem(), sound);
    ExternalEditorPanel restored;
    restored.restoreState(pinned.snapshotState());
    EXPECT_TRUE(restored.isPinned());
    EXPECT_EQ(restored.selection().curve.attenuationGuid, first.guid);
    field(parent, "spatialRating", "spatial.attenuationRef", QUuid().toString(QUuid::WithoutBraces));
    QApplication::processEvents();
    EXPECT_FALSE(follow.selection().curve.hasAttenuation());
    EXPECT_EQ(follow.findChild<QStackedWidget*>()->currentIndex(), 0);
    EXPECT_EQ(pinned.selection().curve.attenuationGuid, first.guid);
    UndoRedoManager::instance().undoStack()->undo();
    QApplication::processEvents();
    EXPECT_EQ(follow.selection().curve.attenuationGuid, second.guid);
    pinned.setPinned(false);
    EXPECT_EQ(pinned.selection().curve.attenuationGuid, second.guid);
}

TEST_F(FieldGroupInspectorTest, MixedAttenuationReferencesCannotOpenOrAddADimension)
{
    auto& manager = ProjectManager::instance();
    for (const auto& platform : {QStringLiteral("Windows"), QStringLiteral("Android")}) {
        AttenuationSnapshot definition;
        definition.guid = QUuid::createUuid();
        definition.name = platform + QStringLiteral("Target");
        ASSERT_NE(AttenuationStore::create(manager.currentProject(), definition, &manager), nullptr);
        field(parent, "spatialRating", "spatial.attenuationRef",
              definition.guid.toString(QUuid::WithoutBraces), platform);
    }
    auto* pitch = widget<AttenuationFieldDecorator>("attenuationPoint_pitch");
    ASSERT_NE(pitch, nullptr);
    EXPECT_FALSE(pitch->isEnabled());
    pitch->click();
    EXPECT_EQ(pitch->findChild<QMenu*>(), nullptr);
    PlatformContextService::instance().setSelected({HarkenTest::PlatformIds::Windows},
        HarkenTest::PlatformIds::Windows, false);
    EXPECT_TRUE(pitch->isEnabled());
    EXPECT_FALSE(pitch->isBound());
}

TEST_F(FieldGroupInspectorTest, CollapsedHeaderClickAndSpaceDoNotExpandSection)
{
    auto* check = inherit("spatialRating");
    auto* section = check->parentWidget()->parentWidget();
    auto* collapsible = qobject_cast<CollapsibleSection*>(section);
    ASSERT_NE(collapsible, nullptr);
    collapsible->setExpanded(false);
    check->click();
    EXPECT_FALSE(collapsible->isExpanded());
    EXPECT_EQ(sound->fieldGroups.spatialRating.shared.mode, FieldGroupMode::Local);
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
    QApplication::sendEvent(check, &press);
    QApplication::sendEvent(check, &release);
    EXPECT_FALSE(collapsible->isExpanded());
    EXPECT_EQ(sound->fieldGroups.spatialRating.shared.mode, FieldGroupMode::Inherit);
}

TEST_F(FieldGroupInspectorTest, GroupContextMenuContainsOnlyRebuildAndPlatformReset)
{
    openMenu("mainOutput");
    auto* popup = panel->findChild<QMenu*>("fieldGroupMenu_mainOutput");
    ASSERT_NE(popup, nullptr);
    EXPECT_NE(popup->findChild<QAction*>("rebuildLocal"), nullptr);
    EXPECT_EQ(popup->findChild<QAction*>("UseShared"), nullptr);
    EXPECT_EQ(popup->findChild<QAction*>("Local"), nullptr);
    EXPECT_EQ(popup->findChild<QAction*>("Inherit"), nullptr);
    popup->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    PlatformContextService::instance().setSelected({HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
    openMenu("mainOutput");
    popup = panel->findChild<QMenu*>("fieldGroupMenu_mainOutput");
    ASSERT_NE(popup, nullptr);
    auto* reset = popup->findChild<QAction*>("UseShared");
    ASSERT_NE(reset, nullptr);
    EXPECT_NE(popup->findChild<QAction*>("rebuildLocal"), nullptr);
    EXPECT_EQ(popup->findChild<QAction*>("Local"), nullptr);
    EXPECT_EQ(popup->findChild<QAction*>("Inherit"), nullptr);
    popup->close();
}

TEST_F(FieldGroupInspectorTest, LocalHistorySurvivesInheritAndUndo)
{
    inherit("spatialRating")->click();
    slider("spatialPanning")->binding()->commitValue(0.35);
    ASSERT_TRUE(sound->fieldGroups.spatialRating.shared.localHistory);
    EXPECT_FLOAT_EQ(sound->fieldGroups.spatialRating.shared.localHistory->spatialPanning, 0.35f);
    inherit("spatialRating")->click();
    EXPECT_FALSE(slider("spatialPanning")->isEnabled());
    EXPECT_DOUBLE_EQ(slider("spatialPanning")->value(), 0.0);
    inherit("spatialRating")->click();
    EXPECT_NEAR(slider("spatialPanning")->value(), 0.35, 0.001);
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_TRUE(inherit("spatialRating")->isChecked());
    UndoRedoManager::instance().undoStack()->redo();
    EXPECT_NEAR(slider("spatialPanning")->value(), 0.35, 0.001);
}

TEST_F(FieldGroupInspectorTest, AncestorChangesRefreshValuesWithoutUnlockingChildren)
{
    field(parent, "spatialRating", "spatial.panning", 0.7);
    field(parent, "spatialRating", "spatial.is3DEnabled", true);
    EXPECT_NEAR(slider("spatialPanning")->value(), 0.7, 0.001);
    EXPECT_TRUE(widget<QCheckBox>("prop_spatialIs3DEnabled")->isChecked());
    EXPECT_FALSE(widget<QCheckBox>("prop_spatialIs3DEnabled")->isEnabled());
    EXPECT_FALSE(slider("spatialPanning")->isEnabled());
    EXPECT_TRUE(inherit("spatialRating")->toolTip().contains(parent->name));
    const auto before = sound->fieldGroups;
    slider("spatialPanning")->binding()->commitValue(-0.9);
    widget<QCheckBox>("prop_spatialIs3DEnabled")->click();
    ref("spatialAttenuationRef")->objectChanged(busA->objectID);
    EXPECT_EQ(sound->fieldGroups, before);
}

TEST_F(FieldGroupInspectorTest, FieldEditAndFieldResetLeaveInheritAndOtherMembersIndependent)
{
    mode(sound, "spatialRating", "Local");
    field(sound, "spatialRating", "spatial.panning", 0.2);
    field(sound, "spatialRating", "spatial.attenuationDistanceScaling", 2.0);
    PlatformContextService::instance().setSelected({HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
    EXPECT_FALSE(inherit("spatialRating")->isChecked());
    EXPECT_TRUE(slider("spatialPanning")->isEnabled());
    EXPECT_TRUE(sound->fieldGroups.spatialRating.platforms.empty());
    const auto before = sound->fieldGroups;
    const int depth = UndoRedoManager::instance().undoStack()->count();
    slider("spatialPanning")->binding()->commitValue(-0.4);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), depth + 1);
    EXPECT_FLOAT_EQ(sound->fieldGroups.spatialRating.shared.localHistory->spatialPanning, 0.2f);
    EXPECT_FLOAT_EQ(sound->fieldGroups.spatialRating.platforms.at(HarkenTest::FieldGroupPlatformIds::Windows).localHistory->spatialPanning, -0.4f);
    EXPECT_FLOAT_EQ(ResolveFieldGroupLocal(sound->fieldGroups.spatialRating, HarkenTest::FieldGroupPlatformIds::Windows).localHistory->spatialAttenuationDistanceScaling, 2.0f);
    EXPECT_FALSE(sound->fieldGroups.spatialRating.platforms.contains(HarkenTest::FieldGroupPlatformIds::Android));
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(sound->fieldGroups, before);
    UndoRedoManager::instance().undoStack()->redo();
    menu("spatialRating", "UseShared");
    EXPECT_TRUE(slider("spatialPanning")->isEnabled());
    EXPECT_NEAR(slider("spatialPanning")->value(), -0.4, 0.001);
    EXPECT_EQ(sound->fieldGroups.spatialRating.platforms.at(HarkenTest::FieldGroupPlatformIds::Windows).localFields,
              FieldGroupFieldMask(harkenplayableproperties::PlayablePropertyIdV1::SpatialPanning));
    auto resetArgs = args(sound, "spatialRating", "Windows");
    resetArgs.insert("field", "spatial.panning");
    const auto result = ActionManager::instance().execute(
        "clear_field_group_field_override", resetArgs);
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    EXPECT_NEAR(slider("spatialPanning")->value(), 0.2, 0.001);
    EXPECT_TRUE(sound->fieldGroups.spatialRating.platforms.empty());
}

TEST_F(FieldGroupInspectorTest, AllUnifiesOnlyTheInheritanceFlagBeforeEnablingFields)
{
    mode(sound, "spatialRating", "Inherit", "Windows");
    selectPlatforms();
    EXPECT_EQ(inherit("spatialRating")->checkState(), Qt::Checked);
    mode(sound, "spatialRating", "Local", "Windows");
    EXPECT_EQ(inherit("spatialRating")->checkState(), Qt::PartiallyChecked);
    EXPECT_FALSE(slider("spatialPanning")->isEnabled());
    inherit("spatialRating")->click();
    EXPECT_EQ(inherit("spatialRating")->checkState(), Qt::Checked);
    EXPECT_FALSE(slider("spatialPanning")->isEnabled());
    inherit("spatialRating")->click();
    EXPECT_TRUE(sound->fieldGroups.spatialRating.platforms.empty());
    EXPECT_FALSE(sound->fieldGroups.spatialRating.platforms.contains(HarkenTest::FieldGroupPlatformIds::Android));
    EXPECT_EQ(sound->fieldGroups.spatialRating.shared.mode, FieldGroupMode::Local);
    EXPECT_TRUE(slider("spatialPanning")->isEnabled());
}

TEST_F(FieldGroupInspectorTest, AllScalarEditClearsOnlyThatFieldsDifferences)
{
    mode(sound, "spatialRating", "Local");
    field(sound, "spatialRating", "spatial.attenuationDistanceScaling", 4.0);
    for (const auto* platform : {"Windows", "Android"}) mode(sound, "spatialRating", "Local", platform);
    field(sound, "spatialRating", "spatial.panning", 0.4, "Windows");
    field(sound, "spatialRating", "spatial.attenuationDistanceScaling", 2.0, "Windows");
    field(sound, "spatialRating", "spatial.panning", -0.4, "Android");
    field(sound, "spatialRating", "spatial.attenuationDistanceScaling", 3.0, "Android");
    selectPlatforms();
    EXPECT_TRUE(slider("spatialPanning")->isEnabled());
    EXPECT_TRUE(slider("spatialPanning")->mixedValue());
    slider("spatialPanning")->binding()->commitValue(0.15);
    const auto windows = *ResolveFieldGroupLocal(sound->fieldGroups.spatialRating, HarkenTest::FieldGroupPlatformIds::Windows).localHistory;
    const auto android = *ResolveFieldGroupLocal(sound->fieldGroups.spatialRating, HarkenTest::FieldGroupPlatformIds::Android).localHistory;
    EXPECT_FLOAT_EQ(windows.spatialPanning, 0.15f);
    EXPECT_FLOAT_EQ(android.spatialPanning, 0.15f);
    EXPECT_FLOAT_EQ(windows.spatialAttenuationDistanceScaling, 2.0f);
    EXPECT_FLOAT_EQ(android.spatialAttenuationDistanceScaling, 3.0f);
    EXPECT_FLOAT_EQ(sound->fieldGroups.spatialRating.shared.localHistory->spatialPanning, 0.15f);
    EXPECT_FLOAT_EQ(sound->fieldGroups.spatialRating.shared.localHistory->spatialAttenuationDistanceScaling, 4.0f);
}

TEST_F(FieldGroupInspectorTest, AllReferenceClearPreservesIndependentOutputVolumes)
{
    mode(sound, "mainOutput", "Local");
    field(sound, "mainOutput", "mainOutputBusRef", busA->guid.toString(QUuid::WithoutBraces));
    field(sound, "mainOutput", "outputBusVolume", -9.0);
    for (const auto* platform : {"Windows", "Android"}) mode(sound, "mainOutput", "Local", platform);
    field(sound, "mainOutput", "mainOutputBusRef", busA->guid.toString(QUuid::WithoutBraces), "Windows");
    field(sound, "mainOutput", "mainOutputBusRef", busB->guid.toString(QUuid::WithoutBraces), "Android");
    field(sound, "mainOutput", "outputBusVolume", -3.0, "Windows");
    field(sound, "mainOutput", "outputBusVolume", -6.0, "Android");
    selectPlatforms();
    EXPECT_FALSE(ref("mainOutputBusRef")->isReadOnly());
    ref("mainOutputBusRef")->objectChanged(0);
    EXPECT_EQ(ref("mainOutputBusRef")->currentObjectID(), master->objectID);
    const auto windows = *ResolveFieldGroupLocal(sound->fieldGroups.mainOutput, HarkenTest::FieldGroupPlatformIds::Windows).localHistory;
    const auto android = *ResolveFieldGroupLocal(sound->fieldGroups.mainOutput, HarkenTest::FieldGroupPlatformIds::Android).localHistory;
    EXPECT_EQ(windows.mainOutputBusRef, toFieldGroupGuid(master->guid));
    EXPECT_EQ(android.mainOutputBusRef, toFieldGroupGuid(master->guid));
    EXPECT_FLOAT_EQ(windows.outputBusVolume, -3.0f);
    EXPECT_FLOAT_EQ(android.outputBusVolume, -6.0f);
    EXPECT_EQ(sound->fieldGroups.mainOutput.shared.localHistory->mainOutputBusRef, toFieldGroupGuid(master->guid));
    EXPECT_FLOAT_EQ(sound->fieldGroups.mainOutput.shared.localHistory->outputBusVolume, -9.0f);
}

TEST_F(FieldGroupInspectorTest, AllNumericCommitUnifiesTheEditedFieldEvenWhenEqualToShared)
{
    mode(sound, "spatialRating", "Local", "Windows");
    mode(sound, "spatialRating", "Local", "Android");
    field(sound, "spatialRating", "spatial.panning", 0.5, "Android");
    mode(sound, "spatialRating", "Local");
    selectPlatforms();
    const double cached = slider("spatialPanning")->value();
    ASSERT_DOUBLE_EQ(cached, 0.0);
    slider("spatialPanning")->simulateValueChange(cached);
    EXPECT_TRUE(sound->fieldGroups.spatialRating.platforms.empty());
    EXPECT_FLOAT_EQ(sound->fieldGroups.spatialRating.shared.localHistory->spatialPanning, 0.0f);
}

TEST_F(FieldGroupInspectorTest, ReferencePickerFiltersUseSelectedGroupScopes)
{
    mode(sound, "send", "Local");
    auto command = args(sound, "send");
    command.insert("sends", QJsonArray{QJsonObject{{"targetBusID", busA->guid.toString(QUuid::WithoutBraces)}, {"sendLevel", -5.0}}});
    ASSERT_TRUE(ActionManager::instance().execute("replace_field_group_sends", command).success);
    mode(sound, "mainOutput", "Local", "Windows");
    mode(sound, "send", "Local", "Windows");
    command = args(sound, "send", "Windows");
    command.insert("sends", QJsonArray{});
    ASSERT_TRUE(ActionManager::instance().execute("replace_field_group_sends", command).success);
    PlatformContextService::instance().setSelected({HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
    EXPECT_TRUE(ref("mainOutputBusRef")->canAcceptObject(busA->objectID));
    selectPlatforms();
    mode(sound, "mainOutput", "Local");
    EXPECT_FALSE(ref("mainOutputBusRef")->canAcceptObject(busA->objectID));
    EXPECT_TRUE(ref("mainOutputBusRef")->canAcceptObject(busB->objectID));
}

TEST_F(FieldGroupInspectorTest, SendPickersExcludeOtherRowsAndKeepCurrentTarget)
{
    mode(sound, "send", "Local");
    auto command = args(sound, "send");
    command.insert("sends", QJsonArray{
        QJsonObject{{"targetBusID", busA->guid.toString(QUuid::WithoutBraces)}, {"sendLevel", -5.0}},
        QJsonObject{{"targetBusID", busB->guid.toString(QUuid::WithoutBraces)}, {"sendLevel", -8.0}}});
    ASSERT_TRUE(ActionManager::instance().execute("replace_field_group_sends", command).success);
    auto* unused = ProjectManager::instance().createItem(master, QStringLiteral("UnusedBus"), ProjectItem::Type::Bus);
    ASSERT_NE(unused, nullptr);
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    auto* sends = widget<QWidget>("prop_sends");
    auto references = sends->findChildren<ObjectRefWidget*>("prop_targetBusID");
    ASSERT_EQ(references.size(), 2);
    for (auto* reference : references) {
        EXPECT_TRUE(reference->canAcceptObject(reference->currentObjectID()));
        EXPECT_FALSE(reference->canAcceptObject(reference->currentObjectID() == busA->objectID
            ? busB->objectID : busA->objectID));
        EXPECT_FALSE(reference->canAcceptObject(master->objectID));
        EXPECT_TRUE(reference->canAcceptObject(unused->objectID));
    }
    const auto openSelector = []() -> SelectorWindow* {
        for (auto* widget : QApplication::topLevelWidgets())
            if (auto* selector = qobject_cast<SelectorWindow*>(widget); selector && selector->isVisible())
                return selector;
        return nullptr;
    };
    references.front()->openSelector();
    auto* selector = openSelector();
    ASSERT_NE(selector, nullptr);
    auto* tree = selector->findChild<QTreeWidget*>();
    ASSERT_NE(tree, nullptr);
    ASSERT_NE(tree->currentItem(), nullptr);
    EXPECT_EQ(tree->currentItem()->data(0, kRoleObjectID).toUInt(), references.front()->currentObjectID());
    const int undoCount = UndoRedoManager::instance().undoStack()->count();
    selector->accept();
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), undoCount);

    auto* add = sends->findChild<QToolButton*>("listAdd_sends");
    ASSERT_NE(add, nullptr);
    add->click();
    selector = openSelector();
    ASSERT_NE(selector, nullptr);
    tree = selector->findChild<QTreeWidget*>();
    ASSERT_NE(tree, nullptr);
    for (auto* target : {busA, busB, unused}) {
        const auto rows = tree->findItems(target->name, Qt::MatchExactly | Qt::MatchRecursive);
        ASSERT_EQ(rows.size(), 1);
        EXPECT_EQ(rows.front()->data(0, kRoleSelectable).toBool(), target == unused);
        if (target == unused) tree->setCurrentItem(rows.front());
    }
    selector->accept();
    ASSERT_EQ(sound->fieldGroups.send.shared.localHistory->sends.size(), 3u);
    EXPECT_EQ(sound->fieldGroups.send.shared.localHistory->sends.back().targetBusID, toFieldGroupGuid(unused->guid));
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(sound->fieldGroups.send.shared.localHistory->sends.size(), 2u);
}

TEST_F(FieldGroupInspectorTest, SendRowsReorderWithUndoAndRespectInheritance)
{
    mode(sound, "send", "Local");
    auto command = args(sound, "send");
    command.insert("sends", QJsonArray{
        QJsonObject{{"targetBusID", busA->guid.toString(QUuid::WithoutBraces)}, {"sendLevel", -5.0}},
        QJsonObject{{"targetBusID", busB->guid.toString(QUuid::WithoutBraces)}, {"sendLevel", -8.0}}});
    ASSERT_TRUE(ActionManager::instance().execute("replace_field_group_sends", command).success);
    QCoreApplication::processEvents();
    auto* table = widget<QWidget>("prop_sends")->findChild<TableWidget*>();
    ASSERT_NE(table, nullptr);
    ASSERT_TRUE(table->canMoveRow(0, 1));
    const auto before = *sound->fieldGroups.send.shared.localHistory;
    UndoRedoManager::instance().clear();
    table->moveRow(0, 1);
    QCoreApplication::processEvents();
    const auto after = *sound->fieldGroups.send.shared.localHistory;
    ASSERT_EQ(after.sends.size(), 2u);
    EXPECT_EQ(after.sends[0], before.sends[1]);
    EXPECT_EQ(after.sends[1], before.sends[0]);
    ASSERT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    UndoRedoManager::instance().undo();
    QCoreApplication::processEvents();
    EXPECT_EQ(*sound->fieldGroups.send.shared.localHistory, before);
    UndoRedoManager::instance().redo();
    QCoreApplication::processEvents();
    EXPECT_EQ(*sound->fieldGroups.send.shared.localHistory, after);
    mode(sound, "send", "Inherit");
    QCoreApplication::processEvents();
    table = widget<QWidget>("prop_sends")->findChild<TableWidget*>();
    ASSERT_NE(table, nullptr);
    EXPECT_FALSE(table->canMoveRow(0, 1));
}

TEST_F(FieldGroupInspectorTest, AllPlatformsSendShowsSharedTableAndSinglePlatformEditsItsOwn)
{
    for (const auto* platform : {"Windows", "Android"}) mode(sound, "send", "Local", platform);
    auto command = args(sound, "send", "Windows");
    command.insert("sends", QJsonArray{QJsonObject{{"targetBusID", busA->guid.toString(QUuid::WithoutBraces)}, {"sendLevel", -5.0}}});
    ASSERT_TRUE(ActionManager::instance().execute("replace_field_group_sends", command).success);
    mode(sound, "send", "Local");
    selectPlatforms();
    auto* list = widget<QWidget>("prop_sends");
    EXPECT_EQ(list->findChild<ParameterSlider*>(), nullptr);
    PlatformContextService::instance().setSelected({HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
    auto* sendLevel = widget<QWidget>("prop_sends")->findChild<ParameterSlider*>();
    ASSERT_NE(sendLevel, nullptr);
    sendLevel->binding()->commitValue(-8.0);
    EXPECT_FLOAT_EQ(sound->fieldGroups.send.platforms.at(HarkenTest::FieldGroupPlatformIds::Windows).localHistory->sends.front().sendLevel, -8.0f);
    EXPECT_TRUE(ResolveFieldGroupLocal(sound->fieldGroups.send, HarkenTest::FieldGroupPlatformIds::Android).localHistory->sends.empty());
    EXPECT_TRUE(sound->fieldGroups.send.shared.localHistory->sends.empty());
}

TEST_F(FieldGroupInspectorTest, AllSendEditClearsPlatformTablesAndOneUndoRestoresThem)
{
    mode(sound, "send", "Local");
    auto sharedCommand = args(sound, "send");
    sharedCommand.insert("sends", QJsonArray{QJsonObject{{"targetBusID", busB->guid.toString(QUuid::WithoutBraces)}, {"sendLevel", -4.0}}});
    ASSERT_TRUE(ActionManager::instance().execute("replace_field_group_sends", sharedCommand).success);
    for (const auto* platform : {"Windows", "Android"}) {
        mode(sound, "send", "Local", platform);
        auto command = args(sound, "send", platform);
        command.insert("sends", QJsonArray{QJsonObject{{"targetBusID", busA->guid.toString(QUuid::WithoutBraces)}, {"sendLevel", -5.0}}});
        ASSERT_TRUE(ActionManager::instance().execute("replace_field_group_sends", command).success);
    }
    selectPlatforms();
    const int count = UndoRedoManager::instance().undoStack()->count();
    auto* level = widget<QWidget>("prop_sends")->findChild<ParameterSlider*>();
    ASSERT_NE(level, nullptr);
    level->simulateValueChange(-8.0);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), count + 1);
    EXPECT_TRUE(sound->fieldGroups.send.platforms.empty());
    EXPECT_FLOAT_EQ(sound->fieldGroups.send.shared.localHistory->sends.front().sendLevel, -8.0f);
    UndoRedoManager::instance().undoStack()->undo();
    for (const auto platform : {HarkenTest::FieldGroupPlatformIds::Windows, HarkenTest::FieldGroupPlatformIds::Android})
        EXPECT_FLOAT_EQ(sound->fieldGroups.send.platforms.at(platform).localHistory->sends.front().sendLevel, -5.0f);
    EXPECT_FLOAT_EQ(sound->fieldGroups.send.shared.localHistory->sends.front().sendLevel, -4.0f);
}

TEST_F(FieldGroupInspectorTest, UsingSharedInheritDisplaysTheRequestedPlatformParent)
{
    field(parent, "spatialRating", "spatial.panning", 0.2);
    field(parent, "spatialRating", "spatial.panning", -0.4, "Windows");
    PlatformContextService::instance().setSelected({HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
    EXPECT_TRUE(inherit("spatialRating")->isChecked());
    EXPECT_FALSE(slider("spatialPanning")->isEnabled());
    EXPECT_NEAR(slider("spatialPanning")->value(), -0.4, 0.001);
    EXPECT_TRUE(inherit("spatialRating")->toolTip().contains(parent->name));
    PlatformContextService::instance().setAll(true);
    EXPECT_TRUE(slider("spatialPanning")->mixedValue());
    EXPECT_FALSE(slider("spatialPanning")->isEnabled());
}

TEST_F(FieldGroupInspectorTest, NumericPreviewCancelsOnModeOrObjectChangesAndCommitsBeforePlatformSwitch)
{
    inherit("spatialRating")->click();
    auto* binding = slider("spatialPanning")->binding();
    binding->beginEdit();
    binding->previewEdit(0.7);
    inherit("spatialRating")->click();
    binding->commitEdit();
    EXPECT_EQ(sound->fieldGroups.spatialRating.shared.mode, FieldGroupMode::Inherit);
    EXPECT_FLOAT_EQ(sound->fieldGroups.spatialRating.shared.localHistory->spatialPanning, 0.0f);
    inherit("spatialRating")->click();
    binding->beginEdit();
    binding->previewEdit(0.6);
    panel->showItemInspector(other);
    binding->commitEdit();
    EXPECT_FALSE(other->fieldGroups.spatialRating.shared.localHistory);
    EXPECT_FLOAT_EQ(sound->fieldGroups.spatialRating.shared.localHistory->spatialPanning, 0.0f);
    panel->showItemInspector(sound);
    binding->beginEdit();
    binding->previewEdit(0.5);
    PlatformContextService::instance().setSelected({HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
    binding->commitEdit();
    EXPECT_TRUE(sound->fieldGroups.spatialRating.platforms.empty());
    EXPECT_FLOAT_EQ(sound->fieldGroups.spatialRating.shared.localHistory->spatialPanning, 0.5f);
}

TEST_F(FieldGroupInspectorTest, NumericCommitUsesOneUndoAndFrozenScope)
{
    inherit("spatialRating")->click();
    const int before = UndoRedoManager::instance().undoStack()->count();
    auto* binding = slider("spatialPanning")->binding();
    binding->beginEdit();
    binding->previewEdit(0.2);
    binding->previewEdit(0.6);
    binding->commitEdit();
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), before + 1);
    EXPECT_FLOAT_EQ(sound->fieldGroups.spatialRating.shared.localHistory->spatialPanning, 0.6f);
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_DOUBLE_EQ(slider("spatialPanning")->value(), 0.0);
}

TEST_F(FieldGroupInspectorTest, UnconfirmedNumericTextCannotLeakIntoAnotherLocalScope)
{
    mode(sound, "spatialRating", "Local", "Windows");
    mode(sound, "spatialRating", "Local", "Android");
    PlatformContextService::instance().setSelected({HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
    auto* editor = slider("spatialPanning")->presenter()->valueEditor();
    ASSERT_NE(editor, nullptr);
    auto* line = editor->findChild<QLineEdit*>();
    ASSERT_NE(line, nullptr);
    line->setText(QStringLiteral("0.75"));
    PlatformContextService::instance().setSelected({HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Android, false);
    editor->interpretText();
    EXPECT_FLOAT_EQ(sound->fieldGroups.spatialRating.platforms.at(HarkenTest::FieldGroupPlatformIds::Windows).localHistory->spatialPanning, 0.0f);
    EXPECT_FLOAT_EQ(sound->fieldGroups.spatialRating.platforms.at(HarkenTest::FieldGroupPlatformIds::Android).localHistory->spatialPanning, 0.0f);
}

TEST_F(FieldGroupInspectorTest, PendingReferencePickerIsCancelledOnGroupScopeAndObjectChange)
{
    inherit("mainOutput")->click();
    auto* output = ref("mainOutputBusRef");
    auto* pick = output->findChild<QToolButton*>("refPick");
    ASSERT_NE(pick, nullptr);
    pick->click();
    SelectorWindow* selector = nullptr;
    for (auto* widget : QApplication::topLevelWidgets())
        if (auto* candidate = qobject_cast<SelectorWindow*>(widget); candidate && candidate->isVisible())
            selector = candidate;
    ASSERT_NE(selector, nullptr);
    inherit("mainOutput")->click();
    EXPECT_FALSE(selector->isVisible());
    inherit("mainOutput")->click();
    pick->click();
    PlatformContextService::instance().setSelected({HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
    EXPECT_FALSE(selector->isVisible());
    PlatformContextService::instance().setAll(true);
    pick->click();
    panel->showItemInspector(other);
    EXPECT_FALSE(selector->isVisible());
    EXPECT_EQ(sound->fieldGroups.mainOutput.shared.localHistory->mainOutputBusRef, toFieldGroupGuid(master->guid));
    EXPECT_FALSE(other->fieldGroups.mainOutput.shared.localHistory);
}

TEST_F(FieldGroupInspectorTest, MenuFromPreviousObjectCannotChangeNewContext)
{
    openMenu("mainOutput");
    auto* popup = panel->findChild<QMenu*>("fieldGroupMenu_mainOutput");
    ASSERT_NE(popup, nullptr);
    auto* action = popup->findChild<QAction*>("rebuildLocal");
    ASSERT_NE(action, nullptr);
    panel->showItemInspector(other);
    action->trigger();
    EXPECT_FALSE(sound->fieldGroups.mainOutput.shared.localHistory);
    EXPECT_FALSE(other->fieldGroups.mainOutput.shared.localHistory);
    popup->close();
}

TEST_F(FieldGroupInspectorTest, InvalidDormantReferenceRequiresExplicitRebuildAndUndoRestoresHistory)
{
    inherit("mainOutput")->click();
    ref("mainOutputBusRef")->objectChanged(busA->objectID);
    inherit("mainOutput")->click();
    const auto dormant = sound->fieldGroups;
    ASSERT_TRUE(ProjectManager::instance().deleteItem(busA));
    inherit("mainOutput")->click();
    EXPECT_EQ(sound->fieldGroups, dormant);
    EXPECT_TRUE(inherit("mainOutput")->isChecked());
    menu("mainOutput", "rebuildLocal");
    EXPECT_FALSE(inherit("mainOutput")->isChecked());
    EXPECT_EQ(ref("mainOutputBusRef")->currentObjectID(), master->objectID);
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(sound->fieldGroups, dormant);
    EXPECT_TRUE(inherit("mainOutput")->isChecked());
}

TEST_F(FieldGroupInspectorTest, SchemaReadOnlyAndConditionCannotBeOverriddenByGroupRefresh)
{
    auto schema = contentObjectInspectorRows(ProjectItem::Type::Sound);
    for (auto& descriptor : schema) {
        if (descriptor.field == "spatialPanning") descriptor.flags.readOnly = true;
        if (descriptor.field == "spatialAttenuationDistanceScaling")
            descriptor.editCondition = "spatialIs3DEnabled == true";
    }
    PropertyEditorEngine engine;
    engine.setItemType(ProjectItem::Type::Sound);
    engine.setSchema(schema, ComponentType::Audio);
    std::unique_ptr<QWidget> page(engine.buildPage());
    engine.bindItem(sound);
    mode(sound, "spatialRating", "Local");
    engine.refreshField("fieldGroups");
    EXPECT_FALSE(page->findChild<ParameterSlider*>("prop_spatialPanning")->isEnabled());
    EXPECT_FALSE(page->findChild<ParameterSlider*>("prop_spatialAttenuationDistanceScaling")->isEnabled());
    field(sound, "spatialRating", "spatial.is3DEnabled", true);
    engine.refreshField("fieldGroups");
    EXPECT_TRUE(page->findChild<ParameterSlider*>("prop_spatialAttenuationDistanceScaling")->isEnabled());
    mode(sound, "spatialRating", "Inherit");
    field(parent, "spatialRating", "spatial.is3DEnabled", true);
    engine.refreshField("fieldGroups");
    EXPECT_FALSE(page->findChild<ParameterSlider*>("prop_spatialAttenuationDistanceScaling")->isEnabled());
}

}
}
