#ifndef HARKEN_HOST_ATT04_NEAR_FAR_PROBE_H
#define HARKEN_HOST_ATT04_NEAR_FAR_PROBE_H

namespace harkenhost {

// Entry for `harken_host att04-nearfar --root <gold-dir> --action <stableId>
// --distance <meters> [--content <packName>] [--seconds N] [--null]`.
// One process, one distance. Null-backend only for ATT04.
// Returns:
//   0 = Activate succeeded, Post produced a firing, peak printed after
//       Post plus at least one more Vn_Update;
//   1 = usage;
//   2 = Init failed;
//   3 = Activate / emitter / Post / peak capture failed.
int RunAtt04NearFarProbe(int argc, char** argv);
void PrintAtt04NearFarProbeUsage();

}

#endif
