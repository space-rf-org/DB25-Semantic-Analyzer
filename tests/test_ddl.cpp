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

const ast::ASTNode* parse(parser::Parser& p, const std::string& sql) {
    auto r = p.parse(sql);
    return r.has_value() ? r.value() : nullptr;
}

std::string scratch_path(const char* name) {
    const char* dir = std::getenv("TMPDIR");
    std::string base = (dir != nullptr && *dir != '\0') ? dir : "/tmp";
    return base + "/db25_" + name;
}

DdlResult run(parser::Parser& p, CatalogManager& mgr, const std::string& sql) {
    const ast::ASTNode* ast = parse(p, sql);
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
    const ast::ASTNode* two_pk =
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

}  // namespace

int main() {
    test_create_table_end_to_end();
    test_statement_validation_layer();
    test_catalog_integrity_layer();
    test_drop_table();
    test_create_index_end_to_end();
    test_create_index_integrity();
    test_drop_index();

    std::printf("ddl: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
