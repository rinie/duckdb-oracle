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

## Secrets

A DuckDB secret keeps the credentials out of the connection string and the query history:

```sql
CREATE SECRET my_oracle (TYPE oracle, user 'scott', password 'tiger', connectString '//myhost:1521/MYSERVICE');
ATTACH '' AS mydb (TYPE oracle, SECRET my_oracle);
```

- `connectString` is required. `user` and `password` must be given together; leave both out for external authentication (for example an Oracle wallet)
- A secret named `__default_oracle` is used by every `ATTACH` that has an **empty** connection string and no `SECRET` option. An `ATTACH` with a connection string always uses that string
- `CREATE PERSISTENT SECRET` keeps the secret across sessions, in an unencrypted file in `~/.duckdb/stored_secrets`
- `duckdb_secrets()` shows the password as `redacted`, and connection errors show `user@connectString` without the password
- Secrets only work with `ATTACH`; the table functions below take the connection string itself

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
| `READ_ONLY` | | Standard DuckDB option (no value): refuse every statement that would change the database (see below) |

By default only the connected user's own schema is visible. To browse tables owned by someone else, use `SCHEMA 'OWNER'` or `PRIVILEGE_LEVEL 'all'`.

### READ_ONLY

```sql
ATTACH 'user/password@host:1521/service' AS mydb (TYPE oracle, READ_ONLY);
```

On a read-only attach DuckDB rejects every `INSERT`, `UPDATE`, `DELETE`, `MERGE` and DDL statement on it (`Cannot execute statement of type "CREATE" on database "mydb" which is attached in read-only mode!`) before anything reaches Oracle, and the extension starts each of its transactions with `SET TRANSACTION READ ONLY`.

The read-only flag belongs to the attached database. The table functions `oracle_execute()`, `oracle_query()`, `oracle_scan()` and `oracle_attach()` take their own connection string and ignore it; `oracle_execute()` can still change data. If writes must be impossible, connect with an Oracle account that only has `SELECT` rights.

### LOAD_COLUMNS

By default a schema listing only fetches table and view names, so attaching and browsing is fast even for large schemas. A table's columns are fetched the first time it is used. Anything that needs columns for all tables, such as `SHOW ALL TABLES`, `duckdb_columns()` and `information_schema.columns`, therefore only sees the tables used so far.

With `LOAD_COLUMNS true` the extension fetches the columns and primary/unique keys of every table and view in the schema with two bulk queries when the schema is first listed, so all of those show every table and view, including the columns of views. This makes the first listing slower on schemas with many tables or columns. Tables the bulk query cannot see are still listed by name and loaded on first use.

## Table functions

Besides `ATTACH`, the extension has table functions that take the connection string themselves (`'user/password@host:1521/service'`, see above). They open their own Oracle connection, so they do not see uncommitted changes of an attached database, and they do not use secrets or the `READ_ONLY` flag.

| Function | Description |
|----------|-------------|
| `oracle_query(conn, sql)` | Runs a `SELECT` and returns the result with the real column names and types (mapped like an attached table). The statement is described first, so a query without rows still returns its columns. Duplicate column names get `_1`, `_2`, ... A trailing semicolon is ignored. Statements without result columns are refused, use `oracle_execute` |
| `oracle_execute(conn, sql)` | Runs one statement (DML or DDL), commits, and returns `OK` |
| `oracle_scan(conn, schema, table)` | Reads one table |
| `oracle_scan_pushdown(conn, schema, table)` | Reads one table and sends `WHERE` filters to Oracle (a filter that cannot be written as Oracle SQL raises an error) |
| `oracle_attach(conn, [source_schema, sink_schema, overwrite, filter_pushdown])` | Creates a DuckDB view with `oracle_scan` (or `oracle_scan_pushdown`) for every table of one schema, by default the connected user's schema into `main` |
| `oracle_clear_cache()` | Forgets the table and column lists that attached databases have cached |

```sql
-- real names and types, usable in joins like any other table
SELECT * FROM oracle_query('user/password@host:1521/service',
    'SELECT department_id, count(*) AS employees FROM hr.employees GROUP BY department_id');

-- DDL and DML (committed immediately)
SELECT * FROM oracle_execute('user/password@host:1521/service', 'CREATE INDEX emp_name_ix ON hr.employees(last_name)');
```

Oracle's unconstrained `NUMBER` (also the result of `COUNT(*)` or `1 + 1`) comes back as `DOUBLE`. Use `CAST(... AS NUMBER(18))` in the Oracle query to get an integer.

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

### Bulk loads

`INSERT`, `UPDATE` and `DELETE` (also through `MERGE` and `CREATE TABLE AS`) send each chunk of up to 2048 rows to Oracle as one array-bound statement, so the number of network round trips depends on the number of chunks, not rows. On Oracle 19c, 100000 rows are inserted in about 3 seconds:

```sql
INSERT INTO mydb.hr.big_table SELECT * FROM read_parquet('data.parquet');
CREATE TABLE mydb.hr.copy_of_csv AS SELECT * FROM read_csv('data.csv');
```

Text and `BLOB` values of more than 32767 bytes are not array-bound: a chunk that contains one is sent as one statement per row, which is slower but works. `ora_debug_show_queries` prints `-- N rows batched` for array-bound statements.

### How row identity works (ROWID)

DuckDB evaluates the `SET` expressions and `WHERE` filter on its side and then hands each affected row — along with its **computed new values** — to the Oracle extension one row at a time. The extension must therefore identify each individual Oracle row precisely so it can write back the correct value.

The extension uses Oracle's `ROWID` pseudo-column for this:

1. **Scan phase** — the Oracle scan includes `ROWID` alongside the projected columns. Each row's 18-character ROWID string is stored in a per-transaction registry; a BIGINT index is written into DuckDB's internal row-ID slot.
2. **Write phase** — for each row in the result the extension looks up the ROWID string and updates exactly that row. This is one array-bound statement per chunk of up to 2048 rows (see above):
   ```sql
   UPDATE schema.table SET col = :1 WHERE ROWID = :2
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
