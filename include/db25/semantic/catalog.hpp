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
};

// A table (or view) definition: an ordered list of typed columns.
struct TableInfo {
    std::string name;
    std::uint32_t table_id = 0;
    std::vector<ColumnInfo> columns;

    [[nodiscard]] const ColumnInfo* find_column(std::string_view col) const noexcept {
        for (const auto& c : columns) {
            if (c.name == col) {
                return &c;
            }
        }
        return nullptr;
    }
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

private:
    std::unordered_map<std::string, TableInfo> tables_;
    std::uint32_t next_table_id_ = 1;
    std::uint32_t schema_version_ = 0;
};

}  // namespace db25::semantic
