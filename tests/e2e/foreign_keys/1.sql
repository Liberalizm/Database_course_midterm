-- Foreign keys: enforcement, referential actions, and the shell's view of them.
CREATE TABLE dept (id INT PRIMARY KEY, name TEXT NOT NULL);
CREATE TABLE emp (
    id INT PRIMARY KEY,
    name TEXT,
    dept_id INT REFERENCES dept ON DELETE CASCADE ON UPDATE CASCADE,
    boss INT,
    CONSTRAINT emp_boss FOREIGN KEY (boss) REFERENCES emp (id) ON DELETE SET NULL
);
.schema emp
.schema dept
INSERT INTO dept VALUES (1, 'eng'), (2, 'ops'), (3, 'hr');
INSERT INTO emp VALUES (1, 'ann', 1, NULL), (2, 'bob', 1, 1), (3, 'cid', 2, 1), (4, 'dan', 2, 3);
INSERT INTO emp VALUES (5, 'eve', 9, NULL);
INSERT INTO emp VALUES (5, 'eve', 3, 42);
UPDATE dept SET id = 20 WHERE id = 2;
SELECT id, name, dept_id, boss FROM emp ORDER BY id;
DELETE FROM emp WHERE id = 1;
SELECT id, name, dept_id, boss FROM emp ORDER BY id;
DELETE FROM dept WHERE id = 20;
SELECT id, name, dept_id, boss FROM emp ORDER BY id;
DROP TABLE dept;
DROP INDEX dept_pkey;
