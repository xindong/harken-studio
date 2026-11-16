#pragma once

#include "Core/CurveSemantics.h"

#include <functional>

class QMenu;

namespace VNX {

QMenu* addCurvePresetMenu(QMenu* parentMenu,
                         const CurvePointDef& a, const CurvePointDef& b,
                         const CurveHandlePairDef& handles, CurveAxisScale scale,
                         std::function<void(CurvePreset)> apply);

}
