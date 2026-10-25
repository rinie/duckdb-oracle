-- Scratch test for batched INSERT / UPDATE / DELETE (array binding).
-- Paste into the DuckDB CLI (started with -unsigned) with the Oracle connection already attached as `ora`:
--   LOAD '<path>/oracle.duckdb_extension';
--   ATTACH 'user/password@host:1521/service' AS ora (TYPE oracle);
-- It only touches the table CLAUDE_BULK in the connected user's own schema and drops it at the end.

DROP TABLE IF EXISTS ora.claude_bulk;
CREATE TABLE ora.claude_bulk (
    id BIGINT, s VARCHAR, dec DECIMAL(12,2), dbl DOUBLE, d DATE, ts TIMESTAMP,
    tstz TIMESTAMP WITH TIME ZONE, iv INTERVAL, flag BOOLEAN, bl BLOB, big VARCHAR
);

-- 1. Correctness of every type, NULLs, an empty string, a 5000 character CLOB and a 10000 byte BLOB.
--    Expected: three rows; row 2 all NULL; row 3 has s NULL (Oracle stores '' as NULL).
SET ora_debug_show_queries = true;   -- the INSERT should print "... VALUES(:1, :2, ...)  -- 3 rows batched"
INSERT INTO ora.claude_bulk VALUES
 (1, 'hello', 12345.67, 0.1::DOUBLE + 0.2::DOUBLE, DATE '2026-10-25', TIMESTAMP '2026-10-25 12:34:56.123456',
  TIMESTAMPTZ '2026-10-25 12:34:56+02', INTERVAL '3 days 04:05:06.789', true, repeat('AB', 5000)::BLOB, repeat('x', 5000)),
 (2, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL),
 (3, '', -0.01, 1.5e100, DATE '0001-01-01', TIMESTAMP '1999-12-31 23:59:59', TIMESTAMPTZ '2026-01-01 00:00:00+00',
  INTERVAL '-1 day -00:00:01', false, '\xDE\xAD\xBE\xEF'::BLOB, 'unicode äöü € ''quoted''');
SELECT id, s, dec, dbl, dbl = 0.1::DOUBLE + 0.2::DOUBLE AS dbl_exact, d, ts, tstz, iv, flag, length(bl) AS bl_len, length(big) AS big_len
FROM ora.claude_bulk ORDER BY id;

-- 2. A value that cannot be bound (40000 characters) falls back to literals for that chunk and still works.
--    Expected: big_len 40000, and no "rows batched" line for this statement.
INSERT INTO ora.claude_bulk(id, big) VALUES (4, repeat('y', 40000));
SELECT id, length(big) AS big_len FROM ora.claude_bulk WHERE id = 4;
DELETE FROM ora.claude_bulk WHERE id = 4;

-- 3. A failing batch must fail as a whole. Expected: an ORA-01438 (value larger than precision) error and still 3 rows.
INSERT INTO ora.claude_bulk(id, dec) VALUES (10, 1.00), (11, 99999999999.99);
SELECT count(*) AS rows_left FROM ora.claude_bulk;   -- 3

SET ora_debug_show_queries = false;
.timer on

-- 4. Speed. 100000 rows used to take one network round trip per row; now one per 2048 rows.
INSERT INTO ora.claude_bulk(id, s, dec, dbl, d, ts)
SELECT 1000 + i, 'row ' || i, i / 100.0, i * 1.5, DATE '2020-01-01' + (i % 1000)::INTEGER,
       TIMESTAMP '2020-01-01' + to_seconds(i)
FROM range(100000) t(i);
SELECT count(*) AS total, min(id), max(id) FROM ora.claude_bulk;   -- 100003, 1, 101999

-- 5. UPDATE of 50000 rows (one array-bound UPDATE per chunk).
UPDATE ora.claude_bulk SET s = 'upd ' || id, dec = dec + 1 WHERE id >= 1000 AND id < 51000;
SELECT s, dec FROM ora.claude_bulk WHERE id IN (1000, 1001, 50999, 51000) ORDER BY id;
-- Expected: 'upd 1000' 1.00, 'upd 1001' 1.01, 'upd 50999' 510.99, 'row 50000' 500.00

-- 6. DELETE of 10000 rows.
DELETE FROM ora.claude_bulk WHERE id >= 91000;
SELECT count(*) AS total FROM ora.claude_bulk;   -- 90003

-- 7. MERGE (uses the same UPDATE / INSERT operators).
MERGE INTO ora.claude_bulk t
USING (SELECT 1000 AS id, 'merged' AS s UNION ALL SELECT 500000, 'new') src ON t.id = src.id
WHEN MATCHED THEN UPDATE SET s = src.s
WHEN NOT MATCHED THEN INSERT (id, s) VALUES (src.id, src.s);
SELECT id, s FROM ora.claude_bulk WHERE id IN (1000, 500000) ORDER BY id;   -- 1000 merged, 500000 new

-- 8. Rollback and commit of a batch.
BEGIN;
INSERT INTO ora.claude_bulk(id) SELECT 700000 + i FROM range(5000) t(i);
ROLLBACK;
SELECT count(*) AS should_be_zero FROM ora.claude_bulk WHERE id >= 700000;
BEGIN;
INSERT INTO ora.claude_bulk(id) SELECT 700000 + i FROM range(5000) t(i);
COMMIT;
SELECT count(*) AS should_be_5000 FROM ora.claude_bulk WHERE id >= 700000;

.timer off
DROP TABLE ora.claude_bulk;
