# MiniDB

A small relational database written from scratch in C++20: disk storage,
a buffer pool, B+ tree indexes, a SQL parser, a cost-based planner and a
Volcano-style executor, behind an interactive SQL shell. No dependencies
besides the C++ standard library (GoogleTest is fetched for the tests).

```
minidb> SELECT d.name, COUNT(e.id) AS staff FROM dept d LEFT JOIN emp e ON e.dept_id = d.id
   ...>   GROUP BY d.name ORDER BY d.name;
 name | staff
------+-------
 eng  | 2
 hr   | 0
 ops  | 1
(3 rows)
```

## Building and running

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/Database_course_midterm mydata.db          # interactive shell
./build/Database_course_midterm mydata.db < script.sql
./build/Database_course_midterm mydata.db --pool 1024   # buffer pool frames (4 KB each)
./build/Database_course_midterm mydata.db --nosync      # no fsync (see "Transactions and durability")
```

The whole database lives in one file, plus its write-ahead log `mydata.db.wal`
while it is open (a clean exit folds the log into the file and deletes it).
Type `.help` in the shell for the SQL summary and the meta commands
(`.tables`, `.schema t`, `.indexes`, `.stats`, `.populate t n`, `.read file.sql`,
`.timer on`, `.checkpoint`, `.exit`). Inside a transaction the prompt changes to `minidb*>`.

## SQL supported

| Area | Supported |
|---|---|
| Types | `INT` (64-bit), `DOUBLE`, `TEXT`, `BOOL`, `DATE`, `TIMESTAMP` (second precision, no time zone, years 1-9999), `NULL`; literals `DATE '2024-01-31'`, `TIMESTAMP '2024-01-31 12:00:00'`, and text in a date context (`d >= '2024-01-01'`) |
| DDL | `CREATE TABLE` with `PRIMARY KEY`, `UNIQUE`, `NOT NULL`, `DEFAULT`, `REFERENCES` / `FOREIGN KEY`, and composite `PRIMARY KEY (a, b)` / `UNIQUE (a, b)` table constraints; `CREATE [UNIQUE] INDEX` on one or more columns; `CREATE [OR REPLACE] VIEW v [(cols)] AS SELECT ...`; `DROP TABLE / INDEX / VIEW`; `ALTER TABLE ADD / DROP / RENAME [COLUMN]`, `RENAME TO`, `ADD [CONSTRAINT n] FOREIGN KEY`, `DROP CONSTRAINT` |
| Foreign keys | single-column, to a `PRIMARY KEY` or `UNIQUE` column (the primary key if none is named); `ON DELETE` / `ON UPDATE` `NO ACTION` (default), `RESTRICT`, `CASCADE`, `SET NULL`, `SET DEFAULT`; self-references; checked at the end of each statement |
| DML | `INSERT ... VALUES / SELECT`, `UPDATE`, `DELETE` — each statement is atomic |
| Queries | `SELECT [DISTINCT]`, `WHERE`, `[INNER] JOIN`, `LEFT / RIGHT / FULL [OUTER] JOIN`, `CROSS JOIN` / comma joins, `GROUP BY`, `HAVING`, `ORDER BY` (names, aliases, positions), `LIMIT` / `OFFSET` |
| Transactions | `BEGIN` / `COMMIT` / `ROLLBACK` (also `START TRANSACTION`, `END`, `ABORT`), `SAVEPOINT s`, `ROLLBACK TO [SAVEPOINT] s`, `RELEASE [SAVEPOINT] s`; DDL is transactional too; durable and crash-safe through the write-ahead log |
| Set operations | `UNION`, `INTERSECT`, `EXCEPT`, each with or without `ALL` |
| Subqueries | scalar `(SELECT ...)`, `x [NOT] IN (SELECT ...)`, `[NOT] EXISTS (...)`, derived tables `FROM (SELECT ...) alias`; correlated subqueries (referring to the outer row) in `WHERE` (also of `UPDATE` / `DELETE`), `HAVING`, the select list and `UPDATE ... SET` (the row being updated); subqueries in `INSERT ... VALUES` may be correlated within themselves |
| Expressions | arithmetic (overflow-checked), comparisons, `AND / OR / NOT` with three-valued logic, `LIKE`, `BETWEEN`, `IN (list)`, `IS [NOT] NULL`, `||`, `CASE` |
| Functions | `ABS`, `ROUND`, `LENGTH`, `UPPER`, `LOWER`, `SUBSTR`, `COALESCE`, `NULLIF`, `CAST(x AS type)`, `DATE(x)`, `YEAR`, `MONTH`, `DAY`, `HOUR`, `MINUTE`, `SECOND`; date `+`/`-` days, timestamp `+`/`-` seconds, `d1 - d2` (days) |
| Aggregates | `COUNT(*)`, `COUNT / SUM / AVG / MIN / MAX`, each also with `DISTINCT` |
| Tools | `EXPLAIN`, `EXPLAIN ANALYZE`, `ANALYZE [table]` (column statistics for the planner) |

Examples (all of them run as an end-to-end test, see
[`tests/e2e/advanced`](tests/e2e/advanced/1.sql)):

```sql
-- Scalar functions, CASE and string concatenation
SELECT UPPER(name) || ' (' || COALESCE(dept_id, 0) || ')' AS who,
       CASE WHEN salary >= 80 THEN 'high' WHEN salary >= 60 THEN 'mid' ELSE 'low' END AS band
  FROM emp ORDER BY id;

-- Subqueries
SELECT name FROM emp WHERE salary > (SELECT AVG(salary) FROM emp);
SELECT name FROM dept WHERE id NOT IN (SELECT dept_id FROM emp WHERE dept_id IS NOT NULL);
SELECT g.dept_id, g.n FROM (SELECT dept_id, COUNT(*) AS n FROM emp GROUP BY dept_id) g WHERE g.n > 1;

-- Set operations
SELECT id FROM dept EXCEPT SELECT dept_id FROM emp;

-- Schema changes
ALTER TABLE emp ADD COLUMN email TEXT UNIQUE;
ALTER TABLE emp RENAME COLUMN salary TO pay;
```

Foreign keys (see [`tests/e2e/foreign_keys`](tests/e2e/foreign_keys/1.sql)):

```sql
CREATE TABLE dept (id INT PRIMARY KEY, name TEXT);
CREATE TABLE emp (
    id INT PRIMARY KEY,
    dept_id INT REFERENCES dept ON DELETE CASCADE ON UPDATE CASCADE,
    boss INT,
    CONSTRAINT emp_boss FOREIGN KEY (boss) REFERENCES emp (id) ON DELETE SET NULL
);
INSERT INTO emp VALUES (5, 9, NULL);
-- ERROR: insert or update on table "emp" violates foreign key constraint "emp_dept_id_fkey":
--        key (dept_id)=(9) is not present in table "dept"
UPDATE dept SET id = 20 WHERE id = 2;   -- employees of dept 2 move to 20
DELETE FROM dept WHERE id = 20;         -- ... and are deleted with it
```

Views (see [`tests/e2e/views`](tests/e2e/views/1.sql)) are stored queries
that can be used wherever a table can be read:

```sql
CREATE VIEW staff AS
  SELECT e.name, d.name AS dept, e.salary FROM emp e LEFT JOIN dept d ON d.id = e.dept_id;
CREATE VIEW payroll (dept, people, total) AS
  SELECT dept, COUNT(*), SUM(salary) FROM staff GROUP BY dept;
SELECT * FROM payroll WHERE total > 60;
```

## Architecture

```
 shell.cpp        statement splitting, meta commands, result printing
 parser.cpp       SQL text -> AST (ast.h)               recursive descent
 planner.cpp      AST -> operator tree                  cost-based
 executors.cpp    operators: scans, filter, joins, aggregate, sort, set ops ...
 expression.cpp   binding and evaluation of expressions
 database.cpp     statement execution, DDL, transactions (BEGIN/COMMIT/SAVEPOINT)
 write_set.cpp    atomic write path: constraints, foreign key actions
 catalog.cpp      tables, indexes, foreign keys, free-page list (persisted from page 0)
 bplus_tree.cpp   B+ tree index on 64-bit keys
 table_heap.cpp   table = linked list of slotted pages
 table_page.cpp   slotted page with variable-length tuples
 buffer_pool.cpp  page cache: pinning, LRU replacement, write-back
 disk_manager.cpp the database file as an array of 4 KB pages + checksums, written via the log
 wal.cpp          write-ahead log: page images, commits, recovery, checkpoints
 file_io.cpp      positioned file I/O with fsync (Win32 / POSIX)
```

**Storage.** Every page is 4 KB. Tables are heap files of slotted pages; a
row is addressed by its RID (page, slot), which stays stable while the row
lives. Pages freed by `DROP TABLE` / `DROP INDEX` / `ALTER TABLE` go to a
free-page list (persisted in the catalog) and are reused before the file
grows. On disk every page is followed by a CRC-32C checksum of its contents
and its page id. The checksum is checked whenever the page is read, so damage
to the file (a flipped bit, a truncated file, a page written to the wrong
place) is reported as an error, never returned as data. Page headers and
slots are also validated on access, so a bad page that somehow has a valid
checksum still produces an error rather than a crash.

**Indexes.** A B+ tree whose entries are (key, RID) pairs, which makes
non-unique indexes natural. `INT`, `DATE` and `TIMESTAMP` columns are indexed by value (equality and
range scans); `TEXT` columns by a 64-bit FNV-1a hash (equality only). Hashes
can collide, so uniqueness checks compare the real values and index scans
re-check the condition. A delete that leaves a node less than half full
borrows entries from a sibling or merges the two; the freed page goes to the
free-page list, and a root left with one child is replaced by it, so an index
shrinks with its table.

A multi-column index — also behind `PRIMARY KEY (a, b)` and `UNIQUE (a, b)` —
uses a "wide" tree with a second 64-bit key part: entries are
(key(a), key2, RID), where key2 is the second column's key for a two-column
index (so an `INT` second column stays ordered) and a hash of the remaining
columns otherwise. The planner uses it for conditions on the leading column
alone (`a = 1`, `a < 5`), for `a = 1 AND b BETWEEN 3 AND 7` when `b` is the
`INT` second column, and for equality on every column. Conditions on the later
columns are always re-checked (key2 may be a hash, or stand for a `NULL`).
Rows whose leading column is `NULL` are not indexed, and a key containing a
`NULL` never violates uniqueness (as in PostgreSQL). Single-column trees keep
the original node format, so existing database files open unchanged.

**Planning.** For every table the planner compares a sequential scan with
each usable index (min/max statistics from the B+ tree, Cardenas' formula
for heap pages touched), pushes single-table conditions below joins,
orders inner joins greedily (smallest input first, avoiding cross
products) and chooses a hash join for equi-joins (smaller side hashed) or
a nested loop otherwise. Queries with an outer join are joined in the order
written; either side of an outer hash join can be the hashed one, since the
executor can emit unmatched rows of its build input as well as of its probe
input. Uncorrelated subqueries are executed once during planning and
replaced by their result; a correlated one keeps its plan and is re-run for
every outer row, with the outer values passed in as parameters. A condition
such as `inner.col = outer.col` on an indexed column becomes an index scan
whose bounds are filled in from the outer row on every run (and the
condition is still re-checked), so `EXISTS` / scalar subqueries over a large
indexed table stay cheap.

**Statistics.** `ANALYZE [table]` records for every column the number of
NULLs and of distinct values, the most common values with their counts, and
an equi-depth histogram (32 buckets; tables over 30000 rows are sampled for
these two). They are kept in the catalog and used for the selectivity of
`=`, `<>`, ranges (`BETWEEN` and `a > x AND a < y` as one range), `IS NULL`
and `IN`, for equi-join sizes (1 / the larger distinct count) and for the
number of groups. Without `ANALYZE` the planner falls back to min/max from
the index and fixed guesses. Changing a table's columns drops its statistics;
row changes do not, so run `ANALYZE` again after large changes.

**Views.** The catalog stores a view's query as text. Every statement that
reads the view parses and plans that text again and uses the plan like a
subquery in `FROM`, so the view always shows the current rows and the current
tables (a `SELECT *` view picks up new columns). `CREATE VIEW` plans the query
once to reject a view that could never work: unknown tables or columns,
duplicate column names, or a view that would read itself, directly or through
other views. The tables under a view can still be changed or dropped later,
as in SQLite; the view then reports the error when it is queried.

**Execution.** Operators follow the Volcano model (`Init()` / `Next(row)`),
so rows stream through the plan. `EXPLAIN ANALYZE` shows estimated vs.
actual row counts per operator.

**Writes.** Every modifying statement first collects all its changes in a
write set, runs the foreign key actions (`CASCADE`, `SET NULL`,
`SET DEFAULT`, which may add changes to further tables, recursively), checks
every affected row (types, `NOT NULL`, row size, uniqueness and foreign keys,
all against the end-of-statement state) and only then writes, so the usual
errors are caught before anything changes, not even in the tables a cascade
reached. Rows referencing a changed key are found through an index on the
referencing column if there is one, else with one scan per statement. Errors
that can only happen while writing (an I/O error, say) are covered by the
transaction machinery below.

**Transactions and durability.** Pages are never written into the database
file directly: every page write (including pages the buffer pool evicts in
the middle of a statement) is appended as a full page image to the
write-ahead log `<file>.wal`. A transaction is one *commit group* of the log:
`COMMIT` sends the catalog and all dirty pages to the log, appends a `COMMIT`
record and fsyncs the log. That record is the commit point; from then on the
transaction survives a crash or power failure. `ROLLBACK` cuts the group off
the log, discards the buffer pool and reloads the catalog from the last
committed state. That is why schema changes (`CREATE` / `DROP` / `ALTER`)
roll back too.

- Without `BEGIN`, every statement is a transaction of its own (autocommit).
- `SAVEPOINT` flushes every change made so far into the log and sets a mark
  in the open group. A page written again after the mark is appended to the
  log instead of overwriting its earlier image there, so `ROLLBACK TO` only
  has to cut the log back to the mark. Savepoints nest and names can be reused.
- Inside `BEGIN ... COMMIT`, every statement runs under an implicit
  savepoint. A failing statement is undone completely, even if it fails
  halfway through writing, and the transaction carries on (like SQLite and
  MySQL; PostgreSQL would abort the whole transaction).
- A transaction still open when the database is closed is rolled back. The
  shell prints a warning in that case.
- One connection at a time, so isolation is trivially serializable.

A *checkpoint* copies the newest committed image of every logged page into
the database file (with its checksum), fsyncs it and empties the log. It runs
after a commit once the log holds 1024 pages (4 MB), on `.checkpoint` (not
inside a transaction), and on a clean exit (which also deletes the log). On
open, recovery replays every complete commit group and drops whatever follows
the last `COMMIT` record: an unfinished transaction, or a record torn by the
crash. Records carry a checksum, the log's salt (which changes whenever the
log is emptied), and a transaction number; the `COMMIT` record also carries a
digest of its group. So stale or torn records, including ones cut off by
`ROLLBACK TO`, are never applied. Because records are whole pages, replay is
idempotent and also repairs pages torn in the middle of a checkpoint. `.stats`
shows the log's size, commits and checkpoints, and the open transaction.

```
 buffer pool ──page images──▶ <file>.wal ──checkpoint──▶ <file>
                              [P][P][COMMIT][P][P][P][COMMIT][P][P]
                              └──────── recovered ─────────┘ └ dropped after a crash
```

Every commit costs one fsync of the log, so grouping statements into one
transaction is much faster than autocommit. Measured on the development
machine (Windows): 1000 single-row `INSERT`s take ~2 s as autocommit vs
~0.1 s inside one `BEGIN ... COMMIT`. `--nosync`
(or `DiskOptions{.sync = false}`) skips the fsyncs: commits stay atomic if the
process crashes, but not if the machine loses power.

## Limitations

- One connection at a time: no concurrency control, and no file locking to
  stop two processes from opening the same database file.
- A transaction's changes stay in the log until it commits, so a very large
  transaction makes the log (not the database file) large until the next
  checkpoint.
- Checksums detect a damaged page in the database file but cannot repair it
  (unless the log still holds a copy, which recovery writes back).
- Database files from before page checksums (pages of exactly 4096 bytes)
  cannot be opened.
- `DEFAULT` must be a constant.
- Views are read-only (no `INSERT` / `UPDATE` / `DELETE` through a view) and
  are not flattened into the query that uses them, so conditions on a view's
  columns are applied after the view's rows are produced.
- Joins are left-deep: no parenthesized joins, and a `RIGHT` / `FULL JOIN`
  cannot follow a comma-separated `FROM` list (write `CROSS JOIN` instead).
- Indexes on `INT`, `TEXT`, `DATE` and `TIMESTAMP` columns only. A multi-column index is not used
  for a range on a column after the second, or on a `TEXT` column.
- Foreign keys are single-column (they cannot reference a composite
  key), and `RESTRICT` behaves like `NO ACTION` (both are checked at the
  end of the statement).

## Tests

About 500 tests in three layers — unit tests per component, integration
tests through the `Database` API, and end-to-end tests that run the shell
binary against golden output — plus randomized differential and
file-corruption tests. See [docs/TESTING.md](docs/TESTING.md).

```bash
ctest --test-dir build -j 8 --output-on-failure
```
