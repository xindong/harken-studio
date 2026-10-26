#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Support/ProjectPlatformTestUtils.h"
#include "Core/Components/VSelection.h"
#include "ProjectManager.h"
#include "PlatformContextService.h"
#include "PropertyEditSession.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"
#include "ActionManager.h"
#include "FieldGroupProjectAdapter.h"
#include "Components/VAudio.h"
#include "Components/VMixNode.h"
#include "Components/VSpatial.h"
#include "Components/VBus.h"
#include "Components/VTrack.h"

namespace VNX {
ActionResult handleAudioItemProperty(ProjectManager*, ProjectItem*, const QString&,
                                     const QJsonObject&, const QObject*);
ActionResult handlePlayableItemProperty(ProjectManager*, ProjectItem*, const QString&,
                                        const QJsonObject&, const QObject*);
}

#include <gtest/gtest.h>

#include <QApplication>
#include <QObject>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUuid>
#include <QVector>

#include <memory>
#include <limits>
#include <utility>
#include <vector>

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

struct SignalCounters
{
    int itemPropertyChanged = 0;
    int busChanged = 0;
    QMetaObject::Connection itemConn;
    QMetaObject::Connection busConn;

    SignalCounters()
    {
        auto& pm = VNX::ProjectManager::instance();
        itemConn = QObject::connect(&pm, &VNX::ProjectManager::itemPropertyChanged,
            [this](VNX::ProjectItem*, const QString&) { ++itemPropertyChanged; });
        busConn = QObject::connect(&pm, &VNX::ProjectManager::busChanged,
            [this](uint32_t, const QString&) { ++busChanged; });
    }
    ~SignalCounters()
    {
        QObject::disconnect(itemConn);
        QObject::disconnect(busConn);
    }
};

int undoStackCount()
{
    return VNX::UndoRedoManager::instance().undoStack()->count();
}

std::shared_ptr<VNX::ProjectItem> makeAudioItem()
{
    auto item = std::make_shared<VNX::ProjectItem>(QStringLiteral("TestSound"),
                                                   VNX::ProjectItem::Type::Sound);
    item->compOrCreate<VNX::VAudio>();
    item->compOrCreate<VNX::VMixNode>();
    return item;
}

std::shared_ptr<VNX::ProjectItem> makeBusItem()
{
    auto item = std::make_shared<VNX::ProjectItem>(QStringLiteral("TestBus"),
                                                   VNX::ProjectItem::Type::Bus);
    item->objectID = 42;
    item->guid = QUuid::createUuid();
    item->compOrCreate<VNX::VBus>();
    item->compOrCreate<VNX::VMixNode>();
    return item;
}

VNX::ActionResult setFieldGroupField(VNX::ProjectItem* item, const QString& group,
                                      const QString& field, const QJsonValue& value)
{
    return VNX::ActionManager::instance().execute(
        QStringLiteral("set_field_group_field"), QJsonObject{
            {QStringLiteral("definition_guid"), item->guid.toString(QUuid::WithoutBraces)},
            {QStringLiteral("group"), group},
            {QStringLiteral("scope"), QStringLiteral("Shared")},
            {QStringLiteral("field"), field},
            {QStringLiteral("value"), value}}, VNX::ActionSource::IPC);
}

} // namespace

TEST(PropertyLiveApply, ItemWriteHasNoBroadcastDirtyOrUndo)
{
    auto item = makeAudioItem();
    SignalCounters counters;
    const int undoBefore = undoStackCount();

    EXPECT_TRUE(VNX::ProjectManager::instance().applyItemPropertyLive(
        item.get(), QStringLiteral("volume"), -6.0f));

    EXPECT_FLOAT_EQ(item->comp<VNX::VMixNode>()->volume, -6.0f);
    EXPECT_FALSE(item->isDirtyObject());
    EXPECT_EQ(counters.itemPropertyChanged, 0);
    EXPECT_EQ(counters.busChanged, 0);
    EXPECT_EQ(undoStackCount(), undoBefore);
}

TEST(PropertyLiveApply, ItemWriteResolvesMixAliasesAndRejectsGroupAliases)
{
    auto item = makeAudioItem();
    auto& pm = VNX::ProjectManager::instance();

    EXPECT_TRUE(pm.applyItemPropertyLive(item.get(), QStringLiteral("makeup_gain"), 4.0f));
    EXPECT_FLOAT_EQ(item->comp<VNX::VMixNode>()->makeUpGain, 4.0f);

    const auto groupsBefore = item->fieldGroups;
    const int undoBefore = undoStackCount();
    SignalCounters counters;
    for (const auto* property : {"output_bus", "outputBus", "outputBusId"})
        EXPECT_FALSE(pm.applyItemPropertyLive(item.get(), QLatin1String(property), 7u));
    EXPECT_EQ(item->comp<VNX::VMixNode>()->outputBusId, 0u);
    EXPECT_EQ(item->fieldGroups, groupsBefore);
    EXPECT_FALSE(item->isDirtyObject());
    EXPECT_EQ(counters.itemPropertyChanged, 0);
    EXPECT_EQ(undoStackCount(), undoBefore);
}

TEST(PropertyLiveApply, ItemWriteRejectsUnknownProperty)
{
    auto item = makeAudioItem();

    EXPECT_FALSE(VNX::ProjectManager::instance().applyItemPropertyLive(
        item.get(), QStringLiteral("noSuchProperty"), 1.0f));
    EXPECT_FALSE(VNX::ProjectManager::instance().applyItemPropertyLive(
        nullptr, QStringLiteral("volume"), 1.0f));
}

TEST(PropertyLiveApply, BusWriteHasNoBroadcastDirtyOrUndo)
{
    auto bus = makeBusItem();
    SignalCounters counters;
    const int undoBefore = undoStackCount();

    EXPECT_TRUE(VNX::ProjectManager::instance().applyBusPropertyLive(
        bus.get(), QStringLiteral("volume"), -6.0f));
    EXPECT_TRUE(VNX::ProjectManager::instance().applyBusPropertyLive(
        bus.get(), QStringLiteral("pan"), -0.25f));

    EXPECT_FLOAT_EQ(bus->comp<VNX::VMixNode>()->volume, -6.0f);
    EXPECT_FLOAT_EQ(bus->comp<VNX::VBus>()->pan, -0.25f);
    EXPECT_FALSE(bus->isDirtyObject());
    EXPECT_EQ(counters.itemPropertyChanged, 0);
    EXPECT_EQ(counters.busChanged, 0);
    EXPECT_EQ(undoStackCount(), undoBefore);
}

TEST(PropertyLiveApply, BusWriteRejectsNonBusItem)
{
    auto item = makeAudioItem();

    EXPECT_FALSE(VNX::ProjectManager::instance().applyBusPropertyLive(
        item.get(), QStringLiteral("volume"), 0.5f));
}

TEST(PropertyEditSession, PreviewWritesModelWithoutSideEffects)
{
    auto item = makeAudioItem();
    SignalCounters counters;
    const int undoBefore = undoStackCount();

    VNX::PropertyEditSession session(item.get(), QStringLiteral("volume"), QVariant(0.0f));
    session.preview(QVariant(-3.0f));
    session.preview(QVariant(-9.0f));

    EXPECT_TRUE(session.isActive());
    EXPECT_FLOAT_EQ(item->comp<VNX::VMixNode>()->volume, -9.0f);
    EXPECT_EQ(session.currentValue().toFloat(), -9.0f);
    EXPECT_EQ(session.startValue().toFloat(), 0.0f);
    EXPECT_FALSE(item->isDirtyObject());
    EXPECT_EQ(counters.itemPropertyChanged, 0);
    EXPECT_EQ(undoStackCount(), undoBefore);

    session.cancel();
}

TEST(PropertyEditSession, CommitWithoutChangeIsSilentlyDropped)
{
    auto item = makeAudioItem();
    SignalCounters counters;
    const int undoBefore = undoStackCount();

    VNX::PropertyEditSession session(item.get(), QStringLiteral("volume"), QVariant(0.0f));
    session.commit();

    EXPECT_FALSE(session.isActive());
    EXPECT_EQ(counters.itemPropertyChanged, 0);
    EXPECT_EQ(undoStackCount(), undoBefore);
}

TEST(PropertyEditSession, CancelRestoresStartValueSilently)
{
    auto item = makeAudioItem();
    item->comp<VNX::VMixNode>()->volume = -12.0f;
    SignalCounters counters;

    VNX::PropertyEditSession session(item.get(), QStringLiteral("volume"), QVariant(-12.0f));
    session.preview(QVariant(3.0f));
    EXPECT_FLOAT_EQ(item->comp<VNX::VMixNode>()->volume, 3.0f);

    session.cancel();

    EXPECT_FALSE(session.isActive());
    EXPECT_FLOAT_EQ(item->comp<VNX::VMixNode>()->volume, -12.0f);
    EXPECT_FALSE(item->isDirtyObject());
    EXPECT_EQ(counters.itemPropertyChanged, 0);
}

TEST(PropertyEditSession, FinishAllActiveCancelsEverySession)
{
    auto item = makeAudioItem();
    item->comp<VNX::VMixNode>()->volume = -12.0f;
    item->comp<VNX::VMixNode>()->pitch = 1.0f;
    SignalCounters counters;

    VNX::PropertyEditSession volumeSession(item.get(), QStringLiteral("volume"), QVariant(-12.0f));
    VNX::PropertyEditSession pitchSession(item.get(), QStringLiteral("pitch"), QVariant(1.0f));
    volumeSession.preview(QVariant(3.0f));
    pitchSession.preview(QVariant(12.0f));

    VNX::PropertyEditSession::finishAllActive(false);

    EXPECT_FALSE(volumeSession.isActive());
    EXPECT_FALSE(pitchSession.isActive());
    EXPECT_FLOAT_EQ(item->comp<VNX::VMixNode>()->volume, -12.0f);
    EXPECT_FLOAT_EQ(item->comp<VNX::VMixNode>()->pitch, 1.0f);
    EXPECT_EQ(counters.itemPropertyChanged, 0);

    volumeSession.preview(QVariant(6.0f));
    EXPECT_FLOAT_EQ(item->comp<VNX::VMixNode>()->volume, -12.0f);
}

TEST(PropertyEditSession, DestructorCancelsActiveSession)
{
    auto item = makeAudioItem();
    item->comp<VNX::VMixNode>()->pitch = 1.0f;

    {
        VNX::PropertyEditSession session(item.get(), QStringLiteral("pitch"), QVariant(1.0f));
        session.preview(QVariant(12.0f));
        EXPECT_FLOAT_EQ(item->comp<VNX::VMixNode>()->pitch, 12.0f);
    }

    EXPECT_FLOAT_EQ(item->comp<VNX::VMixNode>()->pitch, 1.0f);
}

TEST(ActionOriginator, ExecuteExposesOriginatorOnlyDuringHandler)
{
    ensureApp();
    auto& mgr = VNX::ActionManager::instance();

    auto seen = std::make_shared<const QObject*>(nullptr);
    VNX::ActionDef def;
    def.id = QStringLiteral("test_originator_probe");
    def.displayName = QStringLiteral("Originator Probe");
    def.category = QStringLiteral("test");
    def.undoable = false;
    def.handler = [seen](const QJsonObject&) -> VNX::ActionResult {
        *seen = VNX::ActionManager::instance().executeOriginator();
        VNX::ActionResult r;
        r.success = true;
        return r;
    };
    mgr.registerAction(def);

    QObject origin;
    EXPECT_EQ(mgr.executeOriginator(), nullptr);

    mgr.execute(QStringLiteral("test_originator_probe"), {}, VNX::ActionSource::Internal, &origin);
    EXPECT_EQ(*seen, &origin);
    EXPECT_EQ(mgr.executeOriginator(), nullptr);

    *seen = &origin;
    mgr.execute(QStringLiteral("test_originator_probe"), {});
    EXPECT_EQ(*seen, nullptr);
}

TEST(OriginTracking, SessionCommitForwardsOriginatorToActionExecution)
{
    ensureApp();
    auto& mgr = VNX::ActionManager::instance();

    auto seen = std::make_shared<const QObject*>(nullptr);
    auto ran = std::make_shared<bool>(false);
    VNX::ActionDef def;
    def.id = QStringLiteral("set_item_property");
    def.displayName = QStringLiteral("Set Item Property (stub)");
    def.category = QStringLiteral("test");
    def.undoable = false;
    def.handler = [seen, ran](const QJsonObject&) -> VNX::ActionResult {
        *seen = VNX::ActionManager::instance().executeOriginator();
        *ran = true;
        VNX::ActionResult r;
        r.success = true;
        return r;
    };
    const bool stubbed = !mgr.hasAction(def.id);
    if (stubbed)
        mgr.registerAction(def);

    auto item = makeAudioItem();
    QObject origin;
    VNX::PropertyEditSession session(item.get(), QStringLiteral("volume"), QVariant(0.0f), &origin);
    session.preview(QVariant(-6.0f));
    session.commit();

    if (stubbed) {
        EXPECT_TRUE(*ran);
        EXPECT_EQ(*seen, &origin);
        mgr.clearAllForTesting();
    }
    EXPECT_EQ(mgr.executeOriginator(), nullptr);
}

TEST(PropertyEditSession, BusPreviewRoutesToBusComponent)
{
    auto bus = makeBusItem();
    SignalCounters counters;

    VNX::PropertyEditSession session(bus.get(), QStringLiteral("pan"), QVariant(0.0f));
    session.preview(QVariant(0.8f));

    EXPECT_FLOAT_EQ(bus->comp<VNX::VBus>()->pan, 0.8f);
    EXPECT_FALSE(bus->isDirtyObject());
    EXPECT_EQ(counters.busChanged, 0);

    session.cancel();
    EXPECT_FLOAT_EQ(bus->comp<VNX::VBus>()->pan, 0.0f);
}

TEST(PropertyLiveApply, BusPitchWritesMixNodeWithoutSideEffects)
{
    auto bus = makeBusItem();
    SignalCounters counters;
    const int undoBefore = undoStackCount();

    EXPECT_TRUE(VNX::ProjectManager::instance().applyBusPropertyLive(
        bus.get(), QStringLiteral("pitch"), 12.0f));

    EXPECT_FLOAT_EQ(bus->comp<VNX::VMixNode>()->pitch, 12.0f);
    EXPECT_FALSE(bus->isDirtyObject());
    EXPECT_EQ(counters.busChanged, 0);
    EXPECT_EQ(undoStackCount(), undoBefore);
}

TEST(PropertyEditSession, BusPitchPreviewRoutesToDefinitionPath)
{
    auto bus = makeBusItem();
    SignalCounters counters;

    VNX::PropertyEditSession session(bus.get(), QStringLiteral("pitch"), QVariant(0.0f));
    session.preview(QVariant(-7.0f));

    EXPECT_FLOAT_EQ(bus->comp<VNX::VMixNode>()->pitch, -7.0f);
    EXPECT_TRUE(bus->playablePropertyPresence.Has(
        harkenplayableproperties::PlayablePropertyIdV1::Pitch));
    EXPECT_FALSE(bus->isDirtyObject());
    EXPECT_EQ(counters.busChanged, 0);

    session.cancel();
    EXPECT_FLOAT_EQ(bus->comp<VNX::VMixNode>()->pitch, 0.0f);
    EXPECT_FALSE(bus->playablePropertyPresence.Has(
        harkenplayableproperties::PlayablePropertyIdV1::Pitch));
}

TEST(PropertyEditSession, BusOutputVolumePreviewWritesMixNodeNotOverlay)
{
    auto bus = makeBusItem();
    SignalCounters counters;
    const auto locator = VNX::PlayablePropertyAuthoringV1::definitionLocator(*bus);
    ASSERT_TRUE(locator.has_value());
    const auto propertyId = static_cast<uint8_t>(
        harkenplayableproperties::PlayablePropertyIdV1::OutputBusVolume);
    const auto platform = QUuid::createUuid();
    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(
        *bus, *locator, propertyId, -12.0, platform));
    EXPECT_FLOAT_EQ(bus->comp<VNX::VMixNode>()->outputBusVolume, 0.0f);

    QVector<VNX::ProjectPlatformId> platforms{platform};
    VNX::PropertyEditSession session(
        bus.get(), QStringLiteral("outputBusVolume"), QVariant(0.0f),
        nullptr, platforms);
    session.preview(QVariant(-6.0f));

    EXPECT_FLOAT_EQ(bus->comp<VNX::VMixNode>()->outputBusVolume, -6.0f);
    EXPECT_FLOAT_EQ(
        VNX::ProjectManager::instance().effectiveOutputVolumeDbForItem(bus.get()),
        -6.0f);
    const auto overlay = VNX::PlayablePropertyAuthoringV1::readPlatformOverlay(
        *bus, *locator, propertyId, platform);
    ASSERT_TRUE(overlay.has_value());
    EXPECT_TRUE(overlay->present);
    EXPECT_DOUBLE_EQ(overlay->value.toDouble(), -12.0);
    EXPECT_FALSE(bus->isDirtyObject());
    EXPECT_EQ(counters.busChanged, 0);

    session.cancel();
    EXPECT_FLOAT_EQ(bus->comp<VNX::VMixNode>()->outputBusVolume, 0.0f);
}

TEST(PropertyEditSession, BusOutputVolumeLocatorPreviewWritesMixNode)
{
    auto bus = makeBusItem();
    const auto locator = VNX::PlayablePropertyAuthoringV1::definitionLocator(*bus);
    ASSERT_TRUE(locator.has_value());
    const auto propertyId = static_cast<uint8_t>(
        harkenplayableproperties::PlayablePropertyIdV1::OutputBusVolume);
    const auto platform = QUuid::createUuid();
    QVector<VNX::ProjectPlatformId> platforms{platform};
    VNX::PropertyEditSession session(
        bus.get(), *locator, propertyId, nullptr, platforms);
    session.preview(QVariant(-3.0f));
    EXPECT_FLOAT_EQ(bus->comp<VNX::VMixNode>()->outputBusVolume, -3.0f);
    EXPECT_FLOAT_EQ(
        VNX::ProjectManager::instance().effectiveOutputVolumeDbForItem(bus.get()),
        -3.0f);
    const auto overlay = VNX::PlayablePropertyAuthoringV1::readPlatformOverlay(
        *bus, *locator, propertyId, platform);
    ASSERT_TRUE(overlay.has_value());
    EXPECT_FALSE(overlay->present);
}

TEST(PropertyLiveApply, BusOutputVolumeWritesMixNodeWithoutSideEffects)
{
    auto bus = makeBusItem();
    SignalCounters counters;
    const int undoBefore = undoStackCount();

    EXPECT_TRUE(VNX::ProjectManager::instance().applyBusPropertyLive(
        bus.get(), QStringLiteral("outputBusVolume"), -9.0f));

    EXPECT_FLOAT_EQ(bus->comp<VNX::VMixNode>()->outputBusVolume, -9.0f);
    EXPECT_TRUE(bus->playablePropertyPresence.Has(
        harkenplayableproperties::PlayablePropertyIdV1::OutputBusVolume));
    EXPECT_FALSE(bus->isDirtyObject());
    EXPECT_EQ(counters.busChanged, 0);
    EXPECT_EQ(undoStackCount(), undoBefore);
}

TEST(PitchAggregation, ItemChainSumsCentsAdditively)
{
    auto parent = std::make_shared<VNX::ProjectItem>(QStringLiteral("Layer"),
                                                     VNX::ProjectItem::Type::Layer);
    parent->compOrCreate<VNX::VMixNode>()->pitch = 300.0f;

    auto sound = makeAudioItem();
    sound->comp<VNX::VMixNode>()->pitch = 200.0f;
    sound->parent = parent.get();

    EXPECT_FLOAT_EQ(
        VNX::ProjectManager::instance().aggregatePitchForItem(sound.get()), 500.0f);
}

class OutputBusPropertyTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& pm = VNX::ProjectManager::instance();
        if (pm.isProjectOpen())
            pm.closeProject();
        ASSERT_TRUE(tempDir.isValid());
        VNX::ProjectConfig config;
        config.name = QStringLiteral("OutputBusPropertyProject");
        config.location = tempDir.path();
        ASSERT_TRUE(pm.createProject(config));
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::registerProjectActions(VNX::ActionManager::instance(), &pm, nullptr, {});
        VNX::UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        VNX::UndoRedoManager::instance().clear();
        VNX::ActionManager::instance().clearAllForTesting();
        auto& pm = VNX::ProjectManager::instance();
        if (pm.isProjectOpen())
            pm.closeProject();
    }

    QTemporaryDir tempDir;
};

TEST_F(OutputBusPropertyTest, MainOutputGroupFollowsTheEngineeringParent)
{
    auto& pm = VNX::ProjectManager::instance();
    auto* parent = pm.createItem(nullptr, QStringLiteral("Layer"), VNX::ProjectItem::Type::Layer);
    auto* sound = pm.createItem(parent, QStringLiteral("Sound"), VNX::ProjectItem::Type::Sound);
    auto* parentBus = pm.createItem(pm.getMasterBusItem(), QStringLiteral("ParentBus"), VNX::ProjectItem::Type::Bus);
    auto* localBus = pm.createItem(pm.getMasterBusItem(), QStringLiteral("LocalBus"), VNX::ProjectItem::Type::Bus);
    ASSERT_NE(parent, nullptr);
    ASSERT_NE(sound, nullptr);
    ASSERT_NE(parentBus, nullptr);
    ASSERT_NE(localBus, nullptr);
    auto parentGroups = parent->fieldGroups;
    parentGroups.mainOutput.shared = {VNX::FieldGroupMode::Local,
        VNX::MainOutputFieldGroup{VNX::toFieldGroupGuid(parentBus->guid), -3.0f}};
    ASSERT_TRUE(pm.restoreFieldGroupsInternal(parent, parentGroups));
    EXPECT_EQ(pm.resolveEffectiveOutputBusId(sound), parentBus->objectID);

    auto soundGroups = sound->fieldGroups;
    soundGroups.mainOutput.shared = {VNX::FieldGroupMode::Local,
        VNX::MainOutputFieldGroup{VNX::toFieldGroupGuid(localBus->guid), 0.0f}};
    ASSERT_TRUE(pm.restoreFieldGroupsInternal(sound, soundGroups));
    EXPECT_EQ(pm.resolveEffectiveOutputBusId(sound), localBus->objectID);

    soundGroups.mainOutput.shared.mode = VNX::FieldGroupMode::Inherit;
    ASSERT_TRUE(pm.restoreFieldGroupsInternal(sound, soundGroups));
    EXPECT_EQ(pm.resolveEffectiveOutputBusId(sound), parentBus->objectID);
    parentGroups.mainOutput.shared.mode = VNX::FieldGroupMode::Inherit;
    ASSERT_TRUE(pm.restoreFieldGroupsInternal(parent, parentGroups));
    EXPECT_EQ(pm.resolveEffectiveOutputBusId(sound), parentBus->objectID);
}

TEST_F(OutputBusPropertyTest, PitchIncludesContentAndSelectedBusAncestorsAfterPlatformChangesAndEdits)
{
    auto& pm = VNX::ProjectManager::instance();
    pm.setProjectPlatformsInternal(HarkenTest::platforms({"Windows", "Android"}));
    auto* layer = pm.createItem(nullptr, QStringLiteral("Layer"), VNX::ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    auto* container = pm.createItem(layer, QStringLiteral("Container"), VNX::ProjectItem::Type::Container);
    ASSERT_NE(container, nullptr);
    auto* sound = pm.createItem(container, QStringLiteral("Sound"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    auto* masterBus = pm.getMasterBusItem();
    ASSERT_NE(masterBus, nullptr);
    auto* parentBus = pm.createItem(masterBus, QStringLiteral("ParentBus"), VNX::ProjectItem::Type::Bus);
    ASSERT_NE(parentBus, nullptr);
    auto* windowsBus = pm.createItem(parentBus, QStringLiteral("WindowsBus"), VNX::ProjectItem::Type::Bus);
    auto* androidBus = pm.createItem(parentBus, QStringLiteral("AndroidBus"), VNX::ProjectItem::Type::Bus);
    ASSERT_NE(windowsBus, nullptr);
    ASSERT_NE(androidBus, nullptr);

    ASSERT_TRUE(pm.applyItemPropertyLive(layer, QStringLiteral("pitch"), 300.0f));
    ASSERT_TRUE(pm.applyItemPropertyLive(container, QStringLiteral("pitch"), -50.0f));
    ASSERT_TRUE(pm.applyItemPropertyLive(sound, QStringLiteral("pitch"), 150.0f));
    ASSERT_TRUE(pm.applyBusPropertyLive(masterBus, QStringLiteral("pitch"), 25.0f));
    ASSERT_TRUE(pm.applyBusPropertyLive(parentBus, QStringLiteral("pitch"), 100.0f));
    ASSERT_TRUE(pm.applyBusPropertyLive(windowsBus, QStringLiteral("pitch"), 200.0f));
    ASSERT_TRUE(pm.applyBusPropertyLive(androidBus, QStringLiteral("pitch"), -400.0f));

    auto groups = layer->fieldGroups;
    groups.mainOutput.shared = {VNX::FieldGroupMode::Local,
        VNX::MainOutputFieldGroup{VNX::toFieldGroupGuid(windowsBus->guid), 0.0f}};
    groups.mainOutput.platforms[VNX::toFieldGroupGuid(HarkenTest::PlatformIds::Android)] = {
        VNX::FieldGroupMode::Local,
        VNX::MainOutputFieldGroup{VNX::toFieldGroupGuid(androidBus->guid), 0.0f}};
    ASSERT_TRUE(pm.restoreFieldGroupsInternal(layer, groups));

    auto& context = VNX::PlatformContextService::instance();
    context.setSelected({HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
    EXPECT_EQ(pm.resolveEffectiveOutputBusId(sound), windowsBus->objectID);
    EXPECT_FLOAT_EQ(pm.aggregatePitchForItem(sound), 725.0f);

    context.setSelected({HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Android, false);
    EXPECT_EQ(pm.resolveEffectiveOutputBusId(sound), androidBus->objectID);
    EXPECT_FLOAT_EQ(pm.aggregatePitchForItem(sound), 125.0f);

    context.setSelected({HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
    EXPECT_FLOAT_EQ(pm.aggregatePitchForItem(sound), 725.0f);
    ASSERT_TRUE(pm.applyBusPropertyLive(parentBus, QStringLiteral("pitch"), 250.0f));
    EXPECT_FLOAT_EQ(pm.aggregatePitchForItem(sound), 875.0f);

    context.setSelected({HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Android, false);
    EXPECT_FLOAT_EQ(pm.aggregatePitchForItem(sound), 275.0f);
    ASSERT_TRUE(pm.applyItemPropertyLive(layer, QStringLiteral("pitch"), 100.0f));
    EXPECT_FLOAT_EQ(pm.aggregatePitchForItem(sound), 75.0f);

    groups.mainOutput.platforms[VNX::toFieldGroupGuid(HarkenTest::PlatformIds::Android)] = {
        VNX::FieldGroupMode::Local,
        VNX::MainOutputFieldGroup{VNX::toFieldGroupGuid(windowsBus->guid), 0.0f}};
    ASSERT_TRUE(pm.restoreFieldGroupsInternal(layer, groups));
    EXPECT_EQ(pm.resolveEffectiveOutputBusId(sound), windowsBus->objectID);
    EXPECT_FLOAT_EQ(pm.aggregatePitchForItem(sound), 675.0f);

    context.setSelected({HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
    EXPECT_FLOAT_EQ(pm.aggregatePitchForItem(sound), 675.0f);
}

TEST_F(OutputBusPropertyTest, MainOutputGroupCommitWritesAndMarksDirty)
{
    auto& pm = VNX::ProjectManager::instance();
    auto* item = pm.createItem(nullptr, QStringLiteral("OutputSound"), VNX::ProjectItem::Type::Sound);
    auto* bus = pm.createItem(pm.getMasterBusItem(), QStringLiteral("OutputBus"), VNX::ProjectItem::Type::Bus);
    ASSERT_NE(item, nullptr);
    ASSERT_NE(bus, nullptr);
    item->clearDirty();
    const auto before = item->fieldGroups;
    const int undoBefore = undoStackCount();

    const auto result = setFieldGroupField(item, QStringLiteral("mainOutput"),
        QStringLiteral("mainOutputBusRef"), bus->guid.toString(QUuid::WithoutBraces));

    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    EXPECT_EQ(item->comp<VNX::VMixNode>()->outputBusId, bus->objectID);
    EXPECT_EQ(pm.resolveEffectiveOutputBusId(item), bus->objectID);
    ASSERT_TRUE(item->fieldGroups.mainOutput.shared.localHistory);
    EXPECT_EQ(item->fieldGroups.mainOutput.shared.localHistory->mainOutputBusRef,
              VNX::toFieldGroupGuid(bus->guid));
    EXPECT_EQ(item->fieldGroups.mainOutput.shared.mode, before.mainOutput.shared.mode);
    EXPECT_EQ(item->fieldGroups.send, before.send);
    EXPECT_EQ(item->fieldGroups.spatialRating, before.spatialRating);
    EXPECT_TRUE(item->isDirtyObject());
    const auto after = item->fieldGroups;
    auto* stack = VNX::UndoRedoManager::instance().undoStack();
    ASSERT_EQ(stack->count(), undoBefore + 1);
    stack->undo();
    EXPECT_EQ(item->fieldGroups, before);
    EXPECT_EQ(pm.resolveEffectiveOutputBusId(item), pm.getMasterBusItem()->objectID);
    stack->redo();
    EXPECT_EQ(item->fieldGroups, after);
    EXPECT_EQ(pm.resolveEffectiveOutputBusId(item), bus->objectID);
}

TEST_F(OutputBusPropertyTest, BusMakeUpGainRoutesThroughMixerAndSupportsUndoRedo)
{
    auto& pm = VNX::ProjectManager::instance();
    VNX::ProjectItem* bus = pm.createItem(
        pm.getMasterBusItem(), QStringLiteral("GainBus"), VNX::ProjectItem::Type::Bus);
    ASSERT_NE(bus, nullptr);

    const VNX::ActionResult result = handlePlayableItemProperty(
        &pm, bus, QStringLiteral("makeUpGain"),
        QJsonObject{{QStringLiteral("value"), 3.5}}, nullptr);

    ASSERT_TRUE(result.success);
    ASSERT_NE(bus->comp<VNX::VMixNode>(), nullptr);
    EXPECT_FLOAT_EQ(bus->comp<VNX::VMixNode>()->makeUpGain, 3.5f);
    EXPECT_FLOAT_EQ(pm.effectiveBusVolumeDb(bus), 3.5f);
    EXPECT_TRUE(bus->playablePropertyPresence.Has(
        harkenplayableproperties::PlayablePropertyIdV1::MakeUpGain));

    result.undoFn();
    EXPECT_FLOAT_EQ(bus->comp<VNX::VMixNode>()->makeUpGain, 0.0f);
    EXPECT_FLOAT_EQ(pm.effectiveBusVolumeDb(bus), 0.0f);
    EXPECT_FALSE(bus->playablePropertyPresence.Has(
        harkenplayableproperties::PlayablePropertyIdV1::MakeUpGain));
    result.redoFn();
    EXPECT_FLOAT_EQ(bus->comp<VNX::VMixNode>()->makeUpGain, 3.5f);
    EXPECT_FLOAT_EQ(pm.effectiveBusVolumeDb(bus), 3.5f);
    EXPECT_TRUE(bus->playablePropertyPresence.Has(
        harkenplayableproperties::PlayablePropertyIdV1::MakeUpGain));
}

TEST_F(OutputBusPropertyTest, MainOutputGroupRejectsUnknownBus)
{
    auto& pm = VNX::ProjectManager::instance();
    auto* item = pm.createItem(nullptr, QStringLiteral("OutputSound"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(item, nullptr);
    item->clearDirty();
    const auto before = item->fieldGroups;
    const int undoBefore = undoStackCount();

    const auto result = setFieldGroupField(item, QStringLiteral("mainOutput"),
        QStringLiteral("mainOutputBusRef"), QUuid::createUuid().toString(QUuid::WithoutBraces));

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorCode, QStringLiteral("FIELD_GROUP_REJECTED"));
    EXPECT_EQ(item->fieldGroups, before);
    EXPECT_EQ(pm.resolveEffectiveOutputBusId(item), pm.getMasterBusItem()->objectID);
    EXPECT_FALSE(item->isDirtyObject());
    EXPECT_EQ(undoStackCount(), undoBefore);
}

TEST_F(OutputBusPropertyTest, LegacyOutputBusAliasesRequireGroupCommands)
{
    auto& pm = VNX::ProjectManager::instance();
    ASSERT_NE(pm.getMasterBusItem(), nullptr);
    const uint32_t masterId = pm.getMasterBusItem()->objectID;
    auto* item = pm.createItem(nullptr, QStringLiteral("OutputSound"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(item, nullptr);
    item->clearDirty();
    const auto before = item->fieldGroups;
    const int undoBefore = undoStackCount();

    for (const auto* property : {"outputBusId", "outputBus", "output_bus"}) {
        const auto result = VNX::ActionManager::instance().execute(
            QStringLiteral("set_item_property"), QJsonObject{
                {QStringLiteral("authoring_handle"), static_cast<qint64>(item->objectID)},
                {QStringLiteral("property"), QLatin1String(property)},
                {QStringLiteral("value"), static_cast<qint64>(masterId)}}, VNX::ActionSource::IPC);
        EXPECT_FALSE(result.success);
        EXPECT_EQ(result.errorCode, QStringLiteral("FIELD_GROUP_COMMAND_REQUIRED"));
    }

    EXPECT_EQ(item->fieldGroups, before);
    EXPECT_EQ(item->comp<VNX::VMixNode>()->outputBusId, masterId);
    EXPECT_FALSE(item->isDirtyObject());
    EXPECT_EQ(undoStackCount(), undoBefore);
}

TEST_F(OutputBusPropertyTest, MainOutputVolumeGroupCommitWritesAndMarksDirty)
{
    auto& pm = VNX::ProjectManager::instance();
    auto* item = pm.createItem(nullptr, QStringLiteral("OutputSound"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(item, nullptr);
    item->clearDirty();
    const auto before = item->fieldGroups;
    const int undoBefore = undoStackCount();

    const auto result = setFieldGroupField(item, QStringLiteral("mainOutput"),
        QStringLiteral("outputBusVolume"), -6.0);

    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    ASSERT_TRUE(item->fieldGroups.mainOutput.shared.localHistory);
    EXPECT_FLOAT_EQ(item->fieldGroups.mainOutput.shared.localHistory->outputBusVolume, -6.0f);
    EXPECT_FLOAT_EQ(item->comp<VNX::VMixNode>()->outputBusVolume, -6.0f);
    EXPECT_TRUE(item->isDirtyObject());
    const auto after = item->fieldGroups;
    auto* stack = VNX::UndoRedoManager::instance().undoStack();
    ASSERT_EQ(stack->count(), undoBefore + 1);
    stack->undo();
    EXPECT_EQ(item->fieldGroups, before);
    EXPECT_FLOAT_EQ(item->comp<VNX::VMixNode>()->outputBusVolume, 0.0f);
    stack->redo();
    EXPECT_EQ(item->fieldGroups, after);
    EXPECT_FLOAT_EQ(item->comp<VNX::VMixNode>()->outputBusVolume, -6.0f);
}

TEST(SetItemProperty, UnknownPropertyIsRejectedWithoutDirty)
{
    ensureApp();
    auto item = makeAudioItem();

    const VNX::ActionResult r = handleAudioItemProperty(
        &VNX::ProjectManager::instance(), item.get(),
        QStringLiteral("noSuchProperty"),
        QJsonObject{{QStringLiteral("value"), 1}}, nullptr);

    EXPECT_FALSE(r.success);
    EXPECT_FALSE(item->isDirtyObject());
}

class SpatialItemPropertyTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& pm = VNX::ProjectManager::instance();
        if (pm.isProjectOpen())
            pm.closeProject();
        ASSERT_TRUE(tempDir.isValid());
        VNX::ProjectConfig config;
        config.name = QStringLiteral("SpatialItemPropertyProject");
        config.location = tempDir.path();
        ASSERT_TRUE(pm.createProject(config));
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::registerProjectActions(VNX::ActionManager::instance(), &pm, nullptr, {});
        VNX::UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        VNX::UndoRedoManager::instance().clear();
        VNX::ActionManager::instance().clearAllForTesting();
        auto& pm = VNX::ProjectManager::instance();
        if (pm.isProjectOpen())
            pm.closeProject();
    }

    QTemporaryDir tempDir;
};

class PlayablePropertyEditingTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& pm = VNX::ProjectManager::instance();
        if (pm.isProjectOpen()) pm.closeProject();
        ASSERT_TRUE(tempDir.isValid());
        VNX::ProjectConfig config;
        config.name = QStringLiteral("PlayablePropertyEditingProject");
        config.location = tempDir.path();
        ASSERT_TRUE(pm.createProject(config));
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::registerProjectActions(
            VNX::ActionManager::instance(), &pm, nullptr, {});
        VNX::UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        VNX::UndoRedoManager::instance().clear();
        VNX::ActionManager::instance().clearAllForTesting();
        auto& pm = VNX::ProjectManager::instance();
        if (pm.isProjectOpen()) pm.closeProject();
    }

    QTemporaryDir tempDir;
};

TEST_F(PlayablePropertyEditingTest, SpatializationMixSharedEditCancelClearAndUndo)
{
    auto& pm = VNX::ProjectManager::instance();
    auto* sound = pm.createItem(nullptr, QStringLiteral("MixSound"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    const auto groups = sound->fieldGroups;
    auto* spatial = sound->comp<VNX::VSpatial>();
    auto command = [&](const QString& operation, double value) {
        return VNX::ActionManager::instance().execute("set_item_property", QJsonObject{
            {"authoring_handle", static_cast<qint64>(sound->objectID)},
            {"property", "spatializationMix"}, {"override_operation", operation}, {"value", value}},
            VNX::ActionSource::IPC);
    };
    ASSERT_TRUE(command("clear_override", 1.0).success);
    ASSERT_FALSE(spatial->spatializationMixPresent);
    {
        VNX::PropertyEditSession session(sound, "spatializationMix", 1.0);
        session.preview(0.5);
        EXPECT_FLOAT_EQ(spatial->spatializationMix, 0.5f);
        EXPECT_TRUE(spatial->spatializationMixPresent);
        session.cancel();
    }
    EXPECT_FALSE(spatial->spatializationMixPresent);
    ASSERT_TRUE(command("set_override", 1.0).success);
    EXPECT_TRUE(spatial->spatializationMixPresent);
    VNX::UndoRedoManager::instance().undoStack()->undo();
    EXPECT_FALSE(spatial->spatializationMixPresent);
    VNX::UndoRedoManager::instance().undoStack()->redo();
    EXPECT_TRUE(spatial->spatializationMixPresent);
    const auto rejected = VNX::ActionManager::instance().execute("set_item_property", QJsonObject{
        {"authoring_handle", static_cast<qint64>(sound->objectID)},
        {"property", "spatializationMix"}, {"value", 0.0},
        {"platform_ids", QJsonArray{}}}, VNX::ActionSource::IPC);
    EXPECT_FALSE(rejected.success);
    EXPECT_FLOAT_EQ(spatial->spatializationMix, 1.0f);
    EXPECT_EQ(sound->fieldGroups, groups);
    EXPECT_TRUE(sound->platformOverrides.rows.empty());
    EXPECT_EQ(sound->playablePropertyPresence.bits, 0u);
}

TEST_F(PlayablePropertyEditingTest, ExplicitDefaultAndClearOverrideUndoRedo)
{
    auto& pm = VNX::ProjectManager::instance();
    VNX::ProjectItem* layer = pm.createItem(
        nullptr, QStringLiteral("PresenceLayer"), VNX::ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    const auto volume = harkenplayableproperties::PlayablePropertyIdV1::Volume;

    auto set = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{
            {QStringLiteral("authoring_handle"), static_cast<qint64>(layer->objectID)},
            {QStringLiteral("property"), QStringLiteral("volume")},
            {QStringLiteral("override_operation"), QStringLiteral("set_override")},
            {QStringLiteral("value"), 0.0}},
        VNX::ActionSource::Internal);
    ASSERT_TRUE(set.success);
    EXPECT_TRUE(layer->playablePropertyPresence.Has(volume));
    EXPECT_FLOAT_EQ(layer->comp<VNX::VMixNode>()->volume, 0.0f);

    VNX::PropertyEditSession active(
        layer, QStringLiteral("volume"), QVariant(0.0f));
    active.preview(QVariant(-9.0f));
    EXPECT_FLOAT_EQ(layer->comp<VNX::VMixNode>()->volume, -9.0f);

    auto clear = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{
            {QStringLiteral("authoring_handle"), static_cast<qint64>(layer->objectID)},
            {QStringLiteral("property"), QStringLiteral("volume")},
            {QStringLiteral("override_operation"), QStringLiteral("clear_override")}},
        VNX::ActionSource::Internal);
    ASSERT_TRUE(clear.success);
    EXPECT_FALSE(active.isActive());
    EXPECT_FALSE(layer->playablePropertyPresence.Has(volume));

    VNX::UndoRedoManager::instance().undoStack()->undo();
    EXPECT_TRUE(layer->playablePropertyPresence.Has(volume));
    VNX::UndoRedoManager::instance().undoStack()->redo();
    EXPECT_FALSE(layer->playablePropertyPresence.Has(volume));
}

TEST_F(PlayablePropertyEditingTest, LegacyReferenceWritesRequireSectionCommandsWithoutMutation)
{
    auto& pm = VNX::ProjectManager::instance();
    VNX::ProjectItem* sound = pm.createItem(
        nullptr, QStringLiteral("ReferenceSound"), VNX::ProjectItem::Type::Sound);
    VNX::ProjectItem* bus = pm.createItem(
        pm.getMasterBusItem(), QStringLiteral("ReferenceBus"),
        VNX::ProjectItem::Type::Bus);
    VNX::ProjectItem* attenuation = pm.createItem(
        nullptr, QStringLiteral("ReferenceAttenuation"),
        VNX::ProjectItem::Type::Attenuation);
    ASSERT_NE(sound, nullptr);
    ASSERT_NE(bus, nullptr);
    ASSERT_NE(attenuation, nullptr);
    bus->guid = VNX::authoringUuid(harkentest::StudioObjectId(UINT64_C(0x100000101)));
    attenuation->guid = VNX::authoringUuid(harkentest::StudioObjectId(UINT64_C(0x100000202)));
    const auto groupsBefore = sound->fieldGroups;
    const auto mainOutputBefore = sound->comp<VNX::VMixNode>()->mainOutputBusRef;
    const auto attenuationBefore = sound->comp<VNX::VSpatial>()->attenuationRef;
    const auto presenceBefore = sound->playablePropertyPresence.bits;
    const int undoBefore = undoStackCount();
    sound->clearDirty();
    SignalCounters counters;

    const auto execute = [&](const QString& property, const QString& value) {
        return VNX::ActionManager::instance().execute(
            QStringLiteral("set_item_property"),
            QJsonObject{
                {QStringLiteral("authoring_handle"),
                 static_cast<qint64>(sound->objectID)},
                {QStringLiteral("property"), property},
                {QStringLiteral("override_operation"), QStringLiteral("set_override")},
                {QStringLiteral("value"), value}},
            VNX::ActionSource::IPC);
    };
    const std::vector<std::pair<QString, QString>> groupedReferences{
        {QStringLiteral("mainOutputBusRef"), VNX::objectIdString(bus->runtimeId())},
        {QStringLiteral("mainOutputBusRef"), VNX::objectIdString(attenuation->runtimeId())},
        {QStringLiteral("spatialAttenuationRef"), VNX::objectIdString(bus->runtimeId())},
        {QStringLiteral("spatialAttenuationRef"), VNX::objectIdString(attenuation->runtimeId())},
        {QStringLiteral("mainOutputBusRef"), QStringLiteral("18446744073709551614")},
        {QStringLiteral("mainOutputBusRef"), bus->guid.toString(QUuid::WithoutBraces)},
    };
    for (const auto& [property, value] : groupedReferences) {
        const auto result = execute(property, value);
        EXPECT_FALSE(result.success);
        EXPECT_EQ(result.errorCode, QStringLiteral("FIELD_GROUP_COMMAND_REQUIRED"));
    }
    EXPECT_FALSE(execute(QStringLiteral("bindingAttachmentRef"),
                        QStringLiteral("0")).success);
    EXPECT_FALSE(execute(QStringLiteral("bindingAttachmentRef"),
                         QStringLiteral("4294967297")).success);
    EXPECT_TRUE(sound->comp<VNX::VMixNode>()->bindings.isEmpty());
    EXPECT_EQ(sound->fieldGroups, groupsBefore);
    EXPECT_EQ(sound->comp<VNX::VMixNode>()->mainOutputBusRef, mainOutputBefore);
    EXPECT_EQ(sound->comp<VNX::VSpatial>()->attenuationRef, attenuationBefore);
    EXPECT_EQ(sound->playablePropertyPresence.bits, presenceBefore);
    EXPECT_EQ(undoStackCount(), undoBefore);
    EXPECT_EQ(counters.itemPropertyChanged, 0);
    EXPECT_FALSE(sound->isDirtyObject());
}

TEST_F(PlayablePropertyEditingTest, BindingRelationsCannotBeReplacedAsAnObjectProperty)
{
    auto& pm = VNX::ProjectManager::instance();
    auto* sound = pm.createItem(
        nullptr, QStringLiteral("BoundSound"), VNX::ProjectItem::Type::Sound);
    auto* variable = pm.createItem(
        nullptr, QStringLiteral("Drive"), VNX::ProjectItem::Type::Variable);
    ASSERT_NE(sound, nullptr);
    ASSERT_NE(variable, nullptr);
    auto binding = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(), variable->guid);
    binding.guid = QUuid::createUuid();
    ASSERT_TRUE(sound->comp<VNX::VMixNode>()->bindings.upsert(binding));
    const auto before = sound->comp<VNX::VMixNode>()->bindings.toJson();
    const auto presenceBefore = sound->playablePropertyPresence.bits;
    const int undoBefore = undoStackCount();
    sound->clearDirty();

    const auto result = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{
            {QStringLiteral("authoring_handle"), static_cast<qint64>(sound->objectID)},
            {QStringLiteral("property"), QStringLiteral("bindings")},
            {QStringLiteral("value"), QJsonArray{}}},
        VNX::ActionSource::IPC);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(sound->comp<VNX::VMixNode>()->bindings.toJson(), before);
    EXPECT_EQ(sound->playablePropertyPresence.bits, presenceBefore);
    EXPECT_EQ(undoStackCount(), undoBefore);
    EXPECT_FALSE(sound->isDirtyObject());
}

TEST_F(PlayablePropertyEditingTest, LegacySendsWritesRejectAllValuesWithoutMutation)
{
    auto& pm = VNX::ProjectManager::instance();
    VNX::ProjectItem* sound = pm.createItem(
        nullptr, QStringLiteral("SendSound"), VNX::ProjectItem::Type::Sound);
    VNX::ProjectItem* target = pm.createItem(
        pm.getMasterBusItem(), QStringLiteral("SendTarget"),
        VNX::ProjectItem::Type::Bus);
    ASSERT_NE(sound, nullptr);
    ASSERT_NE(target, nullptr);
    sound->clearDirty();
    SignalCounters counters;
    const int undoBefore = undoStackCount();
    const auto groupsBefore = sound->fieldGroups;
    const auto execute = [&](const QJsonValue& value) {
        return VNX::ActionManager::instance().execute(
            QStringLiteral("set_item_property"),
            QJsonObject{
                {QStringLiteral("authoring_handle"),
                 static_cast<qint64>(sound->objectID)},
                {QStringLiteral("property"), QStringLiteral("sends")},
                {QStringLiteral("override_operation"), QStringLiteral("set_override")},
                {QStringLiteral("value"), value}},
            VNX::ActionSource::IPC);
    };
    const QJsonObject validRow{
        {QStringLiteral("targetBusID"), static_cast<qint64>(target->objectID)},
        {QStringLiteral("sendLevel"), -3.0}};
    const std::vector<QJsonValue> legacyValues{
        QJsonArray{},
        QJsonArray{validRow},
        QJsonObject{},
        QJsonArray{1},
        QJsonArray{QJsonObject{{QStringLiteral("targetBusID"), 0},
                               {QStringLiteral("sendLevel"), 0.0}}},
        QJsonArray{validRow, validRow},
        QJsonArray{QJsonObject{{QStringLiteral("targetBusID"), 0x7fffffff},
                               {QStringLiteral("sendLevel"), 0.0}}},
        QJsonArray{QJsonObject{{QStringLiteral("targetBusID"),
                                static_cast<qint64>(target->objectID)},
                               {QStringLiteral("sendLevel"), 25.0}}},
        QJsonArray{QJsonObject{{QStringLiteral("targetBusID"),
                                static_cast<qint64>(target->objectID)},
                               {QStringLiteral("sendLevel"),
                                std::numeric_limits<double>::infinity()}}},
    };
    for (const QJsonValue& value : legacyValues) {
        const auto result = execute(value);
        EXPECT_FALSE(result.success);
        EXPECT_EQ(result.errorCode, QStringLiteral("FIELD_GROUP_COMMAND_REQUIRED"));
        EXPECT_FALSE(sound->playablePropertyPresence.Has(
            harkenplayableproperties::PlayablePropertyIdV1::Sends));
        EXPECT_TRUE(sound->comp<VNX::VMixNode>()->sends.empty());
        EXPECT_FALSE(sound->isDirtyObject());
    }
    EXPECT_EQ(sound->fieldGroups, groupsBefore);
    EXPECT_EQ(counters.itemPropertyChanged, 0);
    EXPECT_EQ(undoStackCount(), undoBefore);
}

TEST_F(PlayablePropertyEditingTest, IdempotentOverrideDoesNotPublishDirtyOrUndo)
{
    auto& pm = VNX::ProjectManager::instance();
    VNX::ProjectItem* sound = pm.createItem(
        nullptr, QStringLiteral("IdempotentSound"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    const QJsonObject args{
        {QStringLiteral("authoring_handle"), static_cast<qint64>(sound->objectID)},
        {QStringLiteral("property"), QStringLiteral("volume")},
        {QStringLiteral("override_operation"), QStringLiteral("set_override")},
        {QStringLiteral("value"), 0.0}};
    ASSERT_TRUE(VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"), args,
        VNX::ActionSource::IPC).success);
    VNX::UndoRedoManager::instance().clear();
    sound->clearDirty();
    SignalCounters counters;
    ASSERT_TRUE(VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"), args,
        VNX::ActionSource::IPC).success);
    EXPECT_FALSE(sound->isDirtyObject());
    EXPECT_EQ(counters.itemPropertyChanged, 0);
    EXPECT_EQ(undoStackCount(), 0);
}

TEST_F(PlayablePropertyEditingTest, TrackOccurrenceSessionRejectsEditingWithoutMutation)
{
    auto& pm = VNX::ProjectManager::instance();
    VNX::ProjectItem* sound = pm.createItem(
        nullptr, QStringLiteral("ClipTarget"), VNX::ProjectItem::Type::Sound);
    VNX::ProjectItem* track = pm.createItem(
        nullptr, QStringLiteral("OccurrenceTrack"), VNX::ProjectItem::Type::Track);
    ASSERT_NE(sound, nullptr);
    ASSERT_NE(track, nullptr);
    auto* timeline = track->comp<VNX::VTrack>();
    ASSERT_NE(timeline, nullptr);
    VNX::Clip clip;
    clip.clipGuid = QUuid::createUuid();
    clip.targetGuid = sound->guid;
    clip.start = 120;
    clip.duration = 480;
    const auto loopCount = harkenplayableproperties::PlayablePropertyIdV1::LoopCount;
    clip.playableProperties.presence.Set(loopCount);
    ASSERT_TRUE(clip.playableProperties.setValue(loopCount, uint32_t{3}));
    ASSERT_TRUE(timeline->clips.mintOccurrenceOrdinal(&clip));
    timeline->clips.items.push_back(clip);
    const auto locator = VNX::PlayablePropertyAuthoringV1::trackClipLocator(
        *track, clip.clipGuid);
    ASSERT_TRUE(locator.has_value());

    const auto clipsBefore = timeline->clips;
    const auto trackBefore = VNX::PlayablePropertyAuthoringV1::definitionToJson(*track);
    const auto soundBefore = VNX::PlayablePropertyAuthoringV1::definitionToJson(*sound);
    VNX::UndoRedoManager::instance().clear();
    track->clearDirty();
    sound->clearDirty();
    SignalCounters counters;
    auto stale = *locator;
    ++stale.stableOccurrenceOrdinal;
    for (const auto& candidate : {*locator, stale}) {
        VNX::PropertyEditSession cancelled(
            track, candidate, static_cast<uint8_t>(loopCount));
        EXPECT_FALSE(cancelled.isActive());
        cancelled.preview(QVariant(1u));
        cancelled.cancel();
        VNX::PropertyEditSession committed(
            track, candidate, static_cast<uint8_t>(loopCount));
        EXPECT_FALSE(committed.isActive());
        committed.preview(QVariant(5u));
        committed.commit();
    }
    EXPECT_FALSE(VNX::PlayablePropertyAuthoringV1::setOverride(
        *track, stale, static_cast<uint8_t>(loopCount), 1, std::nullopt));
    EXPECT_EQ(timeline->clips, clipsBefore);
    EXPECT_EQ(VNX::PlayablePropertyAuthoringV1::definitionToJson(*track), trackBefore);
    EXPECT_EQ(VNX::PlayablePropertyAuthoringV1::definitionToJson(*sound), soundBefore);
    EXPECT_FALSE(track->isDirtyObject());
    EXPECT_FALSE(sound->isDirtyObject());
    EXPECT_EQ(counters.itemPropertyChanged, 0);
    EXPECT_EQ(undoStackCount(), 0);
}

TEST_F(PlayablePropertyEditingTest, ContainerOccurrenceActionsRejectEditingWithoutMutation)
{
    auto& pm = VNX::ProjectManager::instance();
    VNX::ProjectItem* container = pm.createItem(
        nullptr, QStringLiteral("OccurrenceContainer"),
        VNX::ProjectItem::Type::Container);
    ASSERT_NE(container, nullptr);
    VNX::ProjectItem* child = pm.createItem(
        container, QStringLiteral("OccurrenceChild"),
        VNX::ProjectItem::Type::Sound);
    ASSERT_NE(child, nullptr);
    VNX::ProjectItem* other = pm.createItem(
        container, QStringLiteral("OccurrenceOther"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(other, nullptr);
    const auto locator = VNX::PlayablePropertyAuthoringV1::containerChildLocator(
        *container, child->guid);
    ASSERT_TRUE(locator.has_value());
    const auto otherLocator = VNX::PlayablePropertyAuthoringV1::containerChildLocator(
        *container, other->guid);
    ASSERT_TRUE(otherLocator.has_value());
    const auto volume = static_cast<uint8_t>(harkenplayableproperties::PlayablePropertyIdV1::Volume);
    const auto otherBefore = VNX::PlayablePropertyAuthoringV1::readProperty(
        *container, *otherLocator, volume, std::nullopt);
    ASSERT_TRUE(otherBefore.has_value());

    QJsonObject args{
        {QStringLiteral("authoring_handle"),
         static_cast<qint64>(container->objectID)},
        {QStringLiteral("property"), QStringLiteral("sends")},
        {QStringLiteral("override_operation"), QStringLiteral("set_override")},
        {QStringLiteral("occurrence_kind"), QStringLiteral("container_child")},
        {QStringLiteral("occurrence_guid"),
         child->guid.toString(QUuid::WithoutBraces)},
        {QStringLiteral("occurrence_target_guid"),
         child->guid.toString(QUuid::WithoutBraces)},
        {QStringLiteral("occurrence_ordinal"),
         static_cast<qint64>(locator->stableOccurrenceOrdinal)},
        {QStringLiteral("value"), QJsonArray{}}};
    const auto groupsBefore = container->fieldGroups;
    const auto rowsBefore = container->comp<VNX::VSelection>()->selections.shared;
    const auto containerBefore = VNX::PlayablePropertyAuthoringV1::definitionToJson(*container);
    const auto childBefore = VNX::PlayablePropertyAuthoringV1::definitionToJson(*child);
    const auto otherDefinitionBefore = VNX::PlayablePropertyAuthoringV1::definitionToJson(*other);
    VNX::UndoRedoManager::instance().clear();
    container->clearDirty();
    child->clearDirty();
    other->clearDirty();
    SignalCounters counters;
    const auto rejected = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"), args, VNX::ActionSource::IPC);
    EXPECT_FALSE(rejected.success);
    EXPECT_EQ(rejected.errorCode, QStringLiteral("FIELD_GROUP_COMMAND_REQUIRED"));
    EXPECT_EQ(container->fieldGroups, groupsBefore);
    EXPECT_EQ(undoStackCount(), 0);

    args[QStringLiteral("property")] = QStringLiteral("volume");
    args[QStringLiteral("value")] = -7.5;
    for (const auto* operation : {"set_override", "clear_override"}) {
        args[QStringLiteral("override_operation")] = QLatin1String(operation);
        const auto result = VNX::ActionManager::instance().execute(
            QStringLiteral("set_item_property"), args, VNX::ActionSource::IPC);
        EXPECT_FALSE(result.success);
    }
    VNX::PropertyEditSession session(container, *locator, volume);
    EXPECT_FALSE(session.isActive());
    session.preview(QVariant(-9.0f));
    session.commit();
    const auto state = VNX::PlayablePropertyAuthoringV1::readProperty(
        *container, *locator, volume, std::nullopt);
    ASSERT_TRUE(state.has_value());
    EXPECT_FALSE(state->present);
    EXPECT_DOUBLE_EQ(state->value.toDouble(), 0.0);
    EXPECT_EQ(VNX::PlayablePropertyAuthoringV1::readProperty(
                  *container, *otherLocator, volume, std::nullopt), otherBefore);

    QJsonObject staleArgs = args;
    staleArgs[QStringLiteral("occurrence_ordinal")] = static_cast<qint64>(
        locator->stableOccurrenceOrdinal + 1);
    EXPECT_FALSE(VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"), staleArgs,
        VNX::ActionSource::IPC).success);
    EXPECT_EQ(container->comp<VNX::VSelection>()->selections.shared, rowsBefore);
    EXPECT_EQ(VNX::PlayablePropertyAuthoringV1::definitionToJson(*container), containerBefore);
    EXPECT_EQ(VNX::PlayablePropertyAuthoringV1::definitionToJson(*child), childBefore);
    EXPECT_EQ(VNX::PlayablePropertyAuthoringV1::definitionToJson(*other), otherDefinitionBefore);
    EXPECT_FALSE(container->isDirtyObject());
    EXPECT_FALSE(child->isDirtyObject());
    EXPECT_FALSE(other->isDirtyObject());
    EXPECT_EQ(counters.itemPropertyChanged, 0);
    EXPECT_EQ(undoStackCount(), 0);
}

TEST_F(SpatialItemPropertyTest, LayerSpatialTripletExecuteUndoRedoRoundtrip)
{
    auto& pm = VNX::ProjectManager::instance();
    VNX::ProjectItem* layer = pm.createItem(nullptr, QStringLiteral("SP_Layer"),
                                            VNX::ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    auto* spatial = layer->comp<VNX::VSpatial>();
    ASSERT_NE(spatial, nullptr);
    ASSERT_EQ(layer->comp<VNX::VAudio>(), nullptr);

    const auto initialGroups = layer->fieldGroups;
    auto* stack = VNX::UndoRedoManager::instance().undoStack();
    const int undoBefore = stack->count();
    const auto r3d = setFieldGroupField(layer, QStringLiteral("spatialRating"),
        QStringLiteral("spatial.is3DEnabled"), true);
    ASSERT_TRUE(r3d.success) << r3d.errorMessage.toStdString();
    EXPECT_TRUE(spatial->is3DEnabled);
    ASSERT_TRUE(layer->fieldGroups.spatialRating.shared.localHistory);
    EXPECT_TRUE(layer->fieldGroups.spatialRating.shared.localHistory->spatialIs3DEnabled);
    ASSERT_EQ(stack->count(), undoBefore + 1);
    stack->undo();
    EXPECT_EQ(layer->fieldGroups, initialGroups);
    EXPECT_FALSE(spatial->is3DEnabled);
    stack->redo();
    EXPECT_TRUE(spatial->is3DEnabled);

    const VNX::ActionResult rMix = handleAudioItemProperty(
        &pm, layer, QStringLiteral("spatializationMix"),
        QJsonObject{{QStringLiteral("value"), 0.25}}, nullptr);
    ASSERT_TRUE(rMix.success);
    EXPECT_FLOAT_EQ(spatial->spatializationMix, 0.25f);
    rMix.undoFn();
    EXPECT_FLOAT_EQ(spatial->spatializationMix, 1.0f);
    rMix.redoFn();
    EXPECT_FLOAT_EQ(spatial->spatializationMix, 0.25f);

    const auto beforeScale = layer->fieldGroups;
    const auto rScale = setFieldGroupField(layer, QStringLiteral("spatialRating"),
        QStringLiteral("spatial.attenuationDistanceScaling"), 2.5);
    ASSERT_TRUE(rScale.success) << rScale.errorMessage.toStdString();
    EXPECT_FLOAT_EQ(spatial->attenuationDistanceScaling, 2.5f);
    EXPECT_FLOAT_EQ(layer->fieldGroups.spatialRating.shared.localHistory->spatialAttenuationDistanceScaling, 2.5f);
    ASSERT_EQ(stack->count(), undoBefore + 2);
    stack->undo();
    EXPECT_EQ(layer->fieldGroups, beforeScale);
    EXPECT_FLOAT_EQ(spatial->attenuationDistanceScaling, 1.0f);
    stack->redo();
    EXPECT_FLOAT_EQ(spatial->attenuationDistanceScaling, 2.5f);
    EXPECT_FLOAT_EQ(spatial->spatializationMix, 0.25f);
    EXPECT_TRUE(spatial->is3DEnabled);

    EXPECT_TRUE(layer->isDirtyObject());
}

TEST_F(SpatialItemPropertyTest, SoundFullFieldSetRoutesToOwningComponents)
{
    auto& pm = VNX::ProjectManager::instance();
    VNX::ProjectItem* sound = pm.createItem(nullptr, QStringLiteral("SP_Sound"),
                                            VNX::ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);

    const struct { const char* group; const char* prop; QJsonValue value; } plan[] = {
        {nullptr, "volume", -6.0},
        {nullptr, "pitch", 3.0},
        {nullptr, "makeUpGain", 2.0},
        {nullptr, "loopCount", 0},
        {nullptr, "streamingMode", 1},
        {nullptr, "spatializationMix", 0.5},
        {"spatialRating", "spatial.attenuationDistanceScaling", 2.0},
        {"spatialRating", "spatial.is3DEnabled", true},
        {"mainOutput", "outputBusVolume", -3.0},
    };
    for (const auto& step : plan) {
        const auto result = step.group
            ? setFieldGroupField(sound, QLatin1String(step.group), QLatin1String(step.prop), step.value)
            : handleAudioItemProperty(&pm, sound, QLatin1String(step.prop),
                QJsonObject{{QStringLiteral("value"), step.value}}, nullptr);
        ASSERT_TRUE(result.success) << step.prop << ": " << result.errorMessage.toStdString();
    }

    const auto* mix = sound->comp<VNX::VMixNode>();
    ASSERT_NE(mix, nullptr);
    EXPECT_FLOAT_EQ(mix->volume, -6.0f);
    EXPECT_FLOAT_EQ(mix->pitch, 3.0f);
    EXPECT_FLOAT_EQ(mix->outputBusVolume, -3.0f);
    EXPECT_FLOAT_EQ(mix->makeUpGain, 2.0f);

    const auto* audio = sound->comp<VNX::VAudio>();
    ASSERT_NE(audio, nullptr);
    EXPECT_EQ(audio->loopCount, 0u);
    EXPECT_EQ(audio->streamingMode, 1);

    const auto* spatial = sound->comp<VNX::VSpatial>();
    ASSERT_NE(spatial, nullptr);
    EXPECT_FLOAT_EQ(spatial->spatializationMix, 0.5f);
    EXPECT_FLOAT_EQ(spatial->attenuationDistanceScaling, 2.0f);
    EXPECT_TRUE(spatial->is3DEnabled);
    ASSERT_TRUE(sound->fieldGroups.spatialRating.shared.localHistory);
    ASSERT_TRUE(sound->fieldGroups.mainOutput.shared.localHistory);
    const auto& rating = *sound->fieldGroups.spatialRating.shared.localHistory;
    EXPECT_FLOAT_EQ(rating.spatialAttenuationDistanceScaling, 2.0f);
    EXPECT_TRUE(rating.spatialIs3DEnabled);
    EXPECT_FLOAT_EQ(sound->fieldGroups.mainOutput.shared.localHistory->outputBusVolume, -3.0f);
}
