// DB25 Semantic Analyzer - DDL utility dispatch (implementation).

#include "db25/semantic/ddl.hpp"
#include "db25/semantic/analyzer.hpp"
#include "db25/semantic/catalog.hpp"
#include "db25/semantic/scope.hpp"
#include "db25/semantic/type_names.hpp"

#include "db25/ast/node_types.hpp"

#include <string>
#include <vector>

namespace db25::semantic {

using ast::ASTNode;
using ast::DataType;
using ast::NodeType;

namespace {

// DROP object-type / IF-EXISTS / CREATE IF-NOT-EXISTS flags, as set by the
// parser on the statement node's semantic_flags.
constexpr std::uint16_t kFlagIfExistsOrNotExists = 0x01;  // IF [NOT] EXISTS
constexpr std::uint16_t kFlagUnique = 0x02;               // CREATE UNIQUE INDEX
constexpr std::uint16_t kFlagCascade = 0x04;              // DROP ... CASCADE
constexpr std::uint16_t kFlagDropTable = 0x10;            // DROP TABLE
constexpr std::uint16_t kFlagDropIndex = 0x20;            // DROP INDEX

// ALTER TABLE DROP COLUMN CASCADE flag, set by the parser on the
// AlterTableAction node. When unset the drop is RESTRICT (the default); the
// parser's separate RESTRICT bit (0x02) needs no handling since it is the
// default behavior.
constexpr std::uint16_t kAlterActionCascade = 0x01;      // DROP COLUMN ... CASCADE
constexpr std::uint16_t kAlterActionDropDefault = 0x04;  // ALTER COLUMN DROP DEFAULT
constexpr std::uint16_t kAlterActionSetNotNull = 0x08;   // ALTER COLUMN SET NOT NULL
constexpr std::uint16_t kAlterActionDropNotNull = 0x10;  // ALTER COLUMN DROP NOT NULL

// Locate the single DDL statement in a (possibly wrapping) parse tree. Returns
// a mutable node: layer-1 type checking annotates the AST with inferred types.
ASTNode* find_ddl(ASTNode* n) {
    if (n == nullptr) return nullptr;
    if (n->node_type == NodeType::CreateTableStmt ||
        n->node_type == NodeType::CreateIndexStmt ||
        n->node_type == NodeType::AlterTableStmt ||
        n->node_type == NodeType::DropStmt) {
        return n;
    }
    for (ASTNode* c = n->first_child; c != nullptr; c = c->next_sibling) {
        if (ASTNode* h = find_ddl(c)) return h;
    }
    return nullptr;
}

const ASTNode* first_child_of_type(const ASTNode* n, NodeType t) {
    for (const ASTNode* c = n->first_child; c != nullptr; c = c->next_sibling) {
        if (c->node_type == t) return c;
    }
    return nullptr;
}

struct ColFlags {
    bool not_null = false;
    bool primary_key = false;
    bool has_default = false;
    std::string default_expr;  // verbatim DEFAULT text; empty when has_default is false
};

// Collect every column reference in an expression subtree (a bare column name
// is a ColumnRef or, depending on parse context, an Identifier). ColumnRef /
// Identifier nodes are treated as leaves; a function call keeps its name in
// primary_text (not a child), so its arguments are still visited.
void collect_column_refs(const ASTNode* n, std::vector<std::string>& out) {
    if (n == nullptr) return;
    if (n->node_type == NodeType::ColumnRef || n->node_type == NodeType::Identifier) {
        out.emplace_back(n->primary_text);
        return;
    }
    for (const ASTNode* c = n->first_child; c != nullptr; c = c->next_sibling) {
        collect_column_refs(c, out);
    }
}

// Collect referenced column names from a node's Identifier children.
void collect_ref_columns(const ASTNode* n, std::vector<std::string>& out) {
    for (const ASTNode* c = n->first_child; c != nullptr; c = c->next_sibling) {
        if (c->node_type == NodeType::Identifier) out.emplace_back(c->primary_text);
    }
}

// Extract every foreign key (column-level and table-level) from a CREATE TABLE
// node into by-name specs the catalog manager resolves at commit.
std::vector<CatalogManager::ForeignKeySpec> collect_foreign_keys(const ASTNode* create_stmt) {
    std::vector<CatalogManager::ForeignKeySpec> out;
    for (const ASTNode* c = create_stmt->first_child; c != nullptr; c = c->next_sibling) {
        if (c->node_type == NodeType::ColumnDefinition) {
            // Column-level FK: `col T REFERENCES t (cols)`. Local column is this
            // column; the FK node carries the referenced table + columns.
            for (const ASTNode* k = c->first_child; k != nullptr; k = k->next_sibling) {
                if (k->node_type != NodeType::ForeignKeyConstraint) continue;
                CatalogManager::ForeignKeySpec fk;
                fk.columns.emplace_back(c->primary_text);
                fk.ref_table = std::string(k->primary_text);
                collect_ref_columns(k, fk.ref_columns);
                out.push_back(std::move(fk));
            }
        } else if (c->node_type == NodeType::ForeignKeyConstraint) {
            // Table-level FK: local columns are the Identifier children; the
            // ReferencesClause child carries the referenced table + columns.
            CatalogManager::ForeignKeySpec fk;
            const ASTNode* ref = nullptr;
            for (const ASTNode* ch = c->first_child; ch != nullptr; ch = ch->next_sibling) {
                if (ch->node_type == NodeType::ReferencesClause) {
                    ref = ch;
                } else if (ch->node_type == NodeType::Identifier) {
                    fk.columns.emplace_back(ch->primary_text);
                }
            }
            if (ref != nullptr) {
                fk.ref_table = std::string(ref->primary_text);
                collect_ref_columns(ref, fk.ref_columns);
            }
            out.push_back(std::move(fk));
        }
    }
    return out;
}

ColFlags column_flags(const ASTNode* coldef) {
    ColFlags f;
    for (const ASTNode* c = coldef->first_child; c != nullptr; c = c->next_sibling) {
        if (c->node_type == NodeType::ColumnConstraint && c->primary_text == "NOT_NULL") {
            f.not_null = true;
        } else if (c->node_type == NodeType::PrimaryKeyConstraint) {
            f.primary_key = true;  // column-level PRIMARY KEY
        } else if (c->node_type == NodeType::DefaultClause) {
            f.has_default = true;
            // The parser stores the DEFAULT expression's verbatim source text on
            // the clause's primary_text (e.g. "0", "now()"). Persist it so the
            // catalog can reproduce the default faithfully.
            f.default_expr = std::string(c->primary_text);
        }
    }
    return f;
}

// Build the catalog ColumnInfo for a ColumnDefinition node: name, resolved type,
// nullability (PRIMARY KEY implies NOT NULL), and any DEFAULT text. The
// column_id is left 0 here; the catalog assigns it at commit. Shared by CREATE
// TABLE and ALTER TABLE ADD COLUMN.
ColumnInfo build_column_info(const ASTNode* coldef) {
    ColumnInfo ci;
    ci.name = std::string(coldef->primary_text);
    const ASTNode* type_node = first_child_of_type(coldef, NodeType::DataTypeNode);
    ci.type = (type_node != nullptr) ? data_type_from_name(type_node->primary_text)
                                     : DataType::Unknown;
    ColFlags f = column_flags(coldef);
    ci.nullable = !(f.not_null || f.primary_key);
    ci.has_default = f.has_default;
    ci.default_expr = std::move(f.default_expr);
    return ci;
}

// Extract every CHECK constraint (column-level and table-level) from a CREATE
// TABLE node into by-name specs the catalog manager resolves at commit. The
// expression's verbatim text is on the CheckConstraint node's primary_text; the
// referenced local columns are collected from the expression subtree.
std::vector<CatalogManager::CheckSpec> collect_checks(const ASTNode* create_stmt) {
    std::vector<CatalogManager::CheckSpec> out;
    auto emit = [&](const ASTNode* check) {
        CatalogManager::CheckSpec spec;
        spec.expr = std::string(check->primary_text);
        collect_column_refs(check, spec.columns);
        out.push_back(std::move(spec));
    };
    for (const ASTNode* c = create_stmt->first_child; c != nullptr; c = c->next_sibling) {
        if (c->node_type == NodeType::CheckConstraint) {
            emit(c);  // table-level CHECK (...)
        } else if (c->node_type == NodeType::ColumnDefinition) {
            for (const ASTNode* k = c->first_child; k != nullptr; k = k->next_sibling) {
                if (k->node_type == NodeType::CheckConstraint) emit(k);  // column-level
            }
        }
    }
    return out;
}

// A CHECK predicate whose inferred type is one of these is accepted: Boolean is
// the intended type; a wildcard (NULL / Unknown / Any) means the type could not
// be pinned down (e.g. an unmodeled function) and is left to run time rather
// than reported as a false positive.
[[nodiscard]] bool check_type_ok(DataType t) {
    return t == DataType::Boolean || t == DataType::Null ||
           t == DataType::Unknown || t == DataType::Any;
}

// Layer-1 type checking for a CREATE TABLE's CHECK / DEFAULT expressions. Runs
// only after the structural and reference checks pass. Builds a synthetic
// single-relation scope from the statement's OWN declared columns (so it stays
// catalog-independent) and reuses the analyzer's type engine: a CHECK predicate
// must be Boolean; a DEFAULT value must be assignment-compatible with its
// column's declared type (same rule as an INSERT value into that column).
void typecheck_create_table(ASTNode* create, std::vector<std::string>& errors) {
    auto column_type = [](const ASTNode* coldef) {
        const ASTNode* type_node = first_child_of_type(coldef, NodeType::DataTypeNode);
        return type_node != nullptr ? data_type_from_name(type_node->primary_text)
                                    : DataType::Unknown;
    };

    // Synthetic relation: the table's own columns, resolved to their types.
    RelationBinding rel;
    rel.name = std::string(create->primary_text);
    std::uint32_t col_id = 0;
    for (const ASTNode* c = create->first_child; c != nullptr; c = c->next_sibling) {
        if (c->node_type != NodeType::ColumnDefinition) continue;
        ResolvedColumn rc;
        rc.name = std::string(c->primary_text);
        rc.type = column_type(c);
        rc.column_id = col_id++;
        rel.columns.push_back(std::move(rc));
    }
    Scope scope;
    scope.add_relation(std::move(rel));

    // The expressions reference only this table's columns; no catalog is needed.
    InMemoryCatalog empty;
    Analyzer analyzer(empty);

    auto check_predicate = [&](ASTNode* check_node, const std::string& where) {
        ASTNode* expr = check_node->first_child;
        if (expr == nullptr) return;
        const DataType t = analyzer.infer_scalar(expr, scope);
        if (!check_type_ok(t)) {
            errors.emplace_back(where + ": CHECK expression must be Boolean, not " +
                                std::string(data_type_name(t)));
        }
    };

    for (ASTNode* c = create->first_child; c != nullptr; c = c->next_sibling) {
        if (c->node_type == NodeType::CheckConstraint) {
            check_predicate(c, "table CHECK");
        } else if (c->node_type == NodeType::ColumnDefinition) {
            const std::string col(c->primary_text);
            const DataType col_type = column_type(c);
            for (ASTNode* k = c->first_child; k != nullptr; k = k->next_sibling) {
                if (k->node_type == NodeType::CheckConstraint) {
                    check_predicate(k, "CHECK on column '" + col + "'");
                } else if (k->node_type == NodeType::DefaultClause) {
                    ASTNode* expr = k->first_child;
                    if (expr == nullptr) continue;
                    const DataType t = analyzer.infer_scalar(expr, scope);
                    if (!Analyzer::assignment_compatible(col_type, t)) {
                        errors.emplace_back("column '" + col + "': DEFAULT value of type " +
                                            std::string(data_type_name(t)) +
                                            " is not compatible with column type " +
                                            std::string(data_type_name(col_type)));
                    }
                }
            }
        }
    }
}

// The single AlterTableAction child of an ALTER TABLE statement (the parser
// emits exactly one), or nullptr if absent.
const ASTNode* alter_action(const ASTNode* alter) {
    return first_child_of_type(alter, NodeType::AlterTableAction);
}

// Layer-1 well-formedness for ALTER TABLE. Only ADD COLUMN and DROP COLUMN are
// supported; each must name a target, an added column's type must resolve, and
// a column-level constraint other than NOT NULL / DEFAULT on an added column is
// rejected explicitly (rather than silently dropped) - those arrive via ALTER
// in a later change. An added column's DEFAULT is type-checked against the
// column's own declared type (a DEFAULT may not reference other columns).
void validate_alter_table(ASTNode* alter, std::vector<std::string>& errors) {
    if (alter->primary_text.empty()) {
        errors.emplace_back("ALTER TABLE: missing table name");
    }
    const ASTNode* action = alter_action(alter);
    if (action == nullptr) {
        errors.emplace_back("ALTER TABLE: missing action");
        return;
    }
    const std::string_view verb = action->primary_text;
    if (verb == "ADD") {
        const ASTNode* col = first_child_of_type(action, NodeType::ColumnDefinition);
        if (col == nullptr || col->primary_text.empty()) {
            errors.emplace_back("ALTER TABLE ADD COLUMN: missing column definition");
            return;
        }
        const ASTNode* type_node = first_child_of_type(col, NodeType::DataTypeNode);
        if (type_node == nullptr ||
            data_type_from_name(type_node->primary_text) == DataType::Unknown) {
            errors.emplace_back("ALTER TABLE ADD COLUMN '" + std::string(col->primary_text) +
                                "': unknown or missing type");
        }
        // Only NOT NULL / DEFAULT are supported inline; anything else (PRIMARY
        // KEY, UNIQUE, CHECK, REFERENCES) is refused rather than silently lost.
        for (const ASTNode* k = col->first_child; k != nullptr; k = k->next_sibling) {
            const bool ok = k->node_type == NodeType::DataTypeNode ||
                            k->node_type == NodeType::DefaultClause ||
                            (k->node_type == NodeType::ColumnConstraint &&
                             k->primary_text == "NOT_NULL");
            if (!ok) {
                errors.emplace_back("ALTER TABLE ADD COLUMN '" +
                                    std::string(col->primary_text) +
                                    "': only NOT NULL and DEFAULT are supported here");
                break;
            }
        }
        // Type-check the DEFAULT against the added column's own type. The value
        // may not reference table columns, so an empty scope is correct.
        if (errors.empty()) {
            const ASTNode* def = first_child_of_type(col, NodeType::DefaultClause);
            if (def != nullptr && def->first_child != nullptr) {
                const DataType col_type = data_type_from_name(type_node->primary_text);
                Scope scope;
                InMemoryCatalog empty;
                Analyzer analyzer(empty);
                const DataType t = analyzer.infer_scalar(def->first_child, scope);
                if (!Analyzer::assignment_compatible(col_type, t)) {
                    errors.emplace_back("ALTER TABLE ADD COLUMN '" +
                        std::string(col->primary_text) + "': DEFAULT value of type " +
                        std::string(data_type_name(t)) +
                        " is not compatible with column type " +
                        std::string(data_type_name(col_type)));
                }
            }
        }
    } else if (verb == "DROP") {
        const ASTNode* name = first_child_of_type(action, NodeType::Identifier);
        if (name == nullptr || name->primary_text.empty()) {
            errors.emplace_back("ALTER TABLE DROP COLUMN: missing column name");
        }
    } else if (verb == "ALTER") {
        // ALTER COLUMN <c> SET DEFAULT <expr> | DROP DEFAULT. Only the DEFAULT
        // alterations are supported; SET/DROP of other properties (TYPE, NOT
        // NULL, RENAME) are refused rather than silently ignored.
        const ASTNode* name = first_child_of_type(action, NodeType::Identifier);
        if (name == nullptr || name->primary_text.empty()) {
            errors.emplace_back("ALTER TABLE ALTER COLUMN: missing column name");
            return;
        }
        const ASTNode* def = first_child_of_type(action, NodeType::DefaultClause);
        const bool drop_default = (action->semantic_flags & kAlterActionDropDefault) != 0;
        const bool set_not_null = (action->semantic_flags & kAlterActionSetNotNull) != 0;
        const bool drop_not_null = (action->semantic_flags & kAlterActionDropNotNull) != 0;
        if (def == nullptr && !drop_default && !set_not_null && !drop_not_null) {
            errors.emplace_back("ALTER TABLE ALTER COLUMN '" +
                                std::string(name->primary_text) +
                                "': only SET/DROP DEFAULT and SET/DROP NOT NULL are supported");
        }
        // The DEFAULT value is type-checked against the column's existing type in
        // execute_ddl (that needs the catalog); here we only require its presence.
        if (def != nullptr && def->first_child == nullptr) {
            errors.emplace_back("ALTER TABLE ALTER COLUMN '" +
                                std::string(name->primary_text) +
                                "': SET DEFAULT is missing an expression");
        }
    } else {
        errors.emplace_back(
            "ALTER TABLE: only ADD COLUMN, DROP COLUMN and ALTER COLUMN "
            "SET/DROP DEFAULT are supported");
    }
}

}  // namespace

bool validate_ddl(ASTNode* stmt, std::vector<std::string>& errors) {
    ASTNode* d = find_ddl(stmt);
    if (d == nullptr) {
        errors.emplace_back("not a DDL statement");
        return false;
    }

    if (d->node_type == NodeType::DropStmt) {
        const bool is_table = (d->semantic_flags & kFlagDropTable) != 0;
        const bool is_index = (d->semantic_flags & kFlagDropIndex) != 0;
        if (!is_table && !is_index) {
            errors.emplace_back("only DROP TABLE / DROP INDEX are supported");
        }
        if (d->primary_text.empty()) {
            errors.emplace_back("DROP: missing object name");
        }
        return errors.empty();
    }

    if (d->node_type == NodeType::CreateIndexStmt) {
        if (d->primary_text.empty()) errors.emplace_back("CREATE INDEX: missing index name");
        if (d->schema_name.empty()) errors.emplace_back("CREATE INDEX: missing target table");
        int cols = 0;
        for (const ASTNode* c = d->first_child; c != nullptr; c = c->next_sibling) {
            if (c->node_type == NodeType::Identifier) ++cols;
        }
        if (cols == 0) errors.emplace_back("CREATE INDEX: no columns");
        return errors.empty();
    }

    if (d->node_type == NodeType::AlterTableStmt) {
        validate_alter_table(d, errors);
        return errors.empty();
    }

    // CREATE TABLE well-formedness.
    if (d->primary_text.empty()) {
        errors.emplace_back("CREATE TABLE: missing table name");
    }

    int columns = 0;
    int primary_keys = 0;
    std::vector<std::string> seen;
    for (const ASTNode* c = d->first_child; c != nullptr; c = c->next_sibling) {
        if (c->node_type == NodeType::PrimaryKeyConstraint) {
            ++primary_keys;  // table-level PRIMARY KEY (...)
            continue;
        }
        if (c->node_type != NodeType::ColumnDefinition) {
            continue;  // other table-level constraints - not validated in this slice
        }
        ++columns;
        const std::string cname(c->primary_text);
        for (const std::string& prev : seen) {
            if (prev == cname) {
                errors.emplace_back("duplicate column name: " + cname);
                break;
            }
        }
        seen.push_back(cname);

        const ASTNode* type_node = first_child_of_type(c, NodeType::DataTypeNode);
        if (type_node == nullptr) {
            errors.emplace_back("column '" + cname + "': missing type");
        } else if (data_type_from_name(type_node->primary_text) == DataType::Unknown) {
            errors.emplace_back("column '" + cname + "': unknown type '" +
                                std::string(type_node->primary_text) + "'");
        }
        if (column_flags(c).primary_key) {
            ++primary_keys;
        }
    }
    if (columns == 0) {
        errors.emplace_back("CREATE TABLE: no columns");
    }
    if (primary_keys > 1) {
        errors.emplace_back("CREATE TABLE: multiple PRIMARY KEY definitions");
    }

    // Second pass, now that every column name is known: validate DEFAULT and
    // CHECK expressions by reference. A DEFAULT must not reference any column;
    // a CHECK may reference only this table's own columns.
    auto is_pending = [&](const std::string& col) {
        for (const std::string& s : seen) {
            if (s == col) return true;
        }
        return false;
    };
    auto check_refs = [&](const ASTNode* expr_owner, const std::string& what) {
        std::vector<std::string> refs;
        collect_column_refs(expr_owner, refs);
        for (const std::string& r : refs) {
            if (!is_pending(r)) {
                errors.emplace_back(what + ": unknown column '" + r + "'");
            }
        }
    };
    for (const ASTNode* c = d->first_child; c != nullptr; c = c->next_sibling) {
        if (c->node_type == NodeType::ColumnDefinition) {
            const std::string col(c->primary_text);
            for (const ASTNode* k = c->first_child; k != nullptr; k = k->next_sibling) {
                if (k->node_type == NodeType::DefaultClause) {
                    // A DEFAULT may use functions/constants (now(), 0, 'x') but
                    // must not reference a column of this table. A bareword that
                    // does not name a column is assumed to be a function or a
                    // special constant and is left alone.
                    std::vector<std::string> refs;
                    collect_column_refs(k, refs);
                    for (const std::string& r : refs) {
                        if (is_pending(r)) {
                            errors.emplace_back("column '" + col +
                                "': DEFAULT must not reference column '" + r + "'");
                        }
                    }
                } else if (k->node_type == NodeType::CheckConstraint) {
                    check_refs(k, "CHECK on column '" + col + "'");
                }
            }
        } else if (c->node_type == NodeType::CheckConstraint) {
            check_refs(c, "table CHECK");
        }
    }

    // Third pass: type-check the CHECK / DEFAULT expressions. Only meaningful once
    // structure and references are sound, so skip it if anything failed above -
    // an unresolved column would otherwise be re-reported by type inference.
    if (errors.empty()) {
        typecheck_create_table(d, errors);
    }

    return errors.empty();
}

DdlResult execute_ddl(ASTNode* stmt, CatalogManager& mgr) {
    std::vector<std::string> errors;
    if (!validate_ddl(stmt, errors)) {
        std::string joined;
        for (std::size_t i = 0; i < errors.size(); ++i) {
            if (i != 0) joined += "; ";
            joined += errors[i];
        }
        return DdlResult{false, joined, mgr.schema_version()};
    }

    ASTNode* d = find_ddl(stmt);

    if (d->node_type == NodeType::DropStmt) {
        const bool if_exists = (d->semantic_flags & kFlagIfExistsOrNotExists) != 0;
        if ((d->semantic_flags & kFlagDropIndex) != 0) {
            return mgr.drop_index(std::string(d->primary_text), if_exists);
        }
        const bool cascade = (d->semantic_flags & kFlagCascade) != 0;
        return mgr.drop_table(std::string(d->primary_text), if_exists, cascade);
    }

    if (d->node_type == NodeType::CreateIndexStmt) {
        std::vector<std::string> cols;
        for (const ASTNode* c = d->first_child; c != nullptr; c = c->next_sibling) {
            if (c->node_type == NodeType::Identifier) cols.emplace_back(c->primary_text);
        }
        const bool unique = (d->semantic_flags & kFlagUnique) != 0;
        const bool if_not_exists = (d->semantic_flags & kFlagIfExistsOrNotExists) != 0;
        return mgr.create_index(std::string(d->primary_text), std::string(d->schema_name),
                                cols, unique, if_not_exists);
    }

    if (d->node_type == NodeType::AlterTableStmt) {
        const std::string table(d->primary_text);
        const ASTNode* action = alter_action(d);  // validated non-null above
        if (action->primary_text == "ADD") {
            const ASTNode* col = first_child_of_type(action, NodeType::ColumnDefinition);
            return mgr.add_column(table, build_column_info(col));
        }
        if (action->primary_text == "ALTER") {
            const ASTNode* name = first_child_of_type(action, NodeType::Identifier);
            const std::string col(name->primary_text);
            if ((action->semantic_flags & kAlterActionSetNotNull) != 0) {
                return mgr.set_column_nullable(table, col, /*nullable=*/false);
            }
            if ((action->semantic_flags & kAlterActionDropNotNull) != 0) {
                return mgr.set_column_nullable(table, col, /*nullable=*/true);
            }
            if ((action->semantic_flags & kAlterActionDropDefault) != 0) {
                return mgr.drop_column_default(table, col);
            }
            // SET DEFAULT: type-check the new value against the column's EXISTING
            // type (in the catalog, so this cannot be a pure layer-1 check). The
            // default may not reference columns, so an empty scope is correct.
            const ASTNode* def = first_child_of_type(action, NodeType::DefaultClause);
            const TableInfo* t = mgr.catalog().find_table(table);
            const ColumnInfo* ci = (t != nullptr) ? t->find_column(col) : nullptr;
            if (ci != nullptr && def->first_child != nullptr) {
                Scope scope;
                InMemoryCatalog empty;
                Analyzer analyzer(empty);
                const DataType vt = analyzer.infer_scalar(def->first_child, scope);
                if (!Analyzer::assignment_compatible(ci->type, vt)) {
                    return DdlResult{false,
                        "ALTER TABLE " + table + " ALTER COLUMN '" + col +
                        "': DEFAULT value of type " + std::string(data_type_name(vt)) +
                        " is not compatible with column type " +
                        std::string(data_type_name(ci->type)),
                        mgr.schema_version()};
                }
            }
            return mgr.set_column_default(table, col, std::string(def->primary_text));
        }
        // DROP COLUMN. Neither flag set => RESTRICT (the default).
        const ASTNode* name = first_child_of_type(action, NodeType::Identifier);
        const bool cascade = (action->semantic_flags & kAlterActionCascade) != 0;
        return mgr.drop_column(table, std::string(name->primary_text),
                               /*if_exists=*/false, cascade);
    }

    // CREATE TABLE.
    const std::string name(d->primary_text);
    const bool if_not_exists = (d->semantic_flags & kFlagIfExistsOrNotExists) != 0;
    if (if_not_exists && mgr.catalog().find_table(name) != nullptr) {
        return DdlResult{true, {}, mgr.schema_version()};  // no-op success
    }

    std::vector<ColumnInfo> columns;
    for (const ASTNode* c = d->first_child; c != nullptr; c = c->next_sibling) {
        if (c->node_type != NodeType::ColumnDefinition) continue;
        columns.push_back(build_column_info(c));
    }
    return mgr.create_table(name, std::move(columns), collect_foreign_keys(d),
                            collect_checks(d));
}

}  // namespace db25::semantic
