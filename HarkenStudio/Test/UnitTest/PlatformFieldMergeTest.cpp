#include "Core/Components/VSelection.h"
#include "Support/FieldGroupTestUtils.h"
#include "ActionManager.h"
#include "ISettingsStorage.h"
#include "MemorySettingsStorage.h"
#include "PlayablePropertyAuthoringV1.h"
#include "PlatformContext.h"
#include "PlatformContextService.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "PropertyEditSession.h"
#include "FieldGroupProjectAdapter.h"
#include "UndoCommands.h"
#include "Components/VMixNode.h"
#include "Components/VSpatial.h"
#include "Components/VTrack.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/Common/ParameterPresenter.h"
#include "Widgets/Common/ParameterSlider.h"
#include "Widgets/Common/ValueSegmentedControl.h"
#include "Widgets/Inspector/ContentObjectSchemaRows.h"
#include "Widgets/Inspector/InspectorCustomizationBootstrap.h"
#include "Widgets/Inspector/InspectorPanel.h"
#include "Widgets/Shared/PlatformFieldMerge.h"
#include "Widgets/Inspector/PropertyEditorEngine.h"
#include "Widgets/Selector/ObjectRefWidget.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QLineEdit>
#include <QObject>
#include <QSlider>
#include <QTemporaryDir>
#include <QToolButton>
#include <QVector>

#include <memory>
#include <optional>
#include "Support/ProjectPlatformTestUtils.h"

namespace {

using VNX::PlayablePropertyAuthoringV1;
using VNX::Platform;
using VNX::PlatformContext;
using VNX::PlatformFieldView;
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
constexpr uint8_t kLoopCountId = 4;

ProjectItem makeDefinition(ProjectItem::Type type)
{
    ProjectItem item(QStringLiteral("definition"), type);
    item.guid = QUuid::createUuid();
    item.attachDefaultComponents();
    return item;
}

PlatformContext ctx(std::initializer_list<VNX::ProjectPlatformId> selected)
{
    PlatformContext context;
    for (VNX::ProjectPlatformId platform : selected)
        context.selected.append(platform);
    if (!context.selected.isEmpty())
        context.primary = context.selected.front();
    return context;
}

void expectVolume(const std::optional<VNX::PlayablePropertyStateV1>& state,
                  bool present, double value)
{
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state->present, present);
    EXPECT_DOUBLE_EQ(state->value.toDouble(), value);
}

TEST(PlatformFieldMerge, NoOverlayBaselineIsUniformNoneForAnySelection)
{
    ProjectItem sound = makeDefinition(ProjectItem::Type::Sound);
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(sound);
    ASSERT_TRUE(locator.has_value());
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        sound, *locator, kVolumeId, -6.0, std::nullopt));

    const auto shared = PlayablePropertyAuthoringV1::readProperty(
        sound, *locator, kVolumeId, std::nullopt);
    ASSERT_TRUE(shared.has_value());

    const PlatformContext selections[] = {
        ctx({HarkenTest::PlatformIds::Android}),
        ctx({HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android}),
        ctx({HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android, HarkenTest::PlatformIds::iOS})};
    for (const PlatformContext& context : selections) {
        const PlatformFieldView view = VNX::platformFieldView(
            sound, *locator, kVolumeId, context);
        EXPECT_TRUE(view.uniform);
        EXPECT_EQ(view.coverage, PlatformFieldView::Coverage::None);
        EXPECT_EQ(view.value, *shared);
        ASSERT_EQ(view.perPlatform.size(), context.selected.size());
        for (int i = 0; i < context.selected.size(); ++i) {
            EXPECT_EQ(view.perPlatform.at(i).first, context.selected.at(i));
            EXPECT_EQ(view.perPlatform.at(i).second, *shared);
        }
    }
}

TEST(PlatformFieldMerge, MatchingOverlaysAreUniformAll)
{
    ProjectItem sound = makeDefinition(ProjectItem::Type::Sound);
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(sound);
    ASSERT_TRUE(locator.has_value());
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        sound, *locator, kVolumeId, -6.0, std::nullopt));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        sound, *locator, kVolumeId, -12.0, HarkenTest::PlatformIds::Windows));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        sound, *locator, kVolumeId, -12.0, HarkenTest::PlatformIds::Android));

    const PlatformFieldView view = VNX::platformFieldView(
        sound, *locator, kVolumeId, ctx({HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android}));
    EXPECT_TRUE(view.uniform);
    EXPECT_EQ(view.coverage, PlatformFieldView::Coverage::All);
    EXPECT_DOUBLE_EQ(view.value.value.toDouble(), -12.0);
    EXPECT_TRUE(view.value.present);
}

TEST(PlatformFieldMerge, DifferingValuesAreNonUniformAndOrdered)
{
    ProjectItem sound = makeDefinition(ProjectItem::Type::Sound);
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(sound);
    ASSERT_TRUE(locator.has_value());
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        sound, *locator, kVolumeId, -6.0, std::nullopt));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        sound, *locator, kVolumeId, -12.0, HarkenTest::PlatformIds::Android));

    const PlatformContext context = ctx({HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android});
    const PlatformFieldView view = VNX::platformFieldView(
        sound, *locator, kVolumeId, context);
    EXPECT_FALSE(view.uniform);
    ASSERT_EQ(view.perPlatform.size(), 2);
    EXPECT_EQ(view.perPlatform.at(0).first, HarkenTest::PlatformIds::Windows);
    EXPECT_DOUBLE_EQ(view.perPlatform.at(0).second.value.toDouble(), -6.0);
    EXPECT_EQ(view.perPlatform.at(1).first, HarkenTest::PlatformIds::Android);
    EXPECT_DOUBLE_EQ(view.perPlatform.at(1).second.value.toDouble(), -12.0);
}

TEST(PlatformFieldMerge, CoverageNoneSomeAll)
{
    ProjectItem sound = makeDefinition(ProjectItem::Type::Sound);
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(sound);
    ASSERT_TRUE(locator.has_value());
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        sound, *locator, kVolumeId, -6.0, std::nullopt));

    EXPECT_EQ(VNX::platformFieldView(
                  sound, *locator, kVolumeId,
                  ctx({HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android}))
                  .coverage,
              PlatformFieldView::Coverage::None);

    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        sound, *locator, kVolumeId, -12.0, HarkenTest::PlatformIds::Android));
    EXPECT_EQ(VNX::platformFieldView(
                  sound, *locator, kVolumeId,
                  ctx({HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android}))
                  .coverage,
              PlatformFieldView::Coverage::Some);

    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        sound, *locator, kVolumeId, -12.0, HarkenTest::PlatformIds::Windows));
    EXPECT_EQ(VNX::platformFieldView(
                  sound, *locator, kVolumeId,
                  ctx({HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android}))
                  .coverage,
              PlatformFieldView::Coverage::All);
}

TEST(PlatformFieldMerge, EmptySelectionReturnsDefault)
{
    ProjectItem sound = makeDefinition(ProjectItem::Type::Sound);
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(sound);
    ASSERT_TRUE(locator.has_value());
    const PlatformFieldView view = VNX::platformFieldView(
        sound, *locator, kVolumeId, {});
    EXPECT_TRUE(view.uniform);
    EXPECT_EQ(view.coverage, PlatformFieldView::Coverage::None);
    EXPECT_TRUE(view.perPlatform.isEmpty());
    EXPECT_FALSE(view.value.present);
}

TEST(PlatformFieldMerge, DefinitionsKeepPlatformValuesWhileOccurrencesRejectAndIgnoreOverrides)
{
    ProjectItem sound = makeDefinition(ProjectItem::Type::Sound);
    const auto definition = PlayablePropertyAuthoringV1::definitionLocator(sound);
    ASSERT_TRUE(definition.has_value());
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        sound, *definition, kVolumeId, -6.0, std::nullopt));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        sound, *definition, kVolumeId, -12.0, HarkenTest::PlatformIds::Android));
    const PlatformFieldView definitionView = VNX::platformFieldView(
        sound, *definition, kVolumeId, ctx({HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android}));
    EXPECT_FALSE(definitionView.uniform);

    ProjectItem track = makeDefinition(ProjectItem::Type::Track);
    auto* timeline = track.comp<VNX::VTrack>();
    ASSERT_NE(timeline, nullptr);
    VNX::Clip clip;
    clip.clipGuid = QUuid::createUuid();
    clip.targetGuid = sound.guid;
    clip.playableProperties.presence.Set(harkenplayableproperties::PlayablePropertyIdV1::Volume);
    ASSERT_TRUE(clip.playableProperties.setValue(harkenplayableproperties::PlayablePropertyIdV1::Volume, -3.0f));
    auto* clipOverlay = clip.platformOverrides.ensure(HarkenTest::PlatformIds::Android);
    ASSERT_NE(clipOverlay, nullptr);
    clipOverlay->properties.presence.Set(harkenplayableproperties::PlayablePropertyIdV1::Volume);
    ASSERT_TRUE(clipOverlay->properties.setValue(harkenplayableproperties::PlayablePropertyIdV1::Volume, -9.0f));
    ASSERT_TRUE(timeline->clips.mintOccurrenceOrdinal(&clip));
    timeline->clips.items.push_back(clip);
    const auto clipLocator = PlayablePropertyAuthoringV1::trackClipLocator(
        track, clip.clipGuid);
    ASSERT_TRUE(clipLocator.has_value());
    EXPECT_FALSE(PlayablePropertyAuthoringV1::setOverride(
        track, *clipLocator, kVolumeId, -3.0, std::nullopt));
    EXPECT_FALSE(PlayablePropertyAuthoringV1::setOverride(
        track, *clipLocator, kVolumeId, -9.0, HarkenTest::PlatformIds::Android));
    const PlatformFieldView clipView = VNX::platformFieldView(
        track, *clipLocator, kVolumeId, ctx({HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android}));
    EXPECT_TRUE(clipView.uniform);
    EXPECT_EQ(clipView.coverage, PlatformFieldView::Coverage::None);
    EXPECT_FALSE(clipView.value.present);
    EXPECT_EQ(clipView.value.value, QJsonValue(0.0));

    ProjectItem container = makeDefinition(ProjectItem::Type::Container);
    const QUuid childGuid = QUuid::createUuid();
    container.comp<VNX::VSelection>()->selections.shared.occurrenceOrdinalHighWater = 1;
    container.comp<VNX::VSelection>()->selections.shared.setOccurrenceRows({{childGuid, 0}});
    const auto childLocator = PlayablePropertyAuthoringV1::containerChildLocator(
        container, childGuid);
    ASSERT_TRUE(childLocator.has_value());
    EXPECT_FALSE(PlayablePropertyAuthoringV1::setOverride(
        container, *childLocator, kVolumeId, -1.0, std::nullopt));
    EXPECT_FALSE(PlayablePropertyAuthoringV1::setOverride(
        container, *childLocator, kVolumeId, -8.0, HarkenTest::PlatformIds::Android));
    const PlatformFieldView childView = VNX::platformFieldView(
        container, *childLocator, kVolumeId,
        ctx({HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android}));
    EXPECT_TRUE(childView.uniform);
    EXPECT_EQ(childView.coverage, PlatformFieldView::Coverage::None);
    EXPECT_FALSE(childView.value.present);
    EXPECT_EQ(childView.value.value, QJsonValue(0.0));
}

TEST(PlatformFieldMerge, MixedTooltipListsPlatformAndValue)
{
    ProjectItem sound = makeDefinition(ProjectItem::Type::Sound);
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(sound);
    ASSERT_TRUE(locator.has_value());
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        sound, *locator, kVolumeId, -6.0, std::nullopt));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        sound, *locator, kVolumeId, -12.0, HarkenTest::PlatformIds::Android));
    const PlatformFieldView view = VNX::platformFieldView(
        sound, *locator, kVolumeId, ctx({HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android}));
    const QString tooltip = VNX::platformFieldMixedTooltip(view, HarkenTest::platforms(
        {QStringLiteral("Windows"), QStringLiteral("Android")}));
    EXPECT_TRUE(tooltip.contains(QStringLiteral("Windows")));
    EXPECT_TRUE(tooltip.contains(QStringLiteral("-6")));
    EXPECT_TRUE(tooltip.contains(QStringLiteral("Android")));
    EXPECT_TRUE(tooltip.contains(QStringLiteral("-12")));
}

TEST(ParameterSlider, MixedValueLeavesEditorEmptyWithoutEmittingValueChanged)
{
    ensureApp();
    VNX::ParameterSlider slider(QStringLiteral("Volume"));
    slider.setRange(-96.0, 24.0, 1);
    slider.setValue(-12.0);
    int valueChanges = 0;
    QObject::connect(&slider, &VNX::ParameterSlider::valueChanged,
                     [&valueChanges](double) { ++valueChanges; });
    slider.setMixedValue(true);
    EXPECT_TRUE(slider.mixedValue());
    auto* editor = slider.presenter() ? slider.presenter()->valueEditor() : nullptr;
    ASSERT_NE(editor, nullptr);
    auto* line = editor->findChild<QLineEdit*>();
    ASSERT_NE(line, nullptr);
    EXPECT_TRUE(line->text().isEmpty());
    EXPECT_EQ(valueChanges, 0);
    EXPECT_DOUBLE_EQ(slider.value(), -12.0);

    auto* track = slider.findChild<QSlider*>();
    ASSERT_NE(track, nullptr);
    track->setSliderDown(true);
    track->setValue(track->maximum());
    track->setSliderDown(false);
    EXPECT_FALSE(slider.mixedValue());
    EXPECT_FALSE(line->text().isEmpty());
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

TEST(ParameterSlider, MixedValueRestoresSuffixAfterLeavingMixedByDrag)
{
    ensureApp();
    VNX::ParameterSlider slider(QStringLiteral("Volume"));
    slider.setRange(-96.0, 24.0, 1);
    slider.setSuffix(QStringLiteral(" dB"));
    slider.setValue(-12.0);
    auto* editor = slider.presenter() ? slider.presenter()->valueEditor() : nullptr;
    ASSERT_NE(editor, nullptr);
    EXPECT_TRUE(editor->suffix().isEmpty());
    slider.setMixedValue(true);
    EXPECT_TRUE(editor->suffix().isEmpty());

    auto* track = slider.findChild<QSlider*>();
    ASSERT_NE(track, nullptr);
    track->setSliderDown(true);
    track->setValue(track->maximum());
    track->setSliderDown(false);
    EXPECT_FALSE(slider.mixedValue());
    EXPECT_TRUE(editor->suffix().isEmpty());
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
    auto* editor = slider.presenter() ? slider.presenter()->valueEditor() : nullptr;
    ASSERT_NE(editor, nullptr);
    auto* line = editor->findChild<QLineEdit*>();
    ASSERT_NE(line, nullptr);
    EXPECT_TRUE(line->text().isEmpty());
    EXPECT_EQ(valueChanges, 0);
    EXPECT_EQ(committed, 0);
}

} // namespace

namespace VNX {

class PlatformFieldMergeInspector : public ::testing::Test
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
        config.name = QStringLiteral("PlatformFieldInspector");
        config.location = tempDir.path();
        config.platforms = HarkenTest::platforms(QStringList{
            QStringLiteral("Windows"), QStringLiteral("Android"),
            QStringLiteral("iOS")});
        ASSERT_TRUE(pm.createProject(config));
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::registerProjectActions(
            VNX::ActionManager::instance(), &pm, nullptr, {});
        VNX::UndoRedoManager::instance().clear();
        VNX::PlatformContextService::instance().setSelected(
            {HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android, HarkenTest::PlatformIds::iOS},
            HarkenTest::PlatformIds::Windows, true);
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

    std::optional<VNX::PlayablePropertyLocatorV1> locatorOf(ProjectItem* sound)
    {
        return PlayablePropertyAuthoringV1::definitionLocator(*sound);
    }

    bool overlayPresent(ProjectItem* sound, VNX::ProjectPlatformId platform, uint8_t propertyId)
    {
        const auto overlay = PlayablePropertyAuthoringV1::readPlatformOverlay(
            *sound, *locatorOf(sound), propertyId, platform);
        return overlay && overlay->present;
    }

    QTemporaryDir tempDir;
};

TEST_F(PlatformFieldMergeInspector, RootSpatialCheckboxEditsCurrentGroupAndRetainsInheritanceFlag)
{
    ProjectItem* sound = makeSound(QStringLiteral("CheckMix"));
    ASSERT_NE(sound, nullptr);
    auto groups = sound->fieldGroups;
    SpatialRatingFieldGroup android;
    android.spatialIs3DEnabled = true;
    groups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        FieldGroupMode::Local, android};
    groups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Windows] = {
        FieldGroupMode::Local, SpatialRatingFieldGroup{}};
    ASSERT_TRUE(ProjectManager::instance().restoreFieldGroupsInternal(sound, groups));
    ASSERT_TRUE(groups.spatialRating.shared.localHistory);
    EXPECT_EQ(groups.spatialRating.shared.mode, FieldGroupMode::Inherit);
    const auto presence = sound->playablePropertyPresence.bits;
    const auto overlays = sound->platformOverrides;
    const auto properties = PlayablePropertyAuthoringV1::definitionToJson(*sound);
    UndoRedoManager::instance().clear();

    VNX::PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android, HarkenTest::PlatformIds::iOS},
        HarkenTest::PlatformIds::Windows, true);

    VNX::InspectorPanel panel;
    panel.showItemInspector(sound);
    auto* check = panel.findChild<QCheckBox*>(
        QStringLiteral("prop_spatialIs3DEnabled"));
    ASSERT_NE(check, nullptr);
    int callbacks = 0;
    QObject receiver;
    QObject::connect(&panel, &InspectorPanel::propertyChanged, &receiver,
                     [&callbacks] { ++callbacks; });
    EXPECT_TRUE(check->isEnabled());
    EXPECT_EQ(check->checkState(), Qt::PartiallyChecked);
    check->click();
    auto expected = groups;
    expected.spatialRating.shared.localHistory->spatialIs3DEnabled = true;
    EXPECT_EQ(callbacks, 1);
    EXPECT_EQ(sound->fieldGroups.spatialRating.shared, expected.spatialRating.shared);
    for (const auto platform : {HarkenTest::FieldGroupPlatformIds::Windows, HarkenTest::FieldGroupPlatformIds::Android}) {
        const auto local = ResolveFieldGroupLocal(sound->fieldGroups.spatialRating, platform);
        EXPECT_EQ(local.mode, FieldGroupMode::Local);
        ASSERT_TRUE(local.localHistory);
        EXPECT_TRUE(local.localHistory->spatialIs3DEnabled);
        EXPECT_FALSE(sound->fieldGroups.spatialRating.platforms.at(platform).localHistory);
    }
    EXPECT_EQ(sound->playablePropertyPresence.bits, presence);
    EXPECT_EQ(sound->platformOverrides, overlays);
    EXPECT_EQ(PlayablePropertyAuthoringV1::definitionToJson(*sound), properties);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(sound->fieldGroups, groups);
}

TEST_F(PlatformFieldMergeInspector, AllShowsMixedComboWithoutAddingSentinelItems)
{
    ProjectItem* sound = makeSound(QStringLiteral("ComboMix"));
    ASSERT_NE(sound, nullptr);
    const auto locator = locatorOf(sound);
    ASSERT_TRUE(locator.has_value());
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *sound, *locator, kLoopCountId, 0, HarkenTest::PlatformIds::Windows));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *sound, *locator, kLoopCountId, 1, HarkenTest::PlatformIds::Android));
    VNX::PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android, HarkenTest::PlatformIds::iOS},
        HarkenTest::PlatformIds::Windows, true);

    VNX::PropertySchema schema =
        VNX::contentObjectInspectorRows(ProjectItem::Type::Sound);
    VNX::PropertyDescriptor* loopCount = nullptr;
    for (VNX::PropertyDescriptor& row : schema)
        if (row.field == QLatin1String("loopCount")) {
            loopCount = &row;
            break;
        }
    ASSERT_NE(loopCount, nullptr);
    loopCount->type = VNX::PropType::Enum;
    loopCount->flags.segmented = false;
    loopCount->segments.clear();
    loopCount->options = {{QStringLiteral("Infinite"), 0}, {QStringLiteral("Once"), 1}};

    VNX::PropertyEditorEngine engine;
    engine.setSchema(schema, VNX::ComponentType::Audio);
    engine.setItemType(ProjectItem::Type::Sound);
    std::unique_ptr<QWidget> page(engine.buildPage());
    engine.bindItem(sound);
    auto* combo = page->findChild<QComboBox*>(
        QStringLiteral("prop_loopCount"));
    ASSERT_NE(combo, nullptr);
    const int countBefore = combo->count();
    EXPECT_EQ(combo->currentIndex(), -1);
    EXPECT_EQ(combo->count(), countBefore);
    EXPECT_FALSE(combo->toolTip().isEmpty());
    VNX::PlatformContextService::instance().setAll(false);
    EXPECT_EQ(combo->currentData().toInt(), 0);
    VNX::PlatformContextService::instance().setAll(true);
    EXPECT_EQ(combo->currentIndex(), -1);
    EXPECT_EQ(combo->count(), countBefore);
}

TEST_F(PlatformFieldMergeInspector, InheritedPlatformAttenuationRejectsLateReferenceCallback)
{
    auto& pm = VNX::ProjectManager::instance();
    ProjectItem* layer = pm.createItem(
        nullptr, QStringLiteral("ParentLayer"), ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    ProjectItem* sound = pm.createItem(
        layer, QStringLiteral("ChildSound"), ProjectItem::Type::Sound);
    ProjectItem* first = pm.createItem(
        nullptr, QStringLiteral("FirstAttenuation"), ProjectItem::Type::Attenuation);
    ProjectItem* second = pm.createItem(
        nullptr, QStringLiteral("SecondAttenuation"), ProjectItem::Type::Attenuation);
    ASSERT_NE(sound, nullptr);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    auto parentGroups = layer->fieldGroups;
    SpatialRatingFieldGroup firstRating;
    firstRating.spatialAttenuationRef = toFieldGroupGuid(first->guid);
    SpatialRatingFieldGroup secondRating;
    secondRating.spatialAttenuationRef = toFieldGroupGuid(second->guid);
    parentGroups.spatialRating.shared = {FieldGroupMode::Local, firstRating};
    parentGroups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Windows] = {
        FieldGroupMode::Local, firstRating};
    parentGroups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        FieldGroupMode::Local, secondRating};
    ASSERT_TRUE(pm.restoreFieldGroupsInternal(layer, parentGroups));
    auto childGroups = sound->fieldGroups;
    childGroups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Windows] = {
        FieldGroupMode::Inherit, {}};
    ASSERT_TRUE(pm.restoreFieldGroupsInternal(sound, childGroups));
    const auto presence = sound->playablePropertyPresence.bits;
    const auto overlays = sound->platformOverrides;
    const auto properties = PlayablePropertyAuthoringV1::definitionToJson(*sound);
    UndoRedoManager::instance().clear();
    PlatformContextService::instance().setSelected({HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);

    VNX::InspectorPanel panel;
    panel.showItemInspector(sound);
    auto* reference = panel.findChild<VNX::ObjectRefWidget*>(
        QStringLiteral("prop_spatialAttenuationRef"));
    ASSERT_NE(reference, nullptr);
    int callbacks = 0;
    QObject receiver;
    QObject::connect(&panel, &InspectorPanel::propertyChanged, &receiver,
                     [&callbacks] { ++callbacks; });
    EXPECT_TRUE(reference->isReadOnly());
    reference->objectChanged(0);
    EXPECT_EQ(callbacks, 0);
    EXPECT_EQ(sound->fieldGroups, childGroups);
    EXPECT_EQ(layer->fieldGroups, parentGroups);
    EXPECT_EQ(sound->playablePropertyPresence.bits, presence);
    EXPECT_EQ(sound->platformOverrides, overlays);
    EXPECT_EQ(PlayablePropertyAuthoringV1::definitionToJson(*sound), properties);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);

    childGroups.spatialRating.shared = {
        FieldGroupMode::Local, SpatialRatingFieldGroup{}};
    ASSERT_TRUE(pm.restoreFieldGroupsInternal(sound, childGroups));
    EXPECT_TRUE(reference->isReadOnly());
    reference->objectChanged(second->objectID);
    EXPECT_EQ(callbacks, 0);
    EXPECT_EQ(sound->fieldGroups, childGroups);
    EXPECT_EQ(layer->fieldGroups, parentGroups);
    EXPECT_EQ(sound->playablePropertyPresence.bits, presence);
    EXPECT_EQ(sound->platformOverrides, overlays);
    EXPECT_EQ(PlayablePropertyAuthoringV1::definitionToJson(*sound), properties);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(PlatformFieldMergeInspector, AllShowsMixedSliderForDifferentPlatformValues)
{
    ProjectItem* sound = makeSound(QStringLiteral("SliderMix"));
    ASSERT_NE(sound, nullptr);
    const auto locator = locatorOf(sound);
    ASSERT_TRUE(locator.has_value());
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *sound, *locator, kVolumeId, -6.0, std::nullopt));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *sound, *locator, kVolumeId, -12.0, HarkenTest::PlatformIds::Android));
    VNX::PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android, HarkenTest::PlatformIds::iOS},
        HarkenTest::PlatformIds::Windows, true);

    VNX::PropertyEditorEngine engine;
    engine.setSchema(VNX::contentObjectInspectorRows(ProjectItem::Type::Sound),
                     VNX::ComponentType::Audio);
    engine.setItemType(ProjectItem::Type::Sound);
    std::unique_ptr<QWidget> page(engine.buildPage());
    engine.bindItem(sound);
    auto* slider = page->findChild<VNX::ParameterSlider*>(
        QStringLiteral("prop_volume"));
    ASSERT_NE(slider, nullptr);
    EXPECT_TRUE(slider->mixedValue());
    auto* editor = slider->presenter() ? slider->presenter()->valueEditor() : nullptr;
    ASSERT_NE(editor, nullptr);
    auto* line = editor->findChild<QLineEdit*>();
    ASSERT_NE(line, nullptr);
    EXPECT_TRUE(line->text().isEmpty());
    EXPECT_FALSE(slider->toolTip().isEmpty());
}

TEST_F(PlatformFieldMergeInspector, SinglePlatformWriteTouchesOnlyThatPlatform)
{
    ProjectItem* sound = makeSound(QStringLiteral("SingleWrite"));
    ASSERT_NE(sound, nullptr);
    const auto locator = locatorOf(sound);
    ASSERT_TRUE(locator.has_value());
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *sound, *locator, kVolumeId, -6.0, std::nullopt));
    VNX::PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Android, false);

    VNX::InspectorPanel panel;
    panel.showItemInspector(sound);
    auto* slider = panel.findChild<VNX::ParameterSlider*>(
        QStringLiteral("prop_volume"));
    ASSERT_NE(slider, nullptr);
    slider->binding()->beginEdit();
    slider->binding()->previewEdit(-18.0);
    slider->binding()->commitEdit();

    expectVolume(PlayablePropertyAuthoringV1::readProperty(
                     *sound, *locator, kVolumeId, HarkenTest::PlatformIds::Android),
                 true, -18.0);
    expectVolume(PlayablePropertyAuthoringV1::readProperty(
                     *sound, *locator, kVolumeId, std::nullopt),
                 true, -6.0);
    expectVolume(PlayablePropertyAuthoringV1::readProperty(
                     *sound, *locator, kVolumeId, HarkenTest::PlatformIds::Windows),
                 true, -6.0);
    EXPECT_TRUE(overlayPresent(sound, HarkenTest::PlatformIds::Android, kVolumeId));
    EXPECT_FALSE(overlayPresent(sound, HarkenTest::PlatformIds::Windows, kVolumeId));
}

TEST_F(PlatformFieldMergeInspector, SingleTargetWithAllOffWritesOnlyItsOverlay)
{
    ProjectItem* sound = makeSound(QStringLiteral("PartialWrite"));
    ASSERT_NE(sound, nullptr);
    const auto locator = locatorOf(sound);
    ASSERT_TRUE(locator.has_value());
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *sound, *locator, kVolumeId, -6.0, std::nullopt));
    VNX::ProjectManager::instance().setProjectPlatformsInternal(
        HarkenTest::platforms({QStringLiteral("Android")}));
    VNX::PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Android, false);

    VNX::InspectorPanel panel;
    panel.showItemInspector(sound);
    auto* slider = panel.findChild<VNX::ParameterSlider*>(
        QStringLiteral("prop_volume"));
    ASSERT_NE(slider, nullptr);
    slider->binding()->beginEdit();
    slider->binding()->previewEdit(-18.0);
    slider->binding()->commitEdit();

    expectVolume(PlayablePropertyAuthoringV1::readProperty(
                     *sound, *locator, kVolumeId, HarkenTest::PlatformIds::Android),
                 true, -18.0);
    expectVolume(PlayablePropertyAuthoringV1::readProperty(
                     *sound, *locator, kVolumeId, HarkenTest::PlatformIds::Windows),
                 true, -6.0);
    expectVolume(PlayablePropertyAuthoringV1::readProperty(
                     *sound, *locator, kVolumeId, HarkenTest::PlatformIds::iOS),
                 true, -6.0);
    expectVolume(PlayablePropertyAuthoringV1::readProperty(
                     *sound, *locator, kVolumeId, std::nullopt),
                 true, -6.0);
}

TEST_F(PlatformFieldMergeInspector, AllWritesSharedAndClearsEditedFieldOverlays)
{
    ProjectItem* sound = makeSound(QStringLiteral("AllWrite"));
    ASSERT_NE(sound, nullptr);
    const auto locator = locatorOf(sound);
    ASSERT_TRUE(locator.has_value());
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *sound, *locator, kVolumeId, -6.0, std::nullopt));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *sound, *locator, kVolumeId, -12.0, HarkenTest::PlatformIds::Android));
    VNX::PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android, HarkenTest::PlatformIds::iOS},
        HarkenTest::PlatformIds::Windows, true);

    VNX::InspectorPanel panel;
    panel.showItemInspector(sound);
    auto* slider = panel.findChild<VNX::ParameterSlider*>(
        QStringLiteral("prop_volume"));
    ASSERT_NE(slider, nullptr);
    EXPECT_TRUE(slider->mixedValue());
    slider->binding()->beginEdit();
    slider->binding()->previewEdit(-3.0);
    slider->binding()->commitEdit();

    expectVolume(PlayablePropertyAuthoringV1::readProperty(
                     *sound, *locator, kVolumeId, std::nullopt),
                 true, -3.0);
    EXPECT_FALSE(overlayPresent(sound, HarkenTest::PlatformIds::Windows, kVolumeId));
    EXPECT_FALSE(overlayPresent(sound, HarkenTest::PlatformIds::Android, kVolumeId));
    EXPECT_FALSE(overlayPresent(sound, HarkenTest::PlatformIds::iOS, kVolumeId));
}

TEST_F(PlatformFieldMergeInspector, DragAndTypedCommitLandTheSame)
{
    ProjectItem* sound = makeSound(QStringLiteral("TwoPaths"));
    ASSERT_NE(sound, nullptr);
    const auto locator = locatorOf(sound);
    ASSERT_TRUE(locator.has_value());
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *sound, *locator, kVolumeId, -6.0, std::nullopt));
    VNX::PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Android, false);

    VNX::InspectorPanel panel;
    panel.showItemInspector(sound);
    auto* slider = panel.findChild<VNX::ParameterSlider*>(
        QStringLiteral("prop_volume"));
    ASSERT_NE(slider, nullptr);

    slider->binding()->beginEdit();
    slider->binding()->previewEdit(-18.0);
    slider->binding()->commitEdit();
    expectVolume(PlayablePropertyAuthoringV1::readProperty(
                     *sound, *locator, kVolumeId, HarkenTest::PlatformIds::Android),
                 true, -18.0);
    EXPECT_TRUE(overlayPresent(sound, HarkenTest::PlatformIds::Android, kVolumeId));
    expectVolume(PlayablePropertyAuthoringV1::readProperty(
                     *sound, *locator, kVolumeId, std::nullopt),
                 true, -6.0);

    ASSERT_TRUE(PlayablePropertyAuthoringV1::clearOverride(
        *sound, *locator, kVolumeId, HarkenTest::PlatformIds::Android));
    panel.showItemInspector(sound);
    slider = panel.findChild<VNX::ParameterSlider*>(QStringLiteral("prop_volume"));
    ASSERT_NE(slider, nullptr);
    slider->binding()->commitValue(-18.0);
    expectVolume(PlayablePropertyAuthoringV1::readProperty(
                     *sound, *locator, kVolumeId, HarkenTest::PlatformIds::Android),
                 true, -18.0);
    EXPECT_TRUE(overlayPresent(sound, HarkenTest::PlatformIds::Android, kVolumeId));
    expectVolume(PlayablePropertyAuthoringV1::readProperty(
                     *sound, *locator, kVolumeId, std::nullopt),
                 true, -6.0);
}

TEST_F(PlatformFieldMergeInspector, ContextChangeRefreshesBoundField)
{
    ProjectItem* sound = makeSound(QStringLiteral("ContextRefresh"));
    ASSERT_NE(sound, nullptr);
    const auto locator = locatorOf(sound);
    ASSERT_TRUE(locator.has_value());
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *sound, *locator, kVolumeId, -6.0, std::nullopt));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *sound, *locator, kVolumeId, -12.0, HarkenTest::PlatformIds::Android));

    VNX::PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
    VNX::PropertyEditorEngine engine;
    engine.setSchema(VNX::contentObjectInspectorRows(ProjectItem::Type::Sound),
                     VNX::ComponentType::Audio);
    engine.setItemType(ProjectItem::Type::Sound);
    std::unique_ptr<QWidget> page(engine.buildPage());
    engine.bindItem(sound);
    auto* slider = page->findChild<VNX::ParameterSlider*>(
        QStringLiteral("prop_volume"));
    ASSERT_NE(slider, nullptr);
    EXPECT_FALSE(slider->mixedValue());
    EXPECT_DOUBLE_EQ(slider->value(), -6.0);

    VNX::PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android, HarkenTest::PlatformIds::iOS},
        HarkenTest::PlatformIds::Windows, true);
    EXPECT_TRUE(slider->mixedValue());
}

TEST_F(PlatformFieldMergeInspector, AllShowsMixedSegmentsWithoutCommitting)
{
    ProjectItem* sound = makeSound(QStringLiteral("SegMix"));
    ASSERT_NE(sound, nullptr);
    const auto locator = locatorOf(sound);
    ASSERT_TRUE(locator.has_value());
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *sound, *locator, kLoopCountId, 3, HarkenTest::PlatformIds::Windows));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *sound, *locator, kLoopCountId, 0, HarkenTest::PlatformIds::Android));
    const auto windowsBefore = PlayablePropertyAuthoringV1::readPlatformOverlay(
        *sound, *locator, kLoopCountId, HarkenTest::PlatformIds::Windows);
    const auto androidBefore = PlayablePropertyAuthoringV1::readPlatformOverlay(
        *sound, *locator, kLoopCountId, HarkenTest::PlatformIds::Android);
    const auto sharedBefore = PlayablePropertyAuthoringV1::readProperty(
        *sound, *locator, kLoopCountId, std::nullopt);
    VNX::PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android, HarkenTest::PlatformIds::iOS},
        HarkenTest::PlatformIds::Windows, true);

    VNX::PropertyEditorEngine engine;
    engine.setSchema(VNX::contentObjectInspectorRows(ProjectItem::Type::Sound),
                     VNX::ComponentType::Audio);
    engine.setItemType(ProjectItem::Type::Sound);
    std::unique_ptr<QWidget> page(engine.buildPage());
    auto* loop = page->findChild<VNX::ValueSegmentedControl*>(
        QStringLiteral("prop_loopCount"));
    ASSERT_NE(loop, nullptr);
    int picked = 0;
    QObject::connect(loop, &VNX::ValueSegmentedControl::segmentPicked,
                     [&picked](const QVariant&) { ++picked; });
    engine.bindItem(sound);

    for (auto* button : loop->findChildren<QToolButton*>())
        EXPECT_FALSE(button->isChecked());
    EXPECT_FALSE(loop->toolTip().isEmpty());
    EXPECT_EQ(picked, 0);
    EXPECT_EQ(PlayablePropertyAuthoringV1::readPlatformOverlay(
                  *sound, *locator, kLoopCountId, HarkenTest::PlatformIds::Windows),
              windowsBefore);
    EXPECT_EQ(PlayablePropertyAuthoringV1::readPlatformOverlay(
                  *sound, *locator, kLoopCountId, HarkenTest::PlatformIds::Android),
              androidBefore);
    EXPECT_EQ(PlayablePropertyAuthoringV1::readProperty(
                  *sound, *locator, kLoopCountId, std::nullopt),
              sharedBefore);
}

TEST_F(PlatformFieldMergeInspector, AllRebindClearsSegmentSelectionForMixedValues)
{
    ProjectItem* uniform = makeSound(QStringLiteral("SegUniform"));
    ProjectItem* mixed = makeSound(QStringLiteral("SegMixed"));
    ASSERT_NE(uniform, nullptr);
    ASSERT_NE(mixed, nullptr);
    const auto uniformLocator = locatorOf(uniform);
    const auto mixedLocator = locatorOf(mixed);
    ASSERT_TRUE(uniformLocator.has_value());
    ASSERT_TRUE(mixedLocator.has_value());
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *uniform, *uniformLocator, kLoopCountId, 1, std::nullopt));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *mixed, *mixedLocator, kLoopCountId, 2, std::nullopt));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *mixed, *mixedLocator, kLoopCountId, 3, HarkenTest::PlatformIds::Windows));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *mixed, *mixedLocator, kLoopCountId, 0, HarkenTest::PlatformIds::Android));
    VNX::PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android, HarkenTest::PlatformIds::iOS},
        HarkenTest::PlatformIds::Windows, true);

    VNX::PropertyEditorEngine engine;
    engine.setSchema(VNX::contentObjectInspectorRows(ProjectItem::Type::Sound),
                     VNX::ComponentType::Audio);
    engine.setItemType(ProjectItem::Type::Sound);
    std::unique_ptr<QWidget> page(engine.buildPage());
    auto* loop = page->findChild<VNX::ValueSegmentedControl*>(
        QStringLiteral("prop_loopCount"));
    ASSERT_NE(loop, nullptr);

    engine.bindItem(uniform);
    QToolButton* numeric = nullptr;
    for (auto* button : loop->findChildren<QToolButton*>()) {
        if (button->property("segmentPosition").toString() == QLatin1String("first"))
            numeric = button;
    }
    ASSERT_NE(numeric, nullptr);
    EXPECT_TRUE(numeric->isChecked());
    EXPECT_EQ(numeric->text(), QStringLiteral("1"));

    engine.bindItem(mixed);
    EXPECT_FALSE(numeric->isChecked());
    EXPECT_EQ(numeric->text(), QStringLiteral("1"));
}

TEST_F(PlatformFieldMergeInspector, AllAndCurrentPlatformSelectTheirOwnSegments)
{
    ProjectItem* sound = makeSound(QStringLiteral("SegRestore"));
    ASSERT_NE(sound, nullptr);
    const auto locator = locatorOf(sound);
    ASSERT_TRUE(locator.has_value());
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *sound, *locator, kLoopCountId, 3, HarkenTest::PlatformIds::Windows));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *sound, *locator, kLoopCountId, 0, HarkenTest::PlatformIds::Android));
    VNX::PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android, HarkenTest::PlatformIds::iOS},
        HarkenTest::PlatformIds::Windows, true);

    VNX::PropertyEditorEngine engine;
    engine.setSchema(VNX::contentObjectInspectorRows(ProjectItem::Type::Sound),
                     VNX::ComponentType::Audio);
    engine.setItemType(ProjectItem::Type::Sound);
    std::unique_ptr<QWidget> page(engine.buildPage());
    engine.bindItem(sound);
    auto* loop = page->findChild<VNX::ValueSegmentedControl*>(
        QStringLiteral("prop_loopCount"));
    ASSERT_NE(loop, nullptr);
    for (auto* button : loop->findChildren<QToolButton*>())
        EXPECT_FALSE(button->isChecked());
    EXPECT_FALSE(loop->toolTip().isEmpty());

    VNX::PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
    QToolButton* numeric = nullptr;
    for (auto* button : loop->findChildren<QToolButton*>()) {
        if (button->property("segmentPosition").toString() == QLatin1String("first"))
            numeric = button;
    }
    ASSERT_NE(numeric, nullptr);
    EXPECT_TRUE(numeric->isChecked());
    EXPECT_EQ(numeric->text(), QStringLiteral("3"));
    EXPECT_TRUE(loop->toolTip().isEmpty());
    VNX::PlatformContextService::instance().setAll(true);
    EXPECT_FALSE(numeric->isChecked());
    for (auto* button : loop->findChildren<QToolButton*>())
        EXPECT_FALSE(button->isChecked());
}

TEST_F(PlatformFieldMergeInspector, PlatformChangesKeepSelectionRowsFreeOfPlaybackOverrides)
{
    auto& pm = ProjectManager::instance();
    pm.setProjectPlatformsInternal(HarkenTest::platforms({QStringLiteral("Windows")}));
    auto& platforms = PlatformContextService::instance();
    platforms.setAll(true);
    ProjectItem* container = pm.createItem(
        nullptr, QStringLiteral("OccurrenceContainer"), ProjectItem::Type::Container);
    ASSERT_NE(container, nullptr);
    ProjectItem* child = pm.createItem(
        container, QStringLiteral("OccurrenceChild"), ProjectItem::Type::Sound);
    ASSERT_NE(child, nullptr);
    const auto original = container->comp<VNX::VSelection>()->selections.shared;
    UndoRedoManager::instance().clear();

    InspectorPanel panel;
    panel.showItemInspector(container);
    for (const bool all : {true, false, true}) {
        platforms.setAll(all);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        auto* list = panel.findChild<QWidget*>(QStringLiteral("selectionChildrenList"));
        ASSERT_NE(list, nullptr);
        EXPECT_EQ(list->findChildren<ObjectRefWidget*>().size(), 1);
        EXPECT_NE(list->findChild<QWidget*>(QStringLiteral("selectionBehaviorWeight")), nullptr);
        EXPECT_TRUE(list->findChildren<ParameterControlHost*>().isEmpty());
        EXPECT_EQ(list->findChild<QToolButton*>(QStringLiteral("selectionChildExpander")), nullptr);
        EXPECT_EQ(container->comp<VNX::VSelection>()->selections.shared, original);
        EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
    }
}

} // namespace VNX
