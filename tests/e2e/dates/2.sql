-- After a restart: values, DEFAULT and the index are still there.
INSERT INTO trips (id, city, start_day, end_day) VALUES (5, 'Kyiv', '2024-08-24', '2024-08-31');
SELECT id, city, start_day, booked FROM trips WHERE start_day >= DATE '2024-06-01' ORDER BY start_day;
