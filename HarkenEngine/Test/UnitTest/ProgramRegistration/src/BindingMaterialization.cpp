#include "harken_program_registration/BindingMaterialization.h"

#include "harken/param/ParamTargetTable.h"

namespace harkenprogramregistration {

bool MaterializeBindingCurveV1(const harken::CurvePoint* points,
                               const harken::CurveHandlePair* handles,
                               std::uint32_t pointCount,
                               harken::Curve* output) noexcept {
    if (output == nullptr || points == nullptr || pointCount == 0 ||
        (pointCount > 1 && handles == nullptr)) {
        return false;
    }
    harken::Curve candidate{};
    if (!candidate.SetPoints(points, handles, pointCount)) return false;
    for (std::uint32_t i = 0; i < candidate.pointCount; ++i) {
        auto& point = candidate.points[i];
        point.x = point.x == 0.0f ? 0.0f : point.x;
        point.y = point.y == 0.0f ? 0.0f : point.y;
        if (i + 1 < candidate.pointCount) {
            auto& segment = candidate.handles[i];
            segment.h1x = segment.h1x == 0.0f ? 0.0f : segment.h1x;
            segment.h1y = segment.h1y == 0.0f ? 0.0f : segment.h1y;
            segment.h2x = segment.h2x == 0.0f ? 0.0f : segment.h2x;
            segment.h2y = segment.h2y == 0.0f ? 0.0f : segment.h2y;
        }
    }
    *output = candidate;
    return true;
}

bool ValidateBindingTargetV1(std::uint32_t scopeKind,
                             std::uint32_t targetId,
                             std::uint32_t transitionMs) noexcept {
    const harken::TargetMeta* target = harken::FindTargetMeta(scopeKind, targetId);
    return target != nullptr &&
           (transitionMs == 0 || target->supportsTransition);
}

} // namespace harkenprogramregistration
