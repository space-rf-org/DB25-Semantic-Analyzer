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

#include <unordered_map>
#include <vector>

namespace db25::semantic {

using ast::ASTNode;
using ast::DataType;

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

private:
    // Analyze a SELECT query block under `parent` scope and return the list of
    // columns it projects (used when the block is a derived table or CTE body).
    std::vector<ResolvedColumn> analyze_query(ASTNode* select_stmt, Scope* parent);

    // Populate `scope` with the relations named in a FROM clause.
    void resolve_from(ASTNode* from_clause, Scope& scope);
    void resolve_from_item(ASTNode* item, Scope& scope);

    // Resolve a column reference against `scope`, recording type + context and
    // emitting a diagnostic on failure.
    ResolvedColumn resolve_column_ref(ASTNode* col_ref, Scope& scope);

    // Infer and record the type of an expression subtree.
    DataType infer_expr(ASTNode* expr, Scope& scope);

    // Record an inferred type on the node and in the side map.
    void record_type(ASTNode* node, DataType type);

    void add_diagnostic(DiagnosticCode code, std::string message, const ASTNode* at);

    const Catalog& catalog_;
    std::vector<Diagnostic> diagnostics_;
    std::unordered_map<const ASTNode*, DataType> inferred_;
};

}  // namespace db25::semantic
