// The downmix null-drill verb: renders a deterministic multichannel program
// through the engine's real master->device output fold on the null backend and
// writes the pre-fold mix and the folded output as raw interleaved float32, so
// an independent reference tool (HarkenEngine/Test/E2E/downmix_null_reference.py) can fold
// the mix from the spec Appendix A coefficient table and null-compare it to the
// engine's fold sample by sample. The engine side only renders and captures;
// the fold-correctness verdict is the independent reference's. Split from
// Main.cpp like remote-serve / trace-replay: its option surface and run loop
// share nothing with the demo scenes.
#ifndef HARKEN_HOST_DOWNMIX_NULL_DRILL_H
#define HARKEN_HOST_DOWNMIX_NULL_DRILL_H

namespace harkenhost {

// Entry for `harken_host downmix-null-drill [--seconds N] [--out <dir>]`.
// Returns the process exit code:
//   0 = all drills rendered and their artifacts written;
//   1 = usage or output-directory / file-write error;
//   2 = engine init or render failure (no trustworthy artifacts left behind).
// The verb only produces artifacts; run HarkenEngine/Test/E2E/downmix_null_reference.py over
// the same output directory for the null verdict.
int RunDownmixNullDrill(int argc, char** argv);

// Usage text block for downmix-null-drill, printed by the main usage dump.
void PrintDownmixNullDrillUsage();

} // namespace harkenhost

#endif // HARKEN_HOST_DOWNMIX_NULL_DRILL_H
