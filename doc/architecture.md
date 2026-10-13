# Source ownership

Use this map to find the existing owner before adding a second implementation.

| Area | Responsibility |
| --- | --- |
| `libgnucash/engine/` | Books, accounts, transactions, splits, numeric values, and core domain rules |
| `libgnucash/backend/` | XML, SQL, and DBI persistence and compatibility |
| `libgnucash/core-utils/` | Shared runtime utilities |
| `libgnucash/app-utils/` | Application helpers shared with Scheme |
| `gnucash/gnome/`, `gnucash/gnome-utils/` | GTK application orchestration and shared UI |
| `gnucash/gtkbuilder/`, `gnucash/ui/` | Dialog layouts and menu resources |
| `gnucash/register/` | Register model, ledger coordination, and register UI |
| `gnucash/import-export/` | Format-specific import and export adapters |
| `gnucash/report/` | Scheme reports, stylesheets, and report tests |
| `gnucash/online/` | Bank feeds, VAT features, HTTP, credentials, and associated UI |
| `bindings/` | Guile and optional Python interfaces |
| `common/test-core/`, `test-templates/` | Shared test support and examples |
| `borrowed/` | Vendored code; preserve its upstream provenance |
| `data/`, `po/` | Account templates, assets, and translations |

Tests generally live in each owner's `test/` directory (Python bindings use
`tests/`). CMake registers them with CTest. The top-level `testbuild` target
builds test prerequisites, and `check` depends on it before invoking CTest.

The online feature already separates `gnc-online-core` from its GTK library.
Its `HttpTransport` seam supports scripted offline responses; per-book KVP and
the credential store have separate owners. Preserve those boundaries when
adding providers or workflows. See `gnucash/online/README.md` for the details.
