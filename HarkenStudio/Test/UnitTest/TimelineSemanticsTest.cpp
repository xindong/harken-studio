#include "Core/Components/VTrack.h"
#include "Core/ProjectTypes.h"
#include "Core/Timeline/TimelineSemantics.h"

#include <gtest/gtest.h>

#include <QHash>

#include <memory>

using namespace VNX;

namespace {

std::shared_ptr<ProjectItem> makeItem(ProjectItem::Type type, const QString& name)
{
    auto item = std::make_shared<ProjectItem>(name, type);
    item->guid = QUuid::createUuid();
    item->attachDefaultComponents();
    return item;
}

Clip clipTo(const ProjectItem& target, int64_t start, int64_t duration)
{
    return {QUuid::createUuid(), target.guid, start, duration};
}

ClipLedger makeLedger(std::initializer_list<Clip> clips)
{
    ClipLedger ledger;
    for (Clip clip : clips) {
        EXPECT_TRUE(ledger.mintOccurrenceOrdinal(&clip));
        ledger.items.push_back(std::move(clip));
    }
    return ledger;
}

TimelineItemResolver resolverFor(const std::vector<std::shared_ptr<ProjectItem>>& items)
{
    auto index = std::make_shared<QHash<QUuid, const ProjectItem*>>();
    for (const auto& item : items) index->insert(item->guid, item.get());
    return [index](const QUuid& guid) { return index->value(guid, nullptr); };
}

}

TEST(TimelineSemantics, SharedNestedSegmentInDagDoesNotLookLikeCycle)
{
    auto sound = makeItem(ProjectItem::Type::Sound, QStringLiteral("Sound"));
    auto shared = makeItem(ProjectItem::Type::Segment, QStringLiteral("Shared"));
    auto sharedTrack = makeItem(ProjectItem::Type::Track, QStringLiteral("SharedTrack"));
    sharedTrack->comp<VTrack>()->clips = makeLedger({clipTo(*sound, 0, 1)});
    shared->children = {sharedTrack};

    auto root = makeItem(ProjectItem::Type::Segment, QStringLiteral("Root"));
    auto rootTrack = makeItem(ProjectItem::Type::Track, QStringLiteral("RootTrack"));
    rootTrack->comp<VTrack>()->clips = makeLedger({
        clipTo(*shared, 0, 1), clipTo(*shared, 2, 1)
    });
    root->children = {rootTrack};

    const TimelineSemanticsResult result = TimelineSemantics::segmentNominalSpan(
        *root, resolverFor({sound, shared, sharedTrack, root, rootTrack}));
    EXPECT_TRUE(result) << result.validation.message.toStdString();
}

TEST(TimelineSemantics, DeepSegmentReferenceGraphIsResolvedIteratively)
{
    auto sound = makeItem(ProjectItem::Type::Sound, QStringLiteral("Sound"));
    std::vector<std::shared_ptr<ProjectItem>> items = {sound};
    constexpr int kDepth = 2048;
    for (int i = 0; i < kDepth; ++i) {
        auto segment = makeItem(ProjectItem::Type::Segment, QStringLiteral("Segment"));
        auto track = makeItem(ProjectItem::Type::Track, QStringLiteral("Track"));
        const ProjectItem& target = i == 0 ? *sound : *items[items.size() - 2];
        track->comp<VTrack>()->clips = makeLedger({clipTo(target, 0, 1)});
        segment->children = {track};
        items.push_back(segment);
        items.push_back(track);
    }
    const TimelineSemanticsResult result = TimelineSemantics::segmentNominalSpan(
        *items[items.size() - 2], resolverFor(items));
    EXPECT_TRUE(result) << result.validation.message.toStdString();
}
