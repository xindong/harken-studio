#include "ActionClosureSemantics.h"
#include "LanguageContextService.h"
#include "PlatformContextService.h"

namespace VNX::ActionClosureSemantics {

ActionClosurePlan build(const ProjectData& project, harkenstudio::ObjectId actionStableId,
                        const BuildVariant& variant)
{
    return buildForContext(project, actionStableId, variant,
        {currentWorldPlatformScope(PlatformContextService::instance().context()),
         LanguageContextService::instance().language()});
}

}
