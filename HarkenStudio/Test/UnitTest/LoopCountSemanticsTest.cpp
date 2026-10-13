#include "Core/ActionManager.h"
#include "Core/ItemTypeCatalog.h"
#include "Core/Components/VAudio.h"
#include "Core/PlayablePropertyAuthoringV1.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectSerializer.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QJsonObject>
#include <QTemporaryDir>

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

}

TEST(LoopCountSemanticsTest, LegacyAliasIsIgnored)
{
    VAudio audio;
    EXPECT_TRUE(audio.fromJson(QJsonObject{{QStringLiteral("loopEnabled"), true}}));
    EXPECT_EQ(audio.loopCount, 1u);
}

TEST(LoopCountSemanticsTest, CanonicalValueAlwaysSerializes)
{
    VAudio audio;
    EXPECT_TRUE(audio.fromJson(QJsonObject{
        {QStringLiteral("loopCount"), 7},
        {QStringLiteral("loopEnabled"), true}}));
    EXPECT_EQ(audio.loopCount, 7u);
    EXPECT_EQ(audio.toJson().value(QStringLiteral("loopCount")).toInt(), 7);
    EXPECT_FALSE(audio.toJson().contains(QStringLiteral("loopEnabled")));

    audio.loopCount = 1;
    EXPECT_EQ(audio.toJson().value(QStringLiteral("loopCount")).toInt(), 1);
}

TEST(LoopCountSemanticsTest, SoundContainerTrackPieceCarryLoopCount)
{
    for (const ProjectItem::Type type : {ProjectItem::Type::Sound,
                                         ProjectItem::Type::Container,
                                         ProjectItem::Type::Track,
                                         ProjectItem::Type::Piece}) {
        ProjectItem item(QStringLiteral("Content"), type);
        item.guid = QUuid::createUuid();
        item.attachDefaultComponents();
        ASSERT_NE(item.comp<VAudio>(), nullptr);
        item.comp<VAudio>()->loopCount = 9;
        item.playablePropertyPresence.bits = UINT64_C(0x8);
        ProjectSerializer serializer;
        ProjectItem reloaded;
        ASSERT_TRUE(serializer.projectItemFromJson(serializer.projectItemToJson(item), reloaded));
        ASSERT_NE(reloaded.comp<VAudio>(), nullptr);
        EXPECT_EQ(reloaded.comp<VAudio>()->loopCount, 9u);
    }
}

TEST(LoopCountSemanticsTest, AbsentEnvelopeSlotRestoresSinglePassNotInfinite)
{
    for (const ProjectItem::Type type : {ProjectItem::Type::Sound,
                                         ProjectItem::Type::Container,
                                         ProjectItem::Type::Track,
                                         ProjectItem::Type::Piece}) {
        ProjectItem item(QStringLiteral("Content"), type);
        item.guid = QUuid::createUuid();
        item.attachDefaultComponents();
        ASSERT_NE(item.comp<VAudio>(), nullptr);
        item.comp<VAudio>()->loopCount = 5;
        item.playablePropertyPresence.bits = 0;
        ProjectSerializer serializer;
        ProjectItem reloaded;
        ASSERT_TRUE(serializer.projectItemFromJson(serializer.projectItemToJson(item), reloaded));
        ASSERT_NE(reloaded.comp<VAudio>(), nullptr);
        EXPECT_EQ(reloaded.comp<VAudio>()->loopCount, 1u);
    }
}

TEST(LoopCountSemanticsTest, ClearingTheOverrideRestoresSinglePass)
{
    ProjectItem sound(QStringLiteral("Sound"), ProjectItem::Type::Sound);
    sound.guid = QUuid::createUuid();
    sound.attachDefaultComponents();
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(sound);
    ASSERT_TRUE(locator.has_value());
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        sound, *locator, 4, QJsonValue(6), std::nullopt));
    EXPECT_EQ(sound.comp<VAudio>()->loopCount, 6u);
    ASSERT_TRUE(PlayablePropertyAuthoringV1::clearOverride(sound, *locator, 4, std::nullopt));
    EXPECT_EQ(sound.comp<VAudio>()->loopCount, 1u);
}

TEST(LoopCountSemanticsTest, NonContentTypeHasNoAudioComponent)
{
    for (const ProjectItem::Type type : {ProjectItem::Type::None,
                                         ProjectItem::Type::Layer}) {
        ProjectItem item(QStringLiteral("Item"), type);
        item.attachDefaultComponents();
        EXPECT_EQ(item.comp<VAudio>(), nullptr)
            << VNX::ItemTypeCatalog::canonicalName(type).toStdString();
    }
}
