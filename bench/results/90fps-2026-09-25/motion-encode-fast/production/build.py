#!/usr/bin/env python3
"""Build the offline production-codec A/B harness against two local WiVRn builds."""
import argparse
import json
import shlex
import subprocess
from pathlib import Path


def compile_rows(build_dir: Path):
    rows = json.loads((build_dir / "compile_commands.json").read_text())
    row = next(r for r in rows if r["file"].endswith("/server/encoder/nxwarp_codec_direct.cpp"))
    cwd = Path(row.get("directory", build_dir)).resolve()
    args = list(row["arguments"]) if "arguments" in row else shlex.split(row["command"])
    return cwd, args


def clean_args(args):
    out, skip = [], False
    depflags = {"-MF", "-MT", "-MQ", "-MJ"}
    for arg in args:
        if skip:
            skip = False
            continue
        if arg in {"-DNDEBUG", "-MD", "-MMD", "-MP"}:
            continue
        if arg in depflags:
            skip = True
            continue
        if any(arg.startswith(flag) for flag in depflags):
            continue
        out.append(arg)
    return out


def build_one(name, repo, build_dir, out_dir):
    repo, build_dir = repo.resolve(), build_dir.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    cwd, args = compile_rows(build_dir)
    codec_source = repo / "server/encoder/nxwarp_codec_direct.cpp"
    src_i = next(i for i, arg in enumerate(args) if arg.endswith("/server/encoder/nxwarp_codec_direct.cpp"))
    args[src_i] = str(codec_source)
    args = clean_args(args)
    args[1:1] = [f"-I{repo / 'common'}", f"-I{repo / 'server/encoder'}", f"-I{repo}"]
    obj = out_dir / "codec.o"
    args[args.index("-o") + 1] = str(obj)
    subprocess.run(args, cwd=cwd, check=True)

    _, test_args = compile_rows(build_dir)
    test_args = [a for a in test_args if not a.endswith("/server/encoder/nxwarp_codec_direct.cpp")]
    test_args = clean_args(test_args)
    test_args[1:1] = ["-DNX_DIRECT_TEST_WIDTH=2176", f"-I{repo / 'common'}",
                      f"-I{repo / 'server/encoder'}", f"-I{repo}", f"-I{Path(__file__).parent}"]
    test_obj = out_dir / "motion_gpu_test.o"
    test_args[test_args.index("-o") + 1] = str(test_obj)
    test_args = [a for a in test_args if not a.endswith(".cpp")]
    test_args.insert(1, str(Path(__file__).parent / "motion_gpu_test_wrapper.cpp"))
    subprocess.run(test_args, cwd=cwd, check=True)

    link_file = build_dir / "server/CMakeFiles/wivrn-server.dir/link.txt"
    link = shlex.split(link_file.read_text())
    link = [a for a in link if a != "-DNDEBUG" and "--dependency-file=" not in a
            and not a.endswith("main.cpp.o") and not a.endswith("nxwarp_codec_direct.cpp.o")]
    out = out_dir / "direct_motion_bench"
    oi = link.index("-o")
    link[oi + 1] = str(out)
    link[oi:oi] = [str(test_obj), str(obj)]
    subprocess.run(link, cwd=build_dir / "server", check=True)
    print(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--baseline-repo", type=Path, required=True,
                    help="WiVRn checkout at baseline revision 143d6210bdda289621996b15304298adc23cbfa5")
    ap.add_argument("--baseline-build", type=Path, required=True,
                    help="Configured build directory for baseline checkout")
    ap.add_argument("--candidate-repo", type=Path, required=True,
                    help="WiVRn checkout at candidate revision 070b671b1e2cbd18cd1c676bc88c0d12391cf553")
    ap.add_argument("--candidate-build", type=Path, required=True,
                    help="Configured build directory for candidate checkout")
    ap.add_argument("--output", type=Path, required=True, help="Scratch output directory")
    a = ap.parse_args()
    build_one("baseline", a.baseline_repo, a.baseline_build, a.output / "baseline")
    build_one("candidate", a.candidate_repo, a.candidate_build, a.output / "candidate")


if __name__ == "__main__":
    main()
