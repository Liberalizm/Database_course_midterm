-- Composite PRIMARY KEY, a composite UNIQUE constraint and a multi-column index.
CREATE TABLE enrollment (
    student INT,
    course TEXT,
    term INT,
    grade INT,
    PRIMARY KEY (student, course),
    UNIQUE (course, term, student)
);
CREATE INDEX enrollment_term_grade ON enrollment (term, grade);
INSERT INTO enrollment VALUES (1, 'db', 2024, 90), (1, 'os', 2024, 75), (2, 'db', 2024, 82), (2, 'ml', 2025, 68);
INSERT INTO enrollment VALUES (3, 'db', 2025, 95), (3, 'os', 2025, NULL);
-- Same student, same course: rejected, also inside one statement.
INSERT INTO enrollment VALUES (1, 'db', 2025, 50);
INSERT INTO enrollment VALUES (4, 'ml', 2025, 70), (4, 'ml', 2024, 71);
INSERT INTO enrollment (course, term) VALUES ('db', 2026);
-- Swapping keys inside one UPDATE is fine; colliding with another row is not.
UPDATE enrollment SET course = CASE course WHEN 'db' THEN 'os' ELSE 'db' END WHERE student = 1;
UPDATE enrollment SET student = 2 WHERE student = 3 AND course = 'db';
SELECT student, course, grade FROM enrollment ORDER BY student, course;
.schema enrollment
.indexes
-- Lookups through the multi-column indexes (with enough rows to be worth it).
.populate enrollment 2000
SELECT COUNT(*), COUNT(DISTINCT student), MIN(term) FROM enrollment;
EXPLAIN SELECT grade FROM enrollment WHERE student = 2 AND course = 'ml';
EXPLAIN SELECT course FROM enrollment WHERE term = 2025 AND grade >= 70;
SELECT student, course FROM enrollment WHERE term = 2025 AND grade >= 70 ORDER BY student;
CREATE TABLE bad (a INT, b DOUBLE, PRIMARY KEY (a, b));
CREATE TABLE bad (a INT, PRIMARY KEY (a, zz));
CREATE INDEX bad_idx ON enrollment (term, term);
