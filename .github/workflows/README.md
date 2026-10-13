# CI workflows

- `developer-tools.yml` runs the portable task-runner tests on Ubuntu for pushes
  and pull requests. It doesn't build or test the application.
- `ci-tests.yml` builds and runs distribution checks on Ubuntu 22.04. Its
  separate Address Sanitizer job runs on `ubuntu-latest` and is allowed to fail.
- `ci-docker.yml` builds and tests in the Arch Linux container defined under
  `util/ci/actions/archlinux-test/`.
- `mac-tests.yaml` builds and tests on macOS using a prebuilt dependency tarball.
  See `util/ci/macos-ci-deps/README.md` for dependency packaging.
- `coverage.yml` builds coverage and programming documentation. The coverage
  job is allowed to fail. Pages publication is restricted to `gnucash/gnucash`.

Read individual job results before claiming verification. A passing portable
tooling job or an allowed failure doesn't establish that native tests passed.
