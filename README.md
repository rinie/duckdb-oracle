# duckdb-oracle

A DuckDB extension that attaches Oracle databases, modelled after `duckdb-postgres` but using Oracle via [ODPI-C](https://oracle.github.io/odpi/).

## Requirements

- Oracle Client **19c or newer** (Instant Client or a full Oracle client installation) in your `PATH` / `LD_LIBRARY_PATH` / `DYLD_LIBRARY_PATH`. On Linux and macOS the unversioned `libclntsh.so` / `libclntsh.dylib` symlink must exist in that directory (ODPI-C 26 no longer falls back to the versioned library name)
- The `oracle.duckdb_extension` binary for your platform

## Quick start

```sql
-- Load the extension
LOAD 'oracle';

-- Attach an Oracle database (connect string follows Oracle EZConnect syntax)
ATTACH 'user/password@host:1521/service' AS mydb (TYPE oracle);

-- Optional: attach only a specific schema
ATTACH 'user/password@host:1521/service' AS mydb (TYPE oracle, SCHEMA 'HR');

-- Browse schemas and tables (names only; a table's columns are loaded when it is first used)
SELECT schema_name, table_name FROM duckdb_tables() WHERE database_name = 'mydb';

-- SHOW ALL TABLES also lists columns, so it only shows tables whose columns are loaded.
-- Attach with LOAD_COLUMNS to have every table listed (see "ATTACH options").
ATTACH 'user/password@host:1521/service' AS mydb2 (TYPE oracle, LOAD_COLUMNS true);
SHOW ALL TABLES;

-- Query Oracle tables directly
SELECT * FROM mydb.hr.employees LIMIT 10;

-- Cross-database joins (DuckDB local + Oracle remote)
SELECT e.employee_id, e.last_name, d.department_name
FROM mydb.hr.employees e
JOIN mydb.hr.departments d ON e.department_id = d.department_id;
```

## Connection string formats

```
-- Basic
user/password@host/service
user/password@host:1521/service

-- With TNS alias (requires tnsnames.ora or ORACLE_HOME set)
user/password@MYDB

-- Full EZConnect Plus (Oracle 19c+)
user/password@//host:1521/service?connect_timeout=10
```

## ATTACH options

```sql
ATTACH 'user/password@host:1521/service' AS mydb (TYPE oracle, SCHEMA 'HR', LOAD_COLUMNS true);
```

| Option | Default | Description |
|--------|---------|-------------|
| `SECRET` | | Name of a DuckDB secret holding the credentials |
| `SCHEMA` | | Load only this schema (Oracle owner) |
| `PRIVILEGE_LEVEL` | `user` | Which data dictionary views are used. `user`: only the connected user's own schema (`USER_*` views). `all`: every schema the user can see (`ALL_*` views). `dba`: `DBA_*` views, requires the DBA role |
| `ISOLATION_LEVEL` | `read committed` | `read committed` or `serializable` |
| `LOAD_COLUMNS` | `false` | Fetch the columns and key constraints of every table when a schema is listed (see below) |

By default only the connected user's own schema is visible. To browse tables owned by someone else, use `SCHEMA 'OWNER'` or `PRIVILEGE_LEVEL 'all'`.

### LOAD_COLUMNS

By default a schema listing only fetches table and view names, so attaching and browsing is fast even for large schemas. A table's columns are fetched the first time it is used. Anything that needs columns for all tables, such as `SHOW ALL TABLES`, `duckdb_columns()` and `information_schema.columns`, therefore only sees the tables used so far.

With `LOAD_COLUMNS true` the extension fetches the columns and primary/unique keys of every table and view in the schema with two bulk queries when the schema is first listed, so all of those show every table and view, including the columns of views. This makes the first listing slower on schemas with many tables or columns. Tables the bulk query cannot see are still listed by name and loaded on first use.

## Extension options

| Option | Default | Description |
|--------|---------|-------------|
| `ora_connection_limit` | 64 | Maximum concurrent Oracle connections in the pool |
| `ora_connection_cache` | `true` | Keep connections alive between queries |
| `ora_debug_show_queries` | `false` | Print every Oracle SQL statement to stdout |

```sql
-- Show each Oracle query that gets executed
SET ora_debug_show_queries = true;
```

For the data dictionary queries (table names, columns, constraints) it also prints the number of rows and the time each took, which shows where time goes on a large schema.

## Debugging with ODPI-C trace logging

For low-level diagnostics (connection issues, protocol errors, bind parameters), enable ODPI-C debug output by setting the environment variable **before** starting DuckDB:

```bat
:: Windows
set DPI_DEBUG_LEVEL=16
duckdb

:: PowerShell
$env:DPI_DEBUG_LEVEL = 16
duckdb
```

```bash
# Linux / macOS
export DPI_DEBUG_LEVEL=16
duckdb
```

`DPI_DEBUG_LEVEL` is a bitmask. Common values:

| Value | What is logged |
|-------|----------------|
| `1` | Errors |
| `4` | SQL statements sent to Oracle |
| `8` | Bind variable values |
| `16` | All of the above + ODPI-C function calls |
| `64` | Full trace including memory allocations |

The trace is written to stderr. Redirect it to a file with `2>odpi.log`.

## DML — UPDATE and DELETE

`UPDATE` and `DELETE` are supported and work as expected from DuckDB:

```sql
UPDATE mydb.hr.employees SET salary = 5000  WHERE employee_id = 100;
UPDATE mydb.hr.employees SET salary = salary * 1.1 WHERE department_id = 10;
DELETE FROM mydb.hr.employees WHERE employee_id = 100;
```

### How row identity works (ROWID)

DuckDB evaluates the `SET` expressions and `WHERE` filter on its side and then hands each affected row — along with its **computed new values** — to the Oracle extension one row at a time. The extension must therefore identify each individual Oracle row precisely so it can write back the correct value.

The extension uses Oracle's `ROWID` pseudo-column for this:

1. **Scan phase** — the Oracle scan includes `ROWID` alongside the projected columns. Each row's 18-character ROWID string is stored in a per-transaction registry; a BIGINT index is written into DuckDB's internal row-ID slot.
2. **Write phase** — for each row in the result the extension looks up the ROWID string and executes:
   ```sql
   UPDATE schema.table SET col = <value> WHERE ROWID = 'AAABozAAEAAAAGSAAA'
   ```

Using `ROWID` is necessary — not just a convenience — because DuckDB evaluates expressions like `salary * 1.1` itself before calling the extension. At write-back time the extension only receives the already-computed numeric result (e.g. `5500.00`), not the original expression. A WHERE-clause-only approach would reapply the same WHERE to every row in the batch, meaning all rows would get the value that was computed for the *last* row scanned. `ROWID` ensures each computed value reaches exactly the row it was read from, matching what Oracle's own `UPDATE … SET col = col * 1.1` would produce.

> **Note:** ROWID targets the physical row at scan time. If another session moves the row (e.g. via a table reorganisation or `ALTER TABLE … MOVE`) between the scan and the write-back, the update will silently skip that row. For normal OLTP workloads this is not a concern.

## How it works

- **Schema browser** — on first `ATTACH`, a lightweight `all_tables UNION ALL all_views` query enumerates table/view names per schema. No column data is fetched yet, so the UI loads instantly regardless of schema size.
- **Lazy column loading** — the first time a table is referenced in a query, `ReloadEntry` fires two Oracle round-trips (columns + constraints) to fully populate the entry. The result is cached for the session. With the `LOAD_COLUMNS` ATTACH option the columns of the whole schema are fetched up front instead (two bulk queries).
- **USER_* privilege optimization** — when querying the schema of the connected user, the extension uses `user_tab_columns`, `user_tables`, etc. instead of `all_*` views. These skip Oracle's privilege-check layer and are significantly faster on large databases.
- **VIEW detection** — views and base tables are distinguished via a `UNION ALL` (inner-join `all_tables` OR inner-join `all_views`). DuckDB catalog entries are tagged with `CatalogType::VIEW_ENTRY` accordingly.

## Building from source

This repository is an out-of-tree DuckDB extension: it has no submodules and cannot be configured on its own (`cmake -B build` inside this repo fails with `Unknown CMake command "build_loadable_extension"`). Build it through a DuckDB checkout instead, and pass this repo's `extension_config.cmake` to DuckDB's build.

The DuckDB checkout **must match the version in [`duckdb_version`](duckdb_version)**, because extensions are built against DuckDB's unstable C++ API and only load into the exact DuckDB version they were built for.

Linux / macOS:

```bash
git clone --depth 1 --branch v2.0.0 https://github.com/duckdb/duckdb
git clone --depth 1 https://github.com/rinie/duckdb-oracle

cmake -S duckdb -B build -DCMAKE_BUILD_TYPE=Release \
  -DDUCKDB_EXTENSION_CONFIGS="$PWD/duckdb-oracle/extension_config.cmake" \
  -DDUCKDB_EXPLICIT_VERSION=v2.0.0 \
  -DBUILD_COMPLETE_EXTENSION_SET=OFF -DBUILD_UNITTESTS=OFF
cmake --build build --config Release --target oracle_loadable_extension
```

Windows (Visual Studio 2022, from a Developer Command Prompt):

```bat
git clone --depth 1 --branch v2.0.0 https://github.com/duckdb/duckdb
git clone --depth 1 https://github.com/rinie/duckdb-oracle

cmake -S duckdb -B build -G "Visual Studio 17 2022" -A x64 ^
  -DDUCKDB_EXTENSION_CONFIGS=%CD%\duckdb-oracle\extension_config.cmake ^
  -DDUCKDB_EXPLICIT_VERSION=v2.0.0 ^
  -DBUILD_COMPLETE_EXTENSION_SET=OFF -DBUILD_UNITTESTS=OFF
cmake --build build --config Release --target oracle_loadable_extension
```

Since DuckDB 2.0 the version is no longer derived from the git tag: without `-DDUCKDB_EXPLICIT_VERSION=<tag>` the build is stamped `v2.0.0-dev<N>`, and an official DuckDB release refuses to load the extension.

The compiled extension is at `build/extension/oracle/oracle.duckdb_extension`. Add `-DBUILD_SHELL=ON` and build the `shell` target if you also want a `duckdb` CLI to test it with (`LOAD '<path>/oracle.duckdb_extension';`, started with `duckdb -unsigned`).

ODPI-C is downloaded automatically by CMake. The Oracle client libraries are only needed at runtime, not to build.

> The Linux/macOS commands follow the same CMake flow as the Windows build but have not been verified on those platforms yet; please open an issue if they fail.
