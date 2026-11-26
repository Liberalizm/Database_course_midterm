-- Step 1: create data and an index, then exit.
CREATE TABLE items (id INT PRIMARY KEY, qty INT, label TEXT);
CREATE INDEX items_qty ON items (qty);
INSERT INTO items VALUES (1, 10, 'bolt'), (2, 20, 'nut'), (3, 10, 'washer');
.checkpoint
