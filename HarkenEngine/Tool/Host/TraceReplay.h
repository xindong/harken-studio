// The trace-replay verb: re-executes a VTRC session journal on the null
// backend with the recorded config, seed, and block/tick schedule, and
// captures the rendered output for byte comparison. Split from Main.cpp
// because its option surface and run loop share nothing with the demo
// scenes (the remote-serve precedent).
#ifndef HARKEN_HOST_TRACE_REPLAY_H
#define HARKEN_HOST_TRACE_REPLAY_H

namespace harkenhost {

// Entry for `harken_host trace-replay <file> ...`. Returns the process exit
// code:
//   0 = ordinary deterministic replay execution completed; the
//       machine-readable completion and device-format-identity lines state
//       whether the journal is complete and whether a real-device format is
//       provable from VTRC v2;
//   1 = usage, file, or journal parse error;
//   2 = engine init failure;
//   3 = LIMITED deterministic replay completed: the journal carries
//       callback-context API calls, which execute after their enclosing
//       update returns — the render and capture are still produced, but
//       the run is not ordinary proof of the recorded interleaving, and
//       automation must not read it as plain success;
//   4 = replay refused by a content-hash mismatch or unreadable content
//       file (the report names the file; no capture is left behind).
int RunTraceReplay(int argc, char** argv);

// Usage text block for trace-replay, printed by the main usage dump.
void PrintTraceReplayUsage();

} // namespace harkenhost

#endif // HARKEN_HOST_TRACE_REPLAY_H
