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
#include <utility>
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
    std::string_view text;      // primary_text of the grouping expression
    const ASTNode* node = nullptr;  // the key expression, for whole-expression matching
                                    // (GROUP BY date_trunc(..) / a+b, not just a column)
    // The relation-instance qualifier for a key that resolved to a `*`-expanded
    // column (a positional `GROUP BY 1` into a star). Such a key has no AST node
    // and its `text` is the bare base-column name, so the qualifier that would
    // otherwise disambiguate a self-join instance is not in `text`; it is carried
    // here instead (from the star column's ResolvedColumn::qualifier). Empty for
    // every other key, where the qualifier is read from `text` as before.
    std::string_view instance;
};

class Analyzer {
public:
    explicit Analyzer(const Catalog& catalog) : catalog_(catalog) {}

    // Analyze a statement (root node). Handles SELECT statements (including their
    // CTEs and subqueries), set operations (UNION/INTERSECT/EXCEPT), and the DML
    // statements INSERT / UPDATE / DELETE; other statement kinds are ignored
    // gracefully (no diagnostics, no crash).
    void analyze(ASTNode* root);

    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }
    [[nodiscard]] bool has_errors() const;

    // Inferred type recorded for a node (Unknown if none / not analyzed).
    [[nodiscard]] DataType type_of(const ASTNode* node) const;

    // Inferred nullability recorded for a node, using the parser's 2-bit
    // encoding: 0 = unknown, 1 = not-null, 2 = nullable. Returns 0 for nodes
    // that were not analyzed / carry no nullability.
    [[nodiscard]] int nullability_of(const ASTNode* node) const;

    // Whether the given subquery node (a Subquery / SubqueryExpr used as a
    // scalar, IN, or EXISTS operand) was found to be correlated: it references a
    // column resolved in an enclosing query block. False for uncorrelated
    // subqueries and for nodes that are not analyzed subqueries.
    [[nodiscard]] bool is_correlated(const ASTNode* subquery) const;

    // The resolved output projection of a query block (a SELECT statement or a
    // set-operation node), with `SELECT *` / `table.*` expanded to concrete
    // columns in FROM/JOIN order, and (for set operations) the reconciled
    // column types. Returns nullptr if the node was not analyzed as a query.
    [[nodiscard]] const std::vector<ResolvedColumn>* projection_of(
        const ASTNode* query) const;

    // Infer the type of a standalone scalar expression against a caller-built
    // scope, reusing the same type engine as query analysis. Intended for
    // well-formedness checks that live outside a query block (e.g. a DDL CHECK
    // predicate or a DEFAULT value type-checked against the table's own columns).
    // Records types onto the nodes and any diagnostics into this analyzer.
    [[nodiscard]] DataType infer_scalar(ASTNode* expr, Scope& scope);

    // Whether a value of type `value` may be stored into a target of type
    // `target` under the assignment coercion rules (identical types, a wildcard
    // NULL/Unknown, numeric promotion, string collapse, or the soft numeric<->
    // string conversion). Pure and stateless; emits no diagnostics. Used to
    // type-check a DDL DEFAULT against its column, mirroring INSERT assignment.
    [[nodiscard]] static bool assignment_compatible(DataType target, DataType value);

private:
    // Analyze a statement that yields a row set: a SELECT block or a set
    // operation (UNION/INTERSECT/EXCEPT). Returns the projected columns.
    std::vector<ResolvedColumn> analyze_stmt(ASTNode* node, Scope* parent);

    // DML entry points. Each resolves the target table in the catalog and, for
    // UPDATE/DELETE, brings it into a fresh scope so SET values and the WHERE
    // predicate resolve against it. All conservative: unmodeled shapes degrade
    // rather than crash. See docs/DESIGN.md.
    void analyze_insert(ASTNode* insert_stmt);
    void analyze_update(ASTNode* update_stmt);
    void analyze_delete(ASTNode* delete_stmt);

    // Resolve a base-table reference by name and, when found, add it to `scope`
    // as a relation (columns from the catalog). Emits UnresolvedTable and returns
    // nullptr when the name is not a known table. Used by UPDATE / DELETE.
    const TableInfo* bind_base_table(ASTNode* table_ref, Scope& scope);

    // Type-check assigning a value of type `value` into a target column of type
    // `target` using the coercion model's assignment rules: a clean or numeric/
    // string implicit conversion is accepted (the latter as an ImplicitCoercion
    // warning); anything else is a TypeMismatch error. `at` locates the value.
    void check_assignment(DataType target, DataType value, const ASTNode* at);

    // Flag an explicit NULL literal written into a NOT NULL column (an INSERT
    // value or an UPDATE SET value). A column DEFAULT does not apply here - an
    // explicit value overrides it - so a literal NULL is a definite NOT NULL
    // violation. No-op for a nullable column, a null `col`/`value`, or a value
    // that is not a NULL literal.
    void check_not_null_literal(const ColumnInfo* col, ASTNode* value);

    // Evaluate each of the table's CHECK constraints over one row's constant
    // values and report a definite violation. `target_cols`/`vals` are the
    // columns given a value and those value nodes (positionally aligned); a CHECK
    // is only decided when every column it references is bound to a constant, so
    // the outcome is certain (see check_eval.hpp). With `apply_defaults` (INSERT),
    // a column the row omits is bound to its DEFAULT when that default is a
    // constant, so a default-sourced violation is caught; UPDATE passes false
    // because an unset column keeps its existing value, not its default. `at`
    // locates the row for the diagnostic.
    void check_row_against_checks(const TableInfo& table,
                                  const std::vector<const ColumnInfo*>& target_cols,
                                  const std::vector<ASTNode*>& vals, const ASTNode* at,
                                  bool apply_defaults);

    // Validate a LIMIT / OFFSET clause: any operand that is a literal must be a
    // non-negative integer (negative or non-integer literal -> InvalidLimit).
    // Non-literal operands are skipped.
    void check_limit(ASTNode* limit_clause);

    // Register every CTE in a WITH clause (`cte_clause`) into `scope`, so the
    // following query - or set operation - and later CTEs can reference them.
    // Shared by analyze_query and analyze_setop (a WITH clause may sit above a
    // top-level set operation and is then in scope for every branch).
    void register_ctes(ASTNode* cte_clause, Scope& scope);

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
    // `comma_group_start` is the scope-relation index at which the current
    // comma-separated table_reference began. A JOIN's left operand is that whole
    // group (comma binds looser than JOIN), so an outer join null-supplies only
    // [comma_group_start, ...) - never the relations of a preceding comma item.
    void resolve_from_item(ASTNode* item, Scope& scope,
                           std::size_t comma_group_start = 0);

    // Columns of a VALUES list used as a derived table: one per value position in
    // the first row, typed by inference (nullable, anonymous). A column-alias
    // list supplies the names.
    std::vector<ResolvedColumn> columns_from_values(ASTNode* values_stmt, Scope& scope);

    // Rename a derived table's output columns from its "(a, b, ...)" column-alias
    // list, positionally. More aliases than columns is a ColumnAliasCountMismatch;
    // fewer is allowed (trailing columns keep their inferred names). `at` locates
    // the diagnostic.
    void apply_column_aliases(std::vector<ResolvedColumn>& cols, ASTNode* alias_list,
                              ASTNode* at);

    // Resolve a JOIN's USING (col, ...) columns against the left relations
    // (indices [0, left_end)) and right relations ([left_end, right_end)).
    void resolve_using(ASTNode* using_clause, Scope& scope, std::size_t left_end,
                       std::size_t right_end);

    // Coalesce the columns common to a NATURAL join's left ([0, left_end)) and
    // right ([left_end, right_end)) relations, so a bare reference to a shared
    // column is not ambiguous.
    void resolve_natural(Scope& scope, std::size_t left_end, std::size_t right_end);

    // Resolve a column reference against `scope`, recording type + context and
    // emitting a diagnostic on failure.
    ResolvedColumn resolve_column_ref(ASTNode* col_ref, Scope& scope);

    // Infer and record the type of an expression subtree.
    DataType infer_expr(ASTNode* expr, Scope& scope);

    // Warn (soft, like the CASE WHEN condition check) when a value used in a
    // boolean context - a WHERE / HAVING / ON predicate, or an AND / OR / NOT
    // operand - is neither a boolean nor an untyped wildcard. PostgreSQL rejects
    // these outright; the DB25 dialect models a non-boolean boolean-context as an
    // implicit coercion, but it must not be SILENT (it was, everywhere but CASE
    // WHEN). `what` names the context for the message.
    void warn_boolean_context(DataType t, const ASTNode* node, std::string_view what);

    // Analyze a subquery used in an expression position (scalar / IN / EXISTS):
    // analyze its inner query block under a child scope of `enclosing` so that
    // correlated column references resolve outward, record whether it turned out
    // correlated, and return its projected columns.
    std::vector<ResolvedColumn> analyze_subquery(ASTNode* subquery, Scope& enclosing);

    // GROUP BY / HAVING legality. Called after FROM/SELECT/WHERE have been
    // resolved. Resolves the GROUP BY keys, decides whether the query is
    // "grouped" (a GROUP BY clause is present, or the SELECT list contains an
    // aggregate), and, if so, verifies that every column reference outside an
    // aggregate in the SELECT list / ORDER BY / HAVING is a grouping key and
    // that no aggregate is nested directly inside another aggregate.
    // `output_sources` is aligned 1:1 with `output`: the SELECT-list node that
    // produced each column, or nullptr for a `*`-expanded column. Used to null
    // expression columns that read a ROLLUP/CUBE/GROUPING SETS key.
    void analyze_grouping(ASTNode* select_stmt, ASTNode* group_by, Scope& scope,
                          std::vector<ResolvedColumn>& output,
                          const std::vector<ASTNode*>& output_sources);

    // Validate and type an ORDER BY clause that hangs off `container` (a
    // SelectStmt or a set-operation node). Shared by analyze_query and
    // analyze_setop so a set-op's ORDER BY is validated the same way a plain
    // SELECT's is (positional range check, output-name resolution, type /
    // nullability recording). `output` is the statement's projected columns.
    // `from_scope` is the FROM scope an unresolved key falls back to for a plain
    // SELECT (a column may name an input, not just an output); pass nullptr for a
    // set operation, which has no single FROM scope - an ORDER BY key there may
    // reference only an output column, so an unresolved one is UnresolvedColumn.
    void analyze_order_by(ASTNode* container,
                          const std::vector<ResolvedColumn>& output,
                          Scope* from_scope);

    // Does `expr` reference - OUTSIDE any aggregate call - a column whose
    // (table_id, column_id) is in `keys`? A reference inside an aggregate does
    // NOT count: the aggregate reads the raw input rows, where a grouping-set
    // key still holds its real value; only a bare read of the key is NULL in the
    // super-aggregate (subtotal / grand-total) rows. Used to decide which result
    // columns a ROLLUP/CUBE/GROUPING SETS makes nullable.
    [[nodiscard]] bool expr_reads_grouping_key(
        ASTNode* expr,
        const std::vector<std::pair<std::uint32_t, std::uint32_t>>& keys,
        bool in_aggregate);

    // A GROUP BY key may name a SELECT-list output alias (a Postgres extension):
    // `SELECT id AS x FROM t GROUP BY x` groups by `id`. If `key` is an
    // unqualified column reference that names an output alias AND is NOT itself
    // an input column (in case of ambiguity a GROUP BY name binds to the input
    // column first), return the matching SELECT-list item node; otherwise
    // nullptr. Shared by the resolution pass and the grouping-legality pass so
    // both treat the alias key as grouping the aliased expression.
    [[nodiscard]] ASTNode* group_key_alias_item(ASTNode* key, ASTNode* select_list,
                                                Scope& scope) const;

    // Recursively check an expression subtree for grouping legality:
    //   * a bare column reference not exempt from the grouping rule must be a
    //     grouping key (else NonGroupedColumn);
    //   * an aggregate call inside another aggregate is a NestedAggregate.
    // Two independent contexts are tracked:
    //   * `grouping_exempt` - true while walking beneath an aggregate OR inside a
    //     window function (arguments and OVER clause): such columns need not be
    //     grouping keys.
    //   * `in_aggregate` - true only while inside a plain (non-window) aggregate's
    //     arguments, used solely for nested-aggregate detection. A window boundary
    //     resets it, so an aggregate inside an OVER clause is not "nested".
    void check_grouping_expr(ASTNode* expr, const std::vector<GroupKey>& keys,
                             bool grouping_exempt, bool in_aggregate);

    // Record an inferred type on the node and in the side map.
    void record_type(ASTNode* node, DataType type);

    // Record inferred nullability (0=unknown, 1=not-null, 2=nullable) on the node
    // (context.analysis.nullability) and in the side map.
    void record_nullability(ASTNode* node, int nullability);

    // Read back the recorded nullability of a node (0 if none). Used to combine
    // operand nullabilities when typing a parent expression.
    [[nodiscard]] int null_of(const ASTNode* node) const;

    void add_diagnostic(DiagnosticCode code, std::string message, const ASTNode* at,
                        Severity severity = Severity::Error);

    const Catalog& catalog_;
    std::vector<Diagnostic> diagnostics_;
    // Suppression depth for the constant division/modulo-by-zero warning: raised
    // while analyzing a CASE arm that constant-fold analysis proves unreachable
    // (a constant-false guard, or an arm after a constant-true guard), so a
    // provably-dead `1/0` does not warn. See the CaseExpr / BinaryExpr handlers.
    int div0_suppress_ = 0;
    std::unordered_map<const ASTNode*, DataType> inferred_;
    // Inferred nullability per node (parser 2-bit encoding); side mirror of
    // context.analysis.nullability.
    std::unordered_map<const ASTNode*, int> nullability_;
    // Whether a subquery node resolved a correlated (outer-scope) reference.
    std::unordered_map<const ASTNode*, bool> correlated_;
    // Stack of the currently-active subqueries (outermost first), each paired
    // with its enclosing scope and its correlation flag. A subquery is pushed
    // while its body is analyzed and popped afterward. When a column resolves in
    // an enclosing scope, EVERY active subquery whose enclosing scope is at or
    // outside the resolving (owning) scope is marked correlated - not just the
    // innermost - so an intermediate subquery in a multiply-nested chain is
    // correctly flagged when a reference reaches two-or-more scopes out.
    std::vector<std::pair<const Scope*, bool*>> corr_stack_;
    // Per query-block resolved projection (stars expanded, set-op types
    // reconciled), keyed by the SELECT / set-operation node.
    std::unordered_map<const ASTNode*, std::vector<ResolvedColumn>> projections_;

    // Recursion-depth guard for the expression walkers (infer_expr /
    // check_grouping_expr). SQL operator chains (AND/OR, arithmetic, ...) parse
    // to unbounded left-deep trees that the parser does not depth-limit, so a
    // pathologically long chain would overflow the stack; past the limit the
    // over-deep subtree is abandoned (reported once via ExpressionTooComplex).
    int expr_depth_ = 0;
    bool expr_depth_reported_ = false;
};

}  // namespace db25::semantic
