#include "Core/SoundSourceSemantics.h"

#include <QJsonDocument>

#include <gtest/gtest.h>

using namespace VNX;

namespace {

SoundSourceDef source(uint32_t sourceId)
{
    SoundSourceDef result;
    result.entryId = QUuid::createUuid();
    result.sourceId = sourceId;
    return result;
}

}

TEST(SoundSourceSemanticsTest, FrameLedgerRoundTripsSampleExact)
{
    std::vector<SoundSourceDef> sources{source(1), source(2)};
    sources[0].trimStartFrame = 1;
    sources[0].trimEndFrame = 4294967297ull;
    sources[0].loopRegion = SoundLoopRegion{2147483649ull, 4294967296ull};
    sources[1].trimStartFrame = 4409;
    sources[1].trimEndFrame = 4410;

    const auto reloaded = SoundSourceSemantics::entriesFromJson(
        SoundSourceSemantics::entriesToJson(sources));
    ASSERT_TRUE(reloaded);
    ASSERT_EQ(reloaded->size(), sources.size());
    for (size_t index = 0; index < sources.size(); ++index) {
        EXPECT_EQ((*reloaded)[index].trimStartFrame, sources[index].trimStartFrame);
        ASSERT_TRUE((*reloaded)[index].trimEndFrame);
        EXPECT_EQ(*(*reloaded)[index].trimEndFrame, *sources[index].trimEndFrame);
        EXPECT_EQ((*reloaded)[index].loopRegion, sources[index].loopRegion);
    }
    EXPECT_EQ(*reloaded, sources);
}

TEST(SoundSourceSemanticsTest, FrameLedgerSurvivesTheSerializedDocument)
{
    std::vector<SoundSourceDef> sources{source(1)};
    sources[0].trimStartFrame = 123456789;
    sources[0].trimEndFrame = 987654321;
    sources[0].loopRegion = SoundLoopRegion{234567890, 876543210};

    const QJsonDocument document(QJsonObject{
        {QStringLiteral("sources"), SoundSourceSemantics::entriesToJson(sources)}});
    const QJsonArray parsed = QJsonDocument::fromJson(document.toJson(QJsonDocument::Compact))
                                  .object()
                                  .value(QStringLiteral("sources"))
                                  .toArray();
    const auto reloaded = SoundSourceSemantics::entriesFromJson(parsed);
    ASSERT_TRUE(reloaded);
    EXPECT_EQ(*reloaded, sources);
}
