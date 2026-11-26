// Unit + integration tests for MiniDB. No external framework: just CHECK macros.
#include <algorithm>
#include <cstdio>
#include <iostream>
#include <random>
#include <set>

#include "../src/bplus_tree.h"
#include "../src/buffer_pool.h"
#include "../src/database.h"
#include "../src/table_heap.h"
#include "../src/table_page.h"

using namespace minidb;

static int g_failed = 0, g_passed = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (cond) {                                                                  \
            ++g_passed;                                                              \
        } else {                                                                     \
            ++g_failed;                                                              \
            std::cerr << "  FAILED: " #cond " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
        }                                                                            \
    } while (0)

#define CHECK_THROWS(stmt)                \
    do {                                  \
        bool thrown = false;              \
        try {                             \
            stmt;                         \
        } catch (const DbError&) {        \
            thrown = true;                \
        }                                 \
        CHECK(thrown && #stmt);           \
    } while (0)

static std::string TempFile(const std::string& name) {
    std::string p = "test_" + name + ".db";
    std::remove(p.c_str());
    return p;
}

static void TestBufferPool() {
    std::cout << "[buffer pool]\n";
    std::string path = TempFile("bpm");
    DiskManager disk(path);
    BufferPoolManager bpm(8, &disk);
    std::vector<page_id_t> ids;
    for (int i = 0; i < 20; ++i) {  // more pages than frames -> forces evictions
        page_id_t pid;
        PageGuard g = PageGuard::New(&bpm, &pid);
        std::snprintf(g.Data(), 32, "page-%d", i);
        g.MarkDirty();
        ids.push_back(pid);
    }
    CHECK(bpm.Evictions() >= 12);
    for (int i = 0; i < 20; ++i) {
        PageGuard g = PageGuard::Fetch(&bpm, ids[i]);
        CHECK(std::string(g.Data()) == "page-" + std::to_string(i));
    }
    // all frames pinned -> buffer pool must refuse
    std::vector<PageGuard> pinned;
    for (int i = 0; i < 8; ++i) pinned.push_back(PageGuard::Fetch(&bpm, ids[i]));
    CHECK_THROWS(PageGuard::Fetch(&bpm, ids[10]));
    pinned.clear();
    PageGuard ok = PageGuard::Fetch(&bpm, ids[10]);
    CHECK(std::string(ok.Data()) == "page-10");
}

static void TestTablePage() {
    std::cout << "[slotted page]\n";
    char buf[PAGE_SIZE];
    TablePage p(buf);
    p.Init();
    std::vector<uint16_t> slots;
    std::string tuple(100, 'x');
    while (auto s = p.Insert(tuple.data(), tuple.size())) slots.push_back(*s);
    CHECK(slots.size() == (PAGE_SIZE - TablePage::HEADER) / (100 + TablePage::SLOT));
    for (size_t i = 0; i < slots.size(); i += 2) CHECK(p.Delete(slots[i]));
    // big tuple only fits after compaction
    std::string big(1000, 'y');
    auto s = p.Insert(big.data(), big.size());
    CHECK(s.has_value());
    std::string out;
    CHECK(p.Get(*s, &out) && out == big);
    CHECK(p.Get(slots[1], &out) && out == tuple);  // survived compaction, same slot
    // grow a tuple in place
    std::string bigger(300, 'z');
    CHECK(p.Update(slots[1], bigger.data(), bigger.size()));
    CHECK(p.Get(slots[1], &out) && out == bigger);
}

static void TestBPlusTree() {
    std::cout << "[b+ tree]\n";
    std::string path = TempFile("btree");
    DiskManager disk(path);
    BufferPoolManager bpm(32, &disk);
    BPlusTree tree(&bpm, INVALID_PAGE_ID, 4, 4);  // tiny fan-out => deep tree, many splits
    std::vector<int64_t> keys;
    for (int i = 0; i < 5000; ++i) keys.push_back(i % 1000);  // duplicates
    std::mt19937 rng(7);
    std::shuffle(keys.begin(), keys.end(), rng);
    for (size_t i = 0; i < keys.size(); ++i) CHECK(tree.Insert(keys[i], RID{static_cast<int>(i), 0}));
    CHECK(!tree.Insert(keys[0], RID{0, 0}));  // exact duplicate rejected
    CHECK(tree.CheckAndCount() == 5000);
    CHECK(tree.Height() > 4);
    int cnt = 0;
    for (auto it = tree.LowerBound(500); it.Valid() && it.Entry().key == 500; it.Next()) ++cnt;
    CHECK(cnt == 5);
    for (size_t i = 0; i < keys.size(); i += 2) CHECK(tree.Remove(keys[i], RID{static_cast<int>(i), 0}));
    CHECK(tree.CheckAndCount() == 2500);
    CHECK(!tree.Remove(keys[0], RID{0, 0}));

    // full-size nodes, sequential inserts
    BPlusTree big(&bpm, INVALID_PAGE_ID);
    for (int i = 0; i < 100000; ++i) big.Insert(i, RID{i, 0});
    CHECK(big.CheckAndCount() == 100000);
    CHECK(big.Height() == 3);
    auto it = big.LowerBound(77777);
    CHECK(it.Valid() && it.Entry().key == 77777);
}

static std::vector<Row> Q(Database& db, const std::string& sql) { return db.Execute(sql).back().rows; }

static std::string Plan(Database& db, const std::string& sql) {
    std::string s;
    for (auto& r : Q(db, "EXPLAIN " + sql)) s += r[0].AsText() + "\n";
    return s;
}

static void TestSql() {
    std::cout << "[sql]\n";
    std::string path = TempFile("sql");
    {
        Database db(path, 64);
        db.Execute(
            "CREATE TABLE dept (id INT PRIMARY KEY, name TEXT);"
            "CREATE TABLE emp (id INT PRIMARY KEY, name TEXT, dept_id INT, salary DOUBLE);"
            "INSERT INTO dept VALUES (1, 'eng'), (2, 'sales'), (3, 'empty');"
            "INSERT INTO emp VALUES (1, 'ann', 1, 100.0), (2, 'bob', 1, 80), (3, 'cid', 2, 50),"
            "                       (4, 'dan', 2, 70), (5, 'eve', NULL, 60);");

        CHECK(Q(db, "SELECT * FROM emp").size() == 5);
        CHECK(Q(db, "SELECT name FROM emp WHERE salary > 65 AND dept_id = 1").size() == 2);
        CHECK(Q(db, "SELECT name FROM emp WHERE dept_id IS NULL")[0][0].AsText() == "eve");
        CHECK(Q(db, "SELECT name FROM emp WHERE name LIKE '%n'").size() == 2);
        CHECK(Q(db, "SELECT name FROM emp WHERE id IN (1, 3, 99)").size() == 2);
        CHECK(Q(db, "SELECT 1 + 2 * 3")[0][0].AsInt() == 7);

        auto j = Q(db, "SELECT e.name, d.name FROM emp e JOIN dept d ON e.dept_id = d.id ORDER BY e.name");
        CHECK(j.size() == 4 && j[0][0].AsText() == "ann" && j[0][1].AsText() == "eng");

        auto g = Q(db,
                   "SELECT d.name, COUNT(*) AS n, AVG(e.salary) FROM emp e, dept d WHERE e.dept_id = d.id "
                   "GROUP BY d.name HAVING COUNT(*) >= 2 ORDER BY n DESC, d.name");
        CHECK(g.size() == 2 && g[0][1].AsInt() == 2 && g[0][2].AsDouble() == 90.0);

        auto agg = Q(db, "SELECT COUNT(*), COUNT(dept_id), SUM(salary), MIN(name), MAX(id) FROM emp");
        CHECK(agg[0][0].AsInt() == 5 && agg[0][1].AsInt() == 4 && agg[0][2].AsDouble() == 360.0);
        CHECK(agg[0][3].AsText() == "ann" && agg[0][4].AsInt() == 5);
        CHECK(Q(db, "SELECT COUNT(*) FROM emp WHERE id > 100")[0][0].AsInt() == 0);
        CHECK(Q(db, "SELECT DISTINCT dept_id FROM emp").size() == 3);
        CHECK(Q(db, "SELECT id FROM emp ORDER BY salary DESC LIMIT 2 OFFSET 1")[0][0].AsInt() == 2);

        CHECK(db.Execute("UPDATE emp SET salary = salary * 2 WHERE dept_id = 2")[0].message == "UPDATE 2");
        CHECK(Q(db, "SELECT salary FROM emp WHERE id = 3")[0][0].AsDouble() == 100.0);
        CHECK(db.Execute("DELETE FROM emp WHERE salary < 70")[0].message == "DELETE 1");
        CHECK(Q(db, "SELECT COUNT(*) FROM emp")[0][0].AsInt() == 4);

        // constraints and errors
        CHECK_THROWS(db.Execute("INSERT INTO emp VALUES (1, 'dup', 1, 1)"));
        CHECK_THROWS(db.Execute("INSERT INTO emp VALUES (NULL, 'x', 1, 1)"));
        CHECK_THROWS(db.Execute("INSERT INTO emp VALUES ('str', 'x', 1, 1)"));
        CHECK_THROWS(db.Execute("SELECT nope FROM emp"));
        CHECK_THROWS(db.Execute("SELECT id FROM emp e, dept d"));  // ambiguous
        CHECK_THROWS(db.Execute("SELECT name, COUNT(*) FROM emp"));
        CHECK_THROWS(db.Execute("SELEC 1"));
        CHECK_THROWS(db.Execute("UPDATE emp SET id = 2 WHERE id = 1"));
    }
    // reopen: everything must have been persisted
    {
        Database db(path, 64);
        CHECK(Q(db, "SELECT COUNT(*) FROM emp")[0][0].AsInt() == 4);
        CHECK(Q(db, "SELECT name FROM emp WHERE id = 4")[0][0].AsText() == "dan");
        CHECK(db.GetCatalog().GetTable("emp")->row_count == 4);
    }
}

static void TestPlanner() {
    std::cout << "[planner]\n";
    std::string path = TempFile("planner");
    Database db(path, 128);
    db.Execute("CREATE TABLE big (id INT PRIMARY KEY, grp INT, label TEXT);"
               "CREATE TABLE small (id INT PRIMARY KEY, name TEXT);");
    db.Populate("big", 20000);
    db.Populate("small", 50);

    std::string p = Plan(db, "SELECT * FROM big WHERE id = 1234");
    CHECK(p.find("Index Scan using big_pkey") != std::string::npos);
    CHECK(Q(db, "SELECT label FROM big WHERE id = 1234").size() == 1);

    p = Plan(db, "SELECT * FROM big WHERE id BETWEEN 100 AND 199");
    CHECK(p.find("Index Scan") != std::string::npos);
    CHECK(Q(db, "SELECT id FROM big WHERE id BETWEEN 100 AND 199").size() == 100);
    CHECK(Q(db, "SELECT id FROM big WHERE id > 19990").size() == 10);
    CHECK(Q(db, "SELECT id FROM big WHERE 19990 < id AND id <= 19995").size() == 5);

    p = Plan(db, "SELECT * FROM big WHERE grp = 5");  // no index on grp
    CHECK(p.find("Seq Scan on big") != std::string::npos);
    db.Execute("CREATE INDEX big_grp ON big (grp)");
    p = Plan(db, "SELECT * FROM big WHERE grp = 5");
    CHECK(p.find("Index Scan using big_grp") != std::string::npos);
    auto via_index = Q(db, "SELECT COUNT(*) FROM big WHERE grp = 5")[0][0].AsInt();
    auto via_scan = Q(db, "SELECT COUNT(*) FROM big WHERE grp + 0 = 5")[0][0].AsInt();
    CHECK(via_index == via_scan && via_index > 0);

    // equi-join -> hash join, with the small table as build side
    p = Plan(db, "SELECT * FROM big b JOIN small s ON b.grp = s.id");
    CHECK(p.find("Hash Join") != std::string::npos);
    // non-equi join -> nested loop
    p = Plan(db, "SELECT * FROM small a, small b WHERE a.id < b.id");
    CHECK(p.find("Nested Loop") != std::string::npos);
    CHECK(Q(db, "SELECT COUNT(*) FROM small a, small b WHERE a.id < b.id")[0][0].AsInt() == 50 * 49 / 2);

    // hash join and nested loop must agree
    auto hj = Q(db, "SELECT COUNT(*) FROM big b JOIN small s ON b.grp = s.id")[0][0].AsInt();
    auto nl = Q(db, "SELECT COUNT(*) FROM big b JOIN small s ON b.grp + 0 >= s.id AND b.grp <= s.id")[0][0].AsInt();
    CHECK(hj == nl && hj > 0);

    // index maintenance across DELETE / UPDATE
    db.Execute("DELETE FROM big WHERE id <= 10000");
    CHECK(Q(db, "SELECT COUNT(*) FROM big")[0][0].AsInt() == 10000);
    CHECK(Q(db, "SELECT id FROM big WHERE id = 500").empty());
    db.Execute("UPDATE big SET id = id + 100000 WHERE id > 19000");
    CHECK(Q(db, "SELECT COUNT(*) FROM big WHERE id > 100000")[0][0].AsInt() == 1000);
    CHECK(db.GetCatalog().GetTable("big")->row_count == 10000);
    for (IndexInfo* i : db.GetCatalog().IndexesOf("big")) CHECK(i->tree->CheckAndCount() == 10000);
}

int main() {
    try {
        TestBufferPool();
        TestTablePage();
        TestBPlusTree();
        TestSql();
        TestPlanner();
    } catch (const std::exception& e) {
        std::cerr << "unexpected exception: " << e.what() << "\n";
        ++g_failed;
    }
    std::cout << "\n" << g_passed << " checks passed, " << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
