# Changelog

## Unreleased

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
