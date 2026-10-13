#ifndef HARKEN_STUDIO_BUILD_BINDING_MATERIALIZATION_H
#define HARKEN_STUDIO_BUILD_BINDING_MATERIALIZATION_H

#include "harken_studio_build/Curve.h"

#include <cstdint>

namespace harkenstudiobuild {

bool MaterializeBindingCurveV1(const harkenstudiobuild::CurvePoint* points,
                               const harkenstudiobuild::CurveHandlePair* handles,
                               std::uint32_t pointCount,
                               harkenstudiobuild::Curve* output) noexcept;

bool ValidateBindingTargetV1(std::uint32_t scopeKind,
                             std::uint32_t targetId,
                             std::uint32_t transitionMs) noexcept;

} // namespace harkenstudiobuild

#endif // HARKEN_STUDIO_BUILD_BINDING_MATERIALIZATION_H
