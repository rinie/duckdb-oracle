# duckdb-oracle

A DuckDB extension that attaches Oracle databases, modelled after `duckdb-postgres` but using Oracle via [ODPI-C](https://oracle.github.io/odpi/).

## Requirements

- Oracle Instant Client (or a full Oracle client installation) in your `PATH` / `LD_LIBRARY_PATH` / `DYLD_LIBRARY_PATH`
- The `oracle.duckdb_extension` binary for your platform

## Quick start

```sql
-- Load the extension
LOAD 'oracle';

-- Attach an Oracle database (connect string follows Oracle EZConnect syntax)
ATTACH 'user/password@host:1521/service' AS mydb (TYPE oracle);

-- Optional: attach only a specific schema
ATTACH 'user/password@host:1521/service' AS mydb (TYPE oracle, SCHEMA 'HR');

-- Browse schemas and tables
SHOW ALL TABLES;
SELECT * FROM information_schema.tables WHERE table_catalog = 'mydb';

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

## How it works

- **Schema browser** — on first `ATTACH`, a lightweight `all_tables UNION ALL all_views` query enumerates table/view names per schema. No column data is fetched yet, so the UI loads instantly regardless of schema size.
- **Lazy column loading** — the first time a table is referenced in a query, `ReloadEntry` fires two Oracle round-trips (columns + constraints) to fully populate the entry. The result is cached for the session.
- **USER_* privilege optimization** — when querying the schema of the connected user, the extension uses `user_tab_columns`, `user_tables`, etc. instead of `all_*` views. These skip Oracle's privilege-check layer and are significantly faster on large databases.
- **VIEW detection** — views and base tables are distinguished via a `UNION ALL` (inner-join `all_tables` OR inner-join `all_views`). DuckDB catalog entries are tagged with `CatalogType::VIEW_ENTRY` accordingly.

## Building from source

```bash
git clone --recurse-submodules https://github.com/rinie/duckdb-oracle
cd duckdb-oracle
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The compiled extension is at `build/release/oracle.duckdb_extension`.
