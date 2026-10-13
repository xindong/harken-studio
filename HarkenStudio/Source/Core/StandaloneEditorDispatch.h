#pragma once

#include "ProjectTypes.h"

namespace VNX {

enum class StandaloneEditorPushReason
{
    SelectionChanged,
    PinToggled,
    RestoreSync,
};

struct StandaloneEditorDispatch {
    static bool shouldCollapse(bool accepted, bool pinned, bool atDefaultSlot);
    static bool shouldHide(StandaloneEditorPushReason reason);
};

} // namespace VNX
