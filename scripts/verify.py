#!/usr/bin/env python3
"""Run the available local checks without installing dependencies or SDKs."""

import pathlib
import shutil
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]


def run(label, command):
    print("\n{}".format(label), flush=True)
    result = subprocess.run(command, cwd=str(ROOT))
    if result.returncode:
        raise SystemExit(result.returncode)


def main():
    if not shutil.which("node"):
        raise SystemExit("Node.js is required for the browser module tests.")
    if not (shutil.which("clang") or shutil.which("cc")):
        raise SystemExit("A C compiler is required for firmware host tests.")
    run("Python HTTP integration", [sys.executable, "-m", "unittest", "discover", "-s", "tests", "-v"])
    run("Browser audio modules", ["node", "--test", "tests/test_audio.mjs"])
    for path in sorted((ROOT / "web").glob("*.js")):
        run("JavaScript syntax: {}".format(path.name), ["node", "--check", str(path)])
    run("Firmware model on host", ["sh", "firmware/tests/run_host_tests.sh"])
    print("\nLocal checks passed. ESP-IDF cross build, hardware, and actual dot calls remain unverified.")


if __name__ == "__main__":
    main()
