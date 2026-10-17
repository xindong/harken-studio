#include "ActionManager.h"
#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/AssetTypes.h"
#include "Components/VSound.h"
#include "LanguageContextService.h"
#include "ProjectManager.h"
#include "SoundSourceEditing.h"
#include "SoundSourceSemantics.h"
#include "UndoCommands.h"

#include <gtest/gtest.h>

#include <algorithm>

#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUuid>
#include "Support/ProjectPlatformTestUtils.h"

using namespace VNX;

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

ActionResult setProperty(ProjectItem* item, const QString& property, const QJsonValue& value)
{
    return ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        {{QStringLiteral("authoring_handle"), static_cast<qint64>(item->objectID)},
         {QStringLiteral("property"), property},
         {QStringLiteral("value"), value}},
        ActionSource::GUI);
}

class SoundDistinguishLanguageTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& manager = ProjectManager::instance();
        if (manager.isProjectOpen()) manager.closeProject();
        ActionManager::instance().clearAllForTesting();
        registerProjectActions(ActionManager::instance(), &manager, nullptr, {});
        ASSERT_TRUE(temp_.isValid());
        ProjectConfig config;
        config.name = QStringLiteral("DistinguishLanguageProject");
        config.location = temp_.path();
        config.platforms = HarkenTest::platforms({QStringLiteral("Windows"), QStringLiteral("macOS")});
        ASSERT_TRUE(manager.createProject(config));
        layer_ = manager.createItem(nullptr, QStringLiteral("Layer"), ProjectItem::Type::Layer);
        sound_ = manager.createItem(layer_, QStringLiteral("Sound"), ProjectItem::Type::Sound);
        ASSERT_NE(sound_, nullptr);
        UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        if (ProjectManager::instance().isProjectOpen())
            ProjectManager::instance().closeProject();
        ActionManager::instance().clearAllForTesting();
    }

    SoundSourceDef makeSource(uint32_t sourceId)
    {
        AssetInfo asset;
        asset.sourceID = sourceId;
        asset.relativePath = QStringLiteral("%1.wav").arg(sourceId);
        asset.absolutePath = temp_.path() + QLatin1Char('/') + asset.relativePath;
        asset.metadata.sampleRate = 48000;
        asset.metadata.totalSamples = 48000;
        asset.metadata.durationSec = 1.0;
        EXPECT_TRUE(AssetRegistry::instance().registerAsset(asset));
        SoundSourceDef source;
        source.entryId = QUuid::createUuid();
        source.sourceId = sourceId;
        return source;
    }

    QTemporaryDir temp_;
    ProjectItem* layer_ = nullptr;
    ProjectItem* sound_ = nullptr;
};

}

TEST(SoundDistinguishLanguageJson, DefaultOmitsTheKey)
{
    VSound sound;
    EXPECT_FALSE(sound.distinguishLanguage);
    EXPECT_FALSE(sound.toJson().contains(QStringLiteral("distinguishLanguage")));
}

TEST(SoundDistinguishLanguageJson, TrueRoundTrips)
{
    VSound sound;
    sound.distinguishLanguage = true;
    const QJsonObject json = sound.toJson();
    EXPECT_TRUE(json.value(QStringLiteral("distinguishLanguage")).toBool());
    VSound reloaded;
    ASSERT_TRUE(reloaded.fromJson(json));
    EXPECT_TRUE(reloaded.distinguishLanguage);
}

TEST(SoundDistinguishLanguageJson, MissingKeyIsFalse)
{
    VSound reloaded;
    ASSERT_TRUE(reloaded.fromJson(QJsonObject{}));
    EXPECT_FALSE(reloaded.distinguishLanguage);
}

TEST(SoundDistinguishLanguageJson, NonBoolIsRejected)
{
    VSound number;
    EXPECT_FALSE(number.fromJson(QJsonObject{{QStringLiteral("distinguishLanguage"), 1}}));
    VSound text;
    EXPECT_FALSE(text.fromJson(QJsonObject{
        {QStringLiteral("distinguishLanguage"), QStringLiteral("yes")}}));
}

TEST_F(SoundDistinguishLanguageTest, FirstLanguageModeStartsEmptyAndPreservesPlainSources)
{
    auto* sound = sound_->comp<VSound>();
    SoundSources sources;
    sources.plain.entries = {makeSource(8101), makeSource(8102)};
    sources.plain.active = sources.plain.entries[0].entryId;
    sources.plain.platforms[HarkenTest::platformIdString("macOS")].active = sources.plain.entries[1].entryId;
    ASSERT_TRUE(setProperty(sound_, "sources", SoundSourceSemantics::toJson(sources)).success);
    const auto before = sound->sources;
    ASSERT_TRUE(setProperty(sound_, "distinguishLanguage", true).success);
    EXPECT_TRUE(sound->sources.localized.isEmpty());
    EXPECT_EQ(sound->sources, before);
    EXPECT_EQ(SoundSourceEditing::activeSourceId(*sound_, HarkenTest::PlatformIds::Windows, "English"), 0u);
    EXPECT_EQ(sound_->sourceID, 0u);
    ASSERT_TRUE(setProperty(sound_, "distinguishLanguage", false).success);
    EXPECT_EQ(sound->sources, before);
    EXPECT_EQ(SoundSourceEditing::activeSourceId(*sound_, HarkenTest::PlatformIds::macOS, {}), 8102u);
}

TEST_F(SoundDistinguishLanguageTest, BothModesSurviveRepeatedSwitchesSerializationAndUndo)
{
    ProjectManager::instance().setProjectLanguagesInternal({"English", "Japanese"});
    SoundSources sources;
    sources.plain.entries = {makeSource(8101), makeSource(8102)};
    sources.plain.entries[0].trimStartFrame = 19;
    sources.plain.active = sources.plain.entries[0].entryId;
    sources.plain.platforms[HarkenTest::platformIdString("macOS")].active = sources.plain.entries[1].entryId;
    auto& english = sources.localized["English"];
    english.entries = {makeSource(8103), makeSource(8104)};
    english.active = english.entries[0].entryId;
    english.platforms[HarkenTest::platformIdString("Windows")].active = english.entries[1].entryId;
    auto& japanese = sources.localized["Japanese"];
    japanese.entries = {makeSource(8105)};
    japanese.active = japanese.entries[0].entryId;
    ASSERT_TRUE(setProperty(sound_, "sources", SoundSourceSemantics::toJson(sources)).success);
    ASSERT_TRUE(setProperty(sound_, "scheduleOffsetSec", 0.125).success);
    const auto* sound = sound_->comp<VSound>();
    const auto before = sound->sources;
    auto* stack = UndoRedoManager::instance().undoStack();
    stack->clear();
    for (bool enabled : {true, false, true, false}) {
        const int commands = stack->count();
        ASSERT_TRUE(setProperty(sound_, "distinguishLanguage", enabled).success);
        EXPECT_EQ(stack->count(), commands + 1);
        EXPECT_EQ(sound->sources, before);
        VSound restored;
        ASSERT_TRUE(restored.fromJson(sound->toJson()));
        EXPECT_EQ(restored.sources, before);
        EXPECT_EQ(restored.distinguishLanguage, enabled);
        stack->undo();
        EXPECT_EQ(sound->distinguishLanguage, !enabled);
        EXPECT_EQ(sound->sources, before);
        stack->redo();
        EXPECT_EQ(sound->distinguishLanguage, enabled);
        EXPECT_EQ(sound->sources, before);
        EXPECT_DOUBLE_EQ(sound->scheduleOffsetSec, 0.125);
    }
}

TEST_F(SoundDistinguishLanguageTest, EditingOneBankPreservesTheOtherAndUndoRestoresBoth)
{
    ProjectManager::instance().setProjectLanguagesInternal({"English"});
    SoundSources sources;
    sources.plain.entries = {makeSource(8101)};
    sources.plain.active = sources.plain.entries[0].entryId;
    sources.localized["English"].entries = {makeSource(8102)};
    sources.localized["English"].active = sources.localized["English"].entries[0].entryId;
    ASSERT_TRUE(setProperty(sound_, "sources", SoundSourceSemantics::toJson(sources)).success);
    const auto* sound = sound_->comp<VSound>();
    const auto before = sound->sources;
    auto changed = before;
    changed.plain.entries.clear();
    changed.plain.active = QUuid{};
    ASSERT_TRUE(setProperty(sound_, "sources", SoundSourceSemantics::toJson(changed)).success);
    EXPECT_EQ(sound->sources.localized, before.localized);
    EXPECT_TRUE(sound->sources.plain.entries.empty());
    UndoRedoManager::instance().undo();
    EXPECT_EQ(sound->sources, before);
    UndoRedoManager::instance().redo();
    EXPECT_EQ(sound->sources, changed);
}

TEST_F(SoundDistinguishLanguageTest, UnchangedSwitchDoesNotCreateUndo)
{
    const auto* sound = sound_->comp<VSound>();
    const auto before = sound->sources;
    EXPECT_TRUE(setProperty(sound_, "distinguishLanguage", false).success);
    EXPECT_EQ(sound->sources, before);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
    EXPECT_FALSE(setProperty(sound_, "distinguishLanguage", 1).success);
}

TEST_F(SoundDistinguishLanguageTest, RejectsUnknownLanguagesPlatformsAndCrossBankActiveAtomically)
{
    ProjectManager::instance().setProjectLanguagesInternal({"English"});
    SoundSources sources;
    sources.plain.entries = {makeSource(8101)};
    sources.plain.active = sources.plain.entries[0].entryId;
    ASSERT_TRUE(setProperty(sound_, "sources", SoundSourceSemantics::toJson(sources)).success);
    const auto* sound = sound_->comp<VSound>();
    const auto before = sound->sources;
    UndoRedoManager::instance().clear();
    for (const QString& language : {QString(), QStringLiteral("english"), QStringLiteral("French")}) {
        auto invalid = before;
        invalid.localized[language].entries = {makeSource(8200)};
        EXPECT_FALSE(setProperty(sound_, "sources", SoundSourceSemantics::toJson(invalid)).success);
        AssetRegistry::instance().unregisterAsset(8200);
    }
    auto invalid = before;
    invalid.localized["English"].active = before.plain.active;
    EXPECT_FALSE(setProperty(sound_, "sources", SoundSourceSemantics::toJson(invalid)).success);
    invalid = before;
    invalid.plain.platforms[QUuid::createUuid().toString(QUuid::WithoutBraces)].active = before.plain.active;
    EXPECT_FALSE(setProperty(sound_, "sources", SoundSourceSemantics::toJson(invalid)).success);
    EXPECT_EQ(sound->sources, before);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(SoundDistinguishLanguageTest, DormantRemovedLanguageCanBeRetainedAndRepaired)
{
    ProjectManager::instance().setProjectLanguagesInternal({"English", "Japanese"});
    SoundSources sources;
    sources.plain.entries = {makeSource(8101)};
    sources.localized["Japanese"].entries = {makeSource(8102)};
    sources.localized["Japanese"].active = sources.localized["Japanese"].entries[0].entryId;
    ASSERT_TRUE(setProperty(sound_, "sources", SoundSourceSemantics::toJson(sources)).success);
    ProjectManager::instance().setProjectLanguagesInternal({"English"});
    auto changed = sound_->comp<VSound>()->sources;
    changed.plain.active = changed.plain.entries[0].entryId;
    ASSERT_TRUE(setProperty(sound_, "sources", SoundSourceSemantics::toJson(changed)).success);
    const auto dormant = changed.localized;
    ASSERT_TRUE(setProperty(sound_, "distinguishLanguage", true).success);
    EXPECT_EQ(sound_->comp<VSound>()->sources.localized, dormant);
    SoundSourceSemantics::moveToLanguage(changed, {changed.localized["Japanese"].active}, "English");
    ASSERT_TRUE(setProperty(sound_, "sources", SoundSourceSemantics::toJson(changed)).success);
    EXPECT_FALSE(sound_->comp<VSound>()->sources.localized.contains("Japanese"));
    EXPECT_EQ(sound_->comp<VSound>()->sources.localized["English"].entries[0].sourceId, 8102u);
}
TEST_F(SoundDistinguishLanguageTest, PlainEditDoesNotNormalizeDormantRangesAfterTheAssetChanges)
{
    ProjectManager::instance().setProjectLanguagesInternal({"English"});
    SoundSources sources;
    sources.plain.entries = {makeSource(8101)};
    auto row = makeSource(8102);
    auto* asset = AssetRegistry::instance().getAsset(row.sourceId);
    asset->metadata.loopInfo.enabled = true;
    asset->metadata.loopInfo.loopStart = 100;
    asset->metadata.loopInfo.loopEnd = 199;
    row.loopRegion = SoundLoopRegion{100, 200};
    row.loopProvenance = SoundLoopProvenance::File;
    sources.localized["English"].entries = {row};
    ASSERT_TRUE(setProperty(sound_, "sources", SoundSourceSemantics::toJson(sources)).success);
    const auto* sound = sound_->comp<VSound>();
    const auto dormant = sound->sources.localized;
    asset->metadata.loopInfo.loopEnd = 299;
    auto changed = sound->sources;
    changed.plain.entries[0].trimStartFrame = 42;
    ASSERT_TRUE(setProperty(sound_, "sources", SoundSourceSemantics::toJson(changed)).success);
    EXPECT_EQ(sound->sources.localized, dormant);
}
