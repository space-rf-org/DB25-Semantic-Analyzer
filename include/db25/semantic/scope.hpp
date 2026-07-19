// DB25 Semantic Analyzer - Lexical scope / symbol table
//
// A Scope models the set of relations (tables, aliased tables, derived tables,
// CTE references) that are visible while resolving a single query block. Scopes
// nest: a subquery gets a child scope whose parent is the enclosing query, so
// correlated references resolve outward.

#pragma once

#include "db25/ast/node_types.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace db25::semantic {

using ast::DataType;

// A column made visible by a relation binding, already resolved to a type.
struct ResolvedColumn {
    std::string name;
    DataType type = DataType::Unknown;
    bool nullable = true;
    std::uint32_t table_id = 0;
    std::uint32_t column_id = 0;
};

// A relation visible in a scope: a base table, an aliased table, a derived
// table, or a CTE reference. `name` is the underlying relation name; `alias`
// is the correlation name if one was given (empty otherwise). Column references
// qualify against the alias when present, otherwise the name.
struct RelationBinding {
    std::string name;
    std::string alias;  // empty when no alias
    std::uint32_t table_id = 0;
    std::vector<ResolvedColumn> columns;

    // Does the qualifier `q` (from `q.col`) address this relation?
    [[nodiscard]] bool matches_qualifier(std::string_view q) const {
        return alias.empty() ? (name == q) : (alias == q);
    }

    [[nodiscard]] const ResolvedColumn* find_column(std::string_view col) const {
        for (const auto& c : columns) {
            if (c.name == col) {
                return &c;
            }
        }
        return nullptr;
    }
};

// A named relation that a FROM clause may reference by name (a CTE). Distinct
// from an active binding: it becomes a RelationBinding only once referenced.
struct NamedRelation {
    std::string name;
    std::vector<ResolvedColumn> columns;
};

// Result of resolving a column reference.
struct ColumnResolution {
    bool found = false;
    bool ambiguous = false;      // matched more than one relation (bare ref)
    bool bad_qualifier = false;  // qualifier named no visible relation
    ResolvedColumn column;
};

class Scope {
public:
    explicit Scope(Scope* parent = nullptr) : parent_(parent) {}

    [[nodiscard]] Scope* parent() const { return parent_; }

    void add_relation(RelationBinding binding) {
        relations_.push_back(std::move(binding));
    }

    void add_cte(NamedRelation cte) {
        ctes_.push_back(std::move(cte));
    }

    // Look up a CTE by name, walking outward through parent scopes.
    [[nodiscard]] const NamedRelation* find_cte(std::string_view name) const {
        for (const Scope* s = this; s != nullptr; s = s->parent_) {
            for (const auto& c : s->ctes_) {
                if (c.name == name) {
                    return &c;
                }
            }
        }
        return nullptr;
    }

    // Resolve `qualifier.column`. Only relations reachable from this scope
    // outward are considered.
    [[nodiscard]] ColumnResolution resolve_qualified(std::string_view qualifier,
                                                     std::string_view column) const {
        bool qualifier_seen = false;
        for (const Scope* s = this; s != nullptr; s = s->parent_) {
            for (const auto& rel : s->relations_) {
                if (!rel.matches_qualifier(qualifier)) {
                    continue;
                }
                qualifier_seen = true;
                if (const auto* col = rel.find_column(column)) {
                    return {true, false, false, *col};
                }
            }
        }
        ColumnResolution res;
        res.found = false;
        res.bad_qualifier = !qualifier_seen;
        return res;
    }

    // Resolve a bare `column` against all visible relations. Ambiguous if more
    // than one relation in the nearest scope that has it provides a match.
    [[nodiscard]] ColumnResolution resolve_bare(std::string_view column) const {
        for (const Scope* s = this; s != nullptr; s = s->parent_) {
            const ResolvedColumn* hit = nullptr;
            int matches = 0;
            for (const auto& rel : s->relations_) {
                if (const auto* col = rel.find_column(column)) {
                    ++matches;
                    hit = col;
                }
            }
            if (matches == 1) {
                return {true, false, false, *hit};
            }
            if (matches > 1) {
                ColumnResolution res;
                res.ambiguous = true;
                return res;
            }
        }
        return {};
    }

private:
    Scope* parent_;
    std::vector<RelationBinding> relations_;
    std::vector<NamedRelation> ctes_;
};

}  // namespace db25::semantic
