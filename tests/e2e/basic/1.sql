-- Basic SQL features through the interactive shell.
CREATE TABLE groups (id INT PRIMARY KEY, title TEXT);
CREATE TABLE students (id INT PRIMARY KEY, name TEXT, group_id INT, gpa DOUBLE);
INSERT INTO groups VALUES (1, 'CS-101'), (2, 'CS-102'), (3, 'Math-1');
INSERT INTO students VALUES
  (1, 'Alice', 1, 3.9), (2, 'Bob', 1, 3.1), (3, 'Carol', 2, 3.5),
  (4, 'Dave', 2, NULL), (5, 'Erin', NULL, 2.8);
.tables
.schema students
SELECT * FROM students ORDER BY id;
SELECT name, gpa FROM students WHERE gpa >= 3.5 ORDER BY gpa DESC;
SELECT s.name, g.title FROM students s JOIN groups g ON s.group_id = g.id ORDER BY s.name;
SELECT g.title, COUNT(*) AS n, AVG(s.gpa) AS avg_gpa
  FROM students s JOIN groups g ON s.group_id = g.id
  GROUP BY g.title ORDER BY g.title;
UPDATE students SET gpa = 3.0 WHERE gpa IS NULL;
DELETE FROM students WHERE group_id IS NULL;
SELECT COUNT(*), MIN(gpa), MAX(gpa) FROM students;
SELECT DISTINCT group_id FROM students ORDER BY group_id LIMIT 1 OFFSET 1;
EXPLAIN SELECT * FROM students WHERE id = 3;
