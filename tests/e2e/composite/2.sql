-- After a restart: the composite keys are still enforced and still used.
INSERT INTO enrollment VALUES (2, 'ml', 2026, 99);
INSERT INTO enrollment VALUES (2, 'os', 2026, 99);
SELECT student, course, term FROM enrollment WHERE student = 2 ORDER BY course;
EXPLAIN SELECT term FROM enrollment WHERE student = 2;
