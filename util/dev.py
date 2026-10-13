"""Discoverable development tasks around GnuCash's existing CMake targets."""

import argparse
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_JOBS = min(os.cpu_count() or 1, 4)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("task", choices=["configure", "build", "test"])
    parser.add_argument("arguments", nargs=argparse.REMAINDER,
                        help="arguments passed to CMake; use -- before options")
    args = parser.parse_args()
    build_dir = os.environ.get("GNC_BUILD_DIR", str(ROOT / ".build"))
    jobs = os.environ.get("GNC_JOBS", str(DEFAULT_JOBS))
    extra = args.arguments
    if extra[:1] == ["--"]:
        extra = extra[1:]
    if args.task == "configure":
        return subprocess.call(["cmake", "-S", str(ROOT), "-B", build_dir, *extra], cwd=ROOT)
    if not jobs.isascii() or not jobs.isdigit() or int(jobs) < 1:
        parser.error("GNC_JOBS must be a positive integer")
    command = ["cmake", "--build", build_dir, "--parallel", jobs]
    if args.task == "test":
        command += ["--target", "check"]
    return subprocess.call(command + extra, cwd=ROOT,
                           env=dict(os.environ, CTEST_OUTPUT_ON_FAILURE="ON"))


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except OSError as error:
        print(f"Cannot run CMake: {error}. Check that cmake is installed and executable.",
              file=sys.stderr)
        raise SystemExit(1)
