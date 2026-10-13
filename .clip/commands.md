# Project commands

Run from the repository root. Configure before building or testing; native
dependencies are listed in README.dependencies. GNC_BUILD_DIR defaults to
.build, and GNC_JOBS overrides the default limit of four build jobs.

- Configure: `python3 util/dev.py configure` - Generate the CMake build tree. #safe #quick
- Build: `python3 util/dev.py build` - Build the configured application. #safe #slow #companion
- Run: `python3 util/dev.py build && "${GNC_BUILD_DIR:-.build}/bin/gnucash"` - Build and open the desktop app; it can edit real books and contact online services. #destructive #interactive #long-running #companion
- Test: `python3 util/dev.py test` - Build test prerequisites and run the CMake check target. #safe #slow #background
- Tooling tests: `python3 -m unittest discover -s util -p test_dev.py` - Test the task runner using fake CMake executables; does not verify the application. #safe #quick
- Check commands: `clip commands check --strict` - Validate command syntax and effect decorators. #safe #quick
