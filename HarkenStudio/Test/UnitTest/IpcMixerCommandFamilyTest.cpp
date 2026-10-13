#include "ActionManager.h"
#include "Components/VMixNode.h"
#include "Components/VBus.h"
#include "IPC/IPCServer.h"
#include "ProjectManager.h"
#include "UndoCommands.h"

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QFile>
#include <QDirIterator>
#include <QTemporaryDir>
#include <map>

using namespace VNX;

namespace {

std::map<QString, QByteArray> savedProjectBytes(const QString& root)
{
    std::map<QString, QByteArray> result;
    QDirIterator files(root, QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString path = files.next();
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::ReadOnly));
        result.emplace(path, file.readAll());
    }
    return result;
}

class IpcMixerCommandFamilyTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto& manager = ProjectManager::instance();
        if (manager.isProjectOpen()) manager.closeProject();
        ASSERT_TRUE(tempDir_.isValid());
        ProjectConfig config;
        config.name = QStringLiteral("MixerFamilyProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(manager.createProject(config));
        bus_ = manager.getMasterBusItem();
        ASSERT_NE(bus_, nullptr);
        ASSERT_NE(bus_->comp<VMixNode>(), nullptr);
        auto& actions = ActionManager::instance();
        actions.clearAllForTesting();
        registerMixerActions(actions, nullptr, &manager, nullptr);
        server_.setProjectManager(&manager);
        UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
        ActionManager::instance().clearAllForTesting();
    }

    QTemporaryDir tempDir_;
    IPCServer server_{nullptr};
    ProjectItem* bus_ = nullptr;
};

TEST_F(IpcMixerCommandFamilyTest, PitchCommandsUseCentsAndSupportUndoRedo)
{
    using harkenplayableproperties::PlayablePropertyIdV1;
    auto* stack = UndoRedoManager::instance().undoStack();
    for (const double cents : {1200.0, -1200.0, 12.5, 0.0}) {
        SCOPED_TRACE(cents);
        const QJsonObject written = server_.dispatchCommand(
            QStringLiteral("set_bus_pitch"),
            {{QStringLiteral("bus_name"), bus_->name},
             {QStringLiteral("cents"), cents}});
        ASSERT_EQ(written.value(QStringLiteral("status")).toString(),
                  QStringLiteral("ok"));
        const QJsonObject writeResult = written.value(QStringLiteral("result")).toObject();
        EXPECT_EQ(writeResult.value(QStringLiteral("cents")).toDouble(), cents);
        EXPECT_FALSE(writeResult.contains(QStringLiteral("semitones")));

        const QJsonObject queried = server_.dispatchCommand(
            QStringLiteral("get_bus_pitch"), {{QStringLiteral("bus_name"), bus_->name}});
        ASSERT_EQ(queried.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        const QJsonObject readResult = queried.value(QStringLiteral("result")).toObject();
        EXPECT_EQ(readResult.value(QStringLiteral("cents")).toDouble(), cents);
        EXPECT_FALSE(readResult.contains(QStringLiteral("semitones")));
        EXPECT_FLOAT_EQ(bus_->comp<VMixNode>()->pitch, static_cast<float>(cents));
        EXPECT_TRUE(bus_->playablePropertyPresence.Has(PlayablePropertyIdV1::Pitch));
        ASSERT_TRUE(stack->canUndo());
        stack->undo();
        EXPECT_FLOAT_EQ(bus_->comp<VMixNode>()->pitch, 0.0f);
        EXPECT_FALSE(bus_->playablePropertyPresence.Has(PlayablePropertyIdV1::Pitch));
        ASSERT_TRUE(stack->canRedo());
        stack->redo();
        EXPECT_FLOAT_EQ(bus_->comp<VMixNode>()->pitch, static_cast<float>(cents));
        EXPECT_TRUE(bus_->playablePropertyPresence.Has(PlayablePropertyIdV1::Pitch));
        stack->undo();
        stack->clear();
    }
}

TEST_F(IpcMixerCommandFamilyTest, PitchRejectsInvalidCentsWithoutChangingState)
{
    const QJsonObject initial = server_.dispatchCommand(
        QStringLiteral("set_bus_pitch"),
        {{QStringLiteral("bus_name"), bus_->name}, {QStringLiteral("cents"), 1200.0}});
    ASSERT_EQ(initial.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    auto* stack = UndoRedoManager::instance().undoStack();
    stack->setClean();
    const int undoCount = stack->count();
    const int undoIndex = stack->index();
    const auto presence = bus_->playablePropertyPresence.bits;
    const QJsonObject invalidArgs[] = {
        {},
        {{QStringLiteral("semitones"), 12.0}},
        {{QStringLiteral("cents"), QJsonValue::Null}},
        {{QStringLiteral("cents"), QStringLiteral("1200")}},
        {{QStringLiteral("cents"), true}},
        {{QStringLiteral("cents"), QJsonArray{1200.0}}},
        {{QStringLiteral("cents"), QJsonObject{}}},
        {{QStringLiteral("cents"), 1.0e100}},
    };
    for (QJsonObject args : invalidArgs) {
        args.insert(QStringLiteral("bus_name"), bus_->name);
        SCOPED_TRACE(QJsonDocument(args).toJson(QJsonDocument::Compact).toStdString());
        const QJsonObject response = server_.dispatchCommand(QStringLiteral("set_bus_pitch"), args);
        EXPECT_EQ(response.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
        EXPECT_EQ(response.value(QStringLiteral("code")).toString(), QStringLiteral("INVALID_ARGS"));
        EXPECT_FLOAT_EQ(bus_->comp<VMixNode>()->pitch, 1200.0f);
        EXPECT_EQ(bus_->playablePropertyPresence.bits, presence);
        EXPECT_EQ(stack->count(), undoCount);
        EXPECT_EQ(stack->index(), undoIndex);
        EXPECT_TRUE(stack->isClean());
    }
}

TEST_F(IpcMixerCommandFamilyTest, UnsupportedMixedBusPropertiesCannotChangeAuthoringState)
{
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(manager.saveProject());
    auto* stack = UndoRedoManager::instance().undoStack();
    stack->setClean();
    const int undoCount = stack->count();
    const auto presence = bus_->playablePropertyPresence.bits;
    const float volume = bus_->comp<VMixNode>()->volume;
    const auto savedBytes = savedProjectBytes(tempDir_.path());
    ASSERT_FALSE(savedBytes.empty());
    const QJsonObject args{{QStringLiteral("bus_id"), static_cast<qint64>(bus_->objectID)},
                           {QStringLiteral("volume"), -6.0},
                           {QStringLiteral("listenerRelativeRouting"), true}};
    const QJsonObject direct = server_.dispatchCommand(QStringLiteral("set_bus_properties"), args);
    EXPECT_EQ(direct.value(QStringLiteral("code")).toString(),
              QStringLiteral("PROPERTY_NOT_SUPPORTED"));
    QJsonObject previewArgs = args;
    previewArgs.insert(QStringLiteral("preview"), true);
    const QJsonObject preview = server_.dispatchCommand(
        QStringLiteral("set_bus_properties"), previewArgs);
    EXPECT_EQ(preview.value(QStringLiteral("code")).toString(),
              QStringLiteral("PREVIEW_VALIDATION_FAILED"));
    EXPECT_TRUE(preview.value(QStringLiteral("message")).toString().contains(
        QStringLiteral("listenerRelativeRouting")));
    EXPECT_FLOAT_EQ(bus_->comp<VMixNode>()->volume, volume);
    EXPECT_FALSE(bus_->comp<VBus>()->listenerRelativeRouting);
    EXPECT_EQ(bus_->playablePropertyPresence.bits, presence);
    EXPECT_FALSE(bus_->isDirtyObject());
    EXPECT_EQ(stack->count(), undoCount);
    EXPECT_TRUE(stack->isClean());
    EXPECT_EQ(savedProjectBytes(tempDir_.path()), savedBytes);
}

TEST_F(IpcMixerCommandFamilyTest, LegalPanPitchAuthoringWithExplicitPreviewFalseSupportsUndo)
{
    const auto response = server_.dispatchCommand(QStringLiteral("set_bus_properties"),
        {{QStringLiteral("bus_id"), static_cast<qint64>(bus_->objectID)},
         {QStringLiteral("volume"), -6.0}, {QStringLiteral("pitch"), 300.0},
         {QStringLiteral("pan"), 0.25}, {QStringLiteral("preview"), false}});
    EXPECT_EQ(response.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    EXPECT_FLOAT_EQ(bus_->comp<VMixNode>()->volume, -6.0f);
    EXPECT_FLOAT_EQ(bus_->comp<VMixNode>()->pitch, 300.0f);
    EXPECT_FLOAT_EQ(bus_->comp<VBus>()->pan, 0.25f);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_FLOAT_EQ(bus_->comp<VMixNode>()->volume, 0.0f);
    EXPECT_FLOAT_EQ(bus_->comp<VMixNode>()->pitch, 0.0f);
    EXPECT_FLOAT_EQ(bus_->comp<VBus>()->pan, 0.0f);
}

TEST_F(IpcMixerCommandFamilyTest, SendFilterReportsUnsupportedForExistingBus)
{
    ASSERT_TRUE(ProjectManager::instance().saveProject());
    const auto savedBytes = savedProjectBytes(tempDir_.path());
    auto* stack = UndoRedoManager::instance().undoStack();
    stack->setClean();
    const int undoCount = stack->count();
    const auto* project = ProjectManager::instance().currentProject();
    const QString guid = bus_->guid.toString(QUuid::WithoutBraces);
    for (const auto& selector : {bus_->name, guid, QStringLiteral("guid:") + guid,
                                project->itemPath(bus_)}) {
        SCOPED_TRACE(selector.toStdString());
        const QJsonObject args{{QStringLiteral("src_bus"), selector},
                               {QStringLiteral("slot"), 0}};
        for (const auto& command : {QStringLiteral("set_send_filter"),
                                   QStringLiteral("get_send_filter")}) {
            const auto response = server_.dispatchCommand(command, args);
            EXPECT_EQ(response.value(QStringLiteral("code")).toString(),
                      QStringLiteral("PROPERTY_NOT_SUPPORTED"));
            EXPECT_TRUE(response.value(QStringLiteral("message")).toString().contains(guid));
        }
    }
    EXPECT_EQ(stack->count(), undoCount);
    EXPECT_TRUE(stack->isClean());
    EXPECT_FALSE(bus_->isDirtyObject());
    EXPECT_EQ(savedProjectBytes(tempDir_.path()), savedBytes);
    const QJsonObject missing = server_.dispatchCommand(
        QStringLiteral("get_send_filter"),
        {{QStringLiteral("src_bus"), QStringLiteral("NoSuchBus")},
         {QStringLiteral("slot"), 0}});
    EXPECT_EQ(missing.value(QStringLiteral("code")).toString(),
              QStringLiteral("BUS_NOT_FOUND"));
    const QJsonObject invalid = server_.dispatchCommand(
        QStringLiteral("set_send_filter"),
        {{QStringLiteral("src_bus"), bus_->name}, {QStringLiteral("slot"), 0},
         {QStringLiteral("lpf_hz"), -1.0}});
    EXPECT_EQ(invalid.value(QStringLiteral("code")).toString(), QStringLiteral("INVALID_ARGS"));
}

}
