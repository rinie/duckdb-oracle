-- Scratch test for Oracle secrets (validation, default secret, error messages).
-- Paste into the DuckDB CLI (started with -unsigned) after LOAD '<path>/oracle.duckdb_extension';
-- Replace myuser, mypassword and myservice with your own credentials throughout (the first two secrets must be valid). Nothing is created in Oracle.

-- 1. Complete named secret works. Expected: a table count above 0.
CREATE OR REPLACE SECRET claude_ok (TYPE oracle, user 'myuser', password 'mypassword', connectString 'myservice');
ATTACH '' AS ora_ok (TYPE oracle, SECRET claude_ok);
SELECT count(*) AS tables_visible FROM duckdb_tables() WHERE database_name = 'ora_ok';
DETACH ora_ok;

-- 2. Incomplete secrets are refused at ATTACH.
CREATE OR REPLACE SECRET claude_nopw (TYPE oracle, user 'myuser', connectString 'myservice');
ATTACH '' AS ora_x (TYPE oracle, SECRET claude_nopw);      -- expect: must have both user and password, or neither
CREATE OR REPLACE SECRET claude_nocs (TYPE oracle, user 'myuser', password 'mypassword');
ATTACH '' AS ora_x (TYPE oracle, SECRET claude_nocs);      -- expect: has no connectString

-- 3. The default secret only fills in an EMPTY connection string.
CREATE OR REPLACE SECRET __default_oracle (TYPE oracle, user 'myuser', password 'wrong', connectString 'myservice');
ATTACH 'myuser/mypassword@myservice' AS ora_e (TYPE oracle);     -- expect: works (the explicit string wins)
SELECT count(*) AS tables_visible FROM duckdb_tables() WHERE database_name = 'ora_e';
DETACH ora_e;
ATTACH '' AS ora_x (TYPE oracle);                          -- expect: ORA-01017 for "myuser@myservice" (the default secret is used)

-- 4. Error messages never show the password. Expected: ... at "myuser@myservice": ORA-01017 ... (the text must not contain "wrongpw")
ATTACH 'myuser/wrongpw@myservice' AS ora_x (TYPE oracle);

-- 5. Cleanup.
DROP SECRET claude_ok;
DROP SECRET claude_nopw;
DROP SECRET claude_nocs;
DROP SECRET __default_oracle;
