#pragma once

#include "../Component.h"
#include "../Timeline/TimelineTypes.h"

namespace VNX {

class VSegment final : public IComponent
{
public:
    static constexpr ComponentType kType = ComponentType::Segment;
    static constexpr int64_t kDefaultDurationSeconds = 10;

    ComponentType getType() const override { return kType; }
    QJsonObject toJson() const override;
    bool fromJson(const QJsonObject& json) override;

    SegmentTimeSpec segmentTime;
    MarkerLedger markers;
};

}
