#include "RuntimeCapabilityMatrix.h"
#include "LanguageContextService.h"
#include "PlatformContextService.h"

namespace VNX::RuntimeCapabilityMatrix {
namespace {
RuntimeCapabilityContext currentContext()
{
    return {currentWorldPlatformScope(PlatformContextService::instance().context()),
        LanguageContextService::instance().language()};
}
}

RuntimeCapabilityEntry evaluate(const ProjectItem& item,
                                const ProjectData& project,
                                RuntimeCapabilityOperation operation,
                                RuntimeCapabilityFeature feature,
                                std::optional<BuildVariant> variant)
{
    return evaluateForContext(item, project, operation, feature, std::move(variant), currentContext());
}

RuntimeCapabilityAssessment assess(const ProjectItem& item,
                                   const ProjectData& project,
                                   RuntimeCapabilityOperation operation,
                                   std::optional<BuildVariant> variant)
{
    return assessForContext(item, project, operation, std::move(variant), currentContext());
}

RuntimeCapabilityAssessment assess(const ProjectData& project,
                                   harkenstudio::ObjectId rootStableId,
                                   RuntimeCapabilityOperation operation,
                                   std::optional<BuildVariant> variant)
{
    return assessForContext(project, rootStableId, operation, std::move(variant), currentContext());
}

RuntimeCapabilityEntry evaluate(const ProjectItem& item,
                                const ProjectData& project,
                                const QString& operation,
                                RuntimeCapabilityFeature feature)
{
    auto parsed = parseOperation(operation);
    if (!parsed && operation == QStringLiteral("execute"))
        parsed = item.type == ProjectItem::Type::Action ? RuntimeCapabilityOperation::ActionRegistration : RuntimeCapabilityOperation::AuditionPlay;
    if (!parsed)
        return {item.type, RuntimeCapabilityOperation::LiveSync, feature, RuntimeCapabilityState::InvalidAuthoring,
                QStringLiteral("Unknown runtime capability operation")};
    return evaluate(item, project, *parsed, feature);
}

}
