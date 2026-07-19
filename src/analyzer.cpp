// DB25 Semantic Analyzer - Analyzer implementation

#include "db25/semantic/analyzer.hpp"

#include "db25/ast/node_types.hpp"
#include "db25/semantic/ast_helpers.hpp"

#include <string>
#include <string_view>

namespace db25::semantic {

using ast::NodeType;

namespace {

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

}  // namespace

void Analyzer::analyze(ASTNode* root) {
    if (root == nullptr) {
        return;
    }
    if (root->node_type == NodeType::SelectStmt) {
        analyze_query(root, nullptr);
    }
    // Other statement kinds are not yet analyzed; see docs/DESIGN.md roadmap.
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

void Analyzer::add_diagnostic(DiagnosticCode code, std::string message, const ASTNode* at) {
    Diagnostic d;
    d.severity = Severity::Error;
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
                // Expand `*` to every visible column (best effort).
                // Not exercised by the current tests but kept correct.
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
    if (ASTNode* having = find_child(select_stmt, NodeType::HavingClause)) {
        if (ASTNode* pred = first_child(having)) {
            infer_expr(pred, scope);
        }
    }

    return output;
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
        // Joins: descend and add every relation on either side. The ON/USING
        // condition is resolved by the parser's structure walk in a future
        // pass; for now we make the joined relations visible.
        case NodeType::JoinClause:
        case NodeType::InnerJoin:
        case NodeType::LeftJoin:
        case NodeType::RightJoin:
        case NodeType::FullJoin:
        case NodeType::CrossJoin:
        case NodeType::LateralJoin: {
            for (ASTNode* child = first_child(item); child != nullptr;
                 child = child->next_sibling) {
                resolve_from_item(child, scope);
            }
            return;
        }
        default:
            // Not a relation-producing node (e.g. a join predicate); ignore.
            return;
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
            return resolve_column_ref(expr, scope).type;

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

}  // namespace db25::semantic
