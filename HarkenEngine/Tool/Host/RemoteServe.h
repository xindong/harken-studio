// The remote-serve verb: hosts a running engine behind the remote-control
// channel's named pipe so an external process (script, future editor) can
// drive it. Split from Main.cpp because its option surface and run loop
// share nothing with the demo scenes.
#ifndef HARKEN_HOST_REMOTE_SERVE_H
#define HARKEN_HOST_REMOTE_SERVE_H

namespace harkenhost {

// Entry for `harken_host remote-serve ...`. Returns the process exit code:
// 0 = served and shut down cleanly; 1 = usage error; 3 = engine or pipe
// failure.
int RunRemoteServe(int argc, char** argv);

// Usage text block for remote-serve, printed by the main usage dump.
void PrintRemoteServeUsage();

} // namespace harkenhost

#endif // HARKEN_HOST_REMOTE_SERVE_H
