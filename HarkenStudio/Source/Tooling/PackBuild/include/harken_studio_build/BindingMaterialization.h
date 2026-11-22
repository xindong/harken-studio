#ifndef HARKEN_STUDIO_BUILD_BINDING_MATERIALIZATION_H
#define HARKEN_STUDIO_BUILD_BINDING_MATERIALIZATION_H

#include "harken_studio_build/Curve.h"

#include <cstdint>

namespace harken::studio::build {

bool MaterializeBindingCurveV1(const harken::studio::build::CurvePoint* points,
                               const harken::studio::build::CurveHandlePair* handles,
                               std::uint32_t pointCount,
                               harken::studio::build::Curve* output) noexcept;

bool ValidateBindingTargetV1(std::uint32_t scopeKind,
                             std::uint32_t targetId,
                             std::uint32_t transitionMs) noexcept;

} // namespace harken::studio::build

#endif // HARKEN_STUDIO_BUILD_BINDING_MATERIALIZATION_H
