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
```

The whole database lives in one file. Type `.help` in the shell for the
SQL summary and the meta commands (`.tables`, `.schema t`, `.indexes`,
`.stats`, `.populate t n`, `.read file.sql`, `.timer on`, `.exit`).

## SQL supported

| Area | Supported |
|---|---|
| Types | `INT` (64-bit), `DOUBLE`, `TEXT`, `BOOL`, `NULL` |
| DDL | `CREATE TABLE` with `PRIMARY KEY`, `UNIQUE`, `NOT NULL`, `DEFAULT`; `CREATE [UNIQUE] INDEX`; `DROP TABLE / INDEX`; `ALTER TABLE ADD / DROP / RENAME [COLUMN]`, `RENAME TO` |
| DML | `INSERT ... VALUES / SELECT`, `UPDATE`, `DELETE` — each statement is atomic |
| Queries | `SELECT [DISTINCT]`, `WHERE`, `[INNER] JOIN`, `LEFT [OUTER] JOIN`, `CROSS JOIN` / comma joins, `GROUP BY`, `HAVING`, `ORDER BY` (names, aliases, positions), `LIMIT` / `OFFSET` |
| Set operations | `UNION`, `INTERSECT`, `EXCEPT`, each with or without `ALL` |
| Subqueries | scalar `(SELECT ...)`, `x [NOT] IN (SELECT ...)`, `[NOT] EXISTS (...)`, derived tables `FROM (SELECT ...) alias`; correlated subqueries (referring to the outer row) in `WHERE` (also of `UPDATE` / `DELETE`), `HAVING` and the select list |
| Expressions | arithmetic (overflow-checked), comparisons, `AND / OR / NOT` with three-valued logic, `LIKE`, `BETWEEN`, `IN (list)`, `IS [NOT] NULL`, `||`, `CASE` |
| Functions | `ABS`, `ROUND`, `LENGTH`, `UPPER`, `LOWER`, `SUBSTR`, `COALESCE`, `NULLIF` |
| Aggregates | `COUNT(*)`, `COUNT / SUM / AVG / MIN / MAX`, each also with `DISTINCT` |
| Tools | `EXPLAIN` and `EXPLAIN ANALYZE` |

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

## Architecture

```
 shell.cpp        statement splitting, meta commands, result printing
 parser.cpp       SQL text -> AST (ast.h)               recursive descent
 planner.cpp      AST -> operator tree                  cost-based
 executors.cpp    operators: scans, filter, joins, aggregate, sort, set ops ...
 expression.cpp   binding and evaluation of expressions
 database.cpp     statement execution, constraints, atomic write path
 catalog.cpp      tables, indexes, free-page list (persisted from page 0)
 bplus_tree.cpp   B+ tree index on 64-bit keys
 table_heap.cpp   table = linked list of slotted pages
 table_page.cpp   slotted page with variable-length tuples
 buffer_pool.cpp  page cache: pinning, LRU replacement, write-back
 disk_manager.cpp the database file as an array of 4 KB pages, written via the log
 wal.cpp          write-ahead log: page images, commits, recovery, checkpoints
 file_io.cpp      positioned file I/O with fsync (Win32 / POSIX)
```

**Storage.** Every page is 4 KB. Tables are heap files of slotted pages; a
row is addressed by its RID (page, slot), which stays stable while the row
lives. Pages freed by `DROP TABLE` / `DROP INDEX` / `ALTER TABLE` go to a
free-page list (persisted in the catalog) and are reused before the file
grows. Page headers and slots are validated on access, so a damaged file
produces an error rather than a crash.

**Indexes.** A B+ tree whose entries are (key, RID) pairs, which makes
non-unique indexes natural. `INT` columns are indexed by value (equality and
range scans); `TEXT` columns by a 64-bit FNV-1a hash (equality only). Hashes
can collide, so uniqueness checks compare the real values and index scans
re-check the condition. Deletes are lazy (nodes are never merged).

**Planning.** For every table the planner compares a sequential scan with
each usable index (min/max statistics from the B+ tree, Cardenas' formula
for heap pages touched), pushes single-table conditions below joins,
orders inner joins greedily (smallest input first, avoiding cross
products) and chooses a hash join for equi-joins (smaller side hashed) or
a nested loop otherwise. Queries with a `LEFT JOIN` are joined in the order
written. Uncorrelated subqueries are executed once during planning and
replaced by their result; a correlated one keeps its plan and is re-run for
every outer row, with the outer values passed in as parameters.

**Execution.** Operators follow the Volcano model (`Init()` / `Next(row)`),
so rows stream through the plan. `EXPLAIN ANALYZE` shows estimated vs.
actual row counts per operator.

**Writes.** Every modifying statement first computes and checks all
affected rows (types, `NOT NULL`, row size, uniqueness against the
end-of-statement state) and only then writes, so a failing statement
normally changes nothing.

**Write-ahead log and crash recovery.** Each modifying statement is a
transaction. Pages are never written into the database file directly:
every page write (including pages the buffer pool evicts in the middle of a
statement) is appended as a full page image to the log `<file>.wal`. At the
end of the statement the catalog and all dirty pages go to the log, followed
by a `COMMIT` record, and the log is fsynced. From that moment the statement
survives a crash or power failure. If anything fails before that (an I/O
error, say), the statement is rolled back: its log records are cut off, the
buffer pool is discarded and the catalog is reloaded.

A *checkpoint* copies the newest committed image of every logged page into
the database file, fsyncs it and empties the log. It runs when the log
reaches 1024 pages (4 MB), on `.checkpoint`, and on a clean exit (which also
deletes the log). On open, recovery replays every complete commit group and
drops whatever follows the last `COMMIT` record: an unfinished statement, or a
record torn by the crash. Records carry a checksum, the log's salt (which
changes whenever the log is emptied), and a transaction number; the `COMMIT`
record also carries a digest of its group. So stale or torn records are
never applied. Because records are whole pages, replay is idempotent and
also repairs pages torn in the middle of a checkpoint. `.stats` shows the
log's size, commits and checkpoints.

```
 buffer pool ──page images──▶ <file>.wal ──checkpoint──▶ <file>
                              [P][P][COMMIT][P][P][P][COMMIT][P][P]
                              └──────── recovered ─────────┘ └ dropped after a crash
```

## Limitations

- No multi-statement transactions or concurrency: one connection, autocommit only.
- No page checksums in the database file (the log has its own).
- No `RIGHT` / `FULL` joins, foreign keys or views; `DEFAULT` must be a constant.
- Indexes on `INT` and `TEXT` columns only; one column per index.
- B+ tree nodes are not merged after deletes.

## Tests

About 360 tests in three layers — unit tests per component, integration
tests through the `Database` API, and end-to-end tests that run the shell
binary against golden output — plus randomized differential and
file-corruption tests. See [docs/TESTING.md](docs/TESTING.md).

```bash
ctest --test-dir build -j 8 --output-on-failure
```
