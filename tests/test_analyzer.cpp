// DB25 Semantic Analyzer - test suite (self-contained assertion harness).
//
// Real assertions, not prints: every check updates a pass/fail tally and the
// process exits non-zero if anything fails.

#include "db25/parser/parser.hpp"
#include "db25/semantic/analyzer.hpp"
#include "db25/semantic/ast_helpers.hpp"
#include "db25/semantic/catalog.hpp"

#include <cstdio>
#include <string>
#include <string_view>

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

// Build a catalog with users(id INTEGER NOT NULL, name TEXT).
InMemoryCatalog make_catalog() {
    InMemoryCatalog cat;
    cat.add_table("users", {
        ColumnInfo{"id", DataType::Integer, /*nullable=*/false},
        ColumnInfo{"name", DataType::Text, /*nullable=*/true},
    });
    return cat;
}

// A richer catalog for join / set-operation tests:
//   users(id INTEGER NOT NULL, name TEXT)
//   orders(order_id INTEGER NOT NULL, user_id INTEGER NOT NULL, total DOUBLE)
//   sessions(user_id INTEGER NOT NULL, token TEXT)
InMemoryCatalog make_catalog_joins() {
    InMemoryCatalog cat;
    cat.add_table("users", {
        ColumnInfo{"id", DataType::Integer, /*nullable=*/false},
        ColumnInfo{"name", DataType::Text, /*nullable=*/true},
    });
    cat.add_table("orders", {
        ColumnInfo{"order_id", DataType::Integer, /*nullable=*/false},
        ColumnInfo{"user_id", DataType::Integer, /*nullable=*/false},
        ColumnInfo{"total", DataType::Double, /*nullable=*/true},
    });
    cat.add_table("sessions", {
        ColumnInfo{"user_id", DataType::Integer, /*nullable=*/false},
        ColumnInfo{"token", DataType::Text, /*nullable=*/true},
    });
    return cat;
}

// Find the first descendant of a given type (depth-first), or nullptr.
ASTNode* find_descendant(ASTNode* n, NodeType type) {
    if (n == nullptr) return nullptr;
    if (n->node_type == type) return n;
    for (ASTNode* c = first_child(n); c != nullptr; c = c->next_sibling) {
        if (ASTNode* hit = find_descendant(c, type)) return hit;
    }
    return nullptr;
}

// Count diagnostics of a given code.
int count_code(const Analyzer& a, DiagnosticCode code) {
    int n = 0;
    for (const auto& d : a.diagnostics()) {
        if (d.code == code) {
            ++n;
        }
    }
    return n;
}

// --- Tests --------------------------------------------------------------

void test_select_resolves_clean() {
    std::printf("test_select_resolves_clean\n");
    auto cat = make_catalog();
    parser::Parser p;
    auto res = p.parse("SELECT id, name FROM users");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());

    CHECK(!a.has_errors());
    CHECK(a.diagnostics().empty());

    // Column types are inferred from the catalog.
    ASTNode* list = find_child(res.value(), NodeType::SelectList);
    CHECK(list != nullptr);
    ASTNode* id = first_child(list);
    ASTNode* name = id ? id->next_sibling : nullptr;
    CHECK(id != nullptr && a.type_of(id) == DataType::Integer);
    CHECK(name != nullptr && a.type_of(name) == DataType::Text);
    // Resolved column context is recorded on the node.
    CHECK(id != nullptr && id->context.analysis.nullability == 1);   // NOT NULL
    CHECK(name != nullptr && name->context.analysis.nullability == 2);  // nullable
}

void test_unresolved_column() {
    std::printf("test_unresolved_column\n");
    auto cat = make_catalog();
    parser::Parser p;
    auto res = p.parse("SELECT missing FROM users");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());

    CHECK(a.has_errors());
    CHECK(count_code(a, DiagnosticCode::UnresolvedColumn) == 1);
    // The diagnostic carries a non-empty source range.
    bool has_range = false;
    for (const auto& d : a.diagnostics()) {
        if (d.code == DiagnosticCode::UnresolvedColumn) {
            has_range = d.source_end >= d.source_start;
        }
    }
    CHECK(has_range);
}

void test_alias_resolution() {
    std::printf("test_alias_resolution\n");
    auto cat = make_catalog();
    parser::Parser p;
    auto res = p.parse("SELECT u.id FROM users u");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());

    CHECK(!a.has_errors());
    ASTNode* list = find_child(res.value(), NodeType::SelectList);
    ASTNode* uid = first_child(list);
    CHECK(uid != nullptr && a.type_of(uid) == DataType::Integer);
}

void test_derived_table() {
    std::printf("test_derived_table\n");
    auto cat = make_catalog();
    parser::Parser p;
    auto res = p.parse("SELECT t.x FROM (SELECT id AS x FROM users) t");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());

    CHECK(!a.has_errors());
    CHECK(a.diagnostics().empty());
    // t.x resolves to the derived column, whose type flows from users.id.
    ASTNode* list = find_child(res.value(), NodeType::SelectList);
    ASTNode* tx = first_child(list);
    CHECK(tx != nullptr && a.type_of(tx) == DataType::Integer);
}

void test_where_type_inference() {
    std::printf("test_where_type_inference\n");
    auto cat = make_catalog();
    parser::Parser p;
    auto res = p.parse("SELECT id FROM users WHERE id = 1");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());

    CHECK(!a.has_errors());
    ASTNode* where = find_child(res.value(), NodeType::WhereClause);
    CHECK(where != nullptr);
    ASTNode* cmp = first_child(where);
    CHECK(cmp != nullptr && cmp->node_type == NodeType::BinaryExpr);
    // Comparison infers Boolean.
    CHECK(cmp != nullptr && a.type_of(cmp) == DataType::Boolean);
    // The literal on the right infers Integer.
    ASTNode* lhs = cmp ? first_child(cmp) : nullptr;
    ASTNode* rhs = lhs ? lhs->next_sibling : nullptr;
    CHECK(rhs != nullptr && rhs->node_type == NodeType::IntegerLiteral);
    CHECK(rhs != nullptr && a.type_of(rhs) == DataType::Integer);
}

void test_cte_resolution() {
    std::printf("test_cte_resolution\n");
    auto cat = make_catalog();
    parser::Parser p;
    auto res = p.parse("WITH c AS (SELECT id FROM users) SELECT id FROM c");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());

    CHECK(!a.has_errors());
    CHECK(a.diagnostics().empty());
}

void test_unresolved_table() {
    std::printf("test_unresolved_table\n");
    auto cat = make_catalog();
    parser::Parser p;
    auto res = p.parse("SELECT id FROM nonexistent");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());

    CHECK(count_code(a, DiagnosticCode::UnresolvedTable) == 1);
}

// --- SELECT * / table.* expansion --------------------------------------

void test_select_star_expands() {
    std::printf("test_select_star_expands\n");
    auto cat = make_catalog();
    parser::Parser p;
    auto res = p.parse("SELECT * FROM users");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());

    const auto* proj = a.projection_of(res.value());
    CHECK(proj != nullptr);
    if (proj == nullptr) return;
    // Expands to users' columns, in catalog order.
    CHECK(proj->size() == 2);
    if (proj->size() == 2) {
        CHECK((*proj)[0].name == "id" && (*proj)[0].type == DataType::Integer);
        CHECK((*proj)[1].name == "name" && (*proj)[1].type == DataType::Text);
    }
}

void test_select_star_no_from() {
    std::printf("test_select_star_no_from\n");
    auto cat = make_catalog();
    parser::Parser p;
    auto res = p.parse("SELECT *");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    // `SELECT *` with no FROM has nothing to expand.
    CHECK(count_code(a, DiagnosticCode::StarWithoutFrom) == 1);
}

// The consumed parser build cannot represent a qualified star `table.*` (it
// misparses it and drops the FROM clause; see docs/DESIGN.md). We exercise the
// analyzer's qualified-star path by synthesizing the node shape the parser is
// meant to produce: a Star select-list item whose schema_name is the qualifier.
void test_qualified_star_expands() {
    std::printf("test_qualified_star_expands\n");
    auto cat = make_catalog_joins();
    parser::Parser p;
    auto res = p.parse("SELECT * FROM users u JOIN orders o ON u.id = o.user_id");
    CHECK(res.has_value());
    if (!res) return;

    // Locate the Star and the `users u` TableRef (whose schema_name is "u").
    ASTNode* star = find_descendant(res.value(), NodeType::Star);
    ASTNode* from = find_child(res.value(), NodeType::FromClause);
    ASTNode* users_ref = from ? first_child(from) : nullptr;
    CHECK(star != nullptr);
    CHECK(users_ref != nullptr && users_ref->node_type == NodeType::TableRef);
    if (star == nullptr || users_ref == nullptr) return;
    // Make it `u.*` by pointing schema_name at the arena-owned alias text "u".
    star->schema_name = users_ref->schema_name;
    CHECK(star->schema_name == "u");

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());  // ON predicate resolves cleanly

    const auto* proj = a.projection_of(res.value());
    CHECK(proj != nullptr);
    if (proj == nullptr) return;
    // u.* -> only users' columns, not orders'.
    CHECK(proj->size() == 2);
    if (proj->size() == 2) {
        CHECK((*proj)[0].name == "id");
        CHECK((*proj)[1].name == "name");
    }
}

void test_qualified_star_bad_qualifier() {
    std::printf("test_qualified_star_bad_qualifier\n");
    auto cat = make_catalog_joins();
    parser::Parser p;
    auto res = p.parse("SELECT * FROM users u");
    CHECK(res.has_value());
    if (!res) return;

    ASTNode* star = find_descendant(res.value(), NodeType::Star);
    ASTNode* from = find_child(res.value(), NodeType::FromClause);
    ASTNode* users_ref = from ? first_child(from) : nullptr;
    CHECK(star != nullptr && users_ref != nullptr);
    if (star == nullptr || users_ref == nullptr) return;
    // Point the qualifier at "users" (the table name), which is NOT the visible
    // relation name here because the relation is aliased to "u".
    star->schema_name = users_ref->primary_text;  // "users"

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::UnresolvedQualifier) == 1);
}

// --- JOIN ON / USING resolution ----------------------------------------

void test_join_on_resolves() {
    std::printf("test_join_on_resolves\n");
    auto cat = make_catalog_joins();
    parser::Parser p;
    auto res = p.parse("SELECT u.id FROM users u JOIN orders o ON u.id = o.user_id");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());
    CHECK(a.diagnostics().empty());

    // The projected u.id resolves to users.id (Integer).
    ASTNode* list = find_child(res.value(), NodeType::SelectList);
    ASTNode* uid = first_child(list);
    CHECK(uid != nullptr && a.type_of(uid) == DataType::Integer);
}

void test_join_on_unresolved_column() {
    std::printf("test_join_on_unresolved_column\n");
    auto cat = make_catalog_joins();
    parser::Parser p;
    auto res = p.parse("SELECT u.id FROM users u JOIN orders o ON u.id = o.nope");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    // o.nope is not a column of orders.
    CHECK(count_code(a, DiagnosticCode::UnresolvedColumn) == 1);
}

void test_join_using_resolves() {
    std::printf("test_join_using_resolves\n");
    auto cat = make_catalog_joins();
    parser::Parser p;
    auto res = p.parse("SELECT * FROM orders o JOIN sessions s USING (user_id)");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    // user_id is present in both orders and sessions.
    CHECK(count_code(a, DiagnosticCode::UsingColumnMissing) == 0);
    CHECK(!a.has_errors());
}

void test_join_using_missing() {
    std::printf("test_join_using_missing\n");
    auto cat = make_catalog_joins();
    parser::Parser p;
    // order_id exists in orders but not sessions.
    auto res = p.parse("SELECT * FROM orders o JOIN sessions s USING (order_id)");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::UsingColumnMissing) == 1);
}

// --- Set-operation reconciliation --------------------------------------

void test_setop_union_clean() {
    std::printf("test_setop_union_clean\n");
    auto cat = make_catalog_joins();
    parser::Parser p;
    auto res = p.parse("SELECT id FROM users UNION SELECT id FROM users");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());
    CHECK(res.value()->node_type == NodeType::UnionStmt);

    const auto* proj = a.projection_of(res.value());
    CHECK(proj != nullptr);
    if (proj != nullptr) {
        CHECK(proj->size() == 1);
        if (proj->size() == 1) CHECK((*proj)[0].type == DataType::Integer);
    }
}

void test_setop_arity_mismatch() {
    std::printf("test_setop_arity_mismatch\n");
    auto cat = make_catalog_joins();
    parser::Parser p;
    auto res = p.parse("SELECT id FROM users UNION SELECT id, name FROM users");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::SetOpArityMismatch) == 1);
}

void test_setop_type_mismatch() {
    std::printf("test_setop_type_mismatch\n");
    auto cat = make_catalog_joins();
    parser::Parser p;
    // id (Integer) vs name (Text) -> incompatible.
    auto res = p.parse("SELECT id FROM users UNION SELECT name FROM users");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::SetOpTypeMismatch) == 1);
}

void test_setop_numeric_compatible() {
    std::printf("test_setop_numeric_compatible\n");
    auto cat = make_catalog_joins();
    parser::Parser p;
    // id (Integer) vs total (Double) -> numeric-compatible, reconciles to Double.
    auto res = p.parse("SELECT id FROM users UNION SELECT total FROM orders");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::SetOpTypeMismatch) == 0);

    const auto* proj = a.projection_of(res.value());
    CHECK(proj != nullptr);
    if (proj != nullptr && proj->size() == 1) {
        CHECK((*proj)[0].type == DataType::Double);  // promoted
    }
}

}  // namespace

int main() {
    test_select_resolves_clean();
    test_unresolved_column();
    test_alias_resolution();
    test_derived_table();
    test_where_type_inference();
    test_cte_resolution();
    test_unresolved_table();

    // SELECT * / table.* expansion
    test_select_star_expands();
    test_select_star_no_from();
    test_qualified_star_expands();
    test_qualified_star_bad_qualifier();

    // JOIN ON / USING resolution
    test_join_on_resolves();
    test_join_on_unresolved_column();
    test_join_using_resolves();
    test_join_using_missing();

    // Set-operation reconciliation
    test_setop_union_clean();
    test_setop_arity_mismatch();
    test_setop_type_mismatch();
    test_setop_numeric_compatible();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
