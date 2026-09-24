# Third-Party Libraries

Vendored dependencies of the catalog library.

| Library | Version | License | Source |
|---------|---------|---------|--------|
| sqlite3.c / sqlite3.h | 3.53.3 (amalgamation) | Public Domain | https://sqlite.org/download.html |

SQLite is compiled as its own target (`catalog_sqlite3`) with `SQLITE_ENABLE_FTS5`,
outside the strict nkido warning flags. To upgrade: replace both files with a
newer amalgamation and update this table.
