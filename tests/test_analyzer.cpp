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

// A VALUES table-value-constructor is a query block in its own right - a
// top-level statement or a CTE body - not only a FROM-derived table. It must be
// analyzed and produce a projection so projection_of() is populated (agreeing
// with the bound plan) and a CTE over VALUES gets its true arity. Previously
// analyze() / analyze_stmt() / register_ctes ignored a ValuesStmt body, so a
// top-level VALUES had no projection and a WITH-over-VALUES CTE had 0 columns.
void test_values_query_block_projection() {
    std::printf("test_values_query_block_projection\n");
    auto cat = make_catalog();
    parser::Parser p;
    // Top-level VALUES: projection has one column per value position.
    {
        auto res = p.parse("VALUES (1, 'a'), (3, 'b')");
        CHECK(res.has_value());
        if (res) {
            Analyzer a(cat);
            a.analyze(res.value());
            CHECK(!a.has_errors());
            const auto* proj = a.projection_of(res.value());
            CHECK(proj != nullptr && proj->size() == 2);
            if (proj != nullptr && proj->size() == 2) {
                CHECK((*proj)[0].type == DataType::Integer);
                CHECK((*proj)[1].type == DataType::Text);
            }
        }
    }
    // A CTE whose body is VALUES gets its true arity, so `SELECT * FROM x`
    // projects that many columns (was 0 -> star expanded to nothing).
    {
        auto res = p.parse("WITH x AS (VALUES (1, 2)) SELECT * FROM x");
        CHECK(res.has_value());
        if (res) {
            Analyzer a(cat);
            a.analyze(res.value());
            CHECK(!a.has_errors());
            const auto* proj = a.projection_of(res.value());
            CHECK(proj != nullptr && proj->size() == 2);
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

// A CTE column-alias list `WITH t(a, b, ...)` may not name MORE columns than the
// CTE body projects (Postgres: `WITH query "t" has N columns available but M
// columns specified`). Regression: the CTE rename loop silently truncated the
// extra aliases, so the arity error went unreported and a later reference to a
// dropped alias failed with a misleading UnresolvedColumn. The derived-table
// sibling already rejected this; the CTE path now reuses the same check.
void test_cte_column_alias_count_mismatch() {
    std::printf("test_cte_column_alias_count_mismatch\n");
    auto cat = make_catalog();  // users(id INTEGER NOT NULL, name TEXT)
    parser::Parser p;

    auto mismatches = [&](const char* sql) -> int {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return -1;
        Analyzer a(cat);
        a.analyze(res.value());
        return count_code(a, DiagnosticCode::ColumnAliasCountMismatch);
    };

    // 3 aliases over a 1-column body -> flagged.
    CHECK(mismatches("WITH t(a, b, c) AS (SELECT id FROM users) SELECT a FROM t") == 1);
    // Exact and fewer alias counts are both legal -> not flagged.
    CHECK(mismatches("WITH t(a) AS (SELECT id FROM users) SELECT a FROM t") == 0);
    CHECK(mismatches("WITH t(a, b) AS (SELECT id, name FROM users) SELECT a FROM t") == 0);
    CHECK(mismatches("WITH t(a) AS (SELECT id, name FROM users) SELECT a FROM t") == 0);
}

// A WITH RECURSIVE CTE references itself inside its own body. The analyzer
// pre-registers the CTE (with its anchor term's column types) before analyzing
// the body, so the recursive self-reference resolves. Regression: the CTE name
// was registered only AFTER its body was analyzed, so `FROM t` in the recursive
// term was a false UnresolvedTable and each recursive column a false
// UnresolvedColumn.
void test_recursive_cte() {
    std::printf("test_recursive_cte\n");
    // emp(id INT NOT NULL, name TEXT, manager_id INT) for a hierarchy walk.
    InMemoryCatalog cat;
    cat.add_table("emp", {ColumnInfo{"id", DataType::Integer, /*nullable=*/false},
                          ColumnInfo{"name", DataType::Text, /*nullable=*/true},
                          ColumnInfo{"manager_id", DataType::Integer, /*nullable=*/true}});
    parser::Parser p;

    auto analyze = [&](const char* sql) -> Analyzer {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        Analyzer a(cat);
        if (res) a.analyze(res.value());
        return a;
    };

    // Canonical counter: valid, no diagnostics; the recursive column types from
    // the anchor (SELECT 1 -> Integer).
    {
        auto res = p.parse(
            "WITH RECURSIVE t(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM t WHERE n<10) "
            "SELECT n FROM t");
        CHECK(res.has_value());
        Analyzer a(cat);
        if (res) a.analyze(res.value());
        CHECK(!a.has_errors());
        ASTNode* list = find_child(res.value(), NodeType::SelectList);
        ASTNode* n = list != nullptr ? first_child(list) : nullptr;
        CHECK(n != nullptr && a.type_of(n) == DataType::Integer);
    }

    // Hierarchy walk: the recursive term JOINs a base table against the CTE.
    CHECK(!analyze(
        "WITH RECURSIVE anc(id, manager_id) AS ("
        "  SELECT id, manager_id FROM emp WHERE manager_id IS NULL "
        "  UNION ALL "
        "  SELECT e.id, e.manager_id FROM emp e JOIN anc a ON e.manager_id = a.id) "
        "SELECT id FROM anc").has_errors());

    // A recursive CTE is still union-reconciled: an anchor/recursive-term arity
    // mismatch is reported (SetOpArityMismatch), not silently accepted.
    CHECK(count_code(
        analyze("WITH RECURSIVE t(a, b) AS (SELECT 1, 2 UNION ALL SELECT a FROM t) "
                "SELECT a FROM t"),
        DiagnosticCode::SetOpArityMismatch) == 1);

    // The column-alias arity check still applies to a recursive CTE.
    CHECK(count_code(
        analyze("WITH RECURSIVE t(a, b, c) AS (SELECT 1 UNION ALL SELECT a+1 FROM t) "
                "SELECT a FROM t"),
        DiagnosticCode::ColumnAliasCountMismatch) == 1);

    // A recursive term may not WIDEN a column past the anchor type: SQL fixes the
    // CTE's column types from the anchor (SELECT 1 -> Integer), so a recursive
    // term producing Double (n * 1.5) is rejected, not silently accepted with an
    // inconsistent result type. (Postgres: "column 1 has type integer in
    // non-recursive term but type numeric overall".)
    CHECK(count_code(
        analyze("WITH RECURSIVE t(n) AS (SELECT 1 UNION ALL SELECT n * 1.5 FROM t "
                "WHERE n < 5) SELECT n FROM t"),
        DiagnosticCode::RecursiveTypeMismatch) == 1);
    // A recursive term coercible to the anchor type (n + 1 stays Integer) is fine.
    CHECK(count_code(
        analyze("WITH RECURSIVE t(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM t "
                "WHERE n < 5) SELECT n FROM t"),
        DiagnosticCode::RecursiveTypeMismatch) == 0);

    // Non-linear recursion (the recursive term references the CTE more than once)
    // is illegal - a self-join of the working table has no defined fixpoint.
    CHECK(count_code(
        analyze("WITH RECURSIVE t(n) AS (SELECT 1 UNION ALL "
                "SELECT a.n + b.n FROM t a, t b WHERE a.n < 5) SELECT n FROM t"),
        DiagnosticCode::RecursiveReferenceNotLinear) == 1);
    // The JOIN form of a double self-reference is likewise rejected.
    CHECK(count_code(
        analyze("WITH RECURSIVE t(n) AS (SELECT 1 UNION ALL "
                "SELECT t1.n + 1 FROM t t1 JOIN t t2 ON t1.n = t2.n) SELECT n FROM t"),
        DiagnosticCode::RecursiveReferenceNotLinear) == 1);
    // A single self-reference (linear recursion) is accepted.
    CHECK(count_code(
        analyze("WITH RECURSIVE t(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM t "
                "WHERE n < 5) SELECT n FROM t"),
        DiagnosticCode::RecursiveReferenceNotLinear) == 0);

    // A semantic error in the ANCHOR term is reported exactly ONCE, not doubled
    // (the anchor is analyzed once for its columns and once as the UNION's first
    // branch; the pre-registration pass must not leak its diagnostics).
    CHECK(count_code(
        analyze("WITH RECURSIVE t(n) AS (SELECT nosuchcol UNION ALL SELECT n FROM t) "
                "SELECT n FROM t"),
        DiagnosticCode::UnresolvedColumn) == 1);
}

// A recursive CTE's output nullability is the UNION (OR) of the anchor and the
// recursive term. The CTE is registered with anchor-only nullability so the
// self-reference can resolve; the recursive term's nullability must then be
// widened back in - else a NULL-producing recursive term is reported NOT NULL
// (the unsafe direction: a consumer could drop a needed NULL check).
// An unknown function's result nullability must degrade to nullable rather than
// confidently inheriting NOT NULL from non-null arguments: its semantics are
// opaque and many functions -- unknown aggregates especially (EVERY, BOOL_AND,
// UDFs) return NULL over an empty group. A KNOWN scalar function still
// propagates its arguments' nullability.
void test_unknown_function_nullability() {
    std::printf("test_unknown_function_nullability\n");
    InMemoryCatalog cat;
    cat.add_table("users", {ColumnInfo{"id", DataType::Integer, /*nullable=*/false},
                            ColumnInfo{"name", DataType::Text, /*nullable=*/true}});
    parser::Parser p;
    auto proj0_nullable = [&](const char* sql) -> int {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return -1;
        Analyzer a(cat);
        a.analyze(res.value());
        const auto* proj = a.projection_of(res.value());
        if (proj == nullptr || proj->empty()) return -1;
        return (*proj)[0].nullable ? 1 : 0;
    };
    // EVERY is the SQL-standard synonym of BOOL_AND but is not in the catalog;
    // over non-null input it was wrongly reported NOT NULL. It returns NULL over
    // an empty group, so its result must be nullable (the unsafe->safe fix).
    CHECK(proj0_nullable("SELECT EVERY(id > 0) FROM users") == 1);
    // Any unknown function of a not-null argument: result nullable, not NOT NULL.
    CHECK(proj0_nullable("SELECT some_unknown_fn(id) FROM users") == 1);
    // Guard: a KNOWN scalar function over a not-null argument stays NOT NULL.
    CHECK(proj0_nullable("SELECT ABS(id) FROM users") == 0);
    // Guard: a KNOWN scalar function over a nullable argument stays nullable.
    CHECK(proj0_nullable("SELECT UPPER(name) FROM users") == 1);
}

// EXISTS / NOT EXISTS require a subquery operand. The parser is lenient about
// the operand shape (a scalar `EXISTS 5` or `EXISTS ((SELECT 1) + 2)` parses),
// so the analyzer must reject a non-subquery operand -- otherwise it analyzes
// clean and the binder is handed a tree it cannot lower (analyzer-clean =>
// bind-ok seam). A real subquery operand still analyzes clean.
void test_exists_requires_subquery() {
    std::printf("test_exists_requires_subquery\n");
    InMemoryCatalog cat;
    cat.add_table("users", {ColumnInfo{"id", DataType::Integer, /*nullable=*/false},
                            ColumnInfo{"name", DataType::Text, /*nullable=*/true}});
    parser::Parser p;
    auto has_err = [&](const char* sql) -> int {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return -1;
        Analyzer a(cat);
        a.analyze(res.value());
        return a.has_errors() ? 1 : 0;
    };
    // Scalar / expression operands are rejected.
    CHECK(has_err("SELECT EXISTS 5 FROM users") == 1);
    CHECK(has_err("SELECT id FROM users WHERE EXISTS 5") == 1);
    CHECK(has_err("SELECT * FROM users WHERE EXISTS ((SELECT 1) + 2)") == 1);
    CHECK(has_err("SELECT NOT EXISTS 5 FROM users") == 1);
    // Guard: a real subquery operand analyzes clean.
    CHECK(has_err("SELECT EXISTS (SELECT 1) FROM users") == 0);
    CHECK(has_err("SELECT * FROM users u WHERE EXISTS (SELECT 1 FROM users v WHERE v.id = u.id)") == 0);
    CHECK(has_err("SELECT * FROM users WHERE NOT EXISTS (SELECT 1)") == 0);
}

// EVERY is the SQL-standard synonym of BOOL_AND, so it must be treated as an
// aggregate: a bare non-grouped column alongside EVERY() is illegal (implicit
// single-group aggregate), its result type is BOOLEAN, and there is no
// UnknownFunction warning -- exactly as BOOL_AND already behaves.
void test_every_is_bool_and_synonym() {
    std::printf("test_every_is_bool_and_synonym\n");
    InMemoryCatalog cat;
    cat.add_table("users", {ColumnInfo{"id", DataType::Integer, /*nullable=*/false},
                            ColumnInfo{"name", DataType::Text, /*nullable=*/true}});
    parser::Parser p;
    // A bare non-grouped column alongside EVERY() -> NonGroupedColumn (aggregate
    // query), and NO UnknownFunction warning; identical to BOOL_AND.
    for (const char* fn : {"EVERY", "BOOL_AND"}) {
        const std::string sql = std::string{"SELECT name, "} + fn + "(id > 0) FROM users";
        auto res = p.parse(sql.c_str());
        CHECK(res.has_value());
        if (!res) continue;
        Analyzer a(cat);
        a.analyze(res.value());
        CHECK(count_code(a, DiagnosticCode::NonGroupedColumn) == 1);
        CHECK(count_code(a, DiagnosticCode::UnknownFunction) == 0);
        const auto* proj = a.projection_of(res.value());
        CHECK(proj != nullptr && proj->size() == 2);
        if (proj != nullptr && proj->size() == 2) {
            CHECK((*proj)[1].type == DataType::Boolean);  // EVERY(...) is BOOLEAN
        }
    }
    // A grouped EVERY over an empty group is nullable (like BOOL_AND).
    {
        auto res = p.parse("SELECT EVERY(id > 0) FROM users");
        CHECK(res.has_value());
        if (res) {
            Analyzer a(cat);
            a.analyze(res.value());
            CHECK(!a.has_errors());
            const auto* proj = a.projection_of(res.value());
            CHECK(proj != nullptr && !proj->empty() && (*proj)[0].nullable);
        }
    }
}

// FILTER (WHERE ...) is only permitted on an aggregate. Applying it to a scalar
// function must be rejected; an aggregate with FILTER stays clean.
void test_filter_requires_aggregate() {
    std::printf("test_filter_requires_aggregate\n");
    InMemoryCatalog cat;
    cat.add_table("users", {ColumnInfo{"id", DataType::Integer, /*nullable=*/false},
                            ColumnInfo{"name", DataType::Text, /*nullable=*/true}});
    parser::Parser p;
    auto count_filter_err = [&](const char* sql) -> int {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return -1;
        Analyzer a(cat);
        a.analyze(res.value());
        return count_code(a, DiagnosticCode::FilterOnNonAggregate);
    };
    // Scalar function with FILTER -> rejected.
    CHECK(count_filter_err("SELECT abs(id) FILTER (WHERE id > 1) FROM users") == 1);
    CHECK(count_filter_err("SELECT upper(name) FILTER (WHERE id > 1) FROM users") == 1);
    // Aggregate with FILTER -> clean.
    CHECK(count_filter_err("SELECT count(*) FILTER (WHERE id > 1) FROM users") == 0);
    CHECK(count_filter_err("SELECT sum(id) FILTER (WHERE id > 1) FROM users") == 0);
    CHECK(count_filter_err("SELECT every(id > 0) FILTER (WHERE id > 1) FROM users") == 0);
}

// An aggregate's argument may not contain a window function (Postgres:
// "aggregate function calls cannot contain window function calls"): windowing
// runs after aggregation, so a window result can never feed an aggregate. Both
// analyzed clean before and reached the binder as an unlowerable set function.
void test_window_inside_aggregate_rejected() {
    std::printf("test_window_inside_aggregate_rejected\n");
    auto cat = make_catalog();  // users(id INTEGER NOT NULL, name TEXT)
    parser::Parser p;
    auto count_wia = [&](const char* sql) -> int {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return -1;
        Analyzer a(cat);
        a.analyze(res.value());
        return count_code(a, DiagnosticCode::WindowInAggregate);
    };
    CHECK(count_wia("SELECT sum(row_number() OVER ()) FROM users") == 1);
    CHECK(count_wia("SELECT max(rank() OVER (ORDER BY id)) FROM users") == 1);
    // A window in the aggregate's FILTER predicate is equally illegal.
    CHECK(count_wia("SELECT count(*) FILTER (WHERE row_number() OVER () > 1) FROM users")
          == 1);
    // Guards: a plain aggregate, a plain window function, and a window whose
    // argument is a plain aggregate-over-nothing are all fine.
    CHECK(count_wia("SELECT sum(id) FROM users") == 0);
    CHECK(count_wia("SELECT row_number() OVER (ORDER BY id) FROM users") == 0);
    CHECK(count_wia("SELECT sum(id) OVER () FROM users") == 0);
}

// Aggregate and window functions are not allowed in a RETURNING list (Postgres:
// RETURNING projects the individual affected rows - there is no grouping or
// window framing). Both analyzed clean before and could not be lowered.
void test_aggregate_window_in_returning_rejected() {
    std::printf("test_aggregate_window_in_returning_rejected\n");
    auto cat = make_catalog();  // users(id INTEGER NOT NULL, name TEXT)
    parser::Parser p;
    auto codes = [&](const char* sql, DiagnosticCode code) -> int {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return -1;
        Analyzer a(cat);
        a.analyze(res.value());
        return count_code(a, code);
    };
    CHECK(codes("INSERT INTO users (id,name) VALUES (1,'a') RETURNING count(*)",
                DiagnosticCode::AggregateInReturning) == 1);
    CHECK(codes("INSERT INTO users (id,name) VALUES (1,'a') RETURNING sum(id)",
                DiagnosticCode::AggregateInReturning) == 1);
    CHECK(codes("INSERT INTO users (id,name) VALUES (1,'a') RETURNING row_number() OVER ()",
                DiagnosticCode::WindowInReturning) == 1);
    // Guard: a plain-column / expression / star RETURNING stays clean.
    CHECK(codes("INSERT INTO users (id,name) VALUES (1,'a') RETURNING id, id + 1",
                DiagnosticCode::AggregateInReturning) == 0);
    CHECK(codes("INSERT INTO users (id,name) VALUES (1,'a') RETURNING id, id + 1",
                DiagnosticCode::WindowInReturning) == 0);
}

// Under SELECT DISTINCT the visible row IS the select list, so an ORDER BY item
// must be composed of selected items: it must match an output column by
// name/ordinal or structurally equal a whole projected select-list expression.
// A key over a non-projected column resolves against the FROM scope but the
// binder rejects it (it would need a hidden sort column that changes the
// distinct key), so the analyzer must reject it too - the two stages must agree.
void test_distinct_order_by_must_be_in_select_list() {
    std::printf("test_distinct_order_by_must_be_in_select_list\n");
    InMemoryCatalog cat;
    cat.add_table("emp", {ColumnInfo{"id", DataType::Integer, /*nullable=*/false},
                          ColumnInfo{"dept", DataType::Text, /*nullable=*/true},
                          ColumnInfo{"sal", DataType::Integer, /*nullable=*/true}});
    parser::Parser p;
    auto err = [&](const char* sql) -> int {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return -1;
        Analyzer a(cat);
        a.analyze(res.value());
        return count_code(a, DiagnosticCode::OrderByNotInSelectDistinct);
    };
    // Non-projected column under DISTINCT -> rejected.
    CHECK(err("SELECT DISTINCT dept FROM emp ORDER BY sal") == 1);
    CHECK(err("SELECT DISTINCT dept FROM emp ORDER BY id") == 1);
    CHECK(err("SELECT DISTINCT dept FROM emp ORDER BY sal * 2") == 1);
    // A projected EXPRESSION must match verbatim; a different literal is a
    // different key.
    CHECK(err("SELECT DISTINCT sal + 1 AS s FROM emp ORDER BY sal + 2") == 1);
    // Projected column (bare, qualified, expression, ordinal, star) -> clean.
    CHECK(err("SELECT DISTINCT dept FROM emp ORDER BY dept") == 0);
    CHECK(err("SELECT DISTINCT emp.dept FROM emp ORDER BY emp.dept") == 0);
    CHECK(err("SELECT DISTINCT dept, sal FROM emp ORDER BY sal") == 0);
    CHECK(err("SELECT DISTINCT sal + 1 AS s FROM emp ORDER BY sal + 1") == 0);
    CHECK(err("SELECT DISTINCT sal FROM emp ORDER BY 1") == 0);
    CHECK(err("SELECT DISTINCT * FROM emp ORDER BY sal") == 0);
    // Without DISTINCT a hidden sort column is allowed -> clean.
    CHECK(err("SELECT dept FROM emp ORDER BY sal") == 0);
    CHECK(err("SELECT dept FROM emp ORDER BY sal * 2") == 0);
}

// The ON CONFLICT DO UPDATE SET assignments and the RETURNING clause of a DML
// statement were never analyzed, so a bad column reference, type mismatch, or
// NOT-NULL violation in them passed silently -- unlike the identical standalone
// UPDATE SET. Both are now checked.
void test_dml_on_conflict_and_returning_analyzed() {
    std::printf("test_dml_on_conflict_and_returning_analyzed\n");
    auto cat = make_catalog();  // users(id INTEGER NOT NULL, name TEXT)
    parser::Parser p;
    auto codes = [&](const char* sql, DiagnosticCode code) -> int {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return -1;
        Analyzer a(cat);
        a.analyze(res.value());
        return count_code(a, code);
    };
    auto clean = [&](const char* sql) -> bool {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return false;
        Analyzer a(cat);
        a.analyze(res.value());
        return !a.has_errors();
    };
    // ON CONFLICT DO UPDATE SET: unresolved column / NOT NULL / type checks, the
    // same as a standalone UPDATE SET.
    CHECK(codes("INSERT INTO users (id,name) VALUES (1,'a') "
                "ON CONFLICT (id) DO UPDATE SET nonexistent = 5",
                DiagnosticCode::UnresolvedColumn) == 1);
    CHECK(codes("INSERT INTO users (id,name) VALUES (1,'a') "
                "ON CONFLICT (id) DO UPDATE SET id = NULL",
                DiagnosticCode::NotNullViolation) == 1);
    CHECK(codes("INSERT INTO users (id,name) VALUES (1,'a') "
                "ON CONFLICT (id) DO UPDATE SET name = 5",
                DiagnosticCode::ImplicitCoercion) == 1);
    // Guard: a well-formed DO UPDATE SET, including an `excluded.col` reference
    // to the proposed row, analyzes clean.
    CHECK(clean("INSERT INTO users (id,name) VALUES (1,'a') "
                "ON CONFLICT (id) DO UPDATE SET name = excluded.name"));
    CHECK(clean("INSERT INTO users (id,name) VALUES (1,'a') "
                "ON CONFLICT (id) DO UPDATE SET name = 'b'"));

    // RETURNING (INSERT / UPDATE / DELETE): a bad column reference is flagged.
    CHECK(codes("INSERT INTO users (id,name) VALUES (1,'a') RETURNING nonexistent",
                DiagnosticCode::UnresolvedColumn) == 1);
    CHECK(codes("UPDATE users SET name = 'a' RETURNING nonexistent",
                DiagnosticCode::UnresolvedColumn) == 1);
    CHECK(codes("DELETE FROM users RETURNING nonexistent",
                DiagnosticCode::UnresolvedColumn) == 1);
    // Guard: well-formed RETURNING (explicit columns, an expression, and *)
    // analyzes clean.
    CHECK(clean("INSERT INTO users (id,name) VALUES (1,'a') RETURNING id, name"));
    CHECK(clean("UPDATE users SET name = 'a' RETURNING id, name"));
    CHECK(clean("DELETE FROM users RETURNING *"));
    CHECK(clean("INSERT INTO users (id,name) VALUES (1,'a') RETURNING id + 1"));

    // ON CONFLICT conflict-target (arbiter) columns must exist in the target
    // table -- the parenthesized index-inference list was never resolved, so a
    // non-existent arbiter column was accepted clean even though the DO UPDATE
    // SET / RETURNING columns of the same statement are checked.
    CHECK(codes("INSERT INTO users (id,name) VALUES (1,'a') "
                "ON CONFLICT (missing) DO NOTHING",
                DiagnosticCode::UnresolvedColumn) == 1);
    CHECK(codes("INSERT INTO users (id,name) VALUES (1,'a') "
                "ON CONFLICT (id, bogus) DO NOTHING",
                DiagnosticCode::UnresolvedColumn) == 1);
    CHECK(codes("INSERT INTO users (id,name) VALUES (1,'a') "
                "ON CONFLICT (missing) DO UPDATE SET name = 'b'",
                DiagnosticCode::UnresolvedColumn) == 1);
    // Guard: valid arbiter columns (single and multi-column) analyze clean, and
    // the DO UPDATE SET payload columns are not mistaken for arbiter columns.
    CHECK(clean("INSERT INTO users (id,name) VALUES (1,'a') ON CONFLICT (id) DO NOTHING"));
    CHECK(clean("INSERT INTO users (id,name) VALUES (1,'a') "
                "ON CONFLICT (id) DO UPDATE SET name = excluded.name"));
}

void test_recursive_cte_nullability() {
    std::printf("test_recursive_cte_nullability\n");
    InMemoryCatalog cat;
    cat.add_table("nums", {ColumnInfo{"i", DataType::Integer, /*nullable=*/false},
                           ColumnInfo{"ni", DataType::Integer, /*nullable=*/true}});
    parser::Parser p;
    auto proj0_nullable = [&](const char* sql) -> int {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return -1;
        Analyzer a(cat);
        a.analyze(res.value());
        const auto* proj = a.projection_of(res.value());
        if (proj == nullptr || proj->empty()) return -1;
        return (*proj)[0].nullable ? 1 : 0;
    };
    // Recursive term projects the nullable column -> CTE column is nullable
    // (was wrongly reported NOT NULL).
    CHECK(proj0_nullable(
        "WITH RECURSIVE t(n) AS (SELECT 1 UNION ALL "
        "SELECT ni FROM nums JOIN t ON t.n = nums.i) SELECT n FROM t") == 1);
    // Guard: both terms NOT NULL -> NOT NULL.
    CHECK(proj0_nullable(
        "WITH RECURSIVE t(n) AS (SELECT 1 UNION ALL "
        "SELECT i FROM nums JOIN t ON t.n = nums.i) SELECT n FROM t") == 0);
    // Guard: a nullable ANCHOR still makes it nullable (the anchor path already
    // worked; this pins that the widening does not regress it).
    CHECK(proj0_nullable(
        "WITH RECURSIVE t(n) AS (SELECT ni FROM nums UNION ALL "
        "SELECT 1 FROM t WHERE t.n < 10) SELECT n FROM t") == 1);
}

// A derived table that exposes the same output alias twice (SELECT id AS a,
// name AS a) makes a reference to that name AMBIGUOUS - it must not silently
// resolve to the first column.
void test_duplicate_derived_alias_ambiguous() {
    std::printf("test_duplicate_derived_alias_ambiguous\n");
    auto cat = make_catalog();  // users(id INT NOT NULL, name TEXT)
    parser::Parser p;
    auto namb = [&](const char* sql) -> int {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return -1;
        Analyzer a(cat);
        a.analyze(res.value());
        return static_cast<int>(count_code(a, DiagnosticCode::AmbiguousColumn));
    };
    // Qualified and bare references to the duplicated alias are ambiguous.
    CHECK(namb("SELECT t.a FROM (SELECT id AS a, name AS a FROM users) t") == 1);
    CHECK(namb("SELECT a FROM (SELECT id AS a, name AS a FROM users) t") == 1);
    // Guard: distinct aliases resolve cleanly (no false ambiguity).
    CHECK(namb("SELECT t.a FROM (SELECT id AS a, name AS b FROM users) t") == 0);
    CHECK(namb("SELECT a, b FROM (SELECT id AS a, name AS b FROM users) t") == 0);
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

// An ordinary (non-LATERAL) derived table is evaluated independently: its body
// may reference enclosing query scopes but NOT its own FROM-clause siblings.
// The analyzer used to analyze the body against the current FROM scope (which
// already holds the preceding siblings), so `SELECT * FROM t1, (SELECT t1.x) s`
// resolved `t1.x` clean though it needs LATERAL - and the binder then failed to
// bind it. The body is now scoped to the enclosing query, matching the binder.
void test_non_lateral_derived_table_cannot_see_siblings() {
    std::printf("test_non_lateral_derived_table_cannot_see_siblings\n");
    auto cat = make_catalog_joins();  // users, orders, sessions
    parser::Parser p;
    auto has_err = [&](const char* sql) -> bool {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return false;
        Analyzer a(cat);
        a.analyze(res.value());
        return a.has_errors();
    };
    // A derived table referencing a comma / JOIN sibling is rejected: LATERAL
    // is required (and its presence is what test_lateral_join_correlation checks
    // makes the same reference legal).
    CHECK(has_err("SELECT * FROM users u, (SELECT u.id) s"));
    CHECK(has_err("SELECT * FROM users u JOIN (SELECT u.id) s ON true"));
    // An independent (uncorrelated) derived table stays clean.
    CHECK(!has_err("SELECT * FROM users u, (SELECT order_id FROM orders) s"));
    CHECK(!has_err("SELECT * FROM users u JOIN (SELECT user_id FROM orders) s "
                   "ON u.id = s.user_id"));
    // Correlation to an ENCLOSING query scope is still allowed (matches the
    // binder, which resolves it through the enclosing chain).
    CHECK(!has_err("SELECT * FROM users u WHERE u.id IN "
                   "(SELECT v FROM (SELECT u.id AS v) s)"));
}

// The LATERAL counterpart: a LATERAL derived table IS evaluated per left row, so
// its body MAY reference the preceding FROM items. The analyzer grants that
// sibling visibility only under LATERAL (the parser marks it with a LateralJoin
// node). This is the accept side of the reject cases above - same references,
// now legal - across the comma form, CROSS JOIN LATERAL and INNER JOIN LATERAL.
// Outer / NATURAL JOIN LATERAL are rejected at the parser and so never reach the
// analyzer.
void test_lateral_derived_table_sees_siblings() {
    std::printf("test_lateral_derived_table_sees_siblings\n");
    auto cat = make_catalog_joins();  // users, orders, sessions
    parser::Parser p;
    auto has_err = [&](const char* sql) -> bool {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return false;
        Analyzer a(cat);
        a.analyze(res.value());
        return a.has_errors();
    };
    // The very references rejected without LATERAL now resolve with it.
    CHECK(!has_err("SELECT * FROM users u, LATERAL (SELECT u.id) s"));
    CHECK(!has_err("SELECT * FROM users u CROSS JOIN LATERAL (SELECT u.id) s"));
    CHECK(!has_err("SELECT * FROM users u JOIN LATERAL (SELECT u.id) s ON true"));
    // LEFT [OUTER] JOIN LATERAL is correlated too; its RHS is additionally
    // null-extended, but that is a binder concern - the analyzer just resolves
    // the correlation. (RIGHT/FULL JOIN LATERAL are rejected at the parser.)
    CHECK(!has_err(
        "SELECT * FROM users u LEFT JOIN LATERAL (SELECT u.id) s ON true"));
    CHECK(!has_err(
        "SELECT * FROM users u LEFT OUTER JOIN LATERAL (SELECT u.id) s ON true"));
    // A LATERAL body may reference EARLIER comma siblings, not only the immediate
    // left one (`u` is visible past `o`).
    CHECK(!has_err("SELECT * FROM users u, orders o, "
                   "LATERAL (SELECT u.id + o.total AS x) s"));
    // LATERAL does not invent columns: an unknown reference is still an error.
    CHECK(has_err("SELECT * FROM users u, LATERAL (SELECT u.nonesuch) s"));
    // And an independent LATERAL body (no correlation) is fine too.
    CHECK(!has_err("SELECT * FROM users u, LATERAL (SELECT total FROM orders) s"));
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

// The merged USING/NATURAL column is COALESCE(left, right); with a NOT NULL join
// key it is NOT NULL under every join type (the preserved side is always
// present), so projection_of must report it NOT NULL - matching the bound plan
// schema. Previously the analyzer OR'd in the null-supplying side's flag and
// wrongly reported the merged column nullable under RIGHT / FULL. A QUALIFIED
// reference still reads the per-side (nullable) copy.
void test_using_merged_column_nullability() {
    std::printf("test_using_merged_column_nullability\n");
    auto cat = make_catalog_joins();  // orders.user_id, sessions.user_id NOT NULL
    parser::Parser p;
    // Bare merged `user_id` -> NOT NULL for INNER / LEFT / RIGHT / FULL / NATURAL.
    auto merged_nullable = [&](const char* sql) -> int {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return -1;
        Analyzer a(cat);
        a.analyze(res.value());
        CHECK(!a.has_errors());
        const auto* proj = a.projection_of(res.value());
        if (proj == nullptr || proj->empty()) return -1;
        return (*proj)[0].nullable ? 1 : 0;
    };
    CHECK(merged_nullable("SELECT user_id FROM orders o JOIN sessions s USING (user_id)") == 0);
    CHECK(merged_nullable("SELECT user_id FROM orders o LEFT JOIN sessions s USING (user_id)") == 0);
    CHECK(merged_nullable("SELECT user_id FROM orders o RIGHT JOIN sessions s USING (user_id)") == 0);
    CHECK(merged_nullable("SELECT user_id FROM orders o FULL JOIN sessions s USING (user_id)") == 0);
    CHECK(merged_nullable("SELECT user_id FROM orders o NATURAL RIGHT JOIN sessions s") == 0);
    // A QUALIFIED reference to the null-supplying side keeps per-side nullability:
    // under RIGHT join the left (orders) side is null-supplied, so o.user_id is
    // nullable even though its base column is NOT NULL.
    CHECK(merged_nullable("SELECT o.user_id FROM orders o RIGHT JOIN sessions s USING (user_id)") == 1);
    // ...and NOT NULL when the qualified side is the preserved one.
    CHECK(merged_nullable("SELECT s.user_id FROM orders o RIGHT JOIN sessions s USING (user_id)") == 0);
}

// A merged USING/NATURAL key is authoritative for its OWN join, but an outer
// join that ENCLOSES the merge and null-supplies the merged relation as a whole
// makes the merged key nullable too - the binder ORs the null flag into every
// child column uniformly, so projection_of must agree (contract invariant #3).
// Previously the analyzer exempted a merged key from ALL null-supplying-side
// adjustment, so it stayed NOT NULL under an enclosing FULL/RIGHT join while the
// bound plan had it nullable - a metadata disagreement across the seam.
void test_using_merged_column_enclosing_outer_join() {
    std::printf("test_using_merged_column_enclosing_outer_join\n");
    auto cat = make_catalog_joins();  // orders/sessions.user_id, users.id NOT NULL
    parser::Parser p;
    auto merged_nullable = [&](const char* sql) -> int {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return -1;
        Analyzer a(cat);
        a.analyze(res.value());
        CHECK(!a.has_errors());
        const auto* proj = a.projection_of(res.value());
        if (proj == nullptr || proj->empty()) return -1;
        return (*proj)[0].nullable ? 1 : 0;
    };
    // Enclosing FULL join: the merged relation (orders JOIN sessions) is
    // null-supplied, so the merged key `user_id` becomes nullable.
    CHECK(merged_nullable(
        "SELECT user_id FROM orders o JOIN sessions s USING (user_id) "
        "FULL JOIN users u ON u.id = user_id") == 1);
    // Enclosing RIGHT join with the merged relation on the (null-supplied) left.
    CHECK(merged_nullable(
        "SELECT user_id FROM orders o JOIN sessions s USING (user_id) "
        "RIGHT JOIN users u ON u.id = user_id") == 1);
    // Enclosing LEFT join keeps the merged relation on the PRESERVED side, so
    // the merged key stays NOT NULL (no over-nulling).
    CHECK(merged_nullable(
        "SELECT user_id FROM orders o JOIN sessions s USING (user_id) "
        "LEFT JOIN users u ON u.id = user_id") == 0);
    // Guard: with NO enclosing outer join the merged key is still NOT NULL
    // (the fix must not regress the own-level authoritative value).
    CHECK(merged_nullable(
        "SELECT user_id FROM orders o FULL JOIN sessions s USING (user_id)") == 0);
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

// A WITH clause above a top-level set operation is in scope for EVERY branch
// (the parser attaches the CTEClause to the set-op node). analyze_setop must
// register those CTEs before analyzing branches - else both arms fail to resolve
// the CTE (false UnresolvedTable / UnresolvedColumn).
void test_cte_above_setop() {
    std::printf("test_cte_above_setop\n");
    auto cat = make_catalog_joins();  // users(id INT NOT NULL, name TEXT)
    parser::Parser p;
    auto clean = [&](const char* sql) -> bool {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return false;
        Analyzer a(cat);
        a.analyze(res.value());
        return !a.has_errors();
    };
    // CTE referenced in the 1st arm / 2nd arm / both; INTERSECT / EXCEPT; and
    // WITH RECURSIVE above a UNION - all must analyze clean.
    CHECK(clean("WITH t AS (SELECT id FROM users) "
                "SELECT id FROM t UNION SELECT id FROM users"));
    CHECK(clean("WITH t AS (SELECT id FROM users) "
                "SELECT id FROM users UNION SELECT id FROM t"));
    CHECK(clean("WITH t AS (SELECT id FROM users) "
                "SELECT id FROM t INTERSECT SELECT id FROM t"));
    CHECK(clean("WITH t AS (SELECT id FROM users) "
                "SELECT id FROM users EXCEPT SELECT id FROM t"));
    CHECK(clean("WITH RECURSIVE r(n) AS "
                "(SELECT 1 UNION ALL SELECT n + 1 FROM r WHERE n < 10) "
                "SELECT n FROM r UNION SELECT id FROM users"));
    // The output schema is the single reconciled column {id, Integer}.
    {
        auto res = p.parse("WITH t AS (SELECT id FROM users) "
                           "SELECT id FROM t UNION SELECT id FROM users");
        CHECK(res.has_value());
        if (res) {
            Analyzer a(cat);
            a.analyze(res.value());
            const auto* proj = a.projection_of(res.value());
            CHECK(proj != nullptr && proj->size() == 1);
            if (proj != nullptr && proj->size() == 1) {
                CHECK((*proj)[0].type == DataType::Integer);
            }
        }
    }
}

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

// Set-op result nullability is per-kind, not a blanket OR across branches:
//   UNION / UNION ALL: a row can come from ANY branch -> OR.
//   INTERSECT: a row is in EVERY branch (NULL matches NULL) -> AND.
//   EXCEPT: rows are drawn only from the LEFT input -> the left's nullability.
// The blanket OR wrongly marked EXCEPT/INTERSECT of a NOT-NULL left nullable.
void test_setop_except_intersect_nullability() {
    std::printf("test_setop_except_intersect_nullability\n");
    InMemoryCatalog cat;
    cat.add_table("nn", {ColumnInfo{"a", DataType::Integer, /*nullable=*/false}});
    cat.add_table("nl", {ColumnInfo{"b", DataType::Integer, /*nullable=*/true}});
    parser::Parser p;

    auto proj0_nullable = [&](const char* sql) -> int {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return -1;
        Analyzer a(cat);
        a.analyze(res.value());
        const auto* proj = a.projection_of(res.value());
        if (proj == nullptr || proj->empty()) return -1;
        return (*proj)[0].nullable ? 1 : 0;
    };

    // EXCEPT: NOT-NULL left -> NOT NULL (was wrongly nullable via blanket OR).
    CHECK(proj0_nullable("SELECT a FROM nn EXCEPT SELECT b FROM nl") == 0);
    // INTERSECT: NOT-NULL left AND nullable right -> NOT NULL.
    CHECK(proj0_nullable("SELECT a FROM nn INTERSECT SELECT b FROM nl") == 0);
    // UNION: a row can come from either side -> nullable (unchanged).
    CHECK(proj0_nullable("SELECT a FROM nn UNION SELECT b FROM nl") == 1);
    // Guard: EXCEPT with a NULLABLE left stays nullable (left's nullability).
    CHECK(proj0_nullable("SELECT b FROM nl EXCEPT SELECT a FROM nn") == 1);
    // Guard: INTERSECT of two nullable sides stays nullable (AND).
    CHECK(proj0_nullable("SELECT b FROM nl INTERSECT SELECT b FROM nl") == 1);
}

// A reconciliation context (UNION/INTERSECT/EXCEPT, VALUES, CASE, COALESCE/
// GREATEST/LEAST) uses Postgres's select_common_type, NOT arithmetic promotion:
// REAL reconciled with an exact numeric (int/bigint/smallint/decimal) stays REAL,
// widening to DOUBLE only against DOUBLE. Arithmetic still widens real+int to
// double.
void test_reconcile_real_keeps_real() {
    std::printf("test_reconcile_real_keeps_real\n");
    InMemoryCatalog cat;
    cat.add_table("t", {ColumnInfo{"r", DataType::Real, true},
                        ColumnInfo{"i", DataType::Integer, true},
                        ColumnInfo{"d", DataType::Double, true},
                        ColumnInfo{"bb", DataType::Boolean, true}});
    parser::Parser p;
    auto out0 = [&](const char* sql) -> DataType {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return DataType::Unknown;
        Analyzer a(cat);
        a.analyze(res.value());
        const auto* proj = a.projection_of(res.value());
        return (proj != nullptr && !proj->empty()) ? (*proj)[0].type : DataType::Unknown;
    };
    // REAL reconciled with an exact numeric stays REAL (was Double), both orders.
    CHECK(out0("SELECT r FROM t UNION SELECT i FROM t") == DataType::Real);
    CHECK(out0("SELECT i FROM t UNION SELECT r FROM t") == DataType::Real);
    CHECK(out0("SELECT COALESCE(r, i) FROM t") == DataType::Real);
    CHECK(out0("SELECT GREATEST(r, i) FROM t") == DataType::Real);
    CHECK(out0("SELECT CASE WHEN bb THEN r ELSE i END FROM t") == DataType::Real);
    // REAL vs DOUBLE still widens to DOUBLE (real coerces to double).
    CHECK(out0("SELECT r FROM t UNION SELECT d FROM t") == DataType::Double);
    // Guard: ARITHMETIC is unchanged - real + int -> double.
    CHECK(out0("SELECT r + i FROM t") == DataType::Double);
    // Guard: exact-only reconcile unchanged - int UNION decimal ladder still widens.
    CHECK(out0("SELECT i FROM t UNION SELECT d FROM t") == DataType::Double);
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

// A window function runs AFTER grouping, so its arguments and OVER (PARTITION BY
// / ORDER BY) expressions may reference only the grouped output - a group key or
// an aggregate. A bare ungrouped column there is illegal, exactly as in the
// SELECT list (Postgres: "column ... must appear in the GROUP BY clause or be
// used in an aggregate function"). Regression: the grouping check exempted every
// child of a window call, so an ungrouped column hidden in an OVER clause slipped
// past the analyzer and only failed later at bind.
void test_groupby_window_ungrouped_column() {
    std::printf("test_groupby_window_ungrouped_column\n");
    auto cat = make_catalog_emp();  // emp(id, name, dept, region, salary, age)
    parser::Parser p;
    auto flags = [&](const char* sql) -> int {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return -1;
        Analyzer a(cat);
        a.analyze(res.value());
        return count_code(a, DiagnosticCode::NonGroupedColumn);
    };

    // Ungrouped column inside the window's OVER clause / argument -> flagged.
    CHECK(flags("SELECT dept, ROW_NUMBER() OVER (ORDER BY salary) "
                "FROM emp GROUP BY dept") == 1);
    CHECK(flags("SELECT dept, RANK() OVER (PARTITION BY age) "
                "FROM emp GROUP BY dept") == 1);
    CHECK(flags("SELECT dept, SUM(salary) OVER (PARTITION BY dept) "
                "FROM emp GROUP BY dept") == 1);  // raw salary arg of a window aggregate

    // Legal: OVER references a group key or an aggregate; a window OVER an
    // aggregate; or the query is not grouped at all.
    CHECK(flags("SELECT dept, ROW_NUMBER() OVER (ORDER BY dept) "
                "FROM emp GROUP BY dept") == 0);
    CHECK(flags("SELECT dept, RANK() OVER (ORDER BY SUM(salary)) "
                "FROM emp GROUP BY dept") == 0);
    CHECK(flags("SELECT dept, SUM(SUM(salary)) OVER (PARTITION BY dept) "
                "FROM emp GROUP BY dept") == 0);
    CHECK(flags("SELECT ROW_NUMBER() OVER (ORDER BY salary) FROM emp") == 0);
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

    // A POSITIONAL key resolving to an aggregate SELECT item must be rejected
    // just like the directly-written form - the ordinal literal is not itself an
    // aggregate, so the raw-key check misses it; the resolved n-th item must be
    // re-tested. Regression: `SELECT COUNT(*) FROM emp GROUP BY 1` slipped
    // through and the binder was handed COUNT() as a group key.
    auto pos_agg = [&](const char* sql) {
        auto r = p.parse(sql);
        CHECK(r.has_value());
        if (!r) return;
        Analyzer a(cat);
        a.analyze(r.value());
        CHECK(count_code(a, DiagnosticCode::AggregateInGroupBy) == 1);
    };
    pos_agg("SELECT COUNT(*) FROM emp GROUP BY 1");
    // A window function is likewise illegal as a group key, in every spelling
    // (grouping precedes windowing). contains_aggregate skips windowed calls, so
    // these are caught by the dedicated window check.
    pos_agg("SELECT RANK() OVER (ORDER BY salary) FROM emp GROUP BY 1");        // positional
    pos_agg("SELECT RANK() OVER (ORDER BY salary) FROM emp "
            "GROUP BY RANK() OVER (ORDER BY salary)");                         // direct
    pos_agg("SELECT RANK() OVER (ORDER BY salary) AS r FROM emp GROUP BY r");  // alias

    // Legal positional keys pointing at a non-aggregate item stay clean.
    auto ok3 = p.parse("SELECT salary + 1, COUNT(*) FROM emp GROUP BY 1");
    CHECK(ok3.has_value());
    if (ok3) {
        Analyzer a(cat);
        a.analyze(ok3.value());
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

void test_aggregate_in_join_condition_flagged() {
    std::printf("test_aggregate_in_join_condition_flagged\n");
    auto cat = make_catalog_joins();
    parser::Parser p;
    // An aggregate in a JOIN ON condition is illegal: a join predicate is a
    // filter evaluated before aggregation, the same position as WHERE.
    auto res = p.parse(
        "SELECT id FROM users u JOIN orders o ON SUM(o.total) > 0");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::AggregateInJoinCondition) == 1);
    CHECK(a.has_errors());

    // Still flagged when the query is grouped and the aggregate is nested in a
    // CASE inside the ON expression.
    auto res2 = p.parse(
        "SELECT o.user_id, COUNT(*) FROM orders o JOIN sessions s "
        "ON CASE WHEN MAX(o.total) > 0 THEN s.user_id ELSE 0 END = o.user_id "
        "GROUP BY o.user_id");
    CHECK(res2.has_value());
    if (!res2) return;
    Analyzer a2(cat);
    a2.analyze(res2.value());
    CHECK(count_code(a2, DiagnosticCode::AggregateInJoinCondition) == 1);
    CHECK(a2.has_errors());
}

void test_window_in_join_condition_flagged() {
    std::printf("test_window_in_join_condition_flagged\n");
    auto cat = make_catalog_joins();
    parser::Parser p;
    // A window function in a JOIN ON condition is illegal for the same reason a
    // window in WHERE is: it is computed after filtering and grouping.
    auto res = p.parse(
        "SELECT id FROM users u JOIN orders o ON ROW_NUMBER() OVER () > 0");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    CHECK(count_code(a, DiagnosticCode::WindowNotAllowed) == 1);
    CHECK(a.has_errors());
}

void test_normal_join_condition_not_flagged() {
    std::printf("test_normal_join_condition_not_flagged\n");
    auto cat = make_catalog_joins();
    parser::Parser p;
    // A plain equijoin predicate has no aggregate/window: nothing to flag. An
    // aggregate legitimately inside an ON subquery is likewise not flagged
    // (contains_aggregate stops at subquery boundaries).
    const char* ok[] = {
        "SELECT u.id FROM users u JOIN orders o ON u.id = o.user_id",
        "SELECT u.id FROM users u JOIN orders o "
        "ON u.id = (SELECT MAX(o2.user_id) FROM orders o2)",
    };
    for (const char* sql : ok) {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) continue;
        Analyzer a(cat);
        a.analyze(res.value());
        CHECK(count_code(a, DiagnosticCode::AggregateInJoinCondition) == 0);
        CHECK(count_code(a, DiagnosticCode::WindowNotAllowed) == 0);
    }
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

    // A chain far past the parser's flat-operator-chain cap is rejected AT PARSE
    // (the producer-owned AST-depth gate), so no unbounded tree ever reaches the
    // analyzer.
    {
        std::string sql = "SELECT * FROM users WHERE id > 0";
        for (int i = 0; i < 5000; ++i) {
            sql += " AND id > 0";
        }
        CHECK(!p.parse(sql).has_value());  // rejected by the parser's chain cap
    }

    // A chain that PARSES (under the parser cap) but exceeds the analyzer's own
    // expression-depth guard (kMaxExprDepth) must be analyzed without a stack
    // overflow and flagged ExpressionTooComplex - the analyzer's defense-in-depth.
    {
        std::string sql = "SELECT * FROM users WHERE id > 0";
        for (int i = 0; i < 700; ++i) {
            sql += " AND id > 0";
        }
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return;
        Analyzer a(cat);
        a.analyze(res.value());  // must return, not overflow the stack
        CHECK(count_code(a, DiagnosticCode::ExpressionTooComplex) == 1);
    }
}

// A non-boolean value used in a boolean context - a WHERE / HAVING predicate or
// an AND / OR / NOT operand - is flagged with a soft ImplicitCoercion (matching
// the CASE WHEN condition check), not silently accepted.
void test_boolean_context_non_boolean_flagged() {
    std::printf("test_boolean_context_non_boolean_flagged\n");
    auto cat = make_catalog();  // users(id INTEGER NOT NULL, name TEXT)
    parser::Parser p;
    auto coerce = [&](const char* sql) -> int {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return -1;
        Analyzer a(cat);
        a.analyze(res.value());
        return count_code(a, DiagnosticCode::ImplicitCoercion);
    };
    // Non-boolean in a boolean context is flagged (one per non-boolean operand).
    CHECK(coerce("SELECT id FROM users WHERE id") == 1);
    CHECK(coerce("SELECT id FROM users WHERE id AND id") == 2);
    CHECK(coerce("SELECT id OR id FROM users") == 2);
    CHECK(coerce("SELECT NOT id FROM users") == 1);
    CHECK(coerce("SELECT NOT name FROM users") == 1);
    // A boolean context with boolean operands stays clean.
    CHECK(coerce("SELECT id FROM users WHERE id = 1") == 0);
    CHECK(coerce("SELECT id FROM users WHERE id = 1 AND name IS NOT NULL") == 0);
    CHECK(coerce("SELECT id FROM users WHERE NOT (id = 1)") == 0);
}

// A positional ORDER BY (`ORDER BY n`) must reference an existing output column
// (1..N); a <= 0 or out-of-range ordinal is an error, exactly as GROUP BY
// positions are validated.
void test_order_by_positional_validated() {
    std::printf("test_order_by_positional_validated\n");
    auto cat = make_catalog();  // users(id, name)
    parser::Parser p;
    auto bad = [&](const char* sql) -> int {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return -1;
        Analyzer a(cat);
        a.analyze(res.value());
        return count_code(a, DiagnosticCode::InvalidOrderByPosition);
    };
    CHECK(bad("SELECT id FROM users ORDER BY 2") == 1);   // only 1 output column
    CHECK(bad("SELECT id FROM users ORDER BY 0") == 1);
    CHECK(bad("SELECT id FROM users ORDER BY -1") == 1);
    CHECK(bad("SELECT id, name FROM users ORDER BY 3") == 1);
    // A literal >= 2^64 must not wrap modulo 2^64 back into a valid ordinal:
    // 18446744073709551617 == 2^64 + 1 would wrap to 1 without the clamp, so
    // it must still be flagged out of range.
    CHECK(bad("SELECT id, name FROM users ORDER BY 18446744073709551617") == 1);
    CHECK(bad("SELECT id, name FROM users ORDER BY 18446744073709551618") == 1);
    // Valid positions and a name reference stay clean.
    CHECK(bad("SELECT id, name FROM users ORDER BY 1") == 0);
    CHECK(bad("SELECT id, name FROM users ORDER BY 2") == 0);
    CHECK(bad("SELECT id FROM users ORDER BY id") == 0);
}

// A top-level ORDER BY on a set operation (UNION/INTERSECT/EXCEPT) is validated
// the same way a plain SELECT's is: its keys reference the union's OUTPUT
// columns only. Previously analyze_setop dropped the ORDER BY entirely, so
// illegal keys were silently accepted.
void test_setop_order_by_validated() {
    std::printf("test_setop_order_by_validated\n");
    auto cat = make_catalog();  // users(id, name)
    parser::Parser p;
    auto codes = [&](const char* sql, DiagnosticCode code) -> int {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return -1;
        Analyzer a(cat);
        a.analyze(res.value());
        return count_code(a, code);
    };
    // A one-column union: `ORDER BY 5` / `ORDER BY -1` are out of range.
    CHECK(codes("SELECT id FROM users UNION SELECT id FROM users ORDER BY 5",
                DiagnosticCode::InvalidOrderByPosition) == 1);
    CHECK(codes("SELECT id FROM users UNION SELECT id FROM users ORDER BY -1",
                DiagnosticCode::InvalidOrderByPosition) == 1);
    // A name that is not an output column is unresolved.
    CHECK(codes("SELECT id FROM users UNION SELECT id FROM users ORDER BY bogus_col",
                DiagnosticCode::UnresolvedColumn) == 1);
    // Legal keys (an in-range position, an output-column name) stay clean.
    CHECK(codes("SELECT id FROM users UNION SELECT id FROM users ORDER BY 1",
                DiagnosticCode::InvalidOrderByPosition) == 0);
    CHECK(codes("SELECT id FROM users UNION SELECT id FROM users ORDER BY id",
                DiagnosticCode::UnresolvedColumn) == 0);
    // INTERSECT / EXCEPT are validated identically.
    CHECK(codes("SELECT id FROM users INTERSECT SELECT id FROM users ORDER BY 9",
                DiagnosticCode::InvalidOrderByPosition) == 1);
    CHECK(codes("SELECT id FROM users EXCEPT SELECT id FROM users ORDER BY nope",
                DiagnosticCode::UnresolvedColumn) == 1);
    // A compound-expression key is illegal in a set-op ORDER BY (Postgres allows
    // only result column names or positions): `id + 1`, `UPPER(name)` are
    // rejected, not silently accepted and left unanalyzed.
    CHECK(codes("SELECT id FROM users UNION SELECT id FROM users ORDER BY id + 1",
                DiagnosticCode::UnresolvedColumn) == 1);
    CHECK(codes("SELECT id FROM users UNION SELECT id FROM users ORDER BY UPPER(name)",
                DiagnosticCode::UnresolvedColumn) == 1);
    CHECK(codes("SELECT id FROM users INTERSECT SELECT id FROM users ORDER BY id * 2",
                DiagnosticCode::UnresolvedColumn) == 1);
    // A legal position / output-name key alongside is still clean (no
    // over-reporting on the valid form).
    CHECK(codes("SELECT id FROM users UNION SELECT id FROM users ORDER BY id",
                DiagnosticCode::UnresolvedColumn) == 0);
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

// A GROUP BY position literal >= 2^64 must not wrap modulo 2^64 back into a
// small in-range ordinal. `18446744073709551617` == 2^64 + 1 would wrap to 1
// without the clamp, silently regrouping by the 1st output column; with the
// clamp the ordinal matches no SELECT item, so `dept` stays non-grouped.
void test_groupby_positional_overflow() {
    std::printf("test_groupby_positional_overflow\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    auto res = p.parse("SELECT dept, COUNT(*) FROM emp GROUP BY 18446744073709551617");
    CHECK(res.has_value());
    if (!res) return;

    Analyzer a(cat);
    a.analyze(res.value());
    // dept is not grouped by the (out-of-range) position, so it is flagged...
    CHECK(count_code(a, DiagnosticCode::NonGroupedColumn) == 1);
    // ...and the out-of-range position itself is now reported.
    CHECK(count_code(a, DiagnosticCode::InvalidOrderByPosition) == 1);
}

// A positional GROUP BY (`GROUP BY n`) must reference an existing output column
// (1..N); a <= 0 or out-of-range ordinal is an error - exactly as ORDER BY
// positions are validated - not a silently-registered phantom grouping key.
void test_groupby_position_out_of_range() {
    std::printf("test_groupby_position_out_of_range\n");
    auto cat = make_catalog();  // users(id, name)
    auto cat_emp = make_catalog_emp();  // emp(dept, age, ...)
    parser::Parser p;
    auto bad = [&](const InMemoryCatalog& c, const char* sql) -> int {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return -1;
        Analyzer a(c);
        a.analyze(res.value());
        return count_code(a, DiagnosticCode::InvalidOrderByPosition);
    };
    // All-aggregate/constant select lists: previously accepted with ZERO
    // diagnostics because the phantom key satisfied the grouping rule.
    CHECK(bad(cat, "SELECT COUNT(*) FROM users GROUP BY 2") == 1);   // only 1 output col
    CHECK(bad(cat, "SELECT COUNT(*) FROM users GROUP BY 0") == 1);
    CHECK(bad(cat, "SELECT 1 FROM users GROUP BY 7") == 1);
    CHECK(bad(cat_emp, "SELECT dept FROM emp GROUP BY dept, 99") == 1);
    CHECK(bad(cat_emp, "SELECT MAX(salary) FROM emp GROUP BY dept, 7") == 1);
    // Valid positions stay clean.
    CHECK(bad(cat, "SELECT id, name FROM users GROUP BY 1") == 0);
    CHECK(bad(cat, "SELECT id, name FROM users GROUP BY 1, 2") == 0);
    CHECK(bad(cat_emp, "SELECT dept, COUNT(*) FROM emp GROUP BY 1") == 0);
}

// A grouped query whose SELECT list uses `*` / `table.*` must validate every
// star-expanded column against the GROUP BY keys - a star column is a raw,
// never-aggregated column, so one that is not a grouping key is illegal. This
// was silently accepted (the Star item fell through the grouping-legality check).
void test_groupby_star_validated() {
    std::printf("test_groupby_star_validated\n");
    auto cat = make_catalog_emp();  // emp(id, name, dept, region, salary, age)
    parser::Parser p;
    auto ngc = [&](const char* sql) -> int {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return -1;
        Analyzer a(cat);
        a.analyze(res.value());
        return count_code(a, DiagnosticCode::NonGroupedColumn);
    };
    // `*` over GROUP BY dept: id, name, region, salary, age are non-grouped (5).
    CHECK(ngc("SELECT * FROM emp GROUP BY dept") == 5);
    // `emp.*` behaves the same as `*` over a single relation.
    CHECK(ngc("SELECT emp.* FROM emp GROUP BY dept") == 5);
    // `*, COUNT(*)` GROUP BY id: name, dept, region, salary, age non-grouped (5).
    CHECK(ngc("SELECT *, COUNT(*) FROM emp GROUP BY id") == 5);
    // Guard: `*` GROUP BY every column is clean (all star columns are keys).
    CHECK(ngc("SELECT * FROM emp GROUP BY id, name, dept, region, salary, age") == 0);
    // POSITIONAL keys over a star: `GROUP BY 1..6` groups by all six EXPANDED
    // columns, so a fully-grouped `SELECT *` is legal (0 diagnostics) - Postgres
    // numbers output columns AFTER star expansion. `GROUP BY 1` groups by id, so
    // only the other five columns are non-grouped (id must NOT be flagged).
    CHECK(ngc("SELECT * FROM emp GROUP BY 1, 2, 3, 4, 5, 6") == 0);
    CHECK(ngc("SELECT * FROM emp GROUP BY 1") == 5);
    CHECK(ngc("SELECT *, COUNT(*) FROM emp GROUP BY 1, 2, 3, 4, 5, 6") == 0);
    // Guard: `*` over an all-aggregate-free ungrouped query is unaffected (no
    // GROUP BY, no aggregate -> analyze_grouping not entered).
    CHECK(ngc("SELECT * FROM emp") == 0);
    // A positional key pointing at an ALIASED item (`id AS foo` is output
    // column 7) must be identified by the SOURCE column's name (`id`), NOT the
    // output alias (`foo`). Otherwise the star's `id` column is compared against
    // key text `foo`, fails same_column_name, and is spuriously flagged. Here
    // GROUP BY 7 groups id (via the alias), and 2..6 group the rest, so every
    // star column is a key: 0 non-grouped. (Before the fix: 1 - phantom `id`.)
    CHECK(ngc("SELECT *, id AS foo FROM emp GROUP BY 7, 2, 3, 4, 5, 6") == 0);
    // GROUP BY 7 alone groups only id (via the aliased item); name, dept,
    // region, salary, age remain non-grouped (5). Before the fix the aliased
    // key text `foo` also failed to group the star's `id`, giving 6.
    CHECK(ngc("SELECT *, id AS foo FROM emp GROUP BY 7") == 5);
}

// A positional GROUP BY key that resolves to a `*`-expanded column must retain
// the relation INSTANCE it came from: in a self-join `emp a, emp b` the columns
// a.id and b.id share (table_id, column_id), so a key on one does not cover the
// other. Two paths must honour this: the SELECT-list legality check for a
// SEPARATE column item (key_matches), and the check for a star's OWN expanded
// columns (the star loop).
void test_groupby_star_selfjoin_instance() {
    std::printf("test_groupby_star_selfjoin_instance\n");
    auto cat = make_catalog_emp();  // emp(id, name, dept, region, salary, age)
    parser::Parser p;
    auto ngc = [&](const char* sql) -> int {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return -1;
        Analyzer a(cat);
        a.analyze(res.value());
        return count_code(a, DiagnosticCode::NonGroupedColumn);
    };
    // `GROUP BY 1` groups a.id (the 1st star-expanded column of instance a). The
    // separate item b.id is a DIFFERENT instance and must be flagged: a.name,
    // a.dept, a.region, a.salary, a.age (5) + b.id (1) = 6. Before the fix the
    // star key's bare text "id" matched b.id via same_relation_instance (one
    // side unqualified), so b.id was silently accepted and the count was 5.
    CHECK(ngc("SELECT a.*, b.id FROM emp a, emp b GROUP BY 1") == 6);
    // Control: the explicit qualified key already disambiguated correctly.
    CHECK(ngc("SELECT a.*, b.id FROM emp a, emp b GROUP BY a.id") == 6);
    // Two star keys: GROUP BY 1,3 groups a.id and a.dept; b.dept is a distinct
    // instance and must be flagged: a.name,a.region,a.salary,a.age (4) + b.dept
    // (1) = 5. Before the fix: 4 (b.dept wrongly grouped by a.dept's key).
    CHECK(ngc("SELECT a.*, b.dept FROM emp a, emp b GROUP BY 1, 3") == 5);
    // The star's OWN columns honour the instance too: `b.*` over an explicit key
    // on a.id must NOT treat b.id as grouped (the star loop ignored relation
    // instance entirely). b.name,b.dept,b.region,b.salary,b.age (5) + b.id (1) =
    // 6. Before the fix: 5 (b.id matched a.id's key by (table_id, column_id)).
    CHECK(ngc("SELECT b.*, a.id FROM emp a, emp b GROUP BY a.id") == 6);
    // A star fully covering its own instance is clean even though the other
    // self-join instance is unselected: a.* grouped by 1..6 -> 0 non-grouped.
    CHECK(ngc("SELECT a.* FROM emp a, emp b GROUP BY 1, 2, 3, 4, 5, 6") == 0);
    // Both instances' columns selected and both fully grouped -> clean.
    CHECK(ngc("SELECT a.*, b.* FROM emp a, emp b "
              "GROUP BY 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12") == 0);
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

// `GROUP BY ()` is the empty grouping set (grand total): the query groups every
// row into ONE group, so a bare non-aggregated column is illegal exactly as
// under an aggregate. The parser now emits a childless GroupByClause for `()`
// (it used to drop the clause, leaving the query looking ungrouped and the bare
// column silently accepted). The analyzer treats any GroupByClause as grouped
// with zero keys and flags the column - no analyzer change was needed, this
// pins the end-to-end behavior across the parser-pin bump.
void test_empty_grouping_set_grand_total() {
    std::printf("test_empty_grouping_set_grand_total\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    auto ngc = [&](const char* sql) -> int {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return -1;
        Analyzer a(cat);
        a.analyze(res.value());
        return count_code(a, DiagnosticCode::NonGroupedColumn);
    };
    // A bare column over the grand total is non-grouped (illegal). Before the
    // parser fix the clause was dropped and this was silently accepted (0).
    CHECK(ngc("SELECT dept FROM emp GROUP BY ()") == 1);
    // Two bare columns -> two diagnostics.
    CHECK(ngc("SELECT dept, name FROM emp GROUP BY ()") == 2);
    // An aggregate-only select list over `GROUP BY ()` is clean (grand total).
    CHECK(ngc("SELECT COUNT(*) FROM emp GROUP BY ()") == 0);
    // A constant is not a column reference, so it is fine under the grand total.
    CHECK(ngc("SELECT 1 FROM emp GROUP BY ()") == 0);
    // `GROUP BY (), dept` == `GROUP BY dept`: the empty set contributes no key,
    // so the query is grouped by dept. `dept` is grouped (clean), a different
    // bare column is not. Before the parser fix the `()` terminated the list and
    // dropped `dept`, collapsing to a grand total that flagged dept (== 1).
    CHECK(ngc("SELECT dept, COUNT(*) FROM emp GROUP BY (), dept") == 0);
    CHECK(ngc("SELECT dept, name FROM emp GROUP BY (), dept") == 1);  // name only
    // A trailing empty set is likewise transparent.
    CHECK(ngc("SELECT dept, COUNT(*) FROM emp GROUP BY dept, ()") == 0);
    // Two real keys around empty sets: both grouped, clean.
    CHECK(ngc("SELECT dept, region FROM emp GROUP BY (), dept, (), region") == 0);
}

// The parser now REJECTS statements it used to silently truncate, so the
// analyzer is never handed a mangled AST (a `SELECT *` whose FROM lost a
// relation, or a set operation reduced to its bare left arm). Confirm these
// no longer parse - the pin bump carries the parser fix (parser #105).
// (Comma-form `LATERAL (subq)`, once dropped here, is now a supported
// derived-table join - see test_lateral_join_correlation - so it is no longer
// a truncation case.)
void test_silently_truncated_inputs_now_rejected() {
    std::printf("test_silently_truncated_inputs_now_rejected\n");
    parser::Parser p;
    auto rejected = [&](const char* sql) -> bool { return !p.parse(sql).has_value(); };
    CHECK(rejected("SELECT id FROM emp UNION"));
    CHECK(rejected("SELECT id FROM emp INTERSECT"));
    CHECK(rejected("UPDATE emp SET dept = WHERE id = 1"));
    // Guards: the well-formed forms still parse.
    CHECK(p.parse("SELECT id FROM emp UNION SELECT id FROM emp").has_value());
    CHECK(p.parse("UPDATE emp SET dept = 'x' WHERE id = 1").has_value());
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

void test_comma_then_outer_join_nullability() {
    std::printf("test_comma_then_outer_join_nullability\n");
    // Comma binds looser than JOIN, so `events e, users u RIGHT JOIN orders o` is
    // `events CROSS (users RIGHT JOIN orders)`: only the RIGHT/FULL join's own
    // left operand (users) is null-supplied. A comma-joined relation that
    // precedes the join (events) must keep its base NOT NULL. Regression: the
    // null-supplying range started at index 0, wrongly nullifying every preceding
    // comma relation.
    InMemoryCatalog cat;
    cat.add_table("users", {ColumnInfo{"id", DataType::Integer, /*nullable=*/false},
                            ColumnInfo{"note", DataType::Text, /*nullable=*/true}});
    cat.add_table("orders", {ColumnInfo{"id", DataType::Integer, /*nullable=*/false},
                             ColumnInfo{"uid", DataType::Integer, /*nullable=*/false}});
    cat.add_table("sessions", {ColumnInfo{"sid", DataType::Integer, /*nullable=*/false}});
    cat.add_table("events", {ColumnInfo{"eid", DataType::Integer, /*nullable=*/false},
                             ColumnInfo{"n", DataType::Integer, /*nullable=*/false}});
    parser::Parser p;

    // Read the nullability the analyzer records on the FIRST select-list column.
    auto null_of_first = [&](const char* sql) -> int {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return -1;
        Analyzer a(cat);
        a.analyze(res.value());
        ASTNode* list = find_child(res.value(), NodeType::SelectList);
        ASTNode* first = list ? first_child(list) : nullptr;
        return first ? a.nullability_of(first) : -1;
    };

    // Comma relation preceding a RIGHT / FULL join keeps NOT NULL (== 1).
    CHECK(null_of_first("SELECT e.n FROM events e, users u "
                        "RIGHT JOIN orders o ON u.id = o.uid") == 1);
    CHECK(null_of_first("SELECT u.id FROM users u, orders o "
                        "FULL JOIN sessions s ON o.uid = s.sid") == 1);
    // The join's OWN left operand IS null-supplied (== 2).
    CHECK(null_of_first("SELECT u.id FROM events e, users u "
                        "RIGHT JOIN orders o ON u.id = o.uid") == 2);
    // Unchanged: comma + LEFT join leaves the comma relation NOT NULL; a single
    // RIGHT/FULL still null-supplies its (only) left operand.
    CHECK(null_of_first("SELECT e.n FROM events e, users u "
                        "LEFT JOIN orders o ON u.id = o.uid") == 1);
    CHECK(null_of_first("SELECT u.id FROM users u "
                        "RIGHT JOIN orders o ON u.id = o.uid") == 2);
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

void test_nullif_cross_category_warns() {
    std::printf("test_nullif_cross_category_warns\n");
    auto cat = make_catalog_null();
    parser::Parser p;
    // NULLIF(id, note) compares an integer to TEXT: a cross-category comparison,
    // so a soft implicit-coercion warning (matching =, IN, BETWEEN, IS DISTINCT).
    {
        auto res = p.parse("SELECT NULLIF(id, note) FROM users");
        CHECK(res.has_value());
        Analyzer a(cat);
        if (res) a.analyze(res.value());
        CHECK(count_code(a, DiagnosticCode::ImplicitCoercion) == 1);
        CHECK(!a.has_errors());  // a warning, not an error
    }
    // Same-category operands: no warning.
    {
        auto res = p.parse("SELECT NULLIF(id, id) FROM users");
        CHECK(res.has_value());
        Analyzer a(cat);
        if (res) a.analyze(res.value());
        CHECK(count_code(a, DiagnosticCode::ImplicitCoercion) == 0);
    }
}

// A constant division / modulo by zero in a REACHABLE position warns (accepted,
// not rejected); inside a CASE arm that constant-fold analysis proves dead it is
// suppressed. Mirrors PostgreSQL's plan-time CASE arm elimination + constant
// folding (which errors on a reachable 1/0 but not on one in a dead arm).
void test_case_constant_div_by_zero() {
    std::printf("test_case_constant_div_by_zero\n");
    InMemoryCatalog cat;
    cat.add_table("case_tbl", {ColumnInfo{"i", DataType::Integer, /*nullable=*/true}});
    parser::Parser p;
    auto div0 = [&](const char* sql) -> int {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        Analyzer a(cat);
        if (res) a.analyze(res.value());
        // The division-by-zero diagnostic is a soft WARNING (DB25 preserves 1/0),
        // so the statement is always accepted - has_errors() stays false.
        CHECK(!a.has_errors());
        return count_code(a, DiagnosticCode::DivisionByZero);
    };
    // Reachable constant div/mod by zero warns.
    CHECK(div0("SELECT 1/0") == 1);
    CHECK(div0("SELECT 10 % 0") == 1);
    // A non-zero (or non-constant) divisor does not warn.
    CHECK(div0("SELECT i / 2 FROM case_tbl") == 0);
    CHECK(div0("SELECT 1 / i FROM case_tbl") == 0);
    // Searched CASE: the 1/0 branch is reachable (i is not constant) -> warns;
    // constant-guarded dead branches do not.
    CHECK(div0("SELECT CASE WHEN i > 100 THEN 1/0 ELSE 0 END FROM case_tbl") == 1);
    CHECK(div0("SELECT CASE WHEN 1=0 THEN 1/0 WHEN 1=1 THEN 1 ELSE 2/0 END") == 0);
    // Simple CASE: operand=value comparison eliminates the dead arms.
    CHECK(div0("SELECT CASE 1 WHEN 0 THEN 1/0 WHEN 1 THEN 1 ELSE 2/0 END") == 0);
    // Two independently-reachable dead-by-nothing arms each warn.
    CHECK(div0("SELECT CASE WHEN i>5 THEN 1/0 WHEN i>10 THEN 2/0 ELSE 3 END "
               "FROM case_tbl") == 2);
    // An ELSE after a constant-true arm is dead.
    CHECK(div0("SELECT CASE WHEN 1=1 THEN 1 ELSE 1/0 END") == 0);
}

// A constant integer + - * whose value overflows its result integer type warns
// (IntegerOverflow). PostgreSQL does not widen intN arithmetic and raises
// out-of-range at runtime; DB25 diagnoses the provable constant cases as a soft
// warning (same family / dead-arm suppression as DivisionByZero), so the
// statement still analyzes clean (has_errors() stays false).
void test_constant_integer_overflow() {
    std::printf("test_constant_integer_overflow\n");
    InMemoryCatalog cat;
    cat.add_table("t", {ColumnInfo{"i", DataType::Integer, /*nullable=*/true}});
    parser::Parser p;
    auto ovf = [&](const char* sql) -> int {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        Analyzer a(cat);
        if (res) a.analyze(res.value());
        CHECK(!a.has_errors());  // soft warning, flows through
        return count_code(a, DiagnosticCode::IntegerOverflow);
    };
    // int4 out of range (int + int stays int; no implicit widening).
    CHECK(ovf("SELECT 2147483647 + 1") == 1);
    CHECK(ovf("SELECT 2000000000 + 2000000000") == 1);
    CHECK(ovf("SELECT 2147483647 * 2") == 1);
    // int8 out of range (bigint + bigint).
    CHECK(ovf("SELECT 9223372036854775807 + 1") == 1);
    CHECK(ovf("SELECT 9223372036854775807 * 2") == 1);
    // In range: no warning.
    CHECK(ovf("SELECT 2147483646 + 1") == 0);   // == int4 max
    CHECK(ovf("SELECT 4000000000 + 1") == 0);   // operands bigint, fits int8
    CHECK(ovf("SELECT 1 + 2 + 3") == 0);
    CHECK(ovf("SELECT i + 1 FROM t") == 0);     // non-constant, not provable
    // Suppressed inside a provably-dead CASE arm (like DivisionByZero).
    CHECK(ovf("SELECT CASE WHEN 1=0 THEN 2147483647 + 1 ELSE 0 END") == 0);
    // Reachable arm still warns.
    CHECK(ovf("SELECT CASE WHEN i > 5 THEN 2147483647 + 1 ELSE 0 END FROM t") == 1);
}

// `||` is array concatenation when an operand is an array (yielding an array),
// not string concatenation to Text. Without this a CASE branch `ARRAY[...] || y`
// was typed Text and failed to reconcile with a sibling ARRAY[...] branch,
// raising a spurious "incompatible CASE result types" error.
void test_array_concat_typing() {
    std::printf("test_array_concat_typing\n");
    InMemoryCatalog cat;
    parser::Parser p;
    auto errs = [&](const char* sql) -> bool {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        Analyzer a(cat);
        if (res) a.analyze(res.value());
        return a.has_errors();
    };
    auto type1 = [&](const char* sql) -> DataType {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        Analyzer a(cat);
        if (res) a.analyze(res.value());
        ASTNode* list = find_child(res.value(), NodeType::SelectList);
        ASTNode* it = list != nullptr ? first_child(list) : nullptr;
        return it != nullptr ? a.type_of(it) : DataType::Unknown;
    };
    // Array || array is an array; string || string stays text.
    CHECK(type1("SELECT ARRAY['a','b'] || ARRAY['c']") == DataType::Array);
    CHECK(type1("SELECT 'a' || 'b'") == DataType::Text);
    // A CASE whose branches are arrays (one via ||) reconciles cleanly.
    CHECK(!errs("SELECT CASE WHEN true THEN ARRAY['a'] || ARRAY['b'] "
                "ELSE ARRAY['x','y'] END"));
    // The pg_case corpus statement: no false CASE-result-type error.
    CHECK(!errs("SELECT CASE 'foo'::text WHEN 'foo' "
                "THEN ARRAY['a','b','c','d'] || enum_range(NULL::casetestenum)::text[] "
                "ELSE ARRAY['x','y'] END"));
    // A genuine mismatch (integer vs text branches) is still an error.
    CHECK(errs("SELECT CASE WHEN true THEN 1 ELSE 'x' END"));
}

// F2: arithmetic on two operands of the SAME non-numeric type (text, boolean)
// is a hard type error, not a silent bogus result type. Postgres: "operator
// does not exist: text + text". coerce()'s identical-type / same-category
// shortcuts wrongly returned Ok under Arithmetic; only numeric (or NULL)
// operands may unify there.
void test_arithmetic_same_type_nonnumeric_errors() {
    std::printf("test_arithmetic_same_type_nonnumeric_errors\n");
    InMemoryCatalog cat;
    cat.add_table("t", {
        ColumnInfo{"txt", DataType::Text, /*nullable=*/false},
        ColumnInfo{"flag", DataType::Boolean, /*nullable=*/false},
        ColumnInfo{"n", DataType::Integer, /*nullable=*/false},
        ColumnInfo{"d", DataType::Double, /*nullable=*/false},
        ColumnInfo{"r", DataType::Real, /*nullable=*/false},
        ColumnInfo{"dec", DataType::Decimal, /*nullable=*/false},
    });
    parser::Parser p;
    auto errs = [&](const char* sql) -> bool {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        Analyzer a(cat);
        if (res) a.analyze(res.value());
        return a.has_errors();
    };
    // Same-type non-numeric arithmetic: every operator, text and boolean.
    CHECK(errs("SELECT txt + txt FROM t"));
    CHECK(errs("SELECT txt - txt FROM t"));
    CHECK(errs("SELECT txt * txt FROM t"));
    CHECK(errs("SELECT txt / txt FROM t"));
    CHECK(errs("SELECT txt % txt FROM t"));
    CHECK(errs("SELECT flag + flag FROM t"));
    CHECK(errs("SELECT flag * flag FROM t"));
    // The diagnostic is a TypeMismatch specifically.
    {
        auto res = p.parse("SELECT txt + txt FROM t");
        CHECK(res.has_value());
        Analyzer a(cat);
        if (res) a.analyze(res.value());
        CHECK(count_code(a, DiagnosticCode::TypeMismatch) >= 1);
    }
    // Cross-category arithmetic (already an error) stays an error.
    CHECK(errs("SELECT txt + n FROM t"));
    // Legal numeric arithmetic stays clean, including NULL (wildcard) operands.
    CHECK(!errs("SELECT n + n FROM t"));
    CHECK(!errs("SELECT d + d FROM t"));
    CHECK(!errs("SELECT n + d FROM t"));
    CHECK(!errs("SELECT n + NULL FROM t"));
    // `||` is string concatenation, NOT arithmetic - it must stay clean.
    CHECK(!errs("SELECT txt || txt FROM t"));

    // Modulo '%' has no operator for approximate floats (float4/float8) in
    // PostgreSQL - only the exact numeric types (int*/NUMERIC) support it. A REAL
    // or DOUBLE operand must be a TypeMismatch, not silently typed Double.
    CHECK(errs("SELECT d % n FROM t"));
    CHECK(errs("SELECT n % d FROM t"));
    CHECK(errs("SELECT r % n FROM t"));
    CHECK(errs("SELECT d % r FROM t"));
    {
        auto res = p.parse("SELECT d % n FROM t");
        CHECK(res.has_value());
        Analyzer a(cat);
        if (res) a.analyze(res.value());
        CHECK(count_code(a, DiagnosticCode::TypeMismatch) >= 1);
    }
    // But '%' on EXACT numerics stays clean, and float is fine for +-*/.
    CHECK(!errs("SELECT n % n FROM t"));
    CHECK(!errs("SELECT dec % n FROM t"));
    CHECK(!errs("SELECT d / n FROM t"));
    CHECK(!errs("SELECT d * r FROM t"));
}

// F4: numeric(Decimal) + real promotes to DOUBLE PRECISION (float8), not real.
// real unifies exactly only with itself; mixed with any other numeric type
// Postgres widens to double, the numeric category's preferred type.
void test_numeric_real_promotes_double() {
    std::printf("test_numeric_real_promotes_double\n");
    InMemoryCatalog cat;
    cat.add_table("nums", {
        ColumnInfo{"dec", DataType::Decimal, /*nullable=*/false},
        ColumnInfo{"re",  DataType::Real, /*nullable=*/false},
        ColumnInfo{"i",   DataType::Integer, /*nullable=*/false},
        ColumnInfo{"d",   DataType::Double, /*nullable=*/false},
    });
    parser::Parser p;
    auto type1 = [&](const char* sql) -> DataType {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        Analyzer a(cat);
        if (res) a.analyze(res.value());
        ASTNode* list = find_child(res.value(), NodeType::SelectList);
        ASTNode* it = list != nullptr ? first_child(list) : nullptr;
        return it != nullptr ? a.type_of(it) : DataType::Unknown;
    };
    // numeric + real -> double (both orders).
    CHECK(type1("SELECT dec + re FROM nums") == DataType::Double);
    CHECK(type1("SELECT re + dec FROM nums") == DataType::Double);
    // real mixed with integer / double -> double.
    CHECK(type1("SELECT re + i FROM nums") == DataType::Double);
    CHECK(type1("SELECT re + d FROM nums") == DataType::Double);
    // real + real stays real (exact self-operator).
    CHECK(type1("SELECT re + re FROM nums") == DataType::Real);
    // The non-real ladder is unchanged: decimal + integer -> decimal, int -> int.
    CHECK(type1("SELECT dec + i FROM nums") == DataType::Decimal);
    CHECK(type1("SELECT i + i FROM nums") == DataType::Integer);
}

// F3: a grouping column nested in ROLLUP()/CUBE()/GROUPING SETS() is a real
// grouping column, so a SELECT of it must be legal. The group-key collector
// must descend into the GroupingElement node (Postgres makes the arguments of
// ROLLUP/CUBE/GROUPING SETS grouping columns).
void test_group_by_grouping_element_columns() {
    std::printf("test_group_by_grouping_element_columns\n");
    auto cat = make_catalog_emp();  // emp(id,name,dept,region,salary,age)
    parser::Parser p;
    auto ngc = [&](const char* sql) -> int {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return -1;
        Analyzer a(cat);
        a.analyze(res.value());
        return static_cast<int>(count_code(a, DiagnosticCode::NonGroupedColumn));
    };
    // ROLLUP / CUBE make their argument columns grouping columns: SELECT of them
    // is legal (was wrongly flagged "must appear in the GROUP BY clause").
    CHECK(ngc("SELECT dept, SUM(salary) FROM emp GROUP BY ROLLUP(dept)") == 0);
    CHECK(ngc("SELECT dept, SUM(salary) FROM emp GROUP BY CUBE(dept)") == 0);
    CHECK(ngc("SELECT dept, region, SUM(salary) FROM emp "
              "GROUP BY ROLLUP(dept, region)") == 0);
    // PARENTHESIZED grouping members: a column wrapped in a ColumnList (a
    // GROUPING SETS member) or a RowConstructor (a parenthesized element or a
    // bare parenthesized GROUP BY list) is still a grouping column. The flatten
    // must descend through those wrappers, not just GroupingElement.
    CHECK(ngc("SELECT dept, region, SUM(salary) FROM emp "
              "GROUP BY GROUPING SETS ((dept), (region))") == 0);
    CHECK(ngc("SELECT dept, SUM(salary) FROM emp "
              "GROUP BY GROUPING SETS ((dept))") == 0);
    CHECK(ngc("SELECT dept, region, SUM(salary) FROM emp "
              "GROUP BY ROLLUP((dept, region))") == 0);
    CHECK(ngc("SELECT dept, region FROM emp GROUP BY (dept, region)") == 0);
    // Guard: a column NOT inside the grouping element is still non-grouped -
    // including alongside a parenthesized member.
    CHECK(ngc("SELECT dept, name, SUM(salary) FROM emp GROUP BY ROLLUP(dept)") == 1);
    CHECK(ngc("SELECT dept, name, SUM(salary) FROM emp "
              "GROUP BY GROUPING SETS ((dept))") == 1);
}

// M2: a NOT NULL column used as a ROLLUP/CUBE/GROUPING SETS grouping key is NULL
// in the super-aggregate (subtotal / grand-total) rows, so it becomes nullable in
// the result even though its base column is NOT NULL - matching Postgres. A plain
// GROUP BY key, and a bare parenthesized GROUP BY list, are NOT nulled (they add
// no super-aggregate rows). The re-marking flows into both the projection
// (projection_of) and the SELECT-list ColumnRef (nullability_of).
void test_group_by_grouping_set_key_nullable() {
    std::printf("test_group_by_grouping_set_key_nullable\n");
    auto cat = make_catalog_emp();  // emp(id INT NOT NULL, ..., salary DOUBLE, ...)
    parser::Parser p;

    // (id, ColumnRef-nullability, projection-nullability) for the leading column.
    struct R { int col_null; bool proj_null; bool ok; };
    auto probe = [&](const char* sql) -> R {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return {-1, false, false};
        Analyzer a(cat);
        a.analyze(res.value());
        ASTNode* list = find_child(res.value(), NodeType::SelectList);
        ASTNode* first = list ? first_child(list) : nullptr;
        const auto* proj = a.projection_of(res.value());
        const bool have = first != nullptr && proj != nullptr && !proj->empty();
        return {have ? a.nullability_of(first) : -1,
                have ? (*proj)[0].nullable : false, have};
    };

    // ROLLUP over the NOT NULL column `id` -> id becomes nullable (2 / true).
    {
        R r = probe("SELECT id, SUM(salary) FROM emp GROUP BY ROLLUP(id)");
        CHECK(r.ok);
        CHECK(r.col_null == 2);
        CHECK(r.proj_null == true);
    }
    // CUBE likewise nulls its NOT NULL argument column.
    {
        R r = probe("SELECT id, SUM(salary) FROM emp GROUP BY CUBE(id)");
        CHECK(r.ok);
        CHECK(r.col_null == 2);
        CHECK(r.proj_null == true);
    }
    // GROUPING SETS member -> nullable too (the member column is grouped-set).
    {
        R r = probe("SELECT id, SUM(salary) FROM emp GROUP BY GROUPING SETS ((id))");
        CHECK(r.ok);
        CHECK(r.col_null == 2);
        CHECK(r.proj_null == true);
    }
    // Guard: a PLAIN GROUP BY of the NOT NULL column keeps it NOT NULL (1 / false).
    {
        R r = probe("SELECT id, SUM(salary) FROM emp GROUP BY id");
        CHECK(r.ok);
        CHECK(r.col_null == 1);
        CHECK(r.proj_null == false);
    }
    // Guard: a bare parenthesized GROUP BY list is NOT a grouping set - the
    // column stays NOT NULL (no super-aggregate rows are introduced).
    {
        R r = probe("SELECT id, region FROM emp GROUP BY (id, region)");
        CHECK(r.ok);
        CHECK(r.col_null == 1);
        CHECK(r.proj_null == false);
    }
}

// GROUPING(col, ...) is the grouping-set indicator function: a BigInt bitmask,
// never NULL. It is neither a value aggregate nor a scalar function, so the
// analyzer types it explicitly - without this it degraded to an UnknownFunction
// warning with an Unknown result type.
void test_grouping_function_typing() {
    std::printf("test_grouping_function_typing\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    const char* ok[] = {
        "SELECT dept, GROUPING(dept), SUM(salary) FROM emp GROUP BY ROLLUP(dept)",
        "SELECT GROUPING(dept) FROM emp GROUP BY GROUPING SETS ((dept))",
        "SELECT GROUPING(dept, region) FROM emp GROUP BY CUBE(dept, region)",
    };
    for (const char* sql : ok) {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) continue;
        Analyzer a(cat);
        a.analyze(res.value());
        // No UnknownFunction warning for GROUPING.
        CHECK(count_code(a, DiagnosticCode::UnknownFunction) == 0);
        // The GROUPING() output column is BigInt and NOT NULL. It is the first
        // projection column in the last two queries; find it in the first.
        const auto* proj = a.projection_of(res.value());
        CHECK(proj != nullptr && !proj->empty());
        if (proj != nullptr) {
            for (const auto& c : *proj) {
                // The bitmask column, wherever it lands, is a not-null BigInt.
                if (c.type == DataType::BigInt) {
                    CHECK(c.nullable == false);
                }
            }
        }
    }
}

// GROUPING(...) is only defined in a grouped query. Used with no GROUP BY (and
// no aggregate that would make the block grouped), it must be REJECTED - it was
// previously typed BigInt and accepted silently (Postgres: "GROUPING must be
// used in a grouped query").
void test_grouping_without_group_by_rejected() {
    std::printf("test_grouping_without_group_by_rejected\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    // Illegal: no grouping present anywhere in the block.
    const char* bad[] = {
        "SELECT GROUPING(dept) FROM emp",
        "SELECT GROUPING(dept), dept FROM emp",
        "SELECT id FROM emp ORDER BY GROUPING(dept)",
    };
    for (const char* sql : bad) {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) continue;
        Analyzer a(cat);
        a.analyze(res.value());
        CHECK(count_code(a, DiagnosticCode::GroupingWithoutGroupBy) == 1);
    }
    // Legal: a grouped query (GROUP BY, grouping set, or an aggregate + HAVING)
    // must NOT raise it.
    const char* ok[] = {
        "SELECT dept, GROUPING(dept) FROM emp GROUP BY dept",
        "SELECT dept, GROUPING(dept) FROM emp GROUP BY ROLLUP(dept)",
        "SELECT GROUPING(dept) FROM emp GROUP BY dept HAVING COUNT(*) > 1",
    };
    for (const char* sql : ok) {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) continue;
        Analyzer a(cat);
        a.analyze(res.value());
        CHECK(count_code(a, DiagnosticCode::GroupingWithoutGroupBy) == 0);
    }
    // A GROUPING nested in a subquery belongs to THAT block: an outer non-grouped
    // query with a grouped subquery using GROUPING is clean at the outer level.
    {
        auto res = p.parse("SELECT (SELECT GROUPING(dept) FROM emp GROUP BY dept "
                           "LIMIT 1) FROM emp");
        CHECK(res.has_value());
        if (res) {
            Analyzer a(cat);
            a.analyze(res.value());
            CHECK(count_code(a, DiagnosticCode::GroupingWithoutGroupBy) == 0);
        }
    }
}

// M2 (pass 13): an ORDER BY key that references a ROLLUP/CUBE/GROUPING SETS
// output column must carry the SAME nullability the SELECT-list column and the
// projection do - nullable - because the key is NULL in the super-aggregate
// rows. The ORDER BY key was resolved BEFORE the grouping-set nullability
// re-mark, so it was left stale NOT NULL; the binder consumes that annotation.
void test_order_by_grouping_set_key_nullable() {
    std::printf("test_order_by_grouping_set_key_nullable\n");
    auto cat = make_catalog_emp();  // emp(id INT NOT NULL, ...)
    parser::Parser p;
    // Return the ORDER BY key's recorded nullability (2 = nullable, 1 = not).
    auto ob_null = [&](const char* sql) -> int {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return -1;
        Analyzer a(cat);
        a.analyze(res.value());
        ASTNode* ob = find_child(res.value(), NodeType::OrderByClause);
        ASTNode* key = ob ? first_child(ob) : nullptr;
        return key ? a.nullability_of(key) : -1;
    };
    // ROLLUP/CUBE/GROUPING SETS: the ORDER BY key `id` is nullable (2), matching
    // the SELECT-list column, whether referenced by name or by position.
    CHECK(ob_null("SELECT id, SUM(salary) FROM emp GROUP BY ROLLUP(id) ORDER BY id") == 2);
    CHECK(ob_null("SELECT id, SUM(salary) FROM emp GROUP BY CUBE(id) ORDER BY id") == 2);
    CHECK(ob_null("SELECT id, SUM(salary) FROM emp "
                  "GROUP BY GROUPING SETS ((id)) ORDER BY id") == 2);
    CHECK(ob_null("SELECT id, SUM(salary) FROM emp GROUP BY ROLLUP(id) ORDER BY 1") == 2);
    // A QUALIFIED ORDER BY key that resolves to the grouping-set column must be
    // nullable too - identical to the unqualified/positional spellings (both a
    // bare table qualifier and a table alias).
    CHECK(ob_null("SELECT id, SUM(salary) FROM emp GROUP BY ROLLUP(id) ORDER BY emp.id") == 2);
    CHECK(ob_null("SELECT id, SUM(salary) FROM emp GROUP BY CUBE(id) ORDER BY emp.id") == 2);
    CHECK(ob_null("SELECT id, SUM(salary) FROM emp e "
                  "GROUP BY ROLLUP(id) ORDER BY e.id") == 2);
    // A COMPOUND-EXPRESSION ORDER BY key that reads the grouping-set key is
    // nullable too - identical to the same expression in the SELECT list.
    CHECK(ob_null("SELECT id, SUM(salary) FROM emp GROUP BY ROLLUP(id) ORDER BY id + 1") == 2);
    CHECK(ob_null("SELECT id, SUM(salary) FROM emp GROUP BY CUBE(id) ORDER BY id * 2") == 2);
    CHECK(ob_null("SELECT id, SUM(salary) FROM emp GROUP BY ROLLUP(id) "
                  "ORDER BY id + id") == 2);
    // Guard: a PLAIN GROUP BY key is NOT nulled, so its ORDER BY key stays NOT
    // NULL (1) - the refresh must not over-null a non-grouping-set key,
    // qualified, positional, or expression.
    CHECK(ob_null("SELECT id, SUM(salary) FROM emp GROUP BY id ORDER BY id") == 1);
    CHECK(ob_null("SELECT id, SUM(salary) FROM emp GROUP BY id ORDER BY 1") == 1);
    CHECK(ob_null("SELECT id, SUM(salary) FROM emp GROUP BY id ORDER BY emp.id") == 1);
    CHECK(ob_null("SELECT id, SUM(salary) FROM emp GROUP BY id ORDER BY id + 1") == 1);
}

// M2 follow-up: a SELECT-list EXPRESSION that reads a ROLLUP/CUBE/GROUPING SETS
// grouping key (outside an aggregate) is also NULL in the super-aggregate rows,
// so it is nullable - not only the bare key column. A read INSIDE an aggregate
// (SUM(key)) does NOT null the aggregate: it sees the present raw value. An
// expression that does not reference the key is unaffected.
void test_group_by_grouping_set_expression_nullable() {
    std::printf("test_group_by_grouping_set_expression_nullable\n");
    auto cat = make_catalog_emp();  // emp(id INT NOT NULL, ..., salary DOUBLE, ...)
    parser::Parser p;

    // Return (nullability_of, projection.nullable) for the select-list item at
    // index `idx`.
    struct R { int col_null; int proj_null; bool ok; };
    auto probe = [&](const char* sql, std::size_t idx) -> R {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return {-1, -1, false};
        Analyzer a(cat);
        a.analyze(res.value());
        ASTNode* list = find_child(res.value(), NodeType::SelectList);
        ASTNode* item = list ? first_child(list) : nullptr;
        for (std::size_t i = 0; i < idx && item != nullptr; ++i) {
            item = item->next_sibling;
        }
        const auto* proj = a.projection_of(res.value());
        const bool have = item != nullptr && proj != nullptr && proj->size() > idx;
        return {have ? a.nullability_of(item) : -1,
                have ? ((*proj)[idx].nullable ? 2 : 1) : -1, have};
    };

    // `id + 0` over a ROLLUP key: the expression is nullable (was wrongly NOT NULL).
    {
        R r = probe("SELECT id + 0 FROM emp GROUP BY ROLLUP(id)", 0);
        CHECK(r.ok);
        CHECK(r.col_null == 2);
        CHECK(r.proj_null == 2);
    }
    // GROUPING SETS member, expression form.
    {
        R r = probe("SELECT id * 2 FROM emp GROUP BY GROUPING SETS ((id))", 0);
        CHECK(r.ok);
        CHECK(r.col_null == 2);
        CHECK(r.proj_null == 2);
    }
    // Mixed list: expression over the key is nulled; an aggregate that reads the
    // key (SUM(id)) and COUNT(*) are governed by their own nullability, NOT the
    // grouping-set nulling - COUNT(*) stays NOT NULL.
    {
        R expr = probe("SELECT id + 0, SUM(id), COUNT(*) FROM emp GROUP BY CUBE(id)", 0);
        CHECK(expr.ok);
        CHECK(expr.col_null == 2);   // id + 0 -> nullable
        CHECK(expr.proj_null == 2);
        R cnt = probe("SELECT id + 0, SUM(id), COUNT(*) FROM emp GROUP BY CUBE(id)", 2);
        CHECK(cnt.ok);
        CHECK(cnt.col_null == 1);    // COUNT(*) stays NOT NULL (does not read the key)
        CHECK(cnt.proj_null == 1);
    }
    // Guard: with a PLAIN GROUP BY (no grouping set), an expression over the
    // NOT NULL key stays NOT NULL - no super-aggregate rows are introduced.
    {
        R r = probe("SELECT id + 0 FROM emp GROUP BY id", 0);
        CHECK(r.ok);
        CHECK(r.col_null == 1);
        CHECK(r.proj_null == 1);
    }
    // Guard: an expression that does NOT read the grouping-set key keeps its own
    // nullability - a literal expression stays NOT NULL under ROLLUP(id).
    {
        R r = probe("SELECT id, 1 + 2 FROM emp GROUP BY ROLLUP(id)", 1);
        CHECK(r.ok);
        CHECK(r.col_null == 1);      // 1 + 2 does not read id -> stays NOT NULL
        CHECK(r.proj_null == 1);
    }
}

// A window function's result nullability is intrinsic (RANK / ROW_NUMBER /
// COUNT(*) OVER are never NULL). A grouping-set key appearing only in the OVER
// clause (PARTITION BY / ORDER BY) does NOT flow into the window value, so it
// must not be nulled by the ROLLUP/CUBE/GROUPING SETS pass - even though the
// same key in a plain expression IS nulled.
void test_group_by_grouping_set_window_not_nulled() {
    std::printf("test_group_by_grouping_set_window_not_nulled\n");
    auto cat = make_catalog_emp();  // emp(id INT NOT NULL, ..., salary DOUBLE)
    parser::Parser p;

    struct R { int col_null; int proj_null; bool ok; };
    auto probe = [&](const char* sql, std::size_t idx) -> R {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return {-1, -1, false};
        Analyzer a(cat);
        a.analyze(res.value());
        ASTNode* list = find_child(res.value(), NodeType::SelectList);
        ASTNode* item = list ? first_child(list) : nullptr;
        for (std::size_t i = 0; i < idx && item != nullptr; ++i) {
            item = item->next_sibling;
        }
        const auto* proj = a.projection_of(res.value());
        const bool have = item != nullptr && proj != nullptr && proj->size() > idx;
        return {have ? a.nullability_of(item) : -1,
                have ? ((*proj)[idx].nullable ? 2 : 1) : -1, have};
    };

    // A window function partitioned/ordered by the ROLLUP key stays NOT NULL.
    const char* not_null_windows[] = {
        "SELECT id, RANK() OVER (PARTITION BY id) FROM emp GROUP BY ROLLUP(id)",
        "SELECT id, ROW_NUMBER() OVER (PARTITION BY id) FROM emp GROUP BY ROLLUP(id)",
        "SELECT id, COUNT(*) OVER (PARTITION BY id) FROM emp GROUP BY CUBE(id)",
        "SELECT id, RANK() OVER (ORDER BY id) FROM emp GROUP BY ROLLUP(id)",
    };
    for (const char* sql : not_null_windows) {
        R w = probe(sql, 1);   // the window column
        CHECK(w.ok);
        CHECK(w.col_null == 1);   // never NULL - not nulled by the grouping set
        CHECK(w.proj_null == 1);
        // Guard: the bare grouping-set key column is STILL nulled (A-EXPR-NULL).
        R k = probe(sql, 0);
        CHECK(k.ok);
        CHECK(k.col_null == 2);
        CHECK(k.proj_null == 2);
    }

    // Guard: a plain expression over the key is still nulled (F3 fix is scoped to
    // window functions, not all functions).
    {
        R e = probe("SELECT id, id + 0 FROM emp GROUP BY ROLLUP(id)", 1);
        CHECK(e.ok);
        CHECK(e.col_null == 2);
        CHECK(e.proj_null == 2);
    }
}

// A window function may not appear in WHERE or HAVING (it is computed after both);
// the analyzer must reject it, exactly as it rejects an aggregate in WHERE - else
// the binder lowers the window into a Filter predicate with nothing to compute it.
void test_window_in_where_having_rejected() {
    std::printf("test_window_in_where_having_rejected\n");
    auto cat = make_catalog_emp();
    parser::Parser p;
    auto nwin = [&](const char* sql) -> int {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return -1;
        Analyzer a(cat);
        a.analyze(res.value());
        return static_cast<int>(count_code(a, DiagnosticCode::WindowNotAllowed));
    };
    // Window in WHERE / HAVING is rejected.
    CHECK(nwin("SELECT id FROM emp WHERE ROW_NUMBER() OVER () > 1") == 1);
    CHECK(nwin("SELECT dept, COUNT(*) FROM emp GROUP BY dept "
               "HAVING RANK() OVER (ORDER BY dept) > 1") == 1);
    // Guards: a window in the SELECT list or ORDER BY is legal (not flagged); an
    // aggregate (not a window) in WHERE is a DIFFERENT diagnostic; and a window
    // inside a WHERE subquery is that block's own business, not flagged here.
    CHECK(nwin("SELECT id, ROW_NUMBER() OVER () FROM emp") == 0);
    CHECK(nwin("SELECT id FROM emp ORDER BY ROW_NUMBER() OVER ()") == 0);
    CHECK(nwin("SELECT id FROM emp WHERE salary > 1") == 0);
    CHECK(nwin("SELECT id FROM emp WHERE id IN "
               "(SELECT ROW_NUMBER() OVER () FROM emp)") == 0);
}

// A quantified comparison `x <cmp> ALL|ANY|SOME (subquery)` is boolean-valued.
// The parser packs the quantifier into the operator text ("> ALL", "= ANY",
// ...); the analyzer once had no branch for it, so it typed the predicate
// Unknown with NO diagnostic (analyze-clean) and the binder then rejected the
// whole legal query. It must type Boolean, run the single-column subquery check,
// and warn on a cross-category comparison.
void test_quantified_comparison_boolean() {
    std::printf("test_quantified_comparison_boolean\n");
    auto cat = make_catalog_emp();  // emp(id INT NOT NULL, name TEXT, ..., salary DOUBLE)
    parser::Parser p;

    // Type of the WHERE predicate, or Unknown if it could not be located.
    auto pred_type = [&](const char* sql) -> DataType {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return DataType::Unknown;
        Analyzer a(cat);
        a.analyze(res.value());
        ASTNode* where = find_child(res.value(), NodeType::WhereClause);
        ASTNode* pred = where ? first_child(where) : nullptr;
        return pred ? a.type_of(pred) : DataType::Unknown;
    };
    auto clean = [&](const char* sql) -> bool {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return false;
        Analyzer a(cat);
        a.analyze(res.value());
        return !a.has_errors();
    };
    auto ncode = [&](const char* sql, DiagnosticCode code) -> int {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return -1;
        Analyzer a(cat);
        a.analyze(res.value());
        return static_cast<int>(count_code(a, code));
    };

    // Every quantifier / comparison combination types Boolean and analyzes clean
    // (same-type, same-category operands -> no coercion warning).
    const char* boolean_forms[] = {
        "SELECT id FROM emp WHERE salary > ALL (SELECT salary FROM emp)",
        "SELECT id FROM emp WHERE salary > ANY (SELECT salary FROM emp)",
        "SELECT id FROM emp WHERE salary = SOME (SELECT salary FROM emp)",
        "SELECT id FROM emp WHERE salary >= ALL (SELECT salary FROM emp)",
        "SELECT id FROM emp WHERE salary <> ANY (SELECT salary FROM emp)",
        "SELECT id FROM emp e WHERE id < ALL (SELECT age FROM emp)",
    };
    for (const char* sql : boolean_forms) {
        CHECK(pred_type(sql) == DataType::Boolean);
        CHECK(clean(sql));
    }

    // Single-column enforcement: a multi-column subquery on the right of a
    // quantified comparison is rejected (ALL/ANY require exactly one column).
    CHECK(ncode("SELECT id FROM emp WHERE salary > ALL (SELECT id, salary FROM emp)",
                DiagnosticCode::ScalarSubqueryColumns) == 1);

    // A cross-category comparison (TEXT vs INTEGER) is still typed Boolean but
    // warns - matching the plain-comparison path.
    CHECK(pred_type("SELECT id FROM emp WHERE name > ALL (SELECT id FROM emp)") ==
          DataType::Boolean);
    CHECK(ncode("SELECT id FROM emp WHERE name > ALL (SELECT id FROM emp)",
                DiagnosticCode::ImplicitCoercion) == 1);
}

// A row-valued `IN (subquery)` - `(a, b) IN (SELECT x, y FROM t)` - is legal
// when the subquery's arity matches the row width; the arity check must compare
// against the LHS row width, not a hard-coded 1.
void test_row_in_subquery() {
    std::printf("test_row_in_subquery\n");
    InMemoryCatalog cat;
    cat.add_table("users", {ColumnInfo{"id", DataType::Integer, /*nullable=*/false},
                            ColumnInfo{"name", DataType::Text, /*nullable=*/true}});
    cat.add_table("sessions", {ColumnInfo{"user_id", DataType::Integer, /*nullable=*/false},
                               ColumnInfo{"token", DataType::Text, /*nullable=*/true}});
    parser::Parser p;
    auto errs = [&](const char* sql) -> bool {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        Analyzer a(cat);
        if (res) a.analyze(res.value());
        return a.has_errors();
    };
    auto ncode = [&](const char* sql, DiagnosticCode code) -> int {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        Analyzer a(cat);
        if (res) a.analyze(res.value());
        return count_code(a, code);
    };
    // Row-valued IN over a matching-arity, type-compatible subquery is legal.
    CHECK(!errs("SELECT id FROM users WHERE (id, name) IN "
                "(SELECT user_id, token FROM sessions)"));
    // Scalar IN is unaffected.
    CHECK(!errs("SELECT id FROM users WHERE id IN (SELECT user_id FROM sessions)"));
    // Arity mismatch still errors, both directions.
    CHECK(ncode("SELECT id FROM users WHERE (id, name) IN "
                "(SELECT user_id FROM sessions)", DiagnosticCode::InSubqueryColumns) == 1);
    CHECK(ncode("SELECT id FROM users WHERE id IN "
                "(SELECT user_id, token FROM sessions)", DiagnosticCode::InSubqueryColumns) == 1);
    // A pairwise type mismatch is a soft ImplicitCoercion warning, not a hard
    // error (id INT vs token TEXT).
    CHECK(!errs("SELECT id FROM users WHERE (id, name) IN "
                "(SELECT token, user_id FROM sessions)"));
    CHECK(ncode("SELECT id FROM users WHERE (id, name) IN "
                "(SELECT token, user_id FROM sessions)",
                DiagnosticCode::ImplicitCoercion) == 1);
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

// Temporal arithmetic with a WILDCARD operand - an untyped NULL literal or a
// bind parameter ($1) - is legal (Postgres infers the untyped operand, typically
// INTERVAL): `timestamp + $1`, `timestamp - $1`, `date + NULL` all yield the
// temporal's type, nullable. It was wrongly a hard TypeMismatch (temporal_arith
// had no wildcard guard, unlike coerce()).
void test_temporal_wildcard_arith() {
    std::printf("test_temporal_wildcard_arith\n");
    auto cat = make_catalog_temporal();  // events(d DATE, ts TIMESTAMP NN, iv INTERVAL NN, ...)
    parser::Parser p;
    // (projected type, analyze-clean).
    auto probe = [&](const char* sql) -> std::pair<DataType, bool> {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) return {DataType::Unknown, false};
        Analyzer a(cat);
        a.analyze(res.value());
        ASTNode* list = find_child(res.value(), NodeType::SelectList);
        ASTNode* item = list ? first_child(list) : nullptr;
        return {item ? a.type_of(item) : DataType::Unknown, !a.has_errors()};
    };
    // timestamp +/- wildcard -> timestamp, clean (both NULL and a bind parameter).
    for (const char* sql : {"SELECT ts + NULL FROM events",
                            "SELECT ts + $1 FROM events",
                            "SELECT ts - $1 FROM events"}) {
        auto [t, clean] = probe(sql);
        CHECK(clean);
        CHECK(t == DataType::Timestamp);
    }
    // date + wildcard -> date; wildcard + timestamp -> timestamp.
    {
        auto [t, c] = probe("SELECT d + $1 FROM events");
        CHECK(c);
        CHECK(t == DataType::Date);
    }
    {
        auto [t, c] = probe("SELECT NULL + ts FROM events");
        CHECK(c);
        CHECK(t == DataType::Timestamp);
    }
    // `wildcard - temporal` is asymmetric with `temporal - wildcard`: the untyped
    // left operand infers as the SAME temporal, so the result is the difference
    // type. `$1 - timestamp` / `NULL - timestamp` -> Interval (timestamp minus
    // timestamp), and `$1 - interval` -> Interval, but `$1 - date` -> Integer
    // (date minus date is whole days). All must analyze clean.
    for (const char* sql : {"SELECT $1 - ts FROM events",
                            "SELECT NULL - ts FROM events"}) {
        auto [t, c] = probe(sql);
        CHECK(c);
        CHECK(t == DataType::Interval);
    }
    {
        auto [t, c] = probe("SELECT $1 - iv FROM events");
        CHECK(c);
        CHECK(t == DataType::Interval);
    }
    {
        auto [t, c] = probe("SELECT $1 - d FROM events");
        CHECK(c);
        CHECK(t == DataType::Integer);
    }
    // Guard: a genuinely invalid temporal mix is STILL rejected (not swallowed by
    // the wildcard path) - timestamp + timestamp has no operator.
    {
        auto [t, c] = probe("SELECT ts + ts FROM events");
        CHECK(!c);
    }
}

// A temporal literal is a non-null constant of its temporal type: an
// `INTERVAL '1 day'` is Interval (was left Unknown, which could mis-reconcile in
// set-ops / CASE / arithmetic), and a DATE/TIME/TIMESTAMP literal carries its
// concrete type. The DateTimeLiteral node is exercised via a synthetic node
// because the pinned parser predates the `DATE '...'` literal (it is produced
// end-to-end once the parser pin bumps).
void test_temporal_literal_types() {
    std::printf("test_temporal_literal_types\n");
    auto cat = make_catalog();
    parser::Parser p;
    // INTERVAL literal, end-to-end through the parser.
    auto res = p.parse("SELECT INTERVAL '1 day'");
    CHECK(res.has_value());
    if (res) {
        Analyzer a(cat);
        a.analyze(res.value());
        const auto* proj = a.projection_of(res.value());
        CHECK(proj != nullptr && proj->size() == 1);
        if (proj != nullptr && proj->size() == 1) {
            CHECK((*proj)[0].type == DataType::Interval);
            CHECK(!(*proj)[0].nullable);  // a literal is never NULL
        }
    }
    // DateTimeLiteral typing (synthetic node -> infer_scalar).
    {
        Analyzer a(cat);
        Scope scope;
        for (DataType dt : {DataType::Date, DataType::Time, DataType::Timestamp}) {
            ASTNode node;
            node.node_type = NodeType::DateTimeLiteral;
            node.data_type = dt;
            CHECK(a.infer_scalar(&node, scope) == dt);
            CHECK(a.nullability_of(&node) == 1);
        }
        // An unset data_type defaults to Timestamp (never Unknown).
        ASTNode node;
        node.node_type = NodeType::DateTimeLiteral;
        CHECK(a.infer_scalar(&node, scope) == DataType::Timestamp);
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

// date - date -> Integer (whole elapsed days), per PostgreSQL - NOT Interval and
// NOT Date. Only time-time / timestamp-timestamp yield Interval. This must agree
// with the wildcard rule ($1 - date -> Integer) and lets a date difference
// reconcile with a real INTEGER in UNION / COALESCE / CASE.
void test_temporal_date_minus_date() {
    std::printf("test_temporal_date_minus_date\n");
    auto cat = make_catalog_temporal();
    parser::Parser p;
    std::optional<parser::ParseResult> h;
    Analyzer a(cat);
    ASTNode* e = analyze_temporal(a, p, h, "SELECT d - d FROM events");
    CHECK(count_code(a, DiagnosticCode::TypeMismatch) == 0);
    CHECK(e != nullptr && a.type_of(e) == DataType::Integer);
}

// The date difference (INTEGER) reconciles with a real INTEGER downstream. When
// `d - d` mis-typed INTERVAL these legal queries were wrongly rejected with a
// SetOpTypeMismatch / COALESCE TypeMismatch.
void test_temporal_date_diff_reconciles() {
    std::printf("test_temporal_date_diff_reconciles\n");
    auto cat = make_catalog_temporal();  // events(..., d2 DATE, n INTEGER NN)
    parser::Parser p;
    for (const char* sql : {
            "SELECT d2 - d FROM events UNION SELECT n FROM events",
            "SELECT COALESCE(d2 - d, n) FROM events"}) {
        auto res = p.parse(sql);
        CHECK(res.has_value());
        if (!res) continue;
        Analyzer a(cat);
        a.analyze(res.value());
        CHECK(!a.has_errors());
    }
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
    test_values_query_block_projection();
    test_where_type_inference();
    test_cte_resolution();
    test_cte_setop_body();
    test_cte_column_alias_count_mismatch();
    test_recursive_cte();
    test_unknown_function_nullability();
    test_exists_requires_subquery();
    test_every_is_bool_and_synonym();
    test_filter_requires_aggregate();
    test_dml_on_conflict_and_returning_analyzed();
    test_window_inside_aggregate_rejected();
    test_aggregate_window_in_returning_rejected();
    test_distinct_order_by_must_be_in_select_list();
    test_recursive_cte_nullability();
    test_duplicate_derived_alias_ambiguous();
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
    test_non_lateral_derived_table_cannot_see_siblings();
    test_lateral_derived_table_sees_siblings();
    test_join_using_resolves();
    test_join_using_missing();
    test_parenthesized_join_group_resolves();
    test_join_using_coalesces_bare_ref();
    test_natural_join_coalesces_bare_ref();
    test_using_merged_column_nullability();
    test_using_merged_column_enclosing_outer_join();
    test_join_on_shared_name_still_ambiguous();
    test_star_over_using_coalesces();
    test_star_over_natural_coalesces();

    // Set-operation reconciliation
    test_cte_above_setop();
    test_setop_union_clean();
    test_setop_arity_mismatch();
    test_setop_type_mismatch();
    test_setop_numeric_compatible();
    test_setop_except_intersect_nullability();
    test_reconcile_real_keeps_real();

    // GROUP BY / HAVING legality & function typing
    test_groupby_clean_count_star();
    test_groupby_non_grouped_column();
    test_groupby_window_ungrouped_column();
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
    test_boolean_context_non_boolean_flagged();
    test_order_by_positional_validated();
    test_setop_order_by_validated();
    test_wide_relation_resolution();
    test_groupby_positional_single();
    test_groupby_positional_multi();
    test_groupby_positional_still_flags_non_grouped();
    test_groupby_positional_overflow();
    test_groupby_position_out_of_range();
    test_groupby_star_validated();
    test_groupby_star_selfjoin_instance();
    test_avg_result_type();
    test_scalar_function_type();
    test_unknown_function_degrades();
    test_aggregate_makes_query_grouped();
    test_empty_grouping_set_grand_total();
    test_silently_truncated_inputs_now_rejected();

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
    test_comma_then_outer_join_nullability();
    test_inner_join_nullability_unchanged();

    // Type coercion
    test_integer_literal_width_by_magnitude();
    test_coercion_numeric_comparison_clean();
    test_coercion_text_int_comparison_warns();
    test_nullif_cross_category_warns();
    test_case_constant_div_by_zero();
    test_constant_integer_overflow();
    test_array_concat_typing();
    test_arithmetic_same_type_nonnumeric_errors();
    test_numeric_real_promotes_double();
    test_group_by_grouping_element_columns();
    test_group_by_grouping_set_key_nullable();
    test_grouping_function_typing();
    test_grouping_without_group_by_rejected();
    test_order_by_grouping_set_key_nullable();
    test_group_by_grouping_set_expression_nullable();
    test_group_by_grouping_set_window_not_nulled();
    test_window_in_where_having_rejected();
    test_quantified_comparison_boolean();
    test_row_in_subquery();
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
    test_temporal_date_diff_reconciles();
    test_temporal_nullable_operand();
    test_interval_scaling_typed();
    test_temporal_wildcard_arith();
    test_temporal_literal_types();
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
