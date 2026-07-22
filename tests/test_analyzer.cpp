// DB25 Semantic Analyzer - test suite (self-contained assertion harness).
//
// Real assertions, not prints: every check updates a pass/fail tally and the
// process exits non-zero if anything fails.

#include "db25/parser/parser.hpp"
#include "db25/semantic/analyzer.hpp"
#include "db25/semantic/ast_helpers.hpp"
#include "db25/semantic/catalog.hpp"

#include <cstdio>
#include <optional>
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

// A catalog for temporal-arithmetic tests:
//   events(d DATE NOT NULL, ts TIMESTAMP NOT NULL, iv INTERVAL NOT NULL,
//          d2 DATE, n INTEGER NOT NULL)
InMemoryCatalog make_catalog_temporal() {
    InMemoryCatalog cat;
    cat.add_table("events", {
        ColumnInfo{"d", DataType::Date, /*nullable=*/false},
        ColumnInfo{"ts", DataType::Timestamp, /*nullable=*/false},
        ColumnInfo{"iv", DataType::Interval, /*nullable=*/false},
        ColumnInfo{"d2", DataType::Date, /*nullable=*/true},
        ColumnInfo{"n", DataType::Integer, /*nullable=*/false},
    });
    return cat;
}

// A catalog for string-concatenation tests:
//   people(first TEXT NOT NULL, last TEXT NOT NULL, mid TEXT)
InMemoryCatalog make_catalog_people() {
    InMemoryCatalog cat;
    cat.add_table("people", {
        ColumnInfo{"first", DataType::Text, /*nullable=*/false},
        ColumnInfo{"last", DataType::Text, /*nullable=*/false},
        ColumnInfo{"mid", DataType::Text, /*nullable=*/true},
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

// The parser emits a qualified star `table.*` as a Star select-list item whose
// schema_name holds the qualifier (see docs/DESIGN.md). The end-to-end tests
// below parse real `alias.*` SQL; this synthetic case additionally pins down the
// behaviour when the qualifier matches only one of several visible relations by
// pointing schema_name at an arena-owned alias.
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

// --- Qualified star, end-to-end (parser emits the Star node itself) ------
//
// These parse real `alias.*` SQL, confirming the whole path works without any
// hand-synthesized AST: the parser produces a Star whose schema_name carries the
// qualifier, and expand_star turns it into exactly that relation's columns.

void test_qualified_star_e2e_expands() {
    std::printf("test_qualified_star_e2e_expands\n");
    auto cat = make_catalog_joins();
    parser::Parser p;
    auto res = p.parse("SELECT o.* FROM orders o");
    CHECK(res.has_value());
    if (!res) return;

    // The parser really does produce a Star with the qualifier in schema_name.
    ASTNode* star = find_descendant(res.value(), NodeType::Star);
    CHECK(star != nullptr && star->node_type == NodeType::Star);
    CHECK(star != nullptr && alias_of(star) == "o");

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());

    const auto* proj = a.projection_of(res.value());
    CHECK(proj != nullptr);
    if (proj == nullptr) return;
    // o.* -> orders' three columns, in catalog order, with types + nullability.
    CHECK(proj->size() == 3);
    if (proj->size() == 3) {
        CHECK((*proj)[0].name == "order_id" &&
              (*proj)[0].type == DataType::Integer && !(*proj)[0].nullable);
        CHECK((*proj)[1].name == "user_id" &&
              (*proj)[1].type == DataType::Integer && !(*proj)[1].nullable);
        CHECK((*proj)[2].name == "total" &&
              (*proj)[2].type == DataType::Double && (*proj)[2].nullable);
    }
}

void test_qualified_star_e2e_mixed() {
    std::printf("test_qualified_star_e2e_mixed\n");
    auto cat = make_catalog_joins();
    parser::Parser p;
    auto res = p.parse("SELECT o.*, o.order_id FROM orders o");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());

    const auto* proj = a.projection_of(res.value());
    CHECK(proj != nullptr);
    if (proj == nullptr) return;
    // o.* expands to three columns, then the explicit o.order_id appends a fourth.
    CHECK(proj->size() == 4);
    if (proj->size() == 4) {
        CHECK((*proj)[0].name == "order_id");
        CHECK((*proj)[1].name == "user_id");
        CHECK((*proj)[2].name == "total");
        CHECK((*proj)[3].name == "order_id" &&
              (*proj)[3].type == DataType::Integer);
    }
}

void test_qualified_star_e2e_bad_qualifier() {
    std::printf("test_qualified_star_e2e_bad_qualifier\n");
    auto cat = make_catalog_joins();
    parser::Parser p;
    // `x` names no visible relation (the relation is aliased `o`).
    auto res = p.parse("SELECT x.* FROM orders o");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::UnresolvedQualifier) == 1);
    // Nothing is expanded for an unresolved qualifier.
    const auto* proj = a.projection_of(res.value());
    CHECK(proj != nullptr && proj->empty());
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

void test_from_duplicate_alias_flagged() {
    std::printf("test_from_duplicate_alias_flagged\n");
    auto cat = make_catalog_joins();
    parser::Parser p;
    // The correlation name `o` is specified twice in the FROM clause.
    auto res = p.parse("SELECT o.user_id FROM orders o, orders o");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::DuplicateRelation) == 1);
    CHECK(a.has_errors());
}

void test_join_duplicate_alias_flagged() {
    std::printf("test_join_duplicate_alias_flagged\n");
    auto cat = make_catalog_joins();
    parser::Parser p;
    // A self-join that reuses the same alias `a` on both sides is ambiguous.
    auto res = p.parse(
        "SELECT a.user_id FROM orders a JOIN orders a ON a.order_id = a.order_id");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::DuplicateRelation) == 1);
}

void test_from_distinct_aliases_not_flagged() {
    std::printf("test_from_distinct_aliases_not_flagged\n");
    auto cat = make_catalog_joins();
    parser::Parser p;
    // Distinct aliases for the same base table are legal (comma self-join).
    auto res = p.parse("SELECT a.user_id FROM orders a, orders b");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::DuplicateRelation) == 0);
}

void test_self_join_distinct_aliases_not_flagged() {
    std::printf("test_self_join_distinct_aliases_not_flagged\n");
    auto cat = make_catalog_joins();
    parser::Parser p;
    // A plain self-join with distinct aliases must not be flagged.
    auto res = p.parse(
        "SELECT a.user_id FROM orders a JOIN orders b ON a.order_id = b.order_id");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::DuplicateRelation) == 0);
    CHECK(!a.has_errors());
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

// A USING column is COALESCED into one output column, so a bare (unqualified)
// reference to it must resolve, not report ambiguity.
void test_join_using_coalesces_bare_ref() {
    std::printf("test_join_using_coalesces_bare_ref\n");
    auto cat = make_catalog_joins();
    parser::Parser p;
    auto res = p.parse("SELECT user_id FROM orders o JOIN sessions s USING (user_id)");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::AmbiguousColumn) == 0);
    CHECK(!a.has_errors());
}

// A NATURAL join coalesces every common column (here user_id), so a bare
// reference resolves unambiguously - and a qualified reference still works.
void test_natural_join_coalesces_bare_ref() {
    std::printf("test_natural_join_coalesces_bare_ref\n");
    auto cat = make_catalog_joins();
    parser::Parser p;
    auto res = p.parse("SELECT user_id, s.token FROM orders o NATURAL JOIN sessions s");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::AmbiguousColumn) == 0);
    CHECK(!a.has_errors());
}

// Coalescing is specific to USING / NATURAL: a plain ON join over a shared
// column name leaves BOTH copies visible, so a bare reference stays ambiguous.
void test_join_on_shared_name_still_ambiguous() {
    std::printf("test_join_on_shared_name_still_ambiguous\n");
    auto cat = make_catalog_joins();
    parser::Parser p;
    auto res = p.parse(
        "SELECT user_id FROM orders o JOIN sessions s ON o.user_id = s.user_id");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::AmbiguousColumn) == 1);
}

// `SELECT *` over a USING / NATURAL join emits the shared column exactly ONCE
// (the coalesced right-hand copy is dropped from the expansion), so the
// projection is narrower than the un-coalesced left++right frame.
void test_star_over_using_coalesces() {
    std::printf("test_star_over_using_coalesces\n");
    auto cat = make_catalog_joins();
    parser::Parser p;
    // orders(order_id,user_id,total) + sessions(user_id,token), user_id shared.
    auto res = p.parse("SELECT * FROM orders o JOIN sessions s USING (user_id)");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());
    const auto* proj = a.projection_of(res.value());
    CHECK(proj != nullptr);
    if (proj == nullptr) return;
    // [order_id, user_id, total, token] - user_id once, not twice (would be 5).
    CHECK(proj->size() == 4);
    int user_id_count = 0;
    for (const auto& c : *proj) {
        if (c.name == "user_id") ++user_id_count;
    }
    CHECK(user_id_count == 1);
}

void test_star_over_natural_coalesces() {
    std::printf("test_star_over_natural_coalesces\n");
    auto cat = make_catalog_joins();
    parser::Parser p;
    auto res = p.parse("SELECT * FROM orders o NATURAL JOIN sessions s");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());
    const auto* proj = a.projection_of(res.value());
    CHECK(proj != nullptr);
    if (proj == nullptr) return;
    CHECK(proj->size() == 4);  // user_id coalesced to one
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

void test_aggregate_in_where_flagged() {
    std::printf("test_aggregate_in_where_flagged\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    // COUNT(*) in WHERE is illegal: aggregates belong in HAVING.
    auto res = p.parse("SELECT dept FROM emp WHERE COUNT(*) > 1 GROUP BY dept");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::AggregateInWhere) == 1);
    CHECK(a.has_errors());
}

void test_normal_where_not_flagged() {
    std::printf("test_normal_where_not_flagged\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    // A plain predicate in WHERE has no aggregate: nothing to flag.
    auto res = p.parse("SELECT dept FROM emp WHERE salary > 1000 GROUP BY dept");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::AggregateInWhere) == 0);
}

void test_aggregate_in_having_not_flagged() {
    std::printf("test_aggregate_in_having_not_flagged\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    // The same aggregate placed correctly in HAVING must not trip the WHERE rule.
    auto res = p.parse("SELECT dept FROM emp GROUP BY dept HAVING COUNT(*) > 1");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::AggregateInWhere) == 0);
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

void test_groupby_positional_single() {
    std::printf("test_groupby_positional_single\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    // GROUP BY 1 refers to the 1st output column (dept); COUNT(*) is aggregated.
    auto res = p.parse("SELECT dept, COUNT(*) FROM emp GROUP BY 1");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    // Clean: positional key groups by dept, so dept is a grouping key.
    CHECK(!a.has_errors());
    CHECK(count_code(a, DiagnosticCode::NonGroupedColumn) == 0);
}

void test_groupby_positional_multi() {
    std::printf("test_groupby_positional_multi\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    // GROUP BY 1, 2 groups by the 1st (dept) and 2nd (age) output columns.
    auto res = p.parse("SELECT dept, age, COUNT(*) FROM emp GROUP BY 1, 2");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    // Clean: both dept and age are grouping keys via their positions.
    CHECK(!a.has_errors());
    CHECK(count_code(a, DiagnosticCode::NonGroupedColumn) == 0);
}

void test_groupby_positional_still_flags_non_grouped() {
    std::printf("test_groupby_positional_still_flags_non_grouped\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    // GROUP BY 1 groups by dept; age is neither grouped nor aggregated, so it
    // is still an illegal non-grouped column (the fix must not silence this).
    auto res = p.parse("SELECT dept, age FROM emp GROUP BY 1");
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

// --- Extended built-in function catalog --------------------------------

// A representative subset of the scalar catalog: string, numeric and
// date/time functions resolve to their proper types and, being recognized,
// raise no UnknownFunction warning.
void test_scalar_catalog_string_numeric_types() {
    std::printf("test_scalar_catalog_string_numeric_types\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    auto res = p.parse(
        "SELECT SUBSTR(name, 1, 3), CHAR_LENGTH(name), ROUND(salary), "
        "POWER(age, 2), SIGN(age) FROM emp");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());
    // Every function above is in the catalog, so no spurious warnings.
    CHECK(count_code(a, DiagnosticCode::UnknownFunction) == 0);

    ASTNode* sub = find_function(res.value(), "SUBSTR");
    CHECK(sub != nullptr && a.type_of(sub) == DataType::Text);
    ASTNode* clen = find_function(res.value(), "CHAR_LENGTH");
    CHECK(clen != nullptr && a.type_of(clen) == DataType::Integer);
    // ROUND preserves the (numeric) argument type: salary is DOUBLE.
    ASTNode* rnd = find_function(res.value(), "ROUND");
    CHECK(rnd != nullptr && a.type_of(rnd) == DataType::Double);
    // POWER widens to an approximate DOUBLE result.
    ASTNode* pw = find_function(res.value(), "POWER");
    CHECK(pw != nullptr && a.type_of(pw) == DataType::Double);
    ASTNode* sg = find_function(res.value(), "SIGN");
    CHECK(sg != nullptr && a.type_of(sg) == DataType::Integer);
}

// NOW() is niladic and never returns NULL; it is a recognized function.
void test_scalar_catalog_now_not_null() {
    std::printf("test_scalar_catalog_now_not_null\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    auto res = p.parse("SELECT NOW() FROM emp");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::UnknownFunction) == 0);
    ASTNode* now = find_function(res.value(), "NOW");
    CHECK(now != nullptr && a.type_of(now) == DataType::Timestamp);
    CHECK(now != nullptr && now->context.analysis.nullability == 1);  // NOT NULL
}

// The newly registered statistical / collection / boolean aggregates carry
// their proper result types and produce no UnknownFunction warning.
void test_aggregate_catalog_types() {
    std::printf("test_aggregate_catalog_types\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    auto res = p.parse(
        "SELECT STDDEV(salary), VAR_POP(salary), STRING_AGG(name, ','), "
        "BOOL_OR(age > 30) FROM emp");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::UnknownFunction) == 0);

    ASTNode* sd = find_function(res.value(), "STDDEV");
    CHECK(sd != nullptr && a.type_of(sd) == DataType::Double);
    ASTNode* vp = find_function(res.value(), "VAR_POP");
    CHECK(vp != nullptr && a.type_of(vp) == DataType::Double);
    ASTNode* sa = find_function(res.value(), "STRING_AGG");
    CHECK(sa != nullptr && a.type_of(sa) == DataType::Text);
    ASTNode* bo = find_function(res.value(), "BOOL_OR");
    CHECK(bo != nullptr && a.type_of(bo) == DataType::Boolean);
    // An aggregate over a possibly-empty group is nullable.
    CHECK(sd != nullptr && sd->context.analysis.nullability == 2);
}

// A new aggregate must be treated as an aggregate by the grouping logic:
// a bare non-grouped column alongside STDDEV(...) is illegal.
void test_new_aggregate_forces_grouping() {
    std::printf("test_new_aggregate_forces_grouping\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    auto res = p.parse("SELECT name, STDDEV(salary) FROM emp");
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

// Explicit-column-list INSERT, parsed end-to-end: the parser emits the
// InsertStmt shape (TableRef, ColumnList[Identifier...], ValuesClause[row]).
void test_insert_explicit_columns_clean() {
    std::printf("test_insert_explicit_columns_clean\n");
    auto cat = make_catalog();
    parser::Parser p;
    auto res = p.parse("INSERT INTO users (id, name) VALUES (1, 'a')");
    CHECK(res.has_value());
    if (!res) return;
    // The parser really produced an explicit column list.
    CHECK(res.value()->node_type == NodeType::InsertStmt);
    CHECK(find_child(res.value(), NodeType::ColumnList) != nullptr);

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());
    CHECK(a.diagnostics().empty());
}

void test_insert_explicit_unknown_column() {
    std::printf("test_insert_explicit_unknown_column\n");
    auto cat = make_catalog();
    parser::Parser p;
    // bogus is not a column of users.
    auto res = p.parse("INSERT INTO users (id, bogus) VALUES (1, 2)");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::UnresolvedColumn) == 1);
}

void test_insert_duplicate_column_flagged() {
    std::printf("test_insert_duplicate_column_flagged\n");
    auto cat = make_catalog();
    parser::Parser p;
    // `id` is named twice in the target column list.
    auto res = p.parse("INSERT INTO users (id, id) VALUES (1, 2)");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::DuplicateColumn) == 1);
    CHECK(a.has_errors());
}

void test_insert_distinct_columns_not_flagged() {
    std::printf("test_insert_distinct_columns_not_flagged\n");
    auto cat = make_catalog();
    parser::Parser p;
    // A normal INSERT with distinct target columns has no duplicate.
    auto res = p.parse("INSERT INTO users (id, name) VALUES (1, 'a')");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::DuplicateColumn) == 0);
}

void test_insert_not_null_violation() {
    std::printf("test_insert_not_null_violation\n");
    auto cat = make_catalog();  // id is NOT NULL with no default
    parser::Parser p;
    // id is omitted from the explicit column list.
    auto res = p.parse("INSERT INTO users (name) VALUES ('a')");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
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
    parser::Parser p;
    auto res = p.parse("INSERT INTO users (name) VALUES ('a')");  // id omitted, but has default
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
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

void test_update_where() {
    std::printf("test_update_where\n");
    auto cat = make_catalog();
    parser::Parser p;
    auto res = p.parse("UPDATE users SET name = 'b' WHERE id = 1");
    CHECK(res.has_value());
    if (!res) return;
    // The parser really produced a DML WHERE clause.
    CHECK(res.value()->node_type == NodeType::UpdateStmt);
    CHECK(find_child(res.value(), NodeType::WhereClause) != nullptr);

    Analyzer a(cat);
    a.analyze(res.value());
    // id resolves against the target table; the assignment is clean.
    CHECK(!a.has_errors());
    CHECK(a.diagnostics().empty());
}

void test_update_where_unresolved() {
    std::printf("test_update_where_unresolved\n");
    auto cat = make_catalog();
    parser::Parser p;
    // bogus in the WHERE predicate is not a column of users.
    auto res = p.parse("UPDATE users SET name = 'b' WHERE bogus = 1");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
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

void test_delete_where() {
    std::printf("test_delete_where\n");
    auto cat = make_catalog();
    parser::Parser p;
    auto res = p.parse("DELETE FROM users WHERE id = 1");
    CHECK(res.has_value());
    if (!res) return;
    CHECK(res.value()->node_type == NodeType::DeleteStmt);
    CHECK(find_child(res.value(), NodeType::WhereClause) != nullptr);

    Analyzer a(cat);
    a.analyze(res.value());
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

void test_limit_negative() {
    std::printf("test_limit_negative\n");
    auto cat = make_catalog();
    parser::Parser p;
    // A negative LIMIT literal parses as LimitClause -> IntegerLiteral "-1".
    auto res = p.parse("SELECT id FROM users LIMIT -1");
    CHECK(res.has_value());
    if (!res) return;
    ASTNode* limit = find_child(res.value(), NodeType::LimitClause);
    CHECK(limit != nullptr);

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::InvalidLimit) == 1);
}

void test_float_literal_type() {
    std::printf("test_float_literal_type\n");
    auto cat = make_catalog();
    parser::Parser p;
    // A fractional / scientific literal parses as a FloatLiteral and types Double.
    auto res = p.parse("SELECT 1.5, 1e3 FROM users");
    CHECK(res.has_value());
    if (!res) return;

    ASTNode* list = find_child(res.value(), NodeType::SelectList);
    ASTNode* f1 = list ? first_child(list) : nullptr;
    ASTNode* f2 = f1 ? f1->next_sibling : nullptr;
    CHECK(f1 != nullptr && f1->node_type == NodeType::FloatLiteral);
    CHECK(f2 != nullptr && f2->node_type == NodeType::FloatLiteral);

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());
    CHECK(f1 != nullptr && a.type_of(f1) == DataType::Double);
    CHECK(f2 != nullptr && a.type_of(f2) == DataType::Double);
}

// --- Expression typing: CAST / BETWEEN / LIKE / IS NULL / window --------

void test_cast_expr_type() {
    std::printf("test_cast_expr_type\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    // CAST takes the named target type; nullability flows from the operand.
    auto res = p.parse("SELECT CAST(name AS INTEGER) FROM emp");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());
    ASTNode* cast = find_descendant(res.value(), NodeType::CastExpr);
    CHECK(cast != nullptr && a.type_of(cast) == DataType::Integer);
    CHECK(cast != nullptr && a.nullability_of(cast) == 2);  // name is nullable
}

void test_cast_varchar_type() {
    std::printf("test_cast_varchar_type\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    // A length (VARCHAR(10)) does not change the type category; a NOT NULL
    // operand keeps the cast not-null.
    auto res = p.parse("SELECT CAST(id AS VARCHAR(10)) FROM emp");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());
    ASTNode* cast = find_descendant(res.value(), NodeType::CastExpr);
    CHECK(cast != nullptr && a.type_of(cast) == DataType::VarChar);
    CHECK(cast != nullptr && a.nullability_of(cast) == 1);  // id is NOT NULL
}

void test_between_boolean() {
    std::printf("test_between_boolean\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    auto res = p.parse("SELECT id FROM emp WHERE salary BETWEEN 100 AND 500");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());
    CHECK(a.diagnostics().empty());  // numeric vs numeric: clean
    ASTNode* bt = find_descendant(res.value(), NodeType::BetweenExpr);
    CHECK(bt != nullptr && a.type_of(bt) == DataType::Boolean);
    CHECK(bt != nullptr && a.nullability_of(bt) == 2);  // salary is nullable
}

void test_between_coercion_warns() {
    std::printf("test_between_coercion_warns\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    // Text value against integer bounds: a soft cross-category coercion.
    auto res = p.parse("SELECT id FROM emp WHERE name BETWEEN 1 AND 9");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());  // a warning, not an error
    CHECK(count_code(a, DiagnosticCode::ImplicitCoercion) == 1);
}

void test_like_boolean() {
    std::printf("test_like_boolean\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    auto res = p.parse("SELECT id FROM emp WHERE name LIKE 'a%'");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());
    CHECK(a.diagnostics().empty());  // text LIKE text-pattern: clean
    ASTNode* lk = find_descendant(res.value(), NodeType::LikeExpr);
    CHECK(lk != nullptr && a.type_of(lk) == DataType::Boolean);
    CHECK(lk != nullptr && a.nullability_of(lk) == 2);  // name is nullable
}

void test_like_nonstring_warns() {
    std::printf("test_like_nonstring_warns\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    // LIKE on an integer column: a soft non-string coercion warning.
    auto res = p.parse("SELECT id FROM emp WHERE id LIKE 'a%'");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());
    CHECK(count_code(a, DiagnosticCode::ImplicitCoercion) == 1);
}

void test_is_null_boolean_notnull() {
    std::printf("test_is_null_boolean_notnull\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    // IS [NOT] NULL always yields a defined boolean, even on a nullable operand.
    auto res = p.parse("SELECT id FROM emp WHERE name IS NULL");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());
    ASTNode* isn = find_descendant(res.value(), NodeType::IsNullExpr);
    CHECK(isn != nullptr && a.type_of(isn) == DataType::Boolean);
    CHECK(isn != nullptr && a.nullability_of(isn) == 1);  // never NULL
}

void test_window_rank_type() {
    std::printf("test_window_rank_type\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    // A ranking window function is BIGINT and never NULL; the OVER clause's
    // partition/order columns resolve, and RANK is not an unknown function.
    auto res = p.parse(
        "SELECT RANK() OVER (PARTITION BY dept ORDER BY salary) FROM emp");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());
    CHECK(count_code(a, DiagnosticCode::UnknownFunction) == 0);
    ASTNode* fn = find_descendant(res.value(), NodeType::FunctionCall);
    CHECK(fn != nullptr && a.type_of(fn) == DataType::BigInt);
    CHECK(fn != nullptr && a.nullability_of(fn) == 1);
}

void test_window_sum_over_type() {
    std::printf("test_window_sum_over_type\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    // An aggregate used as a window function keeps its aggregate result type
    // (SUM over a DOUBLE stays DOUBLE) and is nullable.
    auto res = p.parse("SELECT SUM(salary) OVER (PARTITION BY dept) FROM emp");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());
    ASTNode* fn = find_descendant(res.value(), NodeType::FunctionCall);
    CHECK(fn != nullptr && a.type_of(fn) == DataType::Double);
    CHECK(fn != nullptr && a.nullability_of(fn) == 2);
}

void test_window_aggregate_does_not_force_grouping() {
    std::printf("test_window_aggregate_does_not_force_grouping\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    // A windowed aggregate does not collapse rows, so a bare column alongside it
    // must NOT be reported as a non-grouped column.
    auto res = p.parse(
        "SELECT id, SUM(salary) OVER (PARTITION BY dept) FROM emp");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());
    CHECK(count_code(a, DiagnosticCode::NonGroupedColumn) == 0);
}

// --- Ambiguous column resolution ---------------------------------------

void test_ambiguous_column() {
    std::printf("test_ambiguous_column\n");
    auto cat = make_catalog_null();  // users(id, note) and orders(id, uid, amount)
    parser::Parser p;
    // `id` exists in both joined relations and is referenced UNqualified, so it
    // cannot be resolved to a single relation.
    auto res = p.parse("SELECT id FROM users u JOIN orders o ON u.id = o.uid");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::AmbiguousColumn) == 1);
    CHECK(a.has_errors());
}

// --- RIGHT / FULL JOIN nullability -------------------------------------

void test_right_join_nullability() {
    std::printf("test_right_join_nullability\n");
    auto cat = make_catalog_null();
    parser::Parser p;
    // RIGHT JOIN: the LEFT relation (users u) is null-supplied, so u.id becomes
    // nullable even though it is NOT NULL in the catalog; the right side (orders
    // o) keeps its NOT NULL. The parser emits this as a JoinClause whose
    // primary_text is "RIGHT JOIN", which drives the join_null_side path.
    auto res = p.parse("SELECT o.id, u.id FROM users u RIGHT JOIN orders o ON u.id = o.uid");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());

    ASTNode* list = find_child(res.value(), NodeType::SelectList);
    ASTNode* oid = first_child(list);
    ASTNode* uid = oid ? oid->next_sibling : nullptr;
    CHECK(oid != nullptr && a.nullability_of(oid) == 1);  // preserved (right side)
    CHECK(uid != nullptr && a.nullability_of(uid) == 2);  // null-supplied (left side)
}

void test_full_join_nullability() {
    std::printf("test_full_join_nullability\n");
    auto cat = make_catalog_null();
    parser::Parser p;
    // FULL JOIN: both sides are null-supplied, so both o.id and u.id become
    // nullable regardless of their base NOT NULL constraints.
    auto res = p.parse("SELECT o.id, u.id FROM users u FULL JOIN orders o ON u.id = o.uid");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());

    ASTNode* list = find_child(res.value(), NodeType::SelectList);
    ASTNode* oid = first_child(list);
    ASTNode* uid = oid ? oid->next_sibling : nullptr;
    CHECK(oid != nullptr && a.nullability_of(oid) == 2);  // null-supplied
    CHECK(uid != nullptr && a.nullability_of(uid) == 2);  // null-supplied
}

// --- Bind-parameter typing ---------------------------------------------

// The consumed parser build drops bind parameters ($1 / ?) before the analyzer
// sees them (they never appear as a NodeType::Parameter in a parse tree; see
// docs/DESIGN.md). We exercise the analyzer's Parameter typing path directly by
// synthesizing the node shape the parser is meant to produce: an expression leaf
// whose node_type is Parameter, sitting where an operand is inferred.
void test_parameter_typing() {
    std::printf("test_parameter_typing\n");
    auto cat = make_catalog_null();
    // Real end-to-end: the parser now emits a NodeType::Parameter node for `?`
    // and `$1` placeholders (previously it dropped them, so this test had to
    // synthesize the node). The RHS of the WHERE comparison parses as a genuine
    // Parameter, which the analyzer then types.
    for (const char* sql : {"SELECT id FROM users WHERE id = ?",
                            "SELECT id FROM users WHERE id = $1"}) {
        parser::Parser p;
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) continue;

        ASTNode* param = find_descendant(res.value(), NodeType::Parameter);
        CHECK(param != nullptr);  // parser produced a real Parameter node
        if (param == nullptr) continue;

        Analyzer a(cat);
        a.analyze(res.value());
        // A parameter's type is unknown until bound, and it may be bound to NULL.
        CHECK(a.type_of(param) == DataType::Unknown);
        CHECK(a.nullability_of(param) == 2);
    }
}

// --- Correlated scalar subquery ----------------------------------------

void test_correlated_scalar_subquery() {
    std::printf("test_correlated_scalar_subquery\n");
    auto cat = make_catalog_null();
    parser::Parser p;
    // The scalar subquery in the SELECT list references the outer u.id, so it
    // resolves against the enclosing query and is marked correlated (no
    // diagnostic: this is legal). It projects one column (amount -> Double).
    auto res = p.parse(
        "SELECT (SELECT amount FROM orders o WHERE o.uid = u.id) FROM users u");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());

    ASTNode* subq = find_descendant(res.value(), NodeType::Subquery);
    CHECK(subq != nullptr && a.is_correlated(subq));
    CHECK(subq != nullptr && a.type_of(subq) == DataType::Double);
}

void test_correlated_aggregate_subquery_not_grouped() {
    std::printf("test_correlated_aggregate_subquery_not_grouped\n");
    auto cat = make_catalog_null();
    parser::Parser p;
    // A correlated COUNT(*) subquery in the SELECT list. The aggregate lives in
    // the INNER query block; it must NOT make the OUTER query grouped, and the
    // outer grouping legality rule must NOT reach across the subquery boundary.
    // The correlated reference to the outer u.id (and the inner o.uid) are not
    // columns of the outer relation, so neither may be flagged NonGroupedColumn.
    auto res = p.parse(
        "SELECT (SELECT COUNT(*) FROM orders o WHERE o.uid = u.id) FROM users u");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());
    CHECK(count_code(a, DiagnosticCode::NonGroupedColumn) == 0);

    ASTNode* subq = find_descendant(res.value(), NodeType::Subquery);
    CHECK(subq != nullptr && a.is_correlated(subq));
}

void test_subquery_grouping_boundary_guard() {
    std::printf("test_subquery_grouping_boundary_guard\n");
    parser::Parser p;

    // Guard 1: a single-block query with an aggregate but no GROUP BY still
    // flags the bare non-grouped column (the fix must not weaken this).
    {
        auto cat = make_catalog_emp();
        auto res = p.parse("SELECT dept, COUNT(*) FROM emp");
        CHECK(res.has_value());
        if (res) {
            Analyzer a(cat);
            a.analyze(res.value());
            CHECK(count_code(a, DiagnosticCode::NonGroupedColumn) == 1);
        }
    }

    // Guard 2: an INNER subquery that is itself illegally grouped still fires a
    // NonGroupedColumn from its OWN grouping analysis. Stopping the outer pass
    // at the subquery boundary must not disable the inner block's own check.
    {
        auto cat = make_catalog_null();
        auto res = p.parse("SELECT (SELECT uid FROM orders GROUP BY id) FROM users");
        CHECK(res.has_value());
        if (res) {
            Analyzer a(cat);
            a.analyze(res.value());
            CHECK(count_code(a, DiagnosticCode::NonGroupedColumn) == 1);
        }
    }
}

// --- Aggregate result typing (SUM promotion, MIN/MAX preservation) ------

void test_sum_integer_promotes_bigint() {
    std::printf("test_sum_integer_promotes_bigint\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    // SUM over an INTEGER column widens to BIGINT (overflow-safe accumulation).
    auto res = p.parse("SELECT SUM(age) FROM emp");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());
    ASTNode* sum = find_function(res.value(), "SUM");
    CHECK(sum != nullptr && a.type_of(sum) == DataType::BigInt);
}

void test_min_max_preserve_type() {
    std::printf("test_min_max_preserve_type\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    // MIN/MAX return the input column's type unchanged.
    auto res = p.parse("SELECT MIN(salary), MAX(age) FROM emp");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());
    ASTNode* mn = find_function(res.value(), "MIN");
    ASTNode* mx = find_function(res.value(), "MAX");
    CHECK(mn != nullptr && a.type_of(mn) == DataType::Double);   // salary is DOUBLE
    CHECK(mx != nullptr && a.type_of(mx) == DataType::Integer);  // age is INTEGER
}

// --- INTERSECT / EXCEPT set-operation roots ----------------------------

void test_intersect_except_roots() {
    std::printf("test_intersect_except_roots\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    // INTERSECT and EXCEPT produce their own dedicated root statement node types.
    auto in = p.parse("SELECT id FROM emp INTERSECT SELECT age FROM emp");
    CHECK(in.has_value());
    if (in) {
        Analyzer a(cat);
        a.analyze(in.value());
        CHECK(in.value()->node_type == NodeType::IntersectStmt);
    }
    auto ex = p.parse("SELECT id FROM emp EXCEPT SELECT age FROM emp");
    CHECK(ex.has_value());
    if (ex) {
        Analyzer a(cat);
        a.analyze(ex.value());
        CHECK(ex.value()->node_type == NodeType::ExceptStmt);
    }
}

// --- Temporal arithmetic ------------------------------------------------
//
// Analyze `SELECT <arith> FROM events`, returning the SELECT-list BinaryExpr.
ASTNode* analyze_temporal(Analyzer& a, parser::Parser& p,
                          std::optional<parser::ParseResult>& holder,
                          const char* sql) {
    holder = p.parse(sql);
    CHECK(holder->has_value());
    if (!holder->has_value()) return nullptr;
    a.analyze(holder->value());
    return find_descendant(holder->value(), NodeType::BinaryExpr);
}

// EXTRACT(YEAR FROM ts): the leading YEAR is a date-part keyword, NOT a column.
// It must not be reported as an unresolved column, and the call types as Double.
void test_extract_datepart_not_column() {
    std::printf("test_extract_datepart_not_column\n");
    auto cat = make_catalog_temporal();
    parser::Parser p;
    auto h = p.parse("SELECT EXTRACT(YEAR FROM ts) FROM events");
    CHECK(h.has_value());
    if (!h.has_value()) return;
    Analyzer a(cat);
    a.analyze(h.value());
    CHECK(!a.has_errors());
    CHECK(count_code(a, DiagnosticCode::UnresolvedColumn) == 0);
    const ASTNode* ex = find_descendant(h.value(), NodeType::FunctionCall);
    CHECK(ex != nullptr && a.type_of(ex) == DataType::Double);
}

// date + interval -> Date, and not-null since both operands are not-null.
void test_temporal_date_plus_interval() {
    std::printf("test_temporal_date_plus_interval\n");
    auto cat = make_catalog_temporal();
    parser::Parser p;
    std::optional<parser::ParseResult> h;
    Analyzer a(cat);
    ASTNode* e = analyze_temporal(a, p, h, "SELECT d + iv FROM events");
    CHECK(count_code(a, DiagnosticCode::TypeMismatch) == 0);
    CHECK(e != nullptr && a.type_of(e) == DataType::Date);
    CHECK(e != nullptr && a.nullability_of(e) == 1);  // NOT NULL
}

// timestamp + interval -> Timestamp.
void test_temporal_timestamp_plus_interval() {
    std::printf("test_temporal_timestamp_plus_interval\n");
    auto cat = make_catalog_temporal();
    parser::Parser p;
    std::optional<parser::ParseResult> h;
    Analyzer a(cat);
    ASTNode* e = analyze_temporal(a, p, h, "SELECT ts + iv FROM events");
    CHECK(count_code(a, DiagnosticCode::TypeMismatch) == 0);
    CHECK(e != nullptr && a.type_of(e) == DataType::Timestamp);
}

// date + integer -> Date (day arithmetic).
void test_temporal_date_plus_integer() {
    std::printf("test_temporal_date_plus_integer\n");
    auto cat = make_catalog_temporal();
    parser::Parser p;
    std::optional<parser::ParseResult> h;
    Analyzer a(cat);
    ASTNode* e = analyze_temporal(a, p, h, "SELECT d + n FROM events");
    CHECK(count_code(a, DiagnosticCode::TypeMismatch) == 0);
    CHECK(e != nullptr && a.type_of(e) == DataType::Date);
}

// timestamp - timestamp -> Interval (elapsed span, not Timestamp).
void test_temporal_timestamp_minus_timestamp() {
    std::printf("test_temporal_timestamp_minus_timestamp\n");
    auto cat = make_catalog_temporal();
    parser::Parser p;
    std::optional<parser::ParseResult> h;
    Analyzer a(cat);
    ASTNode* e = analyze_temporal(a, p, h, "SELECT ts - ts FROM events");
    CHECK(count_code(a, DiagnosticCode::TypeMismatch) == 0);
    CHECK(e != nullptr && a.type_of(e) == DataType::Interval);
}

// date - date -> Interval (not Date).
void test_temporal_date_minus_date() {
    std::printf("test_temporal_date_minus_date\n");
    auto cat = make_catalog_temporal();
    parser::Parser p;
    std::optional<parser::ParseResult> h;
    Analyzer a(cat);
    ASTNode* e = analyze_temporal(a, p, h, "SELECT d - d FROM events");
    CHECK(count_code(a, DiagnosticCode::TypeMismatch) == 0);
    CHECK(e != nullptr && a.type_of(e) == DataType::Interval);
}

// A nullable temporal operand makes the temporal result nullable.
void test_temporal_nullable_operand() {
    std::printf("test_temporal_nullable_operand\n");
    auto cat = make_catalog_temporal();
    parser::Parser p;
    std::optional<parser::ParseResult> h;
    Analyzer a(cat);
    ASTNode* e = analyze_temporal(a, p, h, "SELECT d2 + iv FROM events");
    CHECK(count_code(a, DiagnosticCode::TypeMismatch) == 0);
    CHECK(e != nullptr && a.type_of(e) == DataType::Date);
    CHECK(e != nullptr && a.nullability_of(e) == 2);  // nullable
}

// date + timestamp has no defined meaning -> TypeMismatch, result Unknown.
void test_temporal_invalid_date_plus_timestamp() {
    std::printf("test_temporal_invalid_date_plus_timestamp\n");
    auto cat = make_catalog_temporal();
    parser::Parser p;
    std::optional<parser::ParseResult> h;
    Analyzer a(cat);
    ASTNode* e = analyze_temporal(a, p, h, "SELECT d + ts FROM events");
    CHECK(count_code(a, DiagnosticCode::TypeMismatch) == 1);
    CHECK(e != nullptr && a.type_of(e) == DataType::Unknown);
}

// --- String concatenation (||) ------------------------------------------

// first || last -> Text, not-null when both operands are not-null.
void test_concat_text_notnull() {
    std::printf("test_concat_text_notnull\n");
    auto cat = make_catalog_people();
    parser::Parser p;
    auto res = p.parse("SELECT first || last FROM people");
    CHECK(res.has_value());
    if (!res) return;
    Analyzer a(cat);
    a.analyze(res.value());
    ASTNode* e = find_descendant(res.value(), NodeType::BinaryExpr);
    CHECK(e != nullptr && a.type_of(e) == DataType::Text);
    CHECK(e != nullptr && a.nullability_of(e) == 1);  // NOT NULL
}

// first || mid -> Text, nullable because `mid` is nullable.
void test_concat_text_nullable() {
    std::printf("test_concat_text_nullable\n");
    auto cat = make_catalog_people();
    parser::Parser p;
    auto res = p.parse("SELECT first || mid FROM people");
    CHECK(res.has_value());
    if (!res) return;
    Analyzer a(cat);
    a.analyze(res.value());
    ASTNode* e = find_descendant(res.value(), NodeType::BinaryExpr);
    CHECK(e != nullptr && a.type_of(e) == DataType::Text);
    CHECK(e != nullptr && a.nullability_of(e) == 2);  // nullable
}

// --- projection_of column identity --------------------------------------

// A direct column reference in the SELECT list carries its resolved base
// (table_id, column_id) into projection_of, matching what `SELECT *` records.
void test_projection_column_identity() {
    std::printf("test_projection_column_identity\n");
    auto cat = make_catalog();
    parser::Parser p;
    auto res = p.parse("SELECT users.id, name FROM users");
    CHECK(res.has_value());
    if (!res) return;
    Analyzer a(cat);
    a.analyze(res.value());

    const auto* proj = a.projection_of(res.value());
    CHECK(proj != nullptr);
    if (proj != nullptr) {
        CHECK(proj->size() == 2);
        // Both output columns carry a non-zero base identity.
        CHECK((*proj)[0].name == "id" && (*proj)[0].table_id != 0 &&
              (*proj)[0].column_id != 0);
        CHECK((*proj)[1].name == "name" && (*proj)[1].table_id != 0 &&
              (*proj)[1].column_id != 0);
    }

    // The identities must match what `SELECT *` produces for the same table.
    auto star = p.parse("SELECT * FROM users");
    CHECK(star.has_value());
    if (star && proj != nullptr && proj->size() == 2) {
        Analyzer a2(cat);
        a2.analyze(star.value());
        const auto* sp = a2.projection_of(star.value());
        CHECK(sp != nullptr && sp->size() == 2);
        if (sp != nullptr && sp->size() == 2) {
            CHECK((*proj)[0].table_id == (*sp)[0].table_id &&
                  (*proj)[0].column_id == (*sp)[0].column_id);
            CHECK((*proj)[1].table_id == (*sp)[1].table_id &&
                  (*proj)[1].column_id == (*sp)[1].column_id);
        }
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
    test_qualified_star_e2e_expands();
    test_qualified_star_e2e_mixed();
    test_qualified_star_e2e_bad_qualifier();

    // JOIN ON / USING resolution
    test_join_on_resolves();
    test_from_duplicate_alias_flagged();
    test_join_duplicate_alias_flagged();
    test_from_distinct_aliases_not_flagged();
    test_self_join_distinct_aliases_not_flagged();
    test_join_on_unresolved_column();
    test_join_using_resolves();
    test_join_using_missing();
    test_join_using_coalesces_bare_ref();
    test_natural_join_coalesces_bare_ref();
    test_join_on_shared_name_still_ambiguous();
    test_star_over_using_coalesces();
    test_star_over_natural_coalesces();

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
    test_aggregate_in_where_flagged();
    test_normal_where_not_flagged();
    test_aggregate_in_having_not_flagged();
    test_order_by_non_grouped();
    test_groupby_positional_single();
    test_groupby_positional_multi();
    test_groupby_positional_still_flags_non_grouped();
    test_avg_result_type();
    test_scalar_function_type();
    test_unknown_function_degrades();
    test_aggregate_makes_query_grouped();

    // Extended built-in function catalog
    test_scalar_catalog_string_numeric_types();
    test_scalar_catalog_now_not_null();
    test_aggregate_catalog_types();
    test_new_aggregate_forces_grouping();

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
    test_insert_duplicate_column_flagged();
    test_insert_distinct_columns_not_flagged();
    test_insert_not_null_violation();
    test_insert_not_null_with_default_ok();

    // DML: UPDATE
    test_update_clean();
    test_update_unknown_column();
    test_update_type_diagnostic();
    test_update_where();
    test_update_where_unresolved();

    // DML: DELETE
    test_delete_clean();
    test_delete_unknown_table();
    test_delete_where();

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
    test_limit_negative();
    test_float_literal_type();

    // Expression typing: CAST / BETWEEN / LIKE / IS NULL / window functions.
    test_cast_expr_type();
    test_cast_varchar_type();
    test_between_boolean();
    test_between_coercion_warns();
    test_like_boolean();
    test_like_nonstring_warns();
    test_is_null_boolean_notnull();
    test_window_rank_type();
    test_window_sum_over_type();
    test_window_aggregate_does_not_force_grouping();

    // Ambiguous column resolution
    test_ambiguous_column();

    // RIGHT / FULL JOIN nullability
    test_right_join_nullability();
    test_full_join_nullability();

    // Bind-parameter typing
    test_parameter_typing();

    // Correlated scalar subquery
    test_correlated_scalar_subquery();
    test_correlated_aggregate_subquery_not_grouped();
    test_subquery_grouping_boundary_guard();

    // Aggregate result typing & INTERSECT / EXCEPT roots
    test_sum_integer_promotes_bigint();
    test_min_max_preserve_type();
    test_intersect_except_roots();

    // Temporal arithmetic
    test_extract_datepart_not_column();
    test_temporal_date_plus_interval();
    test_temporal_timestamp_plus_interval();
    test_temporal_date_plus_integer();
    test_temporal_timestamp_minus_timestamp();
    test_temporal_date_minus_date();
    test_temporal_nullable_operand();
    test_temporal_invalid_date_plus_timestamp();

    // String concatenation (||)
    test_concat_text_notnull();
    test_concat_text_nullable();

    // projection_of column identity
    test_projection_column_identity();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
