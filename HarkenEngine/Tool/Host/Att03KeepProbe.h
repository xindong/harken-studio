#ifndef HARKEN_HOST_ATT03_KEEP_PROBE_H
#define HARKEN_HOST_ATT03_KEEP_PROBE_H

namespace harkenhost {

// Entry for `harken_host att03-keep --init <Init.pack> --content <Content.pack>
// --bad <missing-or-bad.pack> --action <low>`. Null-backend only.
// Returns:
//   0 = legal set published, bad Activate refused, published stayed,
//       same-process Post still produced a firing;
//   1 = usage;
//   2 = Init failed;
//   3 = legal Activate failed, or the refused Activate destroyed published,
//       or Post after the refusal did not produce a firing.
int RunAtt03KeepProbe(int argc, char** argv);
void PrintAtt03KeepProbeUsage();

}

#endif
