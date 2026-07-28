// DB25 Semantic Analyzer - test suite (self-contained assertion harness).
//
// Real assertions, not prints: every check updates a pass/fail tally and the
// process exits non-zero if anything fails.

#include "db25/parser/parser.hpp"
#include "db25/semantic/analyzer.hpp"
#include "db25/semantic/ast_helpers.hpp"
#include "db25/semantic/check_eval.hpp"
#include "db25/semantic/catalog.hpp"
#include "db25/semantic/identifier.hpp"

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

// A deliberately WIDE table (more columns than RelationBinding's index
// threshold, 16) so column resolution exercises the hashed name-index path
// rather than the linear scan. w0..w23; even-indexed columns are nullable.
InMemoryCatalog make_catalog_wide() {
    InMemoryCatalog cat;
    std::vector<ColumnInfo> cols;
    for (int i = 0; i < 24; ++i) {
        cols.push_back(ColumnInfo{"w" + std::to_string(i), DataType::Integer,
                                  /*nullable=*/(i % 2 == 0)});
    }
    cat.add_table("wide", std::move(cols));
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

void test_derived_table_column_aliases() {
    std::printf("test_derived_table_column_aliases\n");
    auto cat = make_catalog();  // users(id INTEGER NOT NULL, name TEXT)
    parser::Parser p;

    // "(...) AS t(a, b)" renames the derived table's output columns positionally:
    // a qualified reference to an ALIAS resolves, and its type flows from the
    // underlying column.
    {
        auto res = p.parse("SELECT t.a, t.b FROM (SELECT id, name FROM users) AS t(a, b)");
        CHECK(res.has_value());
        if (res) {
            Analyzer a(cat);
            a.analyze(res.value());
            CHECK(!a.has_errors());
            ASTNode* list = find_child(res.value(), NodeType::SelectList);
            ASTNode* ta = first_child(list);
            CHECK(ta != nullptr && a.type_of(ta) == DataType::Integer);       // a <- id
            CHECK(ta != nullptr && a.type_of(ta->next_sibling) == DataType::Text);  // b <- name
        }
    }
    // A reference to the ORIGINAL inner name no longer resolves - the alias
    // replaces it.
    {
        auto res = p.parse("SELECT t.id FROM (SELECT id FROM users) AS t(a)");
        CHECK(res.has_value());
        if (res) {
            Analyzer a(cat);
            a.analyze(res.value());
            CHECK(count_code(a, DiagnosticCode::UnresolvedColumn) == 1);
        }
    }
    // SELECT * over the aliased derived table expands to the alias names (clean).
    {
        auto res = p.parse("SELECT * FROM (SELECT id FROM users) AS t(only)");
        CHECK(res.has_value());
        if (res) {
            Analyzer a(cat);
            a.analyze(res.value());
            CHECK(!a.has_errors());
        }
    }
    // More aliases than the derived table has columns is an error.
    {
        auto res = p.parse("SELECT * FROM (SELECT id FROM users) AS t(a, b)");
        CHECK(res.has_value());
        if (res) {
            Analyzer a(cat);
            a.analyze(res.value());
            CHECK(count_code(a, DiagnosticCode::ColumnAliasCountMismatch) == 1);
        }
    }
}

// --- Case-insensitive identifier resolution (finding #20) ---------------
//
// DB25 resolves identifiers case-insensitively over ASCII: the tokenizer strips
// the quotes from a delimited identifier and emits it as a plain identifier
// token, so the analyzer cannot tell `"Foo"` from `Foo` and folds every
// identifier's case at resolution time (see identifier.hpp). These tests pin
// that a reference resolves regardless of the case it and its target were
// written in, and - just as important - that folding does not paper over real
// ambiguity or duplicate-relation errors.

void test_iequals_helper() {
    std::printf("test_iequals_helper\n");
    CHECK(iequals("users", "USERS"));
    CHECK(iequals("Order_Id", "order_id"));
    CHECK(iequals("", ""));
    CHECK(!iequals("user", "users"));       // length differs
    CHECK(!iequals("naïve", "NAÏVE"));       // non-ASCII byte is not folded
    CHECK(!iequals("col1", "col2"));
    // Hash agrees with equality: folded-equal keys must hash the same, or a
    // case-insensitive unordered_map would miss them.
    IdentifierHash h;
    CHECK(h("USERS") == h("users"));
    CHECK(h("Order_Id") == h("order_id"));
}

void test_case_insensitive_table_and_column() {
    std::printf("test_case_insensitive_table_and_column\n");
    auto cat = make_catalog();  // users(id INTEGER NOT NULL, name TEXT), all lower
    parser::Parser p;
    // Table and columns referenced in a different case than the catalog stores.
    auto res = p.parse("SELECT ID, Name FROM USERS");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());
    CHECK(a.diagnostics().empty());

    ASTNode* list = find_child(res.value(), NodeType::SelectList);
    ASTNode* id = first_child(list);
    ASTNode* name = id ? id->next_sibling : nullptr;
    CHECK(id != nullptr && a.type_of(id) == DataType::Integer);
    CHECK(name != nullptr && a.type_of(name) == DataType::Text);
}

void test_case_insensitive_qualifier_and_alias() {
    std::printf("test_case_insensitive_qualifier_and_alias\n");
    auto cat = make_catalog();
    parser::Parser p;
    // Alias declared as `U`; the qualifier `u` addresses it case-insensitively.
    auto res = p.parse("SELECT u.ID FROM users U");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());
    ASTNode* list = find_child(res.value(), NodeType::SelectList);
    ASTNode* uid = first_child(list);
    CHECK(uid != nullptr && a.type_of(uid) == DataType::Integer);
}

void test_case_insensitive_cte() {
    std::printf("test_case_insensitive_cte\n");
    auto cat = make_catalog();
    parser::Parser p;
    // CTE defined as `Recent`, referenced as `RECENT`.
    auto res = p.parse("WITH Recent AS (SELECT id FROM users) SELECT id FROM RECENT");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());
    CHECK(a.diagnostics().empty());
}

void test_case_insensitive_mixed_case_catalog() {
    std::printf("test_case_insensitive_mixed_case_catalog\n");
    // The catalog stores mixed-case names; a lower-case query still resolves,
    // and the stored spelling is preserved (only the lookup key folds).
    InMemoryCatalog cat;
    cat.add_table("Accounts", {
        ColumnInfo{"AccountId", DataType::Integer, /*nullable=*/false},
        ColumnInfo{"Balance", DataType::Double, /*nullable=*/true},
    });
    CHECK(cat.find_table("accounts") != nullptr);          // find_table folds case
    CHECK(cat.find_table("ACCOUNTS") == cat.find_table("Accounts"));
    const TableInfo* t = cat.find_table("accounts");
    CHECK(t != nullptr && t->name == "Accounts");          // display spelling kept
    CHECK(t != nullptr && t->find_column("accountid") != nullptr);  // find_column folds

    parser::Parser p;
    auto res = p.parse("SELECT accountid, balance FROM accounts");
    CHECK(res.has_value());
    if (!res) return;
    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());
    ASTNode* list = find_child(res.value(), NodeType::SelectList);
    ASTNode* acc = first_child(list);
    CHECK(acc != nullptr && a.type_of(acc) == DataType::Integer);
}

void test_case_insensitive_ambiguity_preserved() {
    std::printf("test_case_insensitive_ambiguity_preserved\n");
    // orders and sessions both have `user_id`; a bare reference - even in a
    // different case - is still ambiguous. Folding must not hide the conflict.
    auto cat = make_catalog_joins();
    parser::Parser p;
    auto res = p.parse(
        "SELECT User_Id FROM orders o JOIN sessions s ON o.user_id = s.user_id");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::AmbiguousColumn) == 1);
}

void test_case_insensitive_duplicate_relation() {
    std::printf("test_case_insensitive_duplicate_relation\n");
    // `users` and `USERS` name the same relation twice in FROM - a duplicate
    // correlation name, which SQL rejects. The dedup check folds case too.
    auto cat = make_catalog();
    parser::Parser p;
    auto res = p.parse("SELECT 1 FROM users, USERS");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::DuplicateRelation) == 1);
}

void test_case_insensitive_check_binding() {
    std::printf("test_case_insensitive_check_binding\n");
    // A CHECK that spells its column in a different case than the definition
    // still binds the row value, so a definite violation is caught. Exercises
    // the case-insensitive CheckBindings map.
    InMemoryCatalog cat;
    TableInfo& t = cat.add_table("t", {
        ColumnInfo{"Age", DataType::Integer, /*nullable=*/true},  // column_id 1
    });
    Constraint c; c.kind = Constraint::Kind::Check; c.expr = "AGE >= 0"; c.columns = {1};
    t.constraints.push_back(c);

    parser::Parser p;
    auto viol = [&](const char* sql) {
        auto r = p.parse(sql);
        if (!r.has_value()) return -1;
        Analyzer a(cat);
        a.analyze(r.value());
        return count_code(a, DiagnosticCode::CheckViolation);
    };
    CHECK(viol("INSERT INTO t (age) VALUES (-5)") == 1);   // column ref folds to Age
    CHECK(viol("INSERT INTO t (AGE) VALUES (5)") == 0);
}

void test_check_temporal_value_not_compared_lexically() {
    std::printf("test_check_temporal_value_not_compared_lexically\n");
    // The constant CHECK evaluator has no date parser: comparing a temporal
    // column's string value LEXICALLY is unsound ('2020-01-01 9:30:00' orders
    // GREATER than '2020-01-01 10:00:00' at the hour, '9' > '1'). Such a value is
    // left UNBOUND so the CHECK folds to Unknown - never a false violation - while
    // numeric / text CHECKs and NULL handling are unaffected.
    InMemoryCatalog cat;
    TableInfo& t = cat.add_table("t", {
        ColumnInfo{"ts", DataType::Timestamp, /*nullable=*/true},   // column_id 1
        ColumnInfo{"d", DataType::Date, /*nullable=*/true},         // column_id 2
        ColumnInfo{"note", DataType::Text, /*nullable=*/true},      // column_id 3
        ColumnInfo{"age", DataType::Integer, /*nullable=*/true},    // column_id 4
    });
    auto add_check = [&](const char* expr, std::uint32_t col) {
        Constraint c; c.kind = Constraint::Kind::Check; c.expr = expr; c.columns = {col};
        t.constraints.push_back(c);
    };
    add_check("ts < '2020-01-01 10:00:00'", 1);
    add_check("d > '2019-01-01'", 2);
    add_check("note <> 'x'", 3);
    add_check("age >= 0", 4);
    add_check("ts IS NOT NULL", 1);

    parser::Parser p;
    auto viol = [&](const char* sql) {
        auto r = p.parse(sql);
        CHECK(r.has_value());
        if (!r.has_value()) return -1;
        Analyzer a(cat);
        a.analyze(r.value());
        return count_code(a, DiagnosticCode::CheckViolation);
    };

    // The bug: a chronologically-legal timestamp was flagged by lexical order.
    CHECK(viol("INSERT INTO t (ts) VALUES ('2020-01-01 9:30:00')") == 0);
    // The zero-padded spelling of the same instant was always accepted - the fix
    // makes both spellings agree (no lexical dependence).
    CHECK(viol("INSERT INTO t (ts) VALUES ('2020-01-01 09:30:00')") == 0);
    // A DateStyle-dependent DATE value (MDY '06/15/2020' = 2020-06-15) is likewise
    // no longer lexically flagged.
    CHECK(viol("INSERT INTO t (d) VALUES ('06/15/2020')") == 0);

    // Sound comparisons are unaffected: a text-equality CHECK and a numeric CHECK
    // still catch a definite violation.
    CHECK(viol("INSERT INTO t (note) VALUES ('x')") == 1);
    CHECK(viol("INSERT INTO t (age) VALUES (-5)") == 1);
    // A NULL temporal value still binds (it is not a string literal), so an
    // IS NOT NULL CHECK still catches the violation.
    CHECK(viol("INSERT INTO t (ts) VALUES (NULL)") == 1);
}

void test_values_derived_table() {
    std::printf("test_values_derived_table\n");
    auto cat = make_catalog();
    parser::Parser p;

    // A VALUES list as a derived table: its columns are named by the alias list
    // and typed from the first row, so qualified references resolve.
    {
        auto res = p.parse(
            "SELECT v.id, v.label FROM (VALUES (1, 'eng'), (2, 'sales')) AS v(id, label) "
            "WHERE v.id = 1");
        CHECK(res.has_value());
        if (res) {
            Analyzer a(cat);
            a.analyze(res.value());
            CHECK(!a.has_errors());
            ASTNode* list = find_child(res.value(), NodeType::SelectList);
            ASTNode* vid = first_child(list);
            CHECK(vid != nullptr && a.type_of(vid) == DataType::Integer);       // id <- 1
            CHECK(vid != nullptr && a.type_of(vid->next_sibling) == DataType::Text);  // label <- 'eng'
        }
    }
    // An unaliased column of a VALUES derived table has no name to resolve.
    {
        auto res = p.parse("SELECT v.nope FROM (VALUES (1)) AS v(x)");
        CHECK(res.has_value());
        if (res) {
            Analyzer a(cat);
            a.analyze(res.value());
            CHECK(count_code(a, DiagnosticCode::UnresolvedColumn) == 1);
        }
    }
    // Ragged VALUES rows (a later row narrower or wider than the first, which
    // sets the width) make a malformed relation and must be flagged.
    {
        auto res = p.parse("SELECT * FROM (VALUES (1, 2), (3)) t");
        CHECK(res.has_value());
        if (res) {
            Analyzer a(cat);
            a.analyze(res.value());
            CHECK(count_code(a, DiagnosticCode::ValuesRowArityMismatch) == 1);
        }
    }
    {
        auto res = p.parse("SELECT * FROM (VALUES (1, 2), (3, 4, 5)) t");
        CHECK(res.has_value());
        if (res) {
            Analyzer a(cat);
            a.analyze(res.value());
            CHECK(count_code(a, DiagnosticCode::ValuesRowArityMismatch) == 1);
        }
    }
    // Uniform VALUES rows stay clean (regression guard for the new check).
    {
        auto res = p.parse("SELECT * FROM (VALUES (1, 2), (3, 4)) t");
        CHECK(res.has_value());
        if (res) {
            Analyzer a(cat);
            a.analyze(res.value());
            CHECK(count_code(a, DiagnosticCode::ValuesRowArityMismatch) == 0);
        }
    }
    // A multi-row VALUES is a UNION ALL of its rows, so each column's type is
    // reconciled across every row (not taken from the first row only). A widening
    // mix (int + double) is legal and reconciles cleanly; an incompatible mix
    // (int + text, either order) is flagged. Regression: the later-rows loop only
    // checked arity, so both were silently accepted.
    {
        auto res = p.parse("SELECT a FROM (VALUES (1), (2.5)) AS t(a)");  // numeric widen: legal
        CHECK(res.has_value());
        if (res) {
            Analyzer a(cat);
            a.analyze(res.value());
            CHECK(count_code(a, DiagnosticCode::ValuesColumnTypeMismatch) == 0);
        }
    }
    {
        auto res = p.parse("SELECT a FROM (VALUES (1), ('x')) AS t(a)");  // int then text: illegal
        CHECK(res.has_value());
        if (res) {
            Analyzer a(cat);
            a.analyze(res.value());
            CHECK(count_code(a, DiagnosticCode::ValuesColumnTypeMismatch) == 1);
        }
    }
    {
        auto res = p.parse("SELECT a FROM (VALUES ('x'), (1)) AS t(a)");  // text then int: illegal
        CHECK(res.has_value());
        if (res) {
            Analyzer a(cat);
            a.analyze(res.value());
            CHECK(count_code(a, DiagnosticCode::ValuesColumnTypeMismatch) == 1);
        }
    }
    {
        // Per-column reconciliation: an int column beside a text column is fine.
        auto res = p.parse("SELECT a, b FROM (VALUES (1, 'x'), (2, 'y')) AS t(a, b)");
        CHECK(res.has_value());
        if (res) {
            Analyzer a(cat);
            a.analyze(res.value());
            CHECK(count_code(a, DiagnosticCode::ValuesColumnTypeMismatch) == 0);
        }
    }
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

// A CTE whose body is a set operation (UNION/INTERSECT/EXCEPT) must register its
// columns so the outer query can resolve them. Previously the body lookup only
// matched a SelectStmt, so a set-op body registered the CTE with no columns and
// the outer reference went unresolved.
void test_cte_setop_body() {
    std::printf("test_cte_setop_body\n");
    auto cat = make_catalog();  // users(id INTEGER NOT NULL, name TEXT)
    parser::Parser p;
    auto res = p.parse(
        "WITH t AS (SELECT id FROM users UNION SELECT id FROM users) "
        "SELECT id FROM t WHERE id > 0");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());
    CHECK(count_code(a, DiagnosticCode::UnresolvedColumn) == 0);
    CHECK(count_code(a, DiagnosticCode::UnresolvedTable) == 0);
    // The outer `id` resolves to the CTE's column (Integer, from users.id).
    ASTNode* list = find_child(res.value(), NodeType::SelectList);
    ASTNode* id = list != nullptr ? first_child(list) : nullptr;
    CHECK(id != nullptr && a.type_of(id) == DataType::Integer);
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

// A parenthesized join group `( a JOIN b ) JOIN c` brings every relation of the
// group into scope, so references across the group and the outer join resolve.
void test_parenthesized_join_group_resolves() {
    std::printf("test_parenthesized_join_group_resolves\n");
    auto cat = make_catalog_joins();
    parser::Parser p;
    auto res = p.parse(
        "SELECT u.id FROM (users u JOIN orders o ON o.user_id = u.id) "
        "JOIN sessions s ON s.user_id = o.user_id");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());
    CHECK(count_code(a, DiagnosticCode::UnresolvedColumn) == 0);
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

// A GROUP BY key that IS (or contains) a non-windowed aggregate is illegal:
// grouping produces aggregates, so an aggregate cannot be a grouping key
// (Postgres: "aggregate functions are not allowed in GROUP BY").
void test_groupby_aggregate_key_rejected() {
    std::printf("test_groupby_aggregate_key_rejected\n");
    auto cat = make_catalog_emp();
    parser::Parser p;

    // Direct aggregate key alongside a valid one: only the aggregate key flags.
    auto res = p.parse("SELECT dept, SUM(salary) FROM emp GROUP BY dept, MAX(age)");
    CHECK(res.has_value());
    if (res) {
        Analyzer a(cat);
        a.analyze(res.value());
        CHECK(count_code(a, DiagnosticCode::AggregateInGroupBy) == 1);
    }

    // An aggregate as the sole key is still flagged (the non-grouped `dept` is a
    // separate diagnostic; we assert the aggregate-key one is present).
    auto res2 = p.parse("SELECT dept FROM emp GROUP BY COUNT(*)");
    CHECK(res2.has_value());
    if (res2) {
        Analyzer a(cat);
        a.analyze(res2.value());
        CHECK(count_code(a, DiagnosticCode::AggregateInGroupBy) == 1);
    }

    // A plain column key and a positional key are NOT aggregates: no diagnostic.
    auto ok = p.parse("SELECT dept, COUNT(*) FROM emp GROUP BY dept");
    CHECK(ok.has_value());
    if (ok) {
        Analyzer a(cat);
        a.analyze(ok.value());
        CHECK(count_code(a, DiagnosticCode::AggregateInGroupBy) == 0);
    }
    auto ok2 = p.parse("SELECT dept, COUNT(*) FROM emp GROUP BY 1");
    CHECK(ok2.has_value());
    if (ok2) {
        Analyzer a(cat);
        a.analyze(ok2.value());
        CHECK(count_code(a, DiagnosticCode::AggregateInGroupBy) == 0);
    }
}

void test_groupby_self_join_distinct_instances() {
    std::printf("test_groupby_self_join_distinct_instances\n");
    // A self-join binds both aliases to the SAME catalog table, so `a.id` and
    // `b.id` share (table_id, column_id). A GROUP BY key on one alias must NOT be
    // treated as covering the same column of the OTHER alias - each self-join
    // correlation is a distinct relation instance (Postgres rejects these).
    // Regression: grouping identity compared only (table_id, column_id), so the
    // other alias's column silently satisfied the key.
    InMemoryCatalog cat;
    cat.add_table("users", {ColumnInfo{"id", DataType::Integer, /*nullable=*/false},
                            ColumnInfo{"name", DataType::Text, /*nullable=*/true}});
    parser::Parser p;

    auto flags = [&](const char* sql) -> int {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return -1;
        Analyzer a(cat);
        a.analyze(res.value());
        return count_code(a, DiagnosticCode::NonGroupedColumn);
    };

    // Key on one alias, projected column on the OTHER alias -> non-grouped.
    CHECK(flags("SELECT a.id, b.id FROM users a JOIN users b ON a.id > b.id "
                "GROUP BY a.id") == 1);
    CHECK(flags("SELECT a.name FROM users a JOIN users b ON a.id = b.id "
                "GROUP BY b.name") == 1);
    // The recent grouping-trigger widening also reaches HAVING.
    CHECK(flags("SELECT a.id FROM users a JOIN users b ON a.id = b.id "
                "GROUP BY a.id HAVING b.id > 0") == 1);

    // Grouping BOTH instances, or referencing the grouped instance, is clean.
    CHECK(flags("SELECT a.id, b.id FROM users a JOIN users b ON a.id = b.id "
                "GROUP BY a.id, b.id") == 0);
    CHECK(flags("SELECT b.name FROM users a JOIN users b ON a.id = b.id "
                "GROUP BY b.name") == 0);

    // Non-self-join behaviour is unchanged: a bare key still covers a qualified
    // reference to the single relation, and a genuinely non-grouped column flags.
    CHECK(flags("SELECT users.id FROM users GROUP BY id") == 0);
    CHECK(flags("SELECT name FROM users GROUP BY id") == 1);
}

void test_groupby_derived_same_base_alias() {
    std::printf("test_groupby_derived_same_base_alias\n");
    // A derived table whose body projects ONE base column under TWO names -
    // `(SELECT id AS a, id AS b FROM users) t` - gives t.a and t.b the SAME
    // (table_id, column_id): a derived column carries through the identity of the
    // expression it projects. GROUP BY t.a therefore must NOT be treated as
    // covering t.b (Postgres rejects the second - they are distinct output
    // columns). Regression: grouping identity compared only (table_id,
    // column_id) + relation instance, so a same-base derived alias silently
    // satisfied the key. The referenced column NAME must also agree.
    InMemoryCatalog cat;
    cat.add_table("users", {ColumnInfo{"id", DataType::Integer, /*nullable=*/false},
                            ColumnInfo{"name", DataType::Text, /*nullable=*/true}});
    parser::Parser p;

    auto flags = [&](const char* sql) -> int {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return -1;
        Analyzer a(cat);
        a.analyze(res.value());
        return count_code(a, DiagnosticCode::NonGroupedColumn);
    };

    // Two aliases of the same base column; grouping only one leaves the other
    // non-grouped - unqualified and qualified reference forms alike.
    CHECK(flags("SELECT a, b FROM (SELECT id AS a, id AS b FROM users) t "
                "GROUP BY a") == 1);
    CHECK(flags("SELECT t.a, t.b FROM (SELECT id AS a, id AS b FROM users) t "
                "GROUP BY t.a") == 1);
    // The non-grouped same-base alias is caught in HAVING and ORDER BY too.
    CHECK(flags("SELECT a FROM (SELECT id AS a, id AS b FROM users) t "
                "GROUP BY a HAVING b > 0") == 1);
    CHECK(flags("SELECT a FROM (SELECT id AS a, id AS b FROM users) t "
                "GROUP BY a ORDER BY b") == 1);

    // Grouping BOTH aliases, or referencing only the grouped alias, is clean -
    // no false positive from the added name check.
    CHECK(flags("SELECT a, b FROM (SELECT id AS a, id AS b FROM users) t "
                "GROUP BY a, b") == 0);
    CHECK(flags("SELECT a FROM (SELECT id AS a, id AS b FROM users) t "
                "GROUP BY a") == 0);
    // A derived table that simply passes a base column through keeps working:
    // grouping the (single) derived column covers a reference to it.
    CHECK(flags("SELECT a FROM (SELECT id AS a FROM users) t GROUP BY a") == 0);
    // Positional and alias-form keys resolve to the derived item's identity, so
    // GROUP BY 1 covers a but not the same-base b.
    CHECK(flags("SELECT a, b FROM (SELECT id AS a, id AS b FROM users) t "
                "GROUP BY 1") == 1);
}

void test_orderby_qualified_ref_resolves_to_base_column() {
    std::printf("test_orderby_qualified_ref_resolves_to_base_column\n");
    // A QUALIFIED ORDER BY reference names a base/input column, never a SELECT
    // output alias. Regression: step-7 matched a qualified order-by ref to an
    // output column by BARE name and skipped FROM-scope resolution, so
    // (a) `GROUP BY dept ... ORDER BY emp.dept` left the ref unresolved and the
    //     grouping check falsely raised NonGroupedColumn on a legal query, and
    // (b) `SELECT salary AS id ... ORDER BY e.id` typed the ref from the output
    //     column (Double) instead of base emp.id (Integer).
    auto cat = make_catalog_emp();  // emp(id INT NOT NULL, ..., salary DOUBLE, ...)
    parser::Parser p;

    auto ng = [&](const char* sql) -> int {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return -1;
        Analyzer a(cat);
        a.analyze(res.value());
        return count_code(a, DiagnosticCode::NonGroupedColumn);
    };

    // (a) A qualified ORDER BY of the grouped column is legal - no false error.
    CHECK(ng("SELECT dept, SUM(salary) FROM emp GROUP BY dept ORDER BY emp.dept") == 0);
    CHECK(ng("SELECT dept, SUM(salary) FROM emp e GROUP BY dept ORDER BY e.dept") == 0);
    // An UNQUALIFIED output alias in ORDER BY is still exempt (unchanged).
    CHECK(ng("SELECT dept, COUNT(*) AS c FROM emp GROUP BY dept ORDER BY c") == 0);
    // A qualified ORDER BY of a genuinely non-grouped column is still illegal.
    CHECK(ng("SELECT dept, SUM(salary) FROM emp GROUP BY dept ORDER BY emp.salary") == 1);

    // (b) `e.id` must resolve to base emp.id (Integer, NOT NULL), not the output
    // alias `id` (which renames salary, Double).
    {
        auto res = p.parse("SELECT salary AS id FROM emp e ORDER BY e.id");
        CHECK(res.has_value());
        if (res) {
            Analyzer a(cat);
            a.analyze(res.value());
            ASTNode* ob = find_child(res.value(), NodeType::OrderByClause);
            ASTNode* item = ob ? first_child(ob) : nullptr;
            CHECK(item != nullptr && a.type_of(item) == DataType::Integer);
            CHECK(item != nullptr && a.nullability_of(item) == 1);  // emp.id is NOT NULL
        }
    }
}

void test_groupby_aggregate_in_having_or_orderby_groups() {
    std::printf("test_groupby_aggregate_in_having_or_orderby_groups\n");
    // An aggregate confined to HAVING or ORDER BY (or the mere presence of
    // HAVING) makes the query grouped even without GROUP BY, so a bare
    // non-grouped column in the SELECT list is illegal - matching Postgres.
    // Regression: the `grouped` trigger only looked at GROUP BY and the SELECT
    // list, so these were silently accepted.
    auto cat = make_catalog_emp();
    struct Case { const char* sql; int expect_non_grouped; };
    const Case cases[] = {
        {"SELECT id FROM emp HAVING COUNT(*) > 0", 1},      // HAVING groups; id illegal
        {"SELECT id FROM emp ORDER BY COUNT(*)", 1},        // aggregate in ORDER BY groups
        {"SELECT dept FROM emp HAVING COUNT(*) > 0", 1},    // dept illegal too
        // Legal controls: must NOT be flagged.
        {"SELECT COUNT(*) FROM emp HAVING COUNT(*) > 0", 0},          // only aggregate projected
        {"SELECT dept FROM emp GROUP BY dept HAVING COUNT(*) > 0", 0},// dept is a group key
        {"SELECT id FROM emp ORDER BY id", 0},              // not grouped at all
        {"SELECT id FROM emp", 0},                          // plain select
    };
    for (const auto& c : cases) {
        parser::Parser p;
        auto res = p.parse(c.sql);
        CHECK(res.has_value());
        if (!res) continue;
        Analyzer a(cat);
        a.analyze(res.value());
        CHECK(count_code(a, DiagnosticCode::NonGroupedColumn) == c.expect_non_grouped);
    }
}

// GUARDRAIL MATRIX: the "query is grouped" trigger. Passes have adjusted what
// forces grouping (SELECT-list aggregate, then HAVING presence + ORDER-BY
// aggregate); this table pins the full trigger truth-set so a regression that
// drops or over-adds a trigger fails loudly. Column is the expected count of
// NonGroupedColumn diagnostics.
void test_grouping_trigger_matrix() {
    std::printf("test_grouping_trigger_matrix\n");
    auto cat = make_catalog_emp();
    struct Case { const char* sql; int non_grouped; };
    const Case cases[] = {
        // grouped -> a bare non-key column is flagged
        {"SELECT id FROM emp GROUP BY dept", 1},
        {"SELECT id FROM emp HAVING COUNT(*) > 0", 1},
        {"SELECT id FROM emp ORDER BY COUNT(*)", 1},
        {"SELECT id, COUNT(*) FROM emp", 1},
        // NOT grouped, or grouped-and-legal -> nothing flagged
        {"SELECT COUNT(*) FROM emp HAVING COUNT(*) > 0", 0},
        {"SELECT dept FROM emp GROUP BY dept HAVING COUNT(*) > 0", 0},
        {"SELECT dept, COUNT(*) FROM emp GROUP BY dept", 0},
        {"SELECT id FROM emp ORDER BY id", 0},
        {"SELECT id FROM emp", 0},
        // a WINDOWED aggregate does not collapse rows -> not grouped
        {"SELECT id, ROW_NUMBER() OVER (ORDER BY id) FROM emp", 0},
        // an aggregate inside a SUBQUERY does not group the OUTER query
        {"SELECT id FROM emp WHERE id IN (SELECT COUNT(*) FROM emp)", 0},
    };
    for (const auto& c : cases) {
        parser::Parser p;
        auto res = p.parse(c.sql);
        CHECK(res.has_value());
        if (!res) continue;
        Analyzer a(cat);
        a.analyze(res.value());
        CHECK(count_code(a, DiagnosticCode::NonGroupedColumn) == c.non_grouped);
    }
}

// GUARDRAIL MATRIX: multi-row VALUES column typing. A VALUES derived table is a
// UNION ALL of its rows; each column's type reconciles across all rows (arity
// #66, then types #69). This pins the reconciliation truth-set: numeric widening
// is legal, an incompatible type mix is flagged once per bad column (no cascade),
// NULL unifies, and a ragged row still triggers the arity check.
void test_values_type_reconciliation_matrix() {
    std::printf("test_values_type_reconciliation_matrix\n");
    auto cat = make_catalog_emp();
    struct Case { const char* sql; int type_mismatch; int arity_mismatch; };
    const Case cases[] = {
        // legal reconciliations
        {"SELECT a FROM (VALUES (1),(2),(3)) AS t(a)",            0, 0},
        {"SELECT a FROM (VALUES (1),(2.5)) AS t(a)",             0, 0},  // int+double widen
        {"SELECT a FROM (VALUES (2.5),(1)) AS t(a)",             0, 0},  // double+int widen
        {"SELECT a FROM (VALUES (1),(2.0),(3)) AS t(a)",         0, 0},  // 3-row widen
        {"SELECT a FROM (VALUES (1),(NULL)) AS t(a)",            0, 0},  // NULL unifies
        {"SELECT a FROM (VALUES (NULL),(1)) AS t(a)",            0, 0},
        {"SELECT a,b FROM (VALUES (1,'x'),(2,'y')) AS t(a,b)",   0, 0},  // per-column
        {"SELECT a FROM (VALUES (1)) AS t(a)",                   0, 0},  // single row
        // incompatible type mixes (flagged once per bad column, no cascade)
        {"SELECT a FROM (VALUES (1),('x')) AS t(a)",             1, 0},
        {"SELECT a FROM (VALUES ('x'),(1)) AS t(a)",             1, 0},
        {"SELECT a FROM (VALUES (1),(2),('x')) AS t(a)",         1, 0},
        {"SELECT a FROM (VALUES (1),('x'),(2)) AS t(a)",         1, 0},  // no cascade after row2
        {"SELECT a FROM (VALUES (1),('x'),('y')) AS t(a)",       1, 0},  // still one per column
        {"SELECT a FROM (VALUES (1),(2.5),('x')) AS t(a)",       1, 0},
        {"SELECT a,b FROM (VALUES (1,'x'),(2,3)) AS t(a,b)",     1, 0},  // only col b bad
        {"SELECT a,b FROM (VALUES (1,'x'),('y',2)) AS t(a,b)",   2, 0},  // both cols bad
        // ragged arity is independent of typing
        {"SELECT a FROM (VALUES (1,2),(3)) AS t(a,b)",           -1, 1},
        {"SELECT a FROM (VALUES (1),(2,3)) AS t(a)",             -1, 1},
    };
    for (const auto& c : cases) {
        parser::Parser p;
        auto res = p.parse(c.sql);
        CHECK(res.has_value());
        if (!res) continue;
        Analyzer a(cat);
        a.analyze(res.value());
        if (c.type_mismatch >= 0) {
            CHECK(count_code(a, DiagnosticCode::ValuesColumnTypeMismatch) == c.type_mismatch);
        }
        if (c.arity_mismatch >= 0) {
            CHECK(count_code(a, DiagnosticCode::ValuesRowArityMismatch) == c.arity_mismatch);
        }
    }
}

void test_groupby_output_alias_key() {
    std::printf("test_groupby_output_alias_key\n");
    auto cat = make_catalog_emp();
    parser::Parser p;

    // A GROUP BY key may name a SELECT-list output alias (Postgres extension):
    // the query groups by the aliased expression. These are all legal - no
    // unresolved-column error and no non-grouped-column error.
    auto clean = [&](const char* sql) {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return;
        Analyzer a(cat);
        a.analyze(res.value());
        CHECK(count_code(a, DiagnosticCode::UnresolvedColumn) == 0);
        CHECK(count_code(a, DiagnosticCode::NonGroupedColumn) == 0);
    };
    clean("SELECT id AS x FROM emp GROUP BY x");                 // single-column alias
    clean("SELECT dept AS d, COUNT(*) FROM emp GROUP BY d");     // alias + aggregate
    clean("SELECT salary + 1 AS s FROM emp GROUP BY s");         // compound-expression alias

    // A non-grouped sibling is still flagged (the alias groups only its own
    // expression), and the alias itself resolves (no unresolved error).
    {
        auto res = p.parse("SELECT id AS x, dept FROM emp GROUP BY x");
        CHECK(res.has_value());
        if (res) {
            Analyzer a(cat);
            a.analyze(res.value());
            CHECK(count_code(a, DiagnosticCode::UnresolvedColumn) == 0);
            CHECK(count_code(a, DiagnosticCode::NonGroupedColumn) == 1);  // dept
        }
    }

    // Input-column precedence: `region` is both a base column and the alias for
    // dept here. A GROUP BY name binds to the INPUT column first (Postgres), so
    // the query groups by the base column region, leaving dept non-grouped.
    {
        auto res = p.parse("SELECT dept AS region, COUNT(*) FROM emp GROUP BY region");
        CHECK(res.has_value());
        if (res) {
            Analyzer a(cat);
            a.analyze(res.value());
            CHECK(count_code(a, DiagnosticCode::UnresolvedColumn) == 0);
            CHECK(count_code(a, DiagnosticCode::NonGroupedColumn) == 1);  // dept
        }
    }
}

// The GROUP BY output-alias extension must not swallow two illegal shapes:
//   (a) an AMBIGUOUS key silently reinterpreted as an alias (input precedence
//       is decided on found OR ambiguous, not found alone), and
//   (b) an alias that names an aggregate expression (illegal in GROUP BY).
void test_groupby_alias_ambiguity_and_aggregate() {
    std::printf("test_groupby_alias_ambiguity_and_aggregate\n");
    InMemoryCatalog cat;
    cat.add_table("emp", { ColumnInfo{"id", DataType::Integer, false},
                           ColumnInfo{"dept", DataType::Text, true} });
    cat.add_table("t2",  { ColumnInfo{"id", DataType::Integer, false},
                           ColumnInfo{"v", DataType::Text, true} });
    parser::Parser p;

    // (a) `id` is exposed by both emp and t2 -> ambiguous. Even though an output
    // alias `id` exists, GROUP BY resolves input-first, so the ambiguity must be
    // reported (previously the alias path suppressed it, silently changing the
    // grouping to `dept`).
    {
        auto res = p.parse("SELECT emp.dept AS id FROM emp JOIN t2 ON emp.id = t2.id "
                           "GROUP BY id");
        CHECK(res.has_value());
        if (res) {
            Analyzer a(cat);
            a.analyze(res.value());
            CHECK(count_code(a, DiagnosticCode::AmbiguousColumn) == 1);
        }
    }
    // (b) An alias naming an aggregate is not a valid grouping key. The query
    // must be rejected (not silently accepted with zero diagnostics).
    {
        auto res = p.parse("SELECT COUNT(*) AS c FROM emp GROUP BY c");
        CHECK(res.has_value());
        if (res) {
            Analyzer a(cat);
            a.analyze(res.value());
            CHECK(a.has_errors());
        }
    }
    // Control: an unambiguous non-aggregate alias still groups cleanly.
    {
        auto res = p.parse("SELECT dept AS d, COUNT(*) FROM emp GROUP BY d");
        CHECK(res.has_value());
        if (res) {
            Analyzer a(cat);
            a.analyze(res.value());
            CHECK(!a.has_errors());
        }
    }
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

// An ORDER BY item that references a SELECT output column by its alias is a
// reference to the (already grouped) projection, so it is legal in a grouped
// query - the idiomatic "ORDER BY <aggregate-alias>" / "top-N" pattern. Both
// an aggregate alias and a grouped-column alias must be accepted.
void test_order_by_output_alias_in_grouped_clean() {
    std::printf("test_order_by_output_alias_in_grouped_clean\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    const char* cases[] = {
        "SELECT dept, COUNT(*) AS c FROM emp GROUP BY dept ORDER BY c",
        "SELECT dept AS d FROM emp GROUP BY dept ORDER BY d",
        "SELECT COUNT(*) AS c FROM emp ORDER BY c",  // aggregate-only (implicitly grouped)
    };
    for (const char* sql : cases) {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) continue;
        Analyzer a(cat);
        a.analyze(res.value());
        CHECK(count_code(a, DiagnosticCode::NonGroupedColumn) == 0);
    }

    // The exemption is only for a BARE output-name reference. A qualified
    // reference names a base column and must still obey the grouping rule, even
    // when its column name collides with an output alias.
    auto q = p.parse("SELECT dept, MAX(age) AS salary FROM emp GROUP BY dept "
                     "ORDER BY emp.salary");
    CHECK(q.has_value());
    if (q) {
        Analyzer a(cat);
        a.analyze(q.value());
        CHECK(count_code(a, DiagnosticCode::NonGroupedColumn) == 1);
    }
}

// An aggregate inside a window function's OVER clause is legal (it is a grouped
// aggregate the window is computed over) and must NOT be reported as a nested
// aggregate; a genuine aggregate-inside-aggregate must still be flagged.
void test_aggregate_in_over_clause_not_nested() {
    std::printf("test_aggregate_in_over_clause_not_nested\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    const char* clean[] = {
        "SELECT RANK() OVER (ORDER BY SUM(salary)) FROM emp",
        "SELECT dept, SUM(salary) OVER (PARTITION BY dept ORDER BY COUNT(*)) "
        "FROM emp GROUP BY dept",
    };
    for (const char* sql : clean) {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) continue;
        Analyzer a(cat);
        a.analyze(res.value());
        CHECK(count_code(a, DiagnosticCode::NestedAggregate) == 0);
    }
    // Genuine nesting (no window boundary) is still an error.
    auto res = p.parse("SELECT SUM(COUNT(*)) FROM emp");
    CHECK(res.has_value());
    if (res) {
        Analyzer a(cat);
        a.analyze(res.value());
        CHECK(count_code(a, DiagnosticCode::NestedAggregate) == 1);
    }
}

// A very long operator chain parses to a deeply left-nested tree that the parser
// does not depth-bound. The analyzer must not overflow the stack on it: past the
// recursion limit it abandons the over-deep subtree and reports it once.
// Resolution over a wide relation goes through the hashed name index. Verify it
// resolves the right columns (type + nullability), expands SELECT *, flags a
// missing column, and detects ambiguity on a self-join - exactly as the linear
// path does.
void test_wide_relation_resolution() {
    std::printf("test_wide_relation_resolution\n");
    auto cat = make_catalog_wide();
    parser::Parser p;

    // Bare refs across the width resolve with the right nullability (even = nullable).
    { auto r = p.parse("SELECT w0, w7, w23 FROM wide");
      CHECK(r.has_value());
      if (r) { Analyzer a(cat); a.analyze(r.value());
               CHECK(count_code(a, DiagnosticCode::UnresolvedColumn) == 0);
               CHECK(a.diagnostics().empty()); } }

    // A missing column is still flagged (index miss == linear miss).
    { auto r = p.parse("SELECT w99 FROM wide");
      CHECK(r.has_value());
      if (r) { Analyzer a(cat); a.analyze(r.value());
               CHECK(count_code(a, DiagnosticCode::UnresolvedColumn) == 1); } }

    // SELECT * over the wide table expands to all 24 columns.
    { auto r = p.parse("SELECT * FROM wide");
      CHECK(r.has_value());
      if (r) { Analyzer a(cat); a.analyze(r.value());
               const auto* proj = a.projection_of(r.value());
               CHECK(proj != nullptr);
               if (proj) CHECK(proj->size() == 24); } }

    // A bare ref common to both sides of a self-join is ambiguous (the index must
    // not mask the second relation's match).
    { auto r = p.parse("SELECT w5 FROM wide a JOIN wide b ON a.w0 = b.w0");
      CHECK(r.has_value());
      if (r) { Analyzer a(cat); a.analyze(r.value());
               CHECK(count_code(a, DiagnosticCode::AmbiguousColumn) == 1); } }

    // Qualified refs resolve to the intended side of the self-join.
    { auto r = p.parse("SELECT a.w5, b.w6 FROM wide a JOIN wide b ON a.w0 = b.w0");
      CHECK(r.has_value());
      if (r) { Analyzer a(cat); a.analyze(r.value());
               CHECK(count_code(a, DiagnosticCode::UnresolvedColumn) == 0);
               CHECK(count_code(a, DiagnosticCode::AmbiguousColumn) == 0); } }
}

void test_deep_expression_does_not_crash() {
    std::printf("test_deep_expression_does_not_crash\n");
    auto cat = make_catalog();  // users(id, name)
    parser::Parser p;
    std::string sql = "SELECT * FROM users WHERE id > 0";
    for (int i = 0; i < 5000; ++i) {
        sql += " AND id > 0";
    }
    auto res = p.parse(sql);
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());  // must return, not overflow the stack
    CHECK(count_code(a, DiagnosticCode::ExpressionTooComplex) == 1);
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

void test_greatest_least_nullability() {
    std::printf("test_greatest_least_nullability\n");
    // GREATEST / LEAST skip NULL arguments (Postgres), so the result is NOT NULL
    // iff any argument is NOT NULL - the same rule as COALESCE, not the default
    // "nullable if any argument is nullable". Regression: they fell through to the
    // generic combine_nullable_any and were over-reported nullable.
    auto cat = make_catalog_null();  // users(id NOT NULL, note nullable)
    parser::Parser p;
    auto res = p.parse(
        "SELECT GREATEST(id, note), LEAST(id, note), GREATEST(note, note) FROM users");
    CHECK(res.has_value());
    if (!res) return;
    Analyzer a(cat);
    a.analyze(res.value());
    ASTNode* list = find_child(res.value(), NodeType::SelectList);
    ASTNode* g = list ? first_child(list) : nullptr;
    ASTNode* l = g ? g->next_sibling : nullptr;
    ASTNode* gn = l ? l->next_sibling : nullptr;
    CHECK(g != nullptr && a.nullability_of(g) == 1);   // GREATEST(NN, nullable) -> NOT NULL
    CHECK(l != nullptr && a.nullability_of(l) == 1);    // LEAST(NN, nullable) -> NOT NULL
    CHECK(gn != nullptr && a.nullability_of(gn) == 2);  // GREATEST(nullable,nullable) -> nullable
}

void test_coalesce_greatest_least_type_reconciliation() {
    std::printf("test_coalesce_greatest_least_type_reconciliation\n");
    // COALESCE / GREATEST / LEAST reconcile their argument types with the same
    // UnionReconcile rule as a set operation / VALUES / CASE. Incompatible argument
    // types (text vs integer - which PostgreSQL rejects) must raise a TypeMismatch,
    // not be silently accepted; and with 3+ arguments the fold must NOT re-establish
    // a concrete type that masks the mismatch (regression: `COALESCE(name,id,age)`
    // returned Integer with zero diagnostics).
    InMemoryCatalog cat;
    cat.add_table("users", {ColumnInfo{"id", DataType::Integer, /*nullable=*/false},
                            ColumnInfo{"name", DataType::Text, /*nullable=*/true},
                            ColumnInfo{"age", DataType::Integer, /*nullable=*/true}});
    parser::Parser p;

    // Helper: analyze `SELECT <expr> FROM users`, return the projected item.
    auto proj = [&](Analyzer& a, const parser::ParseResult& res) -> ASTNode* {
        a.analyze(res.value());
        ASTNode* list = find_child(res.value(), NodeType::SelectList);
        return list ? first_child(list) : nullptr;
    };

    // --- incompatible (text vs integer): flagged, degraded to Unknown ---
    struct Bad { const char* sql; };
    const Bad bad[] = {
        {"SELECT COALESCE(id, name) FROM users"},
        {"SELECT GREATEST(id, name) FROM users"},
        {"SELECT LEAST(id, name) FROM users"},
        {"SELECT COALESCE(name, id, age) FROM users"},   // 3-arg: must not mask
        {"SELECT GREATEST(id, name, age) FROM users"},    // mismatch in the middle
    };
    for (const Bad& b : bad) {
        auto res = p.parse(b.sql);
        CHECK(res.has_value());
        if (!res) continue;
        Analyzer a(cat);
        ASTNode* item = proj(a, res);
        CHECK(item != nullptr);
        CHECK(count_code(a, DiagnosticCode::TypeMismatch) == 1);  // b.sql
        CHECK(item != nullptr && a.type_of(item) == DataType::Unknown);
    }

    // --- compatible: no diagnostic, correct reconciled type ---
    struct Good { const char* sql; DataType type; };
    const Good good[] = {
        {"SELECT COALESCE(id, age) FROM users", DataType::Integer},   // int + int
        {"SELECT GREATEST(id, age) FROM users", DataType::Integer},
        {"SELECT COALESCE(name, name) FROM users", DataType::Text},   // text + text
        {"SELECT COALESCE(id, age, id) FROM users", DataType::Integer},  // 3-arg clean
    };
    for (const Good& g : good) {
        auto res = p.parse(g.sql);
        CHECK(res.has_value());
        if (!res) continue;
        Analyzer a(cat);
        ASTNode* item = proj(a, res);
        CHECK(item != nullptr);
        CHECK(count_code(a, DiagnosticCode::TypeMismatch) == 0);  // g.sql
        CHECK(item != nullptr && a.type_of(item) == g.type);
    }
}

void test_setop_derived_table_columns() {
    std::printf("test_setop_derived_table_columns\n");
    // A derived table whose body is a set operation registers its reconciled
    // columns (named/typed from the first branch), so references to them resolve -
    // the derived-table sibling of the set-op CTE fix. Regression: the body was
    // matched only as a direct SelectStmt, so a set-op body registered ZERO
    // columns and every reference was falsely UnresolvedColumn.
    InMemoryCatalog cat;
    cat.add_table("users", {ColumnInfo{"id", DataType::Integer, /*nullable=*/false}});
    cat.add_table("orders", {ColumnInfo{"oid", DataType::Integer, /*nullable=*/false}});
    parser::Parser p;
    const char* sqls[] = {
        "SELECT id FROM (SELECT id FROM users UNION SELECT oid FROM orders) AS t",
        "SELECT id FROM (SELECT id FROM users INTERSECT SELECT oid FROM orders) AS t",
        "SELECT id FROM (SELECT id FROM users EXCEPT SELECT oid FROM orders) AS t",
        "SELECT a FROM (SELECT id AS a FROM users UNION SELECT oid FROM orders) AS t(a)",
    };
    for (const char* sql : sqls) {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) continue;
        Analyzer a(cat);
        a.analyze(res.value());
        CHECK(count_code(a, DiagnosticCode::UnresolvedColumn) == 0);
        CHECK(!a.has_errors());
    }
}

void test_in_expr_nullability() {
    std::printf("test_in_expr_nullability\n");
    auto cat = make_catalog_null();  // users(id NOT NULL, note nullable)
    parser::Parser p;
    // Under 3-valued logic `x IN (...)` is NULL when no element equals x but x or
    // some element is NULL, so the result is nullable if the left operand OR any
    // list element / subquery column is nullable - not just when the left is.
    auto in_null = [&](const char* sql) -> int {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return -1;
        Analyzer a(cat);
        a.analyze(res.value());
        ASTNode* in = find_descendant(res.value(), NodeType::InExpr);
        CHECK(in != nullptr);
        return in != nullptr ? a.nullability_of(in) : -1;
    };
    // NOT NULL left, but a NULL in the list -> nullable (regression: was 1).
    CHECK(in_null("SELECT id IN (1, NULL) FROM users") == 2);
    // NOT NULL left and an all-not-null list -> not-null (unchanged).
    CHECK(in_null("SELECT id IN (1, 2) FROM users") == 1);
    // NOT IN has the same nullability.
    CHECK(in_null("SELECT id NOT IN (1, NULL) FROM users") == 2);
    // A nullable list element (the column `note`) makes it nullable.
    CHECK(in_null("SELECT id IN (note) FROM users") == 2);
    // Subquery RHS: a nullable projected column -> nullable; a not-null column
    // with a not-null left -> not-null.
    CHECK(in_null("SELECT id IN (SELECT note FROM users) FROM users") == 2);
    CHECK(in_null("SELECT id IN (SELECT uid FROM orders) FROM users") == 1);
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

void test_integer_literal_width_by_magnitude() {
    std::printf("test_integer_literal_width_by_magnitude\n");
    // An integer literal is typed by its MAGNITUDE (PostgreSQL int4 -> int8 ->
    // numeric): Integer within signed 32-bit, BigInt within signed 64-bit, else
    // Decimal. Regression: every integer literal was typed Integer, silently
    // narrowing a bigint constant to 32 bits - and that wrong type propagated
    // through set-op / VALUES column reconciliation.
    InMemoryCatalog cat;
    cat.add_table("users", {ColumnInfo{"id", DataType::Integer, /*nullable=*/false}});
    parser::Parser p;

    auto proj_type = [&](const char* sql) -> DataType {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return DataType::Unknown;
        Analyzer a(cat);
        a.analyze(res.value());
        ASTNode* list = find_child(res.value(), NodeType::SelectList);
        ASTNode* item = list ? first_child(list) : nullptr;
        return item ? a.type_of(item) : DataType::Unknown;
    };

    CHECK(proj_type("SELECT 5") == DataType::Integer);
    CHECK(proj_type("SELECT 2147483647") == DataType::Integer);            // int32 max
    CHECK(proj_type("SELECT 2147483648") == DataType::BigInt);            // int32 max + 1
    CHECK(proj_type("SELECT 9223372036854775807") == DataType::BigInt);  // int64 max
    CHECK(proj_type("SELECT 9223372036854775808") == DataType::Decimal); // int64 max + 1
    CHECK(proj_type("SELECT 99999999999999999999999999") == DataType::Decimal);  // > uint64

    // Negative literals: the parser folds the sign into the IntegerLiteral text,
    // so the magnitude read must strip it (from_chars into an unsigned type
    // rejects '-' and used to leave every out-of-int32 negative typed Integer).
    // A negative value reaches one further at each width (|INT_MIN| = INT_MAX+1),
    // so the bounds are sign-aware.
    CHECK(proj_type("SELECT -5") == DataType::Integer);
    CHECK(proj_type("SELECT -2147483648") == DataType::Integer);           // int32 min
    CHECK(proj_type("SELECT -2147483649") == DataType::BigInt);            // past int32 min
    CHECK(proj_type("SELECT -3000000000") == DataType::BigInt);            // pg: bigint
    CHECK(proj_type("SELECT -9223372036854775808") == DataType::BigInt);   // int64 min
    CHECK(proj_type("SELECT -99999999999999999999999999") == DataType::Decimal);

    // The wider type must WIDEN the reconciled set-op / VALUES column, not narrow
    // back to Integer (the cascade the wrong literal type used to cause).
    CHECK(proj_type("SELECT c FROM (SELECT id AS c FROM users "
                    "UNION SELECT 9223372036854775807 AS c) AS t") == DataType::BigInt);
    CHECK(proj_type("SELECT c FROM (VALUES (1),(9223372036854775807)) AS t(c)") ==
          DataType::BigInt);
    // Same cascade, negative RHS: a bigint-magnitude negative literal must widen
    // the reconciled column to BigInt, not narrow it to Integer.
    CHECK(proj_type("SELECT c FROM (SELECT id AS c FROM users "
                    "UNION SELECT -3000000000 AS c) AS t") == DataType::BigInt);
    CHECK(proj_type("SELECT c FROM (VALUES (1),(-9223372036854775808)) AS t(c)") ==
          DataType::BigInt);
}

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

void test_in_value_list_coercion_warns() {
    std::printf("test_in_value_list_coercion_warns\n");
    // `x IN (value-list)` must apply the SAME cross-type comparison check as
    // `x = y`, `x BETWEEN ..`, and `x IN (subquery)`: a cross-category element
    // raises one ImplicitCoercion warning. Regression: the value-list branch only
    // inferred each element's nullability and skipped coerce(), so `id IN (note)`
    // was silently accepted while `id = note` warned.
    auto cat = make_catalog_null();  // users(id INTEGER NOT NULL, note TEXT nullable)
    parser::Parser p;

    auto warns = [&](const char* sql) -> int {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return -1;
        Analyzer a(cat);
        a.analyze(res.value());
        CHECK(!a.has_errors());  // implicit coercion is a warning, never an error
        return count_code(a, DiagnosticCode::ImplicitCoercion);
    };

    // Cross-category IN value lists warn once (matching the `=` sibling).
    CHECK(warns("SELECT id FROM users WHERE id IN (note)") == 1);
    CHECK(warns("SELECT id FROM users WHERE id IN (1, note, 2)") == 1);  // one per IN
    CHECK(warns("SELECT id FROM users WHERE note IN (1, 2)") == 1);
    // Two independent IN expressions each warn.
    CHECK(warns("SELECT id FROM users WHERE id IN (note) OR id IN (note)") == 2);
    // Same-category lists do NOT warn (no over-flagging regression).
    CHECK(warns("SELECT id FROM users WHERE id IN (1, 2, 3)") == 0);
    CHECK(warns("SELECT id FROM users WHERE note IN ('a', 'b')") == 0);
    // A NULL element (wildcard) is compatible with anything - no warning.
    CHECK(warns("SELECT id FROM users WHERE id IN (1, NULL)") == 0);
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

// Given the first (outermost) Subquery descendant, return the first Subquery
// nested strictly inside it, or nullptr. Used to reach the inner block of a
// doubly-nested subquery.
ASTNode* inner_subquery(ASTNode* outer) {
    if (outer == nullptr) return nullptr;
    for (ASTNode* c = first_child(outer); c != nullptr; c = c->next_sibling) {
        if (ASTNode* hit = find_descendant(c, NodeType::Subquery)) return hit;
    }
    return nullptr;
}

void test_nested_correlation_marks_intermediate() {
    std::printf("test_nested_correlation_marks_intermediate\n");
    auto cat = make_catalog_joins();
    parser::Parser p;
    // Doubly-nested EXISTS: `u.id` inside the innermost (sessions) block resolves
    // two scopes out, against the outermost `users u`. Both the middle (orders)
    // and inner (sessions) subqueries are correlated: neither can be evaluated
    // independently of the outer row. The middle one is the regression guard -
    // previously only the innermost active subquery was flagged.
    auto res = p.parse(
        "SELECT id FROM users u WHERE EXISTS (SELECT 1 FROM orders o WHERE "
        "EXISTS (SELECT 1 FROM sessions s WHERE s.user_id = u.id))");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());
    CHECK(a.diagnostics().empty());

    ASTNode* middle = find_descendant(res.value(), NodeType::Subquery);
    ASTNode* inner = inner_subquery(middle);
    CHECK(middle != nullptr && inner != nullptr && middle != inner);
    CHECK(middle != nullptr && a.is_correlated(middle));
    CHECK(inner != nullptr && a.is_correlated(inner));
}

void test_nested_correlation_intermediate_uncorrelated() {
    std::printf("test_nested_correlation_intermediate_uncorrelated\n");
    auto cat = make_catalog_joins();
    parser::Parser p;
    // Control: the inner (sessions) block references only the middle block's
    // `orders o` - one scope out - so the inner subquery is correlated but the
    // middle subquery references nothing outside itself and stays uncorrelated.
    auto res = p.parse(
        "SELECT id FROM users u WHERE EXISTS (SELECT 1 FROM orders o WHERE "
        "EXISTS (SELECT 1 FROM sessions s WHERE s.user_id = o.user_id))");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());
    CHECK(a.diagnostics().empty());

    ASTNode* middle = find_descendant(res.value(), NodeType::Subquery);
    ASTNode* inner = inner_subquery(middle);
    CHECK(middle != nullptr && inner != nullptr && middle != inner);
    CHECK(middle != nullptr && !a.is_correlated(middle));
    CHECK(inner != nullptr && a.is_correlated(inner));
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

void test_insert_check_violation() {
    std::printf("test_insert_check_violation\n");
    // Build a catalog with CHECK constraints directly (as execute_ddl would).
    InMemoryCatalog cat;
    TableInfo& t = cat.add_table("t", {
        ColumnInfo{"age", DataType::Integer, /*nullable=*/true},     // column_id 1
        ColumnInfo{"score", DataType::Integer, /*nullable=*/true},   // column_id 2
        ColumnInfo{"grade", DataType::Text, /*nullable=*/true},      // column_id 3
    });
    Constraint c1; c1.kind = Constraint::Kind::Check; c1.expr = "age >= 0"; c1.columns = {1};
    Constraint c2; c2.kind = Constraint::Kind::Check;
    c2.expr = "score >= 0 AND score <= 100"; c2.columns = {2};
    t.constraints.push_back(c1);
    t.constraints.push_back(c2);

    parser::Parser p;
    auto viol = [&](const char* sql) {
        auto r = p.parse(sql);
        if (!r.has_value()) return -1;
        Analyzer a(cat);
        a.analyze(r.value());
        return count_code(a, DiagnosticCode::CheckViolation);
    };

    // Definite violations: a constant value that fails the predicate.
    CHECK(viol("INSERT INTO t (age) VALUES (-5)") == 1);
    CHECK(viol("INSERT INTO t (score) VALUES (150)") == 1);       // > 100
    CHECK(viol("INSERT INTO t (score) VALUES (-1)") == 1);        // < 0
    // Satisfying values: no violation.
    CHECK(viol("INSERT INTO t (age) VALUES (5)") == 0);
    CHECK(viol("INSERT INTO t (score) VALUES (50)") == 0);
    CHECK(viol("INSERT INTO t (age, score) VALUES (0, 100)") == 0);  // boundaries
    // NULL makes the predicate UNKNOWN (not FALSE): no violation reported.
    CHECK(viol("INSERT INTO t (age) VALUES (NULL)") == 0);
    // A non-literal value can't be folded: stay silent (no false positive).
    CHECK(viol("INSERT INTO t (age) VALUES (some_func(1))") == 0);
    // A CHECK whose column is omitted (uses default) is not decided.
    CHECK(viol("INSERT INTO t (grade) VALUES ('A')") == 0);
    // Multiple rows: only the violating row is flagged.
    CHECK(viol("INSERT INTO t (age) VALUES (1), (-2), (3)") == 1);
}

void test_check_large_integer_arithmetic() {
    std::printf("test_check_large_integer_arithmetic\n");
    // CHECK arithmetic on BIGINT operands must be EXACT int64. Evaluating through
    // double loses precision past 2^53, which would flip a comparison and flag a
    // false violation on a valid row.
    InMemoryCatalog cat;
    TableInfo& t = cat.add_table("big", {
        ColumnInfo{"a", DataType::BigInt, /*nullable=*/true},   // column_id 1
        ColumnInfo{"b", DataType::BigInt, /*nullable=*/true},   // column_id 2
    });
    Constraint c; c.kind = Constraint::Kind::Check;
    c.expr = "a + b = 9007199254740993"; c.columns = {1, 2};  // 2^53 + 1
    t.constraints.push_back(c);

    parser::Parser p;
    auto viol = [&](const char* sql) {
        auto r = p.parse(sql);
        if (!r.has_value()) return -1;
        Analyzer a(cat);
        a.analyze(r.value());
        return count_code(a, DiagnosticCode::CheckViolation);
    };
    // 2^53 + 1: a+b EXACTLY equals the RHS, so the CHECK is satisfied. In double
    // the sum rounds to 2^53 and mis-compares - the old code flagged a false
    // violation here.
    CHECK(viol("INSERT INTO big (a, b) VALUES (9007199254740992, 1)") == 0);
    // A genuine violation still fires (sum != RHS).
    CHECK(viol("INSERT INTO big (a, b) VALUES (1, 1)") == 1);
}

void test_check_integer_overflow_bails() {
    std::printf("test_check_integer_overflow_bails\n");
    // Integer overflow in a CHECK cannot be folded to a definite value, so the
    // evaluator bails (predicate stays Unknown) rather than invoking UB (an
    // out-of-range double->long long cast) or guessing - no false verdict.
    InMemoryCatalog cat;
    TableInfo& t = cat.add_table("ovf", {
        ColumnInfo{"a", DataType::BigInt, /*nullable=*/true},   // column_id 1
        ColumnInfo{"b", DataType::BigInt, /*nullable=*/true},   // column_id 2
    });
    Constraint c; c.kind = Constraint::Kind::Check; c.expr = "a * b = 0"; c.columns = {1, 2};
    t.constraints.push_back(c);

    parser::Parser p;
    auto viol = [&](const char* sql) {
        auto r = p.parse(sql);
        if (!r.has_value()) return -1;
        Analyzer a(cat);
        a.analyze(r.value());
        return count_code(a, DiagnosticCode::CheckViolation);
    };
    // a*b overflows int64: unfoldable -> predicate Unknown -> no (false) violation.
    CHECK(viol("INSERT INTO ovf (a, b) VALUES (9223372036854775807, 2)") == 0);
    // A small, non-overflowing violation still fires (2*3 = 6 <> 0).
    CHECK(viol("INSERT INTO ovf (a, b) VALUES (2, 3)") == 1);
}

void test_check_division_overflow_bails() {
    std::printf("test_check_division_overflow_bails\n");
    // INT64_MIN / -1 (and INT64_MIN % -1) overflow int64 - evaluating them is
    // signed-overflow UB (the / and % paths were previously computed directly,
    // unlike the guarded + - * paths). INT64_MIN is spelled as (-INT64_MAX - 1)
    // because the literal 9223372036854775808 does not parse. The evaluator must
    // fold WITHOUT UB: `/` bails to Unknown (quotient unrepresentable), `%`
    // returns the well-defined 0. Neither yields a false violation. This test
    // also runs under the sanitizers CI job, which is what catches the UB.
    InMemoryCatalog cat;
    TableInfo& t = cat.add_table("dvz", {
        ColumnInfo{"a", DataType::BigInt, /*nullable=*/true},  // column_id 1
    });
    // Two all-constant CHECKs (no column refs); both fold over INT64_MIN.
    Constraint cd; cd.kind = Constraint::Kind::Check;
    cd.expr = "(-9223372036854775807 - 1) / -1 <> 0";       // div: Unknown
    t.constraints.push_back(cd);
    Constraint cm; cm.kind = Constraint::Kind::Check;
    cm.expr = "(-9223372036854775807 - 1) % -1 = 0";        // mod: True (= 0)
    t.constraints.push_back(cm);

    parser::Parser p;
    auto r = p.parse("INSERT INTO dvz (a) VALUES (1)");
    CHECK(r.has_value());
    Analyzer a(cat);
    a.analyze(r.value());
    // No CHECK folds to a definite False, so no violation is reported (and, under
    // ASan/UBSan, no signed-overflow error occurs).
    CHECK(count_code(a, DiagnosticCode::CheckViolation) == 0);

    // Direct evaluator checks pinning the exact folded verdicts.
    db25::semantic::CheckBindings b;
    CHECK(db25::semantic::evaluate_check("(-9223372036854775807 - 1) / -1 <> 0", b)
          == db25::semantic::CheckResult::Unknown);
    CHECK(db25::semantic::evaluate_check("(-9223372036854775807 - 1) % -1 = 0", b)
          == db25::semantic::CheckResult::True);
    // Ordinary division/modulo still fold exactly.
    CHECK(db25::semantic::evaluate_check("7 / 2 = 3", b) == db25::semantic::CheckResult::True);
    CHECK(db25::semantic::evaluate_check("10 % 3 = 1", b) == db25::semantic::CheckResult::True);
}

void test_check_unary_negation_overflow_bails() {
    std::printf("test_check_unary_negation_overflow_bails\n");
    // Applying unary '-' to INT64_MIN is signed-overflow UB: -INT64_MIN is not
    // representable in int64 (aborts under the sanitizers CI job). The binary
    // + - * / % paths guard INT64_MIN, but the unary-minus operator did not, so
    // a CHECK that negates a subexpression folding to INT64_MIN invoked UB.
    // INT64_MIN is written as (-9223372036854775807 - 1) because the literal
    // 9223372036854775808 does not parse. The evaluator must fold WITHOUT UB,
    // yielding Unknown (the negation is unrepresentable), never a false verdict.
    db25::semantic::CheckBindings b;
    CHECK(db25::semantic::evaluate_check("-(-9223372036854775807 - 1) = 0", b)
          == db25::semantic::CheckResult::Unknown);
    CHECK(db25::semantic::evaluate_check("-(-9223372036854775807 - 1) > 0", b)
          == db25::semantic::CheckResult::Unknown);
    // Ordinary unary minus (and double negation) still fold exactly.
    CHECK(db25::semantic::evaluate_check("- -5 = 5", b) == db25::semantic::CheckResult::True);
    CHECK(db25::semantic::evaluate_check("-3 < 0", b) == db25::semantic::CheckResult::True);

    // End-to-end through analyze(): a CHECK negating INT64_MIN reports no
    // violation (folds Unknown) and, under ASan/UBSan, no signed-overflow error.
    InMemoryCatalog cat;
    TableInfo& t = cat.add_table("uneg", {
        ColumnInfo{"a", DataType::BigInt, /*nullable=*/true},  // column_id 1
    });
    Constraint cu; cu.kind = Constraint::Kind::Check;
    cu.expr = "-(-9223372036854775807 - 1) <> 0";  // negation: Unknown
    t.constraints.push_back(cu);

    parser::Parser p;
    auto r = p.parse("INSERT INTO uneg (a) VALUES (1)");
    CHECK(r.has_value());
    Analyzer a(cat);
    a.analyze(r.value());
    CHECK(count_code(a, DiagnosticCode::CheckViolation) == 0);
}

void test_check_and_or_nonboolean_operand_is_unknown() {
    std::printf("test_check_and_or_nonboolean_operand_is_unknown\n");
    // A logical AND/OR whose operand folds to a concrete NON-boolean value (a
    // string / int the evaluator does not coerce to a truth value) is UNKNOWN,
    // never a definite FALSE - otherwise a legal INSERT gets a spurious
    // CheckViolation. A BOOLEAN column assigned the string literal 'true' is
    // legal (see test_assign_string_to_temporal_boolean), so `active AND verified`
    // with active='true' must not be folded to FALSE.
    db25::semantic::CheckBindings bnd;
    using db25::semantic::CheckResult;
    // Non-boolean operands -> Unknown.
    CHECK(db25::semantic::evaluate_check("'true' AND 'true'", bnd) == CheckResult::Unknown);
    CHECK(db25::semantic::evaluate_check("'true' OR 'false'", bnd) == CheckResult::Unknown);
    CHECK(db25::semantic::evaluate_check("TRUE AND 'true'", bnd) == CheckResult::Unknown);
    CHECK(db25::semantic::evaluate_check("1 AND 1", bnd) == CheckResult::Unknown);
    // Genuine boolean logic still folds exactly, and short-circuit is preserved.
    CHECK(db25::semantic::evaluate_check("TRUE AND TRUE", bnd) == CheckResult::True);
    CHECK(db25::semantic::evaluate_check("TRUE AND FALSE", bnd) == CheckResult::False);
    CHECK(db25::semantic::evaluate_check("TRUE OR FALSE", bnd) == CheckResult::True);
    CHECK(db25::semantic::evaluate_check("FALSE OR FALSE", bnd) == CheckResult::False);
    CHECK(db25::semantic::evaluate_check("FALSE AND 'true'", bnd) == CheckResult::False);  // short-circuit
    CHECK(db25::semantic::evaluate_check("TRUE OR 'x'", bnd) == CheckResult::True);        // short-circuit

    // End-to-end: a BOOLEAN column CHECK `active AND verified` with string-literal
    // values must NOT raise a violation on a legal INSERT.
    InMemoryCatalog cat;
    TableInfo& t = cat.add_table("flags", {
        ColumnInfo{"id", DataType::Integer, /*nullable=*/false},      // column_id 1
        ColumnInfo{"active", DataType::Boolean, /*nullable=*/true},   // column_id 2
        ColumnInfo{"verified", DataType::Boolean, /*nullable=*/true}, // column_id 3
    });
    Constraint ck; ck.kind = Constraint::Kind::Check;
    ck.expr = "active AND verified"; ck.columns = {2, 3};
    t.constraints.push_back(ck);

    parser::Parser p;
    auto r2 = p.parse("INSERT INTO flags (id, active, verified) VALUES (1, 'true', 'true')");
    CHECK(r2.has_value());
    if (r2) {
        Analyzer a(cat);
        a.analyze(r2.value());
        CHECK(count_code(a, DiagnosticCode::CheckViolation) == 0);
    }
}

void test_insert_check_violation_from_default() {
    std::printf("test_insert_check_violation_from_default\n");
    // A column omitted from an INSERT takes its DEFAULT. When that default is a
    // constant, it is folded into the CHECK evaluation, so a default-sourced
    // violation is caught - not just violations from explicit values.
    InMemoryCatalog cat;
    TableInfo& t = cat.add_table("t", {
        // status defaults to 'banned', which violates the CHECK below.
        ColumnInfo{"status", DataType::Text, /*nullable=*/true, /*has_default=*/true,
                   /*column_id=*/1, /*default_expr=*/"'banned'"},
        // qty defaults to -1, which violates qty >= 0.
        ColumnInfo{"qty", DataType::Integer, /*nullable=*/true, /*has_default=*/true,
                   /*column_id=*/2, /*default_expr=*/"-1"},
        // note has no default; used to drive an INSERT that omits status/qty.
        ColumnInfo{"note", DataType::Text, /*nullable=*/true, /*has_default=*/false,
                   /*column_id=*/3, /*default_expr=*/""},
        // ok_col defaults to 5, which satisfies its CHECK (ok_col < 100).
        ColumnInfo{"ok_col", DataType::Integer, /*nullable=*/true, /*has_default=*/true,
                   /*column_id=*/4, /*default_expr=*/"5"},
        // ts defaults to a non-constant call: it must stay unfolded (no violation).
        ColumnInfo{"ts", DataType::Integer, /*nullable=*/true, /*has_default=*/true,
                   /*column_id=*/5, /*default_expr=*/"some_func(1)"},
    });
    Constraint cs; cs.kind = Constraint::Kind::Check;
    cs.expr = "status <> 'banned'"; cs.columns = {1};
    Constraint cq; cq.kind = Constraint::Kind::Check;
    cq.expr = "qty >= 0"; cq.columns = {2};
    Constraint co; co.kind = Constraint::Kind::Check;
    co.expr = "ok_col < 100"; co.columns = {4};
    Constraint ct; ct.kind = Constraint::Kind::Check;
    ct.expr = "ts > 0"; ct.columns = {5};
    t.constraints.push_back(cs);
    t.constraints.push_back(cq);
    t.constraints.push_back(co);
    t.constraints.push_back(ct);

    parser::Parser p;
    auto viol = [&](const char* sql) {
        auto r = p.parse(sql);
        if (!r.has_value()) return -1;
        Analyzer a(cat);
        a.analyze(r.value());
        return count_code(a, DiagnosticCode::CheckViolation);
    };

    // Omitting status AND qty applies both bad defaults -> two violations.
    CHECK(viol("INSERT INTO t (note) VALUES ('x')") == 2);
    // Supplying a passing value overrides the bad default -> no status violation
    // (qty still defaults to -1, so exactly one violation remains).
    CHECK(viol("INSERT INTO t (status) VALUES ('active')") == 1);
    // Supplying passing values for both bad-default columns -> clean.
    CHECK(viol("INSERT INTO t (status, qty) VALUES ('active', 3)") == 0);
    // A constant default that satisfies its CHECK is not a violation (ok_col=5),
    // and a non-constant default (ts) stays unfolded -> only the two bad
    // constant defaults are reported when everything is omitted.
    CHECK(viol("INSERT INTO t (note) VALUES ('y')") == 2);
    // Explicitly writing the offending value is still caught (regression guard);
    // qty is given a passing value so only the explicit status violation fires.
    CHECK(viol("INSERT INTO t (status, qty) VALUES ('banned', 3)") == 1);
}

void test_insert_check_violation_constant_fold() {
    std::printf("test_insert_check_violation_constant_fold\n");
    // A supplied value that is a constant expression (not a bare literal) is
    // folded too, so e.g. an arithmetic constant is decided against the CHECK.
    InMemoryCatalog cat;
    TableInfo& t = cat.add_table("t", {
        ColumnInfo{"n", DataType::Integer, /*nullable=*/true},  // column_id 1
    });
    Constraint c; c.kind = Constraint::Kind::Check; c.expr = "n >= 0"; c.columns = {1};
    t.constraints.push_back(c);

    parser::Parser p;
    auto viol = [&](const char* sql) {
        auto r = p.parse(sql);
        if (!r.has_value()) return -1;
        Analyzer a(cat);
        a.analyze(r.value());
        return count_code(a, DiagnosticCode::CheckViolation);
    };
    CHECK(viol("INSERT INTO t (n) VALUES (3 - 5)") == 1);   // folds to -2 -> violation
    CHECK(viol("INSERT INTO t (n) VALUES (2 + 2)") == 0);   // folds to 4 -> ok
}

void test_insert_explicit_null_into_not_null() {
    std::printf("test_insert_explicit_null_into_not_null\n");
    auto cat = make_catalog();  // id INTEGER NOT NULL
    parser::Parser p;
    // An explicit NULL into a NOT NULL column is a violation even though the
    // column IS listed (covered) - the default does not apply to an explicit value.
    auto res = p.parse("INSERT INTO users (id, name) VALUES (NULL, 'a')");
    CHECK(res.has_value());
    if (!res) return;
    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::NotNullViolation) == 1);

    // A NULL into a nullable column is fine.
    auto ok = p.parse("INSERT INTO users (id, name) VALUES (1, NULL)");
    CHECK(ok.has_value());
    if (!ok) return;
    Analyzer a2(cat);
    a2.analyze(ok.value());
    CHECK(count_code(a2, DiagnosticCode::NotNullViolation) == 0);
}

void test_update_explicit_null_into_not_null() {
    std::printf("test_update_explicit_null_into_not_null\n");
    auto cat = make_catalog();
    parser::Parser p;
    // SET a NOT NULL column to an explicit NULL -> violation.
    auto res = p.parse("UPDATE users SET id = NULL");
    CHECK(res.has_value());
    if (!res) return;
    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::NotNullViolation) == 1);

    // SET a nullable column to NULL is fine.
    auto ok = p.parse("UPDATE users SET name = NULL");
    CHECK(ok.has_value());
    if (!ok) return;
    Analyzer a2(cat);
    a2.analyze(ok.value());
    CHECK(count_code(a2, DiagnosticCode::NotNullViolation) == 0);
}

void test_update_check_violation() {
    std::printf("test_update_check_violation\n");
    // An UPDATE SET assigning a constant that fails a CHECK is a definite
    // violation. Unlike INSERT, unset columns keep their existing (unknown)
    // values - no default substitution - so a CHECK over an unset column stays
    // Unknown.
    InMemoryCatalog cat;
    TableInfo& t = cat.add_table("t", {
        ColumnInfo{"age", DataType::Integer, /*nullable=*/true},    // column_id 1
        ColumnInfo{"score", DataType::Integer, /*nullable=*/true},  // column_id 2
        ColumnInfo{"grade", DataType::Text, /*nullable=*/true},     // column_id 3
        ColumnInfo{"lo", DataType::Integer, /*nullable=*/true},     // column_id 4
        ColumnInfo{"hi", DataType::Integer, /*nullable=*/true},     // column_id 5
    });
    Constraint ca; ca.kind = Constraint::Kind::Check; ca.expr = "age >= 0"; ca.columns = {1};
    Constraint cs; cs.kind = Constraint::Kind::Check;
    cs.expr = "score >= 0 AND score <= 100"; cs.columns = {2};
    Constraint cr; cr.kind = Constraint::Kind::Check;  // spans two columns
    cr.expr = "lo <= hi"; cr.columns = {4, 5};
    t.constraints.push_back(ca);
    t.constraints.push_back(cs);
    t.constraints.push_back(cr);

    parser::Parser p;
    auto viol = [&](const char* sql) {
        auto r = p.parse(sql);
        if (!r.has_value()) return -1;
        Analyzer a(cat);
        a.analyze(r.value());
        return count_code(a, DiagnosticCode::CheckViolation);
    };

    // Single-column CHECKs decided by the SET value.
    CHECK(viol("UPDATE t SET age = -1") == 1);
    CHECK(viol("UPDATE t SET age = 5") == 0);
    CHECK(viol("UPDATE t SET score = 150") == 1);        // > 100
    CHECK(viol("UPDATE t SET age = -1, score = 200") == 2);
    CHECK(viol("UPDATE t SET age = 0, score = 100") == 0);  // boundaries
    // A constant expression SET value is folded too.
    CHECK(viol("UPDATE t SET age = 3 - 5") == 1);        // -2 -> violation
    // NULL makes the predicate Unknown, not a violation.
    CHECK(viol("UPDATE t SET age = NULL") == 0);
    // A non-constant SET value can't be folded: stay silent (no false positive).
    CHECK(viol("UPDATE t SET age = some_func(1)") == 0);
    // grade is referenced by no CHECK.
    CHECK(viol("UPDATE t SET grade = 'A'") == 0);
    // A CHECK spanning two columns is only decided when both are SET: setting
    // only lo leaves hi's existing value unknown -> Unknown -> no report.
    CHECK(viol("UPDATE t SET lo = 10") == 0);
    CHECK(viol("UPDATE t SET lo = 10, hi = 5") == 1);    // 10 <= 5 is false
    CHECK(viol("UPDATE t SET lo = 3, hi = 9") == 0);     // 3 <= 9 is true
    // A WHERE clause does not change the CHECK outcome.
    CHECK(viol("UPDATE t SET age = -1 WHERE grade = 'A'") == 1);
}

void test_insert_default_values() {
    std::printf("test_insert_default_values\n");
    auto cat = make_catalog();  // id INTEGER NOT NULL (no default), name TEXT nullable
    parser::Parser p;
    // DEFAULT VALUES fills every column from its default; id is NOT NULL with no
    // default, so it would be inserted as NULL -> a violation.
    auto res = p.parse("INSERT INTO users DEFAULT VALUES");
    CHECK(res.has_value());
    if (!res) return;
    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::NotNullViolation) == 1);
}

void test_insert_default_values_all_defaulted_ok() {
    std::printf("test_insert_default_values_all_defaulted_ok\n");
    // Every column is nullable or has a default: DEFAULT VALUES is clean.
    InMemoryCatalog cat;
    cat.add_table("t", {
        ColumnInfo{"id", DataType::Integer, /*nullable=*/false, /*has_default=*/true},
        ColumnInfo{"name", DataType::Text, /*nullable=*/true},
    });
    parser::Parser p;
    auto res = p.parse("INSERT INTO t DEFAULT VALUES");
    CHECK(res.has_value());
    if (!res) return;
    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::NotNullViolation) == 0);
    CHECK(!a.has_errors());
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

// IS [NOT] DISTINCT FROM is a null-safe comparison: Boolean and NEVER NULL, even
// when both operands are nullable (unlike a plain `=`, whose result is nullable).
void test_is_distinct_from_boolean_notnull() {
    std::printf("test_is_distinct_from_boolean_notnull\n");
    auto cat = make_catalog_emp();  // salary DOUBLE (nullable), age INTEGER (nullable)
    parser::Parser p;
    for (const char* sql : {"SELECT id FROM emp WHERE salary IS DISTINCT FROM age",
                            "SELECT id FROM emp WHERE salary IS NOT DISTINCT FROM age"}) {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) continue;
        Analyzer a(cat);
        a.analyze(res.value());
        CHECK(!a.has_errors());
        CHECK(a.diagnostics().empty());  // numeric vs numeric: no coercion warning
        ASTNode* be = find_descendant(res.value(), NodeType::BinaryExpr);
        CHECK(be != nullptr && a.type_of(be) == DataType::Boolean);
        // Both operands are nullable, yet the null-safe comparison is NOT NULL.
        CHECK(be != nullptr && a.nullability_of(be) == 1);
    }
    // A plain `=` over the same nullable operands IS nullable - contrast guard
    // proving the not-null result is specific to the null-safe operator.
    {
        auto res = p.parse("SELECT id FROM emp WHERE salary = age");
        CHECK(res.has_value());
        if (res) {
            Analyzer a(cat);
            a.analyze(res.value());
            ASTNode* be = find_descendant(res.value(), NodeType::BinaryExpr);
            CHECK(be != nullptr && a.nullability_of(be) == 2);  // nullable
        }
    }
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

// date +/- integer is legal day arithmetic (-> date); date +/- a non-integer
// numeric (double / decimal) is not an SQL operator and must be rejected, not
// silently typed date. Regression: temporal_arith gated on the whole Numeric
// category, so `date + 2.5` / `date + x::double` were accepted and typed date.
void test_date_arith_rejects_non_integer_numeric() {
    std::printf("test_date_arith_rejects_non_integer_numeric\n");
    InMemoryCatalog cat;
    cat.add_table("events", {ColumnInfo{"d", DataType::Date, false},
                             ColumnInfo{"x", DataType::Double, false},
                             ColumnInfo{"n", DataType::Integer, false}});
    parser::Parser p;
    auto first_type = [&](const char* sql, bool& had_error) -> DataType {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return DataType::Unknown;
        Analyzer a(cat);
        a.analyze(res.value());
        had_error = a.has_errors();
        ASTNode* list = find_child(res.value(), NodeType::SelectList);
        ASTNode* item = list ? first_child(list) : nullptr;
        return item ? a.type_of(item) : DataType::Unknown;
    };
    bool err = false;
    // Legal: date +/- integer -> date, no error.
    CHECK(first_type("SELECT d + n FROM events", err) == DataType::Date && !err);
    CHECK(first_type("SELECT d + 1 FROM events", err) == DataType::Date && !err);
    CHECK(first_type("SELECT d - 7 FROM events", err) == DataType::Date && !err);
    // Illegal: date +/- double / decimal -> rejected.
    first_type("SELECT d + x FROM events", err);   CHECK(err);
    first_type("SELECT d + 2.5 FROM events", err);  CHECK(err);
    first_type("SELECT x + d FROM events", err);    CHECK(err);
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

// ---- Legal SQL the coercion / grouping rules used to over-reject (finding #24) ----

// A string literal assigned to a temporal or boolean column is a standard
// implicit conversion (PostgreSQL-canon); accept it SILENTLY (no diagnostic).
// A non-string cross-category assignment (a number into a DATE) still errors.
void test_assign_string_to_temporal_boolean() {
    std::printf("test_assign_string_to_temporal_boolean\n");
    InMemoryCatalog cat;
    cat.add_table("ev", {
        ColumnInfo{"d", DataType::Date, /*nullable=*/true},
        ColumnInfo{"ts", DataType::Timestamp, /*nullable=*/true},
        ColumnInfo{"flag", DataType::Boolean, /*nullable=*/true},
    });
    parser::Parser p;
    auto clean = [&](const char* sql) {
        auto r = p.parse(sql);
        CHECK(r.has_value());
        if (!r.has_value()) return;
        Analyzer a(cat);
        a.analyze(r.value());
        CHECK(!a.has_errors());
        CHECK(a.diagnostics().empty());  // silent: not even an ImplicitCoercion warning
    };
    clean("INSERT INTO ev (d) VALUES ('2020-01-01')");
    clean("INSERT INTO ev (ts) VALUES ('2020-01-01 10:00:00')");
    clean("INSERT INTO ev (flag) VALUES ('true')");
    clean("UPDATE ev SET d = '2020-01-01'");

    // A number assigned to a DATE is still a hard type mismatch (not string).
    auto r = p.parse("INSERT INTO ev (d) VALUES (5)");
    CHECK(r.has_value());
    if (r.has_value()) {
        Analyzer a(cat);
        a.analyze(r.value());
        CHECK(count_code(a, DiagnosticCode::TypeMismatch) == 1);
    }
}

// INTERVAL scales by a number: `iv * 2`, `2 * iv`, `iv / 2` are all intervals.
// Multiplying a non-interval temporal by a number stays invalid.
void test_interval_scaling_typed() {
    std::printf("test_interval_scaling_typed\n");
    auto cat = make_catalog_temporal();  // events(d DATE, ..., iv INTERVAL NOT NULL, ...)
    parser::Parser p;
    std::optional<parser::ParseResult> h;
    for (const char* sql : {"SELECT iv * 2 FROM events",
                            "SELECT 2 * iv FROM events",
                            "SELECT iv / 2 FROM events"}) {
        Analyzer a(cat);
        ASTNode* e = analyze_temporal(a, p, h, sql);
        CHECK(count_code(a, DiagnosticCode::TypeMismatch) == 0);
        CHECK(e != nullptr && a.type_of(e) == DataType::Interval);
    }
    {
        Analyzer a(cat);
        (void)analyze_temporal(a, p, h, "SELECT d * 2 FROM events");  // date * n is meaningless
        CHECK(count_code(a, DiagnosticCode::TypeMismatch) == 1);
    }
}

// A whole-expression GROUP BY key covers the same expression in the projection,
// so its inner column is not flagged non-grouped; function-name case is folded;
// and an unrelated non-grouped column is still flagged (folding is not a wildcard).
void test_groupby_expression_key() {
    std::printf("test_groupby_expression_key\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    auto flags = [&](const char* sql) -> int {
        auto r = p.parse(sql);
        CHECK(r.has_value());
        if (!r.has_value()) return -1;
        Analyzer a(cat);
        a.analyze(r.value());
        return count_code(a, DiagnosticCode::NonGroupedColumn);
    };
    CHECK(flags("SELECT salary + 1 FROM emp GROUP BY salary + 1") == 0);   // arithmetic key
    CHECK(flags("SELECT UPPER(name) FROM emp GROUP BY upper(name)") == 0); // fn key, mixed case
    CHECK(flags("SELECT salary + 1 FROM emp GROUP BY dept") == 1);         // salary not grouped
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

void test_array_constructor_typed() {
    std::printf("test_array_constructor_typed\n");
    auto cat = make_catalog();
    parser::Parser p;
    auto res = p.parse("SELECT ARRAY[1, 2, 3] FROM users");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());

    // The ARRAY[...] constructor has array type and is not itself NULL; its
    // elements resolve (nested column refs would too).
    ASTNode* list = find_child(res.value(), NodeType::SelectList);
    CHECK(list != nullptr);
    ASTNode* arr = list ? first_child(list) : nullptr;
    CHECK(arr != nullptr && arr->node_type == NodeType::ArrayConstructor);
    CHECK(arr != nullptr && a.type_of(arr) == DataType::Array);
    CHECK(arr != nullptr && arr->context.analysis.nullability == 1);
}

// An array over a column resolves the column (regression against the analyzer
// silently skipping constructor children).
void test_array_constructor_resolves_columns() {
    std::printf("test_array_constructor_resolves_columns\n");
    auto cat = make_catalog();
    parser::Parser p;
    auto res = p.parse("SELECT ARRAY[id, id] FROM users");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());
}

// COLLATE annotates a value: the CollateClause takes its operand's type and
// nullability, and the operand column resolves (regression against the analyzer
// losing the column when COLLATE appears in a projection).
void test_collate_takes_operand_type() {
    std::printf("test_collate_takes_operand_type\n");
    auto cat = make_catalog();
    parser::Parser p;
    auto res = p.parse("SELECT name COLLATE \"C\" FROM users");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(!a.has_errors());

    ASTNode* list = find_child(res.value(), NodeType::SelectList);
    CHECK(list != nullptr);
    ASTNode* coll = list ? first_child(list) : nullptr;
    CHECK(coll != nullptr && coll->node_type == NodeType::CollateClause);
    // name is Text and nullable; the collation annotation preserves both.
    CHECK(coll != nullptr && a.type_of(coll) == DataType::Text);
    CHECK(coll != nullptr && coll->context.analysis.nullability == 2);
}

}  // namespace

int main() {
    test_select_resolves_clean();
    test_unresolved_column();
    test_alias_resolution();
    test_derived_table();
    test_derived_table_column_aliases();
    test_iequals_helper();
    test_case_insensitive_table_and_column();
    test_case_insensitive_qualifier_and_alias();
    test_case_insensitive_cte();
    test_case_insensitive_mixed_case_catalog();
    test_case_insensitive_ambiguity_preserved();
    test_case_insensitive_duplicate_relation();
    test_case_insensitive_check_binding();
    test_check_temporal_value_not_compared_lexically();
    test_values_derived_table();
    test_where_type_inference();
    test_cte_resolution();
    test_cte_setop_body();
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
    test_parenthesized_join_group_resolves();
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
    test_groupby_aggregate_key_rejected();
    test_groupby_self_join_distinct_instances();
    test_groupby_derived_same_base_alias();
    test_orderby_qualified_ref_resolves_to_base_column();
    test_groupby_aggregate_in_having_or_orderby_groups();
    test_grouping_trigger_matrix();
    test_values_type_reconciliation_matrix();
    test_groupby_output_alias_key();
    test_groupby_alias_ambiguity_and_aggregate();
    test_groupby_having_aggregate_clean();
    test_having_non_grouped_column();
    test_nested_aggregate();
    test_aggregate_in_where_flagged();
    test_normal_where_not_flagged();
    test_aggregate_in_having_not_flagged();
    test_order_by_non_grouped();
    test_order_by_output_alias_in_grouped_clean();
    test_aggregate_in_over_clause_not_nested();
    test_deep_expression_does_not_crash();
    test_wide_relation_resolution();
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
    test_greatest_least_nullability();
    test_coalesce_greatest_least_type_reconciliation();
    test_setop_derived_table_columns();
    test_in_expr_nullability();
    test_left_join_nullability();
    test_inner_join_nullability_unchanged();

    // Type coercion
    test_integer_literal_width_by_magnitude();
    test_coercion_numeric_comparison_clean();
    test_coercion_text_int_comparison_warns();
    test_in_value_list_coercion_warns();
    test_coercion_arithmetic_text_int_error();

    // Subquery correlation & scalar / IN subqueries
    test_exists_correlated_clean();
    test_nested_correlation_marks_intermediate();
    test_nested_correlation_intermediate_uncorrelated();
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
    test_insert_explicit_null_into_not_null();
    test_insert_default_values();
    test_insert_default_values_all_defaulted_ok();
    test_insert_check_violation();
    test_check_large_integer_arithmetic();
    test_check_integer_overflow_bails();
    test_check_division_overflow_bails();
    test_check_unary_negation_overflow_bails();
    test_check_and_or_nonboolean_operand_is_unknown();
    test_insert_check_violation_from_default();
    test_insert_check_violation_constant_fold();

    // DML: UPDATE
    test_update_clean();
    test_update_explicit_null_into_not_null();
    test_update_check_violation();
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
    test_is_distinct_from_boolean_notnull();
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
    test_date_arith_rejects_non_integer_numeric();

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
    test_interval_scaling_typed();
    test_assign_string_to_temporal_boolean();
    test_groupby_expression_key();
    test_temporal_invalid_date_plus_timestamp();

    // String concatenation (||)
    test_concat_text_notnull();
    test_concat_text_nullable();

    // projection_of column identity
    test_projection_column_identity();
    test_array_constructor_typed();
    test_array_constructor_resolves_columns();
    test_collate_takes_operand_type();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
