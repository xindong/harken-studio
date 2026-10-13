#pragma once

#include "CurveSelection.h"
#include "PlacementContext.h"

#include <optional>

namespace VNX {

struct ExternalEditorSelection
{
    enum class Kind { None, Attenuation, Binding, Source };

    Kind kind = Kind::None;
    QUuid ownerGuid;
    CurveSelection curve;
    BindingAddress bindingAddress;
    QUuid sourceEntryId;
    bool attenuationSlot = false;
    std::optional<PlacementContext> placement;

    static ExternalEditorSelection fromCurve(const CurveSelection& curve,
                                             const QUuid& owner = {})
    {
        ExternalEditorSelection selection;
        selection.kind = curve.hasBinding() ? Kind::Binding
            : curve.hasAttenuation() ? Kind::Attenuation : Kind::None;
        selection.ownerGuid = !owner.isNull() ? owner
            : curve.hasBinding() ? curve.hostGuid : curve.attenuationGuid;
        selection.curve = curve;
        selection.attenuationSlot = curve.hasAttenuation() && !owner.isNull()
            && owner != curve.attenuationGuid;
        selection.bindingAddress = curve.address;
        return selection;
    }

    static ExternalEditorSelection binding(const QUuid& owner, const BindingAddress& address,
                                             std::optional<PlacementContext> placement = {})
    {
        auto selection = fromCurve(CurveSelection::binding(owner, address, {}));
        selection.placement = std::move(placement);
        return selection;
    }

    static ExternalEditorSelection source(const QUuid& owner, const QUuid& entryId)
    {
        ExternalEditorSelection selection;
        selection.kind = Kind::Source;
        selection.ownerGuid = owner;
        selection.sourceEntryId = entryId;
        return selection;
    }

    bool isEmpty() const { return kind == Kind::None; }

    bool sameContext(const ExternalEditorSelection& other) const
    {
        if (kind != other.kind || ownerGuid != other.ownerGuid || placement != other.placement)
            return false;
        switch (kind) {
        case Kind::Attenuation:
            return curve.attenuationGuid == other.curve.attenuationGuid
                && curve.dimension == other.curve.dimension;
        case Kind::Binding:
            return bindingAddress == other.bindingAddress;
        case Kind::Source:
            return sourceEntryId == other.sourceEntryId;
        case Kind::None:
            return false;
        }
        return false;
    }
};

}

Q_DECLARE_METATYPE(VNX::ExternalEditorSelection)
