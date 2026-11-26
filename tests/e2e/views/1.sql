-- Views: stored queries that are read like tables.
CREATE TABLE dept (id INT PRIMARY KEY, name TEXT UNIQUE);
CREATE TABLE emp (id INT PRIMARY KEY, name TEXT, dept_id INT, salary INT);
INSERT INTO dept VALUES (1, 'eng'), (2, 'ops'), (3, 'hr');
INSERT INTO emp VALUES (1, 'ann', 1, 100), (2, 'bob', 1, 80), (3, 'cid', 2, 50), (4, 'dan', NULL, 70);

CREATE VIEW staff AS
  SELECT e.name, d.name AS dept, e.salary
    FROM emp e LEFT JOIN dept d ON d.id = e.dept_id;  -- everyone, with their department
CREATE VIEW payroll (dept, people, total) AS
  SELECT dept, COUNT(*), SUM(salary) FROM staff GROUP BY dept;

SELECT * FROM staff ORDER BY name;
SELECT * FROM payroll WHERE total > 60 ORDER BY dept;
SELECT s.name FROM staff s WHERE s.salary > (SELECT AVG(salary) FROM staff) ORDER BY s.name;

-- A view always shows the current data.
INSERT INTO emp VALUES (5, 'eve', 3, 90);
SELECT * FROM payroll ORDER BY dept;

-- Views are read-only, and cannot be defined in terms of themselves.
INSERT INTO staff VALUES ('x', 'eng', 1);
CREATE OR REPLACE VIEW staff AS SELECT * FROM payroll;
CREATE VIEW broken AS SELECT nope FROM emp;
.tables
.schema payroll
