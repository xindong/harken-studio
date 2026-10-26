#include "ActionStepEditing.h"

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonObject>

#include <limits>

namespace Edit = VNX::ActionStepEditing;

TEST(ActionStepEditing, MaxObjectIdIsPreserved)
{
    QJsonObject o{{QStringLiteral("type"), QStringLiteral("Play")}};
    o[QStringLiteral("targetObjectID")] =
        static_cast<qint64>(std::numeric_limits<uint32_t>::max());
    const auto steps = Edit::stepsFromJson(QJsonArray{o});
    ASSERT_EQ(steps.size(), 1u);
    EXPECT_EQ(steps[0].targetObjectID, std::numeric_limits<uint32_t>::max());
}
