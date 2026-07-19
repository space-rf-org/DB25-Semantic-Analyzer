// DB25 Semantic Analyzer - Analyzer implementation

#include "db25/semantic/analyzer.hpp"

#include "db25/ast/node_types.hpp"
#include "db25/semantic/ast_helpers.hpp"

#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace db25::semantic {

using ast::NodeType;

namespace {

[[nodiscard]] std::string to_upper(std::string_view s) {
    std::string r{s};
    for (char& c : r) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return r;
}

// The set of function names treated as aggregates. Kept as a single table so it
// is trivially extensible: add a name here and it is recognized everywhere
// (grouping legality and result typing).
constexpr std::array<std::string_view, 5> kAggregateNames = {
    "COUNT", "SUM", "AVG", "MIN", "MAX",
};

[[nodiscard]] bool is_aggregate_name(std::string_view upper_name) {
    for (const std::string_view a : kAggregateNames) {
        if (a == upper_name) {
            return true;
        }
    }
    return false;
}

// True if `node` is a column-reference-like leaf. The parser emits ColumnRef
// for a top-level column and Identifier for a bare column that is the direct
// argument of a function call; both carry the (possibly dotted) name in
// primary_text and resolve the same way.
[[nodiscard]] bool is_column_ref_node(NodeType t) {
    return t == NodeType::ColumnRef || t == NodeType::Identifier;
}

[[nodiscard]] bool is_numeric(DataType t) {
    switch (t) {
        case DataType::TinyInt:
        case DataType::SmallInt:
        case DataType::Integer:
        case DataType::BigInt:
        case DataType::Decimal:
        case DataType::Real:
        case DataType::Double:
            return true;
        default:
            return false;
    }
}

// Widen two numeric types to a common arithmetic result type.
[[nodiscard]] DataType promote_numeric(DataType a, DataType b) {
    if (!is_numeric(a) || !is_numeric(b)) {
        return DataType::Unknown;
    }
    auto rank = [](DataType t) -> int {
        switch (t) {
            case DataType::TinyInt: return 1;
            case DataType::SmallInt: return 2;
            case DataType::Integer: return 3;
            case DataType::BigInt: return 4;
            case DataType::Decimal: return 5;
            case DataType::Real: return 6;
            case DataType::Double: return 7;
            default: return 0;
        }
    };
    return rank(a) >= rank(b) ? a : b;
}

[[nodiscard]] bool is_comparison_op(std::string_view op) {
    return op == "=" || op == "==" || op == "<>" || op == "!=" || op == "<" ||
           op == ">" || op == "<=" || op == ">=";
}

[[nodiscard]] bool is_logical_op(std::string_view op) {
    return op == "AND" || op == "OR" || op == "and" || op == "or";
}

[[nodiscard]] bool is_arithmetic_op(std::string_view op) {
    return op == "+" || op == "-" || op == "*" || op == "/" || op == "%";
}

[[nodiscard]] bool is_setop(NodeType t) {
    return t == NodeType::UnionStmt || t == NodeType::IntersectStmt ||
           t == NodeType::ExceptStmt;
}

// A FROM/JOIN child that contributes relations to the scope (as opposed to a
// join predicate such as an ON expression or a USING clause).
[[nodiscard]] bool is_relation_node(NodeType t) {
    switch (t) {
        case NodeType::TableRef:
        case NodeType::Subquery:
        case NodeType::SubqueryExpr:
        case NodeType::JoinClause:
        case NodeType::InnerJoin:
        case NodeType::LeftJoin:
        case NodeType::RightJoin:
        case NodeType::FullJoin:
        case NodeType::CrossJoin:
        case NodeType::LateralJoin:
            return true;
        default:
            return false;
    }
}

// Reconcile two column types for a set operation. `ok` is false only when the
// types are genuinely incompatible; `type` is the unified result otherwise.
// Conservative: exact match, NULL/Unknown unify with anything, and any two
// numeric types unify by numeric promotion. Everything else is incompatible.
struct TypeReconcile {
    bool ok;
    DataType type;
};

[[nodiscard]] TypeReconcile reconcile_types(DataType a, DataType b) {
    if (a == b) {
        return {true, a};
    }
    if (a == DataType::Null || a == DataType::Unknown) {
        return {true, b};
    }
    if (b == DataType::Null || b == DataType::Unknown) {
        return {true, a};
    }
    if (is_numeric(a) && is_numeric(b)) {
        return {true, promote_numeric(a, b)};
    }
    return {false, DataType::Unknown};
}

// SUM widens integer inputs to BigInt (avoids overflow of the accumulator);
// exact/approximate numerics keep their kind. Non-numeric inputs pass through
// unchanged (degrading conservatively rather than guessing).
[[nodiscard]] DataType sum_result(DataType arg) {
    switch (arg) {
        case DataType::TinyInt:
        case DataType::SmallInt:
        case DataType::Integer:
        case DataType::BigInt:
            return DataType::BigInt;
        default:
            return arg;
    }
}

struct FunctionType {
    DataType type;
    bool known;  // false => name not in the signature table (soft-diagnose)
};

// Result type for a function call given its (already inferred) argument types.
// Aggregates and a small, extensible table of common scalar functions are
// modeled; anything else degrades to Unknown with known=false.
[[nodiscard]] FunctionType function_result_type(std::string_view upper_name,
                                                const std::vector<DataType>& args) {
    const DataType arg0 = args.empty() ? DataType::Unknown : args.front();

    if (is_aggregate_name(upper_name)) {
        if (upper_name == "COUNT") {
            return {DataType::BigInt, true};  // COUNT(*) / COUNT(expr)
        }
        if (upper_name == "SUM") {
            return {sum_result(arg0), true};
        }
        if (upper_name == "AVG") {
            return {DataType::Double, true};
        }
        // MIN / MAX preserve the argument type.
        return {arg0, true};
    }

    // Scalar function signature table (extend here as needed).
    if (upper_name == "UPPER" || upper_name == "LOWER") {
        return {DataType::Text, true};
    }
    if (upper_name == "LENGTH") {
        return {DataType::Integer, true};
    }
    if (upper_name == "ABS") {
        return {is_numeric(arg0) ? arg0 : DataType::Unknown, true};
    }
    if (upper_name == "COALESCE") {
        DataType unified = DataType::Unknown;
        for (const DataType a : args) {
            unified = reconcile_types(unified, a).type;
        }
        return {unified, true};
    }
    return {DataType::Unknown, false};
}

}  // namespace

void Analyzer::analyze(ASTNode* root) {
    if (root == nullptr) {
        return;
    }
    if (root->node_type == NodeType::SelectStmt || is_setop(root->node_type)) {
        analyze_stmt(root, nullptr);
    }
    // Other statement kinds are not yet analyzed; see docs/DESIGN.md roadmap.
}

std::vector<ResolvedColumn> Analyzer::analyze_stmt(ASTNode* node, Scope* parent) {
    if (node == nullptr) {
        return {};
    }
    if (is_setop(node->node_type)) {
        return analyze_setop(node, parent);
    }
    if (node->node_type == NodeType::SelectStmt) {
        return analyze_query(node, parent);
    }
    return {};
}

const std::vector<ResolvedColumn>* Analyzer::projection_of(const ASTNode* query) const {
    const auto it = projections_.find(query);
    return it == projections_.end() ? nullptr : &it->second;
}

bool Analyzer::has_errors() const {
    for (const auto& d : diagnostics_) {
        if (d.severity == Severity::Error) {
            return true;
        }
    }
    return false;
}

DataType Analyzer::type_of(const ASTNode* node) const {
    const auto it = inferred_.find(node);
    return it == inferred_.end() ? DataType::Unknown : it->second;
}

void Analyzer::record_type(ASTNode* node, DataType type) {
    if (node == nullptr) {
        return;
    }
    inferred_[node] = type;
    node->data_type = type;  // mirror onto the parser node
}

void Analyzer::add_diagnostic(DiagnosticCode code, std::string message, const ASTNode* at,
                              Severity severity) {
    Diagnostic d;
    d.severity = severity;
    d.code = code;
    d.message = std::move(message);
    if (at != nullptr) {
        d.source_start = at->source_start;
        d.source_end = at->source_end;
    }
    diagnostics_.push_back(std::move(d));
}

std::vector<ResolvedColumn> Analyzer::analyze_query(ASTNode* select_stmt, Scope* parent) {
    Scope scope(parent);

    // 1. CTEs: register each definition as a named relation before resolving
    //    FROM, so the body can reference the current scope and earlier CTEs.
    if (ASTNode* cte_clause = find_child(select_stmt, NodeType::CTEClause)) {
        for (ASTNode* def = first_child(cte_clause); def != nullptr; def = def->next_sibling) {
            if (def->node_type != NodeType::CTEDefinition) {
                continue;
            }
            ASTNode* body = find_child(def, NodeType::SelectStmt);
            NamedRelation cte;
            cte.name = std::string{def->primary_text};
            if (body != nullptr) {
                cte.columns = analyze_query(body, &scope);
            }
            scope.add_cte(std::move(cte));
        }
    }

    // 2. FROM: populate the scope with visible relations.
    if (ASTNode* from = find_child(select_stmt, NodeType::FromClause)) {
        resolve_from(from, scope);
    }

    // 3. SELECT list: resolve/ infer each projected item and build the output
    //    column list (used when this block is a derived table or CTE body).
    std::vector<ResolvedColumn> output;
    if (ASTNode* select_list = find_child(select_stmt, NodeType::SelectList)) {
        for (ASTNode* item = first_child(select_list); item != nullptr;
             item = item->next_sibling) {
            if (item->node_type == NodeType::Star) {
                // `*` / `table.*` -> concrete columns in FROM/JOIN order.
                expand_star(item, scope, output);
                continue;
            }
            const DataType type = infer_expr(item, scope);
            ResolvedColumn out;
            const std::string_view alias = alias_of(item);
            if (!alias.empty()) {
                out.name = std::string{alias};
            } else if (item->node_type == NodeType::ColumnRef) {
                out.name = std::string{split_column_ref(item->primary_text).column};
            } else {
                out.name = std::string{item->primary_text};
            }
            out.type = type;
            output.push_back(std::move(out));
        }
    }

    // 4. WHERE / HAVING: infer the predicate expression (drives type checking
    //    and column resolution inside the predicate).
    if (ASTNode* where = find_child(select_stmt, NodeType::WhereClause)) {
        if (ASTNode* pred = first_child(where)) {
            infer_expr(pred, scope);
        }
    }
    // 5. GROUP BY: resolve each grouping expression against the FROM scope
    //    (emitting the usual unresolved diagnostics) before legality checking.
    ASTNode* group_by = find_child(select_stmt, NodeType::GroupByClause);
    if (group_by != nullptr) {
        for (ASTNode* key = first_child(group_by); key != nullptr; key = key->next_sibling) {
            infer_expr(key, scope);
        }
    }

    // 6. HAVING: resolve/infer the predicate (column refs resolve here too).
    if (ASTNode* having = find_child(select_stmt, NodeType::HavingClause)) {
        if (ASTNode* pred = first_child(having)) {
            infer_expr(pred, scope);
        }
    }

    // 7. ORDER BY: resolve each sort expression against the FROM scope.
    if (ASTNode* order_by = find_child(select_stmt, NodeType::OrderByClause)) {
        for (ASTNode* item = first_child(order_by); item != nullptr; item = item->next_sibling) {
            infer_expr(item, scope);
        }
    }

    // 8. Aggregate / GROUP BY / HAVING legality (validation pass).
    analyze_grouping(select_stmt, group_by, scope);

    projections_[select_stmt] = output;
    return output;
}

void Analyzer::expand_star(ASTNode* star, Scope& scope,
                           std::vector<ResolvedColumn>& output) {
    // A qualified star `table.*` carries the qualifier in schema_name; an
    // unqualified `*` has an empty schema_name.
    //
    // NOTE (parser limitation): the consumed parser build does not currently
    // produce a qualified-star node in this shape - it misparses `table.*` as a
    // multiplication expression (dropping the FROM clause) or, before a comma,
    // as a bare column reference. The qualified-star path below is written
    // against the correct/forward-compatible shape (a Star node whose
    // schema_name holds the qualifier) so it takes effect unchanged once the
    // parser emits it. See docs/DESIGN.md.
    const std::string_view qualifier = alias_of(star);

    if (qualifier.empty()) {
        if (scope.relations().empty()) {
            add_diagnostic(DiagnosticCode::StarWithoutFrom,
                           "'SELECT *' requires a FROM clause", star);
            return;
        }
        for (const auto& rel : scope.relations()) {
            for (const auto& col : rel.columns) {
                output.push_back(col);
            }
        }
        return;
    }

    bool matched = false;
    for (const auto& rel : scope.relations()) {
        if (rel.matches_qualifier(qualifier)) {
            matched = true;
            for (const auto& col : rel.columns) {
                output.push_back(col);
            }
        }
    }
    if (!matched) {
        add_diagnostic(DiagnosticCode::UnresolvedQualifier,
                       "unresolved table qualifier '" + std::string{qualifier} +
                           "' in '" + std::string{qualifier} + ".*'",
                       star);
    }
}

std::vector<ResolvedColumn> Analyzer::analyze_setop(ASTNode* setop, Scope* parent) {
    // Analyze every branch (each a SELECT block or a nested set operation) and
    // collect its projected columns.
    std::vector<std::vector<ResolvedColumn>> branches;
    for (ASTNode* child = first_child(setop); child != nullptr;
         child = child->next_sibling) {
        if (child->node_type == NodeType::SelectStmt || is_setop(child->node_type)) {
            branches.push_back(analyze_stmt(child, parent));
        }
    }

    if (branches.empty()) {
        return {};
    }

    // Fold the branches left-to-right, reconciling arity and types pairwise.
    std::vector<ResolvedColumn> result = branches.front();
    for (std::size_t i = 1; i < branches.size(); ++i) {
        const auto& branch = branches[i];
        if (branch.size() != result.size()) {
            add_diagnostic(DiagnosticCode::SetOpArityMismatch,
                           "set operation branches have different column counts (" +
                               std::to_string(result.size()) + " vs " +
                               std::to_string(branch.size()) + ")",
                           setop);
            continue;  // arity differs: pairwise type check is meaningless
        }
        for (std::size_t j = 0; j < result.size(); ++j) {
            const TypeReconcile r = reconcile_types(result[j].type, branch[j].type);
            if (!r.ok) {
                add_diagnostic(DiagnosticCode::SetOpTypeMismatch,
                               "incompatible types in set operation for output column " +
                                   std::to_string(j + 1),
                               setop);
            }
            result[j].type = r.type;      // reconciled output type
            result[j].nullable = result[j].nullable || branch[j].nullable;
        }
    }

    projections_[setop] = result;
    return result;
}

void Analyzer::resolve_from(ASTNode* from_clause, Scope& scope) {
    for (ASTNode* item = first_child(from_clause); item != nullptr; item = item->next_sibling) {
        resolve_from_item(item, scope);
    }
}

void Analyzer::resolve_from_item(ASTNode* item, Scope& scope) {
    if (item == nullptr) {
        return;
    }
    switch (item->node_type) {
        case NodeType::TableRef: {
            const std::string_view name = item->primary_text;
            const std::string_view alias = alias_of(item);

            RelationBinding binding;
            binding.name = std::string{name};
            binding.alias = std::string{alias};

            // A FROM name may refer to a CTE before a base table.
            if (const NamedRelation* cte = scope.find_cte(name)) {
                binding.columns = cte->columns;
                scope.add_relation(std::move(binding));
                return;
            }
            if (const TableInfo* table = catalog_.find_table(name)) {
                binding.table_id = table->table_id;
                for (const auto& c : table->columns) {
                    binding.columns.push_back(
                        ResolvedColumn{c.name, c.type, c.nullable, table->table_id, c.column_id});
                }
                scope.add_relation(std::move(binding));
                return;
            }
            add_diagnostic(DiagnosticCode::UnresolvedTable,
                           "unresolved table '" + std::string{name} + "'", item);
            return;
        }
        case NodeType::Subquery:
        case NodeType::SubqueryExpr: {
            const std::string_view alias = alias_of(item);
            RelationBinding binding;
            binding.name = std::string{alias};
            binding.alias = std::string{alias};
            if (ASTNode* body = find_child(item, NodeType::SelectStmt)) {
                binding.columns = analyze_query(body, &scope);
            }
            scope.add_relation(std::move(binding));
            return;
        }
        // Joins: bring every relation on the right side into scope, then
        // resolve the join predicate now that both sides are visible. The left
        // side is the FROM-clause sibling processed before this join, so it is
        // already in scope.
        case NodeType::JoinClause:
        case NodeType::InnerJoin:
        case NodeType::LeftJoin:
        case NodeType::RightJoin:
        case NodeType::FullJoin:
        case NodeType::CrossJoin:
        case NodeType::LateralJoin: {
            const std::size_t left_end = scope.relations().size();
            // Pass 1: make the right-hand relation(s) visible.
            for (ASTNode* child = first_child(item); child != nullptr;
                 child = child->next_sibling) {
                if (is_relation_node(child->node_type)) {
                    resolve_from_item(child, scope);
                }
            }
            const std::size_t right_end = scope.relations().size();
            // Pass 2: resolve the ON expression / USING columns against the now
            // fully-populated scope.
            for (ASTNode* child = first_child(item); child != nullptr;
                 child = child->next_sibling) {
                if (child->node_type == NodeType::UsingClause) {
                    resolve_using(child, scope, left_end, right_end);
                } else if (!is_relation_node(child->node_type)) {
                    // The ON predicate: a normal expression whose column refs
                    // resolve (and emit diagnostics) through infer_expr.
                    infer_expr(child, scope);
                }
            }
            return;
        }
        default:
            // Not a relation-producing node (e.g. a join predicate); ignore.
            return;
    }
}

void Analyzer::resolve_using(ASTNode* using_clause, Scope& scope,
                             std::size_t left_end, std::size_t right_end) {
    const auto& relations = scope.relations();
    for (ASTNode* col = first_child(using_clause); col != nullptr;
         col = col->next_sibling) {
        // A USING item names a bare column; primary_text is the column name.
        const std::string_view name = split_column_ref(col->primary_text).column;

        const ResolvedColumn* left_hit = nullptr;
        for (std::size_t i = 0; i < left_end && i < relations.size(); ++i) {
            if (const auto* c = relations[i].find_column(name)) {
                left_hit = c;
            }
        }
        bool right_found = false;
        for (std::size_t i = left_end; i < right_end && i < relations.size(); ++i) {
            if (relations[i].find_column(name) != nullptr) {
                right_found = true;
            }
        }

        if (left_hit == nullptr || !right_found) {
            add_diagnostic(DiagnosticCode::UsingColumnMissing,
                           "USING column '" + std::string{name} +
                               "' is not present in both joined relations",
                           col);
            continue;
        }
        // A USING column collapses to a single merged output column; record its
        // resolved type on the node so downstream consumers see it.
        record_type(col, left_hit->type);
        col->context.analysis.table_id = left_hit->table_id;
        col->context.analysis.column_id = left_hit->column_id;
    }
}

ResolvedColumn Analyzer::resolve_column_ref(ASTNode* col_ref, Scope& scope) {
    const QualifiedRef qref = split_column_ref(col_ref->primary_text);

    ColumnResolution res;
    if (!qref.qualifier.empty()) {
        res = scope.resolve_qualified(qref.qualifier, qref.column);
    } else {
        res = scope.resolve_bare(qref.column);
    }

    if (res.found) {
        // Record resolution onto the parser node's analysis context.
        record_type(col_ref, res.column.type);
        col_ref->context.analysis.table_id = res.column.table_id;
        col_ref->context.analysis.column_id = res.column.column_id;
        col_ref->context.analysis.nullability = res.column.nullable ? 2 : 1;
        return res.column;
    }

    if (res.ambiguous) {
        add_diagnostic(DiagnosticCode::AmbiguousColumn,
                       "ambiguous column '" + std::string{qref.column} + "'", col_ref);
    } else if (res.bad_qualifier) {
        add_diagnostic(DiagnosticCode::UnresolvedQualifier,
                       "unresolved table alias '" + std::string{qref.qualifier} + "'", col_ref);
    } else {
        add_diagnostic(DiagnosticCode::UnresolvedColumn,
                       "unresolved column '" + std::string{col_ref->primary_text} + "'", col_ref);
    }
    return ResolvedColumn{};
}

DataType Analyzer::infer_expr(ASTNode* expr, Scope& scope) {
    if (expr == nullptr) {
        return DataType::Unknown;
    }

    switch (expr->node_type) {
        case NodeType::IntegerLiteral:
            record_type(expr, DataType::Integer);
            return DataType::Integer;
        case NodeType::FloatLiteral:
            record_type(expr, DataType::Double);
            return DataType::Double;
        case NodeType::StringLiteral:
            record_type(expr, DataType::Text);
            return DataType::Text;
        case NodeType::BooleanLiteral:
            record_type(expr, DataType::Boolean);
            return DataType::Boolean;
        case NodeType::NullLiteral:
            record_type(expr, DataType::Null);
            return DataType::Null;

        case NodeType::ColumnRef:
        // A bare column that is the direct argument of a function call is emitted
        // as an Identifier; resolve it the same way as a ColumnRef.
        case NodeType::Identifier:
            return resolve_column_ref(expr, scope).type;

        case NodeType::FunctionCall:
        case NodeType::FunctionExpr: {
            // Infer argument types (recursion also resolves nested column refs).
            // A COUNT(*) star argument contributes no type and is skipped.
            std::vector<DataType> arg_types;
            for (ASTNode* arg = first_child(expr); arg != nullptr; arg = arg->next_sibling) {
                const DataType t = infer_expr(arg, scope);
                if (arg->node_type != NodeType::Star) {
                    arg_types.push_back(t);
                }
            }
            const std::string upper = to_upper(expr->primary_text);
            const FunctionType ft = function_result_type(upper, arg_types);
            if (!ft.known) {
                add_diagnostic(DiagnosticCode::UnknownFunction,
                               "unknown function '" + std::string{expr->primary_text} +
                                   "'; result type is Unknown",
                               expr, Severity::Warning);
            }
            record_type(expr, ft.type);
            return ft.type;
        }

        case NodeType::BinaryExpr: {
            ASTNode* lhs = first_child(expr);
            ASTNode* rhs = lhs != nullptr ? lhs->next_sibling : nullptr;
            const DataType lt = infer_expr(lhs, scope);
            const DataType rt = infer_expr(rhs, scope);
            const std::string_view op = expr->primary_text;

            DataType result = DataType::Unknown;
            if (is_comparison_op(op) || is_logical_op(op)) {
                result = DataType::Boolean;
            } else if (is_arithmetic_op(op)) {
                result = promote_numeric(lt, rt);
            }
            record_type(expr, result);
            return result;
        }

        case NodeType::UnaryExpr: {
            ASTNode* operand = first_child(expr);
            const DataType ot = infer_expr(operand, scope);
            const std::string_view op = expr->primary_text;
            const DataType result =
                (op == "NOT" || op == "not") ? DataType::Boolean : ot;
            record_type(expr, result);
            return result;
        }

        default: {
            // Unknown expression form: recurse so nested column refs still get
            // resolved, but do not claim a type.
            for (ASTNode* c = first_child(expr); c != nullptr; c = c->next_sibling) {
                infer_expr(c, scope);
            }
            return DataType::Unknown;
        }
    }
}

namespace {

// Does the subtree rooted at `node` contain an aggregate function call?
[[nodiscard]] bool contains_aggregate(const ASTNode* node) {
    if (node == nullptr) {
        return false;
    }
    if (node->node_type == NodeType::FunctionCall ||
        node->node_type == NodeType::FunctionExpr) {
        if (is_aggregate_name(to_upper(node->primary_text))) {
            return true;
        }
    }
    for (const ASTNode* c = node->first_child; c != nullptr; c = c->next_sibling) {
        if (contains_aggregate(c)) {
            return true;
        }
    }
    return false;
}

// Does a column reference `ref` match the grouping key `k`? Prefer resolved
// (table_id, column_id) identity; fall back to the reference text when either
// side is unresolved (e.g. derived columns or expression keys).
[[nodiscard]] bool key_matches(const GroupKey& k, const ASTNode* ref) {
    const std::uint32_t tid = ref->context.analysis.table_id;
    const std::uint32_t cid = ref->context.analysis.column_id;
    if (cid != 0 && k.column_id != 0) {
        return tid == k.table_id && cid == k.column_id;
    }
    return ref->primary_text == k.text;
}

}  // namespace

void Analyzer::analyze_grouping(ASTNode* select_stmt, ASTNode* group_by, Scope& scope) {
    (void)scope;  // resolution already happened; this pass is structural.

    ASTNode* select_list = find_child(select_stmt, NodeType::SelectList);

    // The query is "grouped" if it has a GROUP BY clause or any aggregate in the
    // SELECT list. Only grouped queries are subject to the legality rules.
    bool grouped = group_by != nullptr;
    if (!grouped && select_list != nullptr) {
        for (ASTNode* item = first_child(select_list); item != nullptr;
             item = item->next_sibling) {
            if (contains_aggregate(item)) {
                grouped = true;
                break;
            }
        }
    }
    if (!grouped) {
        return;
    }

    // Collect the grouping keys (their resolved identity + text).
    std::vector<GroupKey> keys;
    if (group_by != nullptr) {
        for (ASTNode* key = first_child(group_by); key != nullptr; key = key->next_sibling) {
            GroupKey k;
            k.table_id = key->context.analysis.table_id;
            k.column_id = key->context.analysis.column_id;
            k.text = key->primary_text;
            keys.push_back(k);
        }
    }

    // SELECT list and ORDER BY: every non-aggregated column must be a group key.
    if (select_list != nullptr) {
        for (ASTNode* item = first_child(select_list); item != nullptr;
             item = item->next_sibling) {
            check_grouping_expr(item, keys, /*in_aggregate=*/false);
        }
    }
    if (ASTNode* order_by = find_child(select_stmt, NodeType::OrderByClause)) {
        for (ASTNode* item = first_child(order_by); item != nullptr; item = item->next_sibling) {
            check_grouping_expr(item, keys, /*in_aggregate=*/false);
        }
    }
    // HAVING may reference grouping keys and aggregates; a bare non-grouped
    // column outside an aggregate is illegal here too.
    if (ASTNode* having = find_child(select_stmt, NodeType::HavingClause)) {
        if (ASTNode* pred = first_child(having)) {
            check_grouping_expr(pred, keys, /*in_aggregate=*/false);
        }
    }
}

void Analyzer::check_grouping_expr(ASTNode* expr, const std::vector<GroupKey>& keys,
                                   bool in_aggregate) {
    if (expr == nullptr) {
        return;
    }

    if (expr->node_type == NodeType::FunctionCall ||
        expr->node_type == NodeType::FunctionExpr) {
        const bool is_agg = is_aggregate_name(to_upper(expr->primary_text));
        if (is_agg && in_aggregate) {
            add_diagnostic(DiagnosticCode::NestedAggregate,
                           "aggregate function '" + std::string{expr->primary_text} +
                               "' nested inside another aggregate",
                           expr);
        }
        // Columns beneath an aggregate are exempt from the grouping rule.
        const bool inside = in_aggregate || is_agg;
        for (ASTNode* c = first_child(expr); c != nullptr; c = c->next_sibling) {
            check_grouping_expr(c, keys, inside);
        }
        return;
    }

    if (is_column_ref_node(expr->node_type)) {
        if (!in_aggregate) {
            bool ok = false;
            for (const GroupKey& k : keys) {
                if (key_matches(k, expr)) {
                    ok = true;
                    break;
                }
            }
            if (!ok) {
                add_diagnostic(DiagnosticCode::NonGroupedColumn,
                               "column '" + std::string{expr->primary_text} +
                                   "' must appear in the GROUP BY clause or be used "
                                   "in an aggregate function",
                               expr);
            }
        }
        return;
    }

    for (ASTNode* c = first_child(expr); c != nullptr; c = c->next_sibling) {
        check_grouping_expr(c, keys, in_aggregate);
    }
}

}  // namespace db25::semantic
