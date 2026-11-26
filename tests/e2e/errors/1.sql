-- Errors are reported and the shell keeps going.
SELEC 1;
SELECT * FROM missing;
CREATE TABLE t (a INT PRIMARY KEY, b TEXT);
INSERT INTO t VALUES ('x', 'y');
INSERT INTO t VALUES (1, 'one');
INSERT INTO t VALUES (1, 'again');
SELECT c FROM t;
SELECT 1 / 0;
SELECT b, COUNT(*) FROM t;
.unknowncommand
.schema nope
SELECT * FROM t;
