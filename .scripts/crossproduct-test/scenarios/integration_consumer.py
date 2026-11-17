from pathlib import Path

from drivers import engine
from drivers.process import run_bounded
from scenarios import integration_content as content

HERE = Path(__file__).resolve().parents[1]


def run(ctx) -> None:
    ctx.require("studio-cli", "engine-development", "integration-package", "dotnet")
    exported = content.build(ctx.studio(), ctx.work)
    native = (ctx.engine_install / engine.library_name("development")).parent
    log = ctx.work / "consumer.log"
    code = run_bounded([str(ctx.args.dotnet), "run", "--project", str(HERE / "dotnet" / "HarkenCrossProductConsumer.csproj"),
                        "--configuration", "Release", "--artifacts-path", str(ctx.work / "dotnet"),
                        f"-p:HarkenPackageRoot={ctx.args.integration_package}", "--",
                        str(native), str(exported["main"]), content.ACTIONS["tone"]],
                       timeout=ctx.args.timeout, log=log)
    text = log.read_text(encoding="utf-8", errors="replace")
    ctx.details["consumer"] = text.strip().splitlines()[-1] if text.strip() else ""
    if code != 0 or "PASS" not in text:
        raise AssertionError(f"Integration consumer exited {code}; see {log}")
