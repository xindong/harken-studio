#include "Core/Timeline/TimelineTypes.h"

#include <gtest/gtest.h>

using namespace VNX;

namespace {

Clip makeClip(const QUuid& target, int64_t start = 0, int64_t duration = 1)
{
    return {QUuid::createUuid(), target, start, duration};
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

}

TEST(TimelineLedger, ClipJsonRoundtripPreservesSignedIntegersAndGuids)
{
    ClipLedger ledger = makeLedger({
        makeClip(QUuid::createUuid(), -9007199254740993LL, 17)});
    const QJsonObject json = ledger.toJsonObject();
    QString error;
    const auto parsed = ClipLedger::fromJson(json, &error);
    ASSERT_TRUE(parsed.has_value()) << error.toStdString();
    EXPECT_EQ(*parsed, ledger);
}
