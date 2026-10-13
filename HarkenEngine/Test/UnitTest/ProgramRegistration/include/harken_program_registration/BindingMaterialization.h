#ifndef HARKEN_PROGRAM_REGISTRATION_BINDING_MATERIALIZATION_H
#define HARKEN_PROGRAM_REGISTRATION_BINDING_MATERIALIZATION_H

#include "harken/param/Curve.h"

#include <cstdint>

namespace harkenprogramregistration {

bool MaterializeBindingCurveV1(const harken::CurvePoint* points,
                               const harken::CurveHandlePair* handles,
                               std::uint32_t pointCount,
                               harken::Curve* output) noexcept;

bool ValidateBindingTargetV1(std::uint32_t scopeKind,
                             std::uint32_t targetId,
                             std::uint32_t transitionMs) noexcept;

} // namespace harkenprogramregistration

#endif // HARKEN_PROGRAM_REGISTRATION_BINDING_MATERIALIZATION_H
