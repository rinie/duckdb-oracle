# Changelog

## v2.0.3 (2026-10-25) — DuckDB 2.0.0

### Fixes

- `oracle_query` returns the real column names and types (numbers, dates, timestamps with time zone, intervals, CLOB, RAW, ...) instead of `column0, column1, ...` as VARCHAR, and keeps the columns when the query returns no rows
- `oracle_query` sends the statement once: it used to run at bind time and again at execution, so a DML statement ran twice and was never committed. It now accepts queries only (use `oracle_execute` for DML and DDL) and ignores a trailing semicolon
- `INSERT INTO ... SELECT ... FROM oracle_query(...)` into an attached Oracle database no longer reads the scan data as the wrong type

---

## v2.0.2 (2026-10-25) — DuckDB 2.0.0

### Performance

- `INSERT`, `UPDATE` and `DELETE` (and therefore `MERGE` and `CREATE TABLE AS`) send each chunk of up to 2048 rows to Oracle as one array-bound statement instead of one statement per row. Checked on Oracle 19c: 100000 rows are inserted in about 3 seconds, 50000 rows updated in about 2.6 seconds and 10000 rows deleted in about 1 second
- Values are sent as bind variables instead of SQL text, so `BLOB` values up to 32767 bytes can be inserted (the limit was 2000 bytes) and numbers are converted independently of the NLS settings
- A chunk with a value that cannot be bound (text or bytes above 32767 bytes, infinite dates, `NaN`/infinite floats, nested types) is sent with the previous literal statements, so these keep working
- `ora_debug_show_queries` prints `-- N rows batched` for array-bound statements

### Documentation

- README: document the `READ_ONLY` ATTACH option (checked on Oracle 19c: reads work, `CREATE TABLE` is refused) and that the `oracle_*` table functions ignore it

---

## v2.0.1 (2026-10-25) — DuckDB 2.0.0

### Fixes (found with a scratch-table test on Oracle 19c)

- `CLOB`/`NCLOB` columns were read as empty strings (and `BLOB` columns as empty blobs): they are now fetched as text/bytes, also by `oracle_query`
- Selecting from a table right after `CREATE TABLE` in the same session raised an internal DuckDB error (`Attempted to access index 0 within vector of size 0`) that invalidated the database. The new table is now read back from Oracle after creation
- A schema listing no longer replaces an already loaded table by a name-only entry (which made `duckdb_columns()` / `SHOW ALL TABLES` lose tables that had just been used)
- `INSERT`/`UPDATE` of `TIMESTAMP WITH TIME ZONE` (`ORA-01843`) and `INTERVAL DAY TO SECOND` (`ORA-01867`) values now use proper Oracle literals instead of strings that depend on NLS settings; numbers are sent as numeric literals, not strings
- Text longer than 4000 bytes is now inserted as a concatenation of `TO_CLOB(...)` pieces instead of failing with `ORA-01704` (checked with a 5000 character value)
- `MERGE` with an `UPDATE`/`DELETE` action raised an internal DuckDB error (`Attempted to access index 100 within vector of size 4`) that invalidated the database, because the rows were handed to the UPDATE operator in the wrong layout. MERGE now uses DuckDB's generic merge planning on top of the regular INSERT/UPDATE/DELETE operators
- `ora_debug_show_queries` now also prints the SQL of table scans (including the pushed-down `WHERE` clause)

### Dependencies

- Update [ODPI-C](https://github.com/oracle/odpi) from v5.3.0 to **v26.0.0** (ODPI-C now uses the two-digit release year as its major version)

### Breaking changes (from ODPI-C)

- Oracle Client libraries older than **19c** are no longer supported (dropped in ODPI-C 6.0)
- On Linux and macOS the unversioned `libclntsh.so` / `libclntsh.dylib` symlink is now mandatory; ODPI-C 26 no longer falls back to the versioned library name

### Performance

- Data dictionary queries (table names, columns, constraints) now fetch 2000 rows per network round trip instead of ODPI-C's default of 100, which matters most for the bulk queries of `LOAD_COLUMNS` and for remote databases
- The `LOAD_COLUMNS` column query reads the column dictionary view once and takes the table/view distinction from the objects view, instead of reading it twice. If it fails for any reason the previous (per-table) query is used instead
- `ora_debug_show_queries` also prints the row count and time of data dictionary queries

### Fixes

- With `LOAD_COLUMNS`, views are now listed with their columns by `SHOW ALL TABLES`, `duckdb_columns()` and `information_schema.columns`; before they were missing from `SHOW ALL TABLES`. `duckdb_tables()` still lists base tables only

### Features

- New `LOAD_COLUMNS` ATTACH option (default `false`): fetches the columns and key constraints of every table with two bulk queries when a schema is first listed, so `SHOW ALL TABLES`, `duckdb_columns()` and `information_schema.columns` show all tables instead of only those used so far

### Documentation

- README: document all ATTACH options (`SECRET`, `SCHEMA`, `PRIVILEGE_LEVEL`, `ISOLATION_LEVEL`, `LOAD_COLUMNS`) and that the default `PRIVILEGE_LEVEL` only shows the connected user's own schema

---

## v2.0.0 (2026-10-24) — DuckDB 2.0.0

Port to DuckDB **v2.0.0**. The extension builds against the unstable C++ API, which changed substantially in 2.0; DuckDB 2.0's new stable C API cannot express storage extensions or catalogs, so it is not used.

### Changes

- Names are `Identifier`s throughout the catalog, DDL (`CREATE`/`DROP`/`ALTER`) and DML code
- Table entries own their column list; table functions use the new `FunctionSignature` and bind callbacks
- Filter pushdown (`oracle_scan_pushdown`, `oracle_attach(filter_pushdown := true)`) was rewritten for DuckDB 2.0 expression filters. Supported: comparisons, `IN`, `IS [NOT] NULL`, `AND`/`OR` on integer, decimal, float, `VARCHAR`, `DATE`, `TIMESTAMP` and `TIMESTAMP WITH TIME ZONE` constants
- A pushed-down filter that cannot be rendered as Oracle SQL now raises an error instead of being silently dropped. Use `oracle_scan` (or `oracle_attach` without `filter_pushdown`) to avoid pushdown
- `MERGE` runs multiple actions one after the other

### Build

- `DONT_LINK` was removed from `extension_config.cmake`
- Build with `-DDUCKDB_EXPLICIT_VERSION=v2.0.0`; see the README

### Dependencies

- [ODPI-C v5.3.0](https://github.com/oracle/odpi) — Oracle Database Programming Interface for C
- [DuckDB v2.0.0](https://github.com/duckdb/duckdb/releases/tag/v2.0.0)

---

## v1.5.6 (2026-10-16) — DuckDB 1.5.6

Bump to DuckDB **v1.5.6**.

### Dependencies

- [ODPI-C v5.3.0](https://github.com/oracle/odpi) — Oracle Database Programming Interface for C
- [DuckDB v1.5.6](https://github.com/duckdb/duckdb/releases/tag/v1.5.6)

---

## v1.5.5 (2026-07-24) — DuckDB 1.5.5

Bump to DuckDB **v1.5.5**.

### Dependencies

- [ODPI-C v5.3.0](https://github.com/oracle/odpi) — Oracle Database Programming Interface for C
- [DuckDB v1.5.5](https://github.com/duckdb/duckdb/releases/tag/v1.5.5)

---

## v1.5.4 (2026-06-19) — DuckDB 1.5.4

Bump to DuckDB **v1.5.4**.

### Fixes

- Fix `TIMESTAMP WITH TIME ZONE` type being truncated to plain `TIMESTAMP`
- Fix `num_rows` returning hardcoded 0 for base tables (now reads from `user_tables`)
- Fix `TIMESTAMP_TZ` filter pushdown to emit correct Oracle `AT TIME ZONE 'UTC'` literal

### Dependencies

- [ODPI-C v5.3.0](https://github.com/oracle/odpi) — Oracle Database Programming Interface for C
- [DuckDB v1.5.4](https://github.com/duckdb/duckdb/releases/tag/v1.5.4)

---

## v1.5.3 (2026-05-23) — DuckDB 1.5.3

Bump to DuckDB **v1.5.3**.

### Dependencies

- [ODPI-C v5.3.0](https://github.com/oracle/odpi) — Oracle Database Programming Interface for C
- [DuckDB v1.5.3](https://github.com/duckdb/duckdb/releases/tag/v1.5.3)

---

## v1.4.4 (2026-03-01) — DuckDB 1.4.4 LTS

Initial LTS release of the `oracle` DuckDB extension, built against DuckDB **v1.4.4 LTS**.

### Features

- Connect to Oracle databases via DuckDB's `ATTACH` syntax
- Full Oracle table scanning using ODPI-C v5.3.0
- Filter pushdown for efficient query execution
- Data type conversion between Oracle and DuckDB types
- Secret management for Oracle credentials
- Oracle storage extension interface
- Query logging and debugging support
- Connection pooling

### Supported Platforms

- Linux x86_64
- Linux ARM64
- macOS x86_64
- macOS ARM64 (Apple Silicon)
- Windows x86_64

### Usage

#### Basic connection

```sql
-- Load the extension
LOAD oracle;

-- Attach an Oracle database
ATTACH 'user/password@host:1521/service' AS oracle_db (TYPE oracle);

-- Query Oracle tables and views
SELECT * FROM oracle_db.my_table LIMIT 10;
```

#### Using secrets (recommended)

Secrets keep credentials out of connection strings and query history.

```sql
-- Create a named secret
CREATE SECRET my_oracle_secret (
    TYPE oracle,
    user 'scott',
    password 'tiger',
    connectString '//myhost:1521/MYSERVICE'
);

-- Attach using the secret
ATTACH '' AS oracle_db (TYPE oracle, SECRET my_oracle_secret);
```

#### Default secret (auto-applied)

A secret named `__default_oracle` is picked up automatically for any Oracle
`ATTACH` that does not specify a `SECRET` option:

```sql
CREATE SECRET __default_oracle (
    TYPE oracle,
    user 'scott',
    password 'tiger',
    connectString '//myhost:1521/MYSERVICE'
);

-- No SECRET= needed
ATTACH '' AS oracle_db (TYPE oracle);
```

#### Persistent secrets

```sql
-- Survives DuckDB restarts
CREATE PERSISTENT SECRET my_oracle_secret (
    TYPE oracle,
    user 'scott',
    password 'tiger',
    connectString '//myhost:1521/MYSERVICE'
);
```

### Dependencies

- [ODPI-C v5.3.0](https://github.com/oracle/odpi) — Oracle Database Programming Interface for C
- [DuckDB v1.4.4](https://github.com/duckdb/duckdb/releases/tag/v1.4.4) — LTS release
