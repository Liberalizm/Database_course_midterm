// Integration tests: FOREIGN KEY constraints through the Database API.
#include <random>

#include "database.h"
#include "test_util.h"

namespace minidb {
namespace {

using testing::TempFile;

class ForeignKeyTest : public ::testing::Test {
protected:
    TempFile file;
    std::unique_ptr<Database> db = std::make_unique<Database>(file.path(), 64);

    void Exec(const std::string& sql) { db->Execute(sql); }
    std::vector<Row> Q(const std::string& sql) { return db->Execute(sql).back().rows; }
    std::string Msg(const std::string& sql) { return db->Execute(sql).back().message; }
    int64_t Scalar(const std::string& sql) {
        auto rows = Q(sql);
        EXPECT_EQ(rows.size(), 1u) << sql;
        return rows.at(0).at(0).AsInt();
    }
    // The error message of a statement that must fail.
    std::string Err(const std::string& sql) {
        try {
            db->Execute(sql);
        } catch (const DbError& e) {
            return e.what();
        }
        ADD_FAILURE() << "expected an error: " << sql;
        return "";
    }
    // "1,2,3": the first column of a query, NULL as "null".
    std::string Col(const std::string& sql) {
        std::string out;
        for (const Row& r : Q(sql)) out += (out.empty() ? "" : ",") + (r[0].IsNull() ? "null" : r[0].ToString());
        return out;
    }
    void Reopen() {
        db.reset();
        db = std::make_unique<Database>(file.path(), 64);
    }
    void ExpectIndexesConsistent(const std::string& table) {
        TableInfo* t = db->GetCatalog().RequireTable(table);
        for (IndexInfo* idx : db->GetCatalog().IndexesOf(table)) {
            int64_t non_null = Scalar("SELECT COUNT(" + idx->column + ") FROM " + table);
            EXPECT_EQ(static_cast<int64_t>(idx->tree->CheckAndCount()), non_null) << idx->name;
        }
        EXPECT_EQ(t->row_count, Scalar("SELECT COUNT(*) FROM " + table)) << "row_count statistic drifted";
    }
    // No row of `child` references a missing `parent` key.
    void ExpectNoOrphans(const std::string& child, const std::string& col, const std::string& parent,
                         const std::string& pcol) {
        EXPECT_EQ(Scalar("SELECT COUNT(*) FROM " + child + " WHERE " + col + " IS NOT NULL AND " + col +
                         " NOT IN (SELECT " + pcol + " FROM " + parent + " WHERE " + pcol + " IS NOT NULL)"),
                  0)
            << child << "." << col << " -> " << parent << "." << pcol;
    }

    void SeedCompany(const std::string& on_delete = "", const std::string& on_update = "") {
        Exec("CREATE TABLE dept (id INT PRIMARY KEY, name TEXT);"
             "CREATE TABLE emp (id INT PRIMARY KEY, name TEXT, dept_id INT REFERENCES dept " +
             on_delete + " " + on_update +
             ");"
             "INSERT INTO dept VALUES (1, 'eng'), (2, 'ops'), (3, 'hr');"
             "INSERT INTO emp VALUES (1, 'ann', 1), (2, 'bob', 1), (3, 'cid', 2), (4, 'dan', NULL);");
    }
};

TEST_F(ForeignKeyTest, InsertMustReferenceAnExistingRow) {
    SeedCompany();
    EXPECT_EQ(Err("INSERT INTO emp VALUES (5, 'eve', 9)"),
              "insert or update on table \"emp\" violates foreign key constraint \"emp_dept_id_fkey\": "
              "key (dept_id)=(9) is not present in table \"dept\"");
    EXPECT_EQ(Msg("INSERT INTO emp VALUES (5, 'eve', 3)"), "INSERT 1");
    EXPECT_EQ(Msg("INSERT INTO emp VALUES (6, 'fay', NULL)"), "INSERT 1");  // NULL references nothing
    // One bad row rejects the whole statement.
    EXPECT_NE(Err("INSERT INTO emp VALUES (7, 'gus', 1), (8, 'hal', 42)").find("not present"), std::string::npos);
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM emp"), 6);
    ExpectIndexesConsistent("emp");
}

TEST_F(ForeignKeyTest, InsertSelectAndUpdateOfTheReferencingColumn) {
    SeedCompany();
    EXPECT_EQ(Msg("INSERT INTO emp (id, dept_id) SELECT id + 10, id FROM dept"), "INSERT 3");
    EXPECT_NE(Err("INSERT INTO emp (id, dept_id) SELECT id + 20, id + 1 FROM dept").find("(dept_id)=(4)"),
              std::string::npos);
    EXPECT_EQ(Msg("UPDATE emp SET dept_id = 3 WHERE id = 1"), "UPDATE 1");
    EXPECT_NE(Err("UPDATE emp SET dept_id = dept_id + 1").find("violates foreign key constraint"), std::string::npos);
    EXPECT_EQ(Col("SELECT dept_id FROM emp WHERE id <= 4 ORDER BY id"), "3,1,2,null");
}

TEST_F(ForeignKeyTest, DeleteOrKeyChangeOfAReferencedRowIsRejectedByDefault) {
    SeedCompany();
    EXPECT_EQ(Err("DELETE FROM dept WHERE id = 1"),
              "update or delete on table \"dept\" violates foreign key constraint \"emp_dept_id_fkey\" on table "
              "\"emp\": key (id)=(1) is still referenced from table \"emp\"");
    EXPECT_NE(Err("UPDATE dept SET id = 10 WHERE id = 2").find("still referenced"), std::string::npos);
    EXPECT_NE(Err("DELETE FROM dept").find("still referenced"), std::string::npos);
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM dept"), 3);
    // Unreferenced rows, and columns other than the key, are free to change.
    EXPECT_EQ(Msg("UPDATE dept SET name = 'engineering' WHERE id = 1"), "UPDATE 1");
    EXPECT_EQ(Msg("UPDATE dept SET id = 30 WHERE id = 3"), "UPDATE 1");
    EXPECT_EQ(Msg("DELETE FROM dept WHERE id = 30"), "DELETE 1");
    // Once nothing references it, a row can go.
    Exec("DELETE FROM emp WHERE dept_id = 2");
    EXPECT_EQ(Msg("DELETE FROM dept WHERE id = 2"), "DELETE 1");
    ExpectIndexesConsistent("dept");
}

TEST_F(ForeignKeyTest, ConstraintIsCheckedAtTheEndOfTheStatement) {
    SeedCompany("ON DELETE RESTRICT", "ON UPDATE RESTRICT");
    // Keys 1 and 2 swap places: every referenced key still exists afterwards.
    EXPECT_EQ(Msg("UPDATE dept SET id = CASE id WHEN 1 THEN 2 WHEN 2 THEN 1 ELSE id END"), "UPDATE 3");
    EXPECT_EQ(Col("SELECT name FROM dept ORDER BY id"), "ops,eng,hr");
    // A key shift: 1,2,3 -> 2,3,4 leaves key 1 unreferenced... but emp references 1.
    EXPECT_NE(Err("UPDATE dept SET id = id + 1").find("(id)=(1)"), std::string::npos);
    ExpectIndexesConsistent("dept");
}

TEST_F(ForeignKeyTest, OnDeleteCascade) {
    SeedCompany("ON DELETE CASCADE");
    Exec("CREATE INDEX emp_dept ON emp (dept_id)");  // the cascade finds children through it
    EXPECT_EQ(Msg("DELETE FROM dept WHERE id = 1"), "DELETE 1");  // the message counts the statement's own rows
    EXPECT_EQ(Col("SELECT id FROM emp ORDER BY id"), "3,4");
    EXPECT_EQ(Msg("DELETE FROM dept"), "DELETE 2");
    EXPECT_EQ(Col("SELECT id FROM emp ORDER BY id"), "4");  // the NULL reference stays
    ExpectIndexesConsistent("emp");
    ExpectIndexesConsistent("dept");
}

TEST_F(ForeignKeyTest, CascadeChainsThroughSeveralTables) {
    Exec("CREATE TABLE a (id INT PRIMARY KEY);"
         "CREATE TABLE b (id INT PRIMARY KEY, a_id INT REFERENCES a ON DELETE CASCADE);"
         "CREATE TABLE c (id INT PRIMARY KEY, b_id INT REFERENCES b ON DELETE CASCADE);"
         "CREATE TABLE d (id INT PRIMARY KEY, c_id INT REFERENCES c ON DELETE SET NULL);"
         "INSERT INTO a VALUES (1), (2);"
         "INSERT INTO b VALUES (10, 1), (11, 1), (12, 2);"
         "INSERT INTO c VALUES (100, 10), (101, 11), (102, 12);"
         "INSERT INTO d VALUES (1000, 100), (1001, 102);");
    EXPECT_EQ(Msg("DELETE FROM a WHERE id = 1"), "DELETE 1");
    EXPECT_EQ(Col("SELECT id FROM b ORDER BY id"), "12");
    EXPECT_EQ(Col("SELECT id FROM c ORDER BY id"), "102");
    EXPECT_EQ(Col("SELECT c_id FROM d ORDER BY id"), "null,102");
    for (const char* t : {"a", "b", "c", "d"}) ExpectIndexesConsistent(t);
}

TEST_F(ForeignKeyTest, ACascadeThatHitsARestrictionChangesNothing) {
    Exec("CREATE TABLE a (id INT PRIMARY KEY);"
         "CREATE TABLE b (id INT PRIMARY KEY, a_id INT REFERENCES a ON DELETE CASCADE);"
         "CREATE TABLE c (id INT PRIMARY KEY, b_id INT REFERENCES b);"  // NO ACTION
         "INSERT INTO a VALUES (1), (2);"
         "INSERT INTO b VALUES (10, 1), (11, 1), (12, 2);"
         "INSERT INTO c VALUES (100, 11);");
    EXPECT_NE(Err("DELETE FROM a WHERE id = 1").find("on table \"c\": key (id)=(11)"), std::string::npos);
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM a"), 2);
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM b"), 3);
    EXPECT_EQ(Msg("DELETE FROM a WHERE id = 2"), "DELETE 1");
    EXPECT_EQ(Col("SELECT id FROM b ORDER BY id"), "10,11");
    for (const char* t : {"a", "b", "c"}) ExpectIndexesConsistent(t);
}

TEST_F(ForeignKeyTest, OnDeleteSetNullAndSetDefault) {
    Exec("CREATE TABLE dept (id INT PRIMARY KEY);"
         "CREATE TABLE emp (id INT PRIMARY KEY, d1 INT REFERENCES dept ON DELETE SET NULL,"
         "                  d2 INT DEFAULT 0 REFERENCES dept ON DELETE SET DEFAULT);"
         "INSERT INTO dept VALUES (0), (1), (2);"
         "INSERT INTO emp VALUES (1, 1, 1), (2, 2, 2), (3, 1, 2);");
    EXPECT_EQ(Msg("DELETE FROM dept WHERE id = 1"), "DELETE 1");
    EXPECT_EQ(Col("SELECT d1 FROM emp ORDER BY id"), "null,2,null");
    EXPECT_EQ(Col("SELECT d2 FROM emp ORDER BY id"), "0,2,2");
    // SET DEFAULT must still point at an existing row: the default 0 is gone now.
    Exec("UPDATE emp SET d2 = 2 WHERE id = 1");
    Exec("DELETE FROM dept WHERE id = 0");
    EXPECT_NE(Err("DELETE FROM dept WHERE id = 2").find("key (d2)=(0) is not present"), std::string::npos);
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM dept"), 1);
    EXPECT_EQ(Col("SELECT d1 FROM emp ORDER BY id"), "null,2,null");
    ExpectIndexesConsistent("emp");
}

TEST_F(ForeignKeyTest, SetNullIsRejectedForANotNullColumn) {
    Exec("CREATE TABLE p (id INT PRIMARY KEY)");
    EXPECT_NE(Err("CREATE TABLE c (pid INT NOT NULL REFERENCES p ON DELETE SET NULL)").find("cannot SET NULL"),
              std::string::npos);
    EXPECT_EQ(db->GetCatalog().GetTable("c"), nullptr);  // the failed CREATE TABLE left nothing behind
}

TEST_F(ForeignKeyTest, OnUpdateCascade) {
    SeedCompany("", "ON UPDATE CASCADE");
    EXPECT_EQ(Msg("UPDATE dept SET id = 10 WHERE id = 1"), "UPDATE 1");
    EXPECT_EQ(Col("SELECT dept_id FROM emp ORDER BY id"), "10,10,2,null");
    // A shift moves every key onto one that existed before; children follow
    // their own parent's new key, not whatever row held that key before.
    EXPECT_EQ(Msg("UPDATE dept SET id = id + 1"), "UPDATE 3");
    EXPECT_EQ(Col("SELECT dept_id FROM emp ORDER BY id"), "11,11,3,null");
    EXPECT_EQ(Col("SELECT d.name FROM emp e JOIN dept d ON e.dept_id = d.id ORDER BY e.id"), "eng,eng,ops");
    // Deleting is still restricted (ON DELETE defaults to NO ACTION).
    EXPECT_NE(Err("DELETE FROM dept WHERE id = 11").find("still referenced"), std::string::npos);
    ExpectIndexesConsistent("emp");
}

TEST_F(ForeignKeyTest, OnUpdateSetNull) {
    SeedCompany("", "ON UPDATE SET NULL");
    Exec("UPDATE dept SET id = 7 WHERE id = 1");
    EXPECT_EQ(Col("SELECT dept_id FROM emp ORDER BY id"), "null,null,2,null");
}

TEST_F(ForeignKeyTest, SelfReference) {
    Exec("CREATE TABLE node (id INT PRIMARY KEY, parent INT REFERENCES node (id) ON DELETE CASCADE)");
    // A row may reference a row of the same statement.
    EXPECT_EQ(Msg("INSERT INTO node VALUES (1, NULL), (2, 1), (3, 2), (4, 3), (5, 1), (6, 6)"), "INSERT 6");
    EXPECT_NE(Err("INSERT INTO node VALUES (7, 8)").find("not present"), std::string::npos);
    // Deleting a node deletes its whole subtree.
    EXPECT_EQ(Msg("DELETE FROM node WHERE id = 2"), "DELETE 1");
    EXPECT_EQ(Col("SELECT id FROM node ORDER BY id"), "1,5,6");
    EXPECT_EQ(Msg("DELETE FROM node WHERE id = 6"), "DELETE 1");  // references itself
    ExpectIndexesConsistent("node");
}

TEST_F(ForeignKeyTest, SelfReferenceRestrict) {
    Exec("CREATE TABLE emp (id INT PRIMARY KEY, boss INT REFERENCES emp);"
         "INSERT INTO emp VALUES (1, NULL), (2, 1), (3, 1)");
    EXPECT_NE(Err("DELETE FROM emp WHERE id = 1").find("still referenced"), std::string::npos);
    // Every row of the statement is gone at the end: nothing is left referencing.
    EXPECT_EQ(Msg("DELETE FROM emp WHERE id IN (1, 2, 3)"), "DELETE 3");
    Exec("INSERT INTO emp VALUES (1, NULL), (2, 1)");
    // The boss's key changes, the reference doesn't: rejected, although row 2 is updated too.
    EXPECT_NE(Err("UPDATE emp SET id = id + 10").find("(id)=(1)"), std::string::npos);
    EXPECT_EQ(Msg("UPDATE emp SET id = id + 10, boss = boss + 10"), "UPDATE 2");
    EXPECT_EQ(Col("SELECT boss FROM emp ORDER BY id"), "null,11");
}

TEST_F(ForeignKeyTest, SelfReferenceUpdateCascade) {
    Exec("CREATE TABLE emp (id INT PRIMARY KEY, boss INT REFERENCES emp ON UPDATE CASCADE);"
         "INSERT INTO emp VALUES (1, NULL), (2, 1), (3, 2), (4, 4)");
    EXPECT_EQ(Msg("UPDATE emp SET id = id * 10"), "UPDATE 4");
    EXPECT_EQ(Col("SELECT boss FROM emp ORDER BY id"), "null,10,20,40");
    ExpectIndexesConsistent("emp");
}

TEST_F(ForeignKeyTest, TextKeys) {
    Exec("CREATE TABLE country (code TEXT PRIMARY KEY, name TEXT);"
         "CREATE TABLE city (name TEXT, country TEXT REFERENCES country ON UPDATE CASCADE ON DELETE CASCADE);"
         "INSERT INTO country VALUES ('de', 'Germany'), ('fr', 'France');"
         "INSERT INTO city VALUES ('Berlin', 'de'), ('Paris', 'fr'), ('Lyon', 'fr');");
    EXPECT_NE(Err("INSERT INTO city VALUES ('Rome', 'it')").find("key (country)=(it)"), std::string::npos);
    Exec("UPDATE country SET code = 'FR' WHERE code = 'fr'");
    EXPECT_EQ(Col("SELECT country FROM city ORDER BY name"), "de,FR,FR");
    Exec("DELETE FROM country WHERE code = 'FR'");
    EXPECT_EQ(Col("SELECT name FROM city"), "Berlin");
}

TEST_F(ForeignKeyTest, ReferencingAUniqueColumn) {
    Exec("CREATE TABLE p (id INT PRIMARY KEY, code INT UNIQUE);"
         "CREATE TABLE c (code INT REFERENCES p (code));"
         "INSERT INTO p VALUES (1, 100), (2, 200);"
         "INSERT INTO c VALUES (100)");
    EXPECT_NE(Err("INSERT INTO c VALUES (1)").find("not present"), std::string::npos);
    EXPECT_NE(Err("DELETE FROM p WHERE id = 1").find("(code)=(100)"), std::string::npos);
}

TEST_F(ForeignKeyTest, InvalidDefinitionsAreRejected) {
    Exec("CREATE TABLE p (id INT PRIMARY KEY, code INT, name TEXT UNIQUE);"
         "CREATE TABLE nopk (x INT)");
    EXPECT_EQ(Err("CREATE TABLE c (a INT REFERENCES missing)"), "referenced table \"missing\" does not exist");
    EXPECT_EQ(Err("CREATE TABLE c (a INT REFERENCES nopk)"), "there is no primary key for referenced table \"nopk\"");
    EXPECT_NE(Err("CREATE TABLE c (a INT REFERENCES p (code))").find("there is no unique constraint"),
              std::string::npos);
    EXPECT_NE(Err("CREATE TABLE c (a INT REFERENCES p (zz))").find("does not exist"), std::string::npos);
    EXPECT_EQ(Err("CREATE TABLE c (a INT REFERENCES p (name))"),
              "foreign key columns \"a\" and \"name\" are of incompatible types: INT and TEXT");
    EXPECT_NE(Err("CREATE TABLE c (a INT, FOREIGN KEY (zz) REFERENCES p)").find("\"zz\""), std::string::npos);
    EXPECT_EQ(Err("CREATE TABLE c (a INT CONSTRAINT f REFERENCES p, b INT CONSTRAINT f REFERENCES p)"),
              "constraint \"f\" already exists");
    // None of the failed statements left a table behind.
    EXPECT_EQ(db->GetCatalog().GetTable("c"), nullptr);
    EXPECT_TRUE(db->GetCatalog().ForeignKeys().empty());
    EXPECT_EQ(Msg("CREATE TABLE c (a INT REFERENCES p)"), "CREATE TABLE");
}

TEST_F(ForeignKeyTest, GeneratedNamesAreUnique) {
    Exec("CREATE TABLE p (id INT PRIMARY KEY, code INT UNIQUE);"
         "CREATE TABLE c (a INT REFERENCES p, FOREIGN KEY (a) REFERENCES p (code))");
    auto fks = db->GetCatalog().ForeignKeysOf("c");
    ASSERT_EQ(fks.size(), 2u);
    EXPECT_EQ(fks[0]->name, "c_a_fkey");
    EXPECT_EQ(fks[1]->name, "c_a_fkey1");
    EXPECT_EQ(fks[1]->ToString(), "FOREIGN KEY (a) REFERENCES p(code)");
}

TEST_F(ForeignKeyTest, DropTableAndIndexAreGuarded) {
    SeedCompany();
    EXPECT_EQ(Err("DROP TABLE dept"),
              "cannot drop table \"dept\" because foreign key constraint \"emp_dept_id_fkey\" on table \"emp\" "
              "references it");
    EXPECT_NE(Err("DROP INDEX dept_pkey").find("requires it"), std::string::npos);
    // Another unique index on the column can take over.
    Exec("CREATE UNIQUE INDEX dept_id_u ON dept (id)");
    EXPECT_EQ(Msg("DROP INDEX dept_pkey"), "DROP INDEX");
    EXPECT_NE(Err("DELETE FROM dept WHERE id = 1").find("still referenced"), std::string::npos);
    EXPECT_NE(Err("DROP INDEX dept_id_u").find("requires it"), std::string::npos);
    // Dropping the referencing table drops its constraints, which frees the parent.
    EXPECT_EQ(Msg("DROP TABLE emp"), "DROP TABLE");
    EXPECT_TRUE(db->GetCatalog().ForeignKeys().empty());
    EXPECT_EQ(Msg("DROP TABLE dept"), "DROP TABLE");
}

TEST_F(ForeignKeyTest, SelfReferencingTableCanBeDropped) {
    Exec("CREATE TABLE node (id INT PRIMARY KEY, parent INT REFERENCES node)");
    EXPECT_EQ(Msg("DROP TABLE node"), "DROP TABLE");
    EXPECT_TRUE(db->GetCatalog().ForeignKeys().empty());
}

TEST_F(ForeignKeyTest, AlterTableAddAndDropConstraint) {
    SeedCompany();
    Exec("ALTER TABLE emp DROP CONSTRAINT emp_dept_id_fkey");
    EXPECT_EQ(Msg("INSERT INTO emp VALUES (5, 'eve', 9)"), "INSERT 1");
    // Existing rows must satisfy a new constraint.
    EXPECT_EQ(Err("ALTER TABLE emp ADD CONSTRAINT emp_dept FOREIGN KEY (dept_id) REFERENCES dept ON DELETE CASCADE"),
              "foreign key constraint \"emp_dept\" is violated by an existing row of table \"emp\": key "
              "(dept_id)=(9) is not present in table \"dept\"");
    EXPECT_TRUE(db->GetCatalog().ForeignKeys().empty());
    Exec("DELETE FROM emp WHERE id = 5");
    EXPECT_EQ(Msg("ALTER TABLE emp ADD CONSTRAINT emp_dept FOREIGN KEY (dept_id) REFERENCES dept ON DELETE CASCADE"),
              "ALTER TABLE");
    Exec("DELETE FROM dept WHERE id = 1");
    EXPECT_EQ(Col("SELECT id FROM emp ORDER BY id"), "3,4");
    EXPECT_EQ(Err("ALTER TABLE dept DROP CONSTRAINT emp_dept"), "constraint \"emp_dept\" of relation \"dept\" does not exist");
    EXPECT_EQ(Err("ALTER TABLE emp DROP CONSTRAINT nope"), "constraint \"nope\" of relation \"emp\" does not exist");
}

TEST_F(ForeignKeyTest, AlterTableAddColumnWithReferences) {
    SeedCompany();
    EXPECT_EQ(Msg("ALTER TABLE emp ADD COLUMN mentor INT REFERENCES emp"), "ALTER TABLE");
    EXPECT_NE(Err("UPDATE emp SET mentor = 99 WHERE id = 1").find("not present"), std::string::npos);
    Exec("UPDATE emp SET mentor = 1 WHERE id = 2");
    // Existing rows get the DEFAULT, which must be a key of the referenced table.
    EXPECT_NE(Err("ALTER TABLE emp ADD COLUMN home INT DEFAULT 9 REFERENCES dept").find("is not present"),
              std::string::npos);
    EXPECT_EQ(db->GetCatalog().RequireTable("emp")->schema.Find("", "home"), -1);  // nothing changed
    EXPECT_EQ(Msg("ALTER TABLE emp ADD COLUMN home INT DEFAULT 3 REFERENCES dept"), "ALTER TABLE");
    EXPECT_EQ(Col("SELECT home FROM emp ORDER BY id"), "3,3,3,3");
    EXPECT_NE(Err("DELETE FROM dept WHERE id = 3").find("emp_home_fkey"), std::string::npos);
}

TEST_F(ForeignKeyTest, DropColumnAndRenames) {
    SeedCompany();
    EXPECT_EQ(Err("ALTER TABLE dept DROP COLUMN id"),
              "cannot drop column \"id\" of table \"dept\" because foreign key constraint \"emp_dept_id_fkey\" on "
              "table \"emp\" references it");
    // Renames keep the constraint working.
    Exec("ALTER TABLE dept RENAME COLUMN id TO dept_no");
    Exec("ALTER TABLE dept RENAME TO department");
    Exec("ALTER TABLE emp RENAME COLUMN dept_id TO dno");
    ForeignKey* fk = db->GetCatalog().GetForeignKey("emp_dept_id_fkey");
    ASSERT_NE(fk, nullptr);
    EXPECT_EQ(fk->ToString(), "FOREIGN KEY (dno) REFERENCES department(dept_no)");
    EXPECT_NE(Err("INSERT INTO emp VALUES (9, 'x', 9)").find("not present in table \"department\""), std::string::npos);
    EXPECT_NE(Err("DELETE FROM department WHERE dept_no = 1").find("still referenced"), std::string::npos);
    // The primary key moved along with the table name, so it is still found as such.
    EXPECT_NE(db->GetCatalog().PrimaryKeyIndex("department"), nullptr);
    EXPECT_EQ(Msg("CREATE TABLE x (d INT REFERENCES department)"), "CREATE TABLE");
    // Dropping the referencing column drops its constraint.
    Exec("ALTER TABLE emp DROP COLUMN dno");
    EXPECT_EQ(db->GetCatalog().GetForeignKey("emp_dept_id_fkey"), nullptr);
    Exec("DROP TABLE x");
    EXPECT_EQ(Msg("DROP TABLE department"), "DROP TABLE");
}

TEST_F(ForeignKeyTest, ConstraintsSurviveARestart) {
    SeedCompany("ON DELETE CASCADE", "ON UPDATE SET NULL");
    Reopen();
    ForeignKey* fk = db->GetCatalog().GetForeignKey("emp_dept_id_fkey");
    ASSERT_NE(fk, nullptr);
    EXPECT_EQ(fk->ToString(), "FOREIGN KEY (dept_id) REFERENCES dept(id) ON DELETE CASCADE ON UPDATE SET NULL");
    EXPECT_NE(Err("INSERT INTO emp VALUES (9, 'x', 9)").find("not present"), std::string::npos);
    Exec("DELETE FROM dept WHERE id = 1");
    Exec("UPDATE dept SET id = 20 WHERE id = 2");
    EXPECT_EQ(Col("SELECT dept_id FROM emp ORDER BY id"), "null,null");
    Exec("ALTER TABLE emp DROP CONSTRAINT emp_dept_id_fkey");
    Reopen();
    EXPECT_TRUE(db->GetCatalog().ForeignKeys().empty());
}

TEST_F(ForeignKeyTest, PopulatePicksExistingKeys) {
    SeedCompany();
    db->Populate("emp", 500);
    EXPECT_EQ(Scalar("SELECT COUNT(*) FROM emp"), 504);
    ExpectNoOrphans("emp", "dept_id", "dept", "id");
    EXPECT_EQ(Scalar("SELECT COUNT(DISTINCT dept_id) FROM emp"), 3);
}

// Random statements against a parent / child / grandchild schema with every
// kind of action; after each statement (failed or not) no row may reference a
// missing key, and the indexes must match the tables.
TEST_F(ForeignKeyTest, RandomizedStatementsKeepEveryInvariant) {
    Exec("CREATE TABLE p (id INT PRIMARY KEY, v INT);"
         "CREATE TABLE c (id INT PRIMARY KEY, pid INT REFERENCES p ON DELETE CASCADE ON UPDATE CASCADE,"
         "                pid2 INT REFERENCES p ON DELETE SET NULL);"
         "CREATE INDEX c_pid ON c (pid);"
         "CREATE TABLE g (id INT PRIMARY KEY, cid INT REFERENCES c ON UPDATE CASCADE,"
         "                gid INT REFERENCES g ON DELETE CASCADE)");
    std::mt19937 rng(7);
    auto r = [&rng](int n) { return std::to_string(static_cast<int>(rng() % static_cast<unsigned>(n))); };
    int ok = 0, failed = 0, cascades = 0;
    for (int step = 0; step < 1500; ++step) {
        std::string sql;
        const unsigned kind = rng() % 12;
        const int64_t children_before = Scalar("SELECT COUNT(*) FROM c");
        auto maybe_null = [&](int n) { return rng() % 3 == 0 ? std::string("NULL") : r(n); };
        switch (kind) {
            case 0:
            case 1: sql = "INSERT INTO p VALUES (" + r(40) + ", " + r(5) + "), (" + r(40) + ", " + r(5) + ")"; break;
            case 2:
            case 3: sql = "INSERT INTO c VALUES (" + r(200) + ", " + r(40) + ", " + maybe_null(40) + ")"; break;
            case 4: sql = "INSERT INTO g VALUES (" + r(60) + ", " + maybe_null(200) + ", " + maybe_null(60) + ")"; break;
            case 5: sql = "DELETE FROM p WHERE id = " + r(40); break;
            case 6: sql = "DELETE FROM p WHERE v = " + r(5) + " AND id % 4 = " + r(4); break;
            case 7: sql = "UPDATE p SET id = id + " + r(5) + " WHERE v = " + r(5); break;
            case 8: sql = "DELETE FROM c WHERE id = " + r(200); break;
            case 9: sql = "UPDATE c SET id = id + 1 WHERE pid = " + r(40); break;
            case 10: sql = "DELETE FROM g WHERE id = " + r(60); break;
            case 11: sql = "UPDATE g SET gid = NULL WHERE id = " + r(60); break;
        }
        try {
            db->Execute(sql);
            ++ok;
            if ((kind == 5 || kind == 6) && Scalar("SELECT COUNT(*) FROM c") < children_before) ++cascades;
        } catch (const DbError&) {
            ++failed;
            EXPECT_EQ(Scalar("SELECT COUNT(*) FROM c"), children_before) << "a failed statement changed c: " << sql;
        }
        ExpectNoOrphans("c", "pid", "p", "id");
        ExpectNoOrphans("c", "pid2", "p", "id");
        ExpectNoOrphans("g", "cid", "c", "id");
        ExpectNoOrphans("g", "gid", "g", "id");
        if (step % 50 == 0)
            for (const char* t : {"p", "c", "g"}) ExpectIndexesConsistent(t);
        if (::testing::Test::HasFailure()) FAIL() << "after step " << step << ": " << sql;
    }
    EXPECT_GT(ok, 100);
    EXPECT_GT(failed, 50);
    EXPECT_GT(cascades, 10);  // deletes in p really did reach into c
    for (const char* t : {"p", "c", "g"}) EXPECT_GT(Scalar(std::string("SELECT COUNT(*) FROM ") + t), 0) << t;
    for (const char* t : {"p", "c", "g"}) ExpectIndexesConsistent(t);
}

}  // namespace
}  // namespace minidb
