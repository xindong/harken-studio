#pragma once

#include "../Component.h"
#include "../Timeline/TimelineTypes.h"

namespace VNX {

class VPiece final : public IComponent
{
public:
    static constexpr ComponentType kType = ComponentType::Piece;

    ComponentType getType() const override { return kType; }
    QJsonObject toJson() const override;
    bool fromJson(const QJsonObject& json) override;

    PieceTimeSpec pieceTime;
    MarkerLedger markers;
};

}
