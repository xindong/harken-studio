// ABI pins for retained result and notification values. Everything here is a
// compile-time assertion; the single runtime test exists so the pins
// participate in test discovery and reporting.
#include <harken/Harken.h>

// Action firing admission keeps the published capacity result value.
static_assert(VN_NO_ACTION == -311, "renamed error code must keep its value");

// Notification types keep the values their pre-rename spellings had.
static_assert(VN_NOTIFY_VOICE_ENDED == 0, "renamed value must not renumber");
static_assert(VN_NOTIFY_VOICE_STOPPED_BY_FADE == 1,
              "renamed value must not renumber");
static_assert(VN_NOTIFY_DEGRADATION_OCCURRED == 2,
              "renamed value must not renumber");
static_assert(VN_NOTIFY_VOICE_STARVED == 3, "renamed value must not renumber");
static_assert(VN_NOTIFY_VOICE_STOLEN == 4, "renamed value must not renumber");
static_assert(VN_NOTIFY_VOICE_PARKED == 5, "renamed value must not renumber");
static_assert(VN_NOTIFY_VOICE_RETURNED == 6,
              "renamed value must not renumber");
