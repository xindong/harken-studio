from __future__ import annotations

import importlib.util
import sys
from pathlib import Path
from types import ModuleType


def load_script_module(name: str, script: Path) -> ModuleType:
    script_dir = str(script.parent)
    if script_dir not in sys.path:
        sys.path.insert(0, script_dir)
    spec = importlib.util.spec_from_file_location(name, script)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module
