# Developer tasks

Install the native dependencies described in `README.dependencies` and
`cmake/README_CMAKE.txt`. The optional task runner needs Python 3 and no Python
packages; CMake remains the build definition and direct CMake commands still work.

```sh
python3 util/dev.py --help
python3 util/dev.py configure -- -DCMAKE_BUILD_TYPE=Debug
python3 util/dev.py build
python3 util/dev.py test
python3 -m unittest discover -s util -p test_dev.py
```

`GNC_BUILD_DIR` defaults to the repository's `.build`, which is ignored and
hidden from translation discovery. Use an absolute override for an external
build tree. The runner resolves relative build paths from the repository root.
Builds use at most four CPU cores by default; `GNC_JOBS` accepts a positive
integer override. Tests retain CTest's existing scheduling rules.

Arguments after `--` are forwarded as individual arguments to CMake, preserving
spaces. For example, `build -- --target gnc-engine` selects one build target.
Native tool arguments can follow another `--`, as in CMake itself. Avoid
overriding `--parallel` when `GNC_JOBS` expresses the same intent.

For a focused test, first build its prerequisites, then select the registered
CTest name. Inspect the list to avoid reporting an empty selection as success:

```sh
cmake --build .build --target testbuild --parallel 4
ctest --test-dir .build -N
ctest --test-dir .build --output-on-failure -R '<test-name>'
```

`--test-dir` requires CMake 3.20 or later. With older supported CMake versions,
run CTest from inside `.build`. The runner uses commands supported by the
project's CMake 3.14.5 minimum.

The tooling suite uses temporary fake CMake executables to exercise argument
boundaries and error propagation. It never loads user books, starts GTK, or
contacts financial services. Its passing result is not an application build.

## Convention adoption

Project commands live in `.clip/commands.md`, migrated from the Saggar command
list with descriptions and explicit effects. Run `clip commands` to list them,
`clip commands check --strict` to validate them, and
`clip sync --target agents-md` to refresh the generated guidance in `AGENTS.md`.
The Python runner remains the implementation. Clip does not check its task names
for drift, so review the command list when changing `util/dev.py`. GitHub issues
remain disabled; this paragraph records the scope of the Clip adoption.

Scope: transfer useful practices from Saggar desktop while preserving this
fork's upstream build, languages, compatibility, and release process. GitHub
issues were disabled on this fork during the work, so this section records the
local scope instead of an issue.

Adopted: a task catalog, bounded build concurrency, failure propagation,
portable tooling tests in CI, editor defaults, source ownership documentation,
explicit data and secret boundaries, focused verification, shared-tree Git
safety, and ignored review artifacts. `AGENTS.md` makes the ongoing rules
discoverable; the task runner and CI enforce only their stated behaviors.

Not transferred: npm as a second required runner, Swift formatting, Saggar's
single scheduler, its storage paths, arbitrary coverage floors, private sibling
dependencies, signing gates, or a competing changelog. Existing upstream CI
continues to own native application builds, tests, coverage, and distribution
checks. No application-wide reformat or speculative module split is required.
