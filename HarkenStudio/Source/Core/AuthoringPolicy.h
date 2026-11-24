#pragma once

#include "Timeline/TimelineTypes.h"

namespace harken::studio::AuthoringPolicy {

using ObjectResolver = std::function<const ProjectItem*(uint32_t)>;

bool supportsObjectColorEditing();
TimelineValidation validateTimeline(const ProjectItem& item,
                                    const TimelineItemResolver& resolve,
                                    const ObjectResolver& resolveObject = {});
TimelineValidation validateNewSubtree(const ProjectItem& item,
                                      const ProjectItem* destinationParent,
                                      const TimelineItemResolver& resolve,
                                      const ObjectResolver& resolveObject = {});

}
