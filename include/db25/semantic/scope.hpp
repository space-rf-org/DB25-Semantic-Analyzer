// DB25 Semantic Analyzer - Lexical scope / symbol table
//
// A Scope models the set of relations (tables, aliased tables, derived tables,
// CTE references) that are visible while resolving a single query block. Scopes
// nest: a subquery gets a child scope whose parent is the enclosing query, so
// correlated references resolve outward.

#pragma once

#include "db25/ast/node_types.hpp"
#include "db25/semantic/identifier.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace db25::semantic {

using ast::DataType;

class Scope;

// A column made visible by a relation binding, already resolved to a type.
struct ResolvedColumn {
    std::string name;
    DataType type = DataType::Unknown;
    bool nullable = true;
    std::uint32_t table_id = 0;
    std::uint32_t column_id = 0;
    // The relation INSTANCE this column was projected from, as a qualifier
    // (the correlation alias, or the relation name when unaliased). Populated
    // by expand_star for `*` / `q.*` columns; empty for an explicitly-written
    // SELECT item (which carries its own qualifier in the AST). A self-join
    // (`emp a, emp b`) gives a.id and b.id the same (table_id, column_id), so a
    // positional GROUP BY key resolved to a star-expanded column needs this to
    // tell the two instances apart - otherwise b.id would satisfy a GROUP BY on
    // a.id. See GroupKey::instance.
    std::string qualifier;
    // A USING / NATURAL join coalesces the shared column into ONE output column.
    // The right-hand relation's copy is marked coalesced so a bare (unqualified)
    // reference resolves to the single left copy instead of being reported
    // ambiguous. A qualified reference (`b.id`) still resolves against it.
    bool coalesced = false;
    // The surviving (left) copy of a USING / NATURAL merged column. Its `nullable`
    // is the AUTHORITATIVE nullability of the merged COALESCE(left, right) value
    // (computed from the join type and both base columns), so a bare / `SELECT *`
    // reference must use it verbatim WITHOUT re-applying the null-supplying-side
    // adjustment - the merged value is present whenever either side is. A
    // QUALIFIED reference (`t.id`) still reads the per-side copy and keeps the
    // ordinary outer-join nullability, matching the binder's hidden per-side
    // copies. Set by the analyzer's resolve_using / resolve_natural.
    bool merged = false;
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

    // True when this relation sits on the null-supplying side of an outer join
    // (the right side of a LEFT JOIN, the left side of a RIGHT JOIN, or either
    // side of a FULL JOIN). Every column resolved through it is then nullable
    // regardless of its base NOT NULL constraint, because unmatched rows fill it
    // with NULLs. Set on the binding after the join is resolved.
    bool nullable_from_join = false;

    // Does the qualifier `q` (from `q.col`) address this relation? Identifiers
    // compare case-insensitively (see identifier.hpp).
    [[nodiscard]] bool matches_qualifier(std::string_view q) const {
        return alias.empty() ? iequals(name, q) : iequals(alias, q);
    }

    [[nodiscard]] const ResolvedColumn* find_column(std::string_view col) const {
        // Wide relations: consult a lazily-built name -> first-index map so a
        // lookup is O(1) instead of O(columns). Resolving a wide SELECT list is
        // otherwise O(refs * columns) - the analyzer's worst-case blowup. Narrow
        // relations keep the linear scan, which beats hashing for a few columns.
        // The map records each name's FIRST index, matching the linear scan's
        // first match exactly; indices are positional (valid across moves/copies,
        // since `columns` is never reordered after the binding is built).
        if (columns.size() > kIndexThreshold) {
            if (name_index_.empty()) {
                name_index_.reserve(columns.size());
                for (std::uint32_t i = 0; i < columns.size(); ++i) {
                    name_index_.emplace(columns[i].name, i);  // first occurrence wins
                }
            }
            const auto it = name_index_.find(col);
            return it == name_index_.end() ? nullptr : &columns[it->second];
        }
        for (const auto& c : columns) {
            if (iequals(c.name, col)) {
                return &c;
            }
        }
        return nullptr;
    }

    // How many columns this relation exposes under `col`, ignoring the hidden
    // right-hand copy of a USING / NATURAL coalesced column (which is not a
    // distinct output column). >1 means the relation itself exposes the name
    // twice - a derived table `SELECT id AS a, name AS a` - so a reference to it
    // is AMBIGUOUS, not silently the first match. Linear (used only on the
    // resolution path where find_column already matched).
    [[nodiscard]] std::size_t count_column(std::string_view col) const {
        std::size_t n = 0;
        for (const auto& c : columns) {
            if (!c.coalesced && iequals(c.name, col)) {
                ++n;
            }
        }
        return n;
    }

private:
    // Relations with more than this many columns build a name index; below it the
    // linear scan above is used (cheaper than hashing for a handful of columns).
    static constexpr std::size_t kIndexThreshold = 16;
    // Lazily built name -> first-occurrence index over `columns`. `mutable` so the
    // const find_column can memoize it; owned string keys keep it copy/move-safe.
    // Keyed case-insensitively (see identifier.hpp) so the O(1) wide-relation path
    // matches the linear scan above; the transparent hash/equal also let a
    // std::string_view probe the map without materializing a key. The lazy build
    // assumes single-threaded analysis (the analyzer never shares a Scope/binding
    // across threads); it would need synchronization otherwise.
    mutable std::unordered_map<std::string, std::uint32_t, IdentifierHash, IdentifierEqual>
        name_index_;
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
    // True when the reference resolved in an *enclosing* scope rather than the
    // scope it was looked up from: a correlated reference. Used to mark a
    // subquery correlated (no diagnostic; correlation is legal).
    bool from_outer = false;
    // The scope in which the reference actually resolved (the scope that OWNS
    // the matched relation). For a correlated reference this is an enclosing
    // scope; the analyzer uses it to mark every subquery between the reference's
    // block and this owning scope as correlated - not just the innermost.
    const Scope* owner = nullptr;
    ResolvedColumn column;
};

class Scope {
public:
    explicit Scope(Scope* parent = nullptr) : parent_(parent) {}

    [[nodiscard]] Scope* parent() const { return parent_; }

    void add_relation(RelationBinding binding) {
        relations_.push_back(std::move(binding));
    }

    // Mark the relations in the half-open index range [begin, end) of this
    // scope's own relation list as null-supplied by an outer join. Called by the
    // analyzer once it knows a join's type (see RelationBinding::nullable_from_join).
    // Mark the copy of column `name` in relations [begin, end) as coalesced (the
    // right-hand side of a USING / NATURAL join), so a bare reference to it
    // resolves to the surviving left copy rather than reporting ambiguity.
    void mark_column_coalesced(std::size_t begin, std::size_t end,
                               std::string_view name) {
        for (std::size_t i = begin; i < end && i < relations_.size(); ++i) {
            for (auto& c : relations_[i].columns) {
                if (iequals(c.name, name)) {
                    c.coalesced = true;
                }
            }
        }
    }

    void mark_join_nullable(std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end && i < relations_.size(); ++i) {
            relations_[i].nullable_from_join = true;
        }
    }

    // Record the authoritative nullability of a USING / NATURAL merged column on
    // its surviving (left) copy in relations [begin, end), and flag it merged so
    // a bare / `SELECT *` reference reads this value verbatim (see
    // ResolvedColumn::merged). Applied to every left copy of `name` in range;
    // the right copies are already marked coalesced.
    void set_merged_column_nullability(std::size_t begin, std::size_t end,
                                       std::string_view name, bool nullable) {
        for (std::size_t i = begin; i < end && i < relations_.size(); ++i) {
            for (auto& c : relations_[i].columns) {
                if (!c.coalesced && iequals(c.name, name)) {
                    c.nullable = nullable;
                    c.merged = true;
                }
            }
        }
    }

    // The relations made visible directly by this scope's own FROM clause, in
    // FROM/JOIN order. Used to expand `SELECT *` / `table.*` (which expand over
    // the current query block only, not outer scopes).
    [[nodiscard]] const std::vector<RelationBinding>& relations() const {
        return relations_;
    }

    void add_cte(NamedRelation cte) {
        ctes_.push_back(std::move(cte));
    }

    // Look up a CTE by name, walking outward through parent scopes.
    [[nodiscard]] const NamedRelation* find_cte(std::string_view name) const {
        for (const Scope* s = this; s != nullptr; s = s->parent_) {
            for (const auto& c : s->ctes_) {
                if (iequals(c.name, name)) {
                    return &c;
                }
            }
        }
        return nullptr;
    }

    // Mutable lookup: used to widen a recursive CTE's column nullability with the
    // reconciled recursive-term nullability, which is known only after the body
    // is analyzed (and the CTE was already registered so the self-reference could
    // resolve).
    [[nodiscard]] NamedRelation* find_cte_mutable(std::string_view name) {
        for (Scope* s = this; s != nullptr; s = s->parent_) {
            for (auto& c : s->ctes_) {
                if (iequals(c.name, name)) {
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
                    // The relation exposes this name more than once (a derived
                    // table with a duplicated output alias): the reference is
                    // ambiguous, not silently the first column.
                    if (rel.count_column(column) > 1) {
                        ColumnResolution amb;
                        amb.ambiguous = true;
                        amb.owner = s;
                        return amb;
                    }
                    ColumnResolution res;
                    res.found = true;
                    res.from_outer = (s != this);
                    res.owner = s;
                    res.column = *col;
                    // A relation on the null-supplying side of an outer join
                    // makes every column nullable, base constraint notwithstanding.
                    res.column.nullable = res.column.nullable || rel.nullable_from_join;
                    return res;
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
            const RelationBinding* hit_rel = nullptr;
            int matches = 0;
            for (const auto& rel : s->relations_) {
                if (const auto* col = rel.find_column(column)) {
                    // The right-hand copy of a USING / NATURAL coalesced column
                    // is not a distinct match for a bare reference - it merged
                    // into the left copy - so it must not trigger ambiguity.
                    if (col->coalesced) {
                        continue;
                    }
                    // A single relation exposing the name twice (a derived table
                    // with a duplicated output alias) is itself ambiguous.
                    if (rel.count_column(column) > 1) {
                        ColumnResolution amb;
                        amb.ambiguous = true;
                        amb.owner = s;
                        return amb;
                    }
                    ++matches;
                    hit = col;
                    hit_rel = &rel;
                }
            }
            if (matches == 1) {
                ColumnResolution res;
                res.found = true;
                res.from_outer = (s != this);
                res.owner = s;
                res.column = *hit;
                // A merged USING / NATURAL column carries its own authoritative
                // (COALESCE) nullability; do not re-apply the null-supplying-side
                // adjustment to a bare reference. Non-merged columns get the
                // ordinary outer-join nullability.
                if (!res.column.merged) {
                    res.column.nullable =
                        res.column.nullable || hit_rel->nullable_from_join;
                }
                return res;
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
