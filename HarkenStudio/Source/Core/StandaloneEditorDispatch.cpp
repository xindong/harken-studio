#include "StandaloneEditorDispatch.h"

namespace VNX {

bool StandaloneEditorDispatch::shouldCollapse(bool accepted, bool pinned, bool atDefaultSlot)
{
    return !accepted && !pinned && atDefaultSlot;
}

bool StandaloneEditorDispatch::shouldHide(StandaloneEditorPushReason reason)
{
    return reason != StandaloneEditorPushReason::RestoreSync;
}

} // namespace VNX
