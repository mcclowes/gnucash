# Commands

- Configure: `python3 util/dev.py configure` #quick
- Build: `python3 util/dev.py build` #companion
- Run: `python3 util/dev.py build && "${GNC_BUILD_DIR:-.build}/bin/gnucash"` #quick
- Test: `python3 util/dev.py test` #background
- Tooling tests: `python3 -m unittest discover -s util -p test_dev.py` #quick
