# Working in GnuCash

This fork is a C/C++ and Scheme desktop application built with CMake, GTK,
Guile, and optional Python bindings. Read `HACKING`, `README`, and the nearest
test CMakeLists before editing. Preserve upstream conventions and licensing.

## Tasks and verification

`python3 util/dev.py --help` is the local task catalog. See
`doc/development.md` for prerequisites, arguments, and targeted tests.

- Configure before building; use `.build` or an out-of-source build directory.
- Build concurrency defaults to at most four jobs. `GNC_JOBS` overrides it.
- Run `test`, not bare CTest on an unbuilt tree: the `check` target first builds
  test prerequisites. Preserve existing CTest isolation and resource rules.
- Test new behavior through public interfaces. Reproduce a bug before fixing
  it, then retain the regression test. Keep test books and credentials synthetic.
- Run checks proportional to the change. Report missing dependencies, skipped
  tests, and unavailable platform checks. Tooling tests don't verify the app.
- Don't lower coverage or complexity thresholds to make a change pass. No new
  numeric floor is implied by adopting another project's conventions.

## Ownership and boundaries

See `doc/architecture.md` for the source map. Keep decisions in the engine or
feature core and I/O and GTK coordination at the boundary. Reuse the owner of a
domain rule rather than duplicating it in a dialog, importer, or report.
Split by responsibility, not arbitrary file size. Keep changes focused; don't
reformat unrelated code or modify `borrowed/` to satisfy a local style preference.

- Use existing exact numeric types for money; don't introduce floating-point
  conversions at financial boundaries.
- Treat XML, SQL, KVP, Scheme, and binding shapes as compatibility contracts.
  Identify readers and writers and test old fixtures before changing them.
- Preserve damaged user data. A failed load must not become an empty successful
  load that overwrites the original. Test interrupted and repeated migrations.
- Keep secrets out of books, logs, fixtures, and repository configuration.
- For online features, reuse `HttpTransport`, the secret store, and per-book
  storage in `gnucash/online/`. Offline tests must not submit returns or contact
  banks. Read that directory's README before changing its contracts.
- Keep GTK work on its owning thread and blocking work out of UI callbacks.
  Don't import Saggar's single-clock or Swift-specific rules into this app.
- Keep translatable strings translatable. Match local C/C++/Scheme formatting;
  `.editorconfig` supplies defaults, not a mandate for whole-tree rewrites.

## Delivery and shared work

Track work in existing GitHub issues when available. This fork had issues
disabled when these conventions were adopted; record scope in `doc/` when the
tracker is unavailable rather than changing repository settings implicitly.

Other agents may share the tree. Never switch branches, stash, discard someone
else's edits, or delete an index lock. Prefer the existing checkout for small
changes. Stage and inspect only your work, then commit each verified unit with
an imperative `Feature:`, `Bug:`, `Dependency:`, `Chore:`, or `Refactor:` message.
Use `git commit --only <paths>` only when those files are entirely yours.
Don't push, amend, or rewrite history without explicit authorization.

Preserve `NEWS` and the upstream release process; don't introduce a second
changelog. Update the architecture map when boundaries move. Write concise
sentence-case documentation and comments explaining non-obvious constraints.
Keep Fettle screenshots in `.fettle/shots/`, not in source or fixtures.
