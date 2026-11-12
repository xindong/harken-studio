# Independent Unity Integration API consumers

Run from the repository root with Python 3.12 and .NET 9:

```text
python HarkenIntegration/Test/ExternalConsumer/check_boundary.py
python HarkenIntegration/Test/ExternalConsumer/check_boundary.py --native-dir <production-runtime-directory> --publication HarkenIntegration/Test/UnityFixture/Data/U4Original
```

The runner stages the selected Unity Integration Package, builds `Harken.Core.dll`, and compiles an
unrelated `Harken.ExternalConsumer` assembly referencing only that DLL. It has no
friend grant and no managed API source compile items. A reflection check covers all
generated types/constants; each forbidden access is compiled independently and
must fail with the matching access diagnostic. `--integration-root` (legacy alias: `--sdk-root`) selects an assembled
Unity Integration Package instead of the repository source. Every invocation keeps its own
logs and result manifest under `.temp` or a new `--work-dir`.

Direct builds of either fixture project also keep restore, intermediate and
binary outputs under `.temp/integration-external-consumer`, separated by project and
configuration. `Directory.Build.props` sets this before NuGet restore. The
runner overrides it with `--artifacts-path` inside each invocation's work
directory, so staged fixtures and concurrent runs keep separate build outputs.
Any legacy `bin/` or `obj/` directories are ignored and excluded from staging.

Native execution uses the requested production library and checks file/byte
companion startup, Action preparation/query/stop, spatial calls, resource failures and explicit retry,
ownership, host registration disposal, and optional Remote startup/stop/termination.
Null audio does not render automatically: release before retirement returns
`VN_IN_USE`; shutdown owns the remaining firing. This runner does not claim audio
completion or delivered notification coverage.

Unity Player tests complement the production-library host run; neither replaces
actual device/game/listening acceptance when a change requires it.
