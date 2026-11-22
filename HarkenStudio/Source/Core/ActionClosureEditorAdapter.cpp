#include "ActionClosureSemantics.h"
#include "LanguageContextService.h"
#include "PlatformContextService.h"

namespace harken::studio::ActionClosureSemantics {

ActionClosurePlan build(const ProjectData& project, harken::studio::types::ObjectId actionStableId,
                        const BuildVariant& variant)
{
    return buildForContext(project, actionStableId, variant,
        {currentWorldPlatformScope(PlatformContextService::instance().context()),
         LanguageContextService::instance().language()});
}

}
