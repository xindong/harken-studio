#include <harken/Harken.h>
#include "UnitTest/StudioObjectIdTestSupport.h"
#include "ActionManager.h"
#include "Components/VAction.h"
#include "IPC/IPCServer.h"
#include "PackGenerationRecord.h"
#include "ProjectManager.h"
#include "ProjectPackExporter.h"
#include "RuntimeCapabilityMatrix.h"
#include "Support/AITestSupport.h"
#include "UndoCommands.h"
#include "Support/ProjectPlatformTestUtils.h"

#include <harken/core/CurrentWorldCodec.h>
#include <harken/media/PackLogicImage.h>

#include <gtest/gtest.h>

#include <QAbstractSocket>
#include <QDataStream>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSet>
#include <QTemporaryDir>
#include <QTcpSocket>

#include <algorithm>

using namespace VNX;

namespace {

ProjectItem* findByName(ProjectData* project, const QString& name)
{
    if (!project) return nullptr;
    std::function<ProjectItem*(ProjectItem*)> visit = [&](ProjectItem* item) -> ProjectItem* {
        if (!item) return nullptr;
        if (item->name == name) return item;
        for (auto& child : item->children)
            if (auto* found = visit(child.get())) return found;
        return nullptr;
    };
    for (auto* binding : project->allFileBindings())
        if (binding && binding->rootItem)
            if (auto* found = visit(binding->rootItem.get())) return found;
    return nullptr;
}

QJsonObject gatedPlayStepJson(qint64 targetObjectID, const QString& targetName)
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("Play")},
        {QStringLiteral("targetObjectID"), targetObjectID},
        {QStringLiteral("targetName"), targetName},
        {QStringLiteral("delay"), 0.0},
        {QStringLiteral("quantize"), QJsonObject{
            {QStringLiteral("referenceObjectID"), 0},
            {QStringLiteral("referenceName"), QString()},
            {QStringLiteral("timing"), QStringLiteral("EntryMarker")},
            {QStringLiteral("ifSilent"), QStringLiteral("Drop")}}}};
}

void writeTestWav(const QString& path)
{
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    constexpr quint32 frames = 96;
    stream.writeRawData("RIFF", 4);
    stream << quint32(36 + frames * 2);
    stream.writeRawData("WAVE", 4);
    stream.writeRawData("fmt ", 4);
    stream << quint32(16) << quint16(1) << quint16(1) << quint32(48000)
           << quint32(96000) << quint16(2) << quint16(16);
    stream.writeRawData("data", 4);
    stream << quint32(frames * 2);
    for (quint32 frame = 0; frame < frames; ++frame)
        stream << qint16((frame % 13) * 1000);
    EXPECT_EQ(stream.status(), QDataStream::Ok);
}

class PackBuildIpcScenarioTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        TestSupport::ensureApp();
        auto& manager = ProjectManager::instance();
        if (manager.isProjectOpen()) manager.closeProject();
        ActionManager::instance().clearAllForTesting();
        registerProjectActions(ActionManager::instance(), &manager, nullptr, {});
        UndoRedoManager::instance().clear();
        PackGenerationRecordStore::instance().clear();
        ASSERT_TRUE(temp_.isValid());
        server_.setProjectManager(&manager);
        server_.setAppState(IPCServer::AppState::Ready);
        ASSERT_TRUE(server_.start(0));
        socket_.connectToHost(QHostAddress::LocalHost,
                              static_cast<quint16>(server_.port()));
        ASSERT_TRUE(TestSupport::spinUntil([this] {
            return socket_.state() == QAbstractSocket::ConnectedState;
        }));
        const QJsonObject created = call(
            QStringLiteral("create_project"),
            {{QStringLiteral("name"), QStringLiteral("PackBuildIpcProject")},
             {QStringLiteral("location"), temp_.path()}});
        ASSERT_EQ(created.value(QStringLiteral("status")).toString(),
                  QStringLiteral("ok"));
        projectPath_ = created.value(QStringLiteral("result")).toObject()
                           .value(QStringLiteral("path")).toString();
    }

    void TearDown() override
    {
        socket_.disconnectFromHost();
        TestSupport::spinUntil([this] {
            return socket_.state() == QAbstractSocket::UnconnectedState;
        });
        server_.stop();
        UndoRedoManager::instance().clear();
        PackGenerationRecordStore::instance().clear();
        if (ProjectManager::instance().isProjectOpen())
            ProjectManager::instance().closeProject();
        ActionManager::instance().clearAllForTesting();
    }

    QString platformId() const
    {
        return ProjectPlatformSemantics::idString(ProjectManager::instance().currentProject()->platforms.first().id);
    }

    QJsonObject call(const QString& command, const QJsonObject& args = {})
    {
        const QString id = QStringLiteral("phase6-%1").arg(++request_);
        const QByteArray payload = QJsonDocument(QJsonObject{
            {QStringLiteral("cmd"), command},
            {QStringLiteral("args"), args},
            {QStringLiteral("request_id"), id}}).toJson(QJsonDocument::Compact)
            + QByteArrayLiteral("\n");
        EXPECT_EQ(socket_.write(payload), payload.size());
        socket_.flush();
        if (!TestSupport::spinUntil([this] { return socket_.canReadLine(); })) return {};
        const QJsonObject response =
            QJsonDocument::fromJson(socket_.readLine().trimmed()).object();
        EXPECT_EQ(response.value(QStringLiteral("request_id")).toString(), id);
        return response;
    }

    QJsonObject create(const QString& name, const QString& type)
    {
        const QJsonObject response = call(
            QStringLiteral("create_item"),
            {{QStringLiteral("name"), name}, {QStringLiteral("type"), type}});
        EXPECT_EQ(response.value(QStringLiteral("status")).toString(),
                  QStringLiteral("ok"));
        return response.value(QStringLiteral("result")).toObject();
    }

    QTemporaryDir temp_;
    IPCServer server_{nullptr};
    QTcpSocket socket_;
    QString projectPath_;
    int request_ = 0;
};

}

TEST_F(PackBuildIpcScenarioTest,
       PackPlanCapabilityFailureUndoAndReloadShareOneMachineContract)
{
    const QJsonObject created = create(QStringLiteral("GatedAction"),
                                       QStringLiteral("Action"));
    const QString wavPath = temp_.filePath(QStringLiteral("PlayTarget.wav"));
    ASSERT_NO_FATAL_FAILURE(writeTestWav(wavPath));
    const QJsonObject imported = call(QStringLiteral("import_audio"),
                                      {{QStringLiteral("path"), wavPath}});
    ASSERT_EQ(imported.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    const QJsonObject playTarget = imported.value(QStringLiteral("result")).toObject();
    const QString path = created.value(QStringLiteral("path")).toString();
    ProjectItem* action = findByName(ProjectManager::instance().currentProject(),
                                     QStringLiteral("GatedAction"));
    ASSERT_NE(action, nullptr);
    const harkenstudio::ObjectId stableId = action->runtimeId();
    const QUuid actionGuid = action->guid;

    const QJsonArray steps{gatedPlayStepJson(
        playTarget.value(QStringLiteral("target_parent_authoring_handle")).toInteger(),
        QStringLiteral("PlayTarget"))};
    const QJsonObject edited = call(
        QStringLiteral("set_item_property"),
        {{QStringLiteral("path"), path},
         {QStringLiteral("property"), QStringLiteral("steps")},
         {QStringLiteral("value"), steps}});
    ASSERT_EQ(edited.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    ASSERT_EQ(action->comp<VAction>()->steps.size(), 1u);

    const QJsonObject planResponse = call(
        QStringLiteral("get_action_pack_plan"),
        {{QStringLiteral("actionStableId"), VNX::objectIdString(stableId)},
         {QStringLiteral("platform_ids"), QJsonArray{platformId()}}});
    ASSERT_EQ(planResponse.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    const QJsonArray variants = planResponse.value(QStringLiteral("result"))
                                    .toObject().value(QStringLiteral("variants")).toArray();
    ASSERT_EQ(variants.size(), 1);
    const QJsonObject capability = variants.at(0).toObject()
                                       .value(QStringLiteral("capability")).toObject();
    EXPECT_FALSE(capability.value(QStringLiteral("permitted")).toBool(true));

    const QJsonObject capabilityResponse = call(
        QStringLiteral("get_runtime_capability_matrix"),
        {{QStringLiteral("actionStableId"), VNX::objectIdString(stableId)}});
    ASSERT_EQ(capabilityResponse.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    const QJsonObject machineCapability =
        capabilityResponse.value(QStringLiteral("result")).toObject();
    EXPECT_EQ(machineCapability.value(QStringLiteral("state")),
              capability.value(QStringLiteral("state")));
    EXPECT_EQ(machineCapability.value(QStringLiteral("issues")),
              capability.value(QStringLiteral("issues")));

    PackGenerationRecord staleRecord;
    staleRecord.packName = QStringLiteral("PreviousProject");
    PackGenerationRecordStore::instance().add(staleRecord);
    const QString output = temp_.filePath(QStringLiteral("gated"));
    const QJsonObject startedBuild = call(
        QStringLiteral("export_pack"),
        {{QStringLiteral("output_path"), output},
         {QStringLiteral("action_name"), QStringLiteral("GatedAction")},
         {QStringLiteral("platform_id"), platformId()}});
    ASSERT_EQ(startedBuild.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    EXPECT_TRUE(PackGenerationRecordStore::instance().records().isEmpty());
    QJsonObject buildStatus;
    ASSERT_TRUE(TestSupport::spinUntil([&] {
        buildStatus = call(QStringLiteral("get_pack_build_status"));
        return buildStatus.value(QStringLiteral("status")).toString()
                   == QStringLiteral("ok")
            && !buildStatus.value(QStringLiteral("result")).toObject()
                    .value(QStringLiteral("in_progress")).toBool(true);
    }, 10000));
    EXPECT_TRUE(buildStatus.value(QStringLiteral("result")).toObject()
                    .value(QStringLiteral("message")).toString()
                    .contains(QStringLiteral(
                        "Entry Marker requires an updated Engine marker scheduling contract")));
    QDirIterator published(output, {QStringLiteral("*.bytes"), QStringLiteral("*.pack")}, QDir::Files,
                           QDirIterator::Subdirectories);
    EXPECT_FALSE(published.hasNext());
    const QJsonArray records = call(QStringLiteral("get_last_pack_build_records"))
                                   .value(QStringLiteral("result")).toObject()
                                   .value(QStringLiteral("records")).toArray();
    const auto failure = std::find_if(records.begin(), records.end(), [](const QJsonValue& value) {
        return value.toObject().value(QStringLiteral("packName")).toString()
            == QStringLiteral("GatedAction");
    });
    ASSERT_NE(failure, records.end());
    EXPECT_FALSE(failure->toObject().value(QStringLiteral("success")).toBool(true));
    EXPECT_TRUE(failure->toObject().value(QStringLiteral("errors")).toString()
                    .contains(QStringLiteral(
                        "Entry Marker requires an updated Engine marker scheduling contract")));
    EXPECT_TRUE(std::none_of(records.begin(), records.end(), [](const QJsonValue& value) {
        return value.toObject().value(QStringLiteral("packName")).toString()
            == QStringLiteral("PreviousProject");
    }));

    ASSERT_EQ(call(QStringLiteral("undo")).value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    EXPECT_TRUE(action->comp<VAction>()->steps.empty());
    ASSERT_EQ(call(QStringLiteral("redo")).value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    ASSERT_EQ(action->comp<VAction>()->steps.size(), 1u);

    ASSERT_EQ(call(QStringLiteral("save_project"))
                  .value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    ASSERT_EQ(call(QStringLiteral("open_project"),
                   {{QStringLiteral("path"), projectPath_}})
                  .value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    action = findByName(ProjectManager::instance().currentProject(),
                        QStringLiteral("GatedAction"));
    ASSERT_NE(action, nullptr);
    EXPECT_EQ(action->guid, actionGuid);
    EXPECT_EQ(action->runtimeId(), stableId);
    ASSERT_EQ(action->comp<VAction>()->steps.size(), 1u);
    const QJsonArray reopenedRecords =
        call(QStringLiteral("get_last_pack_build_records"))
            .value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("records")).toArray();
    EXPECT_TRUE(reopenedRecords.isEmpty());

    const QJsonObject reopened = call(
        QStringLiteral("get_action_pack_plan"),
        {{QStringLiteral("actionStableId"),
          VNX::objectIdString(action->runtimeId())},
         {QStringLiteral("platform_ids"), QJsonArray{platformId()}}});
    EXPECT_EQ(reopened.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
}

TEST_F(PackBuildIpcScenarioTest, PackPlanQueryBoundsAndDeduplicatesDimensions)
{
    create(QStringLiteral("BoundedAction"), QStringLiteral("Action"));
    const ProjectItem* action = findByName(
        ProjectManager::instance().currentProject(), QStringLiteral("BoundedAction"));
    ASSERT_NE(action, nullptr);
    const QString stableId = VNX::objectIdString(action->runtimeId());

    const QJsonObject deduplicated = call(
        QStringLiteral("get_action_pack_plan"),
        {{QStringLiteral("actionStableId"), stableId},
         {QStringLiteral("platform_ids"),
          QJsonArray{platformId(), platformId()}},
         {QStringLiteral("languages"), QJsonArray{}}});
    ASSERT_EQ(deduplicated.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    EXPECT_EQ(deduplicated.value(QStringLiteral("result")).toObject()
                  .value(QStringLiteral("variants")).toArray().size(), 1);

    QJsonArray oversized;
    for (int i = 0; i < 65; ++i) oversized.append(platformId());
    const QJsonObject tooLarge = call(
        QStringLiteral("get_action_pack_plan"),
        {{QStringLiteral("actionStableId"), stableId},
         {QStringLiteral("platform_ids"), oversized},
         {QStringLiteral("languages"), QJsonArray{}}});
    EXPECT_EQ(tooLarge.value(QStringLiteral("status")).toString(),
              QStringLiteral("error"));
    EXPECT_EQ(tooLarge.value(QStringLiteral("code")).toString(),
              QStringLiteral("INVALID_ARGS"));

    const QJsonObject unsupported = call(
        QStringLiteral("get_action_pack_plan"),
        {{QStringLiteral("actionStableId"), stableId},
         {QStringLiteral("platform_ids"), QJsonArray{QStringLiteral("Unknown")}},
         {QStringLiteral("languages"), QJsonArray{}}});
    EXPECT_EQ(unsupported.value(QStringLiteral("status")).toString(),
              QStringLiteral("error"));
    EXPECT_EQ(unsupported.value(QStringLiteral("code")).toString(),
              QStringLiteral("INVALID_ARGS"));

    const QJsonObject missingAction = call(QStringLiteral("get_action_pack_plan"));
    EXPECT_EQ(missingAction.value(QStringLiteral("status")).toString(),
              QStringLiteral("error"));
    EXPECT_EQ(missingAction.value(QStringLiteral("code")).toString(),
              QStringLiteral("INVALID_ARGS"));
}

TEST_F(PackBuildIpcScenarioTest, EngineDeploymentCommandsAreNotExposed)
{
    for (const QString& command : {QStringLiteral("set_unity_project_path"),
                                   QStringLiteral("get_unity_project_path"),
                                   QStringLiteral("deploy_packs_to_unity"),
                                   QStringLiteral("set_pack_file_extension")}) {
        const QJsonObject response = call(command);
        EXPECT_EQ(response.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
        EXPECT_EQ(response.value(QStringLiteral("code")).toString(),
                  QStringLiteral("UNKNOWN_COMMAND"));
    }
}

TEST_F(PackBuildIpcScenarioTest, OmittedOutputPathUsesTheProjectBuildDirectory)
{
    const QString relative = QStringLiteral("Exports/Audio.pack");
    ASSERT_EQ(call(QStringLiteral("set_pack_build_output_dir"),
                   {{QStringLiteral("path"), relative}})
                  .value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    const QString outputRoot = QDir(QFileInfo(projectPath_).absolutePath()).filePath(relative);
    const QJsonObject settings = call(QStringLiteral("get_pack_export_settings"))
        .value(QStringLiteral("result")).toObject();
    EXPECT_EQ(settings.value(QStringLiteral("pack_build_output_dir")).toString(), relative);
    EXPECT_EQ(settings.value(QStringLiteral("pack_build_output_root")).toString(), outputRoot);
    EXPECT_EQ(settings.value(QStringLiteral("pack_file_extension")).toString(), QStringLiteral(".bytes"));
    EXPECT_FALSE(settings.contains(QStringLiteral("unity_project_path")));
    EXPECT_FALSE(settings.contains(QStringLiteral("unity_pack_deploy_dir")));
    EXPECT_FALSE(settings.contains(QStringLiteral("auto_deploy")));

    const QJsonObject started = call(QStringLiteral("export_pack"),
                                    {{QStringLiteral("platform_id"), platformId()}});
    ASSERT_EQ(started.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    ASSERT_TRUE(TestSupport::spinUntil([this] {
        return !server_.dispatchCommand(QStringLiteral("get_pack_build_status"), {})
                    .value(QStringLiteral("result")).toObject()
                    .value(QStringLiteral("in_progress")).toBool(true);
    }, 10000));
    const auto records = PackGenerationRecordStore::instance().records();
    ASSERT_FALSE(records.isEmpty());
    EXPECT_TRUE(QFileInfo::exists(QDir(outputRoot).filePath(QStringLiteral("HarkenPackIndex.json"))));
    for (const auto& record : records) {
        EXPECT_TRUE(record.success);
        EXPECT_EQ(QFileInfo(record.outputPath).absolutePath(),
                  QDir(outputRoot).filePath(QStringLiteral("Windows")));
        EXPECT_TRUE(QFileInfo::exists(record.outputPath));
    }
}

TEST_F(PackBuildIpcScenarioTest, CanceledBuildCannotCompleteOrPublishOverItsSuccessor)
{
    create(QStringLiteral("CanceledAction"), QStringLiteral("Action"));
    create(QStringLiteral("SuccessorAction"), QStringLiteral("Action"));
    const QJsonObject first = server_.dispatchCommand(
        QStringLiteral("export_pack"),
        {{QStringLiteral("output_path"), temp_.filePath(QStringLiteral("canceled"))},
         {QStringLiteral("action_name"), QStringLiteral("CanceledAction")},
         {QStringLiteral("platform_id"), platformId()}});
    ASSERT_EQ(first.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    ASSERT_TRUE(ProjectPackExporter::waitForBackgroundExports(10000));

    ASSERT_EQ(server_.dispatchCommand(QStringLiteral("cancel_pack_build"), {})
                  .value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    const QJsonObject blocked = server_.dispatchCommand(
        QStringLiteral("export_pack"),
        {{QStringLiteral("output_path"), temp_.filePath(QStringLiteral("successor"))},
         {QStringLiteral("action_name"), QStringLiteral("SuccessorAction")},
         {QStringLiteral("platform_id"), platformId()}});
    EXPECT_EQ(blocked.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
    EXPECT_EQ(blocked.value(QStringLiteral("code")).toString(),
              QStringLiteral("PACK_BUILD_IN_PROGRESS"));

    QCoreApplication::sendPostedEvents(&server_, QEvent::MetaCall);

    const QJsonObject status = server_.dispatchCommand(
        QStringLiteral("get_pack_build_status"), {});
    ASSERT_EQ(status.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    EXPECT_FALSE(status.value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("in_progress")).toBool(true));
    EXPECT_TRUE(PackGenerationRecordStore::instance().records().isEmpty());

    const QJsonObject second = server_.dispatchCommand(
        QStringLiteral("export_pack"),
        {{QStringLiteral("output_path"), temp_.filePath(QStringLiteral("successor"))},
         {QStringLiteral("action_name"), QStringLiteral("SuccessorAction")},
         {QStringLiteral("platform_id"), platformId()}});
    ASSERT_EQ(second.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    ASSERT_TRUE(TestSupport::spinUntil([this] {
        return !server_.dispatchCommand(QStringLiteral("get_pack_build_status"), {})
                    .value(QStringLiteral("result")).toObject()
                    .value(QStringLiteral("in_progress")).toBool(true);
    }, 10000));
    const QVector<PackGenerationRecord> records =
        PackGenerationRecordStore::instance().records();
    EXPECT_TRUE(std::any_of(records.begin(), records.end(), [](const auto& record) {
        return record.packName == QStringLiteral("SuccessorAction") && !record.success;
    }));
    EXPECT_TRUE(std::none_of(records.begin(), records.end(), [](const auto& record) {
        return record.packName == QStringLiteral("CanceledAction");
    }));
}

TEST_F(PackBuildIpcScenarioTest, PackFilePathsAndAmbiguousActionRequestsAreRejectedBeforeStarting)
{
    create(QStringLiteral("RequestedAction"), QStringLiteral("Action"));
    PackGenerationRecord previous;
    previous.packName = QStringLiteral("PreviousBuild");
    PackGenerationRecordStore::instance().add(previous);
    for (const QString& relative : {QStringLiteral("single.pack"),
                                    QStringLiteral("single.PACK"),
                                    QStringLiteral("single.PACK/"),
                                    QStringLiteral("single.bytes"),
                                    QStringLiteral("single.BYTES/"),
                                    QStringLiteral("single.pack.bytes")}) {
        const QString output = temp_.filePath(relative);
        const QJsonObject rejected = call(
            QStringLiteral("export_pack"),
            {{QStringLiteral("output_path"), output},
             {QStringLiteral("action_name"), QStringLiteral("RequestedAction")},
             {QStringLiteral("platform_id"), platformId()}});
        EXPECT_EQ(rejected.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
        EXPECT_EQ(rejected.value(QStringLiteral("code")).toString(), QStringLiteral("INVALID_ARGS"));
        EXPECT_TRUE(rejected.value(QStringLiteral("message")).toString()
                        .contains(QStringLiteral("output directory")));
        EXPECT_FALSE(QFileInfo::exists(output));
    }
    const QString unknownOutput = temp_.filePath(QStringLiteral("unknown"));
    const QJsonObject unknown = call(
        QStringLiteral("export_pack"),
        {{QStringLiteral("output_path"), unknownOutput},
         {QStringLiteral("action_name"), QStringLiteral("UnknownAction")},
         {QStringLiteral("platform_id"), platformId()}});
    EXPECT_EQ(unknown.value(QStringLiteral("code")).toString(), QStringLiteral("INVALID_ARGS"));
    EXPECT_FALSE(QFileInfo::exists(unknownOutput));
    create(QStringLiteral("DuplicateAction"), QStringLiteral("Action"));
    auto* duplicate = findByName(ProjectManager::instance().currentProject(),
                                 QStringLiteral("DuplicateAction"));
    ASSERT_NE(duplicate, nullptr);
    duplicate->name = QStringLiteral("RequestedAction");
    const QString ambiguousOutput = temp_.filePath(QStringLiteral("ambiguous"));
    const QJsonObject ambiguous = call(
        QStringLiteral("export_pack"),
        {{QStringLiteral("output_path"), ambiguousOutput},
         {QStringLiteral("action_name"), QStringLiteral("RequestedAction")},
         {QStringLiteral("platform_id"), platformId()}});
    EXPECT_EQ(ambiguous.value(QStringLiteral("code")).toString(), QStringLiteral("INVALID_ARGS"));
    EXPECT_TRUE(ambiguous.value(QStringLiteral("message")).toString()
                    .contains(QStringLiteral("ambiguous")));
    EXPECT_FALSE(QFileInfo::exists(ambiguousOutput));
    EXPECT_FALSE(call(QStringLiteral("get_pack_build_status"))
                     .value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("in_progress")).toBool(true));
    EXPECT_FALSE(ProjectPackExporter::hasBackgroundExportInFlight());
    const auto records = PackGenerationRecordStore::instance().records();
    ASSERT_EQ(records.size(), 1);
    EXPECT_EQ(records.front().packName, QStringLiteral("PreviousBuild"));
}

TEST_F(PackBuildIpcScenarioTest, DirectoryExportKeepsTheNamedActionSeparateFromSameNamedGroups)
{
    const QString wavPath = temp_.filePath(QStringLiteral("tone.wav"));
    ASSERT_NO_FATAL_FAILURE(writeTestWav(wavPath));
    const QJsonObject imported = call(QStringLiteral("import_audio"),
                                      {{QStringLiteral("path"), wavPath}});
    ASSERT_EQ(imported.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    const QJsonObject source = imported.value(QStringLiteral("result")).toObject();
    const QJsonArray steps{QJsonObject{
        {QStringLiteral("type"), QStringLiteral("Play")},
        {QStringLiteral("targetObjectID"),
         source.value(QStringLiteral("target_parent_authoring_handle"))},
        {QStringLiteral("targetName"), source.value(QStringLiteral("target_parent_name"))}}};
    for (const QString& name : {QStringLiteral("RequestedAction"), QStringLiteral("UnrelatedAction")}) {
        const QJsonObject action = create(name, QStringLiteral("Action"));
        const QJsonObject edited = call(
            QStringLiteral("set_item_property"),
            {{QStringLiteral("path"), action.value(QStringLiteral("path"))},
             {QStringLiteral("property"), QStringLiteral("steps")},
             {QStringLiteral("value"), steps}});
        ASSERT_EQ(edited.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    }
    const ProjectItem* action = findByName(ProjectManager::instance().currentProject(),
                                           QStringLiteral("RequestedAction"));
    ASSERT_NE(action, nullptr);
    const ProjectItem* unrelated = findByName(ProjectManager::instance().currentProject(),
                                              QStringLiteral("UnrelatedAction"));
    ASSERT_NE(unrelated, nullptr);
    PackGroup group;
    group.name = action->name;
    group.actionIds = {unrelated->runtimeId()};
    ProjectManager::instance().currentProject()->packGroups.push_back(group);
    const QString output = temp_.filePath(QStringLiteral("Generated"));
    const QJsonObject started = call(
        QStringLiteral("export_pack"),
        {{QStringLiteral("output_path"), output},
         {QStringLiteral("action_name"), action->name},
         {QStringLiteral("platform_id"), platformId()}});
    ASSERT_EQ(started.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    QJsonObject status;
    ASSERT_TRUE(TestSupport::spinUntil([&] {
        status = call(QStringLiteral("get_pack_build_status"))
                     .value(QStringLiteral("result")).toObject();
        return !status.value(QStringLiteral("in_progress")).toBool(true);
    }, 10000));
    ASSERT_EQ(status.value(QStringLiteral("message")).toString(), QStringLiteral("Export complete"));
    EXPECT_EQ(status.value(QStringLiteral("progress")).toInt(), 100);
    EXPECT_TRUE(QFileInfo::exists(QDir(output).filePath(
        HarkenEditor::Bridge::packIndexFileName())));

    const QDir platformDir(QDir(output).filePath(QStringLiteral("Windows")));
    QFile manifestFile(platformDir.filePath(HarkenEditor::Bridge::packManifestFileName()));
    ASSERT_TRUE(manifestFile.open(QIODevice::ReadOnly));
    const QJsonArray packs = QJsonDocument::fromJson(manifestFile.readAll()).object()
                                 .value(QStringLiteral("packs")).toArray();
    ASSERT_EQ(packs.size(), 2);
    QSet<QString> names;
    for (const QJsonValue& value : packs) {
        const QJsonObject pack = value.toObject();
        const QString name = pack.value(QStringLiteral("name")).toString();
        names.insert(name);
        const QString relative = pack.value(QStringLiteral("path")).toString();
        QFile file(platformDir.filePath(relative));
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        harken::PackCanonicalImage image;
        ASSERT_EQ(VN_OK, harken::ParsePackCanonicalImage(
            reinterpret_cast<const std::uint8_t*>(bytes.constData()),
            static_cast<std::uint64_t>(bytes.size()), &image));
        ASSERT_TRUE(image.hasPartition);
        EXPECT_TRUE(image.hasRequirement);
        harken::CurrentWorldBodyImage world;
        ASSERT_TRUE(world.Decode(image.partitionBody.data(), image.partitionBody.size(),
                                 image.partitionHeader.bodyVintage,
                                 harken::CurrentWorldDecodeMode::StrictArtifact));
        ASSERT_NE(world.ProductionView(), nullptr);
        if (name == action->name) {
            EXPECT_EQ(relative, action->name + QStringLiteral(".bytes"));
            ASSERT_EQ(world.ProductionView()->actionCount, 1u);
            EXPECT_EQ(world.ProductionView()->actions[0].actionStableId, action->runtimeId());
            ASSERT_EQ(image.media.size(), 1u);
            EXPECT_FALSE(image.dependencyEdges.empty());
        } else {
            EXPECT_EQ(name, QStringLiteral("Init"));
            EXPECT_EQ(world.ProductionView()->actionCount, 0u);
        }
    }
    EXPECT_EQ(names, (QSet<QString>{QStringLiteral("Init"), QStringLiteral("RequestedAction")}));
    const auto records = PackGenerationRecordStore::instance().records();
    ASSERT_EQ(records.size(), 2);
    for (const auto& record : records) {
        EXPECT_TRUE(record.success) << record.errors.toStdString();
        EXPECT_TRUE(names.contains(record.packName));
    }
}

TEST_F(PackBuildIpcScenarioTest, RuntimeCapabilityQueriesShareOperationsAndBackends)
{
    auto& manager = ProjectManager::instance();
    auto* piece = manager.createItem(nullptr, QStringLiteral("Absolute"), ProjectItem::Type::Piece);
    ASSERT_NE(piece, nullptr);
    auto* track = manager.createItem(piece, QStringLiteral("Track"), ProjectItem::Type::Track);
    ASSERT_NE(track, nullptr);
    auto* variable = manager.createItem(nullptr, QStringLiteral("Variable"), ProjectItem::Type::Variable);
    ASSERT_NE(variable, nullptr);
    auto* project = manager.currentProject();
    for (const auto operation : {RuntimeCapabilityOperation::LiveSync,
                                 RuntimeCapabilityOperation::AuditionPlay,
                                 RuntimeCapabilityOperation::PackBuild,
                                 RuntimeCapabilityOperation::Delete}) {
        for (auto* item : {track, piece, variable}) {
            const auto response = call(QStringLiteral("get_runtime_capability_matrix"),
                {{QStringLiteral("rootStableId"), VNX::objectIdString(item->runtimeId())},
                 {QStringLiteral("operation"), RuntimeCapabilityMatrix::operationName(operation)}});
            ASSERT_EQ(response.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
            const auto actual = response.value(QStringLiteral("result")).toObject();
            const auto expected = RuntimeCapabilityMatrix::assess(*item, *project, operation);
            EXPECT_EQ(actual, expected.toJson());
            EXPECT_EQ(actual.value(QStringLiteral("permitted")).toBool(),
                      item != variable || operation != RuntimeCapabilityOperation::AuditionPlay);
            if (item != variable)
                EXPECT_EQ(actual.value(QStringLiteral("backend")).toString(),
                    operation == RuntimeCapabilityOperation::PackBuild
                        ? QStringLiteral("Pack") : QStringLiteral("CurrentWorld"));
        }
    }
    const auto rejected = call(QStringLiteral("get_runtime_capability_matrix"),
        {{QStringLiteral("rootStableId"), VNX::objectIdString(piece->runtimeId())},
         {QStringLiteral("operation"), QStringLiteral("unknown")}});
    EXPECT_EQ(rejected.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
}
