#include "harken_studio_build/BindingMaterialization.h"

#include "harken_studio_build/BindingTargets.h"

namespace harken::studio::build {

bool MaterializeBindingCurveV1(const harken::studio::build::CurvePoint* points,
                               const harken::studio::build::CurveHandlePair* handles,
                               std::uint32_t pointCount,
                               harken::studio::build::Curve* output) noexcept {
    if (output == nullptr || points == nullptr || pointCount == 0 ||
        (pointCount > 1 && handles == nullptr)) {
        return false;
    }
    harken::studio::build::Curve candidate{};
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
    const harken::studio::build::TargetMeta* target = harken::studio::build::FindTargetMeta(scopeKind, targetId);
    return target != nullptr &&
           (transitionMs == 0 || target->supportsTransition);
}

} // namespace harken::studio::build
