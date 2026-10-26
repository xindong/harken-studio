#!/usr/bin/env python3
"""Compile a staged SDK separately from a non-friend consumer and access probes."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "HarkenIntegration/Test/ExternalConsumer"

# Each probe compiles separately, so one inaccessible name cannot mask another.
NEGATIVE = {
    "HarkenNative": "HarkenNative.Vn_Update()",
    "HarkenAbi": "HarkenAbi.CheckLayout()",
    "VnPCMDesc": "typeof(VnPCMDesc)",
    "VnSnapshot": "typeof(VnSnapshot)",
    "VnMonitorRecordHeader": "typeof(VnMonitorRecordHeader)",
    "VnBusTopologyDesc": "typeof(VnBusTopologyDesc)",
    "VariableCatalogGuard": "VariableCatalogGuard.QueryAndEvaluate()",
    "VariableCatalogEmitter": "VariableCatalogEmitter.SymbolName(\"x\")",
    "ActionCatalogEmitter": "typeof(ActionCatalogEmitter)",
    "RemoteCommunication": "RemoteCommunication.Stop()",
    "CreateBus": "HarkenEngine.CreateBus(default, default)",
    "ReparentBus": "HarkenEngine.ReparentBus(default, default)",
    "DestroyBus": "HarkenEngine.DestroyBus(default)",
    "NotificationFnPtr": "HarkenCallbacks.NotificationFnPtr",
    "OutstandingPinCount": "PackByteProvider.OutstandingPinCount",
    "VN_SNAPSHOT_MAX_VOICES": "VnConstants.VN_SNAPSHOT_MAX_VOICES",
}


def run(command, log, *, expect_success=True, env=None):
    result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8",
                            errors="replace", env=env)
    output = result.stdout + result.stderr
    log.write_text(output, encoding="utf-8")
    if expect_success and result.returncode:
        raise RuntimeError(f"Command failed; see {log}\n{output}")
    return result.returncode, output


def verify(sdk, work, native_dir=None, publication=None):
    staged = work / "sdk"
    shutil.copytree(sdk, staged)
    shutil.copytree(FIXTURE, work / "fixture", ignore=shutil.ignore_patterns("bin", "obj"))
    fixture = work / "fixture"
    shutil.copy2(ROOT / "HarkenIntegration/Test/ContractTest/RemoteCommunicationContract.cs", fixture)
    # The consumer references only the compiled DLL, never SDK source items.
    run(["dotnet", "build", str(fixture / "Harken.Core.csproj"), "-c", "Release",
         f"-p:HarkenSdkRoot={staged}", "--artifacts-path", str(work / "artifacts"),
         "-o", str(work / "library"), "--nologo"], work / "library.log")
    library = work / "library/Harken.Core.dll"
    common = ["dotnet", "build", str(fixture / "Consumer.csproj"), "-c", "Release",
              f"-p:HarkenCoreAssembly={library}", "--artifacts-path", str(work / "artifacts"), "--nologo"]
    positive = work / "positive"
    run(common + [f"-p:ConsumerSource={fixture / 'Program.cs'}", f"-p:RemoteContractSource={fixture / 'RemoteCommunicationContract.cs'}", "-o", str(positive)], work / "positive.log")
    policy = ROOT / "HarkenEngine/Source/Tooling/Binding/sdk_api_surface.json"
    # Reflection checks every generated symbol's effective public visibility.
    run(["dotnet", str(positive / "Harken.ExternalConsumer.dll"), "--surface", str(policy)],
        work / "surface.log")
    for name, expression in NEGATIVE.items():
        source = work / f"Denied_{name}.cs"
        source.write_text("using System; using Harken; class Denied { static void Main() { "
                          + f"var denied = {expression};" + " } }\n", encoding="utf-8")
        code, output = run(common + [f"-p:ConsumerSource={source}", "-o", str(work / "negative" / name)],
                           work / f"negative-{name}.log", expect_success=False)
        errors = re.findall(r"error (CS\d+): ([^\r\n]+)", output)
        expected_errors = {"CS0246"} if name == "VnMonitorRecordHeader" else {"CS0122", "CS0117"}
        if code == 0 or not errors or any(number not in expected_errors or name not in message
                                          for number, message in errors):
            raise RuntimeError(f"{name}: expected only a matching access diagnostic; got\n{output}")
    if (native_dir is None) != (publication is None):
        raise ValueError("Native execution requires both --native-dir and --publication")
    if native_dir:
        environment = dict(os.environ, HARKEN_NATIVE_DIR=str(native_dir))
        run(["dotnet", str(positive / "Harken.ExternalConsumer.dll"), "--host", str(publication)],
            work / "host.log", env=environment)
    report = {"sdk_source": str(sdk), "staged_sdk": str(staged),
              "assembly": "Harken.ExternalConsumer", "negative_probes": list(NEGATIVE),
              "public_surface": "passed", "host_execution": "passed" if native_dir else "not requested"}
    (work / "result.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"External SDK boundary passed: positive consumer and {len(NEGATIVE)} independent access probes. Evidence: {work}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-root", type=Path, default=ROOT / "HarkenIntegration/Unity/Harken")
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--native-dir", type=Path)
    parser.add_argument("--publication", type=Path)
    args = parser.parse_args()
    if args.work_dir:
        work = args.work_dir.resolve()
        work.mkdir(parents=True, exist_ok=False)
    else:
        scratch = ROOT / ".temp"
        scratch.mkdir(exist_ok=True)
        work = Path(tempfile.mkdtemp(prefix="sdk-boundary-", dir=scratch))
    verify(args.sdk_root.resolve(), work,
           args.native_dir.resolve() if args.native_dir else None,
           args.publication.resolve() if args.publication else None)


if __name__ == "__main__":
    main()
