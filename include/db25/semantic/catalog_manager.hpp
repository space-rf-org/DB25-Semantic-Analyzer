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

    // CREATE TABLE. Fails if a table of that name already exists.
    [[nodiscard]] DdlResult create_table(const std::string& name,
                                         std::vector<ColumnInfo> columns) {
        if (catalog_.find_table(name) != nullptr) {
            return fail("table already exists: " + name);
        }
        InMemoryCatalog next = catalog_;  // copy; catalog is small
        next.add_table(name, std::move(columns));
        next.set_schema_version(catalog_.schema_version() + 1);
        return commit(std::move(next));
    }

    // DROP TABLE. With `if_exists`, dropping a missing table is a no-op success
    // (and does NOT bump the version). Without it, a missing table fails.
    [[nodiscard]] DdlResult drop_table(const std::string& name, bool if_exists = false) {
        if (catalog_.find_table(name) == nullptr) {
            if (if_exists) {
                return DdlResult{true, {}, catalog_.schema_version()};
            }
            return fail("no such table: " + name);
        }
        InMemoryCatalog next = catalog_;  // copy
        next.remove_table(name);
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
