#include "Core/AssetPipeline/AssetTypes.h"
#include "Core/Components/VSound.h"
#include "Core/ProjectTypes.h"
#include "Core/SoundSourceEditing.h"
#include "Core/SoundSourceSemantics.h"

#include <gtest/gtest.h>
#include "Support/ProjectPlatformTestUtils.h"

using namespace VNX;

namespace {

SoundSourceDef source(uint32_t sourceId)
{
    SoundSourceDef result;
    result.entryId = QUuid::createUuid();
    result.sourceId = sourceId;
    return result;
}

ProjectItem soundItem()
{
    ProjectItem item(QStringLiteral("Sound"), ProjectItem::Type::Sound);
    item.attachDefaultComponents();
    return item;
}

}

TEST(SoundSourceEditingTest, EmptySourcesIgnoreStaleActiveProjection)
{
    ProjectItem item = soundItem();
    item.sourceID = 42;
    item.sourcePath = QStringLiteral("legacy.wav");
    auto* sound = item.comp<VSound>();
    ASSERT_NE(sound, nullptr);

    EXPECT_EQ(SoundSourceEditing::activeSourceId(item, HarkenTest::PlatformIds::Windows, QString()), 0u);
    EXPECT_EQ(SoundSourceEditing::activeSourceId(item, HarkenTest::PlatformIds::Android, QStringLiteral("English")), 0u);
    EXPECT_EQ(SoundSourceEditing::activeSource(item, HarkenTest::PlatformIds::Windows, QString()), nullptr);
    EXPECT_TRUE(SoundSourceEditing::syncActiveProjection(item, HarkenTest::PlatformIds::Windows, QString()));
    EXPECT_EQ(item.sourceID, 0u);
    EXPECT_TRUE(item.sourcePath.isEmpty());
}

TEST(SoundSourceEditingTest, AuthoredEmptyMatrixDoesNotUseOrKeepTheLegacyProjection)
{
    ProjectItem item = soundItem();
    auto* sound = item.comp<VSound>();
    ASSERT_NE(sound, nullptr);
    sound->sources.plain.entries = {source(11), source(22)};
    item.sourceID = 22;
    item.sourcePath = QStringLiteral("previously-active.wav");

    EXPECT_EQ(SoundSourceEditing::activeSourceId(item, HarkenTest::PlatformIds::Windows, QString()), 0u);
    EXPECT_TRUE(SoundSourceEditing::activeRelativePath(item, HarkenTest::PlatformIds::Windows, QString()).isEmpty());
    EXPECT_TRUE(SoundSourceEditing::syncActiveProjection(item, HarkenTest::PlatformIds::Windows, QString()));
    EXPECT_EQ(item.sourceID, 0u);
    EXPECT_TRUE(item.sourcePath.isEmpty());
}

TEST(SoundSourceEditingTest, LocalizedSoundCannotUseALegacySourceWithoutALanguageSelection)
{
    ProjectItem item = soundItem();
    auto* sound = item.comp<VSound>();
    ASSERT_NE(sound, nullptr);
    sound->distinguishLanguage = true;
    item.sourceID = 42;
    item.sourcePath = QStringLiteral("legacy.wav");

    EXPECT_EQ(SoundSourceEditing::activeSourceId(
        item, HarkenTest::PlatformIds::Windows, QStringLiteral("English")), 0u);
    EXPECT_EQ(SoundSourceEditing::activeSourceId(
        item, HarkenTest::PlatformIds::Windows, QString()), 0u);
    EXPECT_TRUE(SoundSourceEditing::activeRelativePath(
        item, HarkenTest::PlatformIds::Windows, QStringLiteral("English")).isEmpty());
}

TEST(SoundSourceEditingTest, MatrixResolvesByPlatform)
{
    ProjectItem item = soundItem();
    auto* sound = item.comp<VSound>();
    ASSERT_NE(sound, nullptr);
    sound->sources.plain.entries = {source(11), source(22)};
    sound->sources.plain.platforms[HarkenTest::platformIdString("Windows")].active = sound->sources.plain.entries[0].entryId;
    sound->sources.plain.platforms[HarkenTest::platformIdString("Android")].active = sound->sources.plain.entries[1].entryId;

    EXPECT_EQ(SoundSourceEditing::activeSourceId(item, HarkenTest::PlatformIds::Windows, QString()), 11u);
    EXPECT_EQ(SoundSourceEditing::activeSourceId(item, HarkenTest::PlatformIds::Android, QString()), 22u);
}

TEST(SoundSourceEditingTest, MatrixResolvesByLanguage)
{
    ProjectItem item = soundItem();
    auto* sound = item.comp<VSound>();
    ASSERT_NE(sound, nullptr);
    sound->distinguishLanguage = true;
    auto& english = sound->sources.localized["English"];
    english.entries = {source(11)};
    english.active = english.entries[0].entryId;
    auto& japanese = sound->sources.localized["Japanese"];
    japanese.entries = {source(33)};
    japanese.active = japanese.entries[0].entryId;

    EXPECT_EQ(SoundSourceEditing::activeSourceId(item, HarkenTest::PlatformIds::Windows, QStringLiteral("English")), 11u);
    EXPECT_EQ(SoundSourceEditing::activeSourceId(item, HarkenTest::PlatformIds::Windows, QStringLiteral("Japanese")), 33u);
}

TEST(SoundSourceEditingTest, UniversalSelectionIsUsedWhenPlatformRowIsMissing)
{
    ProjectItem item = soundItem();
    auto* sound = item.comp<VSound>();
    ASSERT_NE(sound, nullptr);
    sound->sources.plain.entries = {source(11)};
    sound->sources.plain.active = sound->sources.plain.entries[0].entryId;

    EXPECT_EQ(SoundSourceEditing::activeSourceId(item, HarkenTest::PlatformIds::Windows, QString()), 11u);
    EXPECT_EQ(SoundSourceEditing::activeSourceId(item, HarkenTest::PlatformIds::Android, QString()), 11u);
}

TEST(SoundSourceEditingTest, MissingKeyWithoutUniversalReturnsZeroAndDoesNotUseFirstRow)
{
    ProjectItem item = soundItem();
    auto* sound = item.comp<VSound>();
    ASSERT_NE(sound, nullptr);
    sound->sources.plain.entries = {source(11), source(22)};
    sound->sources.plain.platforms[HarkenTest::platformIdString("Windows")].active = sound->sources.plain.entries[0].entryId;

    EXPECT_EQ(SoundSourceEditing::activeSourceId(item, HarkenTest::PlatformIds::Android, QString()), 0u);
    EXPECT_EQ(SoundSourceEditing::activeSource(item, HarkenTest::PlatformIds::Android, QString()), nullptr);
}

TEST(SoundSourceEditingTest, LanguageMustMatchExactly)
{
    ProjectItem item = soundItem();
    auto* sound = item.comp<VSound>();
    ASSERT_NE(sound, nullptr);
    sound->distinguishLanguage = true;
    auto& english = sound->sources.localized["English"];
    english.entries = {source(33)};
    english.active = english.entries[0].entryId;

    EXPECT_EQ(SoundSourceEditing::activeSourceId(item, HarkenTest::PlatformIds::Windows, QString()), 0u);
    EXPECT_EQ(SoundSourceEditing::activeSourceId(item, HarkenTest::PlatformIds::Windows, QStringLiteral("Japanese")), 0u);
    EXPECT_EQ(SoundSourceEditing::activeSourceId(item, HarkenTest::PlatformIds::Windows, QStringLiteral("English")), 33u);
}

TEST(SoundSourceEditingTest, PreviewLanguageIgnoresBarWhenSwitchIsOff)
{
    ProjectItem item = soundItem();
    auto* sound = item.comp<VSound>();
    ASSERT_NE(sound, nullptr);
    sound->sources.plain.entries = {source(11)};
    sound->sources.plain.platforms[HarkenTest::platformIdString("Windows")].active = sound->sources.plain.entries[0].entryId;

    EXPECT_TRUE(SoundSourceEditing::previewLanguage(item, QStringLiteral("English")).isEmpty());
    EXPECT_EQ(SoundSourceEditing::activeSourceId(
                  item, HarkenTest::PlatformIds::Windows,
                  SoundSourceEditing::previewLanguage(item, QStringLiteral("English"))),
              11u);

    sound->distinguishLanguage = true;
    EXPECT_EQ(SoundSourceEditing::previewLanguage(item, QStringLiteral("English")),
              QStringLiteral("English"));
    EXPECT_EQ(SoundSourceEditing::activeSourceId(
                  item, HarkenTest::PlatformIds::Windows,
                  SoundSourceEditing::previewLanguage(item, QStringLiteral("English"))),
              0u);
}

TEST(SoundSourceEditingTest, DeepSemanticsScanAnyRowNotOnlyActive)
{
    VSound sound;
    sound.sources.plain.entries = {source(22)};
    sound.sources.plain.entries[0].trimStartFrame = 48000;
    const AssetInfo info = [] {
        AssetInfo result;
        result.sourceID = 22;
        result.metadata.sampleRate = 48000;
        result.metadata.totalSamples = 480000;
        return result;
    }();
    const auto resolve = [&info](uint32_t sourceId) -> const AssetInfo* {
        return sourceId == info.sourceID ? &info : nullptr;
    };
    EXPECT_TRUE(SoundSourceSemantics::hasDeepSourceSemantics(sound, resolve));
}
