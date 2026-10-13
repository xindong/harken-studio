#pragma once

#include "../Component.h"
#include "../Timeline/TimelineTypes.h"

namespace VNX {

class VTrack final : public IComponent
{
public:
    static constexpr ComponentType kType = ComponentType::Track;

    ComponentType getType() const override { return kType; }
    QJsonObject toJson() const override;
    bool fromJson(const QJsonObject& json) override;

    ClipLedger clips;
};

}
