#!/usr/bin/env python3
import json
import pathlib
import shutil
import subprocess
import sys


def main():
    if len(sys.argv) != 2:
        raise SystemExit("Usage: flow-helper-smoke.py <installed-helper>")
    repo = pathlib.Path(__file__).resolve().parents[2]
    helper = pathlib.Path(sys.argv[1]).resolve()
    fixture = repo / "tests" / "fixtures" / "flow.cpp"
    work = repo / ".flow-helper-smoke"
    project = work / "project"
    project.mkdir(parents=True, exist_ok=True)
    database = [{
        "directory": str(project),
        "file": str(fixture),
        "arguments": ["clang++", "-std=c++20", "-c", str(fixture), "-o", "flow.o"],
    }]
    (project / "compile_commands.json").write_text(json.dumps(database), encoding="utf-8")
    try:
        result = subprocess.run(
            [
                str(helper),
                "--project", str(project / "compile_commands.json"),
                "--file", str(fixture),
                "--line", "1",
                "--qualified-name", "flow_fixture",
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        graph = json.loads(result.stdout)
        if graph.get("kind") != "clang-cfg" or not graph.get("blocks") or not graph.get("edges"):
            raise RuntimeError(f"Installed helper returned an incomplete graph: {graph}")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
