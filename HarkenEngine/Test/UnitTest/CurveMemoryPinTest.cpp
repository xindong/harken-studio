// Compile-time footprint pins for the preallocated curve-bearing runtime
// storage. Capacity budgets depend on these exact native layouts.
#include "harken/param/BindingTable.h"
#include "harken/param/Curve.h"
#include "harken/param/Spatializer.h"

namespace {

static_assert(sizeof(harken::CurveHandlePair) == 16,
              "Bezier handle-pair footprint drifted");
static_assert(sizeof(harken::Curve) == 180,
              "curve storage footprint drifted");
static_assert(sizeof(harken::BindingTable::Definition) == 456,
              "binding definition footprint drifted");
static_assert(sizeof(harken::AttenuationProfile) == 616,
              "attenuation profile footprint drifted");

} // namespace
