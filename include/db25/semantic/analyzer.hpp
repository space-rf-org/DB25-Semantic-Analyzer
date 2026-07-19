// DB25 Semantic Analyzer - Analyzer
//
// Walks a parsed DB25 AST and performs name resolution and basic type
// inference. Results are recorded two ways (see docs/DESIGN.md):
//   * on the parser node itself: `data_type` for typed expressions, and the
//     AnalysisContext union (`table_id`, `column_id`, `nullability`) for
//     resolved column references;
//   * in a side map keyed by node pointer, for convenient read-back.
//
// The Parser instance that produced the AST must outlive the Analyzer, because
// node text (primary_text / schema_name) points into parser-owned arena memory.

#pragma once

#include "db25/ast/ast_node.hpp"
#include "db25/semantic/catalog.hpp"
#include "db25/semantic/diagnostic.hpp"
#include "db25/semantic/scope.hpp"

#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace db25::semantic {

using ast::ASTNode;
using ast::DataType;

// Identity of a GROUP BY key, used to decide whether a column reference in the
// SELECT list / ORDER BY / HAVING is a grouping key. Preferred comparison is by
// resolved (table_id, column_id); when a key is unresolved (e.g. a derived
// column or an expression) both ids are 0 and we fall back to comparing the
// reference's (possibly dotted) text.
struct GroupKey {
    std::uint32_t table_id = 0;
    std::uint32_t column_id = 0;
    std::string_view text;  // primary_text of the grouping expression
};

class Analyzer {
public:
    explicit Analyzer(const Catalog& catalog) : catalog_(catalog) {}

    // Analyze a statement (root node). Currently handles SELECT statements,
    // including their CTEs and subqueries; other statement kinds are ignored
    // gracefully (no diagnostics, no crash).
    void analyze(ASTNode* root);

    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }
    [[nodiscard]] bool has_errors() const;

    // Inferred type recorded for a node (Unknown if none / not analyzed).
    [[nodiscard]] DataType type_of(const ASTNode* node) const;

    // The resolved output projection of a query block (a SELECT statement or a
    // set-operation node), with `SELECT *` / `table.*` expanded to concrete
    // columns in FROM/JOIN order, and (for set operations) the reconciled
    // column types. Returns nullptr if the node was not analyzed as a query.
    [[nodiscard]] const std::vector<ResolvedColumn>* projection_of(
        const ASTNode* query) const;

private:
    // Analyze a statement that yields a row set: a SELECT block or a set
    // operation (UNION/INTERSECT/EXCEPT). Returns the projected columns.
    std::vector<ResolvedColumn> analyze_stmt(ASTNode* node, Scope* parent);

    // Analyze a SELECT query block under `parent` scope and return the list of
    // columns it projects (used when the block is a derived table or CTE body).
    std::vector<ResolvedColumn> analyze_query(ASTNode* select_stmt, Scope* parent);

    // Analyze a set-operation node (UNION/INTERSECT/EXCEPT): analyze both
    // branches, check arity + pairwise type compatibility, and return the
    // reconciled output columns.
    std::vector<ResolvedColumn> analyze_setop(ASTNode* setop, Scope* parent);

    // Expand a `Star` select-list item into concrete columns, appending them to
    // `output`. Handles unqualified `*` and qualified `table.*`.
    void expand_star(ASTNode* star, Scope& scope, std::vector<ResolvedColumn>& output);

    // Populate `scope` with the relations named in a FROM clause.
    void resolve_from(ASTNode* from_clause, Scope& scope);
    void resolve_from_item(ASTNode* item, Scope& scope);

    // Resolve a JOIN's USING (col, ...) columns against the left relations
    // (indices [0, left_end)) and right relations ([left_end, right_end)).
    void resolve_using(ASTNode* using_clause, Scope& scope, std::size_t left_end,
                       std::size_t right_end);

    // Resolve a column reference against `scope`, recording type + context and
    // emitting a diagnostic on failure.
    ResolvedColumn resolve_column_ref(ASTNode* col_ref, Scope& scope);

    // Infer and record the type of an expression subtree.
    DataType infer_expr(ASTNode* expr, Scope& scope);

    // GROUP BY / HAVING legality. Called after FROM/SELECT/WHERE have been
    // resolved. Resolves the GROUP BY keys, decides whether the query is
    // "grouped" (a GROUP BY clause is present, or the SELECT list contains an
    // aggregate), and, if so, verifies that every column reference outside an
    // aggregate in the SELECT list / ORDER BY / HAVING is a grouping key and
    // that no aggregate is nested directly inside another aggregate.
    void analyze_grouping(ASTNode* select_stmt, ASTNode* group_by, Scope& scope);

    // Recursively check an expression subtree for grouping legality:
    //   * a bare column reference not under an aggregate must be a grouping key
    //     (else NonGroupedColumn);
    //   * an aggregate call inside another aggregate is a NestedAggregate.
    // `in_aggregate` is true while walking inside an aggregate function call.
    void check_grouping_expr(ASTNode* expr, const std::vector<GroupKey>& keys,
                             bool in_aggregate);

    // Record an inferred type on the node and in the side map.
    void record_type(ASTNode* node, DataType type);

    void add_diagnostic(DiagnosticCode code, std::string message, const ASTNode* at,
                        Severity severity = Severity::Error);

    const Catalog& catalog_;
    std::vector<Diagnostic> diagnostics_;
    std::unordered_map<const ASTNode*, DataType> inferred_;
    // Per query-block resolved projection (stars expanded, set-op types
    // reconciled), keyed by the SELECT / set-operation node.
    std::unordered_map<const ASTNode*, std::vector<ResolvedColumn>> projections_;
};

}  // namespace db25::semantic
