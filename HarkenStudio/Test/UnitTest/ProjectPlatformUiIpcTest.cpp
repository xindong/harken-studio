#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Core/ActionManager.h"
#include "Core/AssetPipeline/AssetConverter.h"
#include "Core/AssetPipeline/AssetImporter.h"
#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/AssetPipeline/PathUtils.h"
#include "Core/Components/VAction.h"
#include "Core/IPC/IPCServer.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/PackGenerationRecord.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectPlatformActivity.h"
#include "Core/UndoCommands.h"
#include "RuntimeBridge/PackagingBridge.h"
#include "Widgets/Common/MultiSelectComboBox.h"
#include "Widgets/Common/SplitButton.h"
#include "Widgets/Publisher/PublisherPanel.h"
#include "Support/AITestSupport.h"

#include <gtest/gtest.h>
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>

namespace {
using namespace VNX;
namespace Platforms = ProjectPlatformSemantics;

class ProjectPlatformUiIpc : public testing::Test
{
protected:
    void SetUp() override
    {
        TestSupport::ensureApp();
        auto& manager = ProjectManager::instance();
        if (manager.isProjectOpen()) manager.closeProject();
        PlatformContextService::instance().setStorageForTesting(std::make_unique<MemorySettingsStorage>());
        ActionManager::instance().clearAllForTesting();
        registerProjectActions(ActionManager::instance(), &manager, nullptr, {});
        ProjectConfig config;
        config.name = "PlatformIpc";
        config.location = directory.path();
        ASSERT_TRUE(manager.createProject(config));
        server.setProjectManager(&manager);
        server.setAppState(IPCServer::AppState::Ready);
        low = create("Android_Low", 22050);
        high = create("Android_High", 44100);
        asset.sourceID = 123;
        asset.relativePath = "123.wav";
        asset.absolutePath = QDir(PathUtils::getAudioSourcesDir(AssetRegistry::instance().projectRoot())).filePath(asset.relativePath);
        ASSERT_TRUE(AssetConverter::writeFloatWav(asset.absolutePath, std::vector<float>(960, 0.25f), 2, 48000));
        AssetImporter importer;
        asset.metadata = importer.analyzeFile(asset.absolutePath);
        asset.contentHash = AssetImporter::computeContentHash(asset.absolutePath);
        ASSERT_TRUE(AssetRegistry::instance().registerAsset(asset));
        AudioSourceData source;
        source.sourceID = asset.sourceID;
        source.relativePath = asset.relativePath;
        manager.currentProject()->audioSources.push_back(source);
        auto* layer = manager.createItem(nullptr, "Layer", ProjectItem::Type::Layer);
        auto* sound = manager.createItem(layer, "Sound", ProjectItem::Type::Sound);
        auto* component = sound->comp<VSound>();
        component->sources.plain.entries = {{QUuid::createUuid(), asset.sourceID}};
        component->sources.plain.active = component->sources.plain.entries.front().entryId;
        auto* action = manager.createItem(nullptr, "Play", ProjectItem::Type::Action);
        VActionStep step;
        step.targetObjectID = sound->objectID;
        action->comp<VAction>()->steps = {step};
        actionId = action->runtimeId();
        UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        TestSupport::spinUntil([&]() { return !call("get_pack_build_status").value("result").toObject().value("in_progress").toBool(); }, 30000);
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
        ActionManager::instance().clearAllForTesting();
        PackGenerationRecordStore::instance().clear();
    }

    QJsonObject call(const QString& cmd, const QJsonObject& args = {}) { return server.dispatchCommand(cmd, args); }
    QString create(const QString& name, int rate = 48000)
    {
        ProjectPlatformProfile profile;
        profile.performResample = true;
        profile.sizePolicies.front().config.targetSampleRate = rate;
        const auto result = call("create_project_platform", {{"name", name}, {"base_platform", "Android"}, {"profile", profile.toJson()}});
        EXPECT_EQ(result.value("status").toString(), QStringLiteral("ok"));
        return result.value("result").toObject().value("platform_id").toString();
    }
    QJsonObject transcode(const QString& id)
    {
        const QJsonObject args{{"platform_id", id}, {"source_id", int(asset.sourceID)}};
        const auto started = call("transcode_asset", args);
        EXPECT_EQ(started.value("status").toString(), QStringLiteral("ok"));
        EXPECT_EQ(started.value("result").toObject().value("platform_id").toString(), id);
        EXPECT_TRUE(TestSupport::spinUntil([&]() {
            return !call("get_transcode_status", args).value("result").toObject().value("in_progress").toBool();
        }, 30000));
        return call("get_transcode_status", args).value("result").toObject().value("result").toObject();
    }
    QTemporaryDir directory;
    IPCServer server{nullptr};
    QString low, high;
    AssetInfo asset;
    harkenstudio::ObjectId actionId = harkentest::StudioObjectId(0);
};

TEST_F(ProjectPlatformUiIpc, BaseChangesUseStoredDefaultsAndRetiredProcessingInputsFail)
{
    const auto id = create("Changeable", 32000);
    const auto changed = call("set_project_platform_base", {{"platform_id", id}, {"base_platform", "Windows"}});
    ASSERT_EQ(changed.value("status").toString(), QStringLiteral("ok"));
    const auto definition = changed.value("result").toObject().value("platform").toObject();
    EXPECT_EQ(definition.value("id").toString(), id);
    EXPECT_EQ(definition.value("name").toString(), QStringLiteral("Changeable"));
    EXPECT_EQ(definition.value("profile").toObject(), ProjectPlatformProfile::fromBasePlatform(BasePlatform::Windows).toJson());
    const QJsonObject query{{"source_id", int(asset.sourceID)}};
    const auto before = call("get_conversion_settings", query);
    ASSERT_EQ(before.value("status").toString(), QStringLiteral("ok"));
    EXPECT_FALSE(before.value("result").toObject().contains("normalize"));
    EXPECT_FALSE(before.value("result").toObject().contains("normalizeTargetDb"));
    EXPECT_FALSE(before.value("result").toObject().contains("forceMono"));
    EXPECT_FALSE(before.value("result").toObject().contains("trimSilence"));
    EXPECT_FALSE(before.value("result").toObject().contains("trimSilenceThresholdDb"));
    for (const auto* key : {"normalize", "normalize_target_db", "force_mono", "trim_silence", "trim_threshold_db"}) {
        auto args = query;
        args.insert(key, key == QStringLiteral("normalize") ? QJsonValue(true) : QJsonValue(-1.0));
        EXPECT_EQ(call("set_conversion_settings", args).value("status").toString(), QStringLiteral("error"));
        EXPECT_EQ(call("get_conversion_settings", query), before);
    }
}

TEST_F(ProjectPlatformUiIpc, InvalidSizeLimitTablesCannotReplaceTheProfile)
{
    const QJsonObject query{{"platform_id", low}};
    const auto before = call("get_platform_profile", query);
    for (const auto& rows : std::vector<std::vector<SizePolicy>>{
             {}, {{512, {}}}, {{512, {}}, {512, {}}, {std::nullopt, {}}},
             {{1024, {}}, {512, {}}, {std::nullopt, {}}}}) {
        ProjectPlatformProfile profile{true, rows};
        auto args = query;
        args.insert("profile", profile.toJson());
        EXPECT_EQ(call("set_project_platform_profile", args).value("status").toString(), QStringLiteral("error"));
        EXPECT_EQ(call("get_platform_profile", query), before);
    }
}

TEST_F(ProjectPlatformUiIpc, DiscoveryProfilesAndTranscodePollingDistinguishSiblingInstances)
{
    const auto discovery = call("list_project_platforms").value("result").toObject();
    const auto platforms = discovery.value("platforms").toArray();
    ASSERT_EQ(platforms.size(), 3);
    EXPECT_EQ(platforms[1].toObject().value("id").toString(), low);
    EXPECT_EQ(platforms[2].toObject().value("id").toString(), high);
    QString previousPath;
    for (const auto& id : {low, high}) {
        const auto profile = call("get_platform_profile", {{"platform_id", id}}).value("result").toObject();
        EXPECT_EQ(discovery.value("effective_profiles").toObject().value(id), profile.value("effective_profile"));
        EXPECT_EQ(profile.value("platform").toObject().value("basePlatform").toString(), QStringLiteral("Android"));
        const auto converted = transcode(id);
        ASSERT_EQ(converted.value("status").toString(), QStringLiteral("ok"));
        const auto result = converted.value("result").toObject();
        EXPECT_EQ(result.value("platform_id").toString(), id);
        const auto path = QDir(PathUtils::getMediaCacheDir(AssetRegistry::instance().projectRoot())).filePath(result.value("cache_path").toString());
        EXPECT_NE(path, previousPath);
        previousPath = path;
        std::vector<float> samples;
        int channels = 0, rate = 0;
        ASSERT_TRUE(HarkenEditor::Bridge::decodeAudioFileInterleaved(path, samples, channels, rate));
        EXPECT_EQ(rate, id == low ? 22050 : 44100);
        EXPECT_EQ(channels, 2);
        const auto status = call("get_media_status", {{"platform_id", id}, {"source_ids", QJsonArray{int(asset.sourceID)}}});
        EXPECT_EQ(status.value("result").toObject().value("platform_id").toString(), id);
    }
    EXPECT_EQ(call("get_transcode_status", {{"platform_id", low}, {"source_id", int(asset.sourceID)}}).value("code").toString(), QStringLiteral("CONVERSION_NOT_FOUND"));
    EXPECT_EQ(call("set_project_platform_profile", {{"platform_id", high}, {"profile", QJsonValue::Null}}).value("status").toString(), QStringLiteral("ok"));
    EXPECT_EQ(call("get_transcode_status", {{"platform_id", high}, {"source_id", int(asset.sourceID)}}).value("code").toString(), QStringLiteral("STALE_CONVERSION"));
}

TEST_F(ProjectPlatformUiIpc, LegacyNamesAndMixedFieldsFailBeforeBuildOrMutation)
{
    for (const auto& value : {QStringLiteral("Android"), QStringLiteral("Android_Low"), QUuid::createUuid().toString(QUuid::WithoutBraces), QString()}) {
        const auto result = call("export_pack", {{"platform_id", value}, {"output_path", directory.filePath("Rejected")}});
        EXPECT_EQ(result.value("status").toString(), QStringLiteral("error"));
    }
    EXPECT_EQ(call("export_pack", {{"platform", low}, {"platform_id", high}}).value("code").toString(), QStringLiteral("INVALID_PLATFORM"));
    EXPECT_EQ(call("get_action_pack_plan", {{"actionStableId", VNX::objectIdString(actionId)}, {"platforms", QJsonArray{low}}}).value("code").toString(), QStringLiteral("INVALID_ARGS"));
    EXPECT_FALSE(QDir(directory.filePath("Rejected")).exists());
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
    const auto plan = call("get_action_pack_plan", {{"actionStableId", VNX::objectIdString(actionId)}, {"platform_ids", QJsonArray{low, high}}});
    ASSERT_EQ(plan.value("status").toString(), QStringLiteral("ok"));
    const auto variants = plan.value("result").toObject().value("variants").toArray();
    ASSERT_EQ(variants.size(), 2);
    EXPECT_EQ(variants[0].toObject().value("platform_name").toString(), QStringLiteral("Android_Low"));
    EXPECT_EQ(variants[1].toObject().value("platform_id").toString(), high);
}

TEST_F(ProjectPlatformUiIpc, NarrowPackSelectorExportsOnlyTheRequestedSiblingAndReportsCapturedNames)
{
    for (int i = 0; i < 6; ++i) create(QString("Android_Long_Quality_Preset_%1").arg(i));
    PlatformContextService::instance().setSelected({QUuid(low)}, QUuid(low), false);
    PublisherPanel panel;
    panel.resize(380, 480);
    panel.show();
    QApplication::processEvents();
    auto* selector = panel.findChild<MultiSelectComboBox*>("packPlatformCombo");
    ASSERT_NE(selector, nullptr);
    EXPECT_EQ(panel.width(), 380);
    selector->showPopup();
    QApplication::processEvents();
    auto* view = selector->view();
    ASSERT_EQ(selector->count(), 9);
    for (int i = 0; i < selector->count(); ++i) {
        if (selector->itemData(i).toString() == high) continue;
        const auto index = selector->model()->index(i, 0);
        view->scrollTo(index);
        QApplication::processEvents();
        const QPoint center = view->visualRect(index).center();
        const QPoint global = view->viewport()->mapToGlobal(center);
        QMouseEvent press(QEvent::MouseButtonPress, center, global,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view->viewport(), &press);
        QMouseEvent release(QEvent::MouseButtonRelease, center, global,
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(view->viewport(), &release);
        QApplication::processEvents();
        EXPECT_TRUE(view->isVisible());
    }
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(view, &enter);
    QApplication::processEvents();
    EXPECT_FALSE(view->isVisible());
    EXPECT_EQ(panel.generationPlatforms(), QStringList{high});
    EXPECT_EQ(PlatformContextService::instance().context().primary, QUuid(low));
    EXPECT_EQ(selector->selectedText(), QStringLiteral("Android_High"));
    auto* exportButton = panel.findChild<QAbstractButton*>("projectPackGenerateAllBtn");
    ASSERT_NE(exportButton, nullptr);
    QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
    QApplication::sendEvent(selector, &down);
    QApplication::processEvents();
    view->setCurrentIndex(selector->model()->index(selector->findData(high), 0));
    QKeyEvent space(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    QApplication::sendEvent(view, &space);
    EXPECT_TRUE(view->isVisible());
    EXPECT_TRUE(panel.generationPlatforms().isEmpty());
    EXPECT_TRUE(selector->selectedText().isEmpty());
    EXPECT_FALSE(exportButton->isEnabled());
    view->setCurrentIndex(selector->model()->index(selector->findData(high), 0));
    QApplication::sendEvent(view, &space);
    EXPECT_EQ(selector->selectedData(), QVariantList{high});
    EXPECT_EQ(selector->selectedText(), QStringLiteral("Android_High"));
    EXPECT_TRUE(view->isVisible());
    EXPECT_TRUE(exportButton->isEnabled());
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(view, &escape);
    QApplication::processEvents();
    EXPECT_FALSE(view->isVisible());
    QStringList requested;
    QObject::connect(&panel, &PublisherPanel::exportRequested, &panel,
        [&](const QStringList& ids, const QStringList&) {
            requested = ids;
            ASSERT_EQ(call("export_pack", {{"platform_id", ids.first()}, {"output_path", directory.filePath("Output")}}).value("status").toString(), QStringLiteral("ok"));
        });
    panel.findChild<QAbstractButton*>("projectPackGenerateAllBtn")->click();
    EXPECT_EQ(requested, QStringList{high});
    ASSERT_TRUE(TestSupport::spinUntil([&]() {
        return !call("get_pack_build_status").value("result").toObject().value("in_progress").toBool();
    }, 30000));
    const auto status = call("get_pack_build_status").value("result").toObject().value("result").toObject();
    ASSERT_EQ(status.value("status").toString(), QStringLiteral("ok"));
    const auto result = status.value("result").toObject();
    EXPECT_TRUE(result.value("success").toBool());
    EXPECT_EQ(result.value("platform_id").toString(), high);
    EXPECT_EQ(result.value("platform_name").toString(), QStringLiteral("Android_High"));
    EXPECT_TRUE(QDir(directory.filePath("Output/Android_High")).exists());
    EXPECT_FALSE(QDir(directory.filePath("Output/Android_Low")).exists());
    const auto records = call("get_last_pack_build_records").value("result").toObject().value("records").toArray();
    ASSERT_FALSE(records.isEmpty());
    for (const auto& value : records) {
        const auto record = value.toObject();
        EXPECT_EQ(record.value("platform_id").toString(), high);
        EXPECT_EQ(record.value("platform_name").toString(), QStringLiteral("Android_High"));
        EXPECT_EQ(record.value("base_platform").toString(), QStringLiteral("Android"));
    }
    ProjectManager::instance().closeProject();
    EXPECT_TRUE(call("get_pack_build_status").value("result").toObject().value("result").toObject().isEmpty());
}

TEST_F(ProjectPlatformUiIpc, GlobalContextSeparatesObservationEditingAndExportScope)
{
    auto& manager = ProjectManager::instance();
    auto& context = PlatformContextService::instance();
    context.setSelected({QUuid(low)}, QUuid(low), false);
    manager.setPackExportSelectedPlatformsInternal({high});
    PublisherPanel panel;
    panel.show();
    QApplication::processEvents();
    auto* tree = panel.findChild<QTreeWidget*>();
    ASSERT_NE(tree, nullptr);
    const auto row = [&]() {
        QApplication::processEvents();
        return tree->findItems(QStringLiteral("Play"), Qt::MatchExactly | Qt::MatchRecursive).value(0);
    };
    ASSERT_NE(row(), nullptr);
    const auto* action = manager.findItemByRuntimeStableId(actionId);
    ASSERT_NE(action, nullptr);
    for (const auto& platform : {low, high}) {
        PackGenerationRecord record;
        record.source = QStringLiteral("Action");
        record.packName = action->name;
        record.objectID = action->objectID;
        record.unitKey = QStringLiteral("a:") + objectIdString(actionId);
        record.platform = platform;
        record.success = true;
        record.elapsedMs = platform == low ? 10 : 20;
        panel.addGenerationResult(record);
    }
    panel.refreshPackList();
    EXPECT_EQ(row()->text(5), QStringLiteral("10 ms"));
    EXPECT_EQ(row()->checkState(0), Qt::Checked);

    manager.setPackExportSelectedPlatformsInternal({});
    EXPECT_TRUE(panel.generationPlatforms().isEmpty());
    EXPECT_EQ(row()->text(5), QStringLiteral("10 ms"));
    EXPECT_TRUE(row()->flags().testFlag(Qt::ItemIsUserCheckable));
    row()->setCheckState(0, Qt::Unchecked);
    context.setSelected({QUuid(high)}, QUuid(high), false);
    QApplication::processEvents();
    const auto included = [&](const QString& platform) {
        const auto targeting = manager.getActionPackTargeting();
        return std::none_of(targeting.begin(), targeting.end(), [&](const auto& entry) {
            return entry.stableId == actionId && !entry.includesPlatform(platform);
        });
    };
    EXPECT_FALSE(included(low));
    EXPECT_TRUE(included(high));
    EXPECT_EQ(row()->text(5), QStringLiteral("20 ms"));
    EXPECT_EQ(row()->checkState(0), Qt::Checked);

    context.setAll(true);
    EXPECT_EQ(row()->text(5), QStringLiteral("20 ms"));
    EXPECT_EQ(row()->checkState(0), Qt::PartiallyChecked);
    row()->setCheckState(0, Qt::Unchecked);
    QApplication::processEvents();
    for (const auto& platform : manager.currentProject()->platformIds())
        EXPECT_FALSE(included(platform));
    EXPECT_EQ(row()->checkState(0), Qt::Unchecked);

    context.setSelected({QUuid(low)}, QUuid(low), true);
    EXPECT_EQ(row()->text(5), QStringLiteral("10 ms"));
    EXPECT_EQ(row()->checkState(0), Qt::Unchecked);
    row()->setCheckState(0, Qt::Checked);
    QApplication::processEvents();
    for (const auto& platform : manager.currentProject()->platformIds())
        EXPECT_TRUE(included(platform));

    manager.setPackExportSelectedPlatformsInternal({high});
    QStringList requested;
    QStringList requestedPaths;
    bool forceRequested = false;
    bool cleaningRequested = false;
    QObject::connect(&panel, &PublisherPanel::exportRequested, &panel,
        [&](const QStringList& ids, const QStringList& paths, bool, bool force, bool cleaning) {
            requested = ids;
            requestedPaths = paths;
            forceRequested = force;
            cleaningRequested = cleaning;
        });
    panel.findChild<QAbstractButton*>("projectPackGenerateAllBtn")->click();
    EXPECT_EQ(requested, QStringList{high});
    auto* force = panel.findChild<QCheckBox*>("packForceReexportCheck");
    auto* cleaning = panel.findChild<QCheckBox*>("packSelfCleaningCheck");
    ASSERT_NE(force, nullptr);
    ASSERT_NE(cleaning, nullptr);
    EXPECT_FALSE(force->isChecked());
    EXPECT_TRUE(cleaning->isChecked());
    EXPECT_FALSE(forceRequested);
    EXPECT_TRUE(cleaningRequested);
    force->setChecked(true);
    cleaning->setChecked(false);
    panel.findChild<QAbstractButton*>("projectPackGenerateAllBtn")->click();
    EXPECT_EQ(requested, QStringList{high});
    EXPECT_TRUE(forceRequested);
    EXPECT_FALSE(cleaningRequested);
    cleaning->setChecked(true);
    panel.resize(1000, 600);
    panel.show();
    tree->expandAll();
    row()->setSelected(true);
    QApplication::processEvents();
    for (const bool forceExport : {false, true}) {
        const QPoint contextPoint = tree->visualItemRect(row()).center();
        QTimer::singleShot(0, &panel, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            ASSERT_NE(menu, nullptr);
            const QString label = forceExport ? QStringLiteral("Force Re-export") : QStringLiteral("Export");
            const auto actions = menu->actions();
            const auto found = std::find_if(actions.begin(), actions.end(), [&](const auto* action) {
                return action->text() == label;
            });
            EXPECT_NE(found, actions.end());
            if (found != actions.end()) (*found)->trigger();
            menu->close();
        });
        tree->customContextMenuRequested(contextPoint);
        EXPECT_EQ(requestedPaths, QStringList{QStringLiteral("Play")});
        EXPECT_EQ(forceRequested, forceExport);
        EXPECT_FALSE(cleaningRequested);
    }
    {
        const auto busy = ProjectPlatformActivity::acquire();
        EXPECT_FALSE(force->isEnabled());
        EXPECT_FALSE(cleaning->isEnabled());
        EXPECT_FALSE(panel.findChild<QAbstractButton*>("projectPackGenerateAllBtn")->isEnabled());
    }
    EXPECT_TRUE(force->isEnabled());
    EXPECT_TRUE(cleaning->isEnabled());
    EXPECT_EQ(row()->text(5), QStringLiteral("10 ms"));
    EXPECT_EQ(context.context().primary, QUuid(low));
    EXPECT_TRUE(context.context().all);
    manager.setPackExportSelectedPlatformsInternal({});
    EXPECT_FALSE(panel.findChild<QAbstractButton*>("projectPackGenerateAllBtn")->isEnabled());
}

}
