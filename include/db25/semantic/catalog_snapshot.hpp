// DB25 Semantic Analyzer - persistent catalog snapshot store
//
// Interim persistence for the metadata catalog while the storage engine is
// still being built. The catalog is small and read-mostly, so it is persisted
// as a single self-describing text snapshot that is crash-safe to overwrite:
// save() writes a sibling temp file, fsyncs it, then atomically rename()s it
// into place, so a crash mid-write always leaves the previous snapshot intact.
//
// This is deliberately dependency-free and human-inspectable (`cat` the file).
// It sits behind the same abstraction the analyzer already borrows, and is the
// seed of the eventual embedded metadata profile - not throwaway scaffolding.
//
// On-disk format (line-oriented, deterministic; names are emitted last on each
// line so they may contain spaces):
//
//   DB25CATALOG 1
//   version <schema_version>
//   next_table_id <n>
//   table <table_id> <name...>
//   col <column_id> <TYPE> <null|notnull> <default|nodefault> <name...>
//   col ...
//   table ...

#pragma once

#include "db25/semantic/catalog.hpp"

#include <optional>
#include <string>

namespace db25::semantic {

// Serialize a catalog to the snapshot text format (deterministic: tables are
// ordered by table_id, columns keep their definition order).
[[nodiscard]] std::string serialize_catalog(const InMemoryCatalog& catalog);

// Parse a snapshot back into a catalog. On success returns the catalog; on a
// malformed snapshot returns nullopt and sets `error` (no partial state).
[[nodiscard]] std::optional<InMemoryCatalog> deserialize_catalog(
    const std::string& text, std::string& error);

// Atomically persist `catalog` to `path` (temp file + fsync + rename). Returns
// true on success; on failure returns false and sets `error`, leaving any
// existing snapshot at `path` untouched.
[[nodiscard]] bool save_catalog_snapshot(const InMemoryCatalog& catalog,
                                         const std::string& path,
                                         std::string& error);

// Load a catalog snapshot from `path`. A missing file is NOT an error - it
// yields an empty catalog (a fresh install) - so callers can boot unconditionally.
// A present-but-malformed file returns nullopt with `error` set.
[[nodiscard]] std::optional<InMemoryCatalog> load_catalog_snapshot(
    const std::string& path, std::string& error);

}  // namespace db25::semantic
