-- Step 1: transactions in the shell. The last one is left open at exit.
CREATE TABLE account (id INT PRIMARY KEY, owner TEXT NOT NULL, balance INT NOT NULL);
INSERT INTO account VALUES (1, 'alice', 100), (2, 'bob', 50);

-- ROLLBACK undoes everything since BEGIN, including schema changes.
BEGIN;
UPDATE account SET balance = 0;
CREATE TABLE audit (msg TEXT);
DROP TABLE account;
ROLLBACK;
SELECT * FROM account ORDER BY id;
.tables

-- A transfer that commits. The failing INSERT inside it only undoes itself.
BEGIN;
UPDATE account SET balance = balance - 30 WHERE id = 1;
UPDATE account SET balance = balance + 30 WHERE id = 2;
INSERT INTO account VALUES (2, 'carol', 0);
INSERT INTO account VALUES (3, 'carol', 0);
COMMIT;
SELECT * FROM account ORDER BY id;

-- Savepoints.
BEGIN;
DELETE FROM account WHERE id = 3;
SAVEPOINT before_raise;
UPDATE account SET balance = balance * 10;
SELECT SUM(balance) FROM account;
ROLLBACK TO SAVEPOINT before_raise;
SELECT SUM(balance) FROM account;
RELEASE SAVEPOINT before_raise;
COMMIT;
SELECT * FROM account ORDER BY id;

-- Misplaced transaction statements are errors.
COMMIT;
ROLLBACK TO nowhere;
BEGIN;
BEGIN;
RELEASE nowhere;
ROLLBACK;

-- Left open: the shell rolls it back at exit.
BEGIN;
INSERT INTO account VALUES (99, 'ghost', 1000);
SELECT COUNT(*) FROM account;
