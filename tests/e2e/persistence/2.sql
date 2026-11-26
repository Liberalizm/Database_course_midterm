-- Step 2: a new shell process on the same file must see everything.
.tables
.indexes
SELECT * FROM items WHERE qty = 10 ORDER BY id;
INSERT INTO items VALUES (1, 99, 'duplicate');
INSERT INTO items VALUES (4, 40, 'screw');
