#!/usr/bin/env python3
"""Build paired standalone Vulkan fixtures from the existing server build cache."""
from __future__ import annotations

import argparse
import json
import shlex
import subprocess
from pathlib import Path


def run(argv: list[str], cwd: Path) -> None:
    subprocess.run(argv, cwd=cwd, check=True)


def source_command(build: Path) -> tuple[list[str], Path, Path]:
    rows = json.loads((build / "compile_commands.json").read_text())
    row = next(r for r in rows if r["file"].endswith("/server/encoder/nxwarp_codec_direct.cpp"))
    old_repo = Path(row["file"]).resolve().parents[2]
    return shlex.split(row["command"]), Path(row["directory"]).resolve(), old_repo


def compile_one(base: list[str], cwd: Path, original_source: Path, source: Path,
                output: Path, repo: Path, old_repo: Path, fixture: bool = False) -> None:
    args = list(base)
    args = [arg.replace(str(old_repo), str(repo)) for arg in args]
    try:
        i = args.index(str(original_source))
    except ValueError:
        i = next((i for i, a in enumerate(args) if a.endswith("/server/encoder/nxwarp_codec_direct.cpp")), -1)
    if i < 0:
        raise RuntimeError("compile command has no nxwarp_codec_direct.cpp source")
    args[i] = str(source)
    cleaned: list[str] = []
    skip = False
    for arg in args:
        if skip:
            skip = False
            continue
        if arg in {"-MD", "-MMD", "-MP", "-DNDEBUG"}:
            continue
        if arg in {"-MF", "-MT", "-MQ", "-MJ"}:
            skip = True
            continue
        if any(arg.startswith(x) for x in ("-MF", "-MT", "-MQ", "-MJ")):
            continue
        cleaned.append(arg)
    args = cleaned
    args[args.index("-o") + 1] = str(output)
    additions = [f"-I{repo / 'server' / 'encoder'}", f"-I{repo / 'common'}", f"-I{repo / 'tests'}"]
    if fixture:
        additions.append("-DNX_DIRECT_TEST_WIDTH=2176")
    args[1:1] = additions
    output.parent.mkdir(parents=True, exist_ok=True)
    run(args, cwd)


def link_fixture(build: Path, fixture_obj: Path, codec_obj: Path, output: Path) -> None:
    link = build / "server/CMakeFiles/wivrn-server.dir/link.txt"
    args = shlex.split(link.read_text())
    args = [a for a in args if not a.endswith("main.cpp.o") and not a.endswith("encoder/nxwarp_codec_direct.cpp.o")]
    out_index = args.index("-o")
    args[out_index + 1] = str(output)
    args[out_index:out_index] = [str(fixture_obj), str(codec_obj)]
    run(args, build / "server")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--build", type=Path, required=True, help="existing server CMake build with compile_commands.json")
    parser.add_argument("--compile-only", action="store_true", help="compile baseline/candidate codec objects and fixture object")
    parser.add_argument("--link", action="store_true", help="also link two standalone test binaries; never launches them")
    args = parser.parse_args()
    repo, build = args.repo.resolve(), args.build.resolve()
    scratch = Path(__file__).resolve().parent
    base, cwd, old_repo = source_command(build)
    original_source = old_repo / "server/encoder/nxwarp_codec_direct.cpp"
    fixture_obj = scratch / "direct_motion_gpu_test.o"
    baseline_src = scratch / "nxwarp_codec_direct-baseline-08f55c16.cpp"
    candidate_src = repo / "server/encoder/nxwarp_codec_direct.cpp"
    baseline_obj, candidate_obj = scratch / "codec-baseline.o", scratch / "codec-candidate.o"
    compile_one(base, cwd, original_source, baseline_src, baseline_obj, repo, old_repo)
    compile_one(base, cwd, original_source, candidate_src, candidate_obj, repo, old_repo)
    fixture_source = scratch / "direct_motion_gpu_test.cpp"
    compile_one(base, cwd, original_source, fixture_source, fixture_obj, repo, old_repo, fixture=True)
    print(f"compiled baseline {baseline_obj}")
    print(f"compiled candidate {candidate_obj}")
    print(f"compiled fixture {fixture_obj}")
    if args.link:
        for label, obj in (("baseline", baseline_obj), ("candidate", candidate_obj)):
            binary = scratch / f"motion-gpu-{label}"
            link_fixture(build, fixture_obj, obj, binary)
            print(f"linked {binary}")


if __name__ == "__main__":
    main()
