#ifndef HARKEN_RUNTIME_REMOTE_RUNTIME_H
#define HARKEN_RUNTIME_REMOTE_RUNTIME_H

#include "RemoteSeam.h"

namespace harken::remote {
void SetBeforeCommandDispatchForTests(void (*hook)(void*), void* context);
void SetBeforeBusyReplyForTests(void (*hook)());
unsigned PendingCommandsForTests();
}

#endif
