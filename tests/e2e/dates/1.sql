-- DATE and TIMESTAMP columns: literals, text input, arithmetic, functions, indexes.
CREATE TABLE trips (id INT PRIMARY KEY, city TEXT, start_day DATE, end_day DATE, booked TIMESTAMP DEFAULT TIMESTAMP '2024-01-01 09:00:00');
INSERT INTO trips VALUES (1, 'Oslo', '2024-02-27', '2024-03-02', '2024-01-15 18:30:00'),
                         (2, 'Rome', DATE '2024-06-01', DATE '2024-06-08', TIMESTAMP '2024-03-01T07:05');
INSERT INTO trips (id, city, start_day, end_day) VALUES (3, 'Lima', '2023-12-30', '2024-01-02');
INSERT INTO trips VALUES (4, 'Nowhere', '2023-02-29', NULL, NULL);
SELECT * FROM trips ORDER BY start_day;
SELECT city, end_day - start_day AS nights, start_day + 7 AS week_later, year(start_day) AS y FROM trips ORDER BY id;
SELECT city FROM trips WHERE start_day <= '2024-03-01' AND end_day >= '2024-03-01';
SELECT city, hour(booked) AS h, minute(booked) AS m, date(booked) AS day FROM trips ORDER BY booked;
SELECT CAST('2024-12-24' AS DATE) - CAST(start_day AS DATE) AS days_to_xmas FROM trips WHERE id = 2;
SELECT month(start_day) AS mon, COUNT(*) FROM trips GROUP BY month(start_day) ORDER BY mon;
CREATE INDEX trips_start ON trips (start_day);
SELECT city FROM trips WHERE start_day = DATE '2024-06-01';
SELECT DATE '9999-12-31' + 1;
.schema trips
