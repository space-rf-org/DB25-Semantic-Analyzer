// DB25 Semantic Analyzer - DDL end-to-end tests.
//
// Exercises the full Option-C path: parse a DDL statement, run it through
// execute_ddl (layer 1 statement validation in the analyzer, layer 2 catalog
// integrity in the manager), then assert the resulting catalog state.

#include "db25/parser/parser.hpp"
#include "db25/semantic/catalog.hpp"
#include "db25/semantic/catalog_manager.hpp"
#include "db25/semantic/ddl.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace db25;
using namespace db25::semantic;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool cond, const char* expr, const char* file, int line) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "  FAIL: %s (%s:%d)\n", expr, file, line);
    }
}

#define CHECK(cond) check((cond), #cond, __FILE__, __LINE__)

ast::ASTNode* parse(parser::Parser& p, const std::string& sql) {
    auto r = p.parse(sql);
    return r.has_value() ? r.value() : nullptr;
}

std::string scratch_path(const char* name) {
    const char* dir = std::getenv("TMPDIR");
    std::string base = (dir != nullptr && *dir != '\0') ? dir : "/tmp";
    return base + "/db25_" + name;
}

DdlResult run(parser::Parser& p, CatalogManager& mgr, const std::string& sql) {
    ast::ASTNode* ast = parse(p, sql);
    if (ast == nullptr) return DdlResult{false, "parse failed", mgr.schema_version()};
    return execute_ddl(ast, mgr);
}

void test_create_table_end_to_end() {
    const std::string path = scratch_path("ddl_create.db25cat");
    std::remove(path.c_str());
    std::string load_err;
    CatalogManager mgr(path, load_err);
    parser::Parser p;

    const DdlResult r = run(p, mgr,
        "CREATE TABLE users (id INTEGER NOT NULL PRIMARY KEY, name VARCHAR, "
        "created TIMESTAMP DEFAULT now)");
    CHECK(r.ok);
    CHECK(r.schema_version == 1);

    const TableInfo* t = mgr.catalog().find_table("users");
    CHECK(t != nullptr);
    if (t != nullptr) {
        CHECK(t->columns.size() == 3);
        const ColumnInfo* id = t->find_column("id");
        const ColumnInfo* name = t->find_column("name");
        const ColumnInfo* created = t->find_column("created");
        CHECK(id != nullptr && id->type == DataType::Integer && !id->nullable);
        CHECK(name != nullptr && name->type == DataType::VarChar && name->nullable);
        CHECK(created != nullptr && created->type == DataType::Timestamp &&
              created->has_default);
    }
    // Durable: a fresh manager on the same path recovers the table.
    {
        std::string e;
        CatalogManager mgr2(path, e);
        CHECK(mgr2.catalog().find_table("users") != nullptr);
        CHECK(mgr2.schema_version() == 1);
    }
    std::remove(path.c_str());
}

void test_statement_validation_layer() {
    const std::string path = scratch_path("ddl_validate.db25cat");
    std::remove(path.c_str());
    std::string load_err;
    CatalogManager mgr(path, load_err);
    parser::Parser p;

    // Duplicate column name -> layer-1 failure, catalog untouched.
    const DdlResult dup = run(p, mgr, "CREATE TABLE t (a INTEGER, a TEXT)");
    CHECK(!dup.ok);
    CHECK(mgr.schema_version() == 0);
    CHECK(mgr.catalog().find_table("t") == nullptr);

    // Unknown column type -> layer-1 failure.
    const DdlResult bad = run(p, mgr, "CREATE TABLE t (a NOTAREALTYPE)");
    CHECK(!bad.ok);
    CHECK(mgr.catalog().find_table("t") == nullptr);

    // Two PRIMARY KEYs -> layer-1 failure.
    std::vector<std::string> errs;
    ast::ASTNode* two_pk =
        parse(p, "CREATE TABLE t (a INTEGER PRIMARY KEY, b INTEGER PRIMARY KEY)");
    CHECK(two_pk != nullptr);
    if (two_pk) CHECK(!validate_ddl(two_pk, errs));
    std::remove(path.c_str());
}

void test_catalog_integrity_layer() {
    const std::string path = scratch_path("ddl_integrity.db25cat");
    std::remove(path.c_str());
    std::string load_err;
    CatalogManager mgr(path, load_err);
    parser::Parser p;

    CHECK(run(p, mgr, "CREATE TABLE users (id INTEGER)").ok);   // v1

    // Duplicate table (well-formed statement) -> layer-2 failure at commit.
    const DdlResult dup = run(p, mgr, "CREATE TABLE users (id INTEGER)");
    CHECK(!dup.ok);
    CHECK(mgr.schema_version() == 1);

    // IF NOT EXISTS makes the duplicate a no-op success, no version bump.
    const DdlResult ine = run(p, mgr, "CREATE TABLE IF NOT EXISTS users (id INTEGER)");
    CHECK(ine.ok);
    CHECK(mgr.schema_version() == 1);
    std::remove(path.c_str());
}

void test_drop_table() {
    const std::string path = scratch_path("ddl_drop.db25cat");
    std::remove(path.c_str());
    std::string load_err;
    CatalogManager mgr(path, load_err);
    parser::Parser p;

    CHECK(run(p, mgr, "CREATE TABLE users (id INTEGER)").ok);  // v1
    const DdlResult d = run(p, mgr, "DROP TABLE users");
    CHECK(d.ok);
    CHECK(d.schema_version == 2);
    CHECK(mgr.catalog().find_table("users") == nullptr);

    // DROP of a missing table: error by default, no-op with IF EXISTS.
    CHECK(!run(p, mgr, "DROP TABLE users").ok);
    CHECK(run(p, mgr, "DROP TABLE IF EXISTS users").ok);
    CHECK(mgr.schema_version() == 2);
    std::remove(path.c_str());
}

void test_create_index_end_to_end() {
    const std::string path = scratch_path("ddl_index.db25cat");
    std::remove(path.c_str());
    std::string load_err;
    CatalogManager mgr(path, load_err);
    parser::Parser p;

    CHECK(run(p, mgr, "CREATE TABLE users (id INTEGER, name VARCHAR)").ok);  // v1
    const DdlResult r = run(p, mgr, "CREATE UNIQUE INDEX uq_name ON users (name)");
    CHECK(r.ok);
    CHECK(r.schema_version == 2);
    const IndexInfo* idx = mgr.catalog().find_index("uq_name");
    CHECK(idx != nullptr);
    if (idx != nullptr) {
        CHECK(idx->table == "users");
        CHECK(idx->unique);
        CHECK(idx->columns.size() == 1);
        // Resolved to the name column's id.
        const TableInfo* t = mgr.catalog().find_table("users");
        CHECK(t != nullptr && idx->columns[0] == t->find_column("name")->column_id);
    }
    // Durable across restart.
    {
        std::string e;
        CatalogManager mgr2(path, e);
        CHECK(mgr2.catalog().find_index("uq_name") != nullptr);
        CHECK(mgr2.schema_version() == 2);
    }
    std::remove(path.c_str());
}

void test_create_index_integrity() {
    const std::string path = scratch_path("ddl_index_integ.db25cat");
    std::remove(path.c_str());
    std::string load_err;
    CatalogManager mgr(path, load_err);
    parser::Parser p;
    CHECK(run(p, mgr, "CREATE TABLE users (id INTEGER)").ok);       // v1
    CHECK(run(p, mgr, "CREATE INDEX i1 ON users (id)").ok);          // v2

    // Missing target table -> layer-2 failure.
    CHECK(!run(p, mgr, "CREATE INDEX i2 ON nosuch (id)").ok);
    // Unknown column -> layer-2 failure.
    CHECK(!run(p, mgr, "CREATE INDEX i3 ON users (nope)").ok);
    // Duplicate index name -> layer-2 failure; IF NOT EXISTS is a no-op.
    CHECK(!run(p, mgr, "CREATE INDEX i1 ON users (id)").ok);
    CHECK(run(p, mgr, "CREATE INDEX IF NOT EXISTS i1 ON users (id)").ok);
    CHECK(mgr.schema_version() == 2);  // none of the above bumped the version
    std::remove(path.c_str());
}

void test_drop_index() {
    const std::string path = scratch_path("ddl_dropidx.db25cat");
    std::remove(path.c_str());
    std::string load_err;
    CatalogManager mgr(path, load_err);
    parser::Parser p;
    CHECK(run(p, mgr, "CREATE TABLE users (id INTEGER)").ok);   // v1
    CHECK(run(p, mgr, "CREATE INDEX i1 ON users (id)").ok);      // v2
    const DdlResult d = run(p, mgr, "DROP INDEX i1");
    CHECK(d.ok);
    CHECK(d.schema_version == 3);
    CHECK(mgr.catalog().find_index("i1") == nullptr);
    CHECK(!run(p, mgr, "DROP INDEX i1").ok);                     // missing -> error
    CHECK(run(p, mgr, "DROP INDEX IF EXISTS i1").ok);            // no-op success
    std::remove(path.c_str());
}

const Constraint* first_fk(const TableInfo* t) {
    if (t == nullptr) return nullptr;
    for (const Constraint& c : t->constraints) {
        if (c.kind == Constraint::Kind::ForeignKey) return &c;
    }
    return nullptr;
}

void test_foreign_key_column_and_table_level() {
    const std::string path = scratch_path("ddl_fk.db25cat");
    std::remove(path.c_str());
    std::string load_err;
    CatalogManager mgr(path, load_err);
    parser::Parser p;

    CHECK(run(p, mgr, "CREATE TABLE users (id INTEGER PRIMARY KEY, name VARCHAR)").ok);  // v1

    // Column-level FK.
    CHECK(run(p, mgr, "CREATE TABLE orders (uid INTEGER REFERENCES users (id))").ok);   // v2
    const TableInfo* orders = mgr.catalog().find_table("orders");
    const Constraint* fk = first_fk(orders);
    CHECK(fk != nullptr);
    if (fk != nullptr && orders != nullptr) {
        CHECK(fk->ref_table == "users");
        CHECK(fk->columns.size() == 1 &&
              fk->columns[0] == orders->find_column("uid")->column_id);
        const TableInfo* users = mgr.catalog().find_table("users");
        CHECK(fk->ref_columns.size() == 1 &&
              fk->ref_columns[0] == users->find_column("id")->column_id);
    }

    // Table-level FK.
    CHECK(run(p, mgr,
        "CREATE TABLE carts (uid INTEGER, FOREIGN KEY (uid) REFERENCES users (id))").ok);  // v3
    CHECK(first_fk(mgr.catalog().find_table("carts")) != nullptr);

    // Durable across restart.
    {
        std::string e;
        CatalogManager mgr2(path, e);
        CHECK(first_fk(mgr2.catalog().find_table("orders")) != nullptr);
        CHECK(first_fk(mgr2.catalog().find_table("carts")) != nullptr);
    }
    std::remove(path.c_str());
}

void test_foreign_key_integrity() {
    const std::string path = scratch_path("ddl_fk_integ.db25cat");
    std::remove(path.c_str());
    std::string load_err;
    CatalogManager mgr(path, load_err);
    parser::Parser p;
    CHECK(run(p, mgr, "CREATE TABLE users (id INTEGER)").ok);  // v1

    // Referenced table does not exist -> rejected at commit, catalog untouched.
    CHECK(!run(p, mgr, "CREATE TABLE a (uid INTEGER REFERENCES nosuch (id))").ok);
    CHECK(mgr.catalog().find_table("a") == nullptr);
    CHECK(mgr.schema_version() == 1);

    // Referenced column does not exist -> rejected.
    CHECK(!run(p, mgr, "CREATE TABLE b (uid INTEGER REFERENCES users (nope))").ok);
    CHECK(mgr.catalog().find_table("b") == nullptr);
    std::remove(path.c_str());
}

void test_self_referencing_foreign_key() {
    const std::string path = scratch_path("ddl_fk_self.db25cat");
    std::remove(path.c_str());
    std::string load_err;
    CatalogManager mgr(path, load_err);
    parser::Parser p;
    // A self-referencing FK resolves because the FK is checked against the new
    // catalog, which already includes the table being created.
    const DdlResult r = run(p, mgr,
        "CREATE TABLE emp (id INTEGER PRIMARY KEY, mgr INTEGER REFERENCES emp (id))");
    CHECK(r.ok);
    const Constraint* fk = first_fk(mgr.catalog().find_table("emp"));
    CHECK(fk != nullptr && fk->ref_table == "emp");
    std::remove(path.c_str());
}

void test_drop_restrict_and_cascade() {
    const std::string path = scratch_path("ddl_dropdep.db25cat");
    std::remove(path.c_str());
    std::string load_err;
    CatalogManager mgr(path, load_err);
    parser::Parser p;
    CHECK(run(p, mgr, "CREATE TABLE users (id INTEGER PRIMARY KEY)").ok);            // v1
    CHECK(run(p, mgr, "CREATE TABLE orders (uid INTEGER REFERENCES users (id))").ok); // v2

    // RESTRICT is the default: dropping the referenced table is refused.
    CHECK(!run(p, mgr, "DROP TABLE users").ok);
    CHECK(mgr.catalog().find_table("users") != nullptr);
    CHECK(mgr.schema_version() == 2);

    // CASCADE drops the table and detaches the dependent FK from orders.
    const DdlResult c = run(p, mgr, "DROP TABLE users CASCADE");
    CHECK(c.ok);
    CHECK(mgr.catalog().find_table("users") == nullptr);
    CHECK(first_fk(mgr.catalog().find_table("orders")) == nullptr);  // FK detached
    CHECK(mgr.catalog().find_table("orders") != nullptr);            // orders itself remains
    std::remove(path.c_str());
}

void test_drop_self_ref_and_index_cleanup() {
    const std::string path = scratch_path("ddl_dropself.db25cat");
    std::remove(path.c_str());
    std::string load_err;
    CatalogManager mgr(path, load_err);
    parser::Parser p;
    // A self-referencing FK is not an external dependency: DROP (RESTRICT) works.
    CHECK(run(p, mgr,
        "CREATE TABLE emp (id INTEGER PRIMARY KEY, mgr INTEGER REFERENCES emp (id))").ok);
    CHECK(run(p, mgr, "CREATE INDEX idx_mgr ON emp (mgr)").ok);
    CHECK(mgr.catalog().find_index("idx_mgr") != nullptr);
    CHECK(run(p, mgr, "DROP TABLE emp").ok);
    CHECK(mgr.catalog().find_table("emp") == nullptr);
    // The table's index is dropped with it (no dangling index metadata).
    CHECK(mgr.catalog().find_index("idx_mgr") == nullptr);
    std::remove(path.c_str());
}

void test_check_default_validation() {
    parser::Parser p;
    std::vector<std::string> e;

    // CHECK may reference only this table's own columns.
    e.clear();
    CHECK(validate_ddl(parse(p, "CREATE TABLE t (age INTEGER CHECK (age >= 18))"), e));
    e.clear();
    CHECK(!validate_ddl(parse(p, "CREATE TABLE t (age INTEGER CHECK (height >= 18))"), e));

    // Table-level CHECK over own columns is valid; an unknown column is not.
    e.clear();
    CHECK(validate_ddl(parse(p, "CREATE TABLE t (a INTEGER, b INTEGER, CHECK (a < b))"), e));
    e.clear();
    CHECK(!validate_ddl(parse(p, "CREATE TABLE t (a INTEGER, b INTEGER, CHECK (a < zzz))"), e));

    // DEFAULT: a literal or a function/constant is fine; referencing another
    // column of the same table is not.
    e.clear();
    CHECK(validate_ddl(parse(p, "CREATE TABLE t (a INTEGER DEFAULT 0)"), e));
    e.clear();
    CHECK(validate_ddl(parse(p, "CREATE TABLE t (a TIMESTAMP DEFAULT now())"), e));
    e.clear();
    CHECK(!validate_ddl(parse(p, "CREATE TABLE t (a INTEGER DEFAULT b, b INTEGER)"), e));
}

// The CHECK expression text and the DEFAULT expression text captured by the
// parser must be persisted into the catalog (and survive a snapshot reload),
// so the constraint/default can be reproduced faithfully downstream.
void test_check_default_persistence() {
    const std::string path = scratch_path("ddl_expr_persist.db25cat");
    std::remove(path.c_str());
    std::string load_err;
    CatalogManager mgr(path, load_err);
    parser::Parser p;

    const DdlResult r = run(p, mgr,
        "CREATE TABLE t (age INTEGER DEFAULT 0 CHECK (age >= 0), "
        "b INTEGER, CHECK (age < b))");
    CHECK(r.ok);

    auto check_expr = [](const std::string& s) {
        return [s](const Constraint& c) {
            return c.kind == Constraint::Kind::Check && c.expr == s;
        };
    };
    auto has_check = [&](const TableInfo& t, const std::string& expr) {
        for (const Constraint& c : t.constraints) {
            if (check_expr(expr)(c)) return true;
        }
        return false;
    };

    const TableInfo* t = mgr.catalog().find_table("t");
    CHECK(t != nullptr);
    if (t != nullptr) {
        const ColumnInfo* age = t->find_column("age");
        CHECK(age != nullptr && age->has_default && age->default_expr == "0");
        // Both the column-level and the table-level CHECK are persisted verbatim.
        CHECK(has_check(*t, "age >= 0"));
        CHECK(has_check(*t, "age < b"));
    }

    // Durable: the expression text survives a snapshot reload verbatim.
    {
        std::string e;
        CatalogManager mgr2(path, e);
        const TableInfo* t2 = mgr2.catalog().find_table("t");
        CHECK(t2 != nullptr);
        if (t2 != nullptr) {
            const ColumnInfo* age = t2->find_column("age");
            CHECK(age != nullptr && age->default_expr == "0");
            CHECK(has_check(*t2, "age >= 0"));
            CHECK(has_check(*t2, "age < b"));
        }
    }
    std::remove(path.c_str());
}

// Layer-1 type checking (third validation pass): a CHECK predicate must be
// Boolean; a DEFAULT value must be assignment-compatible with its column type.
void test_check_default_typecheck() {
    parser::Parser p;
    std::vector<std::string> e;

    // --- CHECK must be Boolean ---
    // A comparison is Boolean: accepted.
    e.clear();
    CHECK(validate_ddl(parse(p, "CREATE TABLE t (age INTEGER CHECK (age >= 0))"), e));
    // A bare Boolean column is Boolean: accepted.
    e.clear();
    CHECK(validate_ddl(parse(p, "CREATE TABLE t (active BOOLEAN CHECK (active))"), e));
    // A bare Integer column is not Boolean: rejected.
    e.clear();
    CHECK(!validate_ddl(parse(p, "CREATE TABLE t (age INTEGER CHECK (age))"), e));
    // A table-level arithmetic CHECK is numeric, not Boolean: rejected.
    e.clear();
    CHECK(!validate_ddl(parse(p, "CREATE TABLE t (a INTEGER, b INTEGER, CHECK (a + b))"), e));

    // --- DEFAULT must be assignment-compatible with the column ---
    // Same category: accepted.
    e.clear();
    CHECK(validate_ddl(parse(p, "CREATE TABLE t (a INTEGER DEFAULT 0)"), e));
    // now() is a Timestamp; storing it into a TIMESTAMP is fine.
    e.clear();
    CHECK(validate_ddl(parse(p, "CREATE TABLE t (ts TIMESTAMP DEFAULT now())"), e));
    // A NULL default unifies with anything: accepted.
    e.clear();
    CHECK(validate_ddl(parse(p, "CREATE TABLE t (a INTEGER DEFAULT NULL)"), e));
    // Numeric<->string is a soft assignment conversion (as for an INSERT value):
    // accepted, mirroring the analyzer's coercion model.
    e.clear();
    CHECK(validate_ddl(parse(p, "CREATE TABLE t (a INTEGER DEFAULT '5')"), e));
    // A Timestamp default into an INTEGER column crosses categories: rejected.
    e.clear();
    CHECK(!validate_ddl(parse(p, "CREATE TABLE t (a INTEGER DEFAULT now())"), e));
    // An Integer default into a BOOLEAN column crosses categories: rejected.
    e.clear();
    CHECK(!validate_ddl(parse(p, "CREATE TABLE t (a BOOLEAN DEFAULT 0)"), e));
}

// ALTER TABLE ADD COLUMN: appends a column with a fresh id, persists a DEFAULT,
// rejects a duplicate name and a bad type, and is durable across a reload.
void test_alter_add_column() {
    const std::string path = scratch_path("ddl_alter_add.db25cat");
    std::remove(path.c_str());
    std::string load_err;
    CatalogManager mgr(path, load_err);
    parser::Parser p;

    CHECK(run(p, mgr, "CREATE TABLE t (id INTEGER PRIMARY KEY)").ok);
    const DdlResult r = run(p, mgr, "ALTER TABLE t ADD COLUMN age INTEGER DEFAULT 0");
    CHECK(r.ok);
    const TableInfo* t = mgr.catalog().find_table("t");
    CHECK(t != nullptr);
    if (t != nullptr) {
        CHECK(t->columns.size() == 2);
        const ColumnInfo* age = t->find_column("age");
        CHECK(age != nullptr && age->type == DataType::Integer);
        CHECK(age != nullptr && age->has_default && age->default_expr == "0");
        // Fresh, distinct column id (never reused).
        const ColumnInfo* id = t->find_column("id");
        CHECK(id != nullptr && age != nullptr && id->column_id != age->column_id);
    }

    // Duplicate column name and adding to a missing table both fail (no change).
    CHECK(!run(p, mgr, "ALTER TABLE t ADD COLUMN age TEXT").ok);
    CHECK(!run(p, mgr, "ALTER TABLE nope ADD COLUMN x INTEGER").ok);
    // Unknown type is a layer-1 rejection.
    CHECK(!run(p, mgr, "ALTER TABLE t ADD COLUMN bad NOTATYPE").ok);
    // A DEFAULT incompatible with the new column's type is rejected.
    CHECK(!run(p, mgr, "ALTER TABLE t ADD COLUMN b BOOLEAN DEFAULT 0").ok);

    // Durable across reload.
    {
        std::string e;
        CatalogManager mgr2(path, e);
        const TableInfo* t2 = mgr2.catalog().find_table("t");
        CHECK(t2 != nullptr && t2->find_column("age") != nullptr);
    }
    std::remove(path.c_str());
}

// ALTER TABLE DROP COLUMN: removes a column and its own index; refuses when the
// only column or an external FK depends on it (RESTRICT) unless CASCADE.
void test_alter_drop_column() {
    const std::string path = scratch_path("ddl_alter_drop.db25cat");
    std::remove(path.c_str());
    std::string load_err;
    CatalogManager mgr(path, load_err);
    parser::Parser p;

    CHECK(run(p, mgr, "CREATE TABLE t (id INTEGER PRIMARY KEY, a INTEGER, b INTEGER)").ok);
    CHECK(run(p, mgr, "CREATE INDEX ix ON t (a)").ok);

    // Dropping 'a' takes its index with it automatically.
    CHECK(run(p, mgr, "ALTER TABLE t DROP COLUMN a").ok);
    const TableInfo* t = mgr.catalog().find_table("t");
    CHECK(t != nullptr && t->find_column("a") == nullptr);
    CHECK(mgr.catalog().find_index("ix") == nullptr);

    // Dropping a missing column fails; the last column cannot be dropped.
    CHECK(!run(p, mgr, "ALTER TABLE t DROP COLUMN a").ok);
    CHECK(run(p, mgr, "ALTER TABLE t DROP COLUMN b").ok);   // now (id) remains
    CHECK(!run(p, mgr, "ALTER TABLE t DROP COLUMN id").ok);  // the only column

    // External FK dependency: RESTRICT refuses, CASCADE drops the FK.
    const std::string path3 = scratch_path("ddl_alter_drop3.db25cat");
    std::remove(path3.c_str());
    CatalogManager mgr3(path3, load_err);
    CHECK(run(p, mgr3, "CREATE TABLE parent (pid INTEGER PRIMARY KEY, note TEXT)").ok);
    CHECK(run(p, mgr3,
        "CREATE TABLE child (cid INTEGER, pid INTEGER REFERENCES parent (pid))").ok);
    // parent.pid is referenced by child's FK: RESTRICT refuses.
    CHECK(!run(p, mgr3, "ALTER TABLE parent DROP COLUMN pid").ok);
    CHECK(mgr3.catalog().find_table("parent")->find_column("pid") != nullptr);
    // CASCADE drops the referencing FK, then the column.
    CHECK(run(p, mgr3, "ALTER TABLE parent DROP COLUMN pid CASCADE").ok);
    CHECK(mgr3.catalog().find_table("parent")->find_column("pid") == nullptr);
    {
        const TableInfo* child = mgr3.catalog().find_table("child");
        bool has_fk = false;
        for (const Constraint& c : child->constraints) {
            if (c.kind == Constraint::Kind::ForeignKey) has_fk = true;
        }
        CHECK(!has_fk);
    }
    std::remove(path3.c_str());
    std::remove(path.c_str());
}

// ALTER TABLE well-formedness: unsupported actions / inline constraints rejected.
void test_alter_validation() {
    parser::Parser p;
    std::vector<std::string> e;
    // A column-level CHECK on an added column is refused (not silently dropped).
    e.clear();
    CHECK(!validate_ddl(parse(p, "ALTER TABLE t ADD COLUMN a INTEGER CHECK (a > 0)"), e));
    // A column-level REFERENCES on an added column is likewise refused.
    e.clear();
    CHECK(!validate_ddl(parse(p, "ALTER TABLE t ADD COLUMN a INTEGER REFERENCES u (x)"), e));
    // NOT NULL and DEFAULT together are fine.
    e.clear();
    CHECK(validate_ddl(parse(p, "ALTER TABLE t ADD COLUMN a INTEGER NOT NULL DEFAULT 0"), e));
}

// ALTER TABLE ALTER COLUMN SET / DROP DEFAULT: sets, replaces, type-checks, and
// clears a column's default; durable across reload.
void test_alter_column_default() {
    const std::string path = scratch_path("ddl_alter_coldef.db25cat");
    std::remove(path.c_str());
    std::string load_err;
    CatalogManager mgr(path, load_err);
    parser::Parser p;

    CHECK(run(p, mgr, "CREATE TABLE t (a INTEGER, ts TIMESTAMP)").ok);

    // SET DEFAULT on a column that had none: text is captured and persisted.
    CHECK(run(p, mgr, "ALTER TABLE t ALTER COLUMN a SET DEFAULT 7").ok);
    {
        const ColumnInfo* a = mgr.catalog().find_table("t")->find_column("a");
        CHECK(a != nullptr && a->has_default && a->default_expr == "7");
    }
    // Replace it.
    CHECK(run(p, mgr, "ALTER TABLE t ALTER COLUMN a SET DEFAULT 9").ok);
    CHECK(mgr.catalog().find_table("t")->find_column("a")->default_expr == "9");

    // A type-incompatible default is rejected (Timestamp value into INTEGER).
    CHECK(!run(p, mgr, "ALTER TABLE t ALTER COLUMN a SET DEFAULT now()").ok);
    CHECK(mgr.catalog().find_table("t")->find_column("a")->default_expr == "9");  // unchanged
    // A compatible default on the TIMESTAMP column is accepted.
    CHECK(run(p, mgr, "ALTER TABLE t ALTER COLUMN ts SET DEFAULT now()").ok);
    CHECK(mgr.catalog().find_table("t")->find_column("ts")->default_expr == "now()");

    // DROP DEFAULT clears it; dropping again is a no-op success.
    CHECK(run(p, mgr, "ALTER TABLE t ALTER COLUMN a DROP DEFAULT").ok);
    {
        const ColumnInfo* a = mgr.catalog().find_table("t")->find_column("a");
        CHECK(a != nullptr && !a->has_default && a->default_expr.empty());
    }
    CHECK(run(p, mgr, "ALTER TABLE t ALTER COLUMN a DROP DEFAULT").ok);  // no-op

    // Missing table / column are rejected.
    CHECK(!run(p, mgr, "ALTER TABLE nope ALTER COLUMN a SET DEFAULT 1").ok);
    CHECK(!run(p, mgr, "ALTER TABLE t ALTER COLUMN ghost DROP DEFAULT").ok);

    // Durable across reload.
    {
        std::string e;
        CatalogManager mgr2(path, e);
        const ColumnInfo* ts = mgr2.catalog().find_table("t")->find_column("ts");
        CHECK(ts != nullptr && ts->has_default && ts->default_expr == "now()");
    }
    std::remove(path.c_str());
}

// ALTER TABLE ALTER COLUMN SET / DROP NOT NULL: toggles the column's
// nullability, is idempotent (no-op when already in that state), and durable.
void test_alter_column_not_null() {
    const std::string path = scratch_path("ddl_alter_notnull.db25cat");
    std::remove(path.c_str());
    std::string load_err;
    CatalogManager mgr(path, load_err);
    parser::Parser p;

    CHECK(run(p, mgr, "CREATE TABLE t (a INTEGER)").ok);
    CHECK(mgr.catalog().find_table("t")->find_column("a")->nullable);  // nullable by default

    // SET NOT NULL flips it.
    CHECK(run(p, mgr, "ALTER TABLE t ALTER COLUMN a SET NOT NULL").ok);
    CHECK(!mgr.catalog().find_table("t")->find_column("a")->nullable);
    // Idempotent: setting again is a no-op success.
    CHECK(run(p, mgr, "ALTER TABLE t ALTER COLUMN a SET NOT NULL").ok);
    CHECK(!mgr.catalog().find_table("t")->find_column("a")->nullable);

    // DROP NOT NULL flips it back.
    CHECK(run(p, mgr, "ALTER TABLE t ALTER COLUMN a DROP NOT NULL").ok);
    CHECK(mgr.catalog().find_table("t")->find_column("a")->nullable);

    // Missing table / column are rejected.
    CHECK(!run(p, mgr, "ALTER TABLE nope ALTER COLUMN a SET NOT NULL").ok);
    CHECK(!run(p, mgr, "ALTER TABLE t ALTER COLUMN ghost SET NOT NULL").ok);

    // Durable across reload.
    CHECK(run(p, mgr, "ALTER TABLE t ALTER COLUMN a SET NOT NULL").ok);
    {
        std::string e;
        CatalogManager mgr2(path, e);
        CHECK(!mgr2.catalog().find_table("t")->find_column("a")->nullable);
    }
    std::remove(path.c_str());
}

// PRIMARY KEY is persisted as a first-class constraint (column-level and
// table-level), its columns are forced NOT NULL, it survives a reload, and it
// interacts correctly with DROP NOT NULL and DROP COLUMN.
void test_primary_key_constraint() {
    auto pk_of = [](const TableInfo& t) -> const Constraint* {
        for (const Constraint& c : t.constraints) {
            if (c.kind == Constraint::Kind::PrimaryKey) return &c;
        }
        return nullptr;
    };

    const std::string path = scratch_path("ddl_pk.db25cat");
    std::remove(path.c_str());
    std::string load_err;
    CatalogManager mgr(path, load_err);
    parser::Parser p;

    // Column-level PRIMARY KEY: a constraint over the one column, forced NOT NULL.
    CHECK(run(p, mgr, "CREATE TABLE t (id INTEGER PRIMARY KEY, a INTEGER)").ok);
    {
        const TableInfo* t = mgr.catalog().find_table("t");
        const Constraint* pk = pk_of(*t);
        const ColumnInfo* id = t->find_column("id");
        CHECK(pk != nullptr && pk->columns.size() == 1 && id != nullptr &&
              pk->columns[0] == id->column_id);
        CHECK(!id->nullable);
        // DROP NOT NULL on a PK column is refused; the column stays NOT NULL.
        CHECK(!run(p, mgr, "ALTER TABLE t ALTER COLUMN id DROP NOT NULL").ok);
        CHECK(!mgr.catalog().find_table("t")->find_column("id")->nullable);
    }

    // Table-level PRIMARY KEY (a, b): both columns forced NOT NULL.
    CHECK(run(p, mgr, "CREATE TABLE t2 (a INTEGER, b INTEGER, PRIMARY KEY (a, b))").ok);
    {
        const TableInfo* t = mgr.catalog().find_table("t2");
        const Constraint* pk = pk_of(*t);
        CHECK(pk != nullptr && pk->columns.size() == 2);
        CHECK(!t->find_column("a")->nullable && !t->find_column("b")->nullable);
    }

    // Two PRIMARY KEY definitions (column + table) are rejected.
    CHECK(!run(p, mgr, "CREATE TABLE t3 (a INTEGER PRIMARY KEY, PRIMARY KEY (a))").ok);
    CHECK(mgr.catalog().find_table("t3") == nullptr);

    // Dropping a PK column removes the PK constraint (own constraint cleanup).
    CHECK(run(p, mgr, "ALTER TABLE t2 DROP COLUMN b").ok);
    CHECK(pk_of(*mgr.catalog().find_table("t2")) == nullptr);

    // Durable: the column-level PK on t survives a reload.
    {
        std::string e;
        CatalogManager mgr2(path, e);
        const TableInfo* t = mgr2.catalog().find_table("t");
        const Constraint* pk = pk_of(*t);
        CHECK(pk != nullptr && pk->columns.size() == 1);
        CHECK(!t->find_column("id")->nullable);
    }
    std::remove(path.c_str());
}

// UNIQUE is persisted as a first-class constraint (column-level and
// table-level), a table may have several, its columns are NOT forced NOT NULL,
// it survives a reload, and it is removed when one of its columns is dropped.
void test_unique_constraint() {
    auto uniques = [](const TableInfo& t) {
        std::vector<const Constraint*> out;
        for (const Constraint& c : t.constraints) {
            if (c.kind == Constraint::Kind::Unique) out.push_back(&c);
        }
        return out;
    };

    const std::string path = scratch_path("ddl_unique.db25cat");
    std::remove(path.c_str());
    std::string load_err;
    CatalogManager mgr(path, load_err);
    parser::Parser p;

    // Column-level UNIQUE: a single-column constraint; the column stays nullable.
    CHECK(run(p, mgr, "CREATE TABLE t (email TEXT UNIQUE, a INTEGER)").ok);
    {
        const TableInfo* t = mgr.catalog().find_table("t");
        const auto us = uniques(*t);
        const ColumnInfo* email = t->find_column("email");
        CHECK(us.size() == 1 && us[0]->columns.size() == 1 &&
              us[0]->columns[0] == email->column_id);
        CHECK(email->nullable);  // UNIQUE does not imply NOT NULL
    }

    // Multiple UNIQUEs, including a composite table-level one.
    CHECK(run(p, mgr,
        "CREATE TABLE t2 (a INTEGER UNIQUE, b INTEGER, c INTEGER, UNIQUE (b, c))").ok);
    {
        const TableInfo* t = mgr.catalog().find_table("t2");
        const auto us = uniques(*t);
        CHECK(us.size() == 2);
        bool has_single = false, has_pair = false;
        for (const Constraint* u : us) {
            if (u->columns.size() == 1) has_single = true;
            if (u->columns.size() == 2) has_pair = true;
        }
        CHECK(has_single && has_pair);
    }

    // Dropping a column of a UNIQUE removes that constraint.
    CHECK(run(p, mgr, "ALTER TABLE t2 DROP COLUMN b").ok);
    {
        const auto us = uniques(*mgr.catalog().find_table("t2"));
        // The composite UNIQUE (b, c) is gone; the single-column UNIQUE (a) stays.
        CHECK(us.size() == 1 && us[0]->columns.size() == 1);
    }

    // Durable across reload.
    {
        std::string e;
        CatalogManager mgr2(path, e);
        CHECK(uniques(*mgr2.catalog().find_table("t")).size() == 1);
    }
    std::remove(path.c_str());
}

}  // namespace

int main() {
    test_create_table_end_to_end();
    test_check_default_validation();
    test_check_default_persistence();
    test_check_default_typecheck();
    test_statement_validation_layer();
    test_catalog_integrity_layer();
    test_drop_table();
    test_create_index_end_to_end();
    test_create_index_integrity();
    test_drop_index();
    test_foreign_key_column_and_table_level();
    test_foreign_key_integrity();
    test_self_referencing_foreign_key();
    test_drop_restrict_and_cascade();
    test_drop_self_ref_and_index_cleanup();
    test_alter_add_column();
    test_alter_drop_column();
    test_alter_validation();
    test_alter_column_default();
    test_alter_column_not_null();
    test_primary_key_constraint();
    test_unique_constraint();

    std::printf("ddl: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
