#include "Core/Components/VSelection.h"
#include "Core/FieldGroupProjectAdapter.h"
#include "Core/ActionManager.h"
#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/Components/VTrack.h"
#include "Core/CurrentWorldArtifactWriter.h"
#include "Core/CurrentWorldProjectAdapter.h"
#include "Core/LanguageContextService.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/PropertyEditSession.h"
#include "Core/SoundSourceEditing.h"
#include "Core/SoundSourceSemantics.h"
#include "Core/UndoCommands.h"
#include "RuntimeBridge/CurrentWorldSyncBridgeInternal.h"
#include "Widgets/Shared/PlatformFieldMerge.h"
#include "Widgets/Inspector/InspectorCustomizationBootstrap.h"
#include "Widgets/Inspector/InspectorCustomizationRegistry.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/Common/ParameterSlider.h"
#include "Widgets/Timeline/TimelineEditorPanel.h"

#include <gtest/gtest.h>
#include <QApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QCheckBox>

#include <algorithm>

namespace {
using namespace VNX;
namespace Platforms = ProjectPlatformSemantics;
using Authoring = PlayablePropertyAuthoringV1;

class ProjectPlatformAuthoring : public testing::Test
{
protected:
    void SetUp() override
    {
        if (!qApp) {
            static int argc = 1;
            static char name[] = "harken_editor_tests";
            static char* argv[] = {name, nullptr};
            static QApplication app(argc, argv);
        }
        if (manager().isProjectOpen()) manager().closeProject();
        PlatformContextService::instance().setStorageForTesting(std::make_unique<MemorySettingsStorage>());
        LanguageContextService::instance().setStorageForTesting(std::make_unique<MemorySettingsStorage>());
        ASSERT_TRUE(directory.isValid());
        ProjectConfig config;
        config.name = "PlatformAuthoring";
        config.location = directory.path();
        ASSERT_TRUE(manager().createProject(config));
        windows = project().platforms.front().id;
        ActionManager::instance().clearAllForTesting();
        registerProjectActions(ActionManager::instance(), &manager(), nullptr, {});
        low = createPlatform("Android_Low");
        high = createPlatform("Android_High");
        ASSERT_FALSE(low.isNull());
        ASSERT_FALSE(high.isNull());
        auto* layer = manager().createItem(nullptr, "Layer", ProjectItem::Type::Layer);
        container = manager().createItem(layer, "Container", ProjectItem::Type::Container);
        track = manager().createItem(container, "Track", ProjectItem::Type::Track);
        sound = manager().createItem(track, "Sound", ProjectItem::Type::Sound);
        ASSERT_NE(sound, nullptr);
        ASSERT_NE(track, nullptr);
        track->fieldGroups.spatialRating.shared = {FieldGroupMode::Local, SpatialRatingFieldGroup{}};
        auto* timeline = track->comp<VTrack>();
        ASSERT_EQ(timeline->clips.items.size(), 1u);
        definition = *Authoring::definitionLocator(*sound);
        owners = {sound, container, track};
        locators = {definition, *Authoring::definitionLocator(*container),
                    *Authoring::definitionLocator(*track)};
        setSources(false);
        undo().clear();
        for (auto* item : Platforms::referenceOwners(project())) item->clearDirty();
    }

    void TearDown() override
    {
        PropertyEditSession::finishAllActive(false);
        undo().clear();
        if (manager().isProjectOpen()) manager().closeProject();
        ActionManager::instance().clearAllForTesting();
    }

    ProjectManager& manager() { return ProjectManager::instance(); }
    ProjectData& project() { return *manager().currentProject(); }
    UndoRedoManager& undo() { return UndoRedoManager::instance(); }
    ActionResult run(const QString& command, const QJsonObject& args)
    {
        return ActionManager::instance().execute(command, args, ActionSource::IPC);
    }
    ProjectPlatformId createPlatform(const QString& name)
    {
        const auto result = run("create_project_platform", {{"name", name}, {"base_platform", "Android"}});
        EXPECT_TRUE(result.success) << result.errorMessage.toStdString();
        return QUuid(result.data.value("platform_id").toString());
    }
    void select(ProjectPlatformId id, bool all = false)
    {
        PlatformContextService::instance().setSelected(
            all ? occupiedTargetPlatforms(project().platformIds()) : QVector<ProjectPlatformId>{id}, id, all);
    }
    QJsonObject volumeArgs(int index, const QJsonArray& ids, double value,
                           const QString& operation = "set_override")
    {
        return {{"authoring_handle", static_cast<qint64>(owners.at(index)->objectID)},
            {"property", "volume"}, {"value", value}, {"platform_ids", ids},
            {"override_operation", operation}};
    }
    void setVolume(int index, ProjectPlatformId id, double value)
    {
        const auto result = run("set_item_property", volumeArgs(index, {Platforms::idString(id)}, value));
        ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    }
    double volume(int index, PlatformScope scope)
    {
        const auto state = Authoring::readProperty(*owners.at(index), locators.at(index), 1, scope);
        EXPECT_TRUE(state.has_value());
        return state ? state->value.toDouble() : 0;
    }
    void setSources(bool localized)
    {
        const QString language = localized ? QStringLiteral("English") : QString();
        auto* sources = sound->comp<VSound>();
        sources->distinguishLanguage = localized;
        auto& set = localized ? sources->sources.localized[language] : sources->sources.plain;
        set.entries = {{QUuid::createUuid(), 101}, {QUuid::createUuid(), 102}, {QUuid::createUuid(), 103}};
        set.platforms[Platforms::idString(low)].active = set.entries[0].entryId;
        set.platforms[Platforms::idString(high)].active = set.entries[1].entryId;
        set.active = set.entries[2].entryId;
        for (uint32_t id : {101u, 102u, 103u}) {
            AssetInfo asset;
            asset.sourceID = id;
            asset.relativePath = QString::number(id) + ".wav";
            asset.absolutePath = directory.filePath(asset.relativePath);
            asset.contentHash = QString::number(id);
            asset.metadata.sampleRate = 48000;
            asset.metadata.totalSamples = (id - 100) * 48000;
            asset.metadata.durationSec = id - 100;
            EXPECT_TRUE(AssetRegistry::instance().registerAsset(asset));
        }
        SoundSourceEditing::syncActiveProjection(*sound,
            PlatformContextService::instance().context().primary, language);
    }
    HarkenEditor::Bridge::CurrentWorldSyncRequestPtr request(ProjectPlatformId id)
    {
        QString error;
        auto result = buildCurrentWorldSyncRequest(project(), id, &error);
        EXPECT_NE(result, nullptr) << error.toStdString();
        return result;
    }
    void expectCompiledVolumes(ProjectPlatformId id, float expected)
    {
        const auto result = request(id);
        ASSERT_NE(result, nullptr);
        const auto nodeFor = [&](ProjectItem* owner) {
            return std::find_if(result->project.nodes.begin(), result->project.nodes.end(),
                [&](const auto& node) { return node.guid == owner->runtimeId(); });
        };
        const auto soundNode = nodeFor(sound);
        const auto containerNode = nodeFor(container);
        const auto trackNode = nodeFor(track);
        ASSERT_NE(soundNode, result->project.nodes.end());
        ASSERT_NE(containerNode, result->project.nodes.end());
        ASSERT_NE(trackNode, result->project.nodes.end());
        EXPECT_FLOAT_EQ(soundNode->volume, expected);
        EXPECT_FLOAT_EQ(containerNode->volume, expected);
        EXPECT_FLOAT_EQ(trackNode->volume, expected);
        ASSERT_EQ(containerNode->containerChildOccurrences.size(), 1);
        EXPECT_EQ(containerNode->containerChildOccurrences[0].presence.bits, 0u);
        EXPECT_FLOAT_EQ(containerNode->containerChildOccurrences[0].volume, 0.0f);
        ASSERT_EQ(trackNode->clips.size(), 1);
        EXPECT_EQ(trackNode->clips[0].presence.bits, 0u);
        EXPECT_FLOAT_EQ(trackNode->clips[0].volume, 0.0f);
        result->soundContentRevisions = {{sound->runtimeId(), 1}};
        const auto compiled = HarkenEditor::Bridge::compileCurrentWorldRequest(*result);
        EXPECT_TRUE(compiled.ok()) << static_cast<uint32_t>(compiled.status);
    }

    QTemporaryDir directory;
    ProjectPlatformId windows, low, high;
    ProjectItem* sound = nullptr;
    ProjectItem* container = nullptr;
    ProjectItem* track = nullptr;
    PlayablePropertyLocatorV1 definition;
    QVector<ProjectItem*> owners;
    QVector<PlayablePropertyLocatorV1> locators;
};

TEST_F(ProjectPlatformAuthoring, SiblingAndroidDefinitionOverridesCompileAndIgnoreOccurrenceValues)
{
    const auto seedLegacyValues = [&](PlayablePropertyOccurrenceV1& properties,
                                       PlatformOverrideLedgerV1& platforms) {
        constexpr auto volumeId = harkenplayableproperties::PlayablePropertyIdV1::Volume;
        properties.presence.Set(volumeId);
        ASSERT_TRUE(properties.setValue(volumeId, -18.0f));
        for (const auto id : {low, high}) {
            auto* row = platforms.ensure(id);
            ASSERT_NE(row, nullptr);
            row->properties.presence.Set(volumeId);
            ASSERT_TRUE(row->properties.setValue(volumeId, -24.0f));
        }
    };
    auto& clip = track->comp<VTrack>()->clips.items.front();
    seedLegacyValues(clip.playableProperties, clip.platformOverrides);
    for (int i = 0; i < owners.size(); ++i) {
        ASSERT_TRUE(Authoring::setOverride(*owners[i], locators[i], 1, -3.0, std::nullopt));
        setVolume(i, low, -6.0);
        setVolume(i, high, -12.0);
        EXPECT_DOUBLE_EQ(volume(i, windows), -3.0);
    }
    expectCompiledVolumes(low, -6.0f);
    expectCompiledVolumes(high, -12.0f);
    expectCompiledVolumes(windows, -3.0f);
    const auto guid = sound->guid;
    const auto containerGuid = container->guid;
    const auto trackGuid = track->guid;
    const auto path = project().projectPath;
    ASSERT_TRUE(manager().saveProject());
    manager().closeProject();
    auto prepared = ProjectManager::prepareProject(path);
    ASSERT_NE(prepared.project, nullptr) << prepared.error.toStdString();
    ASSERT_TRUE(manager().commitPreparedProject(std::move(prepared)));
    sound = manager().findItemByGuid(guid);
    container = manager().findItemByGuid(containerGuid);
    track = manager().findItemByGuid(trackGuid);
    ASSERT_NE(sound, nullptr);
    ASSERT_NE(container, nullptr);
    ASSERT_NE(track, nullptr);
    owners = {sound, container, track};
    ASSERT_EQ(container->comp<VNX::VSelection>()->selections.shared.occurrenceRows().size(), 1u);
    EXPECT_EQ(container->comp<VNX::VSelection>()->selections.shared.occurrenceRows().front().weight, 50);
    const auto* timeline = track->comp<VTrack>();
    ASSERT_NE(timeline, nullptr);
    ASSERT_EQ(timeline->clips.items.size(), 1u);
    EXPECT_EQ(timeline->clips.items.front().playableProperties.presence.bits, 0u);
    EXPECT_TRUE(timeline->clips.items.front().platformOverrides.rows.empty());
    expectCompiledVolumes(low, -6.0f);
    expectCompiledVolumes(high, -12.0f);
    const auto result = request(high);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->soundSources.size(), 1);
    EXPECT_EQ(result->soundSources[0].sourceId, 102u);
}

TEST_F(ProjectPlatformAuthoring, AllWritesSharedAndUndoRestoresEveryInstanceOverlay)
{
    for (int i = 0; i < owners.size(); ++i) {
        setVolume(i, low, -6.0);
        setVolume(i, high, -12.0);
    }
    undo().clear();
    select(high, true);
    for (int i = 0; i < owners.size(); ++i) {
        auto args = volumeArgs(i, {}, -9.0);
        args.remove("platform_ids");
        const auto result = run("set_item_property", args);
        ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
        EXPECT_DOUBLE_EQ(volume(i, std::nullopt), -9.0);
        EXPECT_FALSE(Authoring::readPlatformOverlay(*owners[i], locators[i], 1, low)->present);
        EXPECT_FALSE(Authoring::readPlatformOverlay(*owners[i], locators[i], 1, high)->present);
    }
    expectCompiledVolumes(low, -9.0f);
    expectCompiledVolumes(high, -9.0f);
    for (int i = 0; i < owners.size(); ++i) ASSERT_TRUE(undo().undo());
    expectCompiledVolumes(low, -6.0f);
    expectCompiledVolumes(high, -12.0f);
    for (int i = 0; i < owners.size(); ++i) ASSERT_TRUE(undo().redo());
    const auto added = createPlatform("Android_New");
    expectCompiledVolumes(added, -9.0f);
}

TEST_F(ProjectPlatformAuthoring, ClearOverrideUsesExactInstanceAtEveryDefinition)
{
    for (int i = 0; i < owners.size(); ++i) {
        ASSERT_TRUE(Authoring::setOverride(*owners[i], locators[i], 1, -3.0, std::nullopt));
        setVolume(i, low, -6.0);
        setVolume(i, high, -12.0);
        const auto cleared = run("set_item_property", volumeArgs(i,
            {Platforms::idString(low)}, 0, "clear_platform_override"));
        ASSERT_TRUE(cleared.success) << cleared.errorMessage.toStdString();
        EXPECT_DOUBLE_EQ(volume(i, low), -3.0);
        EXPECT_DOUBLE_EQ(volume(i, high), -12.0);
        ASSERT_TRUE(undo().undo());
        EXPECT_DOUBLE_EQ(volume(i, low), -6.0);
        ASSERT_TRUE(undo().redo());
        EXPECT_DOUBLE_EQ(volume(i, low), -3.0);
    }
}

TEST_F(ProjectPlatformAuthoring, ContextSwitchCommitsEveryActiveDefinitionBeforeObserversRun)
{
    select(low);
    QVector<std::shared_ptr<PropertyEditSession>> sessions;
    for (int i = 0; i < owners.size(); ++i) {
        sessions.append(std::make_shared<PropertyEditSession>(owners[i], locators[i], 1,
            nullptr, QVector<ProjectPlatformId>{low}));
        sessions.back()->preview(-7.0);
    }
    EXPECT_EQ(undo().undoStack()->count(), 0);
    QObject observer;
    int observed = 0;
    QObject::connect(&PlatformContextService::instance(), &PlatformContextService::contextChanged,
        &observer, [&](const PlatformContext&) {
            ++observed;
            for (int i = 0; i < owners.size(); ++i) {
                EXPECT_FALSE(sessions[i]->isActive());
                EXPECT_DOUBLE_EQ(volume(i, low), -7.0);
                EXPECT_DOUBLE_EQ(volume(i, high), 0.0);
            }
            EXPECT_EQ(undo().undoStack()->count(), 3);
        });
    select(high);
    EXPECT_EQ(observed, 1);
    for (auto& session : sessions) {
        session->preview(-20.0);
        session->commit();
    }
    EXPECT_EQ(undo().undoStack()->count(), 3);
    for (int i = 0; i < owners.size(); ++i) ASSERT_TRUE(undo().undo());
    for (int i = 0; i < owners.size(); ++i) {
        EXPECT_DOUBLE_EQ(volume(i, low), 0.0);
        EXPECT_DOUBLE_EQ(volume(i, high), 0.0);
    }
}

TEST_F(ProjectPlatformAuthoring, SameContextKeepsSessionActiveAndPrimaryOnlyChangeCommitsAll)
{
    select(low, true);
    PropertyEditSession session(sound, definition, 1, nullptr, std::nullopt);
    session.preview(-5.0);
    select(low, true);
    EXPECT_TRUE(session.isActive());
    EXPECT_TRUE(undo().isClean());
    select(high, true);
    EXPECT_FALSE(session.isActive());
    EXPECT_EQ(undo().undoStack()->count(), 1);
    EXPECT_TRUE(PlatformContextService::instance().context().all);
    EXPECT_DOUBLE_EQ(volume(0, std::nullopt), -5.0);
    ASSERT_TRUE(undo().undo());
    EXPECT_DOUBLE_EQ(volume(0, std::nullopt), 0.0);
}

TEST_F(ProjectPlatformAuthoring, SingleInstanceKeepsIndependentValuesAfterAnotherInstanceIsCreated)
{
    ASSERT_TRUE(run("delete_project_platform", {{"platform_id", Platforms::idString(low)}}).success);
    ASSERT_TRUE(run("delete_project_platform", {{"platform_id", Platforms::idString(high)}}).success);
    select(windows, false);
    PropertyEditSession session(sound, definition, 1, nullptr, QVector<ProjectPlatformId>{windows});
    session.preview(-4.0);
    const auto added = createPlatform("Android_AfterEdit");
    EXPECT_FALSE(session.isActive());
    EXPECT_DOUBLE_EQ(volume(0, std::nullopt), 0.0);
    EXPECT_DOUBLE_EQ(volume(0, added), 0.0);
    EXPECT_DOUBLE_EQ(volume(0, windows), -4.0);
    EXPECT_TRUE(Authoring::readPlatformOverlay(*sound, definition, 1, windows)->present);
}

TEST_F(ProjectPlatformAuthoring, SwitchingInstancesRefreshesSourcePathAndDurationWithoutAuthoredChanges)
{
    const auto ledger = SoundSourceSemantics::toJson(sound->comp<VSound>()->sources);
    select(low, true);
    EXPECT_EQ(sound->sourceID, 101u);
    EXPECT_EQ(sound->sourcePath, "101.wav");
    EXPECT_EQ(SoundSourceEditing::activeTrimmedDuration(*sound, low, {}), 1.0);
    select(high, true);
    EXPECT_EQ(sound->sourceID, 102u);
    EXPECT_EQ(sound->sourcePath, "102.wav");
    EXPECT_EQ(SoundSourceEditing::activeTrimmedDuration(*sound, high, {}), 2.0);
    EXPECT_TRUE(PlatformContextService::instance().context().all);
    EXPECT_EQ(SoundSourceSemantics::toJson(sound->comp<VSound>()->sources), ledger);
    EXPECT_FALSE(sound->isDirtyObject());
    EXPECT_TRUE(undo().isClean());
    const auto result = request(high);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->soundSources.size(), 1);
    EXPECT_EQ(result->soundSources[0].sourceId, 102u);
}

TEST_F(ProjectPlatformAuthoring, SourcesResolveExactInstanceAndLanguageThenSharedWithoutSiblingFallback)
{
    project().projectLanguages = {"English", "Japanese"};
    setSources(true);
    auto* sources = sound->comp<VSound>();
    const QUuid japanese = QUuid::createUuid();
    sources->sources.localized["Japanese"].entries.push_back({japanese, 104});
    sources->sources.localized["Japanese"].active = japanese;
    EXPECT_EQ(SoundSourceEditing::activeSourceId(*sound, low, "English"), 101u);
    EXPECT_EQ(SoundSourceEditing::activeSourceId(*sound, high, "English"), 102u);
    EXPECT_EQ(SoundSourceEditing::activeSourceId(*sound, windows, "English"), 103u);
    EXPECT_EQ(SoundSourceEditing::activeSourceId(*sound, low, "Japanese"), 104u);
    EXPECT_EQ(SoundSourceEditing::activeSourceId(*sound, high, "Japanese"), 104u);
    sources->sources.localized["English"].active = QUuid{};
    sources->sources.localized["English"].platforms.remove(Platforms::idString(high));
    const auto missing = SoundSourceSemantics::resolveSourceSelection(*sources,
        Platforms::idString(high), "English");
    EXPECT_EQ(missing.source, nullptr);
    EXPECT_FALSE(missing.error.isEmpty());
    EXPECT_EQ(SoundSourceEditing::activeSourceId(*sound, high, "English"), 0u);
    EXPECT_EQ(SoundSourceEditing::activeSourceId(*sound, high, "Japanese"), 104u);
}

TEST_F(ProjectPlatformAuthoring, ExplicitWorldArtifactIsIndependentOfPrimaryAndInstanceName)
{
    for (int i = 0; i < owners.size(); ++i) {
        setVolume(i, low, -6.0);
        setVolume(i, high, -12.0);
    }
    const auto artifact = [&](ProjectPlatformId id, const QString& fileName) {
        const QString path = directory.filePath(fileName);
        QString error;
        EXPECT_TRUE(WriteCurrentWorldArtifactV1(project(), id, path,
            {{sound->runtimeId(), SoundSourceEditing::activeSourceId(*sound, id, {})}},
            nullptr, &error)) << error.toStdString();
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::ReadOnly));
        return file.readAll();
    };
    select(windows);
    const auto fromWindows = artifact(high, "high-from-windows.vcw");
    ASSERT_FALSE(fromWindows.isEmpty());
    select(low);
    const auto fromLow = artifact(high, "high-from-low.vcw");
    EXPECT_EQ(fromLow, fromWindows);
    EXPECT_NE(artifact(low, "low.vcw"), fromWindows);
    ASSERT_TRUE(run("rename_project_platform", {{"platform_id", Platforms::idString(high)},
        {"name", "Mobile Premium"}}).success);
    EXPECT_EQ(artifact(high, "renamed.vcw"), fromWindows);
}

TEST_F(ProjectPlatformAuthoring, UnknownCompileIdIsRejectedBeforeSharedFallback)
{
    QString error;
    CurrentWorldBuildStatus status = CurrentWorldBuildStatus::Ready;
    EXPECT_EQ(buildCurrentWorldSyncRequest(project(), QUuid::createUuid(), &error, &status), nullptr);
    EXPECT_EQ(status, CurrentWorldBuildStatus::InvalidAuthoring);
    EXPECT_TRUE(error.contains("does not belong"));
    EXPECT_EQ(buildCurrentWorldSyncRequest(project(), ProjectPlatformId{}, &error), nullptr);
    EXPECT_NE(buildCurrentWorldSyncRequest(project(), std::nullopt, &error), nullptr);
    EXPECT_TRUE(undo().isClean());
}

TEST_F(ProjectPlatformAuthoring, MixedValuesDisplayBothInstanceNamesAndKeepTheirIds)
{
    setVolume(0, low, -6.0);
    setVolume(0, high, -12.0);
    PlatformContext context{{low, high}, low, false};
    const auto view = platformFieldView(*sound, definition, 1, context);
    EXPECT_FALSE(view.uniform);
    EXPECT_EQ(view.coverage, PlatformFieldView::Coverage::All);
    ASSERT_EQ(view.perPlatform.size(), 2);
    EXPECT_EQ(view.perPlatform[0].first, low);
    EXPECT_EQ(view.perPlatform[1].first, high);
    EXPECT_EQ(platformFieldMixedTooltip(view, project().platforms),
              "Android_Low: -6\nAndroid_High: -12");
    ASSERT_TRUE(run("rename_project_platform", {{"platform_id", Platforms::idString(high)},
        {"name", "Mobile Premium"}}).success);
    EXPECT_EQ(platformFieldMixedTooltip(view, project().platforms),
              "Android_Low: -6\nMobile Premium: -12");
}

TEST_F(ProjectPlatformAuthoring, SourceActiveControlsKeepSiblingInstancesSeparate)
{
    registerBuiltinInspectorCustomizations();
    auto* provider = InspectorCustomizationRegistry::instance().sectionProvider(
        ProjectItem::Type::Sound, "sources");
    ASSERT_NE(provider, nullptr);
    std::unique_ptr<QWidget> widget(provider->build(nullptr));
    provider->bind(widget.get(), sound);
    select(high, true);
    auto checks = widget->findChildren<QCheckBox*>("sourceActive");
    ASSERT_EQ(checks.size(), 3);
    EXPECT_EQ(checks[0]->checkState(), Qt::PartiallyChecked);
    select(low);
    EXPECT_TRUE(checks[0]->isChecked());
    const auto before = sound->comp<VSound>()->sources;
    checks[2]->click();
    EXPECT_EQ(SoundSourceEditing::activeSourceId(*sound, low, {}), 103u);
    EXPECT_EQ(SoundSourceEditing::activeSourceId(*sound, high, {}), 102u);
    ASSERT_TRUE(undo().undo());
    EXPECT_EQ(sound->comp<VSound>()->sources, before);
    select(high, true);
    checks[2]->click();
    for (const auto& platform : project().platforms)
        EXPECT_EQ(SoundSourceEditing::activeSourceId(*sound, platform.id, {}), 103u);
}
}
