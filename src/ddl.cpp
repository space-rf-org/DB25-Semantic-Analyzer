// DB25 Semantic Analyzer - DDL utility dispatch (implementation).

#include "db25/semantic/ddl.hpp"
#include "db25/semantic/type_names.hpp"

#include "db25/ast/node_types.hpp"

#include <string>
#include <vector>

namespace db25::semantic {

using ast::ASTNode;
using ast::NodeType;

namespace {

// DROP object-type / IF-EXISTS / CREATE IF-NOT-EXISTS flags, as set by the
// parser on the statement node's semantic_flags.
constexpr std::uint16_t kFlagIfExistsOrNotExists = 0x01;  // IF [NOT] EXISTS
constexpr std::uint16_t kFlagUnique = 0x02;               // CREATE UNIQUE INDEX
constexpr std::uint16_t kFlagCascade = 0x04;              // DROP ... CASCADE
constexpr std::uint16_t kFlagDropTable = 0x10;            // DROP TABLE
constexpr std::uint16_t kFlagDropIndex = 0x20;            // DROP INDEX

// Locate the single DDL statement in a (possibly wrapping) parse tree.
const ASTNode* find_ddl(const ASTNode* n) {
    if (n == nullptr) return nullptr;
    if (n->node_type == NodeType::CreateTableStmt ||
        n->node_type == NodeType::CreateIndexStmt ||
        n->node_type == NodeType::DropStmt) {
        return n;
    }
    for (const ASTNode* c = n->first_child; c != nullptr; c = c->next_sibling) {
        if (const ASTNode* h = find_ddl(c)) return h;
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
};

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
        }
    }
    return f;
}

}  // namespace

bool validate_ddl(const ASTNode* stmt, std::vector<std::string>& errors) {
    const ASTNode* d = find_ddl(stmt);
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

    return errors.empty();
}

DdlResult execute_ddl(const ASTNode* stmt, CatalogManager& mgr) {
    std::vector<std::string> errors;
    if (!validate_ddl(stmt, errors)) {
        std::string joined;
        for (std::size_t i = 0; i < errors.size(); ++i) {
            if (i != 0) joined += "; ";
            joined += errors[i];
        }
        return DdlResult{false, joined, mgr.schema_version()};
    }

    const ASTNode* d = find_ddl(stmt);

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

    // CREATE TABLE.
    const std::string name(d->primary_text);
    const bool if_not_exists = (d->semantic_flags & kFlagIfExistsOrNotExists) != 0;
    if (if_not_exists && mgr.catalog().find_table(name) != nullptr) {
        return DdlResult{true, {}, mgr.schema_version()};  // no-op success
    }

    std::vector<ColumnInfo> columns;
    for (const ASTNode* c = d->first_child; c != nullptr; c = c->next_sibling) {
        if (c->node_type != NodeType::ColumnDefinition) continue;
        ColumnInfo ci;
        ci.name = std::string(c->primary_text);
        const ASTNode* type_node = first_child_of_type(c, NodeType::DataTypeNode);
        ci.type = (type_node != nullptr) ? data_type_from_name(type_node->primary_text)
                                         : DataType::Unknown;
        const ColFlags f = column_flags(c);
        ci.nullable = !(f.not_null || f.primary_key);  // PRIMARY KEY implies NOT NULL
        ci.has_default = f.has_default;
        columns.push_back(std::move(ci));
    }
    return mgr.create_table(name, std::move(columns), collect_foreign_keys(d));
}

}  // namespace db25::semantic
