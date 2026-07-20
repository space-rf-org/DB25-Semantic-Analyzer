// DB25 Semantic Analyzer - test suite (self-contained assertion harness).
//
// Real assertions, not prints: every check updates a pass/fail tally and the
// process exits non-zero if anything fails.

#include "db25/parser/parser.hpp"
#include "db25/semantic/analyzer.hpp"
#include "db25/semantic/ast_helpers.hpp"
#include "db25/semantic/catalog.hpp"

#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
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

// A catalog for grouping / aggregate tests:
//   emp(id INTEGER NOT NULL, name TEXT, dept TEXT, region TEXT,
//       salary DOUBLE, age INTEGER)
InMemoryCatalog make_catalog_emp() {
    InMemoryCatalog cat;
    cat.add_table("emp", {
        ColumnInfo{"id", DataType::Integer, /*nullable=*/false},
        ColumnInfo{"name", DataType::Text, /*nullable=*/true},
        ColumnInfo{"dept", DataType::Text, /*nullable=*/true},
        ColumnInfo{"region", DataType::Text, /*nullable=*/true},
        ColumnInfo{"salary", DataType::Double, /*nullable=*/true},
        ColumnInfo{"age", DataType::Integer, /*nullable=*/true},
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

// Minimal AST synthesis, used only to exercise analyzer paths the consumed
// parser build cannot yet represent: an explicit INSERT column list, a DML WHERE
// predicate, and a negative LIMIT literal (see the parser-limitation notes in
// docs/DESIGN.md). Nodes are heap-allocated so their addresses are stable, and
// primary_text points at string literals with static storage. This mirrors the
// approach already used by the qualified-star tests above, which synthesize a
// node shape the parser does not emit.
struct SynthTree {
    std::vector<std::unique_ptr<ASTNode>> nodes;

    ASTNode* make(NodeType t, std::string_view text = {}) {
        auto n = std::make_unique<ASTNode>(t);
        n->primary_text = text;
        ASTNode* p = n.get();
        nodes.push_back(std::move(n));
        return p;
    }

    // Append `child` as the last child of `parent`, maintaining the sibling chain
    // and child_count the analyzer relies on.
    static ASTNode* adopt(ASTNode* parent, ASTNode* child) {
        child->parent = parent;
        if (parent->first_child == nullptr) {
            parent->first_child = child;
        } else {
            ASTNode* c = parent->first_child;
            while (c->next_sibling != nullptr) c = c->next_sibling;
            c->next_sibling = child;
        }
        ++parent->child_count;
        return child;
    }
};

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

// Find the first FunctionCall descendant with the given name (or nullptr).
ASTNode* find_function(ASTNode* n, std::string_view name) {
    if (n == nullptr) return nullptr;
    if ((n->node_type == NodeType::FunctionCall ||
         n->node_type == NodeType::FunctionExpr) &&
        n->primary_text == name) {
        return n;
    }
    for (ASTNode* c = first_child(n); c != nullptr; c = c->next_sibling) {
        if (ASTNode* hit = find_function(c, name)) return hit;
    }
    return nullptr;
}

// --- GROUP BY / HAVING legality & function typing ----------------------

void test_groupby_clean_count_star() {
    std::printf("test_groupby_clean_count_star\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    auto res = p.parse("SELECT dept, COUNT(*) FROM emp GROUP BY dept");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    // Clean: dept is a grouping key, COUNT(*) is an aggregate.
    CHECK(!a.has_errors());
    CHECK(count_code(a, DiagnosticCode::NonGroupedColumn) == 0);

    // COUNT(*) is typed as an integer type (BigInt).
    ASTNode* cnt = find_function(res.value(), "COUNT");
    CHECK(cnt != nullptr && a.type_of(cnt) == DataType::BigInt);
}

void test_groupby_non_grouped_column() {
    std::printf("test_groupby_non_grouped_column\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    auto res = p.parse("SELECT dept, name FROM emp GROUP BY dept");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    // name is neither grouped nor aggregated.
    CHECK(count_code(a, DiagnosticCode::NonGroupedColumn) == 1);
}

void test_groupby_having_aggregate_clean() {
    std::printf("test_groupby_having_aggregate_clean\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    auto res = p.parse(
        "SELECT dept, SUM(salary) FROM emp GROUP BY dept HAVING SUM(salary) > 1000");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    // HAVING references only an aggregate and dept is grouped: clean.
    CHECK(!a.has_errors());
    CHECK(count_code(a, DiagnosticCode::NonGroupedColumn) == 0);

    // SUM(salary) over a DOUBLE stays numeric (Double).
    ASTNode* sum = find_function(res.value(), "SUM");
    CHECK(sum != nullptr && a.type_of(sum) == DataType::Double);
}

void test_having_non_grouped_column() {
    std::printf("test_having_non_grouped_column\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    // name is not a grouping key and is not under an aggregate in HAVING.
    auto res = p.parse(
        "SELECT dept, SUM(salary) FROM emp GROUP BY dept HAVING name = 'x'");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::NonGroupedColumn) == 1);
}

void test_nested_aggregate() {
    std::printf("test_nested_aggregate\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    auto res = p.parse("SELECT SUM(COUNT(*)) FROM emp");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::NestedAggregate) == 1);
}

void test_order_by_non_grouped() {
    std::printf("test_order_by_non_grouped\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    // name in ORDER BY is not a grouping key.
    auto res = p.parse("SELECT dept, COUNT(*) FROM emp GROUP BY dept ORDER BY name");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::NonGroupedColumn) == 1);
}

void test_avg_result_type() {
    std::printf("test_avg_result_type\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    auto res = p.parse("SELECT AVG(salary) FROM emp");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    ASTNode* avg = find_function(res.value(), "AVG");
    // AVG result is Double.
    CHECK(avg != nullptr && a.type_of(avg) == DataType::Double);
}

void test_scalar_function_type() {
    std::printf("test_scalar_function_type\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    auto res = p.parse("SELECT UPPER(name) FROM emp");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    // UPPER(...) -> Text; not a grouped query, so no grouping diagnostics.
    CHECK(!a.has_errors());
    ASTNode* up = find_function(res.value(), "UPPER");
    CHECK(up != nullptr && a.type_of(up) == DataType::Text);
}

void test_unknown_function_degrades() {
    std::printf("test_unknown_function_degrades\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    auto res = p.parse("SELECT WIDGETIZE(name) FROM emp");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    // Unknown function: type is Unknown, and it is only a soft (warning)
    // diagnostic, so it is not an error.
    CHECK(!a.has_errors());
    CHECK(count_code(a, DiagnosticCode::UnknownFunction) == 1);
    ASTNode* fn = find_function(res.value(), "WIDGETIZE");
    CHECK(fn != nullptr && a.type_of(fn) == DataType::Unknown);
}

void test_aggregate_makes_query_grouped() {
    std::printf("test_aggregate_makes_query_grouped\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    // No GROUP BY, but the aggregate makes the query grouped, so the bare
    // non-aggregated `name` is illegal.
    auto res = p.parse("SELECT name, COUNT(*) FROM emp");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::NonGroupedColumn) == 1);
}

// A catalog for nullability / correlation / subquery tests:
//   users(id INTEGER NOT NULL, note TEXT)
//   orders(id INTEGER NOT NULL, uid INTEGER NOT NULL, amount DOUBLE)
InMemoryCatalog make_catalog_null() {
    InMemoryCatalog cat;
    cat.add_table("users", {
        ColumnInfo{"id", DataType::Integer, /*nullable=*/false},
        ColumnInfo{"note", DataType::Text, /*nullable=*/true},
    });
    cat.add_table("orders", {
        ColumnInfo{"id", DataType::Integer, /*nullable=*/false},
        ColumnInfo{"uid", DataType::Integer, /*nullable=*/false},
        ColumnInfo{"amount", DataType::Double, /*nullable=*/true},
    });
    return cat;
}

// --- Nullability propagation -------------------------------------------

void test_nullability_columns_and_functions() {
    std::printf("test_nullability_columns_and_functions\n");
    auto cat = make_catalog_null();
    parser::Parser p;
    // id is NOT NULL, note is nullable; COALESCE(note,'x') is not-null;
    // SUM(id) is nullable; COUNT(*) is not-null.
    auto res = p.parse(
        "SELECT id, note, COALESCE(note, 'x'), SUM(id), COUNT(*) FROM users");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());

    ASTNode* list = find_child(res.value(), NodeType::SelectList);
    ASTNode* id = first_child(list);
    ASTNode* note = id ? id->next_sibling : nullptr;
    ASTNode* coalesce = note ? note->next_sibling : nullptr;
    ASTNode* sum = coalesce ? coalesce->next_sibling : nullptr;
    ASTNode* count = sum ? sum->next_sibling : nullptr;

    CHECK(id != nullptr && a.nullability_of(id) == 1);        // NOT NULL column
    CHECK(note != nullptr && a.nullability_of(note) == 2);    // nullable column
    CHECK(coalesce != nullptr && a.nullability_of(coalesce) == 1);  // COALESCE(.., 'x')
    CHECK(sum != nullptr && a.nullability_of(sum) == 2);      // SUM -> nullable
    CHECK(count != nullptr && a.nullability_of(count) == 1);  // COUNT -> not-null
    // Mirrored onto the parser node's analysis context.
    CHECK(id != nullptr && id->context.analysis.nullability == 1);
}

void test_left_join_nullability() {
    std::printf("test_left_join_nullability\n");
    auto cat = make_catalog_null();
    parser::Parser p;
    // orders.id is NOT NULL in the catalog, but it is on the right of a LEFT
    // JOIN, so o.id resolves as nullable; the left side (u.id) stays not-null.
    auto res = p.parse("SELECT o.id, u.id FROM users u LEFT JOIN orders o ON u.id = o.uid");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());

    ASTNode* list = find_child(res.value(), NodeType::SelectList);
    ASTNode* oid = first_child(list);
    ASTNode* uid = oid ? oid->next_sibling : nullptr;
    CHECK(oid != nullptr && a.nullability_of(oid) == 2);  // null-supplied side
    CHECK(uid != nullptr && a.nullability_of(uid) == 1);  // preserved side
}

void test_inner_join_nullability_unchanged() {
    std::printf("test_inner_join_nullability_unchanged\n");
    auto cat = make_catalog_null();
    parser::Parser p;
    // INNER JOIN: no side is null-supplied, so o.id keeps its NOT NULL.
    auto res = p.parse("SELECT o.id FROM users u JOIN orders o ON u.id = o.uid");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    ASTNode* list = find_child(res.value(), NodeType::SelectList);
    ASTNode* oid = first_child(list);
    CHECK(oid != nullptr && a.nullability_of(oid) == 1);
}

// --- Type coercion -----------------------------------------------------

void test_coercion_numeric_comparison_clean() {
    std::printf("test_coercion_numeric_comparison_clean\n");
    auto cat = make_catalog_null();
    parser::Parser p;
    // amount is DOUBLE, id is INTEGER: numeric comparison, no coercion warning.
    auto res = p.parse("SELECT id FROM orders WHERE amount > id");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());
    CHECK(count_code(a, DiagnosticCode::ImplicitCoercion) == 0);
}

void test_coercion_text_int_comparison_warns() {
    std::printf("test_coercion_text_int_comparison_warns\n");
    auto cat = make_catalog_null();
    parser::Parser p;
    // note is TEXT compared to an integer literal: soft implicit-coercion warning.
    auto res = p.parse("SELECT id FROM users WHERE note = 1");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::ImplicitCoercion) == 1);
    // A warning, not an error.
    CHECK(!a.has_errors());
}

void test_coercion_arithmetic_text_int_error() {
    std::printf("test_coercion_arithmetic_text_int_error\n");
    auto cat = make_catalog_null();
    parser::Parser p;
    // note (TEXT) + id (INTEGER): a hard arithmetic type mismatch.
    auto res = p.parse("SELECT note + id FROM users");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::TypeMismatch) == 1);
    CHECK(a.has_errors());
}

// --- Subquery correlation ----------------------------------------------

void test_exists_correlated_clean() {
    std::printf("test_exists_correlated_clean\n");
    auto cat = make_catalog_null();
    parser::Parser p;
    // u.id inside the subquery resolves against the enclosing query: correlated,
    // no diagnostic; EXISTS(...) types Boolean.
    auto res = p.parse(
        "SELECT u.id FROM users u WHERE EXISTS (SELECT 1 FROM orders o WHERE o.uid = u.id)");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());
    CHECK(a.diagnostics().empty());

    ASTNode* where = find_child(res.value(), NodeType::WhereClause);
    ASTNode* exists = where ? first_child(where) : nullptr;
    CHECK(exists != nullptr && a.type_of(exists) == DataType::Boolean);
    ASTNode* subq = find_descendant(res.value(), NodeType::Subquery);
    CHECK(subq != nullptr && a.is_correlated(subq));
}

void test_subquery_unresolved_in_neither_scope() {
    std::printf("test_subquery_unresolved_in_neither_scope\n");
    auto cat = make_catalog_null();
    parser::Parser p;
    // `zzz` is a column of neither the subquery's orders nor the outer users.
    auto res = p.parse(
        "SELECT u.id FROM users u WHERE EXISTS (SELECT 1 FROM orders o WHERE o.uid = zzz)");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::UnresolvedColumn) == 1);
}

void test_scalar_subquery_single_column() {
    std::printf("test_scalar_subquery_single_column\n");
    auto cat = make_catalog_null();
    parser::Parser p;
    // Scalar subquery projects one column (COUNT(*) -> BigInt); clean.
    auto res = p.parse("SELECT (SELECT COUNT(*) FROM orders) FROM users");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());
    CHECK(count_code(a, DiagnosticCode::ScalarSubqueryColumns) == 0);

    ASTNode* list = find_child(res.value(), NodeType::SelectList);
    ASTNode* subq = first_child(list);
    CHECK(subq != nullptr && subq->node_type == NodeType::Subquery);
    CHECK(subq != nullptr && a.type_of(subq) == DataType::BigInt);  // integer-typed
    // Uncorrelated.
    CHECK(subq != nullptr && !a.is_correlated(subq));
}

void test_scalar_subquery_too_many_columns() {
    std::printf("test_scalar_subquery_too_many_columns\n");
    auto cat = make_catalog_null();
    parser::Parser p;
    // Scalar subquery projects two columns: ScalarSubqueryColumns diagnostic.
    auto res = p.parse("SELECT (SELECT id, uid FROM orders) FROM users");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::ScalarSubqueryColumns) == 1);
}

void test_in_subquery_single_compatible() {
    std::printf("test_in_subquery_single_compatible\n");
    auto cat = make_catalog_null();
    parser::Parser p;
    // id (INTEGER) IN (SELECT uid ...) where uid is INTEGER: one column,
    // type-compatible, clean.
    auto res = p.parse("SELECT id FROM users WHERE id IN (SELECT uid FROM orders)");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());
    CHECK(count_code(a, DiagnosticCode::InSubqueryColumns) == 0);
    CHECK(count_code(a, DiagnosticCode::ImplicitCoercion) == 0);
}

void test_in_subquery_multi_column() {
    std::printf("test_in_subquery_multi_column\n");
    auto cat = make_catalog_null();
    parser::Parser p;
    // Subquery on the right of IN projects two columns: diagnostic.
    auto res = p.parse("SELECT id FROM users WHERE id IN (SELECT uid, id FROM orders)");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::InSubqueryColumns) == 1);
}

void test_in_subquery_incompatible_type() {
    std::printf("test_in_subquery_incompatible_type\n");
    auto cat = make_catalog_null();
    parser::Parser p;
    // id (INTEGER) IN (SELECT note ...) where note is TEXT: single column but a
    // cross-category comparison -> implicit-coercion diagnostic.
    auto res = p.parse("SELECT id FROM users WHERE id IN (SELECT note FROM users)");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::InSubqueryColumns) == 0);
    CHECK(count_code(a, DiagnosticCode::ImplicitCoercion) == 1);
}

// --- DML: INSERT -------------------------------------------------------

void test_insert_clean() {
    std::printf("test_insert_clean\n");
    auto cat = make_catalog();  // users(id INTEGER NOT NULL, name TEXT)
    parser::Parser p;
    // Implicit column list = (id, name); the consumed parser drops an explicit
    // column list, so we use the implicit form here (see docs/DESIGN.md).
    auto res = p.parse("INSERT INTO users VALUES (1, 'a')");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(res.value()->node_type == NodeType::InsertStmt);
    CHECK(!a.has_errors());
    CHECK(a.diagnostics().empty());
}

void test_insert_arity_mismatch() {
    std::printf("test_insert_arity_mismatch\n");
    auto cat = make_catalog();
    parser::Parser p;
    // One value but the (implicit) target column list has two columns.
    auto res = p.parse("INSERT INTO users VALUES (1)");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::InsertArityMismatch) == 1);
}

void test_insert_type_implicit_coercion() {
    std::printf("test_insert_type_implicit_coercion\n");
    auto cat = make_catalog();
    parser::Parser p;
    // 'x' (Text) into id (Integer): numeric<->string is a soft implicit
    // conversion under the assignment coercion model (a warning, not an error).
    auto res = p.parse("INSERT INTO users VALUES ('x', 'a')");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::ImplicitCoercion) == 1);
    CHECK(count_code(a, DiagnosticCode::TypeMismatch) == 0);
    CHECK(!a.has_errors());
}

void test_insert_type_mismatch() {
    std::printf("test_insert_type_mismatch\n");
    auto cat = make_catalog();
    parser::Parser p;
    // TRUE (Boolean) into id (Integer): boolean vs numeric is not an implicit
    // conversion -> a hard TypeMismatch error.
    auto res = p.parse("INSERT INTO users VALUES (TRUE, 'a')");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::TypeMismatch) == 1);
    CHECK(a.has_errors());
}

void test_insert_select_clean() {
    std::printf("test_insert_select_clean\n");
    auto cat = make_catalog();
    parser::Parser p;
    // Projection (id, name) matches the target's implicit column list.
    auto res = p.parse("INSERT INTO users SELECT id, name FROM users");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());
    CHECK(a.diagnostics().empty());
}

void test_insert_select_arity_mismatch() {
    std::printf("test_insert_select_arity_mismatch\n");
    auto cat = make_catalog();
    parser::Parser p;
    // Projection has one column; the target has two.
    auto res = p.parse("INSERT INTO users SELECT id FROM users");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::InsertArityMismatch) == 1);
}

void test_insert_unknown_table() {
    std::printf("test_insert_unknown_table\n");
    auto cat = make_catalog();
    parser::Parser p;
    auto res = p.parse("INSERT INTO nonexistent VALUES (1)");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::UnresolvedTable) == 1);
}

// Explicit-column-list INSERT: the parser build drops the column list, so the
// InsertStmt shape (TableRef, ColumnList, ValuesClause[row]) is synthesized.
void test_insert_explicit_columns_clean() {
    std::printf("test_insert_explicit_columns_clean\n");
    auto cat = make_catalog();
    SynthTree t;
    ASTNode* ins = t.make(NodeType::InsertStmt);
    t.adopt(ins, t.make(NodeType::TableRef, "users"));
    ASTNode* cols = t.adopt(ins, t.make(NodeType::ColumnList));
    t.adopt(cols, t.make(NodeType::ColumnRef, "id"));
    t.adopt(cols, t.make(NodeType::ColumnRef, "name"));
    ASTNode* values = t.adopt(ins, t.make(NodeType::ValuesClause));
    ASTNode* row = t.adopt(values, t.make(NodeType::Subquery));
    t.adopt(row, t.make(NodeType::IntegerLiteral, "1"));
    t.adopt(row, t.make(NodeType::StringLiteral, "'a'"));

    Analyzer a(cat);
    a.analyze(ins);
    CHECK(!a.has_errors());
    CHECK(a.diagnostics().empty());
}

void test_insert_explicit_unknown_column() {
    std::printf("test_insert_explicit_unknown_column\n");
    auto cat = make_catalog();
    SynthTree t;
    ASTNode* ins = t.make(NodeType::InsertStmt);
    t.adopt(ins, t.make(NodeType::TableRef, "users"));
    ASTNode* cols = t.adopt(ins, t.make(NodeType::ColumnList));
    t.adopt(cols, t.make(NodeType::ColumnRef, "id"));
    t.adopt(cols, t.make(NodeType::ColumnRef, "bogus"));  // not a column of users
    ASTNode* values = t.adopt(ins, t.make(NodeType::ValuesClause));
    ASTNode* row = t.adopt(values, t.make(NodeType::Subquery));
    t.adopt(row, t.make(NodeType::IntegerLiteral, "1"));
    t.adopt(row, t.make(NodeType::IntegerLiteral, "2"));

    Analyzer a(cat);
    a.analyze(ins);
    CHECK(count_code(a, DiagnosticCode::UnresolvedColumn) == 1);
}

void test_insert_not_null_violation() {
    std::printf("test_insert_not_null_violation\n");
    auto cat = make_catalog();  // id is NOT NULL with no default
    SynthTree t;
    ASTNode* ins = t.make(NodeType::InsertStmt);
    t.adopt(ins, t.make(NodeType::TableRef, "users"));
    ASTNode* cols = t.adopt(ins, t.make(NodeType::ColumnList));
    t.adopt(cols, t.make(NodeType::ColumnRef, "name"));  // id omitted
    ASTNode* values = t.adopt(ins, t.make(NodeType::ValuesClause));
    ASTNode* row = t.adopt(values, t.make(NodeType::Subquery));
    t.adopt(row, t.make(NodeType::StringLiteral, "'a'"));

    Analyzer a(cat);
    a.analyze(ins);
    // id is NOT NULL, receives no value, and has no default.
    CHECK(count_code(a, DiagnosticCode::NotNullViolation) == 1);
    CHECK(count_code(a, DiagnosticCode::InsertArityMismatch) == 0);
}

void test_insert_not_null_with_default_ok() {
    std::printf("test_insert_not_null_with_default_ok\n");
    // id is NOT NULL but has a default, so omitting it is fine.
    InMemoryCatalog cat;
    cat.add_table("users", {
        ColumnInfo{"id", DataType::Integer, /*nullable=*/false, /*has_default=*/true},
        ColumnInfo{"name", DataType::Text, /*nullable=*/true},
    });
    SynthTree t;
    ASTNode* ins = t.make(NodeType::InsertStmt);
    t.adopt(ins, t.make(NodeType::TableRef, "users"));
    ASTNode* cols = t.adopt(ins, t.make(NodeType::ColumnList));
    t.adopt(cols, t.make(NodeType::ColumnRef, "name"));  // id omitted, but has default
    ASTNode* values = t.adopt(ins, t.make(NodeType::ValuesClause));
    ASTNode* row = t.adopt(values, t.make(NodeType::Subquery));
    t.adopt(row, t.make(NodeType::StringLiteral, "'a'"));

    Analyzer a(cat);
    a.analyze(ins);
    CHECK(count_code(a, DiagnosticCode::NotNullViolation) == 0);
    CHECK(!a.has_errors());
}

// --- DML: UPDATE -------------------------------------------------------

void test_update_clean() {
    std::printf("test_update_clean\n");
    auto cat = make_catalog();
    parser::Parser p;
    auto res = p.parse("UPDATE users SET name = 'b'");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(res.value()->node_type == NodeType::UpdateStmt);
    CHECK(!a.has_errors());
    CHECK(a.diagnostics().empty());
}

void test_update_unknown_column() {
    std::printf("test_update_unknown_column\n");
    auto cat = make_catalog();
    parser::Parser p;
    auto res = p.parse("UPDATE users SET missing = 1");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::UnresolvedColumn) == 1);
}

void test_update_type_diagnostic() {
    std::printf("test_update_type_diagnostic\n");
    auto cat = make_catalog();
    parser::Parser p;
    // 'x' (Text) into id (Integer): a soft implicit conversion (type diagnostic).
    auto res = p.parse("UPDATE users SET id = 'x'");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::ImplicitCoercion) == 1);
}

void test_update_where_synth() {
    std::printf("test_update_where_synth\n");
    auto cat = make_catalog();
    // Synthesize `UPDATE users SET name = 'b' WHERE id = 1`; the parser build
    // mis-parses a DML WHERE predicate, so we build the WHERE subtree.
    SynthTree t;
    ASTNode* upd = t.make(NodeType::UpdateStmt);
    t.adopt(upd, t.make(NodeType::TableRef, "users"));
    ASTNode* set = t.adopt(upd, t.make(NodeType::SetClause));
    ASTNode* asgn = t.adopt(set, t.make(NodeType::BinaryExpr, "name"));
    t.adopt(asgn, t.make(NodeType::StringLiteral, "'b'"));
    ASTNode* where = t.adopt(upd, t.make(NodeType::WhereClause));
    ASTNode* eq = t.adopt(where, t.make(NodeType::BinaryExpr, "="));
    t.adopt(eq, t.make(NodeType::ColumnRef, "id"));
    t.adopt(eq, t.make(NodeType::IntegerLiteral, "1"));

    Analyzer a(cat);
    a.analyze(upd);
    // id resolves against the target table; the assignment is clean.
    CHECK(!a.has_errors());
    CHECK(a.diagnostics().empty());
}

void test_update_where_unresolved_synth() {
    std::printf("test_update_where_unresolved_synth\n");
    auto cat = make_catalog();
    SynthTree t;
    ASTNode* upd = t.make(NodeType::UpdateStmt);
    t.adopt(upd, t.make(NodeType::TableRef, "users"));
    ASTNode* set = t.adopt(upd, t.make(NodeType::SetClause));
    ASTNode* asgn = t.adopt(set, t.make(NodeType::BinaryExpr, "name"));
    t.adopt(asgn, t.make(NodeType::StringLiteral, "'b'"));
    ASTNode* where = t.adopt(upd, t.make(NodeType::WhereClause));
    ASTNode* eq = t.adopt(where, t.make(NodeType::BinaryExpr, "="));
    t.adopt(eq, t.make(NodeType::ColumnRef, "bogus"));  // not a column of users
    t.adopt(eq, t.make(NodeType::IntegerLiteral, "1"));

    Analyzer a(cat);
    a.analyze(upd);
    CHECK(count_code(a, DiagnosticCode::UnresolvedColumn) == 1);
}

// --- DML: DELETE -------------------------------------------------------

void test_delete_clean() {
    std::printf("test_delete_clean\n");
    auto cat = make_catalog();
    parser::Parser p;
    auto res = p.parse("DELETE FROM users");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(res.value()->node_type == NodeType::DeleteStmt);
    CHECK(!a.has_errors());
}

void test_delete_unknown_table() {
    std::printf("test_delete_unknown_table\n");
    auto cat = make_catalog();
    parser::Parser p;
    auto res = p.parse("DELETE FROM nonexistent");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::UnresolvedTable) == 1);
}

void test_delete_where_synth() {
    std::printf("test_delete_where_synth\n");
    auto cat = make_catalog();
    // Synthesize `DELETE FROM users WHERE id = 1`.
    SynthTree t;
    ASTNode* del = t.make(NodeType::DeleteStmt);
    t.adopt(del, t.make(NodeType::TableRef, "users"));
    ASTNode* where = t.adopt(del, t.make(NodeType::WhereClause));
    ASTNode* eq = t.adopt(where, t.make(NodeType::BinaryExpr, "="));
    t.adopt(eq, t.make(NodeType::ColumnRef, "id"));
    t.adopt(eq, t.make(NodeType::IntegerLiteral, "1"));

    Analyzer a(cat);
    a.analyze(del);
    CHECK(!a.has_errors());
    CHECK(a.diagnostics().empty());
}

// --- CASE expression typing --------------------------------------------

void test_case_searched_text() {
    std::printf("test_case_searched_text\n");
    auto cat = make_catalog();
    parser::Parser p;
    auto res = p.parse("SELECT CASE WHEN id > 0 THEN 'a' ELSE 'b' END FROM users");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());
    ASTNode* ce = find_descendant(res.value(), NodeType::CaseExpr);
    CHECK(ce != nullptr && a.type_of(ce) == DataType::Text);
    // Both branches are not-null and there is an ELSE, so the CASE is not-null.
    CHECK(ce != nullptr && a.nullability_of(ce) == 1);
}

void test_case_type_mismatch() {
    std::printf("test_case_type_mismatch\n");
    auto cat = make_catalog();
    parser::Parser p;
    // 'a' (Text) vs 1 (Integer) across CASE branches: incompatible.
    auto res = p.parse("SELECT CASE WHEN id > 0 THEN 'a' ELSE 1 END FROM users");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::TypeMismatch) == 1);
}

void test_case_no_else_nullable() {
    std::printf("test_case_no_else_nullable\n");
    auto cat = make_catalog();
    parser::Parser p;
    // No ELSE: an unmatched CASE yields NULL, so the result is nullable.
    auto res = p.parse("SELECT CASE WHEN id > 0 THEN 'a' END FROM users");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    ASTNode* ce = find_descendant(res.value(), NodeType::CaseExpr);
    CHECK(ce != nullptr && a.type_of(ce) == DataType::Text);
    CHECK(ce != nullptr && a.nullability_of(ce) == 2);
}

void test_case_simple_clean() {
    std::printf("test_case_simple_clean\n");
    auto cat = make_catalog();
    parser::Parser p;
    // Simple CASE: the WHEN operands are compared to `id`, not booleans, so no
    // non-boolean-condition warning is emitted.
    auto res = p.parse("SELECT CASE id WHEN 1 THEN 'a' ELSE 'b' END FROM users");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    ASTNode* ce = find_descendant(res.value(), NodeType::CaseExpr);
    CHECK(ce != nullptr && a.type_of(ce) == DataType::Text);
    CHECK(count_code(a, DiagnosticCode::ImplicitCoercion) == 0);
    CHECK(!a.has_errors());
}

void test_case_nonboolean_when_warns() {
    std::printf("test_case_nonboolean_when_warns\n");
    auto cat = make_catalog();
    parser::Parser p;
    // Searched CASE whose WHEN operand is `id` (Integer, not boolean): a soft
    // warning, not an error.
    auto res = p.parse("SELECT CASE WHEN id THEN 'a' ELSE 'b' END FROM users");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::ImplicitCoercion) == 1);
    CHECK(!a.has_errors());
}

// --- ORDER BY / LIMIT --------------------------------------------------

void test_order_by_alias_resolves() {
    std::printf("test_order_by_alias_resolves\n");
    auto cat = make_catalog();
    parser::Parser p;
    // ORDER BY references the SELECT-list alias `x`.
    auto res = p.parse("SELECT id AS x FROM users ORDER BY x");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());
    CHECK(count_code(a, DiagnosticCode::UnresolvedColumn) == 0);
}

void test_order_by_unknown_column() {
    std::printf("test_order_by_unknown_column\n");
    auto cat = make_catalog();
    parser::Parser p;
    // `bogus` is neither an output name nor a column of users.
    auto res = p.parse("SELECT id FROM users ORDER BY bogus");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::UnresolvedColumn) == 1);
}

void test_limit_valid() {
    std::printf("test_limit_valid\n");
    auto cat = make_catalog();
    parser::Parser p;
    auto res = p.parse("SELECT id FROM users LIMIT 10");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::InvalidLimit) == 0);
    CHECK(!a.has_errors());
}

void test_limit_float_invalid() {
    std::printf("test_limit_float_invalid\n");
    auto cat = make_catalog();
    parser::Parser p;
    // A fractional LIMIT literal is not a valid (integer) row count.
    auto res = p.parse("SELECT id FROM users LIMIT 1.5");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::InvalidLimit) == 1);
}

void test_limit_negative_synth() {
    std::printf("test_limit_negative_synth\n");
    auto cat = make_catalog();
    parser::Parser p;
    // The parser build drops a negative LIMIT literal, so attach a synthesized
    // LimitClause with a negative integer operand to a real SELECT.
    auto res = p.parse("SELECT id FROM users");
    CHECK(res.has_value());
    if (!res) return;

    SynthTree t;
    ASTNode* limit = t.make(NodeType::LimitClause);
    t.adopt(limit, t.make(NodeType::IntegerLiteral, "-1"));
    SynthTree::adopt(res.value(), limit);

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::InvalidLimit) == 1);
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

    // GROUP BY / HAVING legality & function typing
    test_groupby_clean_count_star();
    test_groupby_non_grouped_column();
    test_groupby_having_aggregate_clean();
    test_having_non_grouped_column();
    test_nested_aggregate();
    test_order_by_non_grouped();
    test_avg_result_type();
    test_scalar_function_type();
    test_unknown_function_degrades();
    test_aggregate_makes_query_grouped();

    // Nullability propagation
    test_nullability_columns_and_functions();
    test_left_join_nullability();
    test_inner_join_nullability_unchanged();

    // Type coercion
    test_coercion_numeric_comparison_clean();
    test_coercion_text_int_comparison_warns();
    test_coercion_arithmetic_text_int_error();

    // Subquery correlation & scalar / IN subqueries
    test_exists_correlated_clean();
    test_subquery_unresolved_in_neither_scope();
    test_scalar_subquery_single_column();
    test_scalar_subquery_too_many_columns();
    test_in_subquery_single_compatible();
    test_in_subquery_multi_column();
    test_in_subquery_incompatible_type();

    // DML: INSERT
    test_insert_clean();
    test_insert_arity_mismatch();
    test_insert_type_implicit_coercion();
    test_insert_type_mismatch();
    test_insert_select_clean();
    test_insert_select_arity_mismatch();
    test_insert_unknown_table();
    test_insert_explicit_columns_clean();
    test_insert_explicit_unknown_column();
    test_insert_not_null_violation();
    test_insert_not_null_with_default_ok();

    // DML: UPDATE
    test_update_clean();
    test_update_unknown_column();
    test_update_type_diagnostic();
    test_update_where_synth();
    test_update_where_unresolved_synth();

    // DML: DELETE
    test_delete_clean();
    test_delete_unknown_table();
    test_delete_where_synth();

    // CASE expression typing
    test_case_searched_text();
    test_case_type_mismatch();
    test_case_no_else_nullable();
    test_case_simple_clean();
    test_case_nonboolean_when_warns();

    // ORDER BY / LIMIT
    test_order_by_alias_resolves();
    test_order_by_unknown_column();
    test_limit_valid();
    test_limit_float_invalid();
    test_limit_negative_synth();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
