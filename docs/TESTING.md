# Testing & CI

MiniDB is tested in three layers. Each layer is a separate CI step, and each
engine component has its own test executable, so a failure points straight at
the part that broke.

```
tests/
├── unit/                    one executable per component, tested in isolation
│   ├── test_disk_manager    storage: raw page I/O
│   ├── test_lru_replacer    storage: eviction policy
│   ├── test_buffer_pool     storage: caching, pinning, write-back, PageGuard
│   ├── test_value           storage: values, schemas, tuple serialization
│   ├── test_table_page      storage: slotted page (insert/delete/compact/update)
│   ├── test_table_heap      storage: heap file of linked pages
│   ├── test_bplus_tree      storage: B+ tree index (splits, duplicates, ranges)
│   ├── test_catalog         storage: metadata + persistence in page 0
│   ├── test_lexer           query:   tokenizer
│   ├── test_parser          query:   SQL -> AST
│   ├── test_expression      query:   binding, evaluation, 3-valued logic, LIKE
│   ├── test_executors       query:   every operator, fed by an in-memory source
│   ├── test_planner         query:   access paths, join order/algorithm, errors
│   └── test_shell           query:   statement splitting, scripts, meta commands, .read
├── integration/             whole engine via the Database API (SQL in, rows out)
│   ├── test_sql.cpp         DDL/DML, constraints, index consistency, index-vs-scan
│   └── test_persistence.cpp data survives restarts and simulated crashes
└── e2e/                     the real shell binary, compared to golden output
    ├── run_case.cmake       runner: N.sql -> compare with N.expected
    ├── basic/               queries, joins, aggregates, EXPLAIN
    ├── advanced/            LEFT JOIN, functions, subqueries, set ops, TEXT keys, ALTER TABLE
    ├── errors/              error messages; the shell keeps going after errors
    └── persistence/         3 separate shell processes on the same file
```

Isolation techniques:

* **Executors** are tested with `VectorExecutor` (`tests/test_util.h`), a fake
  child operator that produces fixed rows, so no tables, buffer pool or planner
  are involved.
* **Expressions** are built from AST nodes directly, not through the parser.
* **Storage** tests use a fresh temporary database file per test (`TempFile`),
  and several of them use a tiny buffer pool on purpose so that eviction happens
  all the time.
* **Cross-checks**: index scan vs. sequential scan and hash join vs. nested loop
  must return the same results on random data.

## Running tests locally

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build -j 8 --output-on-failure    # everything (335 tests)

ctest --test-dir build -L storage                  # one layer
ctest --test-dir build -L query
ctest --test-dir build -L integration
ctest --test-dir build -L e2e
ctest --test-dir build -L buffer_pool              # one component
ctest --test-dir build -R "BPlusTree.*"            # by test name

bash ci/run-tests.sh build Release unit            # exactly what CI runs
```

In CLion, every GoogleTest case shows up in the test runner, and each
`test_<component>` target can be run or debugged on its own.

### Adding tests

* New component: add `tests/unit/test_<name>.cpp` **and** list `<name>` in
  `MINIDB_STORAGE_COMPONENTS` or `MINIDB_QUERY_COMPONENTS` in
  `tests/CMakeLists.txt`. CMake refuses to configure if you forget, so a test
  file can never be silently left out of CI.
* New end-to-end case: create `tests/e2e/<case>/1.sql`, run
  `ctest -R e2e.<case>`, **check by hand** that
  `build/tests/e2e_work/<case>/1.actual` is correct, then copy it to
  `tests/e2e/<case>/1.expected`.

## CI pipeline (`.github/workflows/ci.yml`)

Triggered on **every push to every branch** and on **every pull request into `main`**.

| Job | What it does |
|---|---|
| Linux GCC | Release build with `-Werror`, then storage → query → integration → e2e |
| Linux Clang | same with Clang |
| Windows MSVC | same with Visual Studio |
| Sanitizers | Debug build with AddressSanitizer + UndefinedBehaviorSanitizer, all tests (finds memory errors, leaks, UB) |
| **CI passed** | green only if *all* jobs above are green; this is the check to require |

`ci/run-tests.sh` wraps `ctest` and also fails the job if no tests matched or
if any test was *skipped*: a crashing suite setup shows up as "skipped" in
CTest and would otherwise look green. On failure, test logs and e2e outputs
are uploaded as a build artifact.

## Protecting `main` (one-time setup, repository admin)

The workflow runs the checks; GitHub's branch protection is what *blocks the
merge* when they fail. Set it up once after the workflow has run at least once
(so GitHub knows the check name):

**Web UI:** *Settings → Branches → Add branch ruleset* (or *Add classic branch
protection rule*) for `main`:

1. ✅ Require a pull request before merging
2. ✅ Require status checks to pass → add **`CI passed`**
3. ✅ Require branches to be up to date before merging
4. ✅ Block force pushes
5. (optional) ✅ Do not allow bypassing the above settings

**Or with the GitHub CLI:**

```bash
gh api -X PUT repos/<owner>/<repo>/branches/main/protection \
  -H "Accept: application/vnd.github+json" \
  -F "required_status_checks[strict]=true" \
  -f "required_status_checks[contexts][]=CI passed" \
  -F "enforce_admins=true" \
  -F "required_pull_request_reviews[required_approving_review_count]=0" \
  -F "restrictions=null"
```

Workflow from then on: work on a branch → push (CI runs on every commit) →
open a pull request → merge only becomes possible once **CI passed** is green.
