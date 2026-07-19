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

}  // namespace

int main() {
    test_select_resolves_clean();
    test_unresolved_column();
    test_alias_resolution();
    test_derived_table();
    test_where_type_inference();
    test_cte_resolution();
    test_unresolved_table();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
