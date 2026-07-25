// DB25 Semantic Analyzer - Catalog interface
//
// A minimal, injectable schema catalog. The analyzer only depends on the
// abstract `Catalog` interface; `InMemoryCatalog` is the default in-memory
// implementation used by tests and simple embeddings.

#pragma once

#include "db25/ast/node_types.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace db25::semantic {

using ast::DataType;

// A single column in a table definition.
struct ColumnInfo {
    std::string name;
    DataType type = DataType::Unknown;
    bool nullable = true;
    // True when the column has a DEFAULT (or is otherwise auto-populated, e.g. a
    // serial/identity column). An INSERT that omits a NOT NULL column only
    // violates the constraint when the column also has no default. Optional and
    // defaults to false so existing catalog builders keep compiling; positioned
    // before column_id so brace-init call sites that pass {name, type, nullable}
    // are unaffected.
    bool has_default = false;
    std::uint32_t column_id = 0;  // stable id within the owning table
    // The DEFAULT expression's source text (e.g. "0", "now()"), stored verbatim
    // for fidelity; empty when has_default is false. Not evaluated at this layer.
    std::string default_expr;
};

// A table-level constraint beyond per-column nullability/default. Column
// references are by column_id (stable within a table); a ForeignKey also names
// the referenced table and its column_ids; a Check carries its expression text.
struct Constraint {
    enum class Kind : std::uint8_t { PrimaryKey, Unique, ForeignKey, Check };
    Kind kind = Kind::Check;
    std::string name;                        // "" when unnamed
    std::vector<std::uint32_t> columns;      // local column_ids (PK / Unique / FK source)
    std::string ref_table;                   // ForeignKey: referenced table name
    std::vector<std::uint32_t> ref_columns;  // ForeignKey: referenced column_ids
    std::string expr;                        // Check: expression text
};

// A table (or view) definition: an ordered list of typed columns plus any
// table-level constraints.
struct TableInfo {
    std::string name;
    std::uint32_t table_id = 0;
    std::vector<ColumnInfo> columns;
    std::vector<Constraint> constraints;

    [[nodiscard]] const ColumnInfo* find_column(std::string_view col) const noexcept {
        for (const auto& c : columns) {
            if (c.name == col) {
                return &c;
            }
        }
        return nullptr;
    }
};

// A secondary index over one table's columns (by column_id).
struct IndexInfo {
    std::string name;
    std::uint32_t index_id = 0;
    std::string table;                   // the indexed table's name
    std::vector<std::uint32_t> columns;  // indexed column_ids, in order
    bool unique = false;
};

// Abstract catalog interface - inject any implementation into the Analyzer.
class Catalog {
public:
    virtual ~Catalog() = default;
    [[nodiscard]] virtual const TableInfo* find_table(std::string_view name) const = 0;
};

// Simple in-memory catalog. Build it up with add_table()/table builder helpers.
class InMemoryCatalog final : public Catalog {
public:
    // Register a table with the given typed columns. Assigns table/column ids.
    // Returns a reference to the stored table.
    TableInfo& add_table(std::string name, std::vector<ColumnInfo> columns) {
        const std::uint32_t table_id = next_table_id_++;
        TableInfo info;
        info.name = std::move(name);
        info.table_id = table_id;
        std::uint32_t col_id = 1;
        for (auto& c : columns) {
            c.column_id = col_id++;
            info.columns.push_back(std::move(c));
        }
        const std::string key = info.name;
        auto [it, _] = tables_.emplace(key, std::move(info));
        return it->second;
    }

    [[nodiscard]] const TableInfo* find_table(std::string_view name) const override {
        const auto it = tables_.find(std::string{name});
        return it == tables_.end() ? nullptr : &it->second;
    }

    // Every table, ordered by table_id for a deterministic enumeration (the
    // snapshot store relies on this so a saved catalog is byte-stable).
    [[nodiscard]] std::vector<const TableInfo*> tables() const {
        std::vector<const TableInfo*> out;
        out.reserve(tables_.size());
        for (const auto& [_, t] : tables_) {
            out.push_back(&t);
        }
        std::sort(out.begin(), out.end(),
                  [](const TableInfo* a, const TableInfo* b) {
                      return a->table_id < b->table_id;
                  });
        return out;
    }

    // Monotonic schema version, bumped by the catalog manager on each committed
    // DDL. Persisted in the snapshot so a reloaded catalog resumes at the same
    // version (and the id allocator does not collide with existing tables).
    [[nodiscard]] std::uint32_t schema_version() const noexcept { return schema_version_; }
    void set_schema_version(std::uint32_t v) noexcept { schema_version_ = v; }
    [[nodiscard]] std::uint32_t next_table_id() const noexcept { return next_table_id_; }
    void set_next_table_id(std::uint32_t n) noexcept { next_table_id_ = n; }

    // Insert a fully-formed table, preserving its table_id and column_ids. Used
    // by the snapshot loader to reconstruct the exact catalog state; add_table()
    // (which allocates fresh ids) is the path for new DDL. Replaces any existing
    // table of the same name.
    void restore_table(TableInfo info) {
        const std::string key = info.name;
        tables_.insert_or_assign(key, std::move(info));
    }

    // Remove a table by name. Returns true if a table was removed, false if no
    // such table existed. The id allocator is not rewound (dropped ids are not
    // reused), so a later CREATE never collides with a dropped table's id.
    bool remove_table(std::string_view name) {
        return tables_.erase(std::string{name}) != 0;
    }

    // ----- Indexes -----

    // Register an index, assigning its index_id. Returns a reference to it.
    IndexInfo& add_index(std::string name, std::string table,
                         std::vector<std::uint32_t> columns, bool unique) {
        IndexInfo idx;
        idx.name = std::move(name);
        idx.index_id = next_index_id_++;
        idx.table = std::move(table);
        idx.columns = std::move(columns);
        idx.unique = unique;
        const std::string key = idx.name;
        auto [it, _] = indexes_.emplace(key, std::move(idx));
        return it->second;
    }

    [[nodiscard]] const IndexInfo* find_index(std::string_view name) const {
        const auto it = indexes_.find(std::string{name});
        return it == indexes_.end() ? nullptr : &it->second;
    }

    bool remove_index(std::string_view name) {
        return indexes_.erase(std::string{name}) != 0;
    }

    // Cascade-cleanup helpers used by DROP TABLE. Remove every secondary index
    // built on `table`, and every foreign key (in any table) that references
    // `ref_table`. Both return the number removed.
    std::size_t drop_indexes_on(std::string_view table) {
        std::size_t removed = 0;
        for (auto it = indexes_.begin(); it != indexes_.end();) {
            if (it->second.table == table) { it = indexes_.erase(it); ++removed; }
            else { ++it; }
        }
        return removed;
    }
    std::size_t drop_foreign_keys_referencing(std::string_view ref_table) {
        std::size_t removed = 0;
        for (auto& [_, t] : tables_) {
            auto& cs = t.constraints;
            const std::size_t before = cs.size();
            cs.erase(std::remove_if(cs.begin(), cs.end(),
                         [&](const Constraint& c) {
                             return c.kind == Constraint::Kind::ForeignKey &&
                                    c.ref_table == ref_table;
                         }),
                     cs.end());
            removed += before - cs.size();
        }
        return removed;
    }

    // Every index, ordered by index_id for a deterministic enumeration.
    [[nodiscard]] std::vector<const IndexInfo*> indexes() const {
        std::vector<const IndexInfo*> out;
        out.reserve(indexes_.size());
        for (const auto& [_, i] : indexes_) out.push_back(&i);
        std::sort(out.begin(), out.end(),
                  [](const IndexInfo* a, const IndexInfo* b) {
                      return a->index_id < b->index_id;
                  });
        return out;
    }

    // Loader helpers (preserve ids, mirror the table equivalents).
    void restore_index(IndexInfo idx) {
        const std::string key = idx.name;
        indexes_.insert_or_assign(key, std::move(idx));
    }
    [[nodiscard]] std::uint32_t next_index_id() const noexcept { return next_index_id_; }
    void set_next_index_id(std::uint32_t n) noexcept { next_index_id_ = n; }

private:
    std::unordered_map<std::string, TableInfo> tables_;
    std::unordered_map<std::string, IndexInfo> indexes_;
    std::uint32_t next_table_id_ = 1;
    std::uint32_t next_index_id_ = 1;
    std::uint32_t schema_version_ = 0;
};

}  // namespace db25::semantic
