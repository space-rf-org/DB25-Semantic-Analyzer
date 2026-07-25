// DB25 Semantic Analyzer - catalog manager
//
// The single writer for schema truth. All DDL goes through here; the analyzer
// and everyone else borrow the current catalog read-only. Each committed DDL:
//   1. produces the next catalog state (a copy + mutation),
//   2. is persisted DURABLY and ATOMICALLY before it is visible in memory
//      (copy -> mutate -> save snapshot -> only then swap), so memory and disk
//      never disagree and a crash cannot leave a half-applied schema,
//   3. bumps the monotonic schema_version and notifies watchers.
//
// The watcher hook models the async-lease version notification of the eventual
// separate metadata instance; here it fires synchronously in-process (the
// interim stub). Callers that want to resolve names against a specific schema
// version borrow catalog() and read schema_version().

#pragma once

#include "db25/semantic/catalog.hpp"
#include "db25/semantic/catalog_snapshot.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace db25::semantic {

// Outcome of a DDL operation. `ok == false` leaves the catalog and its on-disk
// snapshot completely unchanged (the mutation is all-or-nothing).
struct DdlResult {
    bool ok = false;
    std::string error;               // populated when !ok
    std::uint32_t schema_version = 0;  // the version after a successful commit
};

class CatalogManager {
public:
    // Open the catalog persisted at `snapshot_path`, loading the current state
    // (an empty catalog if no snapshot exists yet). `load_error` is set (and the
    // manager left empty) only if a snapshot is present but malformed.
    explicit CatalogManager(std::string snapshot_path, std::string& load_error)
        : path_(std::move(snapshot_path)) {
        if (auto loaded = load_catalog_snapshot(path_, load_error)) {
            catalog_ = std::move(*loaded);
        }
    }

    // The current authoritative catalog, borrowed read-only for name binding.
    [[nodiscard]] const InMemoryCatalog& catalog() const noexcept { return catalog_; }
    [[nodiscard]] std::uint32_t schema_version() const noexcept {
        return catalog_.schema_version();
    }

    // Register a watcher fired (in-process, synchronously for now) with the new
    // schema version after every COMMITTED mutation. A failed DDL fires nothing.
    void add_watcher(std::function<void(std::uint32_t)> cb) {
        watchers_.push_back(std::move(cb));
    }

    // A foreign key expressed by NAME, as extracted from a CREATE TABLE
    // statement. The manager resolves the names to column_ids at commit against
    // the new catalog (which includes the table being created, so a
    // self-referencing FK resolves).
    struct ForeignKeySpec {
        std::string name;                      // "" if unnamed
        std::vector<std::string> columns;      // local column names
        std::string ref_table;
        std::vector<std::string> ref_columns;  // referenced column names
    };

    // A CHECK constraint expressed by its verbatim source text plus the local
    // columns it references (by NAME, resolved to column_ids at commit). The
    // expression is stored, not evaluated, at this layer; the referenced columns
    // are recorded so the constraint's dependencies are known.
    struct CheckSpec {
        std::string name;                   // "" if unnamed
        std::string expr;                   // verbatim expression text
        std::vector<std::string> columns;   // local column names referenced
    };

    // CREATE TABLE. Fails if a table of that name already exists. Any foreign
    // keys are validated and resolved to column_ids here (the serialized commit
    // point), where cross-object references are sound: the referenced table and
    // columns must exist and the local/referenced column counts must match.
    [[nodiscard]] DdlResult create_table(const std::string& name,
                                         std::vector<ColumnInfo> columns,
                                         std::vector<ForeignKeySpec> foreign_keys = {},
                                         std::vector<CheckSpec> checks = {}) {
        if (catalog_.find_table(name) != nullptr) {
            return fail("table already exists: " + name);
        }
        InMemoryCatalog next = catalog_;  // copy; catalog is small
        TableInfo& self = next.add_table(name, std::move(columns));

        for (const ForeignKeySpec& fk : foreign_keys) {
            const std::string where = "FK on '" + name + "'";
            if (fk.columns.empty() || fk.ref_columns.empty()) {
                return fail(where + ": foreign key needs explicit local and referenced columns");
            }
            if (fk.columns.size() != fk.ref_columns.size()) {
                return fail(where + ": " + std::to_string(fk.columns.size()) +
                            " local columns but " + std::to_string(fk.ref_columns.size()) +
                            " referenced columns");
            }
            Constraint c;
            c.kind = Constraint::Kind::ForeignKey;
            c.name = fk.name;
            c.ref_table = fk.ref_table;
            for (const std::string& col : fk.columns) {
                const ColumnInfo* ci = self.find_column(col);
                if (ci == nullptr) return fail(where + ": no local column '" + col + "'");
                c.columns.push_back(ci->column_id);
            }
            const TableInfo* rt = next.find_table(fk.ref_table);
            if (rt == nullptr) {
                return fail(where + ": referenced table '" + fk.ref_table + "' does not exist");
            }
            for (const std::string& col : fk.ref_columns) {
                const ColumnInfo* ci = rt->find_column(col);
                if (ci == nullptr) {
                    return fail(where + ": referenced table '" + fk.ref_table +
                                "' has no column '" + col + "'");
                }
                c.ref_columns.push_back(ci->column_id);
            }
            self.constraints.push_back(std::move(c));
        }

        for (CheckSpec& chk : checks) {
            Constraint c;
            c.kind = Constraint::Kind::Check;
            c.name = std::move(chk.name);
            c.expr = std::move(chk.expr);
            for (const std::string& col : chk.columns) {
                const ColumnInfo* ci = self.find_column(col);
                if (ci == nullptr) {
                    return fail("CHECK on '" + name + "': no local column '" + col + "'");
                }
                c.columns.push_back(ci->column_id);
            }
            self.constraints.push_back(std::move(c));
        }

        next.set_schema_version(catalog_.schema_version() + 1);
        return commit(std::move(next));
    }

    // DROP TABLE. With `if_exists`, dropping a missing table is a no-op success
    // (and does NOT bump the version).
    //
    // Referential dependencies are enforced at this serialized commit point.
    // A foreign key in ANOTHER table that references this one is a dependency:
    //   - RESTRICT (the default, `cascade == false`): the drop is refused.
    //   - CASCADE (`cascade == true`): the drop proceeds and those referencing
    //     foreign keys are removed from the dependent tables.
    // A self-referencing foreign key is not a dependency (its only referrer is
    // the table going away). The table's own secondary indexes are always
    // dropped with it, regardless of RESTRICT/CASCADE.
    [[nodiscard]] DdlResult drop_table(const std::string& name, bool if_exists = false,
                                       bool cascade = false) {
        if (catalog_.find_table(name) == nullptr) {
            if (if_exists) {
                return DdlResult{true, {}, catalog_.schema_version()};
            }
            return fail("no such table: " + name);
        }
        // Find dependents: foreign keys in OTHER tables referencing `name`.
        for (const TableInfo* t : catalog_.tables()) {
            if (t->name == name) continue;  // self-reference is not a dependency
            for (const Constraint& c : t->constraints) {
                if (c.kind == Constraint::Kind::ForeignKey && c.ref_table == name && !cascade) {
                    return fail("cannot drop table '" + name + "': table '" + t->name +
                                "' has a foreign key referencing it (use CASCADE)");
                }
            }
        }
        InMemoryCatalog next = catalog_;  // copy
        next.remove_table(name);
        next.drop_indexes_on(name);                  // the table's own indexes go with it
        if (cascade) {
            next.drop_foreign_keys_referencing(name);  // detach dependent FKs
        }
        next.set_schema_version(catalog_.schema_version() + 1);
        return commit(std::move(next));
    }

    // CREATE INDEX. Catalog-integrity checks, all sound only at this serialized
    // commit point: the index name must be unused, the target table must exist,
    // and every named column must exist in it (resolved here to column_ids).
    // With `if_not_exists`, an existing index of that name is a no-op success.
    [[nodiscard]] DdlResult create_index(const std::string& name,
                                         const std::string& table,
                                         const std::vector<std::string>& columns,
                                         bool unique, bool if_not_exists = false) {
        if (catalog_.find_index(name) != nullptr) {
            if (if_not_exists) {
                return DdlResult{true, {}, catalog_.schema_version()};
            }
            return fail("index already exists: " + name);
        }
        const TableInfo* t = catalog_.find_table(table);
        if (t == nullptr) {
            return fail("CREATE INDEX " + name + ": no such table: " + table);
        }
        if (columns.empty()) {
            return fail("CREATE INDEX " + name + ": no columns");
        }
        std::vector<std::uint32_t> column_ids;
        column_ids.reserve(columns.size());
        for (const std::string& col : columns) {
            const ColumnInfo* c = t->find_column(col);
            if (c == nullptr) {
                return fail("CREATE INDEX " + name + ": table '" + table +
                            "' has no column '" + col + "'");
            }
            column_ids.push_back(c->column_id);
        }
        InMemoryCatalog next = catalog_;  // copy
        next.add_index(name, table, std::move(column_ids), unique);
        next.set_schema_version(catalog_.schema_version() + 1);
        return commit(std::move(next));
    }

    // DROP INDEX. Mirrors DROP TABLE's if_exists semantics.
    [[nodiscard]] DdlResult drop_index(const std::string& name, bool if_exists = false) {
        if (catalog_.find_index(name) == nullptr) {
            if (if_exists) {
                return DdlResult{true, {}, catalog_.schema_version()};
            }
            return fail("no such index: " + name);
        }
        InMemoryCatalog next = catalog_;  // copy
        next.remove_index(name);
        next.set_schema_version(catalog_.schema_version() + 1);
        return commit(std::move(next));
    }

    // ALTER TABLE ADD COLUMN. The table must exist and the column name must be
    // new; the column is appended with a fresh column_id (ids are never reused,
    // so existing constraints/indexes that key on other columns stay valid).
    [[nodiscard]] DdlResult add_column(const std::string& table, ColumnInfo column) {
        const TableInfo* t = catalog_.find_table(table);
        if (t == nullptr) {
            return fail("ALTER TABLE " + table + ": no such table");
        }
        if (t->find_column(column.name) != nullptr) {
            return fail("ALTER TABLE " + table + ": column '" + column.name +
                        "' already exists");
        }
        InMemoryCatalog next = catalog_;  // copy
        TableInfo copy = *next.find_table(table);
        std::uint32_t max_id = 0;
        for (const ColumnInfo& c : copy.columns) {
            max_id = std::max(max_id, c.column_id);
        }
        column.column_id = max_id + 1;
        copy.columns.push_back(std::move(column));
        next.restore_table(std::move(copy));
        next.set_schema_version(catalog_.schema_version() + 1);
        return commit(std::move(next));
    }

    // ALTER TABLE DROP COLUMN. Follows PostgreSQL: the column's OWN indexes and
    // table constraints that involve it are dropped automatically; an EXTERNAL
    // foreign key in another table that references the column requires CASCADE
    // (RESTRICT, the default, refuses). A table's last remaining column cannot be
    // dropped. With `if_exists`, dropping a missing column is a no-op success.
    [[nodiscard]] DdlResult drop_column(const std::string& table, const std::string& col,
                                        bool if_exists = false, bool cascade = false) {
        const TableInfo* t = catalog_.find_table(table);
        if (t == nullptr) {
            return fail("ALTER TABLE " + table + ": no such table");
        }
        const ColumnInfo* target = t->find_column(col);
        if (target == nullptr) {
            if (if_exists) {
                return DdlResult{true, {}, catalog_.schema_version()};
            }
            return fail("ALTER TABLE " + table + ": no such column '" + col + "'");
        }
        if (t->columns.size() == 1) {
            return fail("ALTER TABLE " + table + ": cannot drop the only column '" + col +
                        "'");
        }
        const std::uint32_t cid = target->column_id;

        // External dependents: a foreign key in another table referencing this
        // column. RESTRICT refuses; CASCADE drops those foreign keys below.
        if (!cascade) {
            for (const TableInfo* other : catalog_.tables()) {
                if (other->name == table) continue;
                for (const Constraint& c : other->constraints) {
                    if (c.kind != Constraint::Kind::ForeignKey || c.ref_table != table) {
                        continue;
                    }
                    if (std::find(c.ref_columns.begin(), c.ref_columns.end(), cid) !=
                        c.ref_columns.end()) {
                        return fail("cannot drop column '" + col + "' of '" + table +
                                    "': foreign key in '" + other->name +
                                    "' references it (use CASCADE)");
                    }
                }
            }
        }

        InMemoryCatalog next = catalog_;  // copy
        if (cascade) {
            next.drop_foreign_keys_referencing_column(table, cid);  // external FKs
        }
        next.drop_indexes_on_column(table, cid);  // the column's own indexes

        // Remove the column and any of this table's own constraints that involve
        // it (a composite PK / UNIQUE / FK-source / CHECK loses meaning when one
        // of its columns is gone, so it is dropped whole).
        TableInfo copy = *next.find_table(table);
        auto& cs = copy.constraints;
        cs.erase(std::remove_if(cs.begin(), cs.end(),
                     [cid](const Constraint& c) {
                         return std::find(c.columns.begin(), c.columns.end(), cid) !=
                                c.columns.end();
                     }),
                 cs.end());
        auto& cols = copy.columns;
        cols.erase(std::remove_if(cols.begin(), cols.end(),
                       [&col](const ColumnInfo& c) { return c.name == col; }),
                   cols.end());
        next.restore_table(std::move(copy));

        next.set_schema_version(catalog_.schema_version() + 1);
        return commit(std::move(next));
    }

    // ALTER TABLE ALTER COLUMN SET DEFAULT. Sets (or replaces) the column's
    // default expression text. The table and column must exist. Type-checking
    // the expression against the column type is done by the caller (the analyzer,
    // which owns the type engine) before this commit.
    [[nodiscard]] DdlResult set_column_default(const std::string& table,
                                               const std::string& col,
                                               const std::string& expr_text) {
        const TableInfo* t = catalog_.find_table(table);
        if (t == nullptr) {
            return fail("ALTER TABLE " + table + ": no such table");
        }
        if (t->find_column(col) == nullptr) {
            return fail("ALTER TABLE " + table + ": no such column '" + col + "'");
        }
        InMemoryCatalog next = catalog_;  // copy
        TableInfo copy = *next.find_table(table);
        for (ColumnInfo& c : copy.columns) {
            if (c.name == col) {
                c.has_default = true;
                c.default_expr = expr_text;
                break;
            }
        }
        next.restore_table(std::move(copy));
        next.set_schema_version(catalog_.schema_version() + 1);
        return commit(std::move(next));
    }

    // ALTER TABLE ALTER COLUMN DROP DEFAULT. Clears the column's default. The
    // table and column must exist; dropping a default when there is none is a
    // no-op success (PostgreSQL does not error), and does not bump the version.
    [[nodiscard]] DdlResult drop_column_default(const std::string& table,
                                                const std::string& col) {
        const TableInfo* t = catalog_.find_table(table);
        if (t == nullptr) {
            return fail("ALTER TABLE " + table + ": no such table");
        }
        const ColumnInfo* c = t->find_column(col);
        if (c == nullptr) {
            return fail("ALTER TABLE " + table + ": no such column '" + col + "'");
        }
        if (!c->has_default) {
            return DdlResult{true, {}, catalog_.schema_version()};  // no-op
        }
        InMemoryCatalog next = catalog_;  // copy
        TableInfo copy = *next.find_table(table);
        for (ColumnInfo& cc : copy.columns) {
            if (cc.name == col) {
                cc.has_default = false;
                cc.default_expr.clear();
                break;
            }
        }
        next.restore_table(std::move(copy));
        next.set_schema_version(catalog_.schema_version() + 1);
        return commit(std::move(next));
    }

private:
    DdlResult fail(std::string msg) const {
        return DdlResult{false, std::move(msg), catalog_.schema_version()};
    }

    // Persist `next` durably, then swap it in and notify. Nothing observable
    // changes until the snapshot is safely on disk.
    DdlResult commit(InMemoryCatalog next) {
        std::string err;
        if (!save_catalog_snapshot(next, path_, err)) {
            return fail("persist failed: " + err);  // catalog_ untouched
        }
        catalog_ = std::move(next);
        const std::uint32_t v = catalog_.schema_version();
        for (auto& w : watchers_) {
            w(v);
        }
        return DdlResult{true, {}, v};
    }

    std::string path_;
    InMemoryCatalog catalog_;
    std::vector<std::function<void(std::uint32_t)>> watchers_;
};

}  // namespace db25::semantic
