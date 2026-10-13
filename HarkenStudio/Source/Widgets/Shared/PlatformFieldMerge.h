#pragma once

#include "Core/PlatformContext.h"
#include "Core/PlayablePropertyAuthoringV1.h"

#include <QPair>
#include <QVector>

namespace VNX {

struct PropertyDescriptor;

struct PlatformFieldView {
    enum class Coverage { None, Some, All };

    bool uniform = true;
    PlayablePropertyStateV1 value;
    QVector<QPair<ProjectPlatformId, PlayablePropertyStateV1>> perPlatform;
    Coverage coverage = Coverage::None;
};

PlatformFieldView platformFieldView(ProjectItem& owner,
                                   const PlayablePropertyLocatorV1& locator,
                                   uint8_t propertyId,
                                   const PlatformContext& context);

QString platformFieldMixedTooltip(const PlatformFieldView& view,
                                   const QVector<ProjectPlatform>& platforms,
                                   const PropertyDescriptor* descriptor = nullptr);

}
