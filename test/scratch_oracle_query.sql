-- Scratch test for oracle_query (real column names and types, one execution) and oracle_execute.
-- Paste into the DuckDB CLI (started with -unsigned) after LOAD '<path>/oracle.duckdb_extension';
-- Part 6 also needs the database attached as `ora`: ATTACH 'user/password@host:1521/service' AS ora (TYPE oracle);
-- It creates the tables CLAUDE_Q and CLAUDE_Q2 in the connected user's schema and drops them at the end.

SET VARIABLE conn = 'user/password@host:1521/service';   -- <-- edit

-- 1. oracle_execute runs DDL and DML and commits. Expected: 'OK' three times.
SELECT * FROM oracle_execute(getvariable('conn'), 'CREATE TABLE claude_q (id NUMBER(10), amount NUMBER(12,2), anynum NUMBER, name VARCHAR2(50), note CLOB, raw_col RAW(16), d DATE, ts TIMESTAMP(6), tstz TIMESTAMP(6) WITH TIME ZONE, iv INTERVAL DAY(2) TO SECOND(6), bd BINARY_DOUBLE)');
SELECT * FROM oracle_execute(getvariable('conn'), 'INSERT INTO claude_q VALUES (1, 12345.67, 1.5, ''hello'', ''a clob'', HEXTORAW(''DEADBEEF''), DATE ''2026-10-25'', TIMESTAMP ''2026-10-25 12:34:56.123456'', TIMESTAMP ''2026-10-25 12:34:56.123456 +02:00'', INTERVAL ''3 04:05:06.789'' DAY TO SECOND, 0.1)');
SELECT * FROM oracle_execute(getvariable('conn'), 'INSERT INTO claude_q (id) VALUES (2)');

-- 2. Real column names and types (a trailing semicolon is accepted).
--    Expected: ID BIGINT, AMOUNT DECIMAL(12,2), ANYNUM DOUBLE, NAME VARCHAR, NOTE VARCHAR, RAW_COL BLOB, D TIMESTAMP,
--    TS TIMESTAMP, TSTZ TIMESTAMP WITH TIME ZONE, IV INTERVAL, BD DOUBLE.
DESCRIBE SELECT * FROM oracle_query(getvariable('conn'), 'SELECT * FROM claude_q;');
--    Expected: row 1 = 1, 12345.67, 1.5, hello, 'a clob', DEADBEEF, 2026-10-25 00:00:00, 2026-10-25 12:34:56.123456,
--    2026-10-25 10:34:56.123456+00 (shown in your session time zone), 3 days 04:05:06.789, 0.1; row 2 = 2 and NULLs.
SELECT * FROM oracle_query(getvariable('conn'), 'SELECT * FROM claude_q ORDER BY id');

-- 3. No rows still gives the columns. Expected: columns ID and NAME, 0 rows.
SELECT * FROM oracle_query(getvariable('conn'), 'SELECT id, name FROM claude_q WHERE 1 = 0');

-- 4. Expressions, duplicate names and ROWID. Expected: columns named after the expression, ID and ID_1 (row 2.0, 1, 1) and RID (18 characters), ID.
SELECT * FROM oracle_query(getvariable('conn'), 'SELECT 1 + 1, id, id FROM claude_q WHERE id = 1');
SELECT length(rid) AS rid_length, id FROM oracle_query(getvariable('conn'), 'SELECT ROWID AS rid, id FROM claude_q ORDER BY id');

-- 5. A statement that returns no columns is refused (and not run). Expected: an error mentioning oracle_execute, then 2 rows.
SELECT * FROM oracle_query(getvariable('conn'), 'DELETE FROM claude_q');
SELECT count(*) AS rows_left FROM oracle_query(getvariable('conn'), 'SELECT * FROM claude_q');   -- 2

-- 6. The query is sent once (it used to run at bind and again at execution). Expected: the SELECT is printed once.
SET ora_debug_show_queries = true;
SELECT count(*) FROM oracle_query(getvariable('conn'), 'SELECT * FROM claude_q');
SET ora_debug_show_queries = false;

-- 7. Joins with DuckDB data, and writing the result into the attached database (needs `ora`).
--    Expected: one row (1, one, hello); then CLAUDE_Q2 holds 2 rows.
SELECT q.id, r.label, q.name FROM oracle_query(getvariable('conn'), 'SELECT id, name FROM claude_q') q
JOIN (VALUES (1, 'one')) r(id, label) ON q.id = r.id;
CREATE TABLE ora.claude_q2 AS SELECT id, name FROM oracle_query(getvariable('conn'), 'SELECT id, name FROM claude_q');
SELECT count(*) AS rows_copied FROM ora.claude_q2;   -- 2

-- 8. Cleanup.
DROP TABLE ora.claude_q2;
SELECT * FROM oracle_execute(getvariable('conn'), 'DROP TABLE claude_q');
