#include "Support/FieldGroupTestUtils.h"
#include "ActionManager.h"
#include "Components/VMixNode.h"
#include "ISettingsStorage.h"
#include "MemorySettingsStorage.h"
#include "PlayablePropertyAuthoringV1.h"
#include "PlatformContext.h"
#include "PlatformContextService.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "FieldGroupProjectAdapter.h"
#include "UndoCommands.h"
#include "Widgets/Inspector/ContentObjectSchemaRows.h"
#include "Widgets/Inspector/BusInspectorSchema.h"
#include "Widgets/Inspector/InspectorCustomizationBootstrap.h"
#include "Widgets/Inspector/InspectorPanel.h"
#include "Widgets/Inspector/InspectorSlotStrip.h"
#include "Widgets/Inspector/PropertyEditorEngine.h"
#include "Widgets/Selector/ObjectRefWidget.h"
#include "Widgets/Common/ParameterSlider.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Core/ThemeManager.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QTemporaryDir>
#include <QToolButton>
#include <QWidget>

#include <memory>
#include "Support/ProjectPlatformTestUtils.h"

namespace {

using VNX::PlayablePropertyAuthoringV1;
using VNX::ProjectItem;

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

constexpr uint8_t kVolumeId = 1;
void expectBindingGutter(const QStringList& targets)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen())
        pm.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("BindingOnlyGutter");
    config.location = tempDir.path();
    config.platforms = HarkenTest::platforms(targets);
    if (!targets.isEmpty())
        ASSERT_TRUE(pm.createProject(config));

    VNX::PropertyEditorEngine engine;
    engine.setSchema(VNX::contentObjectInspectorRows(ProjectItem::Type::Sound),
                     VNX::ComponentType::Audio);
    engine.setItemType(ProjectItem::Type::Sound);
    std::unique_ptr<QWidget> page(engine.buildPage());
    auto* gutter = page->findChild<QWidget*>(
        QStringLiteral("inspectorFieldGutter_volume"));
    ASSERT_NE(gutter, nullptr);
    const auto& inspector = VNX::ThemeManager::instance().metrics().inspector;
    EXPECT_EQ(gutter->minimumWidth(), inspector.slotWidthFor(2));
    EXPECT_EQ(gutter->maximumWidth(), inspector.slotWidthFor(2));
    EXPECT_EQ(page->findChild<QWidget*>(QStringLiteral("source_volume")), nullptr);
    EXPECT_EQ(page->findChild<QWidget*>(QStringLiteral("platform_volume")), nullptr);
    pm.closeProject();
}

} // namespace

namespace VNX {

class PlatformOverrideInspector : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        VNX::PlatformContextService::instance().setStorage(
            std::make_unique<VNX::MemorySettingsStorage>());
        VNX::registerBuiltinInspectorCustomizations();
        auto& pm = VNX::ProjectManager::instance();
        if (pm.isProjectOpen())
            pm.closeProject();
        ASSERT_TRUE(tempDir.isValid());
        VNX::ProjectConfig config;
        config.name = QStringLiteral("PlatformOverrideInspector");
        config.location = tempDir.path();
        config.platforms = HarkenTest::platforms(QStringList{
            QStringLiteral("Windows"), QStringLiteral("Android")});
        ASSERT_TRUE(pm.createProject(config));
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::registerProjectActions(
            VNX::ActionManager::instance(), &pm, nullptr, {});
        VNX::UndoRedoManager::instance().clear();
        VNX::PlatformContextService::instance().setSelected(
            {HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Windows, true);
    }

    void TearDown() override
    {
        VNX::UndoRedoManager::instance().clear();
        VNX::ActionManager::instance().clearAllForTesting();
        auto& pm = VNX::ProjectManager::instance();
        if (pm.isProjectOpen())
            pm.closeProject();
        VNX::PlatformContextService::instance().setStorage(
            VNX::SettingsStorageFactory::create(
                VNX::SettingsStorageFactory::StorageType::QSettings,
                QStringLiteral("HarkenSound"),
                QStringLiteral("HarkenStudio")));
    }

    ProjectItem* makeSound(const QString& name)
    {
        return VNX::ProjectManager::instance().createItem(
            nullptr, name, ProjectItem::Type::Sound);
    }

    ProjectItem* makeBus(const QString& name)
    {
        auto& pm = VNX::ProjectManager::instance();
        return pm.createItem(pm.getMasterBusItem(), name, ProjectItem::Type::Bus);
    }

    std::optional<VNX::PlayablePropertyLocatorV1> locatorOf(ProjectItem* item)
    {
        return PlayablePropertyAuthoringV1::definitionLocator(*item);
    }

    std::unique_ptr<QWidget> buildBoundPage(ProjectItem* sound)
    {
        engine.setSchema(VNX::contentObjectInspectorRows(ProjectItem::Type::Sound),
                         VNX::ComponentType::Audio);
        engine.setItemType(ProjectItem::Type::Sound);
        std::unique_ptr<QWidget> page(engine.buildPage());
        engine.bindItem(sound);
        return page;
    }

    void setSharedLocalGroups(ProjectItem* sound, ProjectItem* mainBus = nullptr)
    {
        auto& pm = ProjectManager::instance();
        if (!mainBus) mainBus = pm.getMasterBusItem();
        ASSERT_NE(mainBus, nullptr);
        auto groups = sound->fieldGroups;
        groups.mainOutput.shared = {
            FieldGroupMode::Local, MainOutputFieldGroup{toFieldGroupGuid(mainBus->guid), 0.0f}};
        groups.send.shared = {FieldGroupMode::Local, SendFieldGroup{}};
        groups.spatialRating.shared = {FieldGroupMode::Local, SpatialRatingFieldGroup{}};
        ASSERT_TRUE(pm.restoreFieldGroupsInternal(sound, groups));
    }

    QTemporaryDir tempDir;
    VNX::PropertyEditorEngine engine;
};

TEST(PlatformOverrideInspectorStandalone, ClosedProjectKeepsBindingGutter)
{
    expectBindingGutter({});
}

TEST(PlatformOverrideInspectorStandalone, SingleTargetKeepsBindingGutter)
{
    expectBindingGutter({QStringLiteral("Windows")});
}

TEST_F(PlatformOverrideInspector, MultiPlatformGutterContainsVariableAndAttenuation)
{
    ProjectItem* sound = makeSound(QStringLiteral("GutterSound"));
    ASSERT_NE(sound, nullptr);
    std::unique_ptr<QWidget> page = buildBoundPage(sound);
    auto* gutter = page->findChild<QWidget*>(
        QStringLiteral("inspectorFieldGutter_volume"));
    ASSERT_NE(gutter, nullptr);
    const auto& inspector = VNX::ThemeManager::instance().metrics().inspector;
    EXPECT_EQ(gutter->minimumWidth(), inspector.slotWidthFor(2));
    EXPECT_EQ(gutter->maximumWidth(), inspector.slotWidthFor(2));
    EXPECT_EQ(page->findChild<QWidget*>(QStringLiteral("source_volume")), nullptr);
    EXPECT_EQ(page->findChild<QWidget*>(QStringLiteral("platform_volume")), nullptr);
    auto* strip = gutter->findChild<VNX::InspectorSlotStrip*>();
    ASSERT_NE(strip, nullptr);
    EXPECT_EQ(strip->occupiedSlotCount(), 2);
    EXPECT_NE(strip->slotWidget(VNX::InspectorSlotKind::Binding), nullptr);
    EXPECT_NE(strip->slotWidget(VNX::InspectorSlotKind::Attenuation), nullptr);
}

TEST_F(PlatformOverrideInspector, PlatformSelectionKeepsBindingGutterWidth)
{
    ProjectItem* sound = makeSound(QStringLiteral("NoneSound"));
    ASSERT_NE(sound, nullptr);
    const auto locator = locatorOf(sound);
    ASSERT_TRUE(locator.has_value());
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *sound, *locator, kVolumeId, -6.0, std::nullopt));

    std::unique_ptr<QWidget> page = buildBoundPage(sound);
    auto* gutter = page->findChild<QWidget*>(
        QStringLiteral("inspectorFieldGutter_volume"));
    ASSERT_NE(gutter, nullptr);
    const int widthBefore = gutter->minimumWidth();
    auto* slider = page->findChild<ParameterSlider*>(QStringLiteral("prop_volume"));
    ASSERT_NE(slider, nullptr);
    EXPECT_DOUBLE_EQ(slider->value(), -6.0);

    VNX::PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Android, false);
    EXPECT_EQ(gutter->minimumWidth(), widthBefore);
    EXPECT_EQ(gutter->maximumWidth(), widthBefore);
    EXPECT_DOUBLE_EQ(slider->value(), -6.0);
    EXPECT_EQ(page->findChild<QWidget*>(QStringLiteral("platform_volume")), nullptr);
}

TEST_F(PlatformOverrideInspector, SingleTargetBusSendsRequireAllRatherThanTargetCoverage)
{
    auto& manager = ProjectManager::instance();
    manager.setProjectPlatformsInternal(HarkenTest::platforms({QStringLiteral("Windows")}));
    auto* bus = makeBus(QStringLiteral("SourceBus"));
    auto* target = makeBus(QStringLiteral("TargetBus"));
    ASSERT_NE(bus, nullptr);
    ASSERT_NE(target, nullptr);
    auto* mix = bus->comp<VMixNode>();
    ASSERT_NE(mix, nullptr);
    mix->sends.push_back({target->objectID, VMixNode::dbToLinear(-5.0f)});
    auto& platforms = PlatformContextService::instance();
    platforms.setSelected({HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
    ASSERT_FALSE(platforms.context().all);
    ASSERT_TRUE(selectedEqualsTargetPlatforms(platforms.context().selected, manager.getTargetPlatforms()));
    engine.setSchema(busInspectorSchema(), ComponentType::Bus);
    engine.setItemType(ProjectItem::Type::Bus);
    std::unique_ptr<QWidget> page(engine.buildPage());
    engine.bindItem(bus);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    int changes = 0;
    QObject::connect(&engine, &PropertyEditorEngine::fieldChanged, page.get(),
        [&changes](const QString&, const QVariant&, const QVariant&) { ++changes; });
    auto* sends = page->findChild<QWidget*>(QStringLiteral("prop_sends"));
    ASSERT_NE(sends, nullptr);
    EXPECT_EQ(sends->findChild<ParameterSlider*>(), nullptr);
    bool displayedLevel = false;
    for (auto* label : sends->findChildren<QLabel*>())
        if (label->property("roValue").toBool()) {
            EXPECT_TRUE(label->text().contains(QStringLiteral("-5.0")));
            displayedLevel = true;
        }
    EXPECT_TRUE(displayedLevel);
    auto* reference = sends->findChild<ObjectRefWidget*>();
    ASSERT_NE(reference, nullptr);
    EXPECT_TRUE(reference->isReadOnly());
    reference->objectChanged(0);
    EXPECT_EQ(changes, 0);

    platforms.setAll(true);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    auto* level = sends->findChild<ParameterSlider*>();
    reference = sends->findChild<ObjectRefWidget*>();
    ASSERT_NE(level, nullptr);
    ASSERT_NE(reference, nullptr);
    EXPECT_TRUE(level->isEnabled());
    EXPECT_FALSE(reference->isReadOnly());
    level->simulateValueChange(-9.0);
    EXPECT_EQ(changes, 1);
}

TEST_F(PlatformOverrideInspector, SingleTargetVolumeWritesOnlyCurrentPlatformUntilAllIsEnabled)
{
    auto& manager = ProjectManager::instance();
    manager.setProjectPlatformsInternal(HarkenTest::platforms({QStringLiteral("Windows")}));
    auto* sound = makeSound(QStringLiteral("OneTargetSound"));
    ASSERT_NE(sound, nullptr);
    const auto locator = locatorOf(sound);
    ASSERT_TRUE(locator);
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(*sound, *locator, kVolumeId, -2.0, std::nullopt));
    auto& platforms = PlatformContextService::instance();
    platforms.setSelected({HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
    InspectorPanel panel;
    panel.showItemInspector(sound);
    auto* volume = panel.findChild<ParameterSlider*>(QStringLiteral("prop_volume"));
    ASSERT_NE(volume, nullptr);
    volume->simulateValueChange(-7.0);
    const auto shared = PlayablePropertyAuthoringV1::readProperty(*sound, *locator, kVolumeId, std::nullopt);
    const auto overlay = PlayablePropertyAuthoringV1::readPlatformOverlay(*sound, *locator, kVolumeId, HarkenTest::PlatformIds::Windows);
    ASSERT_TRUE(shared && overlay && overlay->present);
    EXPECT_DOUBLE_EQ(shared->value.toDouble(), -2.0);
    EXPECT_DOUBLE_EQ(overlay->value.toDouble(), -7.0);
    platforms.setAll(true);
    ASSERT_TRUE(platforms.context().all);
    EXPECT_DOUBLE_EQ(volume->value(), -7.0);
    volume->simulateValueChange(-4.0);
    const auto editedShared = PlayablePropertyAuthoringV1::readProperty(*sound, *locator, kVolumeId, std::nullopt);
    const auto retainedOverlay = PlayablePropertyAuthoringV1::readPlatformOverlay(*sound, *locator, kVolumeId, HarkenTest::PlatformIds::Windows);
    ASSERT_TRUE(editedShared && retainedOverlay);
    EXPECT_FALSE(retainedOverlay->present);
    EXPECT_DOUBLE_EQ(editedShared->value.toDouble(), -4.0);
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(PlayablePropertyAuthoringV1::readPlatformOverlay(*sound, *locator, kVolumeId, HarkenTest::PlatformIds::Windows), overlay);
}

TEST_F(PlatformOverrideInspector, MasterOutputRemainsReadOnlyAndPreservesAllPlatformValues)
{
    auto& manager = ProjectManager::instance();
    auto* master = manager.getMasterBusItem();
    auto* target = makeBus(QStringLiteral("TargetBus"));
    ASSERT_NE(master, nullptr);
    ASSERT_NE(target, nullptr);
    const auto locator = locatorOf(master);
    ASSERT_TRUE(locator);
    const auto outputVolume = static_cast<uint8_t>(
        harkenplayableproperties::PlayablePropertyIdV1::OutputBusVolume);
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *master, *locator, outputVolume, -3.0, std::nullopt));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *master, *locator, outputVolume, -6.0, HarkenTest::PlatformIds::Windows));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *master, *locator, outputVolume, -9.0, HarkenTest::PlatformIds::Android));
    ASSERT_FLOAT_EQ(master->comp<VMixNode>()->outputBusVolume, -3.0f);
    const auto before = PlayablePropertyAuthoringV1::definitionToJson(*master);
    const auto outputBefore = master->comp<VMixNode>()->outputBusId;
    UndoRedoManager::instance().clear();
    master->clearDirty();
    int changes = 0;
    int previews = 0;
    InspectorPanel panel;
    panel.showBusInspector(master->objectID);
    QObject::connect(&manager, &ProjectManager::itemPropertyChanged, &panel,
        [&changes] { ++changes; });
    QObject::connect(&manager, &ProjectManager::itemPropertyPreviewed, &panel,
        [&previews] { ++previews; });
    auto* volume = panel.findChild<ParameterSlider*>(QStringLiteral("prop_outputBusVolume"));
    auto* output = panel.findChild<ObjectRefWidget*>(QStringLiteral("prop_outputBusId"));
    ASSERT_NE(volume, nullptr);
    ASSERT_NE(output, nullptr);
    const auto check = [&] {
        EXPECT_FALSE(volume->isEnabled());
        EXPECT_TRUE(output->isReadOnly());
        EXPECT_FALSE(volume->mixedValue());
        EXPECT_DOUBLE_EQ(volume->value(), -3.0);
        volume->binding()->beginEdit();
        EXPECT_FALSE(volume->binding()->isEditing());
        volume->binding()->previewEdit(-18.0);
        volume->binding()->commitEdit();
        volume->simulateValueChange(0.0);
        output->objectChanged(target->objectID);
        EXPECT_EQ(PlayablePropertyAuthoringV1::definitionToJson(*master), before);
        EXPECT_FLOAT_EQ(master->comp<VMixNode>()->outputBusVolume, -3.0f);
        EXPECT_EQ(master->comp<VMixNode>()->outputBusId, outputBefore);
        EXPECT_FALSE(master->isDirtyObject());
        EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
        EXPECT_EQ(changes, 0);
        EXPECT_EQ(previews, 0);
        panel.refreshBusInspector();
        EXPECT_FALSE(volume->isEnabled());
        EXPECT_TRUE(output->isReadOnly());
        EXPECT_FALSE(volume->mixedValue());
        EXPECT_DOUBLE_EQ(volume->value(), -3.0);
    };
    auto& platforms = PlatformContextService::instance();
    platforms.setSelected({HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
    check();
    platforms.setSelected({HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Android, false);
    check();
    platforms.setSelected({HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android},
                          HarkenTest::PlatformIds::Windows, false);
    check();
    platforms.setAll(true);
    check();
    const auto windows = PlayablePropertyAuthoringV1::readPlatformOverlay(
        *master, *locator, outputVolume, HarkenTest::PlatformIds::Windows);
    const auto android = PlayablePropertyAuthoringV1::readPlatformOverlay(
        *master, *locator, outputVolume, HarkenTest::PlatformIds::Android);
    ASSERT_TRUE(windows && android && windows->present && android->present);
    EXPECT_DOUBLE_EQ(windows->value.toDouble(), -6.0);
    EXPECT_DOUBLE_EQ(android->value.toDouble(), -9.0);
}

TEST_F(PlatformOverrideInspector, SwitchingFromMasterKeepsOrdinaryBusOutputVolumeEditable)
{
    auto& manager = ProjectManager::instance();
    auto* master = manager.getMasterBusItem();
    auto* bus = makeBus(QStringLiteral("EditableBus"));
    ASSERT_NE(master, nullptr);
    ASSERT_NE(bus, nullptr);
    const auto locator = locatorOf(bus);
    ASSERT_TRUE(locator);
    const auto outputVolume = static_cast<uint8_t>(
        harkenplayableproperties::PlayablePropertyIdV1::OutputBusVolume);
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *bus, *locator, outputVolume, -2.0, std::nullopt));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *bus, *locator, outputVolume, -4.0, HarkenTest::PlatformIds::Windows));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *bus, *locator, outputVolume, -8.0, HarkenTest::PlatformIds::Android));
    const auto before = PlayablePropertyAuthoringV1::definitionToJson(*bus);
    UndoRedoManager::instance().clear();
    auto& platforms = PlatformContextService::instance();
    platforms.setSelected({HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Android, false);
    InspectorPanel panel;
    panel.showBusInspector(master->objectID);
    auto* volume = panel.findChild<ParameterSlider*>(QStringLiteral("prop_outputBusVolume"));
    auto* output = panel.findChild<ObjectRefWidget*>(QStringLiteral("prop_outputBusId"));
    ASSERT_NE(volume, nullptr);
    ASSERT_NE(output, nullptr);
    EXPECT_FALSE(volume->isEnabled());
    panel.showBusInspector(bus->objectID);
    EXPECT_TRUE(volume->isEnabled());
    EXPECT_TRUE(output->isReadOnly());
    EXPECT_DOUBLE_EQ(volume->value(), -2.0);
    volume->simulateValueChange(-10.0);
    EXPECT_FLOAT_EQ(bus->comp<VMixNode>()->outputBusVolume, -10.0f);
    const auto android = PlayablePropertyAuthoringV1::readPlatformOverlay(
        *bus, *locator, outputVolume, HarkenTest::PlatformIds::Android);
    const auto windows = PlayablePropertyAuthoringV1::readPlatformOverlay(
        *bus, *locator, outputVolume, HarkenTest::PlatformIds::Windows);
    ASSERT_TRUE(android && windows && android->present && windows->present);
    EXPECT_DOUBLE_EQ(android->value.toDouble(), -8.0);
    EXPECT_DOUBLE_EQ(windows->value.toDouble(), -4.0);
    ASSERT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(PlayablePropertyAuthoringV1::definitionToJson(*bus), before);
    EXPECT_FLOAT_EQ(bus->comp<VMixNode>()->outputBusVolume, -2.0f);
    platforms.setAll(true);
    EXPECT_TRUE(volume->isEnabled());
    EXPECT_TRUE(output->isReadOnly());
    volume->simulateValueChange(-5.0);
    EXPECT_FLOAT_EQ(bus->comp<VMixNode>()->outputBusVolume, -5.0f);
    const auto androidAfterAll = PlayablePropertyAuthoringV1::readPlatformOverlay(
        *bus, *locator, outputVolume, HarkenTest::PlatformIds::Android);
    const auto windowsAfterAll = PlayablePropertyAuthoringV1::readPlatformOverlay(
        *bus, *locator, outputVolume, HarkenTest::PlatformIds::Windows);
    ASSERT_TRUE(androidAfterAll && windowsAfterAll
        && androidAfterAll->present && windowsAfterAll->present);
    EXPECT_DOUBLE_EQ(androidAfterAll->value.toDouble(), -8.0);
    EXPECT_DOUBLE_EQ(windowsAfterAll->value.toDouble(), -4.0);
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(PlayablePropertyAuthoringV1::definitionToJson(*bus), before);
    panel.showBusInspector(master->objectID);
    EXPECT_FALSE(volume->isEnabled());
    EXPECT_TRUE(output->isReadOnly());
}

TEST_F(PlatformOverrideInspector, SharedLocalRemainsEditableAcrossRefreshAndAllPlatforms)
{
    ProjectItem* sound = makeSound(QStringLiteral("LockSound"));
    ProjectItem* bus = makeBus(QStringLiteral("LockBus"));
    ASSERT_NE(sound, nullptr);
    ASSERT_NE(bus, nullptr);
    setSharedLocalGroups(sound, bus);

    VNX::PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Android, false);
    std::unique_ptr<QWidget> page = buildBoundPage(sound);
    auto* ref = page->findChild<VNX::ObjectRefWidget*>(
        QStringLiteral("prop_mainOutputBusRef"));
    auto* atten = page->findChild<VNX::ObjectRefWidget*>(
        QStringLiteral("prop_spatialAttenuationRef"));
    auto* sends = page->findChild<QWidget*>(QStringLiteral("prop_sends"));
    ASSERT_NE(ref, nullptr);
    ASSERT_NE(atten, nullptr);
    ASSERT_NE(sends, nullptr);
    EXPECT_EQ(ref->currentObjectID(), bus->objectID);
    EXPECT_FALSE(ref->isReadOnly());
    EXPECT_FALSE(atten->isReadOnly());
    auto* pick = ref->findChild<QToolButton*>(QStringLiteral("refPick"));
    ASSERT_NE(pick, nullptr);
    EXPECT_FALSE(pick->isHidden());
    auto* add = sends->findChild<QToolButton*>(QStringLiteral("listAdd_sends"));
    ASSERT_NE(add, nullptr);
    EXPECT_FALSE(add->isHidden());
    EXPECT_TRUE(sound->fieldGroups.mainOutput.platforms.empty());

    engine.refreshField(QStringLiteral("mainOutputBusRef"));
    engine.refreshField(QStringLiteral("spatialAttenuationRef"));
    engine.refreshField(QStringLiteral("sends"));
    EXPECT_FALSE(ref->isReadOnly());
    EXPECT_FALSE(atten->isReadOnly());
    EXPECT_FALSE(pick->isHidden());
    EXPECT_FALSE(add->isHidden());
    EXPECT_TRUE(sound->fieldGroups.mainOutput.platforms.empty());
    emit ref->objectChanged(ProjectManager::instance().getMasterBusItem()->objectID);
    ASSERT_TRUE(sound->fieldGroups.mainOutput.platforms.contains(HarkenTest::FieldGroupPlatformIds::Android));

    VNX::PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Windows, true);
    EXPECT_FALSE(ref->isReadOnly());
    EXPECT_FALSE(atten->isReadOnly());
    EXPECT_FALSE(pick->isHidden());
    EXPECT_FALSE(add->isHidden());
    EXPECT_FALSE(add->parentWidget() && add->parentWidget()->isHidden());
    EXPECT_EQ(ref->currentObjectID(), 0u);
    EXPECT_FALSE(sound->fieldGroups.mainOutput.platforms.contains(HarkenTest::FieldGroupPlatformIds::Windows));
    EXPECT_TRUE(sound->fieldGroups.send.platforms.empty());
    EXPECT_TRUE(sound->fieldGroups.spatialRating.platforms.empty());
}

TEST_F(PlatformOverrideInspector, AllShowsMixedFieldsWithoutPlatformMarks)
{
    ProjectItem* sound = makeSound(QStringLiteral("MixedSound"));
    ProjectItem* busA = makeBus(QStringLiteral("BusA"));
    ProjectItem* busB = makeBus(QStringLiteral("BusB"));
    ASSERT_NE(sound, nullptr);
    ASSERT_NE(busA, nullptr);
    ASSERT_NE(busB, nullptr);
    auto groups = sound->fieldGroups;
    groups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Windows] = {
        FieldGroupMode::Local, MainOutputFieldGroup{toFieldGroupGuid(busA->guid), 0.0f}};
    groups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        FieldGroupMode::Local, MainOutputFieldGroup{toFieldGroupGuid(busB->guid), 0.0f}};
    groups.send.platforms[HarkenTest::FieldGroupPlatformIds::Windows] = {
        FieldGroupMode::Local, SendFieldGroup{{{toFieldGroupGuid(busB->guid), 0.0f}}}};
    groups.send.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        FieldGroupMode::Local, SendFieldGroup{{{toFieldGroupGuid(busA->guid), -6.0f}}}};
    ASSERT_TRUE(ProjectManager::instance().restoreFieldGroupsInternal(sound, groups));

    VNX::PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Windows, true);
    std::unique_ptr<QWidget> page = buildBoundPage(sound);
    auto* ref = page->findChild<VNX::ObjectRefWidget*>(
        QStringLiteral("prop_mainOutputBusRef"));
    auto* sends = page->findChild<QWidget*>(QStringLiteral("prop_sends"));
    ASSERT_NE(ref, nullptr);
    ASSERT_NE(sends, nullptr);
    EXPECT_EQ(ref->currentObjectID(), 0u);
    EXPECT_FALSE(ref->isReadOnly());
    EXPECT_FALSE(ref->toolTip().isEmpty());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_TRUE(sends->findChildren<QWidget*>(QStringLiteral("listRow")).isEmpty());
    auto* mixed = sends->findChild<QLabel*>(QStringLiteral("listMixedValue"));
    ASSERT_NE(mixed, nullptr);
    EXPECT_TRUE(mixed->isHidden());
    EXPECT_FALSE(sends->toolTip().isEmpty());
    EXPECT_EQ(page->findChild<QWidget*>(QStringLiteral("platform_sends")), nullptr);
    EXPECT_EQ(page->findChild<QWidget*>(
        QStringLiteral("platform_mainOutputBusRef")), nullptr);
    for (const auto* group : {"mainOutput", "send"}) {
        auto* inherit = page->findChild<QCheckBox*>(QStringLiteral("inherit_") + QLatin1String(group));
        ASSERT_NE(inherit, nullptr);
        EXPECT_EQ(inherit->checkState(), Qt::Unchecked);
    }
}

TEST_F(PlatformOverrideInspector, ClearPlatformOverrideActionPreservesBase)
{
    ProjectItem* sound = makeSound(QStringLiteral("RevertSound"));
    ASSERT_NE(sound, nullptr);
    const auto locator = locatorOf(sound);
    ASSERT_TRUE(locator.has_value());
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *sound, *locator, kVolumeId, -6.0, std::nullopt));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *sound, *locator, kVolumeId, -12.0, HarkenTest::PlatformIds::Android));

    VNX::PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Android, false);
    std::unique_ptr<QWidget> page = buildBoundPage(sound);
    const auto result = ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        {{QStringLiteral("authoring_handle"), static_cast<qint64>(sound->objectID)},
         {QStringLiteral("property"), QStringLiteral("volume")},
         {QStringLiteral("override_operation"), QStringLiteral("clear_platform_override")},
         {QStringLiteral("platform_ids"), QJsonArray{HarkenTest::platformIdString("Android")}}});
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();

    const auto overlay = PlayablePropertyAuthoringV1::readPlatformOverlay(
        *sound, *locator, kVolumeId, HarkenTest::PlatformIds::Android);
    EXPECT_TRUE(!overlay || !overlay->present);
    const auto base = PlayablePropertyAuthoringV1::readProperty(
        *sound, *locator, kVolumeId, std::nullopt);
    ASSERT_TRUE(base.has_value());
    EXPECT_TRUE(base->present);
    EXPECT_DOUBLE_EQ(base->value.toDouble(), -6.0);
}

TEST_F(PlatformOverrideInspector, AddingSecondTargetKeepsBindingGutter)
{
    auto& pm = VNX::ProjectManager::instance();
    pm.setProjectPlatformsInternal(HarkenTest::platforms(QStringList{QStringLiteral("Windows")}));
    VNX::PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);

    ProjectItem* sound = makeSound(QStringLiteral("RebuildSound"));
    ASSERT_NE(sound, nullptr);
    VNX::InspectorPanel panel;
    panel.showItemInspector(sound);
    auto* gutter = panel.findChild<QWidget*>(
        QStringLiteral("inspectorFieldGutter_volume"));
    ASSERT_NE(gutter, nullptr);
    const auto& inspector = VNX::ThemeManager::instance().metrics().inspector;
    EXPECT_EQ(gutter->minimumWidth(), inspector.slotWidthFor(2));
    EXPECT_EQ(panel.findChild<QWidget*>(
                  QStringLiteral("platform_volume")),
              nullptr);

    pm.setProjectPlatformsInternal(HarkenTest::platforms(QStringList{
        QStringLiteral("Windows"), QStringLiteral("Android")}));
    EXPECT_EQ(panel.currentContextItem(), sound);
    gutter = panel.findChild<QWidget*>(
        QStringLiteral("inspectorFieldGutter_volume"));
    ASSERT_NE(gutter, nullptr);
    EXPECT_EQ(gutter->minimumWidth(), inspector.slotWidthFor(2));
    EXPECT_EQ(gutter->maximumWidth(), inspector.slotWidthFor(2));
    EXPECT_EQ(panel.findChild<QWidget*>(
                  QStringLiteral("platform_volume")),
              nullptr);
}

TEST_F(PlatformOverrideInspector, DroppingToOneTargetKeepsBindingGutter)
{
    ProjectItem* sound = makeSound(QStringLiteral("ShrinkSound"));
    ASSERT_NE(sound, nullptr);
    VNX::InspectorPanel panel;
    panel.showItemInspector(sound);
    auto* gutter = panel.findChild<QWidget*>(
        QStringLiteral("inspectorFieldGutter_volume"));
    ASSERT_NE(gutter, nullptr);
    const auto& inspector = VNX::ThemeManager::instance().metrics().inspector;
    EXPECT_EQ(gutter->minimumWidth(), inspector.slotWidthFor(2));
    EXPECT_EQ(panel.findChild<QWidget*>(
                  QStringLiteral("platform_volume")),
              nullptr);

    VNX::ProjectManager::instance().setProjectPlatformsInternal(HarkenTest::platforms(QStringList{QStringLiteral("Windows")}));
    EXPECT_EQ(panel.currentContextItem(), sound);
    gutter = panel.findChild<QWidget*>(
        QStringLiteral("inspectorFieldGutter_volume"));
    ASSERT_NE(gutter, nullptr);
    EXPECT_EQ(gutter->minimumWidth(), inspector.slotWidthFor(2));
    EXPECT_EQ(gutter->maximumWidth(), inspector.slotWidthFor(2));
    EXPECT_EQ(panel.findChild<QWidget*>(
                  QStringLiteral("platform_volume")),
              nullptr);
}

TEST_F(PlatformOverrideInspector, SingleTargetWritesAndUndoesOnlyItsPlatformOverride)
{
    auto& pm = ProjectManager::instance();
    pm.setProjectPlatformsInternal(HarkenTest::platforms({QStringLiteral("Windows")}));
    auto& platforms = PlatformContextService::instance();
    platforms.setAll(false);
    ASSERT_FALSE(platforms.context().all);
    ProjectItem* sound = makeSound(QStringLiteral("SinglePlatformSound"));
    ASSERT_NE(sound, nullptr);
    const auto locator = locatorOf(sound);
    ASSERT_TRUE(locator);
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *sound, *locator, kVolumeId, -6.0, std::nullopt));

    InspectorPanel panel;
    panel.showItemInspector(sound);
    auto* slider = panel.findChild<ParameterSlider*>(QStringLiteral("prop_volume"));
    ASSERT_NE(slider, nullptr);
    EXPECT_DOUBLE_EQ(slider->value(), -6.0);
    slider->binding()->commitValue(-12.0);
    const auto base = PlayablePropertyAuthoringV1::readProperty(
        *sound, *locator, kVolumeId, std::nullopt);
    ASSERT_TRUE(base);
    EXPECT_DOUBLE_EQ(base->value.toDouble(), -6.0);
    auto overlay = PlayablePropertyAuthoringV1::readPlatformOverlay(
        *sound, *locator, kVolumeId, HarkenTest::PlatformIds::Windows);
    ASSERT_TRUE(overlay);
    EXPECT_TRUE(overlay->present);
    EXPECT_DOUBLE_EQ(overlay->value.toDouble(), -12.0);
    platforms.setAll(true);
    EXPECT_DOUBLE_EQ(slider->value(), -12.0);
    EXPECT_EQ(PlayablePropertyAuthoringV1::readPlatformOverlay(
                  *sound, *locator, kVolumeId, HarkenTest::PlatformIds::Windows), overlay);
    platforms.setAll(false);
    EXPECT_DOUBLE_EQ(slider->value(), -12.0);
    UndoRedoManager::instance().undoStack()->undo();
    overlay = PlayablePropertyAuthoringV1::readPlatformOverlay(
        *sound, *locator, kVolumeId, HarkenTest::PlatformIds::Windows);
    ASSERT_TRUE(overlay);
    EXPECT_FALSE(overlay->present);
    EXPECT_DOUBLE_EQ(slider->value(), -6.0);
    EXPECT_EQ(PlayablePropertyAuthoringV1::readProperty(
                  *sound, *locator, kVolumeId, std::nullopt), base);
    UndoRedoManager::instance().undoStack()->redo();
    overlay = PlayablePropertyAuthoringV1::readPlatformOverlay(
        *sound, *locator, kVolumeId, HarkenTest::PlatformIds::Windows);
    ASSERT_TRUE(overlay);
    EXPECT_TRUE(overlay->present);
    EXPECT_DOUBLE_EQ(overlay->value.toDouble(), -12.0);
    EXPECT_DOUBLE_EQ(slider->value(), -12.0);
}

TEST_F(PlatformOverrideInspector, SharedLocalHasAnInheritControlWithoutFieldPlatformMarks)
{
    ProjectItem* sound = makeSound(QStringLiteral("LockKeySound"));
    ProjectItem* bus = makeBus(QStringLiteral("LockKeyBus"));
    ASSERT_NE(sound, nullptr);
    ASSERT_NE(bus, nullptr);
    setSharedLocalGroups(sound, bus);

    VNX::PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Android, false);
    std::unique_ptr<QWidget> page = buildBoundPage(sound);
    auto* ref = page->findChild<VNX::ObjectRefWidget*>(
        QStringLiteral("prop_mainOutputBusRef"));
    ASSERT_NE(ref, nullptr);
    EXPECT_FALSE(ref->isReadOnly());
    EXPECT_EQ(page->findChild<QWidget*>(
                  QStringLiteral("platform_mainOutputBusRef")),
              nullptr);
    auto* status = page->findChild<QToolButton*>(QStringLiteral("fieldGroupSettings_mainOutput"));
    auto* inherit = page->findChild<QCheckBox*>(QStringLiteral("inherit_mainOutput"));
    EXPECT_EQ(status, nullptr);
    ASSERT_NE(inherit, nullptr);
    EXPECT_EQ(inherit->checkState(), Qt::Unchecked);
    EXPECT_NE(page->findChild<QWidget*>(QStringLiteral("inspectorFieldGutter_mainOutputBusRef")), nullptr);
}

TEST_F(PlatformOverrideInspector, FirstReferenceEditCreatesOnlyTheSelectedPlatformGroup)
{
    ProjectItem* sound = makeSound(QStringLiteral("UnlockTipSound"));
    ProjectItem* bus = makeBus(QStringLiteral("UnlockTipBus"));
    ASSERT_NE(sound, nullptr);
    ASSERT_NE(bus, nullptr);
    setSharedLocalGroups(sound, bus);

    VNX::PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Android, false);
    std::unique_ptr<QWidget> page = buildBoundPage(sound);
    auto* ref = page->findChild<VNX::ObjectRefWidget*>(
        QStringLiteral("prop_mainOutputBusRef"));
    ASSERT_NE(ref, nullptr);
    EXPECT_FALSE(ref->isReadOnly());

    EXPECT_TRUE(sound->fieldGroups.mainOutput.platforms.empty());
    auto* otherBus = makeBus(QStringLiteral("AndroidOnlyBus"));
    ASSERT_NE(otherBus, nullptr);
    emit ref->objectChanged(otherBus->objectID);
    EXPECT_EQ(sound->fieldGroups.mainOutput.platforms.at(HarkenTest::FieldGroupPlatformIds::Android).mode,
        FieldGroupMode::UseShared);
    EXPECT_EQ(sound->fieldGroups.mainOutput.platforms.at(HarkenTest::FieldGroupPlatformIds::Android).localFields,
        FieldGroupFieldMask(harkenplayableproperties::PlayablePropertyIdV1::MainOutputBusRef));
    EXPECT_FALSE(sound->fieldGroups.mainOutput.platforms.contains(HarkenTest::FieldGroupPlatformIds::Windows));
    ASSERT_TRUE(sound->fieldGroups.mainOutput.platforms.at(HarkenTest::FieldGroupPlatformIds::Android).localHistory.has_value());
    EXPECT_EQ(sound->fieldGroups.mainOutput.platforms.at(HarkenTest::FieldGroupPlatformIds::Android).localHistory->mainOutputBusRef,
        toFieldGroupGuid(otherBus->guid));
    ASSERT_TRUE(sound->fieldGroups.mainOutput.shared.localHistory.has_value());
    EXPECT_EQ(sound->fieldGroups.mainOutput.shared.localHistory->mainOutputBusRef,
        toFieldGroupGuid(bus->guid));
}

TEST_F(PlatformOverrideInspector, AllSendTableEditsReplaceTheFieldForEveryPlatform)
{
    ProjectItem* sound = makeSound(QStringLiteral("MixedTipSound"));
    ProjectItem* busA = makeBus(QStringLiteral("MixedTipBusA"));
    ProjectItem* busB = makeBus(QStringLiteral("MixedTipBusB"));
    ASSERT_NE(sound, nullptr);
    ASSERT_NE(busA, nullptr);
    ASSERT_NE(busB, nullptr);
    auto groups = sound->fieldGroups;
    groups.send.shared = {
        FieldGroupMode::Local, SendFieldGroup{{{toFieldGroupGuid(busA->guid), -4.0f}}}};
    groups.send.platforms[HarkenTest::FieldGroupPlatformIds::Windows] = {
        FieldGroupMode::Local, SendFieldGroup{{{toFieldGroupGuid(busA->guid), 0.0f}}}};
    groups.send.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        FieldGroupMode::Local, SendFieldGroup{{{toFieldGroupGuid(busB->guid), -6.0f}}}};
    ASSERT_TRUE(ProjectManager::instance().restoreFieldGroupsInternal(sound, groups));

    VNX::PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Windows, true);
    std::unique_ptr<QWidget> page = buildBoundPage(sound);
    auto* sends = page->findChild<QWidget*>(QStringLiteral("prop_sends"));
    ASSERT_NE(sends, nullptr);
    auto* mixed = sends->findChild<QLabel*>(QStringLiteral("listMixedValue"));
    ASSERT_NE(mixed, nullptr);
    EXPECT_TRUE(mixed->isHidden());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    auto* level = sends->findChild<ParameterSlider*>();
    ASSERT_NE(level, nullptr);
    level->simulateValueChange(-3.0);
    EXPECT_TRUE(sound->fieldGroups.send.platforms.empty());
    ASSERT_TRUE(sound->fieldGroups.send.shared.localHistory.has_value());
    ASSERT_EQ(sound->fieldGroups.send.shared.localHistory->sends.size(), 1u);
    EXPECT_EQ(sound->fieldGroups.send.shared.localHistory->sends[0].targetBusID, toFieldGroupGuid(busA->guid));
    EXPECT_FLOAT_EQ(sound->fieldGroups.send.shared.localHistory->sends[0].sendLevel, -3.0f);
}

}
