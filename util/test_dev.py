"""Exercise the developer task runner without building GnuCash."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


RUNNER = Path(__file__).with_name("dev.py")


class DeveloperTasks(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="gnucash dev ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.log = self.root / "commands.jsonl"
        fake = self.root / "cmake"
        fake.write_text(
            f"#!{sys.executable}\n"
            "import json, os, sys\n"
            "with open(os.environ['COMMAND_LOG'], 'a') as log:\n"
            "    log.write(json.dumps(sys.argv[1:]) + '\\n')\n"
            "sys.exit(int(os.environ.get('COMMAND_EXIT', '0')))\n"
        )
        fake.chmod(0o755)
        self.env = dict(os.environ, PATH=f"{self.root}{os.pathsep}{os.environ['PATH']}",
                        COMMAND_LOG=str(self.log), GNC_BUILD_DIR=str(self.root / "build dir"),
                        GNC_JOBS="2")

    def run_task(self, *args):
        return subprocess.run([sys.executable, str(RUNNER), *args], env=self.env,
                              cwd=self.root, capture_output=True, text=True)

    def commands(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def test_build_preserves_paths_and_bounds_parallelism(self):
        result = self.run_task("build")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.commands(), [["--build", str(self.root / "build dir"),
                                           "--parallel", "2"]])

    def test_tests_build_fixtures_and_propagate_failure(self):
        self.env["COMMAND_EXIT"] = "7"
        result = self.run_task("test")
        self.assertEqual(result.returncode, 7, result.stderr)
        self.assertEqual(self.commands(), [["--build", str(self.root / "build dir"),
                                           "--parallel", "2", "--target", "check"]])

    def test_configure_forwards_arguments_without_shell_expansion(self):
        option = "-DCMAKE_INSTALL_PREFIX=/tmp/a path/$literal"
        result = self.run_task("configure", "--", option)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.commands(), [["-S", str(RUNNER.parent.parent), "-B",
                                           str(self.root / "build dir"), option]])

    def test_invalid_job_limit_does_not_launch_build(self):
        self.env["GNC_JOBS"] = "0"
        result = self.run_task("build")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("positive integer", result.stderr)
        self.assertFalse(self.log.exists())

    def test_missing_cmake_reports_actionable_error(self):
        (self.root / "cmake").unlink()
        self.env["PATH"] = str(self.root)
        result = self.run_task("build")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cmake", result.stderr)
        self.assertNotIn("Traceback", result.stderr)

    def test_default_parallelism_is_bounded(self):
        del self.env["GNC_JOBS"]
        result = self.run_task("build")
        self.assertEqual(result.returncode, 0, result.stderr)
        jobs = int(self.commands()[0][-1])
        self.assertGreaterEqual(jobs, 1)
        self.assertLessEqual(jobs, 4)


if __name__ == "__main__":
    unittest.main()
