
#pragma once

#include "../Component.h"
#include "../PropertyDescriptor.h"
#include "../ProjectTypes.h"
#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <vector>

namespace VNX {

class VAction : public ReflectedComponent<VAction>
{
public:
    static constexpr ComponentType kType = ComponentType::Action;
    ComponentType getType() const override { return kType; }

    static const PropertySchema& schema();

    std::vector<VActionStep> steps;

    void toJsonExtra(QJsonObject& j) const override;
    void fromJsonExtra(const QJsonObject& j) override;

    QString searchableText() const override { return {}; }
};

inline const PropertySchema& VAction::schema()
{
    static const PropertySchema s = withOwner<VAction>({});
    return s;
}

}
