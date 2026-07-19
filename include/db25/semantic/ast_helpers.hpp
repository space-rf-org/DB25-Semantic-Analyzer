// DB25 Semantic Analyzer - AST access helpers
//
// This header centralizes every place where the analyzer depends on a *layout
// convention* of the DB25 parser AST (as opposed to its stable public shape).
// The most important of these is where table / derived-table aliases are
// stored. Keeping them here means that when the parser evolves, we adjust one
// file instead of hunting through the analyzer.

#pragma once

#include "db25/ast/ast_node.hpp"
#include "db25/ast/node_types.hpp"

#include <string_view>

namespace db25::semantic {

using ast::ASTNode;
using ast::NodeType;

// ---------------------------------------------------------------------------
// Alias coupling (KNOWN COUPLING - see docs/DESIGN.md)
//
// The parser currently stores the alias of a table reference or a derived
// table (subquery in FROM) in ASTNode::schema_name, and is intended to mark it
// with NodeFlags::HasAlias in `semantic_flags`. In practice the build of the
// parser we consume does not always set that flag, so we treat any non-empty
// schema_name on a table/subquery reference as the alias.
//
// If the parser later introduces a dedicated alias field, ONLY this function
// changes.
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::string_view alias_of(const ASTNode* node) noexcept {
    if (node == nullptr) {
        return {};
    }
    return node->schema_name;
}

[[nodiscard]] inline bool has_alias(const ASTNode* node) noexcept {
    return !alias_of(node).empty();
}

// A column reference's primary_text is the (possibly dotted) qualified name,
// e.g. "u.id" or bare "id". Split it into an optional qualifier and the column
// name. Only the last dot is treated as the separator.
struct QualifiedRef {
    std::string_view qualifier;  // empty if the reference is bare
    std::string_view column;
};

[[nodiscard]] inline QualifiedRef split_column_ref(std::string_view text) noexcept {
    const auto pos = text.rfind('.');
    if (pos == std::string_view::npos) {
        return {std::string_view{}, text};
    }
    return {text.substr(0, pos), text.substr(pos + 1)};
}

// Iterate the children of a node via the sibling chain. Usage:
//   for (ASTNode* c = first_child(n); c; c = c->next_sibling) { ... }
[[nodiscard]] inline ASTNode* first_child(const ASTNode* node) noexcept {
    return node ? node->first_child : nullptr;
}

// Find the first direct child of a given node type (or nullptr).
[[nodiscard]] inline ASTNode* find_child(const ASTNode* node, NodeType type) noexcept {
    for (ASTNode* c = first_child(node); c != nullptr; c = c->next_sibling) {
        if (c->node_type == type) {
            return c;
        }
    }
    return nullptr;
}

}  // namespace db25::semantic
