// DB25 Semantic Analyzer - DDL utility dispatch
//
// DDL is NOT relational algebra, so it does not become a logical plan node
// (Option C). Instead a DDL statement is validated and dispatched straight to
// the CatalogManager. Integrity is checked in two layers:
//
//   1. Statement well-formedness (here / validate_ddl): local, catalog-
//      independent checks - duplicate column names, resolvable types, at most
//      one primary key. Pure, so it can run before any catalog lock.
//   2. Catalog integrity (CatalogManager, at its serialized commit): existence,
//      name clashes, dependencies. Only sound against the live schema version,
//      so it MUST happen at the single-writer commit point (TOCTOU).
//
// execute_ddl() runs (1) then (2) and reports the combined outcome.

#pragma once

#include "db25/ast/ast_node.hpp"
#include "db25/semantic/catalog_manager.hpp"

#include <string>
#include <vector>

namespace db25::semantic {

// Statement well-formedness (layer 1). Appends human-readable messages to
// `errors` and returns true iff the statement is well-formed. Catalog state is
// NOT consulted. Handles CREATE TABLE and DROP TABLE; other DDL is reported as
// unsupported (for now) rather than silently accepted.
[[nodiscard]] bool validate_ddl(const ast::ASTNode* stmt,
                                std::vector<std::string>& errors);

// Validate (layer 1) then apply (layer 2) a DDL statement. A malformed
// statement yields a failed DdlResult with the validation errors joined into
// `error`, and the catalog is left untouched. A well-formed statement is
// applied via the manager, whose commit-time checks decide the final outcome.
// `stmt` may be the DDL node itself or any ancestor that contains exactly one
// DDL statement (the node is located within the tree).
[[nodiscard]] DdlResult execute_ddl(const ast::ASTNode* stmt, CatalogManager& mgr);

}  // namespace db25::semantic
