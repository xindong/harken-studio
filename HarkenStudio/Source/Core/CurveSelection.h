#pragma once

#include "BindingSemantics.h"
#include "ProjectTypes.h"

#include <QMetaType>
#include <QString>
#include <QUuid>

namespace VNX {

struct CurveSelection
{
    enum class Kind : uint8_t {
        None = 0,
        Attenuation = 1,
        Binding = 2,
    };

    Kind kind = Kind::None;
    QUuid attenuationGuid;
    QString attenuationName;
    AttenuationDimension dimension = AttenuationDimension::Volume;
    QUuid hostGuid;
    BindingAddress address;
    harkenstudio::ObjectId bindingStableId = {};

    static CurveSelection attenuation(QUuid guid, QString name,
                                      AttenuationDimension dim)
    {
        CurveSelection selection;
        selection.kind = Kind::Attenuation;
        selection.attenuationGuid = guid;
        selection.attenuationName = std::move(name);
        selection.dimension = dim;
        return selection;
    }

    static CurveSelection binding(QUuid host, BindingAddress target,
                                  harkenstudio::ObjectId stableId)
    {
        CurveSelection selection;
        selection.kind = Kind::Binding;
        selection.hostGuid = host;
        selection.address = target;
        selection.bindingStableId = stableId;
        return selection;
    }

    bool hasAttenuation() const
    {
        return kind == Kind::Attenuation
            && (!attenuationGuid.isNull() || !attenuationName.trimmed().isEmpty());
    }

    bool hasBinding() const
    {
        return kind == Kind::Binding && !hostGuid.isNull() && address.slotId != 0;
    }

    bool isEmpty() const
    {
        return !hasAttenuation() && !hasBinding();
    }

    bool belongsTo(const ProjectItem& item) const
    {
        if (!hasAttenuation())
            return false;
        if (!attenuationGuid.isNull())
            return attenuationGuid == item.guid;
        return attenuationName.trimmed() == item.name.trimmed();
    }
};

}

Q_DECLARE_METATYPE(VNX::CurveSelection)
