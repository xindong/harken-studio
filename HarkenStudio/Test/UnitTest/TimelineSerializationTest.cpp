#include "Core/Components/VTrack.h"
#include "Core/ProjectSerializer.h"
#include "Core/ProjectTypes.h"

#include <gtest/gtest.h>

using namespace VNX;

namespace {

ClipLedger makeClipLedger(std::initializer_list<Clip> clips)
{
    ClipLedger ledger;
    for (Clip clip : clips) {
        EXPECT_TRUE(ledger.mintOccurrenceOrdinal(&clip));
        ledger.items.push_back(std::move(clip));
    }
    return ledger;
}

}

TEST(TimelineSerialization, PreservesIntegersBeyondDoublePrecision)
{
    ProjectSerializer serializer;
    ProjectItem track(QStringLiteral("Track"), ProjectItem::Type::Track);
    track.guid = QUuid::createUuid();
    track.attachDefaultComponents();
    const QUuid target = QUuid::createUuid();
    track.comp<VTrack>()->clips = makeClipLedger({
        {QUuid::createUuid(), target, -9007199254740993LL, 9007199254740994LL}
    });

    const QJsonObject saved = serializer.projectItemToJson(track);
    ProjectItem loaded;
    ASSERT_TRUE(serializer.projectItemFromJson(saved, loaded));
    ASSERT_NE(loaded.comp<VTrack>(), nullptr);
    ASSERT_EQ(loaded.comp<VTrack>()->clips.items.size(), 1u);
    EXPECT_EQ(loaded.comp<VTrack>()->clips.items.front().start,
              track.comp<VTrack>()->clips.items.front().start);
    EXPECT_EQ(loaded.comp<VTrack>()->clips.items.front().duration,
              track.comp<VTrack>()->clips.items.front().duration);
}
