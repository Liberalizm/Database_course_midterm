-- Advanced SQL features through the interactive shell (also the examples in README.md).
CREATE TABLE dept (id INT PRIMARY KEY, name TEXT UNIQUE);
CREATE TABLE emp (id INT PRIMARY KEY, name TEXT, dept_id INT, salary INT);
INSERT INTO dept VALUES (1, 'eng'), (2, 'ops'), (3, 'hr');
INSERT INTO emp VALUES (1, 'ann', 1, 100), (2, 'bob', 1, 80), (3, 'cid', 2, 50), (4, 'dan', NULL, 70);

-- LEFT JOIN keeps departments without employees.
SELECT d.name, COUNT(e.id) AS staff FROM dept d LEFT JOIN emp e ON e.dept_id = d.id
  GROUP BY d.name ORDER BY d.name;

-- Scalar functions, CASE and string concatenation.
SELECT UPPER(name) || ' (' || COALESCE(dept_id, 0) || ')' AS who,
       CASE WHEN salary >= 80 THEN 'high' WHEN salary >= 60 THEN 'mid' ELSE 'low' END AS band
  FROM emp ORDER BY id;

-- DISTINCT aggregates.
SELECT COUNT(*), COUNT(dept_id), COUNT(DISTINCT dept_id) FROM emp;

-- Subqueries: scalar, IN, EXISTS and in FROM.
SELECT name FROM emp WHERE salary > (SELECT AVG(salary) FROM emp) ORDER BY name;
SELECT name FROM dept WHERE id NOT IN (SELECT dept_id FROM emp WHERE dept_id IS NOT NULL);
SELECT g.dept_id, g.n FROM (SELECT dept_id, COUNT(*) AS n FROM emp GROUP BY dept_id) g
  WHERE g.n > 1;

-- Set operations.
SELECT id FROM dept EXCEPT SELECT dept_id FROM emp;
SELECT name FROM dept UNION SELECT name FROM emp ORDER BY name LIMIT 4;

-- TEXT keys: UNIQUE is enforced, and equality lookups on big tables use the hash index.
INSERT INTO dept VALUES (4, 'eng');
CREATE TABLE users (id INT PRIMARY KEY, login TEXT UNIQUE);
.populate users 2000
EXPLAIN SELECT id FROM users WHERE login = 'login_77';
SELECT id FROM users WHERE login = 'login_77';

-- ALTER TABLE.
ALTER TABLE emp ADD COLUMN email TEXT UNIQUE;
UPDATE emp SET email = name || '@corp' WHERE dept_id = 1;
ALTER TABLE emp RENAME COLUMN salary TO pay;
ALTER TABLE emp DROP COLUMN dept_id;
SELECT * FROM emp ORDER BY id;
.schema emp

-- Statements are atomic: the second row's duplicate key rejects the whole INSERT.
INSERT INTO emp VALUES (5, 'eve', 90, NULL), (1, 'dup', 1, NULL);
SELECT COUNT(*) FROM emp;
