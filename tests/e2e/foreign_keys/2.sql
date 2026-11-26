-- After a restart the constraints are still there and still enforced.
.schema emp
INSERT INTO emp VALUES (6, 'fay', 2, NULL);
INSERT INTO emp VALUES (6, 'fay', 3, 2);
ALTER TABLE emp DROP CONSTRAINT emp_dept_id_fkey;
INSERT INTO emp VALUES (7, 'gus', 99, NULL);
ALTER TABLE emp ADD CONSTRAINT emp_dept FOREIGN KEY (dept_id) REFERENCES dept;
DELETE FROM emp WHERE id = 7;
ALTER TABLE emp ADD CONSTRAINT emp_dept FOREIGN KEY (dept_id) REFERENCES dept;
DELETE FROM dept WHERE id = 3;
SELECT e.name, d.name FROM emp e JOIN dept d ON e.dept_id = d.id ORDER BY e.name;
