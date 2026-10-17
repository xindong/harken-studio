#ifndef HARKEN_RUNTIME_REMOTE_RUNTIME_H
#define HARKEN_RUNTIME_REMOTE_RUNTIME_H

namespace harken::remote {
void Pump();
void Shutdown(bool engineTerminating = false);
void SetBeforeCommandDispatchForTests(void (*hook)(void*), void* context);
unsigned PendingCommandsForTests();
}

#endif
