-- Step 2: a new shell process sees the views; dropping and redefining them.
.tables
SELECT * FROM payroll ORDER BY dept;
CREATE OR REPLACE VIEW staff AS SELECT name, 'n/a' AS dept, salary FROM emp WHERE salary >= 80;
SELECT * FROM payroll;
DROP VIEW staff;
SELECT * FROM payroll;
DROP VIEW payroll;
.tables
